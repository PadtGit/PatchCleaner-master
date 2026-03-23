# PatchCleaner Playbook

## Project Snapshot

- This repo contains the PatchCleaner Windows GUI application implemented in C++17 with ATL/WTL.
- Treat `PatchCleaner.sln`, `patch_cleaner/PatchCleaner.vcxproj`, and `patch_cleaner/*` as the canonical working tree.
- Treat `Build/*` and `patch_cleaner/Build/*` as generated output unless a task explicitly targets build artifacts.
- Treat `third_party/*` and `tools/*` as vendor/bootstrap surfaces unless a task explicitly targets dependencies or setup files.
- This workspace copy is not a live Git checkout, and there is no repo-local automated test harness beyond build validation.

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
- Preserve `TreatWarningAsError`, Unicode character set settings, `UACExecutionLevel=RequireAdministrator`, and the WTL include path unless the task explicitly changes build policy.
- Preserve explicit source, header, resource, manifest, and image membership in `patch_cleaner/PatchCleaner.vcxproj`. If a task adds or removes source assets, update the project file deliberately.
- Preserve the current installer-cache workflow unless the task explicitly changes behavior: enumerate `%WINDIR%\Installer\*.msi` and `*.msp`, subtract installed packages and patches via MSI APIs, and surface the remaining files in the UI.
- Preserve the current move/delete semantics unless the task explicitly changes them: clear read-only attributes before mutation, move files into `C:\TempPatchCleanerFiles` with unique names, and keep moved/deleted accounting consistent with successful operations.
- Avoid editing generated build outputs or vendored WTL sources unless the user explicitly asks to work there.

## Validation Commands

- Preferred local wrapper for agent-driven builds:

```powershell
& '.\scripts\build-local.ps1' -Configuration Debug -Platform x64
```

- Primary build validation:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'PatchCleaner.sln' /t:Build /p:Configuration=Debug /p:Platform=x64 /m
```

- Optional alternate platform build:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'PatchCleaner.sln' /t:Build /p:Configuration=Debug /p:Platform=Win32 /m
```

- Current environment caveat:
  - The canonical local `MSBuild.exe` path is `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`.
  - Direct `MSBuild.exe` invocation from this shell currently fails with `MSB6001` because the environment contains both `Path` and `PATH`; `scripts/build-local.ps1` sanitizes that before launching MSBuild.
  - Report the actual blocker honestly. Do not claim a green build, and do not silently retarget the project or install toolsets unless the user explicitly asks.

## Workflow Rounds

1. Maintenance
   - Read this playbook first.
   - Map the exact source, project, and resource files involved before editing.
   - Prefer narrow edits in `patch_cleaner/*` and update `PatchCleaner.vcxproj` only when membership changes require it.
   - Run focused validation. If the known `v145` toolchain blocker prevents a build, report that clearly instead of guessing.
2. Code-quality review
   - Have the critic review correctness, behavioral regressions, UI impact, project/resource drift, and platform divergence.
   - Require `PASS` or `REVISE`.
3. Change analysis
   - Only available after this workspace becomes a real Git checkout.
   - Do not substitute file timestamps for Git-backed history requests.

## Subagent Roles

- `patchcleaner-orchestrator`: coordinates exploration, implementation, validation, critique, and final reporting.
- `repo-explorer`: gathers canonical paths, project settings, resource impacts, and validation commands without editing files.
- `cpp-implementer`: makes the smallest defensible C++/WTL or project-file change.
- `code-critic`: returns `PASS` or `REVISE` with concrete risk-based findings.
- `playbook-librarian`: updates only the librarian-managed sections below.

## Git-Dependent Workflows

- Disabled until a real Git checkout exists for this workspace.
- Do not substitute `.gitmodules` or file timestamps for commit history.
- The following requests should currently block with a Git-required explanation:
  - review recent commits from the last 48 hours
  - compare all changes from the last 4 days
  - benchmark or performance analysis anchored to recent commits

The playbook-librarian may update only the two sections below during normal task runs. Other sections change only when repo structure or workflow policy changes.

## Known Pitfalls and Discoveries

- `PatchCleaner.sln` currently references exactly one project: `patch_cleaner/PatchCleaner.vcxproj`.
- The project file explicitly lists source and resource membership, so file additions require deliberate vcxproj updates.
- Build outputs exist under both `Build/*` and `patch_cleaner/Build/*`; treat both trees as generated artifacts.
- The validated local `MSBuild.exe` path is `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`.
- Direct `MSBuild.exe` invocation from this Codex shell can fail with `MSB6001` due to duplicate `Path`/`PATH` environment entries; `scripts/build-local.ps1` normalizes the environment before launching MSBuild.
- `scripts/build-local.ps1` now probes Visual Studio 2026 Community, Professional, Enterprise, and Build Tools MSBuild paths before failing, so repo-local bootstrapper updates do not require script edits to remain usable.
- `.gitmodules` declares `third_party/wtl`, but this workspace copy does not have live Git metadata, so Git-backed workflows are unavailable here.
- The application manifest currently requires administrator execution, and tasks should preserve that unless the user explicitly changes product behavior.
- `tools/wtl.zip` should mirror the vendored `third_party/wtl` tree when refreshed so bootstrap artifacts cannot drift behind the headers actually used by the project.
- The current Move to Temp behavior now writes directly into `C:\TempPatchCleanerFiles` and uses a validated copy-and-delete fallback when the same-volume handle rename is rejected by installer-cache files.

## Improvement Notes

- 2026-03-20: Added a repo-local PatchCleaner playbook, skill package, and subagent role files for staged exploration, implementation, critique, and playbook maintenance.
- 2026-03-22: Refreshed the repo-local bootstrap artifacts by updating `tools/vs_BuildTools.exe`, rebuilding `tools/wtl.zip` from the current vendored WTL tree, and broadening `scripts/build-local.ps1` to find Visual Studio 2026 Build Tools installations.
- 2026-03-22: Simplified Move to Temp to write directly into `C:\TempPatchCleanerFiles` and removed the unused per-run subdirectory path after validating the release build and manual user check.
- Keep this section focused on durable repo guidance, not task-by-task narrative.
