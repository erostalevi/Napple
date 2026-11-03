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

An idea becomes an issue (*Feature request*) when it is concrete. A bug always goes to an issue, not a discussion. The issue-form chooser links to Discussions (`.github/ISSUE_TEMPLATE/config.yml`); blank issues are off so every report comes through a form.
The same legal rules apply in Discussions: no keys, firmware, game files or dumps.

## Conduct and security
Be respectful and stay on topic; the maintainer moderates issues and Discussions. Security problems, and anything sensitive that is already public in the repository (a key, a game file), are reported privately as described in [CONTRIBUTING.md](../../CONTRIBUTING.md#reporting-security-problems), never in a public issue.

## Sign-off decision
Commits carry a DCO sign-off (`git commit -s`), no CLA. Reason: contributors keep their copyright, the project stays MIT, and a sign-off costs one flag. See [CONTRIBUTING.md](../../CONTRIBUTING.md#sign-off-dco). An automatic DCO check (for example a GitHub App) is a possible later step.
