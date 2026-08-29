# Disposable VM validation

These harnesses run real-kernel systemd, controlled trace-loss, and optional
k3s checks in disposable KVM guests. They never mount or change the host
tracefs.

For a fast, offline regression across kernels already installed on the host,
run:

```sh
tests/vm/local-kernel-matrix.sh
```

This boots each kernel directly against a disposable ext4 root derived from the
cached `kindest/node:v1.35.0` image. It validates systemd lifecycle behavior,
normal capture with zero loss, and a controlled overload that must produce a
kernel overrun and degraded readiness. Kernel arguments select individual
installed versions when needed.

For the release-qualification LTS and k3s matrix, each guest uses a
copy-on-write disk backed by a checksum-verified official Ubuntu minimal cloud
image.

Requirements:

- Linux with readable and writable `/dev/kvm`;
- QEMU, `qemu-img`, `genisoimage`, OpenSSH, curl, Docker, and Helm;
- at least four available CPU cores, 8 GiB memory, and 20 GiB disk;
- network access to Ubuntu cloud images, Ubuntu package repositories, and the
  pinned k3s release.

Run both LTS profiles:

```sh
tests/vm/matrix.sh
```

Or run one profile while developing:

```sh
tests/vm/matrix.sh jammy
tests/vm/matrix.sh noble
```

The Jammy guest covers systemd and controlled trace loss. The Noble guest adds
a single-node k3s deployment using k3s `v1.35.5+k3s1`. Results are retained
under `.vm-lab/runs/<UTC timestamp>`. Base cloud images and the ephemeral SSH
key are cached under `.vm-lab/cache`; guest overlays are deleted only
after a successful evidence transfer and retained on failure.

The controlled-loss scenario deliberately applies a 1% CPU quota to FDR and
enables scheduler events with a 64 KiB per-CPU trace buffer. It must observe a
kernel overrun and degraded readiness. This test is safe only because it runs
inside the disposable guest.
