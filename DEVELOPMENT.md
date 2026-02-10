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
- Files are `mmap`'d with `MAP_PRIVATE` + `posix_fadvise(POSIX_FADV_SEQUENTIAL)`
- Fused single-pass: newlines detected via AVX-512 in 16 MB chunks, lines searched immediately while data is cache-hot

### Parallelization (adaptive)
| Scenario | Strategy |
|----------|----------|
| 1 file | Split into line-aligned chunks across up to 16 threads |
| Few files (≤ cores/4) | Each file processed sequentially with internal 16-thread parallelization |
| Many files (> cores/4) | Work queue: one thread per core, each pulls files from a shared queue (no internal parallelization) |

### Output
- Matching lines written directly into a thread-local 1 MB output buffer (no intermediate staging)
- Buffer flushed with a single mutex-locked `fwrite` when full or when a thread finishes its range
- Color escape codes (`\033[41m` / `\033[0m`) added when `isatty(STDOUT_FILENO)` is true

### Resource Management
- RAII guards for file descriptors (`FdGuard`) and mmap regions (`MmapGuard`)
- Lock-free `std::atomic<size_t>` file queue index for multi-file work distribution
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
| grep (single-threaded, for reference) | 2.632s | — | Yes (Feb 2026) |

**Current measured result: 0.627s hot-cache, 1.705s cold-cache (4.19x faster than grep)**

---

## Benchmark Analysis (Feb 2026)

Full results in `benchmarks/`. Run with `hyperfine` (10 runs, 3 warmup) on 2.9 GB file.

### Timing
| Benchmark | Mean | Stddev | Key takeaway |
|-----------|------|--------|-------------|
| Hot-cache | 0.627s | ±0.005s | Tight variance (0.8% CV) |
| Cold-cache | 1.705s | ±0.016s | 2.72x slower — I/O dominates |
| CPU-pinned (NUMA 0) | 0.626s | ±0.004s | Identical to unpinned |
| Output to file vs /dev/null | 0.634s vs 0.635s | | No output cost — batching works |
| vs grep (to file) | 0.628s vs 2.632s | | **4.19x faster** |

### Hardware counters
| Counter | Value | Interpretation |
|---------|-------|---------------|
| IPC | 0.73 | Low — CPU stalled on memory |
| Cache miss rate | 92.8% | Nearly all accesses miss |
| LLC miss rate | 88.9% | Data comes from DRAM |
| Branch misses | 397M | Significant — PGO target |
| Page faults | 59K | Reasonable for 2.9 GB mmap |

### Conclusions
- **Workload is memory-bandwidth bound.** The 92.8% cache miss rate and 0.73 IPC mean the CPU is mostly waiting for DRAM. CPU-side code optimizations (heap alloc, sort, double-buffer removal) improved code quality but did not measurably change wall-clock time.
- **NUMA effects are negligible** for this workload.
- **Output buffering is fully effective** — zero measurable cost to write output.
- **Cold-cache penalty is 1.08s** (63% of hot-cache time), dominated by kernel page fault handling (system time: 0.33s hot → 1.60s cold).
- **grep spends ~21% in `read()` syscalls** (`copy_page_to_iter`). fff avoids this via mmap.
- **397M branch misses** is the most actionable CPU-side metric — PGO should help.

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

### A. Profile-Guided Optimization (PGO)
Now available via `make pgo` + `make pgo-use`. Targets the 397M branch misses with profile-guided branch layout and code placement.
**Estimated impact:** 5–15% (most impactful remaining optimization).

### B. Thread Count Tuning
Currently capped at 16 threads for intra-file parallelism. With memory bandwidth saturated (92.8% cache miss rate), fewer threads may perform equally well or better. Test 4, 8, 12, 16 to find the sweet spot.

### C. `MAP_POPULATE` + `madvise(MADV_SEQUENTIAL)`
**Rationale:** `MAP_POPULATE` pre-faults all pages at mmap time (eliminates 59K on-demand page faults). `madvise(MADV_SEQUENTIAL)` provides mmap-specific readahead hint (complements `posix_fadvise`).
**Estimated impact:** Significant for cold-cache (1.705s → potentially sub-1.2s). Minimal for hot-cache.

### D. `read()` vs `mmap()`
**Rationale:** grep's profile shows 21% in `read()` syscalls, but this includes `copy_page_to_iter` (kernel→user copy) that mmap avoids. Current data suggests mmap is the right choice. Re-evaluate only if cold-cache performance becomes a priority.

### ~~E. LTO~~
Done — `-flto` added to `CXXFLAGS` and `LDFLAGS` in Makefile.

### ~~F. NUMA Awareness~~
Benchmarked: CPU-pinned (0.626s) ≈ unpinned (0.627s). No measurable NUMA effect for this workload. Deprioritized.

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
