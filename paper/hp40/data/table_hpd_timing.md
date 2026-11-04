| compressor | bpc (3 files) | compress | decompress | KiB/s | compress time vs hpd | peak RSS | round trip |
|---|---:|---:|---:|---:|---:|---:|---|
| **hpd** | 2.148 | 35.6 s | 36.4 s | 78.6 | ×1.0 | 599 MiB | 3/3 exact |
| HP40, mem 22 | 2.296 | 197.7 s | 171.3 s | 14.2 | ×5.6 | 10.6 GiB | 3/3 exact |
| HP40-Lite, mem 22 | 2.326 | 92.4 s | 95.3 s | 30.3 | ×2.6 | 6.2 GiB | 3/3 exact |
| HP40, mem 18 | 2.351 | 114.8 s | 113.1 s | 24.4 | ×3.2 | 690 MiB | 3/3 exact |
| HP40-Lite, mem 18 | 2.391 | 69.4 s | 70.2 s | 40.3 | ×2.0 | 404 MiB | 3/3 exact |
| xz -9e | 2.918 | 1.5 s | 0.1 s | 1923.5 | 1/24 | 76 MiB | 3/3 exact |
| brotli -q11 -w24 | 2.897 | 6.6 s | 0.0 s | 421.9 | 1/5 | 31 MiB | 3/3 exact |
| zstd --ultra -22 --long | 3.029 | 2.3 s | 0.0 s | 1208.1 | 1/15 | 653 MiB | 3/3 exact |
| bzip2 -9 | 2.919 | 0.3 s | 0.2 s | 10489.5 | 1/133 | 17 MiB | 3/3 exact |
| gzip -9 | 3.531 | 0.3 s | 0.0 s | 10321.7 | 1/131 | 17 MiB | 3/3 exact |
