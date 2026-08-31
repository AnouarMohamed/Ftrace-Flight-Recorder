# FDR documentation

This directory contains the user and operator documentation for Flight Data
Recorder. The root [README](../README.md) explains the project and provides the
shortest path to a working capture.

## Read this first

| Goal | Document |
|---|---|
| Understand what FDR is and whether it fits | [Project overview](../README.md) |
| Build and run the first host capture | [Getting started](getting-started.md) |
| Select events, filters, buffers, and files | [Configuration reference](configuration.md) |
| Operate, monitor, reload, and troubleshoot | [Operations guide](operations.md) |
| Deploy with plain Kubernetes manifests | [Kustomize guide](../deploy/kubernetes/README.md) |
| Deploy with Helm and enable observability | [Helm guide](../deploy/helm/fdr/README.md) |
| Run the local Prometheus and Grafana lab | [Kind lab](../deploy/kind/README.md) |
| Test real kernels in disposable VMs | [VM validation](../tests/vm/README.md) |
| Review what has been proven | [Validation evidence](validation/README.md) |
| Review the Ubuntu LTS and Noble k3s qualification | [Ubuntu release matrix](validation/2026-08-31-ubuntu-lts-k3s/report.md) |
| Review the performance work and compatibility gates | [Performance optimization plan](performance-optimization-plan.md) |
| Review the userspace copy-loop evidence | [Collector-copy microbenchmark](benchmarks/2026-08-29-text-collector.md) |
| Review the real tracefs text-reader results | [Real-tracefs collector profile](benchmarks/2026-08-30-real-tracefs-text.md) |
| Review the experimental per-CPU/raw result | [Per-CPU backend probe](benchmarks/2026-08-30-per-cpu-backend.md) |
| Review high-core integrity-sampling results | [Trace-loss sampling benchmark](benchmarks/2026-08-30-loss-sampling.md) |
| Review changes since the last release | [Changelog](../CHANGELOG.md) |
| Understand planned work and limits | [Roadmap](../ROADMAP.md) |

## Concepts in plain language

### ftrace

The Linux kernel's built-in tracing framework. It exposes tracepoints and
buffers through tracefs, normally mounted at `/sys/kernel/tracing`.

### tracepoint

A named kernel event such as `sched/sched_switch`. Enabling a tracepoint causes
the kernel to write matching event records into a trace buffer.

### tracefs instance

An isolated tracing workspace under `/sys/kernel/tracing/instances`. FDR gives
each configuration file its own instance so events and buffers do not share the
global tracing state.

### worker and collector

FDR's parent process starts one worker per configuration file. If that file has
a `saveto` directive, the worker remains alive as a collector and continuously
copies `trace_pipe` into the configured file. The compatible collector uses an
8 KiB minimum read, selected by real tracefs profiling rather than a regular
file copy test.

### readiness

The evidence-integrity state. A live FDR process may be unready because a probe
failed, a collector failed, storage discarded output, a file write failed, or
the kernel reported trace loss. Always inspect readiness as well as liveness.

### trace overrun

The kernel overwrote unread events in a trace ring buffer. The recorder may
still be running, but the capture is incomplete. FDR counts this condition and
makes readiness false.

## Supported operating model

FDR is intended to run once on each Linux host or once per selected Kubernetes
node. The administrator owns event selection, storage sizing, access control,
and validation on every supported kernel.

The project does not currently provide a hosted service, central controller,
trace query language, or long-term storage backend.

## Documentation conventions

- Commands assume the repository root unless stated otherwise.
- Host commands use systemd where service management is required.
- Kubernetes examples use the `fdr-system` namespace unless a guide specifies a
  different namespace.
- Destructive load and trace-loss tests belong only in disposable VMs.
- Recorded validation proves only the environment and source revision named in
  its report.
