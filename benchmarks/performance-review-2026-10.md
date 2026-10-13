# October 2026 performance review

Baseline: `e82a95bef1193a376d2e4e7e72cee6b4dfafd082`.
Target: dual Xeon Gold 6140, Linux, sparse literal matches in cached logs.

## Retained changes

1. Skip four 64-byte candidate blocks with one combined empty-filter decision.
   Check the first block before issuing the three extra filters, so dense
   candidate blocks do not pay for speculative scans. Candidate-bearing
   regions keep the original ordered verification and line emission.
2. Locate preceding newlines using bounded AVX-512 byte comparisons and the
   highest set mask bit; locate following newlines using bounded `memchr`.

No C++ standard change or ISA extension beyond Skylake-SP is required. The
6140 lacks VBMI/VBMI2 and several later AVX-512 extensions; unconditionally
using these would make the tool fail on the requested deployment hardware.
Algorithmic changes provided measurable gains without those extensions.

## Measurements

2026-10-13, Xeon Platinum 8573C VM, 9 visible CPUs, 8 CPU-equivalent cgroup
quota, one visible NUMA node, GCC 13.3, glibc 2.39. Both binaries:
`-std=c++17 -O3 -march=skylake-avx512 -flto -fno-rtti -DNDEBUG -pthread`.
No PGO. Three warmups per binary, 40 runs each, randomly interleaved baseline
and candidate, stdout to `/dev/null`. Includes process startup, mapping,
thread creation, scanning, output copying, and teardown. No other tests or
benchmark processes ran during the recorded comparison.

The log corpus is ~1.09 GiB (1,048,576 synthetic log lines repeated eight
times), with a sparse hit every 4,096 lines. Long-line input is ~128 MiB,
4,096 lines of 32 KiB with a match near the middle. The false-candidate corpus
is 128 MiB of 127 `z` bytes per line; `zzzzzzzza` passes the two-byte filter
frequently but never fully matches. This specifically probes wasted work.

| Workload | Baseline median ms | Candidate median ms | Less time | Bootstrap 95% interval, percentage points |
|---|---:|---:|---:|---:|
| Sparse `hoyoverse` | 50.61 | 46.71 | 7.7% | 2.7 to 16.5 |
| Sparse `hoyover` | 47.15 | 42.60 | 9.6% | 3.4 to 13.0 |
| Sparse `xyz` in identifiers | 46.67 | 43.74 | 6.3% | -1.1 to 12.1 |
| No-match `zzzzzzzz` | 49.63 | 44.60 | 10.1% | 3.5 to 16.0 |
| Every-line `INFO` | 245.47 | 84.29 | 65.7% | 62.6 to 70.0 |
| Every-line `a` | 296.57 | 90.35 | 69.5% | 67.4 to 71.5 |
| Long-line `hoyoverse` | 37.84 | 15.84 | 58.1% | 51.5 to 61.7 |
| False-candidate `zzzzzzzza` | 128.79 | 127.83 | 0.7% | -2.6 to 4.3 |

Raw samples are in `performance-review-2026-10.json`. Intervals bootstrap
4,000 paired run indices and use the ratio of medians. They characterize this
run, not all hardware or input distributions. `xyz` is inconclusive at this
confidence level; the adversarial case is effectively unchanged.

Earlier exploratory batches also improved the main sparse queries, although
individual percentages varied substantially. Retaining all four masks for
ordered processing performed less consistently and was rejected. A forced
single-worker experiment on the earlier four-filter candidate improved the
sparse queries by ~6–16% in elapsed time; it is supporting evidence, not a
measurement of the final source.

**Limits:** newer CPU microarchitecture, synthetic data, hot page cache,
virtual storage, and one NUMA node. These are not Gold 6140 results, cold-disk
results, terminal-color measurements, real-output-file measurements, or PGO
comparisons. Confirm the improvement on the real corpus and regenerate PGO
profiles before claiming a deployment-wide gain. More than 1 GiB of input
exceeds the reported 260 MiB LLC, but actual physical cache topology is not
fully exposed by this VM.

## Reproduce

```sh
git show e82a95b:fff.cpp > /tmp/fff-baseline.cpp
g++ -std=c++17 -O3 -march=skylake-avx512 -flto -fno-rtti -DNDEBUG -pthread -o /tmp/fff-baseline /tmp/fff-baseline.cpp
make production
python3 tools/benchmark.py /tmp/fff-baseline ./fff --directory /tmp/fff-corpus --runs 40 --output /tmp/fff-results.json
```

The script reuses corpus files if present. Use a new directory for fresh data.
For deployment validation, also alternate baseline/current runs of the actual
`/srv/fastshare` corpus with the real queries; synthetic speedups cannot replace
that check. Train baseline and candidate independently if comparing PGO.

## Correctness validation

- `make test`: scalar-reference comparison, randomized binary text, patterns
  of 1/2/7/8/63/64/65/129 bytes, rare final-offset filter bytes, every tail size
  from 1 to 768 against an inaccessible page, small line-aligned chunks, and
  lines exceeding the output buffer.
- ASan + UBSan pass. Leak detection disabled because LeakSanitizer cannot
  inspect task information in this environment; no leak-check claim.
- CLI output multisets agree with a literal reference on single-file and
  parallel multi-file sparse input, including repeated input paths.
- `git diff --check` passes. There is no configured CI workflow in this repo.

## Existing issues found (not changed by this optimization)

- **Silent partial scans and output loss:** `map_file` discards all open/stat/
  mapping failures, and `fwrite` results are ignored. The CLI returns zero
  regardless of failures or matches. This is the main correctness concern:
  downstream callers cannot distinguish a successful scan from lost inputs or
  output. Report errors and return an explicit failure status in a follow-up.
- **Affinity is not respected on NUMA:** `get_nprocs()` and node CPU lists ignore
  the caller's allowed CPU mask; `run_numa` sets the full node mask. `taskset`
  restrictions may be widened, so documented pinned tests do not necessarily
  pin the NUMA path. Intersect node masks with `sched_getaffinity`, count only
  usable CPUs, check affinity failures, and cap workers to available chunks.
- **NUMA placement is heuristic:** three page samples do not establish locality
  of an entire multi-MiB chunk, and unmapped/not-resident samples or denied
  queries produce unknown status. No work stealing can strand work on one
  node. Measure node bandwidth, residency, imbalance, and remote accesses
  before assuming this strategy is faster than the flat queue.
- **Long-line chunk setup can rescan the same suffix repeatedly:** successive
  split targets may fall in one huge line, repeatedly walking to the same
  newline and creating empty chunks. Clamp each new target to the previous
  boundary to avoid repeated work; benchmark before altering scheduling.
- **Eager output allocation:** every worker allocates/zeros 1 MiB even with no
  hits. Lazy allocation at first emission is another small-input/no-match
  candidate worth measuring, especially at 72 threads.
- Existing output order is nondeterministic. Input files must remain stable
  during mmap scanning; truncation can cause SIGBUS. Empty and newline-bearing
  queries intentionally match nothing, unlike general grep behavior.

The old development description used BMH/per-line newline scanning, while the
current code uses whole-range two-byte SIMD filtering. Historical counter
interpretations and claims of a memory-bandwidth ceiling should be re-profiled
against the actual current algorithm and real NUMA hardware.
