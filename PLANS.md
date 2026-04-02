# PatchCleaner Execution Plans (ExecPlans)

This document defines what a planning document must contain in this repository. A PatchCleaner execution plan, called an `ExecPlan` in this file, is a single Markdown document that explains how to make a working, observable change to the PatchCleaner Windows application. The reader should be able to start with only the current checkout and the ExecPlan file, then implement, validate, and explain the change without relying on prior conversation.

PatchCleaner is a native Windows GUI application written in C++17 with ATL/WTL. ATL and WTL are the Windows template libraries used by this repo to build the main application window and its message handlers. The canonical project files are `PatchCleaner.sln`, `patch_cleaner/PatchCleaner.vcxproj`, and the source tree under `patch_cleaner/`.

## How to use ExecPlans and `PLANS.md`

When authoring an ExecPlan, read `AGENTS.md` first and follow it together with this file. `AGENTS.md` is the repo playbook that records the build commands, the current project layout, the safety invariants, and the Windows Sandbox validation workflow. Every ExecPlan must mention that it follows both `PLANS.md` and `AGENTS.md`, because a future implementer may open only the plan file and nothing else.

When implementing an ExecPlan, do not stop to ask for "next steps" unless the repository itself blocks progress. Continue milestone by milestone, keep the plan current at every stopping point, and record what changed, what was learned, and what still remains. Treat the plan as a living source of truth, not as a static note written once at the beginning.

When discussing or revising an ExecPlan, update the plan itself rather than relying on chat history. Decisions, reversals, unexpected bugs, and validation evidence must be captured in the document so someone can restart from only the plan and the working tree.

When a design is risky or unclear, include a prototyping milestone that proves feasibility in this repository. In PatchCleaner, that can mean building a narrow proof of concept in `patch_cleaner/ui/main_frame.cpp`, validating an MSI API interaction, or staging a Release build for Windows Sandbox before committing to a broader refactor.

## Repository context every ExecPlan must carry

Every PatchCleaner ExecPlan must restate the repository context it relies on. Do not assume the implementer has memorized the layout.

Treat these paths as canonical unless the task explicitly says otherwise:

- `PatchCleaner.sln`: the single Visual Studio solution entry point.
- `patch_cleaner/PatchCleaner.vcxproj`: the single native Windows project file.
- `patch_cleaner/app/application.cpp` and `patch_cleaner/app/application.h`: application startup, command-line parsing, ATL module lifecycle, and main message loop ownership.
- `patch_cleaner/ui/main_frame.cpp` and `patch_cleaner/ui/main_frame.h`: the main PatchCleaner window, UI commands, list behavior, sorting, scan flow, move flow, delete flow, and elevation handoff points.
- `patch_cleaner/res/resource.h`, `patch_cleaner/res/resource.rc`, and `patch_cleaner/res/compatibility.manifest`: resource identifiers, menus, strings, icons, toolbar assets, and manifest metadata.

Treat these paths as generated or vendor-managed unless the task explicitly targets them:

- `Build/*` and `patch_cleaner/Build/*`: generated build output.
- `third_party/*`: vendored dependencies, including WTL headers.
- `tools/*`: vendor/bootstrap assets such as `vs_BuildTools.exe` and `wtl.zip`.

If a plan introduces a new source, header, resource, manifest, bitmap, or icon file, it must say whether `patch_cleaner/PatchCleaner.vcxproj` needs an explicit membership update. This project file lists source and resource membership directly, so new files are not picked up automatically.

## Requirements

NON-NEGOTIABLE REQUIREMENTS:

- Every ExecPlan must be fully self-contained. In its current form it must contain every instruction, assumption, command, and repo fact needed for a novice to finish the work.
- Every ExecPlan must remain a living document. It must be revised as implementation proceeds, as validation succeeds or fails, and as design decisions change.
- Every ExecPlan must guide a novice through PatchCleaner specifically, not through "a C++ app" in the abstract.
- Every ExecPlan must define working behavior, not just code churn. It must explain what a person can do in the running app after the change and how to observe it.
- Every ExecPlan must define unfamiliar terms in plain language. If the plan mentions ATL, WTL, MSI, MSP, UAC, manifest, message loop, or Windows Sandbox, it must explain what the term means in the context of this repo.
- Every ExecPlan must preserve PatchCleaner's current safety invariants unless the task explicitly changes them and the plan explains why.

PatchCleaner safety invariants that plans must respect and restate when relevant:

- Keep Debug and Release behavior aligned unless the task is explicitly scoped to one configuration.
- Keep Win32 and x64 behavior aligned unless the task is explicitly scoped to one platform.
- Do not silently retarget the project. `patch_cleaner/PatchCleaner.vcxproj` currently uses `PlatformToolset=v145`.
- Do not silently change `TreatWarningAsError`, Unicode settings, the WTL include path, or `UACExecutionLevel=AsInvoker`.
- Preserve the current scan model unless the task explicitly changes product behavior. PatchCleaner finds orphaned Windows installer cache files by enumerating `%WINDIR%\Installer\*.msi` and `*.msp`, subtracting installed packages and patches reported by the Windows Installer APIs, and showing the remaining files in the UI.
- Preserve the current move and delete model unless the task explicitly changes product behavior. PatchCleaner clears read-only attributes before mutation, moves files into `C:\TempPatchCleanerFiles`, uses unique names on collision, and keeps moved and deleted totals aligned with successful operations.

Purpose and user-visible behavior come first. Start every ExecPlan by explaining why the change matters to a PatchCleaner user. Good examples are concrete: "After this change, the main window shows a clearer scan-state summary after clicking Scan" or "After this change, Move to Temp keeps the original file name unless a collision forces a tokenized fallback in `C:\TempPatchCleanerFiles`." Weak examples are internal-only: "Refactor command handling" or "Add helper method."

The implementer can list files, read files, search the repo, run PowerShell commands, build with MSBuild, and optionally launch Windows Sandbox. Do not rely on blog posts, issue trackers, or external docs. If outside knowledge is needed, explain it inside the plan in your own words.

## Formatting

Each ExecPlan must be one single fenced code block labeled `md` when it is shared in chat or another document. That code block must begin with ```` ```md ```` and end with ```` ``` ````. Do not nest more fenced blocks inside it. If you need to show commands, patches, transcripts, or code snippets inside an ExecPlan, indent them instead of opening another fenced block.

If the Markdown file itself contains only the ExecPlan, omit the outer triple backticks. This is how checked-in plan files in this repo should be written.

Use two blank lines after every heading. Use ordinary Markdown headings such as `#`, `##`, and `###`. Write in plain prose first. Avoid tables. Avoid long bullet lists unless prose would become less clear. Checklists are allowed only in the `Progress` section, where they are required.

## PatchCleaner-specific guidance

ExecPlans in this repo must stay grounded in the actual Windows GUI flow. A good plan names the exact file, class, function, command handler, or resource that will change, then ties that edit to something a human can see. For example:

- If a plan adds or changes a command in the main window, it should name `patch_cleaner/ui/main_frame.h`, `patch_cleaner/ui/main_frame.cpp`, and any related IDs in `patch_cleaner/res/resource.h` or menu definitions in `patch_cleaner/res/resource.rc`.
- If a plan changes startup, elevation, or command-line behavior, it should name `patch_cleaner/app/application.cpp` and `patch_cleaner/app/application.h`, then explain how the new behavior appears at launch.
- If a plan changes assets or dialog text, it should name the resource files and explain how the user will see the new text, icon, or menu item in the running app.

Anchor the plan with observable outcomes. PatchCleaner is a desktop application, so acceptance should be phrased in terms of what appears in the window, what files are created or moved, what command becomes available, or what validation output appears in the terminal. A plan should not stop at "the solution builds." It should say what a user does after the build and what they should observe next.

When a change affects scan results, move to temp, deletion, elevation, file accounting, selection totals, sorting, repaint behavior, or command responsiveness, the ExecPlan must include an end-to-end scenario that exercises those behaviors. When a change is smaller, the plan must still describe the smallest useful demonstration in the running app or the most specific test/build evidence available.

## Validation is mandatory

Every PatchCleaner ExecPlan must include exact commands and explain why those commands are enough for the change. Run commands from the repo root, which is the directory that contains `PatchCleaner.sln`. In this checkout that directory is `C:\Users\Bob\Documents\PatchCleaner-master`.

Preferred local build validation:

    powershell -NoLogo -ExecutionPolicy Bypass -File .\scripts\build-local.ps1 -Configuration Debug -Platform x64

Primary direct MSBuild fallback:

    & 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'PatchCleaner.sln' /t:Build /p:Configuration=Debug /p:Platform=x64 /m

Optional alternate platform validation when the change could diverge between x64 and Win32, or when `patch_cleaner/PatchCleaner.vcxproj` was edited:

    powershell -NoLogo -ExecutionPolicy Bypass -File .\scripts\build-local.ps1 -Configuration Debug -Platform Win32

Release validation when the plan touches high-risk user-facing behavior, release packaging, or Windows Sandbox setup:

    powershell -NoLogo -ExecutionPolicy Bypass -File .\scripts\build-local.ps1 -Configuration Release -Platform x64

Windows Sandbox bundle preparation:

    powershell -NoLogo -ExecutionPolicy Bypass -File .\scripts\prepare-sandbox-bundle.ps1

Windows Sandbox bundle preparation plus launch:

    powershell -NoLogo -ExecutionPolicy Bypass -File .\scripts\prepare-sandbox-bundle.ps1 -Launch

The plan must say which of these commands to run and why. A docs-only plan may explain why no build is necessary, but it must say that explicitly. A source, resource, or project change should normally require at least the Debug x64 build. A project-file change, configuration change, or architecture-sensitive change should normally require both Debug x64 and Debug Win32 validation. A change that touches scan correctness, move/delete semantics, elevation, or UI responsiveness should normally require Release x64 validation and, unless the plan gives a strong reason not to, Windows Sandbox coverage.

If validation is blocked, capture the real blocker and the exact command that failed. Do not write around the failure. Example blockers include MSBuild missing from the expected path, `AutoHotkey64_UIA.exe` missing for sandbox automation, or `C:\Windows\System32\WindowsSandbox.exe` unavailable.

## Milestones

Milestones are strongly encouraged for non-trivial PatchCleaner work. Each milestone must describe what exists afterward that did not exist before, what to edit, what to run, and what observable result proves the milestone succeeded.

Each milestone must be independently verifiable. In this repo that often means:

- a build that now succeeds after a project or compile fix,
- a visible main-window behavior change after launching the built executable,
- a deterministic move/delete or sorting scenario,
- or a sandbox run that exercises the real installer-cache workflow on seeded files.

Do not create milestones that only rename helpers or shuffle code without an observable effect. If a milestone is internal, explain how the new internal path will be proven through a test, build, or UI scenario.

## Living plans and design decisions

Every PatchCleaner ExecPlan must contain these sections and keep them current:

- `Progress`
- `Surprises & Discoveries`
- `Decision Log`
- `Outcomes & Retrospective`

`Progress` must be a checklist with timestamps. It must always show the truth. If work is partially complete, split the entry so a future implementer can see what is done and what remains.

`Surprises & Discoveries` must capture unexpected repo behavior, Windows behavior, performance findings, UI quirks, build traps, or validation discoveries. In this repo, that often means things like stale intermediate manifests, collision handling nuances in `C:\TempPatchCleanerFiles`, or a sandbox automation limitation.

`Decision Log` must record each important choice and the reason for it. If the plan changes course, record the old assumption, the new choice, and the reason for the change.

`Outcomes & Retrospective` must summarize what was achieved, what remains open, and what the team learned. Tie the outcome back to the user-visible purpose stated at the top of the plan.

When revising a plan, update every affected section and add a short note at the bottom of the plan describing what changed and why.

## Prototyping milestones and parallel implementations

Prototyping is allowed and often helpful in this repo when it reduces risk. A PatchCleaner prototype might isolate a new sort behavior in `patch_cleaner/ui/main_frame.cpp`, validate a Windows Installer API call before threading it through the full UI, or confirm a new resource and command wiring path before polishing the rest of the feature.

Clearly label a prototype as a prototype. State what question it answers, how to run it, what output proves success, and what will determine whether the prototype is promoted into the main implementation or discarded.

Parallel implementations are also allowed when they reduce risk. For example, it can be reasonable to keep the old path and the new path side by side while proving the replacement in the UI. If you do this, the plan must say how both paths are exercised and how the old path will be removed safely.

## Skeleton of a good PatchCleaner ExecPlan

Copy the structure below into a new Markdown file when writing a plan for this repo. Replace the example text with task-specific details and keep every required section current as work proceeds.

    # <Short, action-oriented description>

    This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

    This ExecPlan follows `PLANS.md` and `AGENTS.md` from the repository root. It must stay self-contained so a contributor can restart from only this file and the current working tree.

    ## Purpose / Big Picture

    Explain what a PatchCleaner user gains after this change and how to see it working. State the visible behavior in the running application, in a build transcript, or in the file system. Example: "After this change, clicking Move to Temp keeps the plain file name when the destination slot is free and only falls back to a tokenized name after an actual collision in `C:\TempPatchCleanerFiles`."

    ## Progress

    Use a checkbox list with timestamps. Every stopping point must appear here.

    - [x] (2026-04-02 15:10Z) Example completed step.
    - [ ] Example incomplete step.
    - [ ] Example partially completed step (completed: updated `patch_cleaner/ui/main_frame.cpp`; remaining: validate Debug x64 build and Windows Sandbox flow).

    ## Surprises & Discoveries

    Record unexpected behavior and include concise evidence.

    - Observation: `scripts/build-local.ps1` forced `Clean;Build` because stale manifest intermediates pointed at an outdated embed manifest path.
      Evidence: terminal output included "Detected stale build intermediates for Debug|x64. Forcing Clean;Build."

    ## Decision Log

    Record every important implementation or planning decision.

    - Decision: Keep the new UI command inside `patch_cleaner::ui::MainFrame` rather than moving it into a new window class.
      Rationale: The current command routing, button enablement, and repaint logic already live in `patch_cleaner/ui/main_frame.cpp`, so adding a second window owner would make the plan harder for a novice to follow.
      Date/Author: 2026-04-02 / <name>

    ## Outcomes & Retrospective

    Summarize what now works, what still does not, and what was learned. Compare the outcome to the purpose stated above.

    ## Context and Orientation

    Describe the relevant repo state for someone new to PatchCleaner. Name the exact files they will touch. Define terms immediately. Example: "WTL is the Windows Template Library used by this repo to build the main frame window in `patch_cleaner/ui/main_frame.h` and `patch_cleaner/ui/main_frame.cpp`."

    ## Plan of Work

    Describe the edits in prose, in the order they will happen. Name files and functions precisely. Example: "In `patch_cleaner/ui/main_frame.h`, add the new command ID wiring and any helper declaration needed by `MainFrame`. In `patch_cleaner/ui/main_frame.cpp`, implement the command handler and update button enablement, status text, and list refresh behavior. In `patch_cleaner/res/resource.h` and `patch_cleaner/res/resource.rc`, add any new menu or string resources. If a new file is introduced, update `patch_cleaner/PatchCleaner.vcxproj` explicitly."

    ## Concrete Steps

    State the exact commands to run and the working directory. Update this section as work proceeds. Include short expected transcripts.

        Working directory: C:\Users\Bob\Documents\PatchCleaner-master

        powershell -NoLogo -ExecutionPolicy Bypass -File .\scripts\build-local.ps1 -Configuration Debug -Platform x64

        Expected evidence:
        MSBuild: C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe
        Project: C:\Users\Bob\Documents\PatchCleaner-master\patch_cleaner\PatchCleaner.vcxproj
        Target: Build (Debug|x64)

        If the change touches high-risk behavior:
        powershell -NoLogo -ExecutionPolicy Bypass -File .\scripts\prepare-sandbox-bundle.ps1 -Launch

    ## Validation and Acceptance

    Describe how to prove the change works. Phrase acceptance as behavior, not only as code structure. Example: "After building Debug x64 and launching PatchCleaner, click Scan, confirm the file list populates, sort by Size, click Select All, then click Move to Temp. Expect the status copy and totals to update, and expect moved files to appear under `C:\TempPatchCleanerFiles`. If a collision file already exists there, expect the moved file to use the documented tokenized fallback name instead of failing."

    If the plan changes elevation, startup, or long-running behavior, say how to observe both the non-elevated path and the admin relaunch path. If the plan changes scan correctness or destructive actions, include the Release x64 and Windows Sandbox validation path unless the plan explains a safer, narrower proof.

    ## Idempotence and Recovery

    Explain which steps are safe to repeat. Include retry guidance for interrupted builds, partially created temp files, or sandbox setup failures. Example: "The build commands are safe to rerun. If `scripts/build-local.ps1` reports stale intermediates, rerun with `-Clean` or allow the script to force `Clean;Build`. If a sandbox bundle already exists, `scripts/prepare-sandbox-bundle.ps1` creates a timestamped run directory rather than overwriting an older bundle."

    ## Artifacts and Notes

    Include the most important transcript snippets, short diffs, or evidence samples as indented text. Keep them short and directly tied to proof.

    ## Interfaces and Dependencies

    Be precise about the code surface that must exist after implementation. Use real PatchCleaner types and functions where possible. Examples:

        In `patch_cleaner/app/application.h`, `patch_cleaner::app::Application` owns startup and message-loop behavior through `ParseCommandLine`, `PreMessageLoop`, `RunMessageLoop`, and `PostMessageLoop`.

        In `patch_cleaner/ui/main_frame.h`, `patch_cleaner::ui::MainFrame` owns user commands such as `OnFileUpdate`, `OnEditSelectAll`, `OnFileMoveToTemp`, and `OnEditDelete`, along with helpers such as `SetBusyOperation`, `RefreshChrome`, and `RecalculateSelectionTotals`.

        If the plan adds a command, say which `COMMAND_ID_HANDLER_EX(...)` entry is required, which resource identifier must be added to `patch_cleaner/res/resource.h`, and where the menu, toolbar, or string change must be made in `patch_cleaner/res/resource.rc`.

        If the plan adds a new source or header, say how `patch_cleaner/PatchCleaner.vcxproj` must be updated so the file is compiled in every intended configuration.

If you follow this file closely, a single stateless coding agent or a human novice can read the plan from top to bottom and deliver a working PatchCleaner change with observable proof. That is the standard for this repository: self-contained, repo-specific, novice-guiding, and outcome-focused.
