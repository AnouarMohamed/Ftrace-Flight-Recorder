# Validation evidence

This directory records reproducible evidence for FDR behavior. A passing report
supports only the source revision, kernel, and environment named in that
report; it is not a universal compatibility claim.

## Validation layers

| Layer | Purpose | Kernel boundary |
|---|---|---|
| `make check` | Parser, lifecycle, HTTP, metrics, reload, rotation, and failure regressions using fake tracefs fixtures | Does not exercise a real kernel tracefs |
| Kind observability lab | DaemonSet lifecycle, real capture, rollout, degraded readiness, recovery, rotation, Prometheus, alerts, and Grafana | Kind shares the host kernel |
| Local-kernel KVM matrix | systemd lifecycle, real capture, and controlled trace loss on installed kernels | Disposable guest, independent boot |
| Ubuntu cloud-image VM matrix | Distribution qualification and optional single-node k3s deployment | Disposable KVM guest |

Run the cheapest layer that can answer the question, then use a real disposable
VM before making kernel-compatibility or performance claims.

## Recorded reference runs

| Date | Environment | Revision | Result and scope |
|---|---|---|---|
| 2026-08-29 | Kind, Linux 7.1.8 | Base `9eecc9f` plus the report's smoke-workflow changes | [Full lifecycle report](2026-08-29-kind-kernel-7.1.8/report.md): real scheduler capture, Prometheus target, healthy/degraded Grafana states, configuration rollout, collector restart, rotation, and cleanup passed |
| 2026-08-29 | Disposable KVM, Linux 7.0.12 and 7.1.8 | `97f77d7` | [Matrix report](2026-08-29-vm-kernels-7.0.12-7.1.8/matrix-report.md): systemd lifecycle, zero-loss normal load, and controlled trace-loss degradation passed on both kernels |

The reports include environment details, metrics, logs, capture samples, and
small screenshots or summaries where they improve reviewability. Large raw
captures are intentionally not committed.

## What has been demonstrated

- configured scheduler tracepoints produce real kernel records;
- the normal tested load completed without reported trace loss;
- controlled overload produced a measurable kernel overrun and unready state;
- a bad runtime probe kept liveness healthy while readiness degraded;
- Kubernetes configuration changes rolled the pod through the ConfigMap hash;
- a persistent collector failure caused container recovery;
- bounded rotation preserved non-empty mode-0600 capture files;
- Prometheus discovered FDR and Grafana displayed the same integrity signals;
- graceful teardown removed the owned tracefs instance.

## What remains open

- release-qualification runs on the selected oldest and current Ubuntu LTS
  kernels;
- the Noble single-node k3s profile;
- representative performance curves by event set, CPU count, and event rate;
- production-style disk-pressure, SELinux, and AppArmor qualification;
- multi-node behavior with genuinely separate node kernels;
- NetworkPolicy enforcement across supported Kubernetes network plugins.

Track completion criteria and ownership in the [project roadmap](../../ROADMAP.md).

## Reproduce the checks

### Fast repository checks

```sh
make check
make sanitize
helm lint --strict deploy/helm/fdr
kubectl kustomize deploy/kubernetes >/dev/null
```

These checks are safe for ordinary development because the test suite uses a
fake tracefs tree. They do not prove real-kernel compatibility.

### Kind integration and observability

Read the [Kind safety boundary](../../deploy/kind/README.md#safety-boundary)
before running:

```sh
export FDR_LAB_ACKNOWLEDGE_HOST_KERNEL=yes
deploy/kind/lab.sh run
```

Kind shares and modifies the host tracefs. The workflow is bounded, but it is
not kernel-isolated. Do not turn it into a stress test.

### Installed-kernel KVM matrix

```sh
tests/vm/local-kernel-matrix.sh
```

This boots installed kernels directly with a disposable guest root and keeps
controlled overload away from the host kernel.

### Ubuntu LTS and k3s matrix

```sh
tests/vm/matrix.sh
tests/vm/matrix.sh jammy
tests/vm/matrix.sh noble
```

The full matrix downloads checksum-verified Ubuntu images and requires KVM,
QEMU, Docker, Helm, network access, and substantial local resources. See the
[VM harness guide](../../tests/vm/README.md) for prerequisites and retained
artifacts.

## Recording new evidence

A durable validation report should include:

- UTC timestamp and exact Git revision;
- OS, kernel, architecture, CPU and memory allocation;
- tool, Kubernetes, chart, and image versions where applicable;
- the configuration and commands used;
- explicit pass/fail criteria rather than screenshots alone;
- before/after metrics for integrity and lifecycle tests;
- relevant logs, exit statuses, and cleanup results;
- a clear statement of what the run did not test.

Keep screenshots minimal and factual. Prefer machine-readable metrics and logs
as the primary evidence. Redact credentials, hostnames, workload data, and
other sensitive trace content before committing artifacts.
