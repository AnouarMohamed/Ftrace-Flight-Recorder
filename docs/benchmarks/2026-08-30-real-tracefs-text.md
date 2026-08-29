# Real-tracefs text collector profile

Date: 2026-08-30  
Status: passed, focused qualification  
Kernel: Fedora `7.1.8-100.fc43.x86_64`

## Decision

Use an 8 KiB minimum read allocation for the compatible text collector.

The 8 KiB candidate reduced total collector CPU per captured GiB by 14.9%
against the pre-optimization baseline. A 64 KiB allocation was only 0.5%
better than 8 KiB, which is not material in this three-round profile and does
not justify an allocation eight times larger. The result is consistent with
Linux's roughly 8 KiB internal trace-sequence formatting buffer.

This is a real `trace_pipe` result, but it is one kernel and one workload. The
14.9% result is below the plan's 25% promotion gate, so it is evidence for the
read-size choice, not a claim that the full optimization program is complete.

## Aggregate results

Each value covers three 10-second runs. CPU is the collector worker's user plus
system time from `/proc/<pid>/stat`, normalized by bytes captured. Reads and
characters are from `/proc/<pid>/io`.

| Candidate | Captured | CPU s/GiB | System s/GiB | Reads/MiB | Mean bytes/read | Max VmHWM | Integrity loss |
|---|---:|---:|---:|---:|---:|---:|---:|
| Baseline | 416.25 MiB | 36.557 | 33.949 | 983.5 | 1,064.7 | 1,424 KiB | 0 |
| 4 KiB | 420.22 MiB | 32.970 | 31.045 | 1,071.3 | 976.6 | 1,424 KiB | 0 |
| **8 KiB** | **452.08 MiB** | **31.123** | **29.469** | **946.1** | **1,101.6** | **1,496 KiB** | **0** |
| 16 KiB | 441.01 MiB | 31.671 | 30.023 | 950.5 | 1,100.3 | 1,500 KiB | 0 |
| 64 KiB | 467.46 MiB | 30.975 | 29.354 | 926.1 | 1,129.7 | 1,348 KiB | 0 |

Every run reported zero `fdr_bytes_dropped_total`, trace overruns, trace dropped
events, and trace commit overruns. Fourteen metric/file snapshots matched
exactly. One snapshot caught the stopped worker between a successful 445-byte
write and its metric increment; the profiler records that bounded observation
race rather than treating it as evidence loss.

## Method

The disposable KVM guest had four vCPUs and 4 GiB RAM. Its isolated FDR instance
used 32 MiB of trace buffer per CPU and enabled `sched_switch` plus
`sched_wakeup`. Four workload threads generated controlled bursts of
`sched_yield()` calls. Candidate order rotated each round to reduce ordering
bias.

The profiler freezes only the collector worker with `SIGSTOP` at the end of
each load window, then reads process CPU, I/O, memory, output size, and FDR
integrity metrics. It resumes the worker before normal systemd shutdown. This
does not make `trace_pipe` return EOF and keeps tracefs behavior real.

All candidates and the baseline were built in Debian 12 against glibc 2.36 so
the binaries matched the guest. The baseline is commit `fc0208a`; candidate
sources were the worktree based on `44611a3`. The harness is
`tests/vm/local-performance.sh` and the committed raw rows are in
[`2026-08-30-real-tracefs-text.tsv`](2026-08-30-real-tracefs-text.tsv).

## Limits and next work

- Repeat on the supported Ubuntu LTS kernel and more CPU counts.
- Add fork/exec, block-I/O, idle, rotation, and disk-pressure profiles.
- Measure an optional per-CPU/raw backend separately; do not infer its gain
  from this text-reader result.
- Keep text mode as the compatible default and preserve every configured event.
