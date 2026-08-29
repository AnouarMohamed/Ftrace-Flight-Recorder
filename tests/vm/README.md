# Disposable VM validation

This harness runs real-kernel systemd, controlled trace-loss, and optional k3s
checks in disposable KVM guests. It never mounts or changes the host tracefs.
Each guest uses a copy-on-write disk backed by a checksum-verified official
Ubuntu cloud image.

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
under `.build/vm-lab/runs/<UTC timestamp>`. Base cloud images and the ephemeral
SSH key are cached under `.build/vm-lab/cache`; guest overlays are deleted only
after a successful evidence transfer and retained on failure.

The controlled-loss scenario deliberately applies a 1% CPU quota to FDR and
enables scheduler events with a 64 KiB per-CPU trace buffer. It must observe a
kernel overrun and degraded readiness. This test is safe only because it runs
inside the disposable guest.
