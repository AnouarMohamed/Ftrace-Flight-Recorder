# Performance optimization plan

Status: implementation in progress
Date: 2026-08-29

## Progress

- Phase 0 has a deterministic 64 MiB collector benchmark with byte-for-byte
  output and metric checks.
- The first Phase 1 tranche caches bounded-output size, uses a conservative
  64 KiB minimum read allocation, schedules storage checks by captured bytes,
  and replaces formatted per-CPU loss parsing with bounded direct reads.
- The initial microbenchmark measured 53.3% lower median process CPU with
  exact output. See the [raw before/after report](benchmarks/2026-08-29-text-collector.md).
- A synthetic 256-CPU benchmark measured 32.9% lower median process CPU in the
  loss sampler, from 4.78 ms to 3.20 ms per complete topology sample. See the
  [loss-sampling report](benchmarks/2026-08-30-loss-sampling.md).
- Metric batching is intentionally not enabled yet: shared counters remain
  current after every completed write until a bounded-time publication design
  is proven not to leave low-rate captures stale.
- Rotation decoupling, cached CPU topology, real-kernel qualification, raw
  capture, and snapshot mode remain open phases below.

## Objective

Reduce FDR collector CPU, syscall, memory, and rotation overhead across small
hosts and high-CPU servers without reducing the evidence collected. The
existing continuous ftrace text collector remains supported and compatible.
Faster capture formats and trigger modes are additive and must fall back
cleanly when a kernel does not support them.

## Preservation contract

An optimization cannot ship if it violates any of these requirements:

- Keep every configured event and filter. Do not sample, silently disable, or
  rewrite probes.
- Preserve every byte and event delivered by `trace_pipe`; do not introduce
  recorder-side loss to meet a CPU target.
- Keep the current ftrace text format, event order, file permissions, append
  behavior, rotation limits, and `minfree` protection in text mode.
- Keep configuration, signals, endpoints, metrics, readiness, and failure
  semantics backward compatible unless a separately documented feature is
  explicitly selected.
- Report kernel loss, storage drops, and write failures as evidence-integrity
  failures. Never hide a performance failure behind liveness.
- Detect kernel capabilities at runtime and retain a tested fallback.
- Make every performance claim reproducible in a disposable environment.

## Evidence motivating the work

The live Fedora 43 workstation run on Linux 7.1.8 exposed two distinct
collector operating points:

| Configuration | Output rate | Collector CPU observed |
|---|---:|---:|
| `sched_switch` plus `sched_wakeup` | 13.7 MiB/s | 72.5% of one CPU |
| Kind lab `sched_wakeup` | about 2.5 MiB/s | 23.7% of one CPU |

These observations are not portable benchmark results. They identify a scaling
problem worth measuring. Source inspection shows avoidable work in the text
collector:

- the read allocation follows `trace_pipe`'s reported block size;
- a bounded output calls `fstat()` for every captured block;
- shared atomic byte counters are updated for every write;
- free-space checks are scheduled by read count rather than time and bytes;
- loss sampling rediscovers and reparses every CPU stats path every five
  seconds.

The benchmark phase must separate kernel tracepoint overhead, text formatting,
collector overhead, and destination-storage overhead before attributing gains.

## Phase 0: benchmark and correctness foundation

Add a deterministic collector harness under `tests/benchmarks/` and retain
reports under `docs/benchmarks/`.

### Deterministic tests

Feed numbered records through a fake trace pipe at controlled byte rates. For
each implementation, verify:

- output bytes and SHA-256 match the input when rotation is disabled;
- concatenated generations contain every record exactly once when rotation is
  enabled;
- reopen signals do not duplicate or omit records;
- `minfree` and rotation failures account for every intentionally discarded
  byte;
- metrics equal persisted and deliberately discarded byte totals;
- shutdown flushes local metric batches before the worker exits.

### Real-kernel scenarios

Run only inside disposable VMs:

1. No enabled events.
2. Scheduler events on an idle guest.
3. Scheduler events under CPU saturation.
4. Process lifecycle events under fork/exec load.
5. Block or filesystem events under sustained I/O.
6. Rotation during sustained capture.
7. Destination storage approaching `minfree`.
8. Controlled collector throttling to prove loss reporting.

For each scenario compare:

- workload without tracing;
- events enabled without a persistent collector;
- the current text collector;
- each optimized collector candidate.

Record event and byte throughput, user and system CPU, syscalls, context
switches, RSS, total kernel trace-buffer allocation, block I/O, rotation pause,
workload impact, and every integrity counter.

### Promotion gates

- Zero unexplained missing, duplicated, or reordered deterministic records.
- Zero additional real-kernel loss at the same buffer and workload.
- No more than 5% CPU regression at low event rates.
- At least 25% lower collector CPU in a sustained text-capture scenario before
  describing the tranche as a performance improvement.
- No more than 1 MiB additional resident memory per text worker.
- Invalid or unsupported tuning values fail before the worker starts.

## Phase 1: optimize the compatible text collector

### 1. Cache output size

Read file size once when opening or reopening the destination. Maintain the
size in memory after successful writes and reset it after rotation. This removes
one `fstat()` from every bounded-output iteration.

External changes remain safe because `SIGUSR1` reopens and restats the file.
FDR continues using `O_APPEND`; administrators must use the documented reopen
signal after external rotation.

### 2. Benchmark larger reads

Do not equate filesystem `st_blksize` with the ideal trace-pipe read size.
Benchmark 4, 16, 64, and 256 KiB allocations. Keep partial reads correct and
keep signal interruption responsive. Select a conservative compiled default
only after real-kernel results; add an explicit advanced override only if
different server classes require it.

### 3. Batch shared metrics

Accumulate written and dropped bytes in worker-local counters. Publish bounded
batches to shared metrics after a byte threshold, a monotonic-time threshold,
rotation, reopen, error, and shutdown. The final totals must be exact; scrape
visibility may lag only by the documented sub-second interval.

### 4. Schedule storage checks by time and volume

Check `fstatvfs()` after the earlier of a monotonic interval or a byte budget.
This keeps protection responsive at high throughput without checking
excessively at low throughput. Recheck immediately after reopen and rotation.

### 5. Optimize loss-stat sampling

Discover per-CPU stats paths once per instance. Use bounded direct reads and
integer parsing instead of repeated directory traversal, `fopen`, and `sscanf`.
Handle CPU hotplug by refreshing the cache when a known path disappears or on
a slower topology interval. Keep the five-second loss-detection objective.

## Phase 2: reduce rotation stalls

The collector must resume writing a new bounded file before slow retention
work such as compression. Implement an internal fast path that:

1. seals or renames the full generation atomically;
2. opens and validates the new mode-0600 destination;
3. resumes collection;
4. delegates optional retention processing to a supervised helper.

Keep existing logrotate behavior as a compatibility mode until the asynchronous
path proves equivalent. A helper failure must be visible but must not make a
partial generation appear complete.

Rotation tests must cover rename failures, reopen failures, helper failure,
signals during rotation, filesystem exhaustion, symlinks, non-regular targets,
and process termination at every transition.

## Phase 3: self-observation and server sizing

Add counters only when they enable a concrete performance decision:

- collector read and write calls;
- bytes per read and write;
- free-space checks;
- rotation duration and failures;
- configured trace-buffer bytes across all CPUs;
- selected collector backend and capability fallback.

Prometheus should derive rates from counters. FDR may emit rate-limited warnings
for sustained high write rate or rotation churn, but it must never disable or
sample probes automatically.

Benchmark CPU counts of 2, 8, 32, and at least one high-core-count environment.
Document how per-CPU buffers scale and consider an optional total-memory sizing
directive. Keep explicit per-CPU sizing fully supported.

## Phase 4: optional raw zero-copy backend

Linux exposes `per_cpu/cpuN/trace_pipe_raw` for binary ring-buffer extraction
and documents `splice()` as the fast transfer path. Prototype this as an
explicit backend, not a silent replacement for text mode.

Requirements:

- Produce a standard interoperable capture, preferably `trace.dat`, rather than
  a private binary format.
- Preserve event formats, clocks, endianness, architecture, CPU mapping, and
  all metadata required for decoding.
- Demonstrate decoded event equivalence with text mode for the same controlled
  workload.
- Open the result with pinned `trace-cmd` and KernelShark versions.
- Detect unsupported files or syscalls and fall back before tracing begins.
- Benchmark per-CPU readers, `buffer_percent`, merge behavior, and CPU hotplug.

Text remains the default until raw capture is qualified across the supported
kernel matrix and its operational workflow is at least as clear.

## Phase 5: snapshot-triggered flight recorder

Where `CONFIG_TRACER_SNAPSHOT` is available, ftrace can swap the active ring
buffer with a spare while tracing continues. Use that capability to retain a
high-detail pre-trigger window without continuously formatting and writing all
events to disk.

The intended state remains:

```text
ARMED -> TRIGGERED -> CAPTURING_POST_WINDOW -> SEALED -> ARMED
```

Snapshot mode is additive. It trades a second kernel buffer for much lower
armed-state disk activity. Continuous text capture remains available for hosts
without snapshot support and for operators who require it.

## Approaches explicitly rejected

- Event sampling or automatic probe disabling.
- Silently shrinking trace buffers.
- Dropping trace blocks to satisfy a CPU budget.
- Default CPU quotas, `nice`, or idle I/O priority that can starve collection.
- Compression in the capture hot path.
- A mandatory custom binary format.
- `io_uring`, direct I/O, or multi-threaded merging before simpler syscall and
  batching changes are measured.
- Performance defaults inferred from a single workstation or storage device.

## Implementation sequence

1. `bench: add deterministic collector performance harness`
2. `perf: cache output size and batch shared byte metrics`
3. `perf: benchmark and tune trace-pipe read batching`
4. `perf: schedule free-space checks by time and volume`
5. `perf: cache per-CPU integrity sampling state`
6. `perf: decouple retention work from capture rotation`
7. `capture: prototype interoperable raw splice backend`
8. `capture: add snapshot-triggered recording`

Each change receives its own before/after report and can be reverted without
changing configurations. Kubernetes requests and operational recommendations
change only after the VM matrix provides evidence.

## Completion criteria

The optimization program is complete when:

- text mode is demonstrably more efficient and remains backward compatible;
- all deterministic and real-kernel integrity gates pass;
- supported servers have measured resource guidance;
- rotation no longer causes avoidable collector stalls;
- the optional raw capture opens in an established external tool;
- snapshot mode preserves demonstrated pre-trigger and post-trigger evidence;
- documentation publishes limitations and raw results alongside improvements.

Primary kernel reference: [Linux ftrace documentation](https://www.kernel.org/doc/html/latest/trace/ftrace.html).
