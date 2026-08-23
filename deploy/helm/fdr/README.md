# FDR Helm chart

This chart installs FDR as a privileged per-node DaemonSet.

~~~sh
helm upgrade --install fdr deploy/helm/fdr \
  --namespace fdr-system --create-namespace
~~~

The chart uses the normal Helm release namespace; it does not create or manage a
Namespace resource itself.

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
  tag: latest

http:
  enabled: true
  address: 0.0.0.0
  port: 9119

nodeSelector:
  kubernetes.io/os: linux

logs:
  hostPath: /var/log/fdr

modules:
  hostPath: /lib/modules
~~~

When HTTP is enabled, the chart adds startup, liveness, and readiness probes.
The endpoint is not exposed through a Service.

## Uninstall

~~~sh
helm uninstall fdr --namespace fdr-system
~~~

Trace instances are removed during graceful shutdown. Host logs under
<code>/var/log/fdr</code> remain intentionally.
