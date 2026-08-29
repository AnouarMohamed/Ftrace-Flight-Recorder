# Trace-loss sampling benchmark: 2026-08-30

## Result

Replacing formatted stdio parsing with bounded direct reads reduced median
process CPU by 32.9% in a synthetic 256-CPU trace-loss sampling benchmark. The
optimized sampler still parsed every CPU on every round and all three loss
counters remained exact.

The absolute cost was already small: the median fell from about 4.78 ms to
3.20 ms for one complete 256-CPU sample. FDR samples once every five seconds,
so this change matters most on high-core servers with several trace instances.

## Method

The benchmark creates 256 fake `per_cpu/cpuN/stats` files with the same counter
labels used by tracefs. It samples the complete topology 50 times and verifies
that overrun, dropped-event, and commit-overrun totals remain zero. Five
independent benchmark processes were run for each implementation.

```sh
make benchmark-loss
```

| Property | Value |
|---|---|
| Baseline revision | `fc0208a` |
| Optimized parser revision | `d1ad3a4` |
| Kernel | Linux 7.1.8-100.fc43.x86_64 |
| CPU | Intel Core i7-8650U, 4 cores / 8 threads |
| Simulated trace CPUs | 256 |
| Full-topology samples per process | 50 |
| Compiler mode | GCC C11, `-O2 -g` plus project warnings |

## Raw measurements

| Candidate | Round | Process CPU (ns) | ns per CPU stats sample |
|---|---:|---:|---:|
| Baseline | 1 | 233,771,139 | 18,263.37 |
| Baseline | 2 | 237,393,444 | 18,546.36 |
| Baseline | 3 | 238,833,116 | 18,658.84 |
| Baseline | 4 | 243,372,553 | 19,013.48 |
| Baseline | 5 | 266,577,857 | 20,826.40 |
| **Baseline median** | | **238,833,116** | **18,658.84** |
| Optimized | 1 | 170,450,810 | 13,316.47 |
| Optimized | 2 | 161,143,902 | 12,589.37 |
| Optimized | 3 | 160,226,156 | 12,517.67 |
| Optimized | 4 | 145,483,690 | 11,365.91 |
| Optimized | 5 | 150,413,998 | 11,751.09 |
| **Optimized median** | | **160,226,156** | **12,517.67** |

The synthetic files isolate traversal, file reads, and parsing. They do not
model tracefs latency, CPU hotplug, or the rest of the HTTP loop. The planned
high-core VM matrix must confirm the result on real per-CPU tracefs files before
it becomes server-sizing guidance.
