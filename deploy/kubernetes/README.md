## Kubernetes

See also the [Helm chart](../helm/fdr/README.md) for node selectors, values,
and upgrades.

FDR captures **host kernel** ftrace data. In Kubernetes it runs as a **DaemonSet**
(one pod per node) with privileged access to the node's tracefs mount.

## Requirements

- Linux nodes with tracefs at `/sys/kernel/tracing`
- A container runtime that allows privileged pods
- Kernel tracepoints referenced in your config must exist on the node kernel

FDR is **not** a replacement for Kubernetes audit logs, Prometheus, or eBPF
observability stacks. It complements them by recording low-level kernel events
(scheduler, network, filesystem tracepoints, etc.) on each node.

## Quick start

Build the image:

```sh
docker build -f deploy/kubernetes/Dockerfile -t fdr:latest .
```

Deploy with kustomize:

```sh
kubectl apply -k deploy/kubernetes/
```

Or without kustomize:

```sh
kubectl apply -f deploy/kubernetes/namespace.yaml
kubectl apply -f deploy/kubernetes/configmap.yaml
kubectl apply -f deploy/kubernetes/daemonset.yaml
```

Verify:

```sh
kubectl -n fdr-system get pods -o wide
kubectl -n fdr-system logs -l app=fdr
```

Trace logs land on the host at `/var/log/fdr/` (hostPath volume).

## Configuration

Edit probe settings in `configmap.yaml`, then re-apply:

```sh
kubectl apply -k deploy/kubernetes/
kubectl -n fdr-system rollout restart daemonset/fdr
```

Each key in the ConfigMap becomes a file under `/etc/fdr.d/` inside the pod.
Use the `.conf` suffix (e.g. `node.conf`).

See the main [README](../../README.md) for directive syntax (`instance`, `enable`,
`saveto`, etc.).

## Why `-f` (foreground)?

On bare metal, `fdrd` daemonizes for systemd. Containers need the main process to
stay in the foreground so Kubernetes does not treat the pod as exited. The
DaemonSet manifest passes `-f` for this.

## Security notes

- The DaemonSet runs **privileged** so it can create ftrace instances and enable
  probes on the host kernel.
- Logs are written to a **hostPath** directory shared with the node OS.
- Restrict who can edit the `fdr-system` namespace and ConfigMap.
- Review enabled probes carefully; verbose tracing adds CPU and I/O overhead.

## Alternatives inside Kubernetes

| Approach | Best for |
|----------|----------|
| **FDR DaemonSet** (this repo) | Persistent kernel tracepoints you choose, black-box style logging |
| **eBPF** (Cilium, Pixie, Inspektor Gadget) | Programmable in-cluster observability, lower overhead |
| **kubectl logs / cluster logging** | Application stdout/stderr, not kernel trace |
| **Kubernetes Audit** | API server request audit trail |

You can run FDR alongside these; they operate at different layers.

## CI image publishing

To publish to GHCR, build and push from a tagged release:

```sh
docker build -f deploy/kubernetes/Dockerfile -t ghcr.io/<owner>/fdr:<tag> .
docker push ghcr.io/<owner>/fdr:<tag>
```

Update `deploy/kubernetes/kustomization.yaml` with your registry path.
