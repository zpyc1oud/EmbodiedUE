# Security

## Intended deployment

The Worker protocol connects cooperating processes on a developer-controlled machine.
The documented training procedure uses local TCP.
The protocol is not a hardened multi-tenant service or a public network API.
Keep Worker endpoints inside that trust boundary.

Checkpoints, ONNX policies, `.uerlpol2` files, UE assets, and plugins can contain executable data or data that affects a parser.
Load artifacts only from trusted sources.
Deployment commands copy files and change project settings.
Before execution, examine the target paths.
Use version control to recover project files.

## Reporting an issue

The project does not yet have a dedicated private security contact or a supported-release policy.
Before public release, the owner must provide a private reporting channel and document it here.

For a suspected vulnerability or exposed credential, use an existing private channel to the repository owner.
If no private channel is available, request a private contact.
Keep exploit details and secret values out of the public request.

Include the affected version or commit, component, impact, and a minimal reproduction.
Remove sensitive data from the report.
For ordinary bugs, use [Contributing](CONTRIBUTING.md#contributing).

If a credential is exposed, the owner should revoke or rotate it.
The owner should also examine the affected repository history.
A change to the current file does not remove the historical exposure.
