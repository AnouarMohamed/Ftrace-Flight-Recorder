#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
preflight=${1:-"$repo_root/deploy/kubernetes/preflight.sh"}
tmp_root=$(mktemp -d /tmp/fdr-preflight-XXXXXX)
fake_bin=$tmp_root/bin

cleanup()
{
	rm -rf "$tmp_root"
}
trap cleanup EXIT HUP INT TERM

[ -x "$repo_root/fdrd" ] || {
	echo "missing built fdrd; run make fdrd first" >&2
	exit 1
}
[ -x "$preflight" ] || {
	echo "missing executable preflight script: $preflight" >&2
	exit 1
}
mkdir -p "$fake_bin"
PATH="$fake_bin:$repo_root:$PATH"
export PATH

write_mountinfo()
{
	mountinfo=$1
	mountpoint=$2
	fs_type=$3
	fs_options=${4:-rw,relatime}

	printf '36 1 0:32 / %s %s - %s %s rw\n' \
	    "$mountpoint" "$fs_options" "$fs_type" "$fs_type" >"$mountinfo"
}

write_config()
{
	config_dir=$1
	event=$2

	printf 'instance node\nenable %s\nminfree 5\nsaveto %s/node.log\n' \
	    "$event" "$tmp_root" >"$config_dir/node.conf"
}

expect_fail()
{
	expected=$1
	shift

	if "$@" >"$tmp_root/output" 2>&1; then
		cat "$tmp_root/output" >&2
		echo "expected preflight failure containing: $expected" >&2
		exit 1
	fi
	if ! grep -Fq -- "$expected" "$tmp_root/output"; then
		cat "$tmp_root/output" >&2
		echo "missing expected failure: $expected" >&2
		exit 1
	fi
}

expect_pass()
{
	if ! "$@" >"$tmp_root/output" 2>&1; then
		cat "$tmp_root/output" >&2
		echo "expected preflight success" >&2
		exit 1
	fi
	grep -Fq 'preflight passed:' "$tmp_root/output"
}

make_tracefs()
{
	root=$1

	mkdir -p "$root/instances" "$root/events/sched/sched_switch"
	: >"$root/events/sched/sched_switch/enable"
}

config=$tmp_root/config
mkdir -p "$config"
write_config "$config" sched/sched_switch
mountinfo=$tmp_root/mountinfo

missing=$tmp_root/missing
expect_fail "is missing" "$preflight" --tracefs "$missing" \
    --config "$config" --mountinfo "$mountinfo"

wrong=$tmp_root/wrong
mkdir -p "$wrong/instances"
write_mountinfo "$mountinfo" "$wrong" ext4
expect_fail "not tracefs" "$preflight" --tracefs "$wrong" \
    --config "$config" --mountinfo "$mountinfo"

unmounted=$tmp_root/unmounted
mkdir -p "$unmounted/instances"
: >"$mountinfo"
expect_fail "cannot resolve a filesystem mount" "$preflight" \
    --tracefs "$unmounted" --config "$config" --mountinfo "$mountinfo"

noinstances=$tmp_root/noinstances
mkdir -p "$noinstances"
write_mountinfo "$mountinfo" "$noinstances" tracefs
expect_fail "instances directory is missing" "$preflight" \
    --tracefs "$noinstances" --config "$config" --mountinfo "$mountinfo"

mkdir -p "$noinstances/instances"
empty_config=$tmp_root/empty-config
mkdir -p "$empty_config"
expect_fail "no configuration files" "$preflight" \
    --tracefs "$noinstances" --config "$empty_config" --mountinfo "$mountinfo"

invalid_config=$tmp_root/invalid-config
mkdir -p "$invalid_config"
printf 'instance node\nminfree 0\n' >"$invalid_config/node.conf"
expect_fail "configuration validation failed" "$preflight" \
    --tracefs "$noinstances" --config "$invalid_config" --mountinfo "$mountinfo"

modern=$tmp_root/sys/kernel/tracing
make_tracefs "$modern"
write_mountinfo "$mountinfo" "$modern" tracefs ro,relatime
expect_fail "mount is not writable" "$preflight" \
    --tracefs "$modern" --config "$config" --mountinfo "$mountinfo"

write_mountinfo "$mountinfo" "$modern" tracefs
expect_pass "$preflight" --tracefs "$modern" --config "$config" \
    --mountinfo "$mountinfo"

missing_event=$tmp_root/missing-config
mkdir -p "$missing_event"
write_config "$missing_event" sched/not_present
expect_fail "configured event 'sched/not_present' is unavailable" \
    "$preflight" --tracefs "$modern" --config "$missing_event" \
    --mountinfo "$mountinfo"

disabled_event=$tmp_root/disabled-config
mkdir -p "$disabled_event"
printf 'instance node\ndisable sched/not_present\nminfree 5\nsaveto %s/node.log\n' \
    "$tmp_root" >"$disabled_event/node.conf"
expect_fail "configured event 'sched/not_present' is unavailable" \
    "$preflight" --tracefs "$modern" --config "$disabled_event" \
    --mountinfo "$mountinfo"

legacy=$tmp_root/sys/kernel/debug/tracing
make_tracefs "$legacy"
write_mountinfo "$mountinfo" "$tmp_root/sys/kernel/debug" debugfs
expect_pass "$preflight" --tracefs "$legacy" --config "$config" \
    --mountinfo "$mountinfo"
grep -Fq 'legacy debugfs tracing mount' "$tmp_root/output"

all_config=$tmp_root/all-config
mkdir -p "$all_config"
printf 'instance node\nenable sched/all\nminfree 5\nsaveto %s/node.log\n' \
    "$tmp_root" >"$all_config/node.conf"
mkdir -p "$modern/events/sched"
: >"$modern/events/sched/enable"
write_mountinfo "$mountinfo" "$modern" tracefs
expect_pass "$preflight" --tracefs "$modern" --config "$all_config" \
    --mountinfo "$mountinfo"

cat >"$fake_bin/modprobe" <<'FAKE'
#!/bin/sh
set -eu
if [ "${1:-}" = "--" ]; then
	shift
fi
printf '%s\n' "$1" >>"${MODPROBE_LOG:?}"
if [ "${FAKE_MODPROBE_FAIL:-0}" = "1" ]; then
	exit 1
fi
mkdir -p "$TRACEFS/events/fake/event"
: >"$TRACEFS/events/fake/event/enable"
FAKE
chmod +x "$fake_bin/modprobe"

module_config=$tmp_root/module-config
mkdir -p "$module_config"
printf 'instance node\nmodprobe fake_mod\nenable fake/event\nminfree 5\nsaveto %s/node.log\n' \
    "$tmp_root" >"$module_config/node.conf"
printf '' >"$tmp_root/modprobe.log"
MODPROBE_LOG=$tmp_root/modprobe.log
TRACEFS=$modern
FAKE_MODPROBE_FAIL=0
export MODPROBE_LOG TRACEFS FAKE_MODPROBE_FAIL
expect_pass "$preflight" --tracefs "$modern" --config "$module_config" \
    --mountinfo "$mountinfo"
grep -Fq 'fake_mod' "$tmp_root/modprobe.log"

FAKE_MODPROBE_FAIL=1
export FAKE_MODPROBE_FAIL
expect_fail "cannot load module 'fake_mod'" "$preflight" \
    --tracefs "$modern" --config "$module_config" --mountinfo "$mountinfo"

printf 'preflight tests passed\n'
