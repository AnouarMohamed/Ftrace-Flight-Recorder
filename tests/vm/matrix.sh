#!/usr/bin/env bash
set -Eeuo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)
lab_root="$repo_root/.vm-lab"
cache_dir="$lab_root/cache"
runtime_root="$lab_root/runtime"
run_id=${FDR_VM_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}
run_dir="$lab_root/runs/$run_id"
ssh_key="$cache_dir/id_ed25519"
k3s_version=${FDR_VM_K3S_VERSION:-v1.35.5+k3s1}
profiles=("${@:-jammy noble}")

require_command()
{
	command -v "$1" >/dev/null 2>&1 || {
		printf 'error: required command not found: %s\n' "$1" >&2
		exit 1
	}
}

for command_name in curl docker genisoimage git qemu-img qemu-system-x86_64 \
    scp sha256sum ssh ssh-keygen tar; do
	require_command "$command_name"
done

[ -r /dev/kvm ] && [ -w /dev/kvm ] || {
	printf 'error: KVM is not accessible at /dev/kvm\n' >&2
	exit 1
}

mkdir -p "$cache_dir" "$runtime_root" "$run_dir"
if [ ! -s "$ssh_key" ]; then
	ssh-keygen -q -t ed25519 -N '' -f "$ssh_key"
fi
public_key=$(cat "$ssh_key.pub")

image_metadata()
{
	case $1 in
	jammy)
		image_url=https://cloud-images.ubuntu.com/minimal/releases/jammy/release/ubuntu-22.04-minimal-cloudimg-amd64.img
		checksum_url=https://cloud-images.ubuntu.com/minimal/releases/jammy/release/SHA256SUMS
		ssh_port=22422
		run_k3s=0
		;;
	noble)
		image_url=https://cloud-images.ubuntu.com/minimal/releases/noble/release/ubuntu-24.04-minimal-cloudimg-amd64.img
		checksum_url=https://cloud-images.ubuntu.com/minimal/releases/noble/release/SHA256SUMS
		ssh_port=22424
		run_k3s=1
		;;
	*)
		printf 'error: unknown VM profile: %s (expected jammy or noble)\n' "$1" >&2
		exit 2
		;;
	esac
	image_name=${image_url##*/}
	base_image="$cache_dir/$1-$image_name"
}

download_image()
{
	profile=$1
	image_metadata "$profile"
	checksum_file="$cache_dir/$profile-SHA256SUMS"
	curl --fail --location --retry 3 --retry-all-errors \
	    --output "$checksum_file.tmp" "$checksum_url"
	mv "$checksum_file.tmp" "$checksum_file"
	expected=$(awk -v name="$image_name" \
	    '$2 == name || $2 == "*" name { print $1; exit }' "$checksum_file")
	[ -n "$expected" ] || {
		printf 'error: checksum for %s not found in %s\n' \
		    "$image_name" "$checksum_url" >&2
		exit 1
	}
	if [ -s "$base_image" ] && \
	    printf '%s  %s\n' "$expected" "$base_image" | sha256sum --check --status; then
		printf 'Using verified cached image %s\n' "$base_image"
		return
	fi
	printf 'Downloading %s\n' "$image_url"
	curl --fail --location --retry 3 --retry-all-errors \
	    --output "$base_image.tmp" "$image_url"
	printf '%s  %s\n' "$expected" "$base_image.tmp" | sha256sum --check --status || {
		rm -f "$base_image.tmp"
		printf 'error: checksum verification failed for %s\n' "$image_url" >&2
		exit 1
	}
	mv "$base_image.tmp" "$base_image"
}

ssh_options=(-i "$ssh_key" -o BatchMode=yes \
    -o ConnectTimeout=5 -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)

stop_vm()
{
	if [ -n "${qemu_pid:-}" ] && kill -0 "$qemu_pid" 2>/dev/null; then
		kill "$qemu_pid" 2>/dev/null || true
		for _ in {1..30}; do
			kill -0 "$qemu_pid" 2>/dev/null || break
			sleep 1
		done
		kill -KILL "$qemu_pid" 2>/dev/null || true
	fi
}

run_profile()
{
	profile=$1
	download_image "$profile"
	image_metadata "$profile"
	profile_run="$run_dir/$profile"
	runtime_dir="$runtime_root/$profile"
	mkdir -p "$profile_run" "$runtime_dir"
	overlay="$runtime_dir/root.qcow2"
	seed="$runtime_dir/seed.iso"
	console="$profile_run/console.log"
	pid_file="$runtime_dir/qemu.pid"
	source_tar="$runtime_dir/fdr-source.tar"
	uefi_vars="$runtime_dir/OVMF_VARS.fd"

	rm -f "$overlay" "$seed" "$pid_file" "$source_tar" "$uefi_vars"
	qemu-img create -q -f qcow2 -F qcow2 -b "$base_image" "$overlay" 16G
	cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$uefi_vars"
	tar --exclude=.git --exclude=.build -C "$repo_root" -cf "$source_tar" .

	cat >"$runtime_dir/user-data" <<EOF
#cloud-config
users:
  - default
ssh_authorized_keys:
  - $public_key
ssh_pwauth: false
disable_root: true
growpart:
  mode: auto
  devices: ['/']
resize_rootfs: true
EOF
	cat >"$runtime_dir/meta-data" <<EOF
instance-id: fdr-$profile-$run_id
local-hostname: fdr-$profile
EOF
	genisoimage -quiet -output "$seed" -volid cidata -joliet -rock \
	    "$runtime_dir/user-data" "$runtime_dir/meta-data"

	printf 'Starting disposable %s VM on SSH port %s\n' "$profile" "$ssh_port"
	qemu-system-x86_64 \
	    -name "fdr-$profile-$run_id" -machine q35,accel=kvm -cpu host \
	    -smp 4 -m 4096 -display none -monitor none \
	    -serial "file:$console" \
	    -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
	    -drive "if=pflash,format=raw,file=$uefi_vars" \
	    -drive "file=$overlay,if=virtio,format=qcow2,discard=unmap" \
	    -drive "file=$seed,if=virtio,format=raw,readonly=on" \
	    -device virtio-net-pci,netdev=net0 \
	    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$ssh_port-:22" \
	    -daemonize -pidfile "$pid_file"
	qemu_pid=$(cat "$pid_file")
	trap stop_vm RETURN

	ready=0
	for _ in {1..180}; do
		if ssh "${ssh_options[@]}" -p "$ssh_port" \
		    ubuntu@127.0.0.1 true 2>/dev/null; then
			ready=1
			break
		fi
		kill -0 "$qemu_pid" 2>/dev/null || break
		sleep 2
	done
	[ "$ready" -eq 1 ] || {
		printf 'error: %s VM did not become reachable; see %s\n' \
		    "$profile" "$console" >&2
		return 1
	}
	ssh "${ssh_options[@]}" -p "$ssh_port" ubuntu@127.0.0.1 \
	    'cloud-init status --wait >/dev/null'

	scp "${ssh_options[@]}" -P "$ssh_port" "$source_tar" \
	    "$script_dir/guest-validate.sh" ubuntu@127.0.0.1:/tmp/
	if [ "$run_k3s" -eq 1 ]; then
		printf 'Building the immutable local image for the k3s guest\n'
		docker build --provenance=false -t fdr-vm:dev \
		    -f "$repo_root/deploy/kubernetes/Dockerfile" "$repo_root"
		docker save --output "$runtime_dir/fdr-image.tar" fdr-vm:dev
		scp "${ssh_options[@]}" -P "$ssh_port" \
		    "$runtime_dir/fdr-image.tar" \
		    "$(command -v helm)" ubuntu@127.0.0.1:/tmp/
	fi

	ssh "${ssh_options[@]}" -p "$ssh_port" ubuntu@127.0.0.1 \
	    "sudo env RUN_K3S=$run_k3s K3S_VERSION='$k3s_version' \
	    bash /tmp/guest-validate.sh /tmp/fdr-source.tar /var/tmp/fdr-validation"
	ssh "${ssh_options[@]}" -p "$ssh_port" ubuntu@127.0.0.1 \
	    'sudo tar -C /var/tmp -cf /tmp/fdr-validation.tar fdr-validation && sudo chown ubuntu:ubuntu /tmp/fdr-validation.tar'
	scp "${ssh_options[@]}" -P "$ssh_port" \
	    ubuntu@127.0.0.1:/tmp/fdr-validation.tar \
	    "$profile_run/results.tar"
	tar -C "$profile_run" -xf "$profile_run/results.tar" --strip-components=1
	rm -f "$profile_run/results.tar"
	ssh "${ssh_options[@]}" -p "$ssh_port" ubuntu@127.0.0.1 \
	    'sudo poweroff' || true
	for _ in {1..60}; do
		kill -0 "$qemu_pid" 2>/dev/null || break
		sleep 1
	done
	stop_vm
	qemu_pid=
	trap - RETURN
	rm -f "$overlay" "$seed" "$pid_file" "$source_tar" "$uefi_vars" \
	    "$runtime_dir/fdr-image.tar" "$runtime_dir/user-data" \
	    "$runtime_dir/meta-data"
}

status=PASSED
for profile in ${profiles[*]}; do
	if ! run_profile "$profile"; then
		status=FAILED
		break
	fi
done

{
	printf '# FDR disposable-VM matrix\n\n'
	printf -- '- Status: **%s**\n' "$status"
	printf -- '- UTC run: `%s`\n' "$run_id"
	printf -- '- Source commit: `%s`\n\n' "$(git -C "$repo_root" rev-parse HEAD)"
	printf '| Profile | Kernel | systemd smoke | Controlled loss | k3s |\n'
	printf '|---|---|---|---|---|\n'
	for profile in ${profiles[*]}; do
		summary="$run_dir/$profile/summary.env"
		if [ -s "$summary" ]; then
			# shellcheck disable=SC1090
			. "$summary"
			printf '| %s | `%s` | %s | %s | %s |\n' "$profile" \
			    "$KERNEL" "$SYSTEMD_SMOKE" "$CONTROLLED_LOSS" "$K3S_SMOKE"
		else
			printf '| %s | unavailable | FAILED | unavailable | unavailable |\n' "$profile"
		fi
	 done
} >"$run_dir/report.md"

printf 'VM matrix %s: %s\n' "$status" "$run_dir/report.md"
[ "$status" = PASSED ]
