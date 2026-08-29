# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and releases use
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Sample tracefs loss statistics every five seconds and export separate
  Prometheus counters for overruns, dropped events, and commit overruns.
- Make readiness degrade when the kernel reports new trace loss so a live
  recorder cannot silently present known-incomplete evidence as ready.
- Add optional Helm `PodMonitor`, six evidence-integrity alert rules, a compact
  Grafana dashboard, and an ingress `NetworkPolicy`.
- Add a tracefs preflight init container with actionable diagnostics for
  Kubernetes startup failures.
- Add a pinned Kind observability lab covering real capture, Prometheus
  discovery, Grafana states, configuration rollout, collector recovery,
  bounded rotation, and tracefs cleanup.
- Add disposable KVM harnesses for installed-kernel, Ubuntu LTS, controlled-loss,
  systemd, and k3s validation paths.
- Add committed Kind and KVM reference evidence for Linux 7.1.8 and Linux
  7.0.12/7.1.8 respectively.
- Add comprehensive getting-started, configuration, operations,
  troubleshooting, incident-evidence, and validation documentation.
- Add a deterministic collector benchmark that verifies byte-for-byte output,
  exact byte metrics, and zero unaccounted drops across timed runs.
- Add a staged performance plan with explicit evidence-preservation gates and
  reproducible qualification requirements.

### Changed

- Pin Kubernetes and Helm deployment images to the tested application release
  instead of a floating tag.
- Restrict default scheduling to Linux nodes, require explicit tolerations,
  and remove the default CPU limit to avoid collector throttling.
- Make the host `/lib/modules` mount opt-in for configurations that actually
  use `modprobe`.
- Harden CI checks for rendered Kubernetes invariants, strict Helm linting,
  dashboard JSON, shell scripts, and the lab automation.
- Advance the Helm chart to version 0.5.0 for the new observability and
  hardening resources.
- Replace the validation overview artwork with a minimal technical evidence
  summary focused on recorded results.
- Reduce text-collector hot-path work with a 64 KiB minimum read allocation,
  cached bounded-output size, and byte-budgeted free-space checks while keeping
  the text format, append behavior, size limit, and drop accounting intact.
- Remove a redundant destination metadata lookup from fallback rotation.
- Parse per-CPU trace-loss statistics with bounded direct reads and checked
  integer conversion instead of formatted stdio scanning.

### Documentation

- Add a milestone-based product and Kubernetes hardening roadmap.
- Document the privileged host-kernel security boundary, module-loading
  tradeoffs, monitoring exposure, and validation limits.
- Document exactly what parse-only validation, reload, readiness, file
  protection, and each metric can and cannot prove.

### Validation

- Pass the complete Kind lifecycle on Linux 7.1.8, including healthy and
  degraded Grafana captures and Prometheus target discovery.
- Pass systemd lifecycle, zero-loss normal load, and controlled-loss detection
  in disposable KVM guests on Linux 7.0.12 and 7.1.8.
- Keep Ubuntu LTS and Noble k3s qualification visible as open release work
  rather than treating the local-kernel results as universal coverage.
- Record a five-round 64 MiB collector microbenchmark with exact output: median
  process CPU fell from 135.9 ms to 63.5 ms (53.3%) on the named Fedora test
  host; real-kernel and storage qualification remain explicitly open.

## [1.4.0] - 2026-08-23

### Added

- C11 `fdrd` daemon with isolated tracefs instances and one worker per
  configuration file.
- Configuration for modules, tracepoint filters, per-CPU buffers, free-space
  protection, bounded files, and logrotate integration.
- Parent supervision, signal-based reload and reopen behavior, and graceful
  tracefs cleanup.
- HTTP liveness, readiness, and Prometheus metrics.
- systemd, RPM, OCI image, Kustomize, and Helm deployment assets.
- Unit, integration, sanitizer, packaging, and container checks.

[Unreleased]: https://github.com/AnouarMohamed/fdr-k8s/compare/v1.4.0...HEAD
[1.4.0]: https://github.com/AnouarMohamed/fdr-k8s/releases/tag/v1.4.0
