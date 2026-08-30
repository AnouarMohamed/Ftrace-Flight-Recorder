#!/usr/bin/env bash
set -Eeuo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)
lab_root="$repo_root/.vm-lab"
root_tree="$lab_root/local-root/tree"
root_image="$lab_root/local-root/performance-root.ext4"
run_id=${FDR_VM_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)-performance}
run_dir="$lab_root/runs/$run_id"
kernel=${1:-$(uname -r)}
baseline_ref=${FDR_PERF_BASELINE_REF:-fc0208a}
baseline_tree="$repo_root/.build/perf-baseline-src"
build_image=${FDR_PERF_BUILD_IMAGE:-golang:1.26.5-bookworm}
trace_cmd_image=${FDR_PERF_TRACE_CMD_IMAGE:-python:3.12-slim-bookworm}
trace_cmd_version=${FDR_PERF_TRACE_CMD_VERSION:-3.1.6-1}
trace_cmd_bundle="$repo_root/.build/trace-cmd-$trace_cmd_version"
profile_mode=${FDR_PERF_MODE:-full}
profile=${kernel//[^[:alnum:]._-]/-}
profile_dir="$run_dir/$profile"
console="$profile_dir/console.log"
overlay="$lab_root/local-root/$profile-performance.qcow2"

for command_name in docker git mke2fs qemu-img qemu-system-x86_64; do
	command -v "$command_name" >/dev/null 2>&1 || {
		printf 'error: required command not found: %s\n' "$command_name" >&2
		exit 1
	}
done
[ -r /dev/kvm ] && [ -w /dev/kvm ] || {
	printf 'error: KVM is not accessible at /dev/kvm\n' >&2
	exit 1
}
[ -r "/boot/vmlinuz-$kernel" ] || {
	printf 'error: kernel image is not readable: /boot/vmlinuz-%s\n' "$kernel" >&2
	exit 1
}
[ -d "/usr/lib/modules/$kernel" ] || {
	printf 'error: modules are unavailable for %s\n' "$kernel" >&2
	exit 1
}
case $profile_mode in
full|backend) ;;
*) printf 'error: FDR_PERF_MODE must be full or backend\n' >&2; exit 1 ;;
esac

build_candidates()
{
	rm -rf -- "$baseline_tree"
	mkdir -p "$baseline_tree"
	git -C "$repo_root" archive "$baseline_ref" src | tar -x -C "$baseline_tree"
	docker run --rm --security-opt label=disable \
	    --user "$(id -u):$(id -g)" \
	    --volume "$repo_root:/src" --workdir /src \
	    "$build_image" sh -ceu '
		make -B performance-binaries
		cc -I.build/perf-baseline-src/src -D_GNU_SOURCE \
		    -DFDR_VERSION=\"1.4.0\" -std=c11 -O2 -g -Wall -Wextra \
		    -Wpedantic -Wformat=2 -Wshadow -Wstrict-prototypes \
		    -o .build/fdrd-perf-baseline \
		    .build/perf-baseline-src/src/main.c \
		    .build/perf-baseline-src/src/runtime.c \
		    .build/perf-baseline-src/src/util.c \
		    .build/perf-baseline-src/src/config.c \
		    .build/perf-baseline-src/src/trace.c \
		    .build/perf-baseline-src/src/harvest.c \
		    .build/perf-baseline-src/src/process.c \
		    .build/perf-baseline-src/src/http.c
	'
}

build_trace_cmd_bundle()
{
	if [ -x "$trace_cmd_bundle/usr/bin/trace-cmd" ] &&
	    grep -Fxq "$trace_cmd_version" "$trace_cmd_bundle/VERSION"; then
		return
	fi
	rm -rf -- "$trace_cmd_bundle"
	mkdir -p "$trace_cmd_bundle"
	docker run --rm --security-opt label=disable \
	    --env "BUNDLE_UID=$(id -u)" --env "BUNDLE_GID=$(id -g)" \
	    --env "TRACE_CMD_VERSION=$trace_cmd_version" \
	    --volume "$trace_cmd_bundle:/bundle" \
	    --entrypoint /bin/sh "$trace_cmd_image" -ceu '
		apt-get update >/dev/null
		apt-get install -y --no-install-recommends \
		    "trace-cmd=$TRACE_CMD_VERSION" >/dev/null
		installed=$(dpkg-query -W trace-cmd | awk "{ print \$2 }")
		[ "$installed" = "$TRACE_CMD_VERSION" ]
		install -D -m 0755 /usr/bin/trace-cmd /bundle/usr/bin/trace-cmd
		install -D -m 0644 /lib/x86_64-linux-gnu/libtraceevent.so.1 \
		    /bundle/lib/x86_64-linux-gnu/libtraceevent.so.1
		install -D -m 0644 /lib/x86_64-linux-gnu/libtracefs.so.1 \
		    /bundle/lib/x86_64-linux-gnu/libtracefs.so.1
		mkdir -p /bundle/usr/lib/x86_64-linux-gnu/traceevent
		cp -a /usr/lib/x86_64-linux-gnu/traceevent/plugins \
		    /bundle/usr/lib/x86_64-linux-gnu/traceevent/
		printf "%s\n" "$installed" >/bundle/VERSION
		chown -R "$BUNDLE_UID:$BUNDLE_GID" /bundle
	'
}

prepare_root()
{
	mkdir -p "$root_tree"
	if [ ! -x "$root_tree/lib/systemd/systemd" ]; then
		container=fdr-perf-rootfs-$run_id
		docker create --name "$container" kindest/node:v1.35.0 >/dev/null
		docker export --output "$lab_root/local-root/rootfs.tar" "$container"
		docker rm "$container" >/dev/null
		tar --no-same-owner --exclude='./dev/*' -C "$root_tree" \
		    -xf "$lab_root/local-root/rootfs.tar"
	fi
	chmod u+rwx "$root_tree" "$root_tree"/*
	chmod -R u+X "$root_tree"
	mkdir -p "$root_tree/usr/lib/modules" "$root_tree/usr/local/sbin" \
	    "$root_tree/etc/systemd/system"
	cp -a "/usr/lib/modules/$kernel" "$root_tree/usr/lib/modules/"
	cp -a "$trace_cmd_bundle"/. "$root_tree"/
	cat >"$root_tree/usr/local/sbin/fdr-local-performance-boot" <<'EOF'
#!/usr/bin/env bash
set -Eeuo pipefail
modprobe 9pnet_virtio
modprobe 9p
mkdir -p /mnt/host
mount -t 9p -o trans=virtio,version=9p2000.L,msize=104857600 \
    hostshare /mnt/host
result=$(tr ' ' '\n' </proc/cmdline | sed -n 's/^fdr.result=//p')
mode=$(tr ' ' '\n' </proc/cmdline | sed -n 's/^fdr.mode=//p')
case $result in
.vm-lab/runs/*) ;;
*) printf 'invalid result path: %s\n' "$result" >&2; exit 2 ;;
esac
exec /mnt/host/tests/vm/local-performance-guest.sh /mnt/host \
    "/mnt/host/$result" "${mode:-full}"
EOF
	chmod 0755 "$root_tree/usr/local/sbin/fdr-local-performance-boot"
	cat >"$root_tree/etc/systemd/system/fdr-local-performance.service" <<'EOF'
[Unit]
Description=FDR local real-tracefs performance validation
After=basic.target
Requires=basic.target
SuccessAction=poweroff-force
FailureAction=poweroff-force

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/fdr-local-performance-boot
StandardOutput=journal+console
StandardError=journal+console
EOF
	cat >"$root_tree/etc/systemd/system/fdr-local-performance.target" <<'EOF'
[Unit]
Description=FDR local performance target
Requires=fdr-local-performance.service
After=fdr-local-performance.service
AllowIsolate=yes
EOF
	ln -sfn fdr-local-performance.target \
	    "$root_tree/etc/systemd/system/default.target"
	rm -f "$root_image"
	mke2fs -q -t ext4 -d "$root_tree" "$root_image" 4G
}

mkdir -p "$profile_dir"
build_candidates
build_trace_cmd_bundle
prepare_root
rm -f "$overlay"
qemu-img create -q -f qcow2 -F raw -b "$root_image" "$overlay" 8G
{
	printf 'kernel=%s\n' "$kernel"
	printf 'baseline_ref=%s\n' "$baseline_ref"
	printf 'build_image=%s\n' "$build_image"
	printf 'trace_cmd_image=%s\n' "$trace_cmd_image"
	printf 'trace_cmd_version=%s\n' "$trace_cmd_version"
	printf 'profile_mode=%s\n' "$profile_mode"
	printf 'source_commit=%s\n' "$(git -C "$repo_root" rev-parse HEAD)"
	printf 'source_dirty=%s\n' "$(git -C "$repo_root" status --porcelain | wc -l)"
} >"$profile_dir/host.env"
printf 'Booting %s for real-tracefs performance profiling\n' "$kernel"
if qemu-system-x86_64 \
    -name "fdr-perf-$profile" -machine q35,accel=kvm -cpu host \
    -smp 4 -m 4096 -display none -monitor none -no-reboot \
    -serial "file:$console" -kernel "/boot/vmlinuz-$kernel" \
    -append "root=/dev/vda rw console=ttyS0 selinux=0 fdr.result=.vm-lab/runs/$run_id/$profile fdr.mode=$profile_mode" \
    -drive "file=$overlay,if=virtio,format=qcow2" \
    -virtfs "local,path=$repo_root,mount_tag=hostshare,security_model=none" \
    -net none && grep -Fxq PASSED "$profile_dir/status.txt"; then
	rm -f "$overlay"
	printf 'Performance run passed: %s\n' "$profile_dir/report.md"
else
	printf 'Performance run failed; overlay retained: %s\n' "$overlay" >&2
	exit 1
fi
