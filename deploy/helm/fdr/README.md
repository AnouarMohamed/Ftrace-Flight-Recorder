# FDR Helm chart

This chart installs FDR as a privileged per-node DaemonSet.

~~~sh
helm upgrade --install fdr deploy/helm/fdr \
  --namespace fdr-system --create-namespace
~~~

The chart uses the normal Helm release namespace; it does not create or manage a
Namespace resource itself.

The defaults schedule only on Linux nodes and do not tolerate control-plane or
other taints. The image is pinned to the chart's tested application release.
Override these settings deliberately for specialized node pools.

## Configuration

Override probe files through values:

~~~yaml
config:
  node.conf: |
    instance node 16m
    enable sched/sched_switch
    minfree 5
    saveto /var/log/fdr/node.log 64m
~~~

~~~sh
helm upgrade --install fdr deploy/helm/fdr \
  --namespace fdr-system \
  --set-file config.node.conf=./my-probes.conf
~~~

The pod template contains a checksum of the ConfigMap, so configuration changes
automatically roll the DaemonSet.

## Common values

~~~yaml
image:
  repository: ghcr.io/anouarmohamed/fdr-k8s
  tag: v1.4.0

http:
  enabled: true
  address: 0.0.0.0
  port: 9119

nodeSelector:
  kubernetes.io/os: linux

tolerations: []

logs:
  hostPath: /var/log/fdr

modules:
  enabled: false
  hostPath: /lib/modules
~~~

When HTTP is enabled, the chart adds startup, liveness, and readiness probes.
The endpoint is not exposed through a Service.

The chart intentionally has no default CPU limit. A throttled trace collector
can lose events while remaining alive; set a limit only after measuring the
selected events on representative nodes. The default memory limit remains
configurable through <code>resources</code>.

## Tracefs preflight

Before the daemon starts, an init container verifies that the configured host
path is tracefs and that its <code>instances</code> directory exists and is
writable. Inspect a failed preflight with:

~~~sh
kubectl -n fdr-system logs POD_NAME -c tracefs-preflight
stat -f -c '%T' /sys/kernel/tracing
mount | grep tracefs
~~~

Set <code>tracefs.hostPath</code> to
<code>/sys/kernel/debug/tracing</code> on hosts that intentionally expose
tracefs there. Disabling <code>preflight.enabled</code> removes the diagnostic,
not the daemon's requirement for a working tracefs mount.

## Tainted node pools

Add only the toleration required by the intended pool. For example:

~~~yaml
nodeSelector:
  kubernetes.io/os: linux
  observability: enabled

tolerations:
  - key: observability
    operator: Equal
    value: enabled
    effect: NoSchedule
~~~

## Optional module loading

Host <code>/lib/modules</code> is not mounted by default. Enable it only when a
configuration contains a <code>modprobe</code> directive:

~~~yaml
modules:
  enabled: true
  hostPath: /lib/modules
~~~

This does not remove the privileged security boundary; it only avoids exposing
host modules to deployments that do not use them.

## Prometheus, alerts, Grafana, and network policy

The PodMonitor is optional because its resource type is supplied by Prometheus
Operator rather than Kubernetes itself. Enable it only after installing those
CRDs:

~~~yaml
monitoring:
  podMonitor:
    enabled: true
    namespace: monitoring
    interval: 30s
    scrapeTimeout: 10s
    additionalLabels:
      release: kube-prometheus-stack
  prometheusRule:
    enabled: true
    namespace: monitoring
    additionalLabels:
      release: kube-prometheus-stack
  grafanaDashboard:
    enabled: true
    namespace: monitoring
    labels:
      grafana_dashboard: "1"
~~~

The PodMonitor selects FDR pods in the release namespace directly, so it does
not require a Service. The PrometheusRule adds alerts for readiness, missing
workers, trace loss, write errors, storage-protection drops, and probe failures.
The dashboard ConfigMap is discovered by Grafana's standard dashboard sidecar
and uses the Prometheus datasource UID <code>prometheus</code>.

All three resources are disabled by default, so the chart installs without
Prometheus Operator or Grafana. Their target namespaces must already exist when
they differ from the FDR release namespace.

Restrict HTTP ingress to monitoring pods with:

~~~yaml
networkPolicy:
  enabled: true
  ingress:
    namespaceSelector:
      matchLabels:
        kubernetes.io/metadata.name: monitoring
    podSelector:
      matchLabels:
        app.kubernetes.io/name: prometheus
~~~

An empty <code>podSelector</code> permits all pods in the selected namespace.
NetworkPolicy enforcement requires a compatible cluster network plugin.
Verify kubelet health probes after enabling the policy because node-originated
probe handling differs across network plugins.

For a complete pinned example, including Prometheus and Grafana installation,
see the [local Kind lab](../../kind/README.md).

## Uninstall

~~~sh
helm uninstall fdr --namespace fdr-system
~~~

Trace instances are removed during graceful shutdown. Host logs under
<code>/var/log/fdr</code> remain intentionally.
