#!/bin/sh
set -eu

tracefs=/sys/kernel/tracing
config_dir=/etc/fdr.d
mountinfo=/proc/self/mountinfo

usage()
{
	printf 'usage: %s [--tracefs PATH] [--config DIR] [--mountinfo FILE]\n' \
	    "$0" >&2
}

die()
{
	printf 'fatal: %s\n' "$*" >&2
	exit 1
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--tracefs)
		[ "$#" -ge 2 ] || die "--tracefs requires a path"
		tracefs=$2
		shift 2
		;;
	--config)
		[ "$#" -ge 2 ] || die "--config requires a directory"
		config_dir=$2
		shift 2
		;;
	--mountinfo)
		[ "$#" -ge 2 ] || die "--mountinfo requires a file"
		mountinfo=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		usage
		die "unknown argument: $1"
		;;
	esac
done

case "$tracefs" in
/) ;;
*) tracefs=${tracefs%/} ;;
esac
[ -n "$tracefs" ] || die "tracefs path must not be empty"
[ -d "$tracefs" ] || die "tracefs path $tracefs is missing; verify tracefs.hostPath and mount the host tracefs"
[ -r "$mountinfo" ] || die "cannot read mount table: $mountinfo"
[ -d "$tracefs/instances" ] || die "tracefs instances directory is missing under $tracefs; verify the host supports tracefs instances"
[ -w "$tracefs/instances" ] || die "tracefs instances directory under $tracefs is not writable; run preflight with a privileged writable tracefs mount"
[ -d "$config_dir" ] || die "configuration directory is missing: $config_dir"

set -- "$config_dir"/*.conf
[ -e "$1" ] || die "no configuration files (*.conf) found in $config_dir"

mount_record=$(
	awk -v path="$tracefs" '
	{
		mountpoint = $5
		prefix = mountpoint == "/" ? "" : mountpoint
		if (mountpoint != path && index(path, prefix "/") != 1)
			next
		if (length(mountpoint) <= best)
			next
		best = length(mountpoint)
		options = $6
		type = ""
		for (i = 1; i <= NF; i++) {
			if ($i == "-") {
				type = $(i + 1)
				break
			}
		}
	}
	END { print type "\t" options }
	' "$mountinfo"
)
fs_type=${mount_record%%	*}
fs_options=${mount_record#*	}
[ -n "$fs_type" ] || die "cannot resolve a filesystem mount for $tracefs; verify it is mounted and visible in $mountinfo"
case ",$fs_options," in
*,rw,*) ;;
*) die "$tracefs mount is not writable (options: $fs_options); remount tracefs read-write on the host" ;;
esac
case "$fs_type" in
tracefs)
	;;
debugfs)
	case "$tracefs" in
	*/tracing)
		printf 'note: %s uses the legacy debugfs tracing mount\n' \
		    "$tracefs" >&2
		;;
	*)
		die "$tracefs is debugfs but not a legacy tracing path"
		;;
	esac
	;;
*)
	die "$tracefs is backed by '$fs_type', not tracefs; verify tracefs.hostPath"
	;;
esac

fdrd_bin=$(command -v fdrd || true)
[ -n "$fdrd_bin" ] || die "fdrd executable is not available in PATH"
"$fdrd_bin" -n -c "$config_dir" ||
	die "configuration validation failed; run 'fdrd -n -c $config_dir' on a host with the same files"

modules=$(awk '$1 == "modprobe" { print $2 }' "$@" | awk '!seen[$0]++')
if [ -n "$modules" ]; then
	command -v modprobe >/dev/null 2>&1 ||
		die "configuration contains modprobe directives but modprobe is unavailable; mount the host module tree and include kmod"
	for module in $modules; do
		modprobe -- "$module" ||
			die "cannot load module '$module'; mount /lib/modules with modules.enabled or preload it on every selected node"
	done
fi

events=$(
	awk '$1 == "enable" || $1 == "disable" { print $2 }' "$@" |
	    awk '!seen[$0]++'
)
for event in $events; do
	subsystem=${event%%/*}
	name=${event#*/}
	if [ "$name" = all ]; then
		[ -d "$tracefs/events/$subsystem" ] ||
			die "configured subsystem '$subsystem' is unavailable on this kernel; load its module or choose an available subsystem"
		[ -w "$tracefs/events/$subsystem/enable" ] ||
			die "configured subsystem '$subsystem' is not writable under $tracefs"
	else
		[ -e "$tracefs/events/$event/enable" ] ||
			die "configured event '$event' is unavailable on this kernel; load its module or choose an event available on all selected nodes"
		[ -w "$tracefs/events/$event/enable" ] ||
			die "configured event '$event' is not writable under $tracefs"
	fi
done

printf 'preflight passed: tracefs=%s type=%s config=%s\n' \
    "$tracefs" "$fs_type" "$config_dir"
