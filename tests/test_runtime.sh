#!/bin/sh
set -eu

daemon=$1
tmp_root=$(mktemp -d /tmp/fdr-runtime-XXXXXX)
port=$((20000 + ($$ % 20000)))
pid=

cleanup()
{
	if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
		kill -TERM "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
	fi
	if [ -d "$tmp_root/instances/setup" ]; then
		rmdir "$tmp_root/instances/setup" 2>/dev/null || true
	fi
	if [ -d "$tmp_root/instances/failing" ]; then
		rmdir "$tmp_root/instances/failing" 2>/dev/null || true
	fi
	if [ -d "$tmp_root/instances/degraded" ]; then
		rmdir "$tmp_root/instances/degraded" 2>/dev/null || true
	fi
	rmdir "$tmp_root/instances" 2>/dev/null || true
	rm -f "$tmp_root/daemon.log"
	rmdir "$tmp_root" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

mkdir "$tmp_root/instances"
"$daemon" -f -d "$tmp_root" -c tests/runtime-fixtures \
    -a 127.0.0.1 -p "$port" >"$tmp_root/daemon.log" 2>&1 &
pid=$!

attempt=0
while ! curl --fail --silent "http://127.0.0.1:$port/healthz" \
    >/dev/null 2>&1; do
	attempt=$((attempt + 1))
	if [ "$attempt" -ge 50 ] || ! kill -0 "$pid" 2>/dev/null; then
		cat "$tmp_root/daemon.log"
		exit 1
	fi
	sleep 0.1
done

curl --fail --silent "http://127.0.0.1:$port/readyz" |
    grep -q '^ready$'
curl --fail --silent "http://127.0.0.1:$port/metrics" |
    grep -q '^fdr_instances 1$'
curl --fail --silent "http://127.0.0.1:$port/metrics" |
    grep -q '^fdr_trace_overruns_total 0$'
curl --fail --silent "http://127.0.0.1:$port/metrics" |
    grep -q '^fdr_trace_dropped_events_total 0$'
curl --fail --silent "http://127.0.0.1:$port/metrics" |
    grep -q '^fdr_rotation_failures_total 0$'

kill -HUP "$pid"
attempt=0
while ! curl --fail --silent "http://127.0.0.1:$port/metrics" |
    grep -q '^fdr_reloads_total 1$'; do
	attempt=$((attempt + 1))
	if [ "$attempt" -ge 50 ] || ! kill -0 "$pid" 2>/dev/null; then
		cat "$tmp_root/daemon.log"
		exit 1
	fi
	sleep 0.1
done

kill -TERM "$pid"
wait "$pid"
pid=

"$daemon" -f -d "$tmp_root" -c tests/degraded-fixtures \
    -a 127.0.0.1 -p "$port" >>"$tmp_root/daemon.log" 2>&1 &
pid=$!
attempt=0
while ! curl --fail --silent "http://127.0.0.1:$port/healthz" \
    >/dev/null 2>&1; do
	attempt=$((attempt + 1))
	if [ "$attempt" -ge 50 ] || ! kill -0 "$pid" 2>/dev/null; then
		cat "$tmp_root/daemon.log"
		exit 1
	fi
	sleep 0.1
done
attempt=0
while curl --fail --silent "http://127.0.0.1:$port/readyz" \
    >/dev/null 2>&1; do
	attempt=$((attempt + 1))
	if [ "$attempt" -ge 50 ] || ! kill -0 "$pid" 2>/dev/null; then
		cat "$tmp_root/daemon.log"
		exit 1
	fi
	sleep 0.1
done
curl --fail --silent "http://127.0.0.1:$port/metrics" |
    grep -q '^fdr_probe_failures_total 1$'
kill -TERM "$pid"
wait "$pid"
pid=

if "$daemon" -f -d "$tmp_root" -c tests/failure-fixtures -p 0 \
    >>"$tmp_root/daemon.log" 2>&1; then
	cat "$tmp_root/daemon.log"
	echo "persistent worker failure did not stop the parent" >&2
	exit 1
fi

echo "runtime tests passed"
