# PatchCleaner Playbook

## Project Snapshot

- This repo contains the PatchCleaner Windows GUI application implemented in C++17 with ATL/WTL.
- Treat `PatchCleaner.sln`, `patch_cleaner/PatchCleaner.vcxproj`, and `patch_cleaner/*` as the canonical working tree.
- Treat `Build/*` and `patch_cleaner/Build/*` as generated output unless a task explicitly targets build artifacts.
- Treat `third_party/*` and `tools/*` as vendor/bootstrap surfaces unless a task explicitly targets dependencies or setup files.
- This workspace is a live Git checkout on `main`, and repo-local validation centers on build checks plus optional Windows Sandbox coverage for release or high-risk changes.

## Canonical Layout

- `PatchCleaner.sln`: single Visual Studio solution entrypoint.
- `patch_cleaner/PatchCleaner.vcxproj`: single native Windows application project.
- `patch_cleaner/app/*`: application bootstrap, ATL module lifecycle, and process-wide setup.
- `patch_cleaner/ui/*`: main window behavior, installer cache enumeration, sorting, move, and delete flows.
- `patch_cleaner/res/*`: resources, manifests, and UI assets referenced by the project file.
- `third_party/wtl/*`: vendored headers referenced by the project include path. Do not edit by default.
- `tools/*`: vendor/bootstrap storage such as `vs_BuildTools.exe` and WTL archives. Do not treat as product code by default.

## Safety Invariants

- Keep Debug/Release and Win32/x64 configurations aligned unless the task explicitly scopes a change to one configuration.
- Preserve `PlatformToolset` truthfully. The project currently targets `v145`; do not silently retarget it unless the task explicitly asks for that change.
- Preserve `TreatWarningAsError`, Unicode character set settings, the current `UACExecutionLevel=AsInvoker`, and the WTL include path unless the task explicitly changes build policy.
- Preserve explicit source, header, resource, manifest, and image membership in `patch_cleaner/PatchCleaner.vcxproj`. If a task adds or removes source assets, update the project file deliberately.
- Preserve the current installer-cache workflow unless the task explicitly changes behavior: enumerate `%WINDIR%\Installer\*.msi` and `*.msp`, subtract installed packages and patches via MSI APIs, and surface the remaining files in the UI.
- Preserve the current move/delete semantics unless the task explicitly changes them: clear read-only attributes before mutation, move files into `C:\TempPatchCleanerFiles` with unique names, and keep moved/deleted accounting consistent with successful operations.
- Avoid editing generated build outputs or vendored WTL sources unless the user explicitly asks to work there.

## Validation Commands

- Preferred local wrapper for agent-driven builds:

```powershell
& '.\scripts\build-local.ps1' -Configuration Debug -Platform x64
```

- Release build validation and Windows Sandbox payload prep:

```powershell
& '.\scripts\build-local.ps1' -Configuration Release -Platform x64
```

- Primary build validation:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'PatchCleaner.sln' /t:Build /p:Configuration=Debug /p:Platform=x64 /m
```

- Optional alternate platform build:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'PatchCleaner.sln' /t:Build /p:Configuration=Debug /p:Platform=Win32 /m
```

- Windows Sandbox bundle preparation:

```powershell
& '.\scripts\prepare-sandbox-bundle.ps1'
```

- Windows Sandbox bundle preparation plus launch:

```powershell
& '.\scripts\prepare-sandbox-bundle.ps1' -Launch
```

- Current environment caveat:
  - The canonical local `MSBuild.exe` path is `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`.
  - Direct `MSBuild.exe` invocation and `scripts/build-local.ps1` both succeed in this environment as of 2026-03-25; keep the wrapper as the preferred entrypoint because it still normalizes `Path`/`PATH` and handles stale manifest intermediates.
  - Release-oriented or high-risk sandbox coverage depends on `C:\Windows\System32\WindowsSandbox.exe` launching successfully and on the generated `.wsb` bundle under `%TEMP%\PatchCleanerSandbox\...`.
  - Report the actual blocker honestly. Do not claim a green build or sandbox run if the toolchain, Windows Sandbox launch, or in-sandbox checklist prevents validation.

## Workflow Rounds

1. Maintenance
   - Read this playbook first.
   - Map the exact source, project, and resource files involved before editing.
   - Prefer narrow edits in `patch_cleaner/*` and update `PatchCleaner.vcxproj` only when membership changes require it.
   - Run focused validation and report the actual build blocker clearly instead of guessing.
2. Code-quality review
   - Have the critic review correctness, behavioral regressions, UI impact, project/resource drift, and platform divergence.
   - Require `PASS` or `REVISE`.
3. Windows Sandbox validation
   - Run only after host builds are green and the critic returns `PASS`.
   - Skip docs-only, playbook-only, or low-risk tooling-only work.
   - Use this round for release candidates, changes touching scan/move/delete/elevation/UI responsiveness, or when the user explicitly requests sandbox coverage.
   - Use `scripts/prepare-sandbox-bundle.ps1` to stage a `Release|x64` bundle outside the repo worktree, then launch Windows Sandbox with the generated `.wsb`.
   - Require the seeded scenario to verify non-elevated launch plus admin relaunch, seeded scan results, sorting, `Select All` totals, `Move to Temp`, tokenized collision fallback under `C:\TempPatchCleanerFiles`, `Delete`, and repaint/busy-state behavior during long-running actions.
4. Change analysis
   - Available in this live Git checkout.
   - Use real commit history for recent-window requests and report empty windows honestly.
   - Do not substitute file timestamps for Git-backed history requests.

## Subagent Roles

- `patchcleaner-orchestrator`: coordinates exploration, implementation, validation, critique, and final reporting.
- `repo-explorer`: gathers canonical paths, project settings, resource impacts, and validation commands without editing files.
- `cpp-implementer`: makes the smallest defensible C++/WTL or project-file change.
- `code-critic`: returns `PASS` or `REVISE` with concrete risk-based findings.
- `playbook-librarian`: updates only the librarian-managed sections below.

## Git-Dependent Workflows

- Available in this workspace because live Git metadata is present.
- Use commit history and diffs for recent-window analysis; do not substitute `.gitmodules` or file timestamps.
- Empty windows such as "last 48 hours" can legitimately return no commits and should be reported as empty rather than treated as a blocker.
- Benchmark or performance analysis anchored to recent commits should still cite the actual commit window being analyzed.

The playbook-librarian may update only the two sections below during normal task runs. Other sections change only when repo structure or workflow policy changes.

## Known Pitfalls and Discoveries

- `PatchCleaner.sln` currently references exactly one project: `patch_cleaner/PatchCleaner.vcxproj`.
- The project file explicitly lists source and resource membership, so file additions require deliberate vcxproj updates.
- Build outputs exist under both `Build/*` and `patch_cleaner/Build/*`; treat both trees as generated artifacts.
- The validated local `MSBuild.exe` path is `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`.
- Direct `MSBuild.exe` invocation and `scripts/build-local.ps1` both succeed in this workspace as of 2026-03-25; keep the wrapper as the preferred agent entrypoint because it still normalizes `Path`/`PATH` and handles stale manifest intermediates.
- `scripts/build-local.ps1` now probes Visual Studio 2026 Community, Professional, Enterprise, and Build Tools MSBuild paths before failing, so repo-local bootstrapper updates do not require script edits to remain usable.
- `.gitmodules` declares `third_party/wtl`, and this workspace now has live Git metadata, so recent-commit and last-N-days analysis should use real history rather than timestamps.
- `patch_cleaner/PatchCleaner.vcxproj` currently sets `UACExecutionLevel=AsInvoker` across all configurations; preserve that unless the task explicitly changes product behavior.
- `tools/wtl.zip` should mirror the vendored `third_party/wtl` tree when refreshed so bootstrap artifacts cannot drift behind the headers actually used by the project.
- The current Move to Temp behavior now writes directly into `C:\TempPatchCleanerFiles` and uses a validated copy-and-delete fallback when the same-volume handle rename is rejected by installer-cache files.
- Move to Temp now keeps the plain filename when that slot is free and switches to a per-operation tokenized suffix only after a collision, so historical temp-root contents do not force an ever-growing `name (n)` probe loop.
- `scripts/prepare-sandbox-bundle.ps1` stages `Release|x64`, `MSVCP140.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`, `AutoHotkey64_UIA.exe`, and the sandbox driver scripts into a bundle under `%TEMP%\PatchCleanerSandbox\...` and emits a `.wsb` outside the repo worktree.
- `scripts/sandbox-startup.ps1` seeds disposable orphan `.msi`/`.msp` files inside Windows Sandbox, precreates a collision file in `C:\TempPatchCleanerFiles`, and adds a fresh delete target after the move phase so `Move to Temp` and `Delete` can both be exercised end to end.
- `scripts/sandbox-driver.ahk` now automates the in-sandbox `Scan`, sorting, `Select All`, `Move to Temp`, and delete-phase actions through PatchCleaner's real window and list controls; manual follow-through should only be needed when elevation or automation timing blocks the run.
- Windows Sandbox validation is a release-only or high-risk round by default; it still depends on `C:\Windows\System32\WindowsSandbox.exe` launching successfully and on the in-sandbox checklist completing.

## Improvement Notes

- 2026-03-20: Added a repo-local PatchCleaner playbook, skill package, and subagent role files for staged exploration, implementation, critique, and playbook maintenance.
- 2026-03-22: Refreshed the repo-local bootstrap artifacts by updating `tools/vs_BuildTools.exe`, rebuilding `tools/wtl.zip` from the current vendored WTL tree, and broadening `scripts/build-local.ps1` to find Visual Studio 2026 Build Tools installations.
- 2026-03-22: Simplified Move to Temp to write directly into `C:\TempPatchCleanerFiles` and removed the unused per-run subdirectory path after validating the release build and manual user check.
- 2026-03-23: Reduced UI repaint work by batching Select All updates, avoiding per-tick relayouts for flash-only animations, collapsing list custom draw to row-level notifications, and bounding Move to Temp collision checks with a per-operation tokenized fallback.
- 2026-03-25: Added repo-local Windows Sandbox bundle/startup helpers and documented a release-only seeded end-to-end scenario for scan, Move to Temp, and Delete validation.
- Keep this section focused on durable repo guidance, not task-by-task narrative.
