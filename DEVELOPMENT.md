# fff — Fast Find in Fields

A high-performance grep-like text search tool written in C++17, targeting Linux machines with AVX-512 support.

**Usage:** `fff <query> <file1> [file2 ...]`

Outputs matching lines with query highlighted in red (when stdout is a terminal).

---

## Target Hardware

- **CPU:** Dual Intel Xeon Gold 6140 (36 cores / 72 threads, 2.3 GHz base / 3.7 GHz turbo)
- **RAM:** 512 GB DDR4
- **Storage:** NVMe
- **ISA required:** AVX-512 (Skylake-SP and later)

---

## Architecture (current state of `fff.cpp`)

### Search Strategy
- **Short patterns (<8 bytes):** AVX-512 first-byte prefilter with 64-byte vector compare, then `memcmp` verification
- **Long patterns (≥8 bytes):** Boyer-Moore-Horspool with shift table built once per `search_file()` call

### I/O
- Files are `mmap`'d with `MAP_PRIVATE` + `posix_fadvise(POSIX_FADV_SEQUENTIAL)` + `madvise(MADV_SEQUENTIAL)`
- Fused single-pass: newlines detected via AVX-512 in 16 MB chunks, lines searched immediately while data is cache-hot

### Parallelization (unified chunk-based)
| Scenario | Strategy |
|----------|----------|
| 1 file | Split into line-aligned chunks across all available cores |
| Multiple files | All files mmap'd upfront, split into line-aligned chunks proportional to size, distributed across all cores via lock-free work queue |

For example, 24 files on a 72-core system: each file gets ~3 chunks, all 72 cores pull chunks from a single work queue.

### Output
- Matching lines written directly into a thread-local 1 MB output buffer (no intermediate staging)
- Buffer flushed with a single mutex-locked `fwrite` when full or when a thread finishes its range
- Color escape codes (`\033[41m` / `\033[0m`) added when `isatty(STDOUT_FILENO)` is true

### Resource Management
- RAII guards for file descriptors (`FdGuard`) and mmap regions (`MmapGuard`)
- Lock-free `std::atomic<size_t>` work queue index for chunk distribution
- Thread-local reusable vectors for match positions and output buffering

---

## Performance History

All benchmarks on: 2.9 GB file (`txtlog_alien_751.txt`), query `"hoyoverse"`, 24,477 matching lines.

| Change | Time | Speedup vs Baseline | Measured? |
|--------|------|---------------------|----------|
| Baseline (with field-aware code, byte-by-byte scan) | 11.3s | 1.0x | Yes |
| BMH shift table built once (was rebuilding per line) | 7.46s | 1.5x | Yes |
| Fused single-pass + direct fwrite | ~7.0s | ~1.6x | Approximate |
| 16-thread parallelization | 0.634s | 17.8x | Yes (Oct 2025) |
| + batched output + code quality pass + LTO | 0.627s | 18.0x | Yes (Feb 2026) |
| + Profile-Guided Optimization (PGO) | 0.599s | 18.9x | Yes (Feb 2026) |
| All-cores parallelism (72 threads) + madvise + PGO | 0.284s | 39.8x | Yes (Feb 2026) |
| grep (single-threaded, for reference) | 2.669s | — | Yes (Feb 2026) |

**Current measured result: 0.284s hot-cache with PGO + LTO + all-cores, 10.2 GB/s throughput (9.44x faster than grep)**

---

## Benchmark Analysis (Feb 2026)

Full results in `benchmarks/`. Run with `hyperfine` (10 runs, 3 warmup) on 2.9 GB file.

### Timing
| Benchmark | Mean | Stddev | Key takeaway |
|-----------|------|--------|-------------|
| Hot-cache (72 threads, PGO+LTO) | 0.284s | ±0.020s | 10.2 GB/s throughput |
| Output to file vs /dev/null | 0.286s vs 0.274s | | ~12ms (4%) output cost |
| vs grep (to file) | 0.283s vs 2.669s | | **9.44x faster** |

### Hardware counters (16-thread PGO → 72-thread PGO)
| Counter | 16-thread PGO | 72-thread PGO | Change |
|---------|--------------|---------------------|--------|
| Cycles | 21.4B | 23.0B | +7.5% (more cores active) |
| Instructions | 14.5B | 14.8B | +2% |
| IPC | 0.68 | 0.64 | -5.9% (more memory contention) |
| Cache miss rate | 93.1% | 89.9% | **-3.4%** |
| LLC load miss rate | 88.9% | 82.9% | **-6.7%** |
| Branch misses | 398.5M | 101.5M | **-74.5%** |
| Page faults | 59,069 | 105,187 | +78% (more concurrent faults) |
| Sys time | 0.38s | 1.13s | +197% (kernel overhead) |

### Conclusions
- **All-cores parallelism was a 2.11x improvement** (0.599s → 0.284s). The single biggest gain since the initial 16-thread parallelization.
- **Throughput: 10.2 GB/s** — using 32% of system memory bandwidth (4 of 12 DDR4-2666 channels populated). Theoretical ceiling is ~32 GB/s.
- **Branch misses dropped 74.5%** (398M → 101M). Smaller per-thread chunks (~40MB vs ~180MB) fit branch predictor tables much better.
- **LLC miss rate improved** from 88.9% → 82.9%. Smaller chunks also improve cache locality.
- **Page faults nearly doubled** (59K → 105K) and sys time tripled (0.38s → 1.13s) — the cost of 72 threads all faulting concurrently. `MAP_POPULATE` could help here.
- **Output buffering still effective** — only ~12ms (4%) cost to write to a real file.
- **grep spends ~21% in `read()` syscalls** (`copy_page_to_iter`). fff avoids this via mmap.

---

## Completed Optimizations

### 1. Removed field-aware parsing
Eliminated ~120 lines of colon-delimited field parsing and `-f` flag. Simplified codebase.

### 2. BMH shift table fix
The shift table was being rebuilt on every line (40M × 265 ops = 10.6B wasted operations). Now built once in `search_file()` and passed to all callees.

### 3. Fused single-pass with chunked SIMD newline detection
Originally Phase 2A scanned the entire file for newlines in one pass (building a 320 MB offset array), then searched lines in a second pass — causing severe cache thrashing (~93% miss rate). Replaced with a fused approach: detect newlines in 16 MB chunks with AVX-512, search each chunk's lines immediately while data is still cache-hot.

### 4. Direct fwrite output
Replaced `std::string` building + `printf` with direct `memcpy` into a thread-local buffer + single `fwrite` call per line.

### 5. Multi-threaded file processing
Large files (≥100 MB) split into line-aligned chunks processed by up to 16 threads. This was the dominant optimization (94% of total speedup).

### 6. Multi-file work queue
Many-file workloads distributed across all cores via a shared queue with mutex-protected index. Avoids nested parallelization (which would spawn cores × 16 threads).

### 7. Batched output
Replaced per-line mutex-locked `fwrite` (24,477 locks) with thread-local 1 MB output buffers that flush only when full or at end of range. Reduces mutex acquisitions by ~99.8%.

### 8. Code quality and micro-optimization pass
- **Eliminated per-line heap allocations:** Search functions (`search_bmh`, `search_short_fast`) refactored from returning `std::vector` by value to populating a thread-local reusable vector via output parameter. Removes millions of `malloc`/`free` pairs.
- **Removed unnecessary `std::sort`:** Match positions are already produced in left-to-right order by both search algorithms.
- **Eliminated double-buffering:** Output is now built directly into `output_buffer` instead of first into a staging `line_buffer` then copied.
- **Lock-free file queue:** Replaced `std::mutex` + `size_t` with `std::atomic<size_t>::fetch_add` for the multi-file work queue.
- **RAII resource management:** Added `FdGuard` and `MmapGuard` for exception-safe file descriptor and mmap lifetime.
- **Proper type safety:** Replaced all C-style casts with `static_cast`/`reinterpret_cast`. Fixed signed/unsigned `st_size` handling with single conversion point.
- **Removed unused `#include <pthread.h>`** (code uses `std::thread`/`std::mutex`).

---

## Known Issues

### Code Issues
All previously identified issues have been resolved (see Completed Optimizations #8 for the full list).

### Output Ordering
- With multi-threading, output lines may be interleaved across threads (not in file order). This is typical for parallel grep tools but worth noting.

---

## Planned / Unfinished Optimizations

Prioritized by expected impact given the memory-bandwidth-bound profile.

### ~~A. Profile-Guided Optimization (PGO)~~
Done — 5.8% wall-clock improvement (0.636s → 0.599s). Gain came from 14.8% instruction reduction via inlining/dead-code elimination. Available via `make pgo` + `make pgo-use`.

### ~~B. Thread Count Tuning~~
Done — removed 16-thread cap, now uses all available cores via `hardware_concurrency()`. On 72-thread system: 2.11x improvement (0.599s → 0.284s). Also reduced branch misses by 74.5% due to smaller per-thread working sets.

### ~~C. `MAP_POPULATE`~~
Rejected — benchmarked and caused **83% regression** (0.284s → 0.519s). `MAP_POPULATE` serializes page-table setup on a single thread during `mmap()`. With 72-thread parallelism, demand faulting in parallel is strictly faster. Page faults dropped (105K → 61K) but the serial PTE setup time dominated.

### D. PGO re-profile periodically
PGO profile should be regenerated after significant code changes to keep inlining/layout decisions optimal.

### E. `read()` vs `mmap()`
Deprioritized — current data favors mmap. Re-evaluate only if cold-cache becomes a priority.

### ~~F. LTO~~
Done — `-flto` added to `CXXFLAGS` and `LDFLAGS` in Makefile.

### ~~G. NUMA Awareness~~
Benchmarked: CPU-pinned (0.626s) ≈ unpinned (0.627s). No measurable NUMA effect. Deprioritized.

### ~~H. madvise(MADV_SEQUENTIAL)~~
Done — added alongside `posix_fadvise` for mmap-level sequential readahead hint.

---

## Build

```bash
make clean && make production   # Standard optimized build

# PGO (Profile-Guided Optimization) — highest performance
make pgo                        # Step 1: instrumented build
./fff_pgo_gen hoyoverse /srv/fastshare/txtlog_alien_751.txt > /dev/null   # Step 2: training run
make pgo-use                    # Step 3: optimized build + cleanup
```

**Compiler flags:** `-std=c++17 -O3 -march=skylake-avx512 -flto -fno-rtti -DNDEBUG -pthread`

---

## Correctness Tests

```bash
# Small file
./fff hoyoverse ./test_10.txt

# Large file, verify match count
./fff hoyoverse /srv/fastshare/txtlog_alien_751.txt | wc -l   # Expect: 24,477
```

---

## Performance Benchmarking

Requires [`hyperfine`](https://github.com/sharkdp/hyperfine) for statistical benchmarking.
All results are saved to `benchmarks/` for later analysis. Create it with `mkdir -p benchmarks`.

### Hot-cache (measures compute performance)
```bash
# Single tool, 10 runs with 3 warmup
hyperfine --warmup 3 --runs 10 \
  --export-json benchmarks/hot-cache.json \
  --export-markdown benchmarks/hot-cache.md \
  './fff hoyoverse /srv/fastshare/txtlog_alien_751.txt > /dev/null'

# A/B comparison vs grep
hyperfine --warmup 3 --runs 10 \
  --export-json benchmarks/vs-grep.json \
  --export-markdown benchmarks/vs-grep.md \
  './fff hoyoverse /srv/fastshare/txtlog_alien_751.txt > /dev/null' \
  'grep -a hoyoverse /srv/fastshare/txtlog_alien_751.txt > /dev/null'

# Before/after comparison (rename old binary to fff_old first)
hyperfine --warmup 3 --runs 10 \
  --export-json benchmarks/before-after.json \
  --export-markdown benchmarks/before-after.md \
  './fff_old hoyoverse /srv/fastshare/txtlog_alien_751.txt > /dev/null' \
  './fff hoyoverse /srv/fastshare/txtlog_alien_751.txt > /dev/null'
```

### Cold-cache (measures I/O + compute, requires root)
```bash
hyperfine --runs 5 \
  --prepare 'sync; echo 3 > /proc/sys/vm/drop_caches' \
  --export-json benchmarks/cold-cache.json \
  --export-markdown benchmarks/cold-cache.md \
  './fff hoyoverse /srv/fastshare/txtlog_alien_751.txt > /dev/null'
```

### CPU-pinned (eliminates NUMA migration noise)
```bash
# Pin to NUMA node 0 (cores 0-17 on dual Xeon Gold 6140)
hyperfine --warmup 3 --runs 10 \
  --export-json benchmarks/cpu-pinned.json \
  --export-markdown benchmarks/cpu-pinned.md \
  'taskset -c 0-17 ./fff hoyoverse /srv/fastshare/txtlog_alien_751.txt > /dev/null'
```

### Output cost measurement
```bash
# Compare discard vs real file output to isolate I/O cost
hyperfine --warmup 3 \
  --export-json benchmarks/output-cost.json \
  --export-markdown benchmarks/output-cost.md \
  './fff hoyoverse /srv/fastshare/txtlog_alien_751.txt > /dev/null' \
  './fff hoyoverse /srv/fastshare/txtlog_alien_751.txt > /tmp/fff_out.txt'
```

### Hardware counters
```bash
# Overview stats (perf writes to stderr)
perf stat -e cycles,instructions,cache-references,cache-misses,branch-misses,page-faults \
  taskset -c 0-15 ./fff hoyoverse /srv/fastshare/txtlog_alien_751.txt > /dev/null \
  2> benchmarks/perf-stat.log

# Memory bandwidth (LLC = Last Level Cache)
perf stat -e LLC-loads,LLC-load-misses,LLC-stores,LLC-store-misses \
  taskset -c 0-15 ./fff hoyoverse /srv/fastshare/txtlog_alien_751.txt > /dev/null \
  2> benchmarks/perf-llc.log
```

### Log file format notes
- **`*.json`** — hyperfine structured output with per-run times, mean, stddev, min, max
- **`*.md`** — hyperfine markdown table (human-readable summary)
- **`perf-*.log`** — raw `perf stat` output (counter values, IPC, miss rates)

---

## Files in This Repo

| File | Description |
|------|-------------|
| `fff.cpp` | Production source |
| `Makefile` | Build targets: `production`, `pgo`/`pgo-use`, `clean` |
| `DEVELOPMENT.md` | This file — master development reference |
| `test_data.txt`, `test_10.txt` | Small test inputs |
| `benchmarks/` | Benchmark results: hyperfine JSON/MD, perf logs, legacy perf reports |
