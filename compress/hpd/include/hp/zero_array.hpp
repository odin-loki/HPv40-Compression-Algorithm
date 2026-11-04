#pragma once
//
// hp/zero_array.hpp — demand-zero arrays for the big model tables.
//
// std::vector<T>(n, 0) writes every byte before the first symbol is coded:
// HP40 spent 17–36 s zeroing 10.6 GiB per file. Anonymous mmap (POSIX) or
// VirtualAlloc (Windows) hands out pages the OS zeroes on first touch, so
// start-up is free and resident memory tracks what the input actually uses.
// Tables are 64-byte aligned (page aligned in practice) for cache-line buckets.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <memory>
#include <utility>

#include "hp/hd.hpp"

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <sys/mman.h>
#endif

namespace hp {

template <class T>
class ZeroArray {
 public:
    ZeroArray() = default;
    explicit ZeroArray(std::size_t n) { allocate(n); }
    ~ZeroArray() { release(); }
    ZeroArray(const ZeroArray&) = delete;
    ZeroArray& operator=(const ZeroArray&) = delete;
    ZeroArray(ZeroArray&& o) noexcept : p_(o.p_), n_(o.n_), mapped_(o.mapped_) {
        o.p_ = nullptr; o.n_ = 0; o.mapped_ = false;
    }
    ZeroArray& operator=(ZeroArray&& o) noexcept {
        if (this != &o) {
            release();
            p_ = o.p_; n_ = o.n_; mapped_ = o.mapped_;
            o.p_ = nullptr; o.n_ = 0; o.mapped_ = false;
        }
        return *this;
    }

    T& operator[](std::size_t i) { return p_[i]; }
    const T& operator[](std::size_t i) const { return p_[i]; }
    T* data() { return p_; }
    const T* data() const { return p_; }
    std::size_t size() const { return n_; }

 private:
    void allocate(std::size_t n) {
        n_ = n;
        const std::size_t bytes = n * sizeof(T);
#if defined(__CUDA_ARCH__)
        p_ = static_cast<T*>(hp_dev_alloc(bytes));   // arena memory is pre-zeroed
        mapped_ = false;
#else
        if (bytes >= (std::size_t(1) << 16)) {
#if defined(_WIN32)
            p_ = static_cast<T*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (p_) { mapped_ = true; return; }
#else
            void* m = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (m != MAP_FAILED) { p_ = static_cast<T*>(m); mapped_ = true; return; }
#endif
        }
        // Small (or mmap failed): ordinary zeroed, 64-byte aligned block.
        void* m = nullptr;
        const std::size_t rounded = (bytes + 63) & ~std::size_t(63);
#if defined(_WIN32)
        m = _aligned_malloc(rounded ? rounded : 64, 64);
#else
        if (posix_memalign(&m, 64, rounded ? rounded : 64) != 0) m = nullptr;
#endif
        if (!m) throw std::bad_alloc();
        std::memset(m, 0, rounded ? rounded : 64);
        p_ = static_cast<T*>(m);
        mapped_ = false;
#endif
    }
    void release() {
        if (!p_) return;
#if defined(__CUDA_ARCH__)
        p_ = nullptr; n_ = 0;   // freed with the arena
#else
        const std::size_t bytes = n_ * sizeof(T);
        if (mapped_) {
#if defined(_WIN32)
            VirtualFree(p_, 0, MEM_RELEASE);
#else
            munmap(p_, bytes);
#endif
        } else {
#if defined(_WIN32)
            _aligned_free(p_);
#else
            std::free(p_);
#endif
        }
        p_ = nullptr; n_ = 0;
#endif
    }

    T* p_ = nullptr;
    std::size_t n_ = 0;
    bool mapped_ = false;
};

// One input history shared by every match model (HP40 kept 14 private copies).
struct HistoryBuf {
    explicit HistoryBuf(int bits) : a(std::size_t(1) << bits), mask((1u << bits) - 1) {}
    ZeroArray<std::uint8_t> a;
    std::uint32_t mask;
};

}  // namespace hp
