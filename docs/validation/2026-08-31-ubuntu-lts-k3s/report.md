# Ubuntu LTS and Noble k3s release qualification

Date: 2026-08-31  
Status: passed  
Application source: `c9abad2c43a613a449b9ae066642761164cb6da7`

## Result

The supported Ubuntu release matrix passed in disposable KVM guests. Ubuntu
Jammy validated FDR on the 5.15 LTS kernel line, and Ubuntu Noble validated the
6.8 LTS kernel line plus a pinned single-node k3s deployment.

| Profile | Reference run | Kernel | systemd | Nominal loss | Controlled loss | k3s |
|---|---|---|---|---|---|---|
| Jammy 22.04 | `20260831T172718Z` | `5.15.0-1106-kvm` | PASS | zero at all checkpoints | PASS | skipped by profile |
| Noble 24.04 | `20260831T173021Z` | `6.8.0-138-generic` | PASS | zero at all checkpoints | PASS | PASS |

Both guests used four vCPUs, 4 GiB RAM, ext4, cgroup v2, and a writable tracefs
mounted at `/sys/kernel/tracing`. The source was built natively inside each
guest and `make check` passed before installation.

## Integrity evidence

The nominal test used `sched/sched_wakeup`, a 32 MiB per-CPU trace buffer, and a
4 MiB bounded output file. Metrics were required to remain ready with zero
storage drops, rotation failures, probe failures, write errors, overruns,
dropped events, and commit overruns at three reset boundaries:

1. after scheduler activity and initial rotations;
2. after valid, rejected, and recovery reloads;
3. after worker termination, systemd recovery, and further rotations.

The exact snapshots are retained as `normal-metrics.txt`,
`reload-metrics.txt`, and `systemd-metrics.txt` under each profile directory.
Both rotated generations were non-empty regular files with mode 0600.

The controlled-loss test applied a 1% CPU quota, selected scheduler switch and
wakeup events, and reduced the trace buffer to 64 KiB per CPU. Readiness became
degraded after five seconds in both guests:

| Profile | Observed overruns | Recorder drops | Readiness |
|---|---:|---:|---:|
| Jammy | 22,381,786 | 0 | 0 |
| Noble | 9,480,886 | 0 | 0 |

These are deliberate overload results, not performance capacity measurements.

## k3s evidence

Noble installed checksum-verified k3s `v1.35.5+k3s1`, imported the locally
built immutable `fdr-vm:dev` image, and installed the Helm chart. The single
DaemonSet pod became ready with no restart, captured `sched_wakeup` into the
host log, and was then uninstalled. The owned `fdr-k3s` tracefs instance was
required to disappear after teardown.

The retained node and workload snapshots are in [`noble/k3s-node.txt`](noble/k3s-node.txt),
[`noble/k3s-resources.txt`](noble/k3s-resources.txt), and
[`noble/k3s-fdr.log`](noble/k3s-fdr.log).

## Harness corrections made during qualification

Exploratory attempts exposed four ways the previous matrix could report
misleading results. The final reference runs include fixes that:

- exclude `.vm-lab`, `fdrd`, and `*.o` from the guest source archive;
- preserve fail-fast behavior and write the report from an exit trap, avoiding
  Bash's suppression of `errexit` inside a conditional function call;
- represent default profiles as a real array; and
- assert zero nominal loss before reload or restart can reset counters.

The first exploratory Jammy run observed nominal loss with a 4 MiB per-CPU
buffer and was rejected. The reference runs use 32 MiB per CPU and include the
new pre-reset metric snapshots; no hidden-loss claim is carried forward.

## Reproduce

Run the profiles independently:

```sh
tests/vm/matrix.sh jammy
tests/vm/matrix.sh noble
```

Or run the complete matrix with `tests/vm/matrix.sh`. The prerequisites and
safety boundary are documented in [`tests/vm/README.md`](../../../tests/vm/README.md).

## Limits

This report qualifies only the named cloud images, kernels, source, and
single-node topology. It does not provide performance sizing, disk-pressure,
SELinux, AppArmor, NetworkPolicy-plugin, or genuinely multi-node evidence.
