# Security

## Intended deployment

The Worker protocol is designed for cooperating processes on a developer-controlled machine. The documented training flow uses local TCP. This is not a hardened multi-tenant service or a public network API. Keep Worker endpoints within that trusted boundary.

Treat checkpoints, ONNX policies, `.uerlpol2` files, UE assets, and plugins as executable or parser-sensitive inputs. Load artifacts from sources you trust. Deployment commands copy files and change project settings; review their target paths and use version control for recovery.

## Reporting an issue

A dedicated private security contact and supported-release policy have not yet been established. Before public release, the owner needs to enable a private reporting channel and document it here.

For a suspected vulnerability or exposed credential, use an existing private channel to the repository owner. If none is available, request a private contact without disclosing exploit details or secret values publicly. Include the affected version/commit, component, impact, and a minimal reproduction with sensitive data removed. For ordinary bugs, follow [Contributing](CONTRIBUTING.md).

If a credential is found, the owner should revoke or rotate it and review any affected repository history. Editing the current file alone does not remove historical exposure.
