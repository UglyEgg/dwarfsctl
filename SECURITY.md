# Security policy

## Supported version

Security fixes are provided for the latest published dwarfsctl release.

## Reporting a vulnerability

Please use GitHub's private
[security advisory form](https://github.com/UglyEgg/dwarfsctl/security/advisories/new).
Do not open a public issue for a vulnerability before a fix is available.

Include the affected version, distribution, reproduction steps and any relevant
image or configuration details. Remove personal data from sample images where
possible. You should receive an acknowledgement within seven days.

The application parses untrusted filesystem images and exposes a read-only FUSE
mount. Path traversal, unsafe extraction, unintended overwrite, command
injection, privilege-boundary and mount-state issues are all in scope.
