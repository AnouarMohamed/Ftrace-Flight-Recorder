#!/usr/bin/env bash
set -Eeuo pipefail

repo_root=$1
result_dir=$2
profile_mode=${3:-full}
config=/etc/fdr.d/performance.conf
results="$result_dir/results.tsv"
backend_results="$result_dir/backend-results.tsv"
duration=${FDR_PERF_DURATION:-10}
rounds=${FDR_PERF_ROUNDS:-3}
workers=${FDR_PERF_WORKERS:-4}
pause_us=${FDR_PERF_PAUSE_US:-50}
yield_burst=${FDR_PERF_YIELD_BURST:-16}

case $profile_mode in
full|backend) ;;
*) printf 'invalid performance profile: %s\n' "$profile_mode" >&2; exit 2 ;;
esac

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
trap 'printf "error: line %s: %s\n" "$LINENO" "$BASH_COMMAND" >&2' ERR

metric()
{
	name=$1
	curl --fail --silent http://127.0.0.1:9119/metrics | \
	    awk -v metric_name="$name" '$1 == metric_name { print $2; exit }'
}

wait_http()
{
	for _ in {1..100}; do
		curl --fail --silent http://127.0.0.1:9119/healthz >/dev/null 2>&1 && return
		sleep 0.1
	done
	return 1
}

proc_times()
{
	awk '{ print $14, $15 }' "/proc/$1/stat"
}

proc_io()
{
	awk '
        $1 == "rchar:" { rchar=$2 }
        $1 == "wchar:" { wchar=$2 }
        $1 == "syscr:" { syscr=$2 }
        $1 == "syscw:" { syscw=$2 }
        END { print rchar, wchar, syscr, syscw }
    ' "/proc/$1/io"
}

install -m 0644 "$repo_root/fdr.service" /usr/lib/systemd/system/fdr.service
install -m 0755 "$repo_root/.build/sched_load" /usr/local/sbin/fdr-sched-load
install -m 0755 "$repo_root/.build/per_cpu_capture" \
    /usr/local/sbin/fdr-per-cpu-capture
mkdir -p /etc/fdr.d /var/log/fdr
chmod 0700 /var/log/fdr
if ! mountpoint -q /sys/kernel/tracing; then
	mount -t tracefs tracefs /sys/kernel/tracing
fi
cat >"$config" <<'EOF'
instance performance 32m
enable sched/sched_switch
enable sched/sched_wakeup
minfree 1
saveto /var/log/fdr/performance.log 1g
EOF
systemctl daemon-reload

uname -a >"$result_dir/uname.txt"
{
	printf 'cpus=%s\n' "$(getconf _NPROCESSORS_ONLN)"
	printf 'clock_ticks=%s\n' "$(getconf CLK_TCK)"
	printf 'duration_seconds=%s\n' "$duration"
	printf 'load_workers=%s\n' "$workers"
	printf 'load_pause_us=%s\n' "$pause_us"
	printf 'load_yield_burst=%s\n' "$yield_burst"
	printf 'rounds=%s\n' "$rounds"
} >"$result_dir/environment.txt"
printf 'candidate\tround\telapsed_ns\toutput_bytes\twritten_bytes\taccounting_delta\tdropped_bytes\ttrace_overruns\ttrace_dropped\tcommit_overruns\tuser_ticks\tsystem_ticks\tread_calls\twrite_calls\trchar\twchar\tvmhwm_kb\n' >"$results"

candidates=(baseline 4k 8k 16k 64k)
binaries=(
	"$repo_root/.build/fdrd-perf-baseline"
	"$repo_root/.build/fdrd-buffer-4096"
	"$repo_root/.build/fdrd-buffer-8192"
	"$repo_root/.build/fdrd-buffer-16384"
	"$repo_root/.build/fdrd-buffer-65536"
)

candidate_count=${#candidates[@]}
if [ "$profile_mode" = full ]; then
for ((round = 1; round <= rounds; round++)); do
	for ((offset = 0; offset < candidate_count; offset++)); do
		index=$(((round - 1 + offset) % candidate_count))
		candidate=${candidates[$index]}
		binary=${binaries[$index]}
		printf 'Profiling candidate=%s round=%s\n' "$candidate" "$round"
		install -m 0755 "$binary" /usr/sbin/fdrd
		rm -f /var/log/fdr/performance.log /var/log/fdr/performance.log.1
		systemctl start fdr
		wait_http
		parent=$(systemctl show fdr -p MainPID --value)
		worker_pid=$(pgrep -P "$parent" fdrd | head -n 1)
		[ -n "$worker_pid" ]
		read -r user_start system_start < <(proc_times "$worker_pid")
		read -r rchar_start wchar_start reads_start writes_start < <(proc_io "$worker_pid")
		started=$(date +%s%N)
		/usr/local/sbin/fdr-sched-load "$duration" "$workers" \
		    "$pause_us" "$yield_burst" >>"$result_dir/load.txt"
		kill -STOP "$worker_pid"
		finished=$(date +%s%N)
		read -r user_finish system_finish < <(proc_times "$worker_pid")
		read -r rchar_finish wchar_finish reads_finish writes_finish < <(proc_io "$worker_pid")
		vmhwm=$(awk '$1 == "VmHWM:" { print $2 }' "/proc/$worker_pid/status")
		output_bytes=$(stat -c %s /var/log/fdr/performance.log)
		if [ -f /var/log/fdr/performance.log.1 ]; then
			output_bytes=$((output_bytes + $(stat -c %s \
			    /var/log/fdr/performance.log.1)))
		fi
		written=$(metric fdr_bytes_written_total)
		dropped=$(metric fdr_bytes_dropped_total)
		overruns=$(metric fdr_trace_overruns_total)
		trace_dropped=$(metric fdr_trace_dropped_events_total)
		commit=$(metric fdr_trace_commit_overruns_total)
		accounting_delta=$((output_bytes - written))
		absolute_delta=$accounting_delta
		if [ "$absolute_delta" -lt 0 ]; then
			absolute_delta=$((-absolute_delta))
		fi
		printf 'verify candidate=%s round=%s file_bytes=%s metric_bytes=%s\n' \
		    "$candidate" "$round" "$output_bytes" "$written"
		[ "$absolute_delta" -le 65536 ]
		printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		    "$candidate" "$round" "$((finished - started))" \
		    "$output_bytes" "$written" "$accounting_delta" "$dropped" \
		    "$overruns" "$trace_dropped" "$commit" \
		    "$((user_finish - user_start))" \
		    "$((system_finish - system_start))" \
		    "$((reads_finish - reads_start))" \
		    "$((writes_finish - writes_start))" \
		    "$((rchar_finish - rchar_start))" \
		    "$((wchar_finish - wchar_start))" "$vmhwm" >>"$results"
		kill -CONT "$worker_pid"
		systemctl stop fdr
		for _ in {1..50}; do
			[ ! -d /sys/kernel/tracing/instances/performance ] && break
			sleep 0.1
		done
		[ ! -d /sys/kernel/tracing/instances/performance ]
	done
done
fi

trace_loss_totals()
{
	awk '
	    $1 == "overrun:" { overruns += $2 }
	    $1 == "dropped" && $2 == "events:" { dropped += $3 }
	    $1 == "commit" && $2 == "overrun:" { commit += $3 }
	    END { print overruns + 0, dropped + 0, commit + 0 }
	' "$1"/per_cpu/cpu*/stats
}

run_backend_probe()
{
	mode=$1
	instance="/sys/kernel/tracing/instances/fdr-backend-$mode"
	output_dir="/var/log/fdr/backend-$mode"
	summary="$result_dir/backend-$mode.log"
	mkdir -m 0700 "$instance" "$output_dir"
	printf '32768\n' >"$instance/buffer_size_kb"
	printf '1\n' >"$instance/events/sched/sched_switch/enable"
	printf '1\n' >"$instance/events/sched/sched_wakeup/enable"
	printf '1\n' >"$instance/tracing_on"
	/usr/local/sbin/fdr-per-cpu-capture "$mode" "$instance" \
	    "$output_dir" >"$summary" 2>&1 &
	collector=$!
	for _ in {1..100}; do
		grep -q '^ready ' "$summary" && break
		kill -0 "$collector"
		sleep 0.05
	done
	grep -q '^ready ' "$summary"
	read -r user_start system_start < <(proc_times "$collector")
	read -r rchar_start wchar_start reads_start writes_start < \
	    <(proc_io "$collector")
	/usr/local/sbin/fdr-sched-load "$duration" "$workers" \
	    "$pause_us" "$yield_burst" >>"$result_dir/backend-load.txt"
	kill -STOP "$collector"
	for _ in {1..100}; do
		state=$(awk '{ print $3 }' "/proc/$collector/stat")
		[ "$state" = T ] && break
		sleep 0.01
	done
	[ "$state" = T ]
	read -r user_finish system_finish < <(proc_times "$collector")
	read -r rchar_finish wchar_finish reads_finish writes_finish < \
	    <(proc_io "$collector")
	vmhwm=$(awk '$1 == "VmHWM:" { print $2 }' "/proc/$collector/status")
	output_bytes=0
	output_files=0
	first_output=
	for output in "$output_dir"/cpu*."$mode"; do
		[ -f "$output" ] || continue
		[ -n "$first_output" ] || first_output=$output
		output_bytes=$((output_bytes + $(stat -c %s "$output")))
		output_files=$((output_files + 1))
	done
	read -r overruns trace_dropped commit < <(trace_loss_totals "$instance")
	if [ -n "$first_output" ]; then
		dd if="$first_output" of="$result_dir/backend-$mode-sample.bin" \
		    bs=4096 count=1 status=none
	fi
	if [ "$mode" = raw ]; then
		metadata="$result_dir/backend-raw-metadata"
		mkdir -p "$metadata"
		cp "$instance/events/header_page" "$metadata/header_page.txt"
		cp "$instance/events/header_event" "$metadata/header_event.txt"
		cp "$instance/events/sched/sched_switch/format" \
		    "$metadata/sched_switch-format.txt"
		cp "$instance/events/sched/sched_wakeup/format" \
		    "$metadata/sched_wakeup-format.txt"
		cp "$instance/trace_clock" "$metadata/trace_clock.txt"
		cp /sys/kernel/tracing/printk_formats "$metadata/printk_formats.txt"
		cp /sys/kernel/tracing/saved_cmdlines "$metadata/saved_cmdlines.txt"
	fi
	kill -CONT "$collector"
	kill -TERM "$collector"
	status=PASS
	if ! wait "$collector"; then
		status=UNSUPPORTED
	fi
	output_bytes=0
	for output in "$output_dir"/cpu*."$mode"; do
		[ -f "$output" ] || continue
		output_bytes=$((output_bytes + $(stat -c %s "$output")))
	done
	read -r reported_bytes operations capture_errors < <(
		awk 'NR > 1 {
		    for (i = 1; i <= NF; i++) {
		        split($i, field, "=")
		        if (field[1] == "bytes") bytes += field[2]
		        if (field[1] == "operations") operations += field[2]
		        if (field[1] == "error" && field[2] != 0) errors++
		    }
		} END { print bytes + 0, operations + 0, errors + 0 }' "$summary"
	)
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
	    "$mode" "$status" "$output_files" "$output_bytes" \
	    "$reported_bytes" "$operations" "$capture_errors" \
	    "$((user_finish - user_start))" \
	    "$((system_finish - system_start))" \
	    "$((reads_finish - reads_start))" \
	    "$((writes_finish - writes_start))" \
	    "$((rchar_finish - rchar_start))" \
	    "$((wchar_finish - wchar_start))" "$vmhwm" "$overruns" \
	    "$trace_dropped" "$commit" >>"$backend_results"
	printf '0\n' >"$instance/tracing_on"
	rm -rf -- "$output_dir"
	rmdir "$instance"
	if [ "$status" = PASS ]; then
		[ "$output_bytes" -gt 0 ]
		[ "$reported_bytes" -eq "$output_bytes" ]
		[ "$capture_errors" -eq 0 ]
		[ "$overruns" -eq 0 ] && [ "$trace_dropped" -eq 0 ] && \
		    [ "$commit" -eq 0 ]
	fi
	[ "$mode" != text ] || [ "$status" = PASS ]
}

printf 'mode\tstatus\tfiles\toutput_bytes\treported_bytes\toperations\tcapture_errors\tuser_ticks\tsystem_ticks\tread_calls\twrite_calls\trchar\twchar\tvmhwm_kb\ttrace_overruns\ttrace_dropped\tcommit_overruns\n' >"$backend_results"
run_backend_probe text
run_backend_probe raw

# Backticks in this block are literal Markdown, not shell substitutions.
# shellcheck disable=SC2016
{
	printf '# Real-tracefs text collector profile\n\n'
	printf -- '- Status: **PASS**\n'
	printf -- '- Kernel: `%s`\n' "$(uname -r)"
	if [ "$profile_mode" = full ]; then
		printf -- '- Candidates: baseline plus 4, 8, 16, and 64 KiB minimum reads\n'
		printf -- '- Raw data: `results.tsv`\n\n'
		awk -v ticks_per_second="$(getconf CLK_TCK)" \
		    -f "$repo_root/tests/benchmarks/summarize-performance.awk" "$results"
		printf '\nCPU is worker user plus system time normalized by captured bytes.\n'
	else
		printf -- '- Profile: additive backend probe only\n'
	fi
	printf '\n## Additive per-CPU backend probe\n\n'
	printf 'The probe is experimental and does not change FDR text mode. '
	printf 'Raw output is not a qualified `trace.dat`.\n\n'
	printf '| Mode | Status | Files | Bytes | User ticks | System ticks | VmHWM KiB | Loss |\n'
	printf '|---|---|---:|---:|---:|---:|---:|---:|\n'
	awk -F '\t' 'NR > 1 { printf "| %s | %s | %s | %s | %s | %s | %s | %d |\n", $1, $2, $3, $4, $8, $9, $14, $15 + $16 + $17 }' \
	    "$backend_results"
} >"$result_dir/report.md"
journalctl -u fdr --no-pager >"$result_dir/fdr-journal.txt"
printf 'PASSED\n' >"$result_dir/status.txt"
trap - EXIT
printf 'Real-tracefs performance profile passed on %s\n' "$(uname -r)"
