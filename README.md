# Flight Data Recorder

[![CI](https://img.shields.io/github/actions/workflow/status/AnouarMohamed/Ftrace-Flight-Recorder/ci.yml?branch=main&style=flat-square&label=CI)](https://github.com/AnouarMohamed/Ftrace-Flight-Recorder/actions/workflows/ci.yml)
[![CodeQL](https://img.shields.io/github/actions/workflow/status/AnouarMohamed/Ftrace-Flight-Recorder/codeql.yml?branch=main&style=flat-square&label=CodeQL)](https://github.com/AnouarMohamed/Ftrace-Flight-Recorder/actions/workflows/codeql.yml)
[![Dependency Review](https://img.shields.io/github/actions/workflow/status/AnouarMohamed/Ftrace-Flight-Recorder/dependency-review.yml?event=pull_request&style=flat-square&label=Dependency%20Review)](https://github.com/AnouarMohamed/Ftrace-Flight-Recorder/actions/workflows/dependency-review.yml)
[![Container](https://img.shields.io/github/actions/workflow/status/AnouarMohamed/Ftrace-Flight-Recorder/publish-image.yml?event=workflow_dispatch&style=flat-square&label=Container)](https://github.com/AnouarMohamed/Ftrace-Flight-Recorder/actions/workflows/publish-image.yml)

[![OpenSSF Scorecard](https://api.securityscorecards.dev/projects/github.com/AnouarMohamed/Ftrace-Flight-Recorder/badge?style=flat-square)](https://securityscorecards.dev/viewer/?uri=github.com/AnouarMohamed/Ftrace-Flight-Recorder)
[![Release](https://img.shields.io/github/v/release/AnouarMohamed/Ftrace-Flight-Recorder?style=flat-square&label=Release)](https://github.com/AnouarMohamed/Ftrace-Flight-Recorder/releases/latest)
[![License](https://img.shields.io/github/license/AnouarMohamed/Ftrace-Flight-Recorder?style=flat-square)](LICENSE.txt)

FDR is a small Linux daemon that continuously records selected kernel ftrace
events into bounded files. It is designed to preserve evidence for failures
that are difficult to reproduce, such as scheduler stalls, network errors,
filesystem problems, and kernel subsystem failures.

FDR is not a tracing user interface or a general-purpose profiler. It is the
always-on collection layer that keeps recent kernel events available for later
inspection.

## Why use FDR?

Kernel failures often disappear before an operator can start a trace. FDR
starts the chosen tracepoints in advance, isolates each configuration in its
own tracefs instance, and continuously drains events to disk.

Use FDR when you need:

- a rolling record of specific kernel tracepoints;
- bounded capture files that will not intentionally fill a filesystem;
- explicit evidence-integrity signals when the recorder cannot keep up;
- one recorder per Linux host or Kubernetes node;
- a small daemon with no database or external control plane.

Do not use FDR as:

- a replacement for perf, eBPF profilers, or interactive tracing tools;
- a trace visualization or long-term storage system;
- an unprivileged container workload;
- proof that a capture is complete without checking readiness and loss metrics.

## How it works

1. FDR reads regular `*.conf` files from `/etc/fdr.d`.
2. Each file creates one isolated tracefs instance and one worker process.
3. The worker enables the configured tracepoints and, when `saveto` is present,
   drains `trace_pipe` into a mode-0600 file.
4. The parent supervises workers, handles safe reloads, samples tracefs loss
   counters, and exposes health, readiness, and Prometheus metrics.

FDR normally runs as root because tracefs controls the host kernel. Its
Kubernetes deployment is privileged for the same reason.

## Quick start on a Linux host

Requirements:

- Linux with tracefs mounted at `/sys/kernel/tracing`;
- a C11 compiler and GNU Make;
- `kmod` only for configurations that use `modprobe`;
- `logrotate` only for external rotation policies.

Build and test:

```sh
make check
make
```

Install a minimal scheduler capture:

```sh
sudo make install
sudo install -d -m 0700 /var/log/fdr
sudo install -m 0644 deploy/kubernetes/fdr.conf /etc/fdr.d/node.conf
sudo fdrd -n -c /etc/fdr.d
sudo systemctl enable --now fdr
```

Confirm that the service is operating and preserving events:

```sh
systemctl status --no-pager fdr
curl --fail http://127.0.0.1:9119/healthz
curl --fail http://127.0.0.1:9119/readyz
curl --silent http://127.0.0.1:9119/metrics
sudo grep -m 1 sched_switch /var/log/fdr/node.log
```

Expected endpoint bodies are `ok` and `ready`. The metrics should show
`fdr_ready 1` and zero new trace-loss counters.

If tracefs is not mounted, review the host policy before mounting it. On a
disposable or approved system the usual command is:

```sh
sudo mount -t tracefs tracefs /sys/kernel/tracing
```

## Minimal configuration

```text
instance node 16m
enable sched/sched_switch
enable sched/sched_wakeup
minfree 5
saveto /var/log/fdr/node.log 64m
```

This creates `/sys/kernel/tracing/instances/node`, allocates approximately
16 MiB of trace buffer per CPU, enables two scheduler events, and preserves a
bounded capture at `/var/log/fdr/node.log`.

Always validate syntax before activation:

```sh
fdrd -n -c /etc/fdr.d
```

See the [configuration reference](docs/configuration.md) before selecting
events, filters, buffer sizes, storage limits, or modules.

## Choose a deployment

| Environment | Recommended path | Notes |
|---|---|---|
| Linux host with systemd | `sudo make install` | HTTP binds to loopback by default |
| Kubernetes with plain manifests | [Kustomize deployment](deploy/kubernetes/README.md) | Privileged DaemonSet, one pod per selected node |
| Kubernetes with Helm | [Helm chart](deploy/helm/fdr/README.md) | Optional PodMonitor, alerts, dashboard, and NetworkPolicy |
| Local integration test | [Kind observability lab](deploy/kind/README.md) | Uses the host kernel and writable host tracefs |
| Kernel compatibility test | [Disposable VM matrix](tests/vm/README.md) | Runs controlled load only inside disposable guests |

## Health and evidence integrity

FDR exposes an unauthenticated IPv4 HTTP listener on `127.0.0.1:9119` by
default:

- `/healthz` says whether the parent event loop is alive;
- `/readyz` says whether the active recorder is trustworthy enough to use;
- `/metrics` exposes cumulative Prometheus counters and current gauges.

Readiness becomes false after probe failures, persistent collector failures,
write failures, or newly observed kernel trace loss. A successful configuration
reload or process restart returns readiness to a fresh state, but cannot recover
events that were already lost. Preserve the current capture and metrics before
reloading during an incident.

See the [operations guide](docs/operations.md) for the complete metric
reference, reload behavior, incident workflow, log rotation, and troubleshooting.

## Security boundary

FDR controls host-kernel tracing and should be treated as an administrative
component:

- restrict write access to `/etc/fdr.d` and Kubernetes ConfigMaps;
- restrict who can change the image, arguments, host paths, or enabled probes;
- do not expose the HTTP listener to an untrusted network without an
  authenticated proxy and network controls;
- review tracepoints and filters for performance and data sensitivity;
- enable host module access only when a reviewed configuration requires it.

Compromise of the privileged Kubernetes pod must be treated as compromise of
the node. Read [SECURITY.md](SECURITY.md) before production deployment.

## Documentation

Start at the [documentation index](docs/README.md), or go directly to:

- [Getting started](docs/getting-started.md)
- [Configuration reference](docs/configuration.md)
- [Operations and troubleshooting](docs/operations.md)
- [Kustomize deployment](deploy/kubernetes/README.md)
- [Helm deployment and observability](deploy/helm/fdr/README.md)
- [Validation evidence](docs/validation/README.md)
- [Changelog](CHANGELOG.md)
- [Roadmap](ROADMAP.md)
- [Contributing](CONTRIBUTING.md)

The installed manual page is also available with `man 8 fdrd`.

## Current validation status

Recorded evidence currently includes:

- a complete Kind lifecycle run on Linux 7.1.8 with Prometheus and Grafana;
- disposable KVM systemd and controlled-loss passes on Linux 7.0.12 and 7.1.8;
- Ubuntu Jammy and Noble KVM qualification, including a Noble single-node k3s
  deployment;
- fake-tracefs unit and runtime tests in ordinary CI.

The release kernel matrix is complete. Representative performance curves,
security-policy qualification, and genuinely multi-node validation remain
open; the evidence index distinguishes those limits from completed checks.

## License

Universal Permissive License 1.0. See [LICENSE.txt](LICENSE.txt).
