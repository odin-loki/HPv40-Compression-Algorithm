# hpd — a context-mixing compressor for everyday files

`hpd` is HP40-Lite (from the `hp` Hutter-Prize lab,
[odin-loki/CompressionAlgorithm](https://github.com/odin-loki/CompressionAlgorithm))
rebuilt for day-to-day use. It keeps HP40's integer-exact context-mixing
model and fixes what the HP40 study (`paper/hp40/HP40_paper.md`) measured as
wrong for general use: speed, memory, start-up, and weak spots on binary data.

## Results

| | GP set: 42 files, 27 MB | full Silesia: 212 MB | speed, 1 thread | peak RAM |
|---|---:|---:|---:|---:|
| **hpd** | **1.823 bpc** (−21.3% vs xz -9e) | **1.474 bpc** (−19.4%), 4 threads | **78.6 KiB/s** | 0.6 GiB per thread |
| HP40, `--mem 22` | 1.851 bpc (−20.1%) | — | 14.2 KiB/s | 10.6 GiB |
| HP40-Lite, `--mem 22` | 1.874 bpc (−19.1%) | — | 30.3 KiB/s | 6.2 GiB |
| xz -9e | 2.316 bpc | 1.829 bpc | 1,924 KiB/s | 76 MiB |

Smallest archive of every tested compressor (xz, brotli, zstd, bzip2, gzip,
HP40) on 40 of 41 GP files and all 12 Silesia files. Speeds are single-job,
same machine (enwik8 1 MiB + ooffice + book1). Full write-up, method and
every number: [`paper/hp40/HPD_part2.md`](../../paper/hp40/HPD_part2.md).

**Trade-off:** compression and decompression run at the same speed, about
80–90 KiB/s per thread. hpd is for archiving and backups, where size matters
more than seconds, not for data you decompress constantly.

## Usage

```
hpd c [options] <in> [<out>]   compress     (out defaults to <in>.hpd)
hpd d [options] <in> [<out>]   decompress   (out defaults to <in> without .hpd)
hpd t <archive>...             test: decode and verify every CRC, write nothing
hpd l <archive>...             list blocks and settings
```

A file name of `-` means stdin / stdout, so `tar cf - dir | hpd c - - > dir.tar.hpd` works.

| option | default | meaning |
|---|---|---|
| `-t N` | all cores | worker threads (one block per thread; fewer if memory is short) |
| `-9` | off | 12 discovery slots instead of 4: about 0.4% smaller, about 25% slower |
| `-c` | off | write to stdout |
| `-f` | off | overwrite an existing output file chosen by default name |
| `-q` / `-v` | | quiet / verbose (one line per block) |
| `-b MiB`, `-B KiB` | auto | block size. Auto: one block up to 16 MiB, then enough blocks for every thread (16–256 MiB each). Smaller blocks mean more parallelism but cost ratio: 8 MiB of enwik8 as 2 blocks is 2.7% larger, as 4 blocks 6% larger. `-B` minimum is 4 KiB. |
| `-s 0\|1` | 1 | pass 64 KiB chunks of already-compressed data (JPEG, zip, video, encrypted) through unmodelled |
| `-x 0\|1` | auto | force the x86 CALL transform off/on |
| `-m bits` | auto | base table size (2^bits slots per model, 16–24). Auto sizes tables to about 4× the block. |
| `-M bits` | 22 | largest automatic table size (16–24) |
| `-g bits` | 2 | how far one model's table may grow above the base (0–8) |
| `-D slots` | 4 | discovery slots (1–12) |
| `-h`, `-V` | | help, version |

Files are streamed in batches of one block per thread, so memory is bounded
by threads × (block + model tables), not by file size.

## What changed from HP40-Lite, and what each change bought

Every change was measured on its own (tuning set, single thread). Changes
marked *exact* produce byte-identical archives.

| change | ratio | speed | memory |
|---|---:|---:|---:|
| strip the 134 compile switches to the Lite set (`unifdef`) | *exact* | — | — |
| **bucketed context tables**: one 32-byte bucket (16-bit checksum + 15 bit-history slots) per context per nibble, 2-way in a cache line, prefetched one step ahead | −0.72% | **1.6–1.9×** | — |
| table-driven `log2_q16` (was a 16-step squaring loop, ~15 calls per bit) | *exact* | 1.57× | — |
| Pitman–Yor estimate without 64-bit divides (13×13 table + reciprocal, checked on all 692,224 inputs) | *exact* | 1.18× | — |
| AVX2 int64 mixer dot, int32 mixer update | *exact* | 1.02× | — |
| demand-zero tables (`mmap`/`VirtualAlloc`), one shared match history | — | start-up ≈ 0 | RSS tracks use |
| per-model growth cap (`-g 2`; HP40 let word tables reach 2³¹ slots) | +0.03% | 1.14× | 1.9 GiB → 0.54 GiB on 1 MiB |
| auto table size (≈4× block) | — | — | 48 MiB … 0.6 GiB |
| **record model**: online period detection + 3 contexts on the cell above | **−2.24%** | −4% | — |
| mixer weight set chosen by position inside the record | −0.26% | −3% | — |
| word-bigram model back on (Lite had dropped it) | −0.14% | −3% | — |
| x86 E8 CALL transform (auto-detected per block) | ooffice −14.0% | — | — |
| stored blocks; give up on data that expands | never > input + 30 B | — | — |
| parallel blocks, streaming I/O | small loss on > 16 MiB | × threads | bounded |
| pass-through of incompressible 64 KiB chunks (entropy ≥ 7.9 bits/byte and no repeats) | ≈ 0 on such data | copy speed on such data | — |
| CRC-32 per block and for the whole stream; streaming container (archive format 3) | +5 B per block, +2 B per archive | — | — |

## Building

```
make              # build/hpd, needs a C++20 compiler (tested: GCC 13 on Linux, MinGW-w64 GCC for Windows)
make test         # round trips: empty, 1 byte, random, repetitive, multi-thread
bash test/fuzz.sh build/hpd 2000   # mutation fuzzer for the decoder (see the script header for an ASan build)
```

Windows: MinGW-w64 (`x86_64-w64-mingw32-g++ -O2 -mavx2 -std=c++20 -static -I include src/hpd.cpp -o hpd.exe`)
is tested (under Wine 9): it writes byte-identical archives to the Linux build
and decodes Linux archives. MSVC should work (`cl /O2 /std:c++20 /arch:AVX2 /EHsc /I include src\hpd.cpp`)
but has not been tested. The codec has no floating point, so an archive made
on one machine decodes on any other.

## Layout

| path | what |
|---|---|
| `src/hpd.cpp` | CLI, container, blocks, threads, x86 transform |
| `include/hp/*.hpp` | the model (from HP40-Lite) with the changes above |
| `include/hp/record.hpp` | record model (new) |
| `include/hp/zero_array.hpp` | demand-zero arrays, shared history (new) |
| `src/hp_main.cpp` | the original single-stream `hp` CLI on this tree (`make build/hp_lite`) |
| `test/roundtrip.sh` | round-trip tests |
| `test/fuzz.sh` | mutation fuzzer for `hpd d` (archive formats 2 and 3) |

## Archive format (version 3)

```
"HPD1" u8 version=3  u8 flags=0
per block: u8 method (0 stored, 1 CM)  u8 filters (bit0 = x86 E8)  u8 table_bits
           u8 slot_extra  u8 discovery_slots  u8 chunk_log (0 = no chunk map)
           u32 raw_bytes  u32 payload_bytes  u32 crc32(raw block)
           [chunk map: ceil(ceil(raw_bytes / 2^chunk_log) / 8) bytes, bit i set = chunk i passed through]
           payload (CM-coded bytes, then the passed-through chunks in order)
end:       u8 0xFF  u64 total_raw_bytes  u32 crc32(all data)
```

All integers little-endian; CRC-32 is the IEEE/zlib polynomial. There are no
sizes up front, so the encoder can write to a pipe. Every setting the decoder
needs is in the block header, and the decoder checks every field against fixed
limits (block ≤ 1 GiB, table bits 16–24, slot extra ≤ 8, discovery slots 1–12)
before allocating anything.

hpd still reads version 2 archives (no CRCs, no chunk maps):

```
"HPD1" u8 version=2  u64 raw_size  u32 block_count
per block: u8 method  u8 filters  u8 table_bits  u8 slot_extra  u8 discovery_slots
           u32 raw_bytes  u32 payload_bytes  payload
```
