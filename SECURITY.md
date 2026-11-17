# Reporting security vulnerabilities

Please do **not** open a public GitHub issue for security vulnerabilities.

If you believe you have found a security issue, report it privately through
GitHub's [security advisory workflow](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/privately-reporting-a-security-vulnerability)
for this repository, or contact the maintainers directly if that option is
unavailable.

Include a clear description of the issue, steps to reproduce it, and the
impact you believe it has.

Non-vulnerability security suggestions (hardening ideas, threat-model notes,
and similar feedback) are welcome as regular GitHub issues.

## Threat model

FDR protects two assets: host-kernel tracing state and the captured event data
written to host storage. The daemon deliberately runs as root with privileged
tracefs access, so compromise of the pod must be treated as compromise of the
node. The container boundary is not a security boundary for this workload.

The main trust boundaries are:

- deployment and image authors, who control what code runs;
- configuration writers, who select modules, probes, filters, and output paths;
- namespace administrators, who can change the DaemonSet or its ConfigMap;
- metrics consumers, who should have read-only network access to the endpoint;
- workloads on the same node, which must not be able to modify FDR state.

The base deployment disables service-account token automount, does not expose
host modules, and binds HTTP to the pod interface only for probes. These reduce
incidental exposure but do not remove the privileged host-kernel boundary.

## Security-related information

FDR controls host-kernel tracefs and normally runs as root. The Kubernetes
DaemonSet is privileged. Write access to configuration, workload arguments, or
the container image must therefore be limited to trusted administrators.
Compromise of the pod must be treated as compromise of the node: privileged
execution and writable tracefs are intentional capabilities, not a container
isolation boundary.

The base Kubernetes deployment does not expose host <code>/lib/modules</code>.
The Helm chart can mount it read-only when <code>modules.enabled=true</code> for
configurations that use <code>modprobe</code>. Module loading changes the host
kernel and must remain disabled unless it is a reviewed requirement.

The HTTP endpoint has no authentication or TLS and binds to loopback by
default. Kubernetes manifests bind it to the pod interface for health probes but
do not create a Service. Do not expose it to an untrusted network without an
authenticated proxy or equivalent network policy.
The Helm chart's optional NetworkPolicy limits ingress only when the cluster
network plugin enforces NetworkPolicy; it is not application-layer
authentication.

Output files are opened without following a final symlink and must be regular
files. Configuration still selects host paths and tracepoints, so configuration
files must not be writable by untrusted users.

Review enabled probes for performance and data sensitivity before production
use. Sample configurations are examples, not universal safe defaults.

## Kubernetes RBAC

FDR does not call the Kubernetes API and does not need a service-account token.
Grant namespace-scoped permissions only to administrators who must roll out the
DaemonSet and change probes. A starting point is:

~~~yaml
apiVersion: rbac.authorization.k8s.io/v1
kind: Role
metadata:
  name: fdr-admin
  namespace: fdr-system
rules:
  - apiGroups: ["apps"]
    resources: ["daemonsets"]
    verbs: ["get", "list", "watch", "create", "patch", "update"]
  - apiGroups: [""]
    resources: ["configmaps"]
    verbs: ["get", "list", "watch", "create", "patch", "update"]
  - apiGroups: [""]
    resources: ["pods", "pods/log"]
    verbs: ["get", "list", "watch"]
---
apiVersion: rbac.authorization.k8s.io/v1
kind: RoleBinding
metadata:
  name: fdr-admin
  namespace: fdr-system
roleRef:
  apiGroup: rbac.authorization.k8s.io
  kind: Role
  name: fdr-admin
subjects:
  - kind: Group
    name: fdr-admins
    apiGroup: rbac.authorization.k8s.io
~~~

Do not grant `cluster-admin`, secret access, or cluster-wide DaemonSet edit
rights for routine probe changes. Treat write access to the ConfigMap as
equivalent to write access to host tracing configuration.

## Reduced module exposure

Module loading is opt-in. The base Kustomize deployment does not mount host
`/lib/modules`, and Helm defaults to `modules.enabled=false`. Configurations
that omit `modprobe` therefore expose no host module tree. This is a reduced
exposure, not a claim that the pod is no longer privileged: tracefs control
still requires the privileged security context. A configuration that asks for
a module without the matching mount fails with a module-load error.

## Pod Security and admission

The DaemonSet requires a privileged Pod Security standard because it writes to
host tracefs. On clusters that enforce `restricted`, installation is rejected
before any pod starts. Use a dedicated namespace whose policy is explicitly
reviewed rather than relaxing a cluster-wide policy:

~~~yaml
apiVersion: v1
kind: Namespace
metadata:
  name: fdr-system
  labels:
    pod-security.kubernetes.io/enforce: privileged
    pod-security.kubernetes.io/enforce-version: latest
    pod-security.kubernetes.io/audit: privileged
    pod-security.kubernetes.io/warn: privileged
~~~

If admission rejects the workload, the apply output normally names the rejected
field or Pod Security warning. Confirm the state with:

~~~sh
kubectl apply -k deploy/kubernetes
kubectl -n fdr-system get events --sort-by=.lastTimestamp
kubectl -n fdr-system describe daemonset fdr
kubectl -n fdr-system get replicaset,pod
~~~

A missing pod with a `CreateContainerConfigError`, `Forbidden`, or
`violates PodSecurity` event is an admission or RBAC failure, not a collector
crash. Resolve the owning policy or namespace label before investigating FDR
logs. Other admission webhooks can deny the same workload; preserve their denial
message because it is usually more specific than the pod status.

Operational hardening and incident-preservation guidance is in the
[operations guide](./docs/operations.md). Directive-specific constraints are
documented in the [configuration reference](./docs/configuration.md).
