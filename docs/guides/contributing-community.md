# Community: labels, Discussions, conduct, security

Proposals for how the issue tracker and the community spaces are run. Maintainers apply them in the GitHub settings; nothing here is enforced by code, except that the issue forms only use labels from the list.

## Labels
The list is kept machine-readable in [`.github/labels.yml`](../../.github/labels.yml) (create them once with the GitHub UI or `gh label create`). Every new issue gets `needs-triage` from its form.

| Group | Labels |
|---|---|
| Kind | `bug`, `enhancement`, `docs`, `compat`, `catalogue`, `mod` |
| Area | `graphics`, `audio`, `input`, `services`, `build-pipeline`, `studio` |
| Contributor help | `good first issue` (small, scoped, no Mac or game needed), `help wanted`, `needs-mac` |
| State | `needs-triage`, `needs-repro`, `legal`, `wontfix` |

Rules: a `good first issue` names the file to start in and how to test; `legal` issues (possible keys, game data or Nintendo code) are handled privately and the content is removed; `wontfix` is for out-of-scope requests (Windows, Linux, JIT, Nintendo online services; see the roadmap in [README.md](../../README.md)). Catalogue work (title pages, matrix updates) is a good entry point because it needs no code.

## Discussions
Enable GitHub Discussions with these categories:

| Category | For | Type |
|---|---|---|
| Announcements | releases, roadmap posts | maintainers only |
| Q&A | "how do I build / run / read this log" | question and answer |
| Show and tell | screenshots of your own screen, mods, catalogue pages | open |
| Ideas | early feature ideas before they are issues | open |
| Catalogue | who is documenting which title, coordination | open |

An idea becomes an issue (*Feature request*) when it is concrete. A bug always goes to an issue, not a discussion. The issue-form chooser links to Discussions and to the private security report (`.github/ISSUE_TEMPLATE/config.yml`); blank issues are off so every report comes through a form.
The same legal rules apply in Discussions: no keys, firmware, game files or dumps.

## Code of Conduct and security check
- [CODE_OF_CONDUCT.md](../../CODE_OF_CONDUCT.md) (Contributor Covenant 2.1) is present, names GitHub private advisories and the maintainer as the report route, and is linked from CONTRIBUTING.md.
- [SECURITY.md](../../SECURITY.md) is present: private reporting through "Report a vulnerability", scope, the "never commit keys" rule and supported versions. Both are linked from the issue-form chooser. No change was needed.
- Open item for the maintainer: private vulnerability reporting must be switched on in the repository settings for the link to work, and a dedicated conduct contact address would be better than a GitHub profile once the community grows.

## Sign-off decision
Commits carry a DCO sign-off (`git commit -s`), no CLA. Reason: contributors keep their copyright, the project stays MIT, and a sign-off costs one flag. See [CONTRIBUTING.md](../../CONTRIBUTING.md#sign-off-dco). An automatic DCO check (for example a GitHub App) is a possible later step.
