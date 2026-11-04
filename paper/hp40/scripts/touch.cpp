// touch.cpp GiB : time first-touch zeroing of N GiB (vector value-init, like hp's tables) and a second memset pass.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
int main(int argc, char** argv) {
    double g = atof(argv[1]);
    size_t n = (size_t)(g * (1ull << 30));
    auto t0 = std::chrono::steady_clock::now();
    std::vector<unsigned char> v(n, 0);
    auto t1 = std::chrono::steady_clock::now();
    std::memset(v.data(), 1, n);
    auto t2 = std::chrono::steady_clock::now();
    double a = std::chrono::duration<double>(t1 - t0).count(), b = std::chrono::duration<double>(t2 - t1).count();
    std::printf("%.1f GiB first-touch %.2f s (%.2f GiB/s)  re-write %.2f s (%.2f GiB/s)  [%d]\n", g, a, g / a, b, g / b, v[n / 2]);
}
