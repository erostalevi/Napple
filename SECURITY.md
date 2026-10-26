# Security policy

## Reporting a vulnerability

Please report security problems **privately**, not in a public issue: use GitHub's
["Report a vulnerability"](https://github.com/erostalevi/NappleCloudRepo/security/advisories/new) (private security advisory) on this repository. Include what you found, how to reproduce it
and the affected commit or release. You can expect an acknowledgement, and a fix or a decision on the report, as time allows: Napple is a small project
maintained by one person.

## Scope

In scope: the `napple` CLI and rewriter (parsing of containers, NCA/NSO/NRO files and other untrusted input), the runtime, the graphics, audio and input
layers, Napple Studio, the packaging scripts and the CI workflows (for example memory-safety bugs on malformed input, path traversal while building an
app, unsafe handling of keys, or secrets exposed through workflows).

Out of scope: bugs that only affect a game's own behaviour, graphics glitches, compatibility problems (use the compatibility report issue form),
and anything that requires running software you were told not to run.

## Keys, firmware and game files are never committed

This repository must contain no `prod.keys`, no firmware, no game files and no data derived from them. If you find any, report it as above and do not
open a public issue that includes it. Never attach keys, firmware or game files to an issue, a pull request or a vulnerability report; logs and
metadata are enough.

## Supported versions

Only the latest commit on `main` and the latest published release receive fixes.
