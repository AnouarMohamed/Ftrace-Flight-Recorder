# Per-CPU text and raw backend probe

Date: 2026-08-30  
Status: extraction prototype passed; production backend not qualified  
Kernel: Fedora `7.1.8-100.fc43.x86_64`

## Result

The capability-checked probe opened all four CPU-local readers. CPU-local text
and raw-page extraction both produced non-empty output on every vCPU with zero
trace overruns, dropped events, or commit overruns.

Raw pages were moved through a kernel pipe into one file per CPU with two-stage
`splice()`. The direct special-file-to-regular-file attempt correctly failed
with `EINVAL`; using a real pipe endpoint is required for this path.

| Mode | CPU-local files | Captured bytes | Collector CPU | Capture batches | Max VmHWM | Kernel loss |
|---|---:|---:|---:|---:|---:|---:|
| Per-CPU text | 4 | 66,588,697 | 1.65 s | 8,436 | 1,556 KiB | 0 |
| Per-CPU raw | 4 | 24,530,944 | 0.09 s | 504 | 1,448 KiB | 0 |

The controlled workload completed 1,618,688 yields during text capture and
2,161,744 during raw capture. This makes raw clearly promising, but these are
separate focused runs and the binary and text byte counts are not comparable.
The result is not a general 94% FDR CPU claim.

## What was preserved

- The same `sched_switch` and `sched_wakeup` events were enabled.
- No event sampling, probe removal, smaller trace buffer, CPU quota, or
  intentional recorder drop was used.
- Each CPU had its own output stream, so the kernel did not perform the global
  text merge.
- The raw evidence bundle retained `header_page`, `header_event`, both event
  formats, trace clock selection, printk formats, and saved command lines.
- Reported collector bytes matched the final files exactly and every collector
  thread ended without an error.

## Why raw is not a product backend yet

The four `.raw` files plus copied metadata are a prototype bundle, not a
standard `trace.dat`. They have not been decoded with a pinned `trace-cmd` or
opened in KernelShark. Decoder equivalence, architecture and endianness
metadata, CPU hotplug, clock correlation, rotation, storage protection,
fallback behavior, and incident workflow are still unproven.

CPU-local text also changes the evidence contract: separate files preserve
per-CPU order but do not provide the current globally merged text stream. It
therefore cannot silently replace compatible text mode.

Production FDR continues using the qualified global text collector. Raw must
remain explicit and additive until a standard archive passes the complete
preservation contract.

## Reproduction

```bash
FDR_PERF_MODE=backend tests/vm/local-performance.sh
```

The host harness builds glibc-compatible candidates, boots the installed kernel
in a disposable four-vCPU KVM guest, captures the metadata, verifies per-CPU
outputs and loss counters, and removes a successful overlay. The raw counters
are committed in [`2026-08-30-per-cpu-backend.tsv`](2026-08-30-per-cpu-backend.tsv).
