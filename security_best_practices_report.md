# PatchCleaner Security Best Practices Review

## Executive Summary

PatchCleaner has a small native code surface and already includes a few good baseline defenses, notably safer DLL search-path setup, heap-corruption termination, `/sdl`, warnings-as-errors, and a fail-closed installer scan. The main security risk is not classic memory-unsafety in the reviewed files; it is the trust the app places in local filesystem state while running with administrator privileges.

The most important improvements are to narrow the privilege boundary, harden the fixed `C:\TempPatchCleanerFiles` move target, and validate filesystem objects before elevated move/delete operations. I did not find command execution, shell launching, registry mutation, or network activity in the canonical source files reviewed.

## Scope

- `PatchCleaner.sln`
- `patch_cleaner/PatchCleaner.vcxproj`
- `patch_cleaner/app/application.cpp`
- `patch_cleaner/app/application.h`
- `patch_cleaner/ui/main_frame.cpp`
- `patch_cleaner/ui/main_frame.h`
- `patch_cleaner/res/compatibility.manifest`
- `patch_cleaner/res/resource.rc`

## High Severity

### SEC-001: Elevated move target is trusted without validating directory type, ACLs, or reparse-point status

Impact: If a local actor or prior run can influence `C:\TempPatchCleanerFiles`, the elevated move operation can be redirected into an unintended location.

Evidence:

- `patch_cleaner/ui/main_frame.cpp:40`
- `patch_cleaner/ui/main_frame.cpp:134`
- `patch_cleaner/ui/main_frame.cpp:139`
- `patch_cleaner/ui/main_frame.cpp:556`
- `patch_cleaner/ui/main_frame.cpp:578`
- `patch_cleaner/ui/main_frame.cpp:588`

Details:

The app uses a fixed quarantine path, `C:\TempPatchCleanerFiles`, and `EnsureDirectoryExists()` treats `ERROR_ALREADY_EXISTS` as success without checking whether the existing object is actually a directory, whether it is a reparse point, or whether its ACLs are appropriate for an elevated workflow. `OnFileMoveToTemp()` then moves files there with `MoveFileEx(...)`.

This is a local trust-boundary issue rather than a remote one, but it matters because the process runs elevated. If the target path is ever redirected, replaced, or configured unsafely, PatchCleaner can end up writing or copying protected installer-cache content into an attacker-chosen location.

Recommended improvements:

- Verify that the destination is a real directory, not a reparse point or other object type.
- Open and inspect the directory by handle before use.
- Apply or verify a restrictive ACL on the quarantine directory.
- Prefer a freshly created per-run subdirectory with randomized naming instead of a permanent shared destination.

## Medium Severity

### SEC-002: The full GUI runs as administrator instead of elevating only destructive operations

Evidence:

- `patch_cleaner/PatchCleaner.vcxproj:113`
- `patch_cleaner/PatchCleaner.vcxproj:140`
- `patch_cleaner/PatchCleaner.vcxproj:172`
- `patch_cleaner/PatchCleaner.vcxproj:204`

Details:

All build configurations request `RequireAdministrator`. That means the entire UI, scan flow, and any future parsing or rendering logic execute with a high-integrity token from process start. In practice, this magnifies the impact of any bug that might otherwise have been limited to the current user.

Recommended improvements:

- Split the product into an unelevated UI plus a narrow elevated helper for move/delete.
- If a helper is too large a change right now, launch as `asInvoker` and prompt for elevation only once the user confirms a destructive action.

### SEC-003: Elevated delete/move operations are path-based and do not reject reparse points before mutation

Evidence:

- `patch_cleaner/ui/main_frame.cpp:143`
- `patch_cleaner/ui/main_frame.cpp:149`
- `patch_cleaner/ui/main_frame.cpp:156`
- `patch_cleaner/ui/main_frame.cpp:585`
- `patch_cleaner/ui/main_frame.cpp:633`

Details:

`ChangeFileWithWritableAttributes()` clears read-only attributes by path and then invokes `MoveFileEx()` or `DeleteFile()` by path. The code does not explicitly reject `FILE_ATTRIBUTE_REPARSE_POINT`, re-resolve the final path, or pin the target object by handle before mutating it.

The installer cache is a relatively trusted location, so this is partly defense-in-depth, but it is still worth hardening because the app runs elevated and performs destructive actions on filesystem objects discovered earlier in the session.

Recommended improvements:

- Open candidate files with `CreateFile(..., FILE_FLAG_OPEN_REPARSE_POINT)` before mutation.
- Reject directories and reparse points explicitly.
- Revalidate that the final path still resolves under `%WINDIR%\Installer`.

## Low Severity

### SEC-004: Enumeration does not filter out directories or reparse points before populating the destructive UI

Evidence:

- `patch_cleaner/ui/main_frame.cpp:98`
- `patch_cleaner/ui/main_frame.cpp:111`
- `patch_cleaner/ui/main_frame.cpp:493`

Details:

`EnumFiles()` inserts any `*.msi` or `*.msp` match it sees without filtering `FILE_ATTRIBUTE_DIRECTORY` or `FILE_ATTRIBUTE_REPARSE_POINT`. That broadens the set of objects that can appear in the list and leaves later destructive paths to discover unsafe objects only at mutation time.

Recommended improvements:

- Exclude non-regular files during enumeration.
- Treat unexpected attributes as a scan error or skip them with telemetry/debug logging.

### SEC-005: Shipping mitigations are only partially pinned in source control

Evidence:

- `patch_cleaner/PatchCleaner.vcxproj:99`
- `patch_cleaner/PatchCleaner.vcxproj:101`
- `patch_cleaner/PatchCleaner.vcxproj:127`
- `patch_cleaner/PatchCleaner.vcxproj:128`
- `patch_cleaner/PatchCleaner.vcxproj:154`
- `patch_cleaner/PatchCleaner.vcxproj:155`
- `patch_cleaner/PatchCleaner.vcxproj:161`
- `patch_cleaner/PatchCleaner.vcxproj:186`
- `patch_cleaner/PatchCleaner.vcxproj:187`
- `patch_cleaner/PatchCleaner.vcxproj:193`

Details:

The project explicitly enables strong baseline settings such as `/W4`, `/WX`, `/sdl`, and compiler-side Control Flow Guard for Release builds. I did not see equivalent explicit linker-side CFG configuration or other exploit mitigations pinned in the project file. I am not claiming those are disabled in the final binary; the concern is that the effective posture can drift with toolchain defaults or local machine settings.

Recommended improvements:

- Explicitly set the release linker guard settings you rely on, including CFG where supported.
- Explicitly pin other release mitigations you want to guarantee, such as ASLR, DEP, and high-entropy VA.
- Consider Spectre mitigation for release builds after measuring compatibility/performance tradeoffs.

### SEC-006: Process-hardening calls are not checked for failure

Evidence:

- `patch_cleaner/app/application.cpp:76`
- `patch_cleaner/app/application.cpp:78`
- `patch_cleaner/app/application.cpp:80`

Details:

`HeapSetInformation`, `SetSearchPathMode`, and `SetDllDirectory(L"")` are good secure defaults, but their return values are ignored. If any of these fail, the process continues without knowing that a hardening step was not applied.

Recommended improvements:

- Check each call and fail closed, or at least surface a fatal error before creating the main window.

## Secure Defaults Already Present

- `patch_cleaner/app/application.cpp:76` enables heap termination on corruption.
- `patch_cleaner/app/application.cpp:78` enables safer search-path behavior.
- `patch_cleaner/app/application.cpp:80` clears the current-directory DLL search path.
- `patch_cleaner/PatchCleaner.vcxproj:99-106`, `patch_cleaner/PatchCleaner.vcxproj:126-133`, `patch_cleaner/PatchCleaner.vcxproj:153-163`, and `patch_cleaner/PatchCleaner.vcxproj:185-195` enable `/W4`, `/WX`, and `/sdl`.
- `patch_cleaner/ui/main_frame.cpp:52-72` and `patch_cleaner/ui/main_frame.cpp:75-95` use resizable buffers for MSI string/SID enumeration rather than fixed-size buffers.
- `patch_cleaner/ui/main_frame.cpp:495-499` and `patch_cleaner/ui/main_frame.cpp:540-545` fail closed if the MSI scan cannot complete safely.
- I did not find `ShellExecute`, `CreateProcess`, `WinExec`, `system`, or registry-write usage in the reviewed canonical source files.

## Suggested Secure-by-Default Roadmap

1. Harden `C:\TempPatchCleanerFiles` immediately: validate it by handle, reject reparse points, and move into a fresh randomized subdirectory with a restrictive ACL.
2. Reduce privilege scope: keep scanning/UI unelevated and elevate only the mutation path.
3. Add file-object validation before move/delete: reject non-regular files and revalidate the final resolved path.
4. Pin shipping exploit mitigations in the project file so release hardening does not depend on environment defaults.
5. Fail closed if loader/process-hardening initialization cannot be applied.

## Notes

- This was a static review only. I did not run a build or dynamic test pass.
- The workspace playbook notes that local MSBuild validation is currently blocked by missing `v145` build tools in this environment.
