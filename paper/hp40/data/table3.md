**Table 3. Single-job timings** (one job alone on the VM; enwik8.1mb + Silesia ooffice + Calgary book1 = 2,865,923 bytes; `data/clean_timing*.jsonl`)

| compressor | bpc | compress wall | of which kernel | KiB/s (wall) | KiB/s (user CPU only) | decompress wall | peak RSS | round trip |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| HP40, mem 22 | 2.296 | 310.3 s | 67.4 s | 9.0 | 11.6 | 294.9 s | 10.6 GiB | 3/3 byte-exact |
| HP40-Lite, mem 22 | 2.326 | 148.5 s | 11.4 s | 18.8 | 20.6 | 149.7 s | 6.2 GiB | 3/3 byte-exact |
| HP40, mem 18 | 2.351 | 202.6 s | 1.4 s | 13.8 | 14.0 | 201.2 s | 690 MiB | 3/3 byte-exact |
| HP40-Lite, mem 18 | 2.391 | 114.3 s | 0.8 s | 24.5 | 24.9 | 113.0 s | 404 MiB | 3/3 byte-exact |
| xz -9e | 2.918 | 2.5 s | 1.0 s | 1120.5 | 1905.8 | 0.1 s | 76 MiB | 3/3 byte-exact |
| brotli -q11 -w24 | 2.897 | 8.6 s | 0.4 s | 327.2 | 349.4 | 0.1 s | 31 MiB | 3/3 byte-exact |
| zstd --ultra -22 --long | 3.029 | 8.2 s | 6.4 s | 341.2 | 1613.3 | 0.0 s | 653 MiB | 3/3 byte-exact |
| bzip2 -9 | 2.919 | 0.3 s | 0.0 s | 10182.5 | 10954.9 | 0.2 s | 17 MiB | 3/3 byte-exact |
