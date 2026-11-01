---
name: <kebab-case, unique>
batch: <YYYY-MM-DD-slug>
branch: claude/<name>-<batch>
base: orchestrate/<batch>
env: mac | linux-cloud
model: claude-opus-5-5 | claude-sonnet-5-5 | claude-haiku-5-5 | claude-fable-5-1
depends_on: []
status: planned   # planned | launched | handed-off | integrating | integrated | blocked
orchestrator_session: <session id>
session: none
launched_at: none
---

# <name>: <one-line goal>

## Goal
What to achieve and why it matters for Switch-platform coverage (which %Analysis items it closes).

## Owned paths
Paths this session may edit. Shared files (`docs/architecture/runtime-services.md`, `COMPATIBILITY.md`, `Cargo.toml`, `Cargo.lock`, top-level Makefiles) are append-only.

## Do not touch
Paths owned by other items of this batch.

## Starting points
Files, docs, change notes and tools to read first.

## Acceptance
Checkable items, each with the status it must reach in this env (verified / tested).

## Env notes
Build commands, test data, env vars, limits of the environment.

## Result
(filled by napple-integrate: outcome, merged commit, link to the archived integrator file)
