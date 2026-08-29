#!/usr/bin/env bash
set -Eeuo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)
lab_root="$repo_root/.vm-lab"
root_tree="$lab_root/local-root/tree"
root_image="$lab_root/local-root/root.ext4"
run_id=${FDR_VM_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)-local}
run_dir="$lab_root/runs/$run_id"
kernels=("${@:-7.0.12-100.fc43.x86_64 7.1.8-100.fc43.x86_64}")

for command_name in docker mke2fs qemu-system-x86_64; do
	command -v "$command_name" >/dev/null 2>&1 || {
		printf 'error: required command not found: %s\n' "$command_name" >&2
		exit 1
	}
done
[ -r /dev/kvm ] && [ -w /dev/kvm ] || {
	printf 'error: KVM is not accessible at /dev/kvm\n' >&2
	exit 1
}

prepare_root()
{
	mkdir -p "$root_tree"
	if [ ! -x "$root_tree/lib/systemd/systemd" ]; then
		container=fdr-vm-rootfs-$run_id
		docker create --name "$container" kindest/node:v1.35.0 >/dev/null
		docker export --output "$lab_root/local-root/rootfs.tar" "$container"
		docker rm "$container" >/dev/null
		tar --no-same-owner --exclude='./dev/*' -C "$root_tree" \
		    -xf "$lab_root/local-root/rootfs.tar"
	fi
	# OCI layers can intentionally contain non-searchable data directories.
	# mke2fs must traverse the full staging tree while populating the image.
	chmod u+rwx "$root_tree" "$root_tree"/*
	chmod -R u+X "$root_tree"
	mkdir -p "$root_tree/usr/lib/modules" "$root_tree/usr/local/sbin" \
	    "$root_tree/etc/systemd/system"
	for kernel in ${kernels[*]}; do
		[ -r "/boot/vmlinuz-$kernel" ] || {
			printf 'error: kernel image is not readable: /boot/vmlinuz-%s\n' "$kernel" >&2
			exit 1
		}
		[ -d "/usr/lib/modules/$kernel" ] || {
			printf 'error: modules are unavailable for %s\n' "$kernel" >&2
			exit 1
		}
		cp -a "/usr/lib/modules/$kernel" "$root_tree/usr/lib/modules/"
	done
	cat >"$root_tree/usr/local/sbin/fdr-local-boot" <<'EOF'
#!/usr/bin/env bash
set -Eeuo pipefail
modprobe 9pnet_virtio
modprobe 9p
mkdir -p /mnt/host
mount -t 9p -o trans=virtio,version=9p2000.L,msize=104857600 \
    hostshare /mnt/host
result=$(tr ' ' '\n' </proc/cmdline | sed -n 's/^fdr.result=//p')
case $result in
.vm-lab/runs/*) ;;
*) printf 'invalid result path: %s\n' "$result" >&2; exit 2 ;;
esac
exec /mnt/host/tests/vm/local-guest-validate.sh /mnt/host "/mnt/host/$result"
EOF
	chmod 0755 "$root_tree/usr/local/sbin/fdr-local-boot"
	cat >"$root_tree/etc/systemd/system/fdr-local-validation.service" <<'EOF'
[Unit]
Description=FDR local-kernel VM validation
After=basic.target
Requires=basic.target
SuccessAction=poweroff-force
FailureAction=poweroff-force

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/fdr-local-boot
StandardOutput=journal+console
StandardError=journal+console
EOF
	cat >"$root_tree/etc/systemd/system/fdr-local-validation.target" <<'EOF'
[Unit]
Description=FDR local-kernel VM validation target
Requires=fdr-local-validation.service
After=fdr-local-validation.service
AllowIsolate=yes
EOF
	ln -sfn fdr-local-validation.target \
	    "$root_tree/etc/systemd/system/default.target"
	rm -f "$root_image"
	mke2fs -q -t ext4 -d "$root_tree" "$root_image" 4G
}

run_kernel()
{
	kernel=$1
	profile=${kernel//[^[:alnum:]._-]/-}
	profile_dir="$run_dir/$profile"
	console="$profile_dir/console.log"
	overlay="$lab_root/local-root/$profile.qcow2"
	mkdir -p "$profile_dir"
	rm -f "$overlay"
	qemu-img create -q -f qcow2 -F raw -b "$root_image" "$overlay" 8G
	printf 'Booting %s in a disposable KVM guest\n' "$kernel"
	qemu-system-x86_64 \
	    -name "fdr-$profile" -machine q35,accel=kvm -cpu host -smp 4 -m 4096 \
	    -display none -monitor none -no-reboot -serial "file:$console" \
	    -kernel "/boot/vmlinuz-$kernel" \
	    -append "root=/dev/vda rw console=ttyS0 selinux=0 fdr.result=.vm-lab/runs/$run_id/$profile" \
	    -drive "file=$overlay,if=virtio,format=qcow2" \
	    -virtfs "local,path=$repo_root,mount_tag=hostshare,security_model=none" \
	    -net none
	rm -f "$overlay"
	grep -Fxq PASSED "$profile_dir/status.txt"
}

mkdir -p "$run_dir"
prepare_root
status=PASSED
for kernel in ${kernels[*]}; do
	if ! run_kernel "$kernel"; then
		status=FAILED
		break
	fi
done

{
	printf '# FDR local-kernel disposable-VM matrix\n\n'
	printf -- '- Status: **%s**\n' "$status"
	printf -- '- UTC run: `%s`\n' "$run_id"
	printf -- '- Source commit: `%s`\n\n' "$(git -C "$repo_root" rev-parse HEAD)"
	printf '| Kernel | systemd smoke | Controlled loss | k3s |\n'
	printf '|---|---|---|---|\n'
	for kernel in ${kernels[*]}; do
		profile=${kernel//[^[:alnum:]._-]/-}
		summary="$run_dir/$profile/summary.env"
		if [ -s "$summary" ]; then
			# shellcheck disable=SC1090
			. "$summary"
			printf '| `%s` | %s | %s | %s |\n' "$KERNEL" \
			    "$SYSTEMD_SMOKE" "$CONTROLLED_LOSS" "$K3S_SMOKE"
		else
			printf '| `%s` | FAILED | unavailable | unavailable |\n' "$kernel"
		fi
	done
} >"$run_dir/report.md"

printf 'Local-kernel VM matrix %s: %s\n' "$status" "$run_dir/report.md"
[ "$status" = PASSED ]
