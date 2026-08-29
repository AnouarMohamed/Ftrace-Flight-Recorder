# FDR project roadmap

This document turns the next stages of FDR into work that can be picked up,
implemented, and verified independently. It is intentionally ordered: prove and
harden the existing recorder before adding a control plane, user interface, or a
second implementation language.

## Product direction

FDR should become a dependable Linux kernel flight recorder: inexpensive enough
to leave running, bounded in its use of memory and disk, and able to preserve
the evidence surrounding an incident that is difficult to reproduce.

The project should optimize for these properties, in order:

1. Never destabilize the node it is observing.
2. Make data loss and degraded operation visible.
3. Preserve useful evidence before and after an incident.
4. Be straightforward to deploy and remove from a Linux host or Kubernetes
   cluster.
5. Produce captures that can be inspected with established tracing tools.

The C daemon remains the kernel-facing data plane. A new language is not a goal
by itself. Go would be appropriate for a future fleet control plane only when
the project needs reconciliation, remote capture orchestration, or centralized
upload management. Rust would be a deliberate replacement of the daemon, not a
small mixed-language addition.

## Current baseline: v1.4.0

Version 1.4.0 provides the foundation on which the roadmap is based:

- isolated ftrace instances and workers;
- strict configuration validation and transactional reloads;
- disk free-space protection and bounded file rotation;
- worker supervision and safe signal handling;
- health, readiness, and Prometheus endpoints;
- systemd, RPM, OCI image, Kustomize, and Helm packaging;
- unit and fake-tracefs runtime tests;
- GCC, Clang, sanitizer, and static-analysis validation.

The fake tracefs tests are useful for deterministic behavior, but they do not
prove correctness, overhead, or event retention on a real kernel. Closing that
gap is the next priority.

## Milestone overview

| Order | Milestone | Outcome |
|---|---|---|
| 1 | Kubernetes hardening | Safer, reproducible deployment defaults |
| 2 | Real-kernel validation | Repeatable proof on disposable Linux nodes |
| 3 | Performance characterization | Published overhead and event-loss evidence |
| 4 | Incident-triggered capture | A distinctive, genuine flight-recorder workflow |
| 5 | Interoperability and demonstration | Captures that users can inspect and understand |

Implementation status in the current development tree:

- [x] Pin the base Helm and Kustomize images to <code>v1.4.0</code>.
- [x] Select Linux nodes and require tolerations to be explicit.
- [x] Remove the unproven default CPU limit.
- [x] Add a tracefs type, directory, and write-access preflight.
- [x] Make host module exposure opt-in in Helm and omit it from the base
      Kustomize deployment.
- [x] Add optional PodMonitor and ingress NetworkPolicy templates.
- [x] Validate the hardened deployment in a disposable Kind cluster against a
      real host kernel and retain the run evidence.
- [ ] Repeat real-kernel validation on the disposable multi-kernel VM matrix.
- [x] Expose trace overrun, dropped-event, and commit-overrun counters and make
      readiness reflect observed loss.
- [ ] Validate trace-loss behavior under controlled load.

Keep the current Grafana dashboard scoped to validation and operations until
milestones 1 through 3 are complete. Do not begin a Kubernetes operator or
broader product UI before then; those components would increase the maintenance
surface without proving that the recorder itself is dependable.

## Milestone 1: Kubernetes hardening

### 1.1 Use immutable images

Replace `latest` in the base Kustomize and Helm values with a released version.
Production examples should show either a semantic-version tag or an image
digest. Keep `latest` only in explicitly labeled development examples.

Acceptance criteria:

- the rendered base manifests contain no `:latest` image;
- a chart user can override both the repository and tag;
- the release documentation records the image digest;
- rollback instructions use an immutable previous version.

### 1.2 Make node selection intentional

Add `kubernetes.io/os: linux` to the default node selector. Do not tolerate all
taints by default. Provide documented opt-in values for control-plane,
infrastructure, and other specially tainted nodes.

Acceptance criteria:

- the default DaemonSet can only schedule on Linux nodes;
- it does not schedule on control-plane nodes unless explicitly configured;
- example overrides demonstrate selecting a node pool;
- Helm and Kustomize produce equivalent scheduling behavior.

### 1.3 Treat resources as a tracing correctness decision

A CPU limit can throttle a collector and cause trace data to be lost even while
the process remains healthy. Remove the default CPU limit until benchmarks
justify one, or document a tested limit for a clearly defined event workload.
Keep requests configurable and publish the assumptions behind the defaults.

Readiness should become false when the recorder can determine that it is no
longer keeping up. Add counters for kernel overruns or dropped events when the
kernel exposes them.

Acceptance criteria:

- resource defaults cite benchmark results;
- throttling and dropped-event behavior are observable;
- the deployment guide explains how event selection and CPU count affect
  resource usage;
- no default limit is presented as safe without supporting measurements.

### 1.4 Add a tracefs preflight

The current deployment assumes the host has tracefs mounted at
`/sys/kernel/tracing`. Add an explicit preflight that distinguishes these
conditions:

- the path is missing;
- the path exists but is not tracefs;
- tracefs is mounted but inaccessible;
- the configured events are unavailable on this kernel.

The pod must fail with a specific, actionable message. Support a configurable
path for distributions that expose tracing through
`/sys/kernel/debug/tracing`. Do not silently mount or change the host unless the
administrator explicitly enables that behavior.

Acceptance criteria:

- a missing or incorrect mount fails before collectors start;
- the error appears in container logs and the readiness state;
- both supported tracefs paths are covered by tests;
- the deployment documentation includes host verification commands.

### 1.5 Complete metrics discovery and network policy

Keep the HTTP server reachable only where it is needed. Add optional Helm
resources for one of the following approaches:

- a headless Service plus `ServiceMonitor`; or
- direct pod discovery plus `PodMonitor`.

The Prometheus Operator resources must remain optional so the chart does not
require its custom resource definitions. Provide an optional NetworkPolicy that
allows metrics traffic only from a configured monitoring namespace or label
selector. Document that probes do not require a cluster-wide Service.

Acceptance criteria:

- the chart installs without Prometheus Operator CRDs by default;
- enabling monitoring produces valid discovery resources;
- metrics can be scraped from every selected node;
- the example NetworkPolicy defaults to denying unrelated ingress;
- `/healthz`, `/readyz`, and `/metrics` behavior is documented separately.

### 1.6 Document the privileged boundary

FDR changes host-kernel tracing state and can load modules, so the privileged
deployment is a major security boundary rather than an incidental chart value.
Document which features need tracefs write access and module loading. Evaluate a
reduced-capability mode without `modprobe`, but do not claim it is safer until it
has been tested against the supported runtimes and admission policies.

Acceptance criteria:

- the security guide includes a concise threat model;
- RBAC guidance limits who can edit the DaemonSet and its ConfigMap;
- module loading can be disabled or omitted for clusters that do not need it;
- installation fails clearly when Pod Security or admission policy rejects the
  workload.

## Milestone 2: real-kernel and real-cluster validation

Create a disposable Linux test environment. Never run destructive tracefs or
disk-pressure tests on a production node or a shared developer workstation.

### Test matrix

At minimum, cover:

- the oldest supported Linux LTS kernel;
- a current Linux LTS kernel;
- the newest kernel available on the CI or test infrastructure;
- systemd installation on a virtual machine;
- a single-node k3s or Kubernetes deployment;
- cgroup v2, which is the expected modern configuration.

Record the architecture, kernel version, container runtime, Kubernetes version,
filesystem, CPU count, and FDR configuration for every run.

### Automated smoke-test workflow

Add a script such as `tests/kubernetes-smoke.sh` that performs the following
steps and preserves diagnostics on failure:

1. Verify that tracefs is mounted and the selected events exist.
2. Install the DaemonSet with an immutable test image.
3. Wait for rollout and require all selected pods to become ready.
4. Generate known scheduler and I/O activity.
5. Verify that the output file grows and contains the selected events.
6. Verify health, readiness, and Prometheus exposition.
7. Change the ConfigMap and verify a controlled rollout or reload.
8. Introduce an unavailable probe and verify degraded readiness.
9. Terminate a collector and verify supervision and pod recovery.
10. Trigger rotation and verify that both current and preserved output are
    regular files with the expected permissions.
11. Delete the workload and verify that its trace instances are removed.

The script must collect pod descriptions, logs, rendered manifests, events, and
relevant tracefs state before teardown when a step fails.

The local implementation is `deploy/kind/lab.sh run`. A recorded pass on Linux
7.1.8, including healthy/degraded Grafana screenshots and the Prometheus target,
is stored in
[`docs/validation/2026-08-29-kind-kernel-7.1.8/`](docs/validation/2026-08-29-kind-kernel-7.1.8/report.md).
This closes the single-host Kind integration gate, not the kernel matrix below.

### CI strategy

Keep fake-tracefs tests in ordinary pull-request CI. Run privileged real-kernel
tests in an isolated job, self-hosted runner, or disposable nested-virtualization
environment. If that environment cannot be trusted for every pull request, run
it nightly and as a required release check.

Definition of done:

- the smoke test is repeatable from a documented clean environment;
- failures retain enough artifacts to diagnose the node and pod state;
- at least two kernel versions pass;
- the supported-kernel statement in the README is backed by recorded runs.

## Milestone 3: performance and event-loss characterization

The goal is not to publish a single impressive number. The goal is to describe
where FDR remains reliable and how it fails when pushed beyond that range.

### Scenarios

Measure at least these workloads:

- daemon running with no enabled events;
- scheduler events on an idle node;
- scheduler events under CPU saturation;
- block or filesystem events under sustained I/O;
- a deliberately high-volume event set;
- output rotation during sustained capture;
- destination filesystem approaching the `minfree` threshold;
- collector CPU throttling, as a controlled failure case.

Run each scenario without FDR and with FDR. Repeat runs, report variance, and
avoid comparing results collected under different kernel or power-management
settings.

### Measurements

Record:

- events generated and events persisted;
- kernel ring-buffer overruns and recorder-side drops;
- bytes written per second;
- process and system CPU usage;
- resident memory and kernel trace-buffer allocation;
- application workload latency or throughput impact;
- rotation duration and its effect on capture;
- time until readiness reports degradation.

Store the benchmark harness in `tests/benchmarks/` and committed reports in
`docs/benchmarks/`. Every report must include the commit, configuration, full
environment, raw result location, and commands needed to reproduce it.

Definition of done:

- results are reproducible on a clean disposable node;
- loss is reported explicitly rather than inferred from process health;
- Kubernetes resource recommendations point to the results;
- the README makes no performance claim that the reports do not support.

## Milestone 4: incident-triggered capture

This milestone turns continuous trace persistence into a recognizable flight
recorder workflow.

### Required behavior

FDR should retain a bounded rolling window and seal a capture when triggered.
The capture should include configurable time before and after the trigger.

The intended state flow is:

~~~text
ARMED -> TRIGGERED -> CAPTURING_POST_WINDOW -> SEALED -> ARMED
~~~

The first implementation should support manual triggers through a dedicated
signal and an authenticated or locally protected HTTP operation. Later
iterations may trigger from a selected kernel event, health condition, or
external orchestrator.

SIGUSR1 is already reserved for reopening log files and must not be reused.

### Capture bundle

Each sealed capture should be self-describing and contain:

- trace data for the retained pre-trigger and post-trigger windows;
- UTC trigger and capture timestamps;
- trigger source and optional operator-supplied reason;
- hostname or Kubernetes node identity;
- kernel release, architecture, boot ID, and CPU count;
- the effective FDR configuration;
- probe failures, overruns, and drop counters;
- FDR version and capture format version;
- checksums for the bundle contents.

Write the bundle transactionally: create it under a temporary name, flush and
close all contents, then rename it into place. A crash must not make an
incomplete bundle appear complete.

### Bounded-storage semantics

Define these behaviors before implementation:

- maximum rolling-window size and maximum number of sealed captures;
- what happens when a trigger arrives while a capture is already active;
- what happens when free space crosses `minfree` during the post-trigger window;
- whether old sealed captures may ever be deleted automatically;
- how partial data and overruns are represented in metadata;
- behavior across daemon restart and configuration reload.

Favor explicit data loss with metrics and metadata over unbounded disk growth.

### Security and privacy

Kernel traces may contain process names, paths, addresses, and workload timing.
Capture endpoints must not be exposed as unauthenticated cluster-wide write
operations. Bundles should default to mode 0600, and documentation must explain
retention, upload, and redaction responsibilities.

Definition of done:

- pre-trigger and post-trigger data are demonstrated on a real kernel;
- storage remains bounded through repeated triggers;
- simultaneous and repeated trigger behavior is tested;
- incomplete and lossy captures are visibly marked;
- the feature has unit, fake-tracefs, and real-kernel tests.

## Milestone 5: interoperability and demonstration

### Trace format

Run a short design spike before selecting a format. Evaluate at least native
ftrace text, `trace-cmd`/`trace.dat`, and a format accepted by Perfetto. Select
one interoperable output only after demonstrating a complete capture-and-open
workflow. Do not create a custom binary trace format unless existing formats
cannot preserve required data.

Document:

- the exact producer and consumer versions tested;
- whether conversion requires an optional dependency;
- which ftrace fields or metadata are lost during conversion;
- a command that opens or queries the resulting capture.

### Demonstration scenario

Build a reproducible demonstration around one real incident class, such as
scheduler latency or an I/O stall:

1. Deploy FDR on a disposable Kubernetes node.
2. Generate the workload and a known disturbance.
3. Trigger a capture.
4. Download the sealed bundle.
5. Open or query it with the documented analysis tool.
6. Identify the evidence that explains the disturbance.

Publish a short recording and keep the scenario script in the repository. The
demo must show actual output and metrics rather than only a successful pod
rollout.

Definition of done:

- a new user can reproduce the demo from a clean environment;
- the resulting capture opens in the documented external tool;
- the README includes one screenshot or concise output example;
- limitations and observed overhead are shown alongside the demo.

## Release checklist

Use this checklist for the release that completes these milestones. Commands
may be moved into scripts as the workflow matures, but the underlying checks
should remain visible.

~~~sh
make check
make sanitize
make clean
make check CC=gcc CFLAGS='-O2 -g -Werror'
make clean
make check CC=clang CFLAGS='-O2 -g -Werror'
helm lint deploy/helm/fdr
helm template fdr deploy/helm/fdr --namespace fdr-system >/dev/null
kubectl kustomize deploy/kubernetes >/dev/null
docker build -f deploy/kubernetes/Dockerfile .
~~~

Before tagging:

- [ ] Unit, runtime, sanitizer, and static-analysis checks pass.
- [ ] Real-kernel smoke tests pass on the supported matrix.
- [ ] Kubernetes and Helm manifests use an immutable image reference.
- [ ] Benchmark results are committed and summarized accurately.
- [ ] Security and upgrade notes cover new behavior.
- [ ] Version numbers agree across the binary, Makefile, chart, image, RPM, and
      manual page.
- [ ] The image is built for every advertised architecture.
- [ ] A fresh installation and an upgrade from the previous release are tested.
- [ ] The release notes link to test evidence, benchmark reports, and known
      limitations.

## Suggested issue breakdown

Create one issue per item so hardening work and product features remain easy to
review:

- `k8s: pin base manifests to immutable image versions`
- `k8s: add Linux node selection and opt-in toleration profiles`
- `k8s: add tracefs preflight and diagnostics`
- `k8s: add optional Prometheus discovery and NetworkPolicy`
- `metrics: expose trace overruns and recorder drops`
- `test: add disposable real-kernel smoke environment`
- `test: automate privileged Kubernetes smoke test`
- `bench: add capture overhead and event-loss harness`
- `capture: specify rolling-window and trigger semantics`
- `capture: implement transactional incident bundles`
- `interop: evaluate trace.dat and Perfetto workflows`
- `docs: publish reproducible incident demonstration`

Each issue should state its threat or failure model, test plan, documentation
impact, and acceptance criteria. Avoid combining an observable behavior change
with a large unrelated refactor.

## Ideas intentionally deferred

These may become useful, but they are not current priorities:

- Kubernetes operator or custom resources;
- centralized trace upload and retention service;
- browser-based trace viewer;
- automatic remediation from trace events;
- eBPF collection backend;
- rewriting the daemon in another language.

Reconsider an item when a concrete user workflow cannot be handled cleanly by
the daemon, configuration files, metrics, and ordinary Kubernetes resources.
