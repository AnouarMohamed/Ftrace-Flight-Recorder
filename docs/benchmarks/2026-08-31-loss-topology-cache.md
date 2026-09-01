# Trace-loss topology cache benchmark

Date: 2026-08-31  
Status: passed, synthetic qualification

## Result

FDR now caches the discovered `per_cpu/cpuN/stats` paths instead of reopening
and scanning the tracefs topology directory every five seconds. It refreshes
the cache when the online CPU count changes, when a cached path disappears, or
after twelve samples as a slower fallback.

In fifteen alternating 256-CPU benchmark pairs, median process CPU for fifty
complete topology samples fell from 134.07 ms to 128.33 ms, a 4.3% reduction.
This is a small additive improvement, not a production sizing claim. The more
important result is that recurring directory traversal is now bounded while
all per-CPU loss files are still read on every sample.

## Correctness

The trace unit test now exercises both refresh paths:

- a newly added synthetic CPU is discovered by the bounded periodic refresh,
  and its overrun and dropped-event counters are included exactly;
- removing a cached CPU stats path triggers immediate rediscovery without
  failing the remaining sample; and
- the cache is released on instance teardown and configuration reload.

On a real system, an online CPU count change triggers rediscovery on the next
five-second integrity sample. The periodic refresh remains as a fallback for
topology changes that do not alter that count.

## Method

The existing `benchmark_loss` harness created 256 synthetic CPU directories
with kernel-format stats files and sampled every file fifty times. Baseline and
candidate processes alternated to reduce drift. Both used GCC C11 with `-O2
-g` and the project warning flags.

| Property | Value |
|---|---|
| Baseline revision | `992c441` |
| Candidate | worktree based on `992c441` plus topology-cache changes |
| Kernel | Linux `7.1.8-100.fc43.x86_64` |
| Host CPU | Intel Core i7-8650U, 4 cores / 8 threads |
| Synthetic trace CPUs | 256 |
| Samples per process | 50 |
| Alternating process pairs | 15 |

Reproduce either candidate with:

```sh
make benchmark-loss
```

## Raw measurements

| Pair | Baseline CPU ns | Candidate CPU ns |
|---:|---:|---:|
| 1 | 135,511,127 | 128,325,661 |
| 2 | 136,115,767 | 136,822,322 |
| 3 | 134,069,351 | 128,779,209 |
| 4 | 136,483,431 | 137,334,370 |
| 5 | 131,847,978 | 128,113,862 |
| 6 | 139,884,319 | 112,520,456 |
| 7 | 134,394,793 | 122,732,153 |
| 8 | 132,455,736 | 123,927,189 |
| 9 | 134,038,893 | 132,734,512 |
| 10 | 130,845,487 | 128,424,528 |
| 11 | 136,763,529 | 123,083,030 |
| 12 | 138,781,513 | 123,839,706 |
| 13 | 128,243,739 | 130,146,142 |
| 14 | 101,098,878 | 127,587,219 |
| 15 | 131,296,464 | 128,920,931 |
| **Median** | **134,069,351** | **128,325,661** |

The median cost per CPU stats sample was 10,474 ns for the baseline and 10,025
ns for the candidate.

## Limits

The files were ordinary synthetic files on an eight-thread host. This isolates
topology traversal and parsing, but does not model tracefs latency, actual CPU
hotplug, NUMA effects, or a high-core-count kernel. The roadmap's real-kernel
high-core matrix remains open before changing server sizing guidance.
