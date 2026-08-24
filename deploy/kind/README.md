# Local Kind observability lab

This lab proves the FDR Kubernetes lifecycle against the local Linux kernel and
verifies that Prometheus and Grafana can observe it. It creates a dedicated
Kind cluster with one control-plane node and one worker, builds and loads the
local FDR image, installs a pinned monitoring stack, deploys one FDR pod, and
runs automated checks.

## Safety boundary

Kind nodes are containers and share the host kernel. This lab passes the host's
writable <code>/sys/kernel/tracing</code> mount into the worker. FDR therefore
changes real host tracefs state even though the Kubernetes cluster is
disposable.

The automation limits the lab to one worker and the isolated
<code>fdr-lab</code> trace instance. Do not change the lab to enable all events,
run high-volume benchmarks, or create multiple FDR pods on a workstation. Use a
disposable virtual machine for those tests.

The script refuses to start or delete the lab until the operator explicitly
acknowledges this boundary. It records ownership when it creates the cluster
and will not delete a same-named pre-existing cluster without the separate
<code>FDR_LAB_DELETE_EXISTING=yes</code> authorization.

## Pinned components

- Kind: use the locally installed version; validated with 0.31.0
- kube-prometheus-stack Helm chart: 88.5.4
- FDR image: locally built as <code>fdr-lab:dev</code>
- FDR chart: the working tree under <code>deploy/helm/fdr</code>

The monitoring profile enables only Prometheus, Prometheus Operator, and
Grafana. Cluster-wide default dashboards, default rules, Alertmanager, node
exporter, kube-state-metrics, and the disposable cluster's admission webhook
are disabled to keep the lab focused. Prometheus still evaluates every FDR
rule; production alert delivery should use the operator's existing
Alertmanager integration.

## Prerequisites

- Linux with tracefs mounted read-write at <code>/sys/kernel/tracing</code>
- Docker Engine
- Kind, Helm, kubectl, curl, jq, and findmnt
- At least 4 CPU cores, 8 GiB available memory, and 15 GiB free disk space
- Network access to the Kind node image, Alpine packages, and the official
  Prometheus Community Helm repository

The script reuses an existing correctly configured Prometheus Community
repository index. It does not force a multi-megabyte index refresh for the
pinned chart on every run.

The first run pulls the exact images used by the pinned monitoring profile
through the host Docker daemon, retries transient failures, and loads them into
Kind before Helm starts. This avoids relying on registry DNS from inside a Kind
node. The loader imports only the cluster's actual CPU architecture, avoiding a
Docker 29 multi-platform manifest import failure. It also caches the pinned chart archive under
<code>.build/fdr-lab-artifacts/cache</code>. The default Helm readiness timeout is
10 minutes; override it with <code>FDR_LAB_MONITORING_TIMEOUT=20m</code> if the
workloads need longer to start. A failed run is resumable by running
<code>up</code> again against the script-owned cluster.

Each host-side image pull attempt is bounded to 15 minutes and retried three
times. Set <code>FDR_LAB_IMAGE_PULL_TIMEOUT=30m</code> for a slow but reliable
connection.

Verify the host before starting:

~~~sh
findmnt -T /sys/kernel/tracing
docker info
kind version
helm version
~~~

The tracefs root is normally accessible only to root, so the invoking user does
not need to pass a host-side writable-directory test. The Helm chart's
<code>tracefs-preflight</code> init container verifies write access from the
privileged workload before FDR starts.

## Run the lab

Read the safety boundary above, then run:

~~~sh
export FDR_LAB_ACKNOWLEDGE_HOST_KERNEL=yes
deploy/kind/lab.sh up
~~~

The command is intentionally end-to-end. It:

1. Builds <code>fdr-lab:dev</code> from the current working tree.
2. Creates the <code>fdr-lab</code> Kind cluster if it does not exist.
3. Loads the local image into the cluster's containerd image store.
4. Caches and loads the pinned monitoring images into the cluster.
5. Installs kube-prometheus-stack 88.5.4 from the cached chart in <code>monitoring</code>.
6. Installs FDR in <code>fdr-lab</code> with PodMonitor, PrometheusRule, dashboard,
   and NetworkPolicy resources enabled.
7. Checks probes and every FDR metric family.
8. Confirms that real scheduler events reach the capture file.
9. Queries Prometheus for <code>fdr_ready</code> and the <code>fdr.rules</code>
   group.
10. Queries Grafana for the provisioned dashboard UID.

A successful run ends with:

~~~text
LAB PASS: FDR captured real host-kernel events and is visible in Prometheus and Grafana
~~~

If a run fails, the cluster is retained. Run
<code>deploy/kind/lab.sh collect</code> before troubleshooting or teardown.

## Inspect the lab

Cluster state:

~~~sh
kubectl --context kind-fdr-lab get pods -A
kubectl --context kind-fdr-lab -n fdr-lab logs daemonset/fdr-lab -c fdrd
kubectl --context kind-fdr-lab -n fdr-lab exec daemonset/fdr-lab -c fdrd -- \
  curl --silent http://127.0.0.1:9119/metrics
~~~

Grafana:

~~~sh
deploy/kind/lab.sh grafana
~~~

Open <http://127.0.0.1:13000>, sign in with <code>admin</code> /
<code>fdr-lab</code>, and open **FDR / Kernel Flight Recorder**.

The dashboard leads with evidence integrity rather than throughput. A recorder
can be running while its capture is incomplete, so trace loss, storage drops,
and write failures remain separately visible.

## Repeat checks and collect evidence

~~~sh
deploy/kind/lab.sh verify
deploy/kind/lab.sh collect
~~~

Collection writes a timestamped directory under
<code>.build/fdr-lab-artifacts</code> containing cluster state, Kubernetes
events, FDR logs, preflight logs, rendered observability resources, current
metrics, a capture sample, and Kind component logs. These are the results to
bring back when diagnosing a failed lab.

## Tear down safely

~~~sh
export FDR_LAB_ACKNOWLEDGE_HOST_KERNEL=yes
deploy/kind/lab.sh down
~~~

Teardown uninstalls FDR first and waits for
<code>/sys/kernel/tracing/instances/fdr-lab</code> to disappear. Only then does
it delete the Kind cluster named <code>fdr-lab</code>. If tracefs cleanup fails,
the script stops and retains the cluster for inspection.

## What this lab does not prove

- Production performance or safe CPU limits
- Multiple independent kernels or genuine per-node tracefs isolation
- Kernel-version compatibility
- SELinux, AppArmor, or distribution-specific behavior
- NetworkPolicy enforcement by Kind's default network plugin
- High-volume, disk-pressure, or destructive failure behavior

Run those tests in disposable Linux virtual machines. A Kind pass is the local
integration gate, not the final production qualification.
