# Helm chart

Install FDR as a per-node DaemonSet with configurable node placement, probes,
and image settings.

## Install

```sh
# Override the image registry to match your GHCR repo
helm upgrade --install fdr ./deploy/helm/fdr \
  --namespace fdr-system --create-namespace \
  --set image.repository=ghcr.io/<owner>/fdr \
  --set image.tag=latest
```

## Worker nodes only

```yaml
# values-workers.yaml
nodeSelector:
  kubernetes.io/os: linux
tolerations:
  - key: node-role.kubernetes.io/worker
    operator: Exists
    effect: NoSchedule
```

```sh
helm upgrade --install fdr ./deploy/helm/fdr -f values-workers.yaml
```

## Custom probes

Edit the `config` map in `values.yaml` or pass `--set-file`:

```sh
helm upgrade --install fdr ./deploy/helm/fdr \
  --set-file config.node.conf=./my-probes.conf
```

## Upgrade after image publish

Tag a release to trigger GHCR publish (`.github/workflows/publish-image.yml`):

```sh
git tag v1.3.0
git push origin v1.3.0
```

Then:

```sh
helm upgrade fdr ./deploy/helm/fdr --set image.tag=v1.3.0
```

## Uninstall

```sh
helm uninstall fdr -n fdr-system
```

Host logs under `/var/log/fdr` are not removed automatically.
