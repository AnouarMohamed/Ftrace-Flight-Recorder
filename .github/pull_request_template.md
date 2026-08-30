## Summary

Describe the problem and the focused change that solves it.

## Validation

- [ ] `make clean check`
- [ ] `make sanitize SANITIZER_CC=clang`
- [ ] Deployment validation, when files under `deploy/` change
- [ ] Documentation and `CHANGELOG.md` updated when behavior changes

If a check was not run, explain why and identify the CI job that should validate it.

## Security and operations

Describe changes to privileges, configuration trust, filesystem access, networking,
kernel interaction, dependencies, or release artifacts. Write `None` when none apply.
