#!/bin/sh
#
# kubernetes-smoke.sh - Lightweight Kind deployment smoke test for FDR.
#
# Licensed under the Universal Permissive License (UPL), Version 1.0.
#
# The test builds the container image, loads it into a Kind cluster, installs
# the Helm chart with a bounded scheduler capture, verifies readiness, capture,
# rotation, and graceful tracefs cleanup. It is intentionally smaller than the
# full observability lab in deploy/kind/lab.sh so it can run on GitHub-hosted
# runners.
#
# Requirements:
# - Docker, Kind, kubectl, Helm, curl, and standard POSIX utilities
# - Linux with tracefs mounted at /sys/kernel/tracing
# - An existing Kind cluster when FDR_K8S_SMOKE_REUSE_CLUSTER=yes
#
# Usage:
#   make .build/sched_load
#   tests/kubernetes-smoke.sh
#
# Environment:
#   FDR_K8S_SMOKE_CLUSTER         Kind cluster name (default fdr-ci-<pid>)
#   FDR_K8S_SMOKE_REUSE_CLUSTER   set to yes to use an existing cluster
#   FDR_K8S_SMOKE_NAMESPACE       Kubernetes namespace (default fdr-smoke)
#   FDR_K8S_SMOKE_RELEASE         Helm release name (default fdr-smoke)
#   FDR_K8S_SMOKE_IMAGE           local image tag (default fdr-k8s-smoke:dev)
#   FDR_K8S_SMOKE_ARTIFACTS       retained diagnostics directory
#
# Exit status: 0 on success, 1 on smoke failure, 2 on invalid environment.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
cluster=${FDR_K8S_SMOKE_CLUSTER:-"fdr-ci-$$"}
reuse_cluster=${FDR_K8S_SMOKE_REUSE_CLUSTER:-no}
namespace=${FDR_K8S_SMOKE_NAMESPACE:-fdr-smoke}
release=${FDR_K8S_SMOKE_RELEASE:-fdr-smoke}
image=${FDR_K8S_SMOKE_IMAGE:-fdr-k8s-smoke:dev}
image_repository=${image%:*}
image_tag=${image##*:}
artifacts=${FDR_K8S_SMOKE_ARTIFACTS:-"$repo_root/.build/kubernetes-smoke"}
instance=fdr-k8s-smoke
workdir=
owns_cluster=0
release_installed=0
worker_node=

say()
{
	printf '%s\n' "$*"
}

fail()
{
	printf 'error: %s\n' "$*" >&2
	exit 1
}

collect_diagnostics()
{
	[ -d "$artifacts" ] || mkdir -p "$artifacts"
	cp "$workdir/values.yaml" "$artifacts/values.yaml" 2>/dev/null || true
	kubectl -n "$namespace" get pods -o wide \
	    >"$artifacts/pods.txt" 2>&1 || true
	kubectl -n "$namespace" describe pods \
	    >"$artifacts/pods-describe.txt" 2>&1 || true
	kubectl -n "$namespace" logs "daemonset/$release" --all-containers \
	    >"$artifacts/fdr.log" 2>&1 || true
	kubectl -n "$namespace" logs "daemonset/$release" \
	    -c tracefs-preflight >"$artifacts/preflight.log" 2>&1 || true
	kubectl -n "$namespace" exec "daemonset/$release" -c fdrd -- \
	    curl --fail --silent http://127.0.0.1:9119/metrics \
	    >"$artifacts/metrics.txt" 2>&1 || true
}

cleanup()
{
	status=$?

	trap - EXIT
	if [ "$release_installed" -eq 1 ]; then
		helm uninstall "$release" --namespace "$namespace" --wait \
		    --timeout 120s >/dev/null 2>&1 || true
	fi
	if [ "$status" -ne 0 ] && [ -n "$workdir" ] && [ -d "$workdir" ]; then
		collect_diagnostics || true
	fi
	if [ "$owns_cluster" -eq 1 ]; then
		kind delete cluster --name "$cluster" >/dev/null 2>&1 || true
	fi
	if [ -n "$workdir" ] && [ -d "$workdir" ]; then
		if [ "$status" -eq 0 ]; then
			mkdir -p "$artifacts"
			cp "$workdir/values.yaml" "$artifacts/values.yaml" || true
			cp "$workdir/metrics.txt" "$artifacts/metrics.txt" || true
		fi
		rm -rf "$workdir"
	fi
	exit "$status"
}

trap cleanup EXIT
trap 'exit 130' HUP INT TERM

metric()
{
	kubectl -n "$namespace" exec "daemonset/$release" -c fdrd -- \
	    curl --fail --silent --show-error \
	    http://127.0.0.1:9119/metrics |
	    awk -v name="$1" '$1 == name { print $2; exit }'
}

wait_for_ready()
{
	attempt=0

	while [ "$attempt" -lt 300 ]; do
		if kubectl -n "$namespace" exec "daemonset/$release" -c fdrd -- \
		    curl --fail --silent --show-error \
		    http://127.0.0.1:9119/readyz >/dev/null 2>&1; then
			return 0
		fi
		attempt=$((attempt + 1))
		sleep 0.2
	done
	return 1
}

wait_for_metric()
{
	name=$1
	minimum=$2
	attempt=0

	while [ "$attempt" -lt 300 ]; do
		value=$(metric "$name" 2>/dev/null || true)
		if [ -n "$value" ] && [ "$value" -ge "$minimum" ] 2>/dev/null; then
			return 0
		fi
		attempt=$((attempt + 1))
		sleep 0.2
	done
	return 1
}

wait_for_capture()
{
	attempt=0

	while [ "$attempt" -lt 300 ]; do
		if kubectl -n "$namespace" exec "daemonset/$release" -c fdrd -- \
		    sh -c 'test -s /var/log/fdr/smoke.log &&
		    grep -q sched_switch /var/log/fdr/smoke.log' \
		    >/dev/null 2>&1; then
			return 0
		fi
		attempt=$((attempt + 1))
		sleep 0.2
	done
	return 1
}

for tool in docker kind kubectl helm curl; do
	command -v "$tool" >/dev/null 2>&1 ||
	    fail "required command not found: $tool"
done
if [ ! -d /sys/kernel/tracing/instances ] && \
    ! sudo -n test -d /sys/kernel/tracing/instances 2>/dev/null; then
	fail "tracefs is not mounted at /sys/kernel/tracing"
fi
[ -x "$repo_root/.build/sched_load" ] ||
    fail ".build/sched_load is not built; run 'make .build/sched_load'"
[ "$reuse_cluster" = yes ] || [ "$reuse_cluster" = no ] ||
    fail "FDR_K8S_SMOKE_REUSE_CLUSTER must be yes or no"

if kind get clusters 2>/dev/null | grep -Fxq "$cluster"; then
	[ "$reuse_cluster" = yes ] ||
	    fail "Kind cluster $cluster already exists; set FDR_K8S_SMOKE_REUSE_CLUSTER=yes to reuse it"
else
	[ "$reuse_cluster" = no ] ||
	    fail "Kind cluster $cluster does not exist"
	say "Creating Kind cluster $cluster"
	kind create cluster --name "$cluster" \
	    --config "$repo_root/deploy/kind/kind-config.yaml" --wait 180s
	owns_cluster=1
fi

workdir=$(mktemp -d /tmp/fdr-k8s-smoke.XXXXXX)
cat >"$workdir/values.yaml" <<EOF
fullnameOverride: $release
image:
  repository: $image_repository
  tag: "$image_tag"
  pullPolicy: Never
config:
  node.conf: null
  smoke.conf: |
    instance $instance 64k
    enable sched/sched_switch
    enable sched/sched_wakeup
    minfree 1
    saveto /var/log/fdr/smoke.log 256k
EOF

say "Building and loading $image"
docker build --tag "$image" --file "$repo_root/deploy/kubernetes/Dockerfile" \
    "$repo_root" >/dev/null
kind load docker-image "$image" --name "$cluster"

say "Installing Helm release $release"
release_installed=1
helm upgrade --install "$release" "$repo_root/deploy/helm/fdr" \
    --namespace "$namespace" --create-namespace \
    --values "$workdir/values.yaml" --wait --timeout 5m

kubectl -n "$namespace" rollout status "daemonset/$release" \
    --timeout=180s
kubectl -n "$namespace" wait --for=condition=Ready pod \
    -l "app.kubernetes.io/instance=$release" --timeout=180s
wait_for_ready || fail "FDR readiness endpoint did not become ready"
wait_for_metric fdr_instances 1 ||
    fail "FDR did not report one configured instance"
wait_for_metric fdr_workers_alive 1 ||
    fail "FDR did not report a running collector"

say "Generating scheduler load"
"$repo_root/.build/sched_load" 3 2 50 16 >/dev/null 2>&1 ||
    fail "scheduler load generator failed"
wait_for_capture || fail "no scheduler events reached the capture file"
wait_for_metric fdr_rotations_total 1 ||
    fail "bounded-size rotation did not occur"
kubectl -n "$namespace" exec "daemonset/$release" -c fdrd -- \
    sh -c 'test -f /var/log/fdr/smoke.log.1' ||
    fail "rotation did not create /var/log/fdr/smoke.log.1"

write_errors=$(metric fdr_write_errors_total)
rotation_failures=$(metric fdr_rotation_failures_total)
ready=$(metric fdr_ready)
[ "$write_errors" -eq 0 ] || fail "write errors observed: $write_errors"
[ "$rotation_failures" -eq 0 ] ||
    fail "rotation failures observed: $rotation_failures"
[ "$ready" -eq 1 ] || fail "readiness degraded during the smoke test"

kubectl -n "$namespace" exec "daemonset/$release" -c fdrd -- \
    curl --fail --silent http://127.0.0.1:9119/metrics \
    >"$workdir/metrics.txt"

say "Uninstalling release and waiting for tracefs cleanup"
helm uninstall "$release" --namespace "$namespace" --wait --timeout 120s
release_installed=0

worker_node=$(kind get nodes --name "$cluster" | grep worker | head -n 1)
[ -n "$worker_node" ] || fail "Kind cluster has no worker node"
for attempt in $(seq 1 200); do
	if docker exec "$worker_node" \
	    test ! -d "/sys/kernel/tracing/instances/$instance"; then
		break
	fi
	sleep 0.2
done
docker exec "$worker_node" \
    test ! -d "/sys/kernel/tracing/instances/$instance" ||
    fail "FDR did not remove trace instance $instance"

say "Kubernetes smoke test passed"
