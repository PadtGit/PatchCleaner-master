---
name: maintain-patchcleaner-wtl-app
description: Maintain and review this repository's PatchCleaner Windows C++/WTL application. Use when Codex needs to edit or audit files under patch_cleaner/*, adjust PatchCleaner.sln or patch_cleaner/PatchCleaner.vcxproj, preserve installer-cache move/delete behavior and build configuration, coordinate explorer, implementer, and critic passes, or update AGENTS.md with durable repo discoveries after meaningful work.
---

# Maintain PatchCleaner WTL App

## Overview

Maintain this repo with a staged workflow that starts from the canonical PatchCleaner source and project files, keeps build and resource membership truthful, validates with the available MSBuild command when possible, and records durable repo knowledge in `AGENTS.md`.

## Start Here

1. Read `AGENTS.md` before making changes.
2. Treat `PatchCleaner.sln`, `patch_cleaner/PatchCleaner.vcxproj`, and `patch_cleaner/*` as canonical.
3. Treat `Build/*`, `patch_cleaner/Build/*`, `third_party/*`, and `tools/*` as out of scope unless the user explicitly targets them.
4. Prefer narrow, behavior-preserving edits over broad rewrites.

## Workflow

### Round 1: Maintenance

- Map the exact source, project, and resource files before editing.
- Update canonical source files first and touch `PatchCleaner.vcxproj` only when file membership changes require it.
- Preserve build configuration alignment, `TreatWarningAsError`, Unicode, the current `UACExecutionLevel=AsInvoker`, and current installer-cache move/delete behavior unless the task explicitly changes them.
- Validate with the MSBuild commands from `AGENTS.md` when possible. If validation fails, report the actual blocker clearly instead of guessing.

### Round 2: Code-Quality Review

- Hand the changed files or diff to the critic agent after implementation.
- Require a top-line `PASS` or `REVISE`.
- Treat correctness, behavior regressions, UI impact, project/resource drift, platform divergence, and broken validation as blockers.
- If the critic returns `REVISE`, fix only the concrete issues with behavioral or maintenance impact, then rerun focused validation.

### Round 3: Windows Sandbox Validation

- Use this round only after host builds are green and the critic returns `PASS`.
- Skip docs-only, playbook-only, or low-risk tooling-only work.
- Use `scripts/prepare-sandbox-bundle.ps1` and `scripts/sandbox-startup.ps1` for release candidates, scan/move/delete/elevation/UI-responsiveness changes, or when the user explicitly requests sandbox coverage.

### Round 4: Change Analysis

- Only do recent-commit or last-N-days analysis when this workspace has live Git metadata.
- Do not substitute file timestamps for commit windows.
- If Git is unavailable, stop and say that the requested workflow is Git-gated in this repo.

## Agent Handoffs

- Let the orchestrator sequence exploration, implementation, critique, and playbook maintenance.
- Let `repo-explorer` gather canonical paths, validation commands, project membership requirements, and repo constraints without editing files.
- Let `cpp-implementer` own minimal source or project-file changes once the task is understood.
- Let `code-critic` return `PASS` or `REVISE` with concrete findings.
- Let `playbook-librarian` update only the librarian-managed sections of `AGENTS.md`.

## Output Expectations

- Summaries should state the files changed, the validation command or check run, the critic result, and any new durable playbook note.
- If no stable repo knowledge was discovered, do not force an `AGENTS.md` edit.
- Do not promise Git-backed history analysis, successful builds, or a green sandbox run when the required Git metadata, toolchain, or in-sandbox validation is unavailable.
