#!/usr/bin/env bash
set -Eeuo pipefail

repo_root=$1
result_dir=$2
config=/etc/fdr.d/vm.conf

mkdir -p "$result_dir"
exec > >(tee "$result_dir/run.log") 2>&1

finish()
{
	rc=$?
	trap - EXIT
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

run_load()
{
	duration=$1
	workers=$2
	end=$((SECONDS + duration))
	pids=()
	for _ in $(seq 1 "$workers"); do
		(
			while [ "$SECONDS" -lt "$end" ]; do
				for _ in {1..2000}; do :; done
			done
		) &
		pids+=("$!")
	done
	for pid in "${pids[@]}"; do
		wait "$pid"
	done
}

run_syscall_load()
{
	duration=$1
	workers=$2
	end=$((SECONDS + duration))
	pids=()
	for _ in $(seq 1 "$workers"); do
		(
			while [ "$SECONDS" -lt "$end" ]; do
				/bin/true
			done
		) &
		pids+=("$!")
	done
	for pid in "${pids[@]}"; do
		wait "$pid"
	done
}

install -m 0755 "$repo_root/fdrd" /usr/sbin/fdrd
install -m 0644 "$repo_root/fdr.service" /usr/lib/systemd/system/fdr.service
mkdir -p /etc/fdr.d /var/log/fdr
chmod 0700 /var/log/fdr
if ! mountpoint -q /sys/kernel/tracing; then
	mount -t tracefs tracefs /sys/kernel/tracing
fi

uname -a >"$result_dir/uname.txt"
cat /etc/os-release >"$result_dir/os-release.txt"
findmnt -T /sys/kernel/tracing >"$result_dir/tracefs.txt"
{
	printf 'cgroup='
	findmnt -n -o FSTYPE -T /sys/fs/cgroup
	printf 'rootfs='
	findmnt -n -o FSTYPE -T /
	printf 'cpus='
	getconf _NPROCESSORS_ONLN
	systemd --version | head -n 1
} >"$result_dir/environment.txt"

printf 'Running real-VM systemd lifecycle smoke\n'
install_config 4m 512k sched/sched_switch sched/sched_wakeup
systemctl daemon-reload
systemctl start fdr
wait_ready
curl --fail --silent http://127.0.0.1:9119/healthz | grep -Fxq ok
cat /proc/loadavg >"$result_dir/normal-load-before.txt"
normal_started=$(date +%s%N)
run_load 12 6
normal_finished=$(date +%s%N)
cat /proc/loadavg >"$result_dir/normal-load-after.txt"
printf 'elapsed_ns=%s\n' "$((normal_finished - normal_started))" \
    >"$result_dir/normal-load-summary.txt"
for _ in {1..30}; do
	[ -s /var/log/fdr/vm.log ] && grep -q 'sched_switch' /var/log/fdr/vm.log && break
	sleep 1
done
grep -q 'sched_switch' /var/log/fdr/vm.log

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
for _ in {1..8}; do
	run_load 3 8
	rotations_after=$(metric fdr_rotations_total)
	[ "$rotations_after" -gt "$rotations_before" ] && break
done
[ "$rotations_after" -gt "$rotations_before" ]
curl --fail --silent http://127.0.0.1:9119/metrics >"$result_dir/systemd-metrics.txt"
grep -q '^fdr_ready 1$' "$result_dir/systemd-metrics.txt"
grep -q '^fdr_trace_overruns_total 0$' "$result_dir/systemd-metrics.txt"
grep -q '^fdr_trace_dropped_events_total 0$' "$result_dir/systemd-metrics.txt"
grep -q '^fdr_trace_commit_overruns_total 0$' "$result_dir/systemd-metrics.txt"
stat -c '%F %a %s %n' /var/log/fdr/vm.log /var/log/fdr/vm.log.1 \
    >"$result_dir/captures.txt"
for capture in /var/log/fdr/vm.log /var/log/fdr/vm.log.1; do
	[ -f "$capture" ] && [ ! -L "$capture" ] && [ -s "$capture" ]
	[ "$(stat -c %a "$capture")" = 600 ]
done
journalctl -u fdr --no-pager >"$result_dir/fdr-journal.txt"
systemctl stop fdr
for _ in {1..30}; do
	[ ! -d /sys/kernel/tracing/instances/vm-smoke ] && break
	sleep 1
done
[ ! -d /sys/kernel/tracing/instances/vm-smoke ]

printf 'Running controlled trace-loss scenario\n'
mkdir -p /etc/systemd/system/fdr.service.d
cat >/etc/systemd/system/fdr.service.d/benchmark.conf <<'EOF'
[Service]
CPUQuota=1%
EOF
install_config 64k 256m sched/sched_switch sched/sched_wakeup \
    raw_syscalls/sys_enter raw_syscalls/sys_exit
rm -f /var/log/fdr/vm.log /var/log/fdr/vm.log.1
systemctl daemon-reload
systemctl start fdr
wait_ready
loss_start=$(date +%s)
run_syscall_load 35 12 &
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
for _ in {1..30}; do
	loss=$(metric fdr_trace_overruns_total 2>/dev/null || printf 0)
	[ "$loss" -gt 0 ] && break
	sleep 1
done
[ "$loss" -gt 0 ]
[ "$(metric fdr_ready)" -eq 0 ]
curl --fail --silent http://127.0.0.1:9119/metrics \
    >"$result_dir/controlled-loss-metrics.txt"
printf 'degraded_after_seconds=%s\n' "$degraded_after" \
    >"$result_dir/controlled-loss-summary.txt"
systemctl stop fdr
rm -f /etc/systemd/system/fdr.service.d/benchmark.conf
systemctl daemon-reload
[ ! -d /sys/kernel/tracing/instances/vm-smoke ]

kernel=$(uname -r)
cat >"$result_dir/summary.env" <<EOF
KERNEL='$kernel'
SYSTEMD_SMOKE='PASS'
CONTROLLED_LOSS='PASS'
K3S_SMOKE='SKIPPED'
EOF
printf 'PASSED\n' >"$result_dir/status.txt"
trap - EXIT
printf 'Guest validation passed on %s\n' "$kernel"
