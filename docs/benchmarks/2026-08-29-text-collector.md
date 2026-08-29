# Text collector benchmark: 2026-08-29

## Result

The first compatible text-collector optimization cut median collector process
CPU by 53.3% in the deterministic 64 MiB copy benchmark. Every run produced a
byte-for-byte match and reported zero dropped bytes.

This is a focused microbenchmark, not a claim that a real ftrace workload will
use 53.3% less total CPU. Kernel event generation, text formatting, storage,
and workload impact require the disposable-VM matrix in the performance plan.

## Compared revisions

| Candidate | Revision | Collector behavior |
|---|---|---|
| Baseline | `fc0208a` | Filesystem block-sized reads and one `fstat()` per bounded write |
| Optimized | `8f6dd2e` | Minimum 64 KiB reads, cached output size, and byte-budgeted free-space checks |

Both candidates used the same benchmark program added by `fc0208a`.

## Environment

| Property | Value |
|---|---|
| Date | 2026-08-29 |
| Kernel | Linux 7.1.8-100.fc43.x86_64 |
| Architecture | x86-64 |
| CPU | Intel Core i7-8650U, 4 cores / 8 threads |
| Compiler flags | GCC, C11, `-O2 -g` plus project warnings |
| Input per round | 67,108,864 bytes |
| Rounds | 5 per candidate |

The workstation had substantial unrelated editor activity. Process CPU time is
the primary comparison because it was much more stable than wall time.

## Command

```sh
make benchmark
```

The runner prints the kernel, architecture, input bytes, wall time, process CPU
time, and throughput for every round. The benchmark generates deterministic
input, invokes the real `fdr_harvest_run()` path, and then requires:

- an exact byte-for-byte input/output match;
- `fdr_bytes_written_total` equal to 67,108,864;
- `fdr_bytes_dropped_total` equal to zero;
- a successful collector exit.

## Raw measurements

### Baseline (`fc0208a`)

| Round | Wall time (ns) | Process CPU (ns) | Throughput (MiB/s) |
|---:|---:|---:|---:|
| 1 | 136,055,716 | 133,831,663 | 470.40 |
| 2 | 132,046,352 | 127,976,286 | 484.68 |
| 3 | 147,208,193 | 144,847,894 | 434.76 |
| 4 | 141,155,424 | 135,912,547 | 453.40 |
| 5 | 149,574,891 | 137,891,652 | 427.88 |
| **Median** | **141,155,424** | **135,912,547** | **453.40** |

### Optimized (`8f6dd2e`)

| Round | Wall time (ns) | Process CPU (ns) | Throughput (MiB/s) |
|---:|---:|---:|---:|
| 1 | 69,100,200 | 63,530,717 | 926.19 |
| 2 | 82,337,575 | 64,486,025 | 777.29 |
| 3 | 87,356,573 | 63,652,210 | 732.63 |
| 4 | 126,589,484 | 62,701,934 | 505.57 |
| 5 | 64,017,818 | 62,742,313 | 999.72 |
| **Median** | **82,337,575** | **63,530,717** | **777.29** |

## Interpretation

| Median measure | Baseline | Optimized | Change |
|---|---:|---:|---:|
| Process CPU | 135.9 ms | 63.5 ms | 53.3% lower |
| Wall time | 141.2 ms | 82.3 ms | 41.7% lower |
| Throughput | 453.4 MiB/s | 777.3 MiB/s | 71.4% higher |

The optimized worker allocates at most about 60 KiB more userspace read-buffer
memory when the old path selected 4 KiB. It does not change the configured
kernel trace-buffer allocation.

The gain comes from fewer read/write iterations and eliminating the bounded
output's repeated file-size syscall. Output is still opened with `O_APPEND`;
successful writes update the cached size, and reopen or rotation restats the
active file. Free-space protection remains enabled and is checked on the first
block and approximately every MiB of traffic.

## Qualification still required

Before changing production sizing guidance, run the real-kernel scenarios in
the [performance optimization plan](../performance-optimization-plan.md): idle,
scheduler load, fork/exec load, sustained I/O, rotation, low disk space, and
controlled loss. Those runs must measure kernel loss, storage drops, workload
impact, RSS, context switches, and storage behavior in addition to collector
CPU.
