#!/bin/sh
#
# kernel-smoke.sh - Real-kernel end-to-end smoke test for fdrd.
#
# Licensed under the Universal Permissive License (UPL), Version 1.0.
#
# This test runs fdrd against the host tracefs mount, captures scheduler
# events, requires a bounded rotation, and verifies graceful tracefs cleanup.
# It intentionally uses a short, low-volume workload so it remains suitable
# for disposable CI runners.
#
# Requirements:
# - root privileges (tracefs instance creation requires CAP_SYS_ADMIN)
# - a writable tracefs mount, normally /sys/kernel/tracing
# - a built fdrd binary and .build/sched_load workload generator
# - curl and standard POSIX utilities
#
# Usage:
#   make check
#   make .build/sched_load
#   sudo env FDR_KERNEL_SMOKE_TRACE_ROOT=/sys/kernel/tracing \
#       tests/kernel-smoke.sh
#
# Environment:
#   FDR_KERNEL_SMOKE_TRACE_ROOT  tracefs root (default /sys/kernel/tracing)
#   FDR_KERNEL_SMOKE_PORT        local HTTP port (default 19119)
#   FDR_KERNEL_SMOKE_ARTIFACTS   retained log directory (default .build/kernel-smoke)
#
# Exit status: 0 on success, 1 on smoke failure, 2 on invalid environment.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
trace_root=${FDR_KERNEL_SMOKE_TRACE_ROOT:-/sys/kernel/tracing}
port=${FDR_KERNEL_SMOKE_PORT:-19119}
artifacts=${FDR_KERNEL_SMOKE_ARTIFACTS:-"$repo_root/.build/kernel-smoke"}
instance="fdr-ci-$$"
daemon_pid=
temporary_dir=

say()
{
	printf '%s\n' "$*"
}

fail()
{
	printf 'error: %s\n' "$*" >&2
	exit 1
}

cleanup()
{
	status=$?

	trap - EXIT
	if [ -n "$daemon_pid" ] && kill -0 "$daemon_pid" 2>/dev/null; then
		kill -TERM "$daemon_pid" 2>/dev/null || true
		wait "$daemon_pid" 2>/dev/null || true
	fi
	if [ -n "$temporary_dir" ] && [ -d "$temporary_dir" ]; then
		mkdir -p "$artifacts"
		for file in daemon.log metrics.txt smoke.conf load.txt; do
			if [ -f "$temporary_dir/$file" ]; then
				cp "$temporary_dir/$file" "$artifacts/$file"
				chmod 0644 "$artifacts/$file"
			fi
		done
		rm -rf "$temporary_dir"
	fi
	if [ -d "$trace_root/instances/$instance" ]; then
		rmdir "$trace_root/instances/$instance" 2>/dev/null || true
	fi
	if [ -n "${SUDO_UID:-}" ] && [ -d "$artifacts" ]; then
		chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$artifacts"
	fi
	exit "$status"
}

trap cleanup EXIT
trap 'exit 130' HUP INT TERM

metric()
{
	curl --fail --silent --show-error \
	    "http://127.0.0.1:$port/metrics" |
	    awk -v name="$1" '$1 == name { print $2; exit }'
}

wait_for_http()
{
	path=$1
	attempt=0

	while [ "$attempt" -lt 200 ]; do
		if curl --fail --silent --show-error \
		    "http://127.0.0.1:$port$path" >/dev/null 2>&1; then
			return 0
		fi
		if ! kill -0 "$daemon_pid" 2>/dev/null; then
			return 1
		fi
		attempt=$((attempt + 1))
		sleep 0.1
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
		if ! kill -0 "$daemon_pid" 2>/dev/null; then
			return 1
		fi
		attempt=$((attempt + 1))
		sleep 0.1
	done
	return 1
}

wait_for_capture()
{
	path=$1
	attempt=0

	while [ "$attempt" -lt 300 ]; do
		if [ -s "$path" ] && grep -q 'sched_switch' "$path"; then
			return 0
		fi
		if ! kill -0 "$daemon_pid" 2>/dev/null; then
			return 1
		fi
		attempt=$((attempt + 1))
		sleep 0.1
	done
	return 1
}

[ "$(id -u)" -eq 0 ] ||
    fail "must run as root; tracefs instance creation requires privileges"
[ -d "$trace_root/instances" ] ||
    fail "$trace_root/instances is missing; mount tracefs first"
[ -w "$trace_root/instances" ] ||
    fail "$trace_root/instances is not writable"
command -v curl >/dev/null 2>&1 || fail "curl is required"
[ -x "$repo_root/fdrd" ] ||
    fail "fdrd is not built; run 'make' before this script"
[ -x "$repo_root/.build/sched_load" ] ||
    fail ".build/sched_load is not built; run 'make .build/sched_load'"

if [ -e "$trace_root/instances/$instance" ]; then
	fail "refusing to reuse pre-existing trace instance $instance"
fi

temporary_dir=$(mktemp -d /tmp/fdr-kernel-smoke.XXXXXX)
logfile="$temporary_dir/capture.log"
mkdir -p "$artifacts"

cat >"$temporary_dir/smoke.conf" <<EOF
instance $instance 64k
enable sched/sched_switch
enable sched/sched_wakeup
minfree 1
saveto $logfile 256k
EOF

say "Starting fdrd against $trace_root"
"$repo_root/fdrd" -f -d "$trace_root" -c "$temporary_dir" \
    -a 127.0.0.1 -p "$port" >"$temporary_dir/daemon.log" 2>&1 &
daemon_pid=$!

wait_for_http /healthz || {
	cat "$temporary_dir/daemon.log" >&2
	fail "fdrd did not become healthy"
}
wait_for_http /readyz || {
	cat "$temporary_dir/daemon.log" >&2
	fail "fdrd did not become ready"
}
wait_for_metric fdr_instances 1 ||
    fail "fdrd did not report one configured instance"
wait_for_metric fdr_workers_alive 1 ||
    fail "fdrd did not report a running collector"

say "Generating scheduler load"
"$repo_root/.build/sched_load" 3 2 50 16 >"$temporary_dir/load.txt" 2>&1 ||
    fail "scheduler load generator failed"

wait_for_capture "$logfile" || {
	cat "$temporary_dir/daemon.log" >&2
	fail "no scheduler events reached the capture file"
}
wait_for_metric fdr_rotations_total 1 || {
	cat "$temporary_dir/daemon.log" >&2
	fail "bounded-size rotation did not occur"
}

[ -f "$logfile.1" ] || fail "rotation did not create $logfile.1"
wait_for_metric fdr_bytes_written_total 1 ||
    fail "fdrd did not report written bytes"
wait_for_metric fdr_write_errors_total 0 ||
    fail "fdrd reported write errors"

write_errors=$(metric fdr_write_errors_total)
rotation_failures=$(metric fdr_rotation_failures_total)
ready=$(metric fdr_ready)
[ "$write_errors" -eq 0 ] || fail "write errors observed: $write_errors"
[ "$rotation_failures" -eq 0 ] ||
    fail "rotation failures observed: $rotation_failures"
[ "$ready" -eq 1 ] || fail "readiness degraded during the smoke test"

curl --fail --silent --show-error \
    "http://127.0.0.1:$port/metrics" >"$temporary_dir/metrics.txt"

kill -TERM "$daemon_pid"
wait "$daemon_pid"
daemon_pid=

for attempt in $(seq 1 100); do
	[ ! -d "$trace_root/instances/$instance" ] && break
	sleep 0.1
done
[ ! -d "$trace_root/instances/$instance" ] ||
    fail "fdrd did not remove trace instance $instance"

say "kernel smoke test passed"
