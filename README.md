# HPv40 Compression Algorithm

HP40 and HP40-Lite studied as general-purpose compressors, and **hpd**, the
everyday context-mixing compressor built from what that study found.

| | GP set: 42 files, 27 MB | speed, 1 thread | peak RAM |
|---|---:|---:|---:|
| **hpd** | **1.823 bpc** (−21.3% vs xz -9e) | 78.6 KiB/s | 0.6 GiB per thread |
| HP40, `--mem 22` | 1.851 bpc (−20.1%) | 14.2 KiB/s | 10.6 GiB |
| HP40-Lite, `--mem 22` | 1.874 bpc (−19.1%) | 30.3 KiB/s | 6.2 GiB |
| xz -9e | 2.316 bpc | 1,924 KiB/s | 76 MiB |

## Layout

| path | what |
|---|---|
| [`compress/hpd/`](compress/hpd/) | hpd: source, build, tests, archive format ([README](compress/hpd/README.md)) |
| [`paper/hp40/`](paper/hp40/) | the paper: Part I [`HP40_paper.md`](paper/hp40/HP40_paper.md) (HP40 / HP40-Lite profiling and expert mining), Part II [`HPD_part2.md`](paper/hp40/HPD_part2.md) (hpd), plus all data, figures and scripts ([README](paper/hp40/README.md)) |

## Quick start

```bash
cd compress/hpd
make && make test
build/hpd c file            # -> file.hpd
build/hpd d file.hpd        # -> file
build/hpd t file.hpd        # verify CRCs without writing
```

HP40 itself comes from the `hp` lab,
[odin-loki/CompressionAlgorithm](https://github.com/odin-loki/CompressionAlgorithm).
This repository was split out of `odin-loki/Cypha` with its history.

## Licence

GNU Affero General Public License v3.0, see [`LICENSE`](LICENSE).
