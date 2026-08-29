# FDR Kind smoke-test report

- Status: **PASSED**
- UTC run: 20260829T141232Z
- Base commit: `9eecc9f00b42535b83b00c583e6e05756386263a`
- Tested source: this report's smoke-workflow changes applied to that base
- Host kernel: `Linux 7.1.8-100.fc43.x86_64 x86_64 GNU/Linux`
- Kind: `kind v0.31.0 go1.25.9 X:nodwarf5 linux/amd64`
- Kubernetes: `v1.36.2`
- Container runtime: Docker `29.7.2`

## Checks

| Check | Result | Evidence |
|---|---|---|
| Real trace capture | PASS | scheduler events reached a regular capture file |
| Prometheus discovery | PASS | FDR target and fdr.rules group discovered |
| Grafana provisioning | PASS | dashboard UID fdr-kernel-recorder is available |
| Configuration rollout | PASS | ConfigMap checksum replaced fdr-lab-2lwfc with fdr-lab-lskpb |
| Probe degradation | PASS | missing tracepoint kept liveness healthy and readiness false |
| Collector recovery | PASS | worker termination caused container restart 0 -> 1 |
| Capture rotation | PASS | rotation count increased 1 -> 2; current and preserved files are regular, non-empty, and mode 0600 |
| Graceful cleanup | PASS | Helm uninstall removed the trace instance before cluster deletion |

## Screenshots

- [grafana-healthy](screenshots/grafana-healthy.png)
- [grafana-degraded](screenshots/grafana-degraded.png)
- [prometheus-targets](screenshots/prometheus-targets.png)

## Retained evidence

- [Environment and tool versions](environment.txt)
- [Final recorder metrics](fdr-metrics.txt)
- [Capture file modes, sizes, and scheduler-event samples](capture-sample.txt)
- [Previous container log after forced collector termination](worker-previous.log)

The complete raw Kubernetes, tracefs, and Kind diagnostics remain in the local
run directory under `.build/fdr-lab-artifacts/runs/20260829T141232Z`; they are
not committed because they contain bulky component logs. A local Kind pass
validates one host kernel. It does not replace the multi-kernel disposable-VM
matrix or controlled-load benchmarks.
