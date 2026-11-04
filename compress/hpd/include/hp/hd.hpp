#pragma once
//
// hp/hd.hpp — containers the model uses, in a form that also runs on the GPU.
//
// CPU build (hpd): vec = std::vector, shared = std::shared_ptr. Nothing changes.
// CUDA build (hpd-gpu, clang -x cuda with `#pragma clang force_cuda_host_device`):
// vec and shared are small host/device classes. On the device every allocation
// comes from the calling thread's arena: one pre-zeroed slice of GPU memory per
// shard, bump-allocated and never freed until the shard is done. On the host
// side of the CUDA build they use operator new / delete.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace hp {

#if !defined(__CUDACC__)

template <class T> using vec = std::vector<T>;
template <class T> using shared = std::shared_ptr<T>;
template <class T, class... A> inline shared<T> make_shared_hd(A&&... a) {
    return std::make_shared<T>(std::forward<A>(a)...);
}

#else  // ------------------------------------------------------------- CUDA build

// One arena per GPU thread (= per shard). The host fills hp_arenas before launch.
struct DevArena {
    unsigned char* base;
    std::size_t used;
    std::size_t cap;
    int overflow;
};
__device__ DevArena* hp_arenas;
__device__ int hp_arena_shift;   // 0: one arena per thread; 5: one per warp (warp-per-shard)

__device__ inline DevArena& hp_my_arena() {
    return hp_arenas[(blockIdx.x * blockDim.x + threadIdx.x) >> hp_arena_shift];
}

// 64-byte aligned bump allocation from this thread's arena. The arena is zeroed
// by the host before the launch, so memory comes back zeroed (ZeroArray relies
// on it). Running out sets `overflow` and traps; the host sizes arenas from a
// measured probe so this does not happen.
__device__ inline void* hp_dev_alloc(std::size_t n) {
    DevArena& a = hp_my_arena();
    const std::size_t off = (a.used + 63) & ~std::size_t(63);
    if (off + n > a.cap) {
        a.overflow = 1;
        __trap();
    }
    a.used = off + n;
    return a.base + off;
}

inline void* hd_alloc(std::size_t n) {
#if defined(__CUDA_ARCH__)
    return hp_dev_alloc(n);
#else
    return ::operator new(n ? n : 1, std::align_val_t{64});
#endif
}
inline void hd_free(void* p) {
#if defined(__CUDA_ARCH__)
    (void)p;  // arena memory is released with the whole arena
#else
    if (p) ::operator delete(p, std::align_val_t{64});
#endif
}

template <class T>
class vec {
 public:
    vec() = default;
    explicit vec(std::size_t n) { resize(n); }
    vec(std::size_t n, const T& v) { assign(n, v); }
    vec(std::initializer_list<T> il) {
        reserve(il.size());
        for (const T& x : il) push_back(x);
    }
    vec(const vec& o) {
        reserve(o.n_);
        for (std::size_t i = 0; i < o.n_; ++i) new (p_ + i) T(o.p_[i]);
        n_ = o.n_;
    }
    vec(vec&& o) noexcept : p_(o.p_), n_(o.n_), cap_(o.cap_) { o.p_ = nullptr; o.n_ = o.cap_ = 0; }
    vec& operator=(const vec& o) {
        if (this != &o) { vec t(o); swap(t); }
        return *this;
    }
    vec& operator=(vec&& o) noexcept {
        if (this != &o) { vec t(std::move(o)); swap(t); }
        return *this;
    }
    ~vec() {
        clear();
        hd_free(p_);
    }

    void swap(vec& o) noexcept {
        T* p = p_; p_ = o.p_; o.p_ = p;
        std::size_t n = n_; n_ = o.n_; o.n_ = n;
        std::size_t c = cap_; cap_ = o.cap_; o.cap_ = c;
    }
    std::size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    T* data() { return p_; }
    const T* data() const { return p_; }
    T& operator[](std::size_t i) { return p_[i]; }
    const T& operator[](std::size_t i) const { return p_[i]; }
    T* begin() { return p_; }
    T* end() { return p_ + n_; }
    const T* begin() const { return p_; }
    const T* end() const { return p_ + n_; }
    T& back() { return p_[n_ - 1]; }

    void reserve(std::size_t c) {
        if (c <= cap_) return;
        T* q = static_cast<T*>(hd_alloc(c * sizeof(T)));
        for (std::size_t i = 0; i < n_; ++i) {
            new (q + i) T(std::move(p_[i]));
            p_[i].~T();
        }
        hd_free(p_);
        p_ = q;
        cap_ = c;
    }
    void resize(std::size_t n) {
        reserve(n);
        while (n_ < n) new (p_ + n_++) T();
        while (n_ > n) p_[--n_].~T();
    }
    void assign(std::size_t n, const T& v) {
        clear();
        reserve(n);
        while (n_ < n) new (p_ + n_++) T(v);
    }
    void clear() {
        while (n_) p_[--n_].~T();
    }
    void push_back(const T& v) {
        if (n_ == cap_) reserve(cap_ ? 2 * cap_ : 8);
        new (p_ + n_++) T(v);
    }
    template <class... A>
    T& emplace_back(A&&... a) {
        if (n_ == cap_) reserve(cap_ ? 2 * cap_ : 8);
        new (p_ + n_) T(std::forward<A>(a)...);
        return p_[n_++];
    }

 private:
    T* p_ = nullptr;
    std::size_t n_ = 0, cap_ = 0;
};

// Reference-counted pointer. Not thread-safe: one Predictor (one thread) owns
// every copy, which is how the model uses it.
template <class T>
class shared {
    struct Box {
        T v;
        int rc;
        template <class... A>
        explicit Box(A&&... a) : v(std::forward<A>(a)...), rc(1) {}
    };

 public:
    shared() = default;
    shared(std::nullptr_t) {}
    shared(const shared& o) : b_(o.b_) { if (b_) ++b_->rc; }
    shared(shared&& o) noexcept : b_(o.b_) { o.b_ = nullptr; }
    shared& operator=(shared o) noexcept {
        Box* t = b_; b_ = o.b_; o.b_ = t;
        return *this;
    }
    ~shared() {
        if (b_ && --b_->rc == 0) {
            b_->~Box();
            hd_free(b_);
        }
    }
    explicit operator bool() const { return b_ != nullptr; }
    T* get() const { return b_ ? &b_->v : nullptr; }
    T* operator->() const { return &b_->v; }
    T& operator*() const { return b_->v; }

    template <class... A>
    static shared make(A&&... a) {
        shared s;
        s.b_ = new (hd_alloc(sizeof(Box))) Box(std::forward<A>(a)...);
        return s;
    }

 private:
    Box* b_ = nullptr;
};
template <class T, class... A> inline shared<T> make_shared_hd(A&&... a) {
    return shared<T>::make(std::forward<A>(a)...);
}

#endif

}  // namespace hp
