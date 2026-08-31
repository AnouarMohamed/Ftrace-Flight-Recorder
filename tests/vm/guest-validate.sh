#!/usr/bin/env bash
set -Eeuo pipefail

source_tar=$1
result_dir=$2
run_k3s=${RUN_K3S:-0}
k3s_version=${K3S_VERSION:-v1.35.5+k3s1}
k3s_install_sha256=${K3S_INSTALL_SHA256:-8598e002e61d658fed7b7542fc6d2c66d8da6eae69e088830105d2ee1ffb6d91}
k3s_installer=
source_dir=/var/tmp/fdr-source
config=/etc/fdr.d/vm.conf

mkdir -p "$result_dir"
exec > >(tee "$result_dir/run.log") 2>&1

finish()
{
	rc=$?
	trap - EXIT
	[ -z "$k3s_installer" ] || rm -f "$k3s_installer"
	if [ "$rc" -ne 0 ]; then
		printf 'FAILED\n' >"$result_dir/status.txt"
		journalctl -u fdr --no-pager >"$result_dir/fdr-journal.txt" 2>&1 || true
	fi
	exit "$rc"
}
trap finish EXIT

metric()
{
	name=$1
	curl --fail --silent http://127.0.0.1:9119/metrics | \
	    awk -v metric_name="$name" '$1 == metric_name { print $2; exit }'
}

wait_ready()
{
	for _ in {1..90}; do
		curl --fail --silent http://127.0.0.1:9119/readyz >/dev/null 2>&1 && return
		sleep 1
	done
	return 1
}

save_healthy_metrics()
{
	destination=$1
	curl --fail --silent http://127.0.0.1:9119/metrics >"$destination"
	grep -q '^fdr_ready 1$' "$destination"
	grep -q '^fdr_bytes_dropped_total 0$' "$destination"
	grep -q '^fdr_rotation_failures_total 0$' "$destination"
	grep -q '^fdr_probe_failures_total 0$' "$destination"
	grep -q '^fdr_write_errors_total 0$' "$destination"
	grep -q '^fdr_trace_overruns_total 0$' "$destination"
	grep -q '^fdr_trace_dropped_events_total 0$' "$destination"
	grep -q '^fdr_trace_commit_overruns_total 0$' "$destination"
}

install_config()
{
	buffer=$1
	max_file=$2
	shift 2
	temporary=$(mktemp)
	{
		printf 'instance vm-smoke %s\n' "$buffer"
		for event_name in "$@"; do
			printf 'enable %s\n' "$event_name"
		done
		printf 'minfree 5\n'
		printf 'saveto /var/log/fdr/vm.log %s\n' "$max_file"
	} >"$temporary"
	install -m 0644 "$temporary" "$config"
	rm -f "$temporary"
}

printf 'Waiting for package manager availability\n'
while fuser /var/lib/dpkg/lock-frontend >/dev/null 2>&1; do sleep 2; done
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y build-essential curl jq logrotate stress-ng sysstat

rm -rf "$source_dir"
mkdir -p "$source_dir"
tar -C "$source_dir" -xf "$source_tar"
cd "$source_dir"
make check
make
make install

mkdir -p /etc/fdr.d /var/log/fdr
chmod 0700 /var/log/fdr
if ! mountpoint -q /sys/kernel/tracing; then
	mount -t tracefs tracefs /sys/kernel/tracing
fi
findmnt -T /sys/kernel/tracing >"$result_dir/tracefs.txt"

uname -a >"$result_dir/uname.txt"
cat /etc/os-release >"$result_dir/os-release.txt"
{
	printf 'cgroup='
	findmnt -n -o FSTYPE -T /sys/fs/cgroup
	printf 'rootfs='
	findmnt -n -o FSTYPE -T /
	printf 'cpus='
	getconf _NPROCESSORS_ONLN
	systemd --version | head -n 1
} >"$result_dir/environment.txt"

printf 'Running systemd smoke test\n'
install_config 32m 4m sched/sched_wakeup
systemctl daemon-reload
systemctl enable --now fdr
wait_ready
curl --fail --silent http://127.0.0.1:9119/healthz | grep -Fxq ok
stress-ng --cpu 2 --switch 2 --fork 2 --timeout 10s --metrics-brief \
    >"$result_dir/normal-load.txt" 2>&1
for _ in {1..30}; do
	[ -s /var/log/fdr/vm.log ] && \
	    grep -q 'sched_wakeup' /var/log/fdr/vm.log && break
	sleep 1
done
grep -q 'sched_wakeup' /var/log/fdr/vm.log
save_healthy_metrics "$result_dir/normal-metrics.txt"

reload_before=$(metric fdr_reloads_total)
printf '# valid reload %s\n' "$(date -u +%s)" >>"$config"
systemctl reload fdr
for _ in {1..30}; do
	reload_after=$(metric fdr_reloads_total 2>/dev/null || printf 0)
	[ "$reload_after" -gt "$reload_before" ] && break
	sleep 1
done
[ "$reload_after" -gt "$reload_before" ]
cp "$config" /var/tmp/fdr-valid.conf
printf 'enable sched/sched_wakeup\n' >"$config"
invalid_before=$(metric fdr_reloads_total)
systemctl reload fdr
sleep 2
wait_ready
[ "$(metric fdr_reloads_total)" -eq "$invalid_before" ]
journalctl -u fdr --since '-1 minute' --no-pager | grep -q 'reload rejected'
install -m 0644 /var/tmp/fdr-valid.conf "$config"
systemctl reload fdr
wait_ready
save_healthy_metrics "$result_dir/reload-metrics.txt"

parent_before=$(systemctl show fdr -p MainPID --value)
restart_before=$(systemctl show fdr -p NRestarts --value)
worker=$(pgrep -P "$parent_before" fdrd | head -n 1)
[ -n "$worker" ]
kill -TERM "$worker"
for _ in {1..90}; do
	parent_after=$(systemctl show fdr -p MainPID --value)
	restart_after=$(systemctl show fdr -p NRestarts --value)
	if [ "$parent_after" -gt 0 ] && [ "$parent_after" != "$parent_before" ] && \
	    [ "$restart_after" -gt "$restart_before" ]; then
		wait_ready && break
	fi
	sleep 1
done
[ "$restart_after" -gt "$restart_before" ]

rotations_before=$(metric fdr_rotations_total)
for _ in {1..30}; do
	stress-ng --switch 4 --fork 4 --timeout 3s >/dev/null 2>&1
	rotations_after=$(metric fdr_rotations_total)
	[ "$rotations_after" -gt "$rotations_before" ] && break
done
[ "$rotations_after" -gt "$rotations_before" ]
for capture in /var/log/fdr/vm.log /var/log/fdr/vm.log.1; do
	[ -f "$capture" ] && [ ! -L "$capture" ] && [ -s "$capture" ]
	[ "$(stat -c %a "$capture")" = 600 ]
done

save_healthy_metrics "$result_dir/systemd-metrics.txt"
stat -c '%F %a %s %n' /var/log/fdr/vm.log /var/log/fdr/vm.log.1 \
    >"$result_dir/captures.txt"
journalctl -u fdr --no-pager >"$result_dir/fdr-journal.txt"
systemctl stop fdr
for _ in {1..30}; do
	[ ! -d /sys/kernel/tracing/instances/vm-smoke ] && break
	sleep 1
done
[ ! -d /sys/kernel/tracing/instances/vm-smoke ]

printf 'Running controlled trace-loss test inside the VM\n'
mkdir -p /etc/systemd/system/fdr.service.d
cat >/etc/systemd/system/fdr.service.d/benchmark.conf <<'EOF'
[Service]
CPUQuota=1%
EOF
install_config 64k 256m sched/sched_switch sched/sched_wakeup
rm -f /var/log/fdr/vm.log /var/log/fdr/vm.log.1
systemctl daemon-reload
systemctl start fdr
wait_ready
loss_start=$(date +%s)
stress-ng --cpu 4 --switch 8 --fork 8 --timeout 30s --metrics-brief \
    >"$result_dir/controlled-load.txt" 2>&1 &
load_pid=$!
degraded_after=0
while kill -0 "$load_pid" 2>/dev/null; do
	if ! curl --fail --silent http://127.0.0.1:9119/readyz >/dev/null 2>&1; then
		degraded_after=$(($(date +%s) - loss_start))
		break
	fi
	sleep 1
done
wait "$load_pid"
for _ in {1..20}; do
	loss=$(metric fdr_trace_overruns_total 2>/dev/null || printf 0)
	[ "$loss" -gt 0 ] && break
	sleep 1
done
[ "$loss" -gt 0 ]
[ "$(metric fdr_ready)" -eq 0 ]
curl --fail --silent http://127.0.0.1:9119/metrics >"$result_dir/controlled-loss-metrics.txt"
printf 'degraded_after_seconds=%s\n' "$degraded_after" \
    >"$result_dir/controlled-loss-summary.txt"
systemctl stop fdr
rm -f /etc/systemd/system/fdr.service.d/benchmark.conf
systemctl daemon-reload
for _ in {1..30}; do
	[ ! -d /sys/kernel/tracing/instances/vm-smoke ] && break
	sleep 1
done
[ ! -d /sys/kernel/tracing/instances/vm-smoke ]

k3s_status=SKIPPED
if [ "$run_k3s" -eq 1 ]; then
	printf 'Running single-node k3s smoke test\n'
	k3s_installer=$(mktemp)
	curl --fail --silent --show-error --location \
	    --proto '=https' --tlsv1.2 \
	    "https://raw.githubusercontent.com/k3s-io/k3s/${k3s_version}/install.sh" \
	    --output "$k3s_installer"
	printf '%s  %s\n' "$k3s_install_sha256" "$k3s_installer" | \
	    sha256sum --check --strict -
	INSTALL_K3S_VERSION="$k3s_version" \
	INSTALL_K3S_EXEC='server --disable=traefik --disable=servicelb' \
	    sh "$k3s_installer"
	rm -f "$k3s_installer"
	k3s_installer=
	for _ in {1..120}; do
		k3s kubectl get node 2>/dev/null | grep -q ' Ready ' && break
		sleep 2
	done
	k3s kubectl get node -o wide >"$result_dir/k3s-node.txt"
	k3s ctr images import /tmp/fdr-image.tar
	install -m 0755 /tmp/helm /usr/local/bin/helm
	KUBECONFIG=/etc/rancher/k3s/k3s.yaml /usr/local/bin/helm upgrade --install \
	    fdr-vm "$source_dir/deploy/helm/fdr" --namespace fdr-vm \
	    --create-namespace --values "$source_dir/tests/vm/k3s-values.yaml" \
	    --wait --timeout 5m
	k3s kubectl -n fdr-vm rollout status daemonset/fdr-vm --timeout=180s
	k3s kubectl -n fdr-vm exec daemonset/fdr-vm -c fdrd -- \
	    curl --fail --silent http://127.0.0.1:9119/readyz | grep -Fxq ready
	k3s kubectl -n fdr-vm exec daemonset/fdr-vm -c fdrd -- sh -ec '
	  for _ in $(seq 1 30); do
	    [ -s /var/log/fdr/k3s.log ] && grep -q sched_wakeup /var/log/fdr/k3s.log && exit 0
	    sleep 1
	  done
	  exit 1
	'
	k3s kubectl -n fdr-vm get all -o wide >"$result_dir/k3s-resources.txt"
	k3s kubectl -n fdr-vm logs daemonset/fdr-vm -c fdrd >"$result_dir/k3s-fdr.log"
	KUBECONFIG=/etc/rancher/k3s/k3s.yaml /usr/local/bin/helm uninstall \
	    fdr-vm --namespace fdr-vm --wait
	for _ in {1..30}; do
		[ ! -d /sys/kernel/tracing/instances/fdr-k3s ] && break
		sleep 1
	done
	[ ! -d /sys/kernel/tracing/instances/fdr-k3s ]
	k3s_status=PASS
fi

kernel=$(uname -r)
cat >"$result_dir/summary.env" <<EOF
KERNEL='$kernel'
SYSTEMD_SMOKE='PASS'
CONTROLLED_LOSS='PASS'
K3S_SMOKE='$k3s_status'
EOF
printf 'PASSED\n' >"$result_dir/status.txt"
trap - EXIT
printf 'Guest validation passed on %s\n' "$kernel"
