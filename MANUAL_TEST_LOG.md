# PatchCleaner Manual Test Log

Use this file to record one testing session after a code change.
Keep the sections you use, delete the ones you do not need, and add more test cases as needed.

## Session

- Date:
- Tester:
- Change under test:
- Git commit or branch:
- Build path:
- Build configuration:
- Windows version:
- Run as admin:
- Smart App Control / Defender notes:
- Other environment notes:

## Setup

- Starting installer cache state:
- Starting `C:\TempPatchCleanerFiles` state:
- Seeded sample files or test data:
- Preconditions:
- Anything unusual before launch:

## Quick Checklist

- App launches:
- Scan completes:
- List shows expected orphan files:
- Sorting by Path works:
- Sorting by Size works:
- Select All updates totals correctly:
- Move to Temp works:
- Collision fallback naming works:
- Delete works:
- Read-only files behave correctly:
- UI stays responsive during long actions:
- Error dialogs shown:
- Unexpected behavior:

## Test Cases

### Case 1: 

- Area:
- Preconditions:
- Steps:
  1.
  2.
  3.
- Expected result:
- Actual result:
- Outcome: Pass / Fail / Blocked
- Repro rate:
- Exact error text:
- Suspected trigger:
- Notes:

### Case 2: 

- Area:
- Preconditions:
- Steps:
  1.
  2.
  3.
- Expected result:
- Actual result:
- Outcome: Pass / Fail / Blocked
- Repro rate:
- Exact error text:
- Suspected trigger:
- Notes:

## Bugs Found

### Bug 1

- Short title:
- Severity: Low / Medium / High
- First seen in:
- Repro steps:
  1.
  2.
  3.
- Expected:
- Actual:
- Exact message or dialog text:
- Screenshot or video path:
- Related log path:
- Possible code area:

### Bug 2

- Short title:
- Severity: Low / Medium / High
- First seen in:
- Repro steps:
  1.
  2.
  3.
- Expected:
- Actual:
- Exact message or dialog text:
- Screenshot or video path:
- Related log path:
- Possible code area:

## Raw Evidence

- App output or debug logs:
- Event Viewer entries:
- Screenshots:
- Videos:
- Crash dumps:
- Extra files to review:

## Final Notes For Codex

- What changed before this test run:
- What you want investigated first:
- What felt flaky vs consistently broken:
- Anything you already ruled out:
