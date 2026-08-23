# Kubernetes deployment

FDR captures host-kernel ftrace events. The manifests deploy a privileged
DaemonSet, one pod per node, with host tracefs mounted read/write.

## Requirements

- Linux nodes with tracefs mounted at <code>/sys/kernel/tracing</code>
- Permission to run privileged pods
- The configured tracepoints on every selected node kernel
- Access to <code>ghcr.io/anouarmohamed/fdr-k8s</code>, or an image override

## Deploy

~~~sh
kubectl apply -k deploy/kubernetes
kubectl -n fdr-system rollout status daemonset/fdr
kubectl -n fdr-system get pods -o wide
~~~

Logs are stored on each host under <code>/var/log/fdr</code>. Host
<code>/lib/modules</code> is mounted read-only for optional
<code>modprobe</code> directives. The container root filesystem is read-only;
only tracefs, host logs, temporary storage, and logrotate state are writable.
Kubernetes API credentials are not mounted.

## Configure

Edit <code>fdr.conf</code>, then re-apply:

~~~sh
kubectl apply -k deploy/kubernetes
~~~

Kustomize generates a content-hashed ConfigMap name, so a configuration change
updates the pod template and rolls the DaemonSet automatically. Readiness stays
false if a configured probe cannot be applied. A persistent collector failure
terminates the container so Kubernetes restarts it.

To use a different image:

~~~sh
cd deploy/kubernetes
kustomize edit set image fdr=registry.example.com/observability/fdr:v1.4.0
kubectl apply -k .
~~~

## Observe

~~~sh
kubectl -n fdr-system logs -l app=fdr
kubectl -n fdr-system port-forward daemonset/fdr 9119:9119
curl http://127.0.0.1:9119/metrics
~~~

The manifests expose HTTP only as a container port; they do not create a
cluster-wide Service.

## Security

The pod must be privileged to create trace instances and enable host probes.
Anyone able to change its image, arguments, or ConfigMap effectively controls a
root process with host-kernel access. Restrict RBAC on the namespace and review
enabled probes for performance and data sensitivity.

The Kubernetes hardening plan—including immutable images, safer scheduling,
tracefs preflight, metrics discovery, network policy, and real-cluster smoke
tests—is tracked in [the project roadmap](../../ROADMAP.md#milestone-1-kubernetes-hardening).
