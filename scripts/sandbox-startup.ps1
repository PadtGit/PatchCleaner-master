param(
  [Parameter(Mandatory = $true)]
  [string]$BundleRoot,

  [Parameter(Mandatory = $true)]
  [string]$ResultsRoot,

  [string]$SessionId = (Get-Date -Format "yyyyMMdd-HHmmss"),

  [switch]$SeedInitialOnly,

  [switch]$SeedDeleteOnly
)

$ErrorActionPreference = "Stop"

$script:InstallerDirectory = Join-Path $env:windir "Installer"
$script:TempMoveDirectory = "C:\TempPatchCleanerFiles"
$script:MoveTargetName = "PatchCleaner-Sandbox-Move.msi"
$script:CollisionTargetName = "PatchCleaner-Sandbox-Collision.msi"
$script:LateDeleteTargetName = "PatchCleaner-Sandbox-Delete.msp"
$script:FillerCount = 64
$script:SessionLogPath = $null
$script:SeedManifestPath = $null
$script:SummaryPath = $null
$script:ChecklistPath = $null
$script:CheckResults = @()
$script:AutomationDriverPath = $null
$script:AutomationRuntimePath = $null

function Test-IsAdministrator {
  $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
  $principal = New-Object Security.Principal.WindowsPrincipal($identity)
  return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Write-SessionLog {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Message
  )

  $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
  $line = "[{0}] {1}" -f $timestamp, $Message
  Write-Host $line
  Add-Content -LiteralPath $script:SessionLogPath -Value $line
}

function Ensure-Directory {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  New-Item -ItemType Directory -Force -Path $Path | Out-Null
}

function New-SizedFile {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [long]$SizeBytes
  )

  Ensure-Directory -Path (Split-Path -Parent $Path)
  $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create,
                                   [System.IO.FileAccess]::Write,
                                   [System.IO.FileShare]::None)
  try {
    $stream.SetLength($SizeBytes)
  } finally {
    $stream.Dispose()
  }
}

function Get-InitialSeedDefinitions {
  $definitions = @(
    [ordered]@{
      name = $script:MoveTargetName
      size = 262144
      role = "move"
    },
    [ordered]@{
      name = $script:CollisionTargetName
      size = 393216
      role = "collision"
    }
  )

  for ($index = 1; $index -le $script:FillerCount; ++$index) {
    $extension = if (($index % 2) -eq 0) { ".msi" } else { ".msp" }
    $definitions += [ordered]@{
      name = ("PatchCleaner-Sandbox-Filler-{0:D2}{1}" -f $index, $extension)
      size = 131072 + ($index * 4096)
      role = "filler"
    }
  }

  return $definitions
}

function Write-SeedManifest {
  param(
    [Parameter(Mandatory = $true)]
    [object]$Manifest
  )

  $Manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $script:SeedManifestPath -Encoding UTF8
}

function Get-SeedManifest {
  if (-not (Test-Path $script:SeedManifestPath)) {
    throw "Seed manifest not found: $($script:SeedManifestPath)"
  }

  return Get-Content -LiteralPath $script:SeedManifestPath -Raw | ConvertFrom-Json
}

function Seed-InitialFiles {
  foreach ($definition in (Get-InitialSeedDefinitions)) {
    $path = Join-Path $script:InstallerDirectory $definition.name
    if (Test-Path $path) {
      Remove-Item -LiteralPath $path -Force
    }
    New-SizedFile -Path $path -SizeBytes $definition.size
  }

  Ensure-Directory -Path $script:TempMoveDirectory
  $collisionSeedPath = Join-Path $script:TempMoveDirectory $script:CollisionTargetName
  New-SizedFile -Path $collisionSeedPath -SizeBytes 65536

  $manifest = [ordered]@{
    session_id = $SessionId
    created_at = (Get-Date).ToString("o")
    installer_directory = $script:InstallerDirectory
    temp_directory = $script:TempMoveDirectory
    initial_files = @((Get-InitialSeedDefinitions))
    collision_seed_path = $collisionSeedPath
    late_delete_target = $script:LateDeleteTargetName
  }
  Write-SeedManifest -Manifest $manifest
  Write-SessionLog ("Seeded {0} initial orphan installer-cache files." -f $manifest.initial_files.Count)
}

function Seed-LateDeleteFile {
  $path = Join-Path $script:InstallerDirectory $script:LateDeleteTargetName
  if (Test-Path $path) {
    Remove-Item -LiteralPath $path -Force
  }

  New-SizedFile -Path $path -SizeBytes 196608
  Write-SessionLog "Seeded fresh delete target: $path"
}

function Invoke-ElevatedSeeder {
  param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("initial", "delete")]
    [string]$Mode
  )

  $scriptPath = $MyInvocation.ScriptName
  if ([string]::IsNullOrWhiteSpace($scriptPath)) {
    $scriptPath = $PSCommandPath
  }
  if ([string]::IsNullOrWhiteSpace($scriptPath)) {
    throw "Could not resolve the sandbox startup script path."
  }

  $switchName = if ($Mode -eq "initial") { "-SeedInitialOnly" } else { "-SeedDeleteOnly" }
  $arguments = @(
    "-NoLogo",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$scriptPath`"",
    "-BundleRoot", "`"$BundleRoot`"",
    "-ResultsRoot", "`"$ResultsRoot`"",
    "-SessionId", $SessionId,
    $switchName
  )

  Write-SessionLog "Requesting elevation for $Mode seeding."
  $process = Start-Process -FilePath (Join-Path $PSHOME "powershell.exe") `
                           -ArgumentList $arguments `
                           -Verb RunAs `
                           -Wait `
                           -PassThru
  if ($process.ExitCode -ne 0) {
    throw "Elevated $Mode seeding failed with exit code $($process.ExitCode)."
  }
}

function Start-PatchCleaner {
  $applicationPath = Join-Path $BundleRoot "PatchCleaner.exe"
  if (-not (Test-Path $applicationPath)) {
    throw "PatchCleaner payload not found: $applicationPath"
  }

  try {
    $shell = New-Object -ComObject Shell.Application
    $shell.ShellExecute($applicationPath, "", (Split-Path -Parent $applicationPath), "open", 1)
    Write-SessionLog "Launched PatchCleaner through Shell.Application."
    return
  } catch {
    Write-SessionLog "Shell.Application launch failed; falling back to Start-Process."
  }

  Start-Process -FilePath $applicationPath | Out-Null
  Write-SessionLog "Launched PatchCleaner through Start-Process."
}

function Write-Checklist {
  param(
    [Parameter(Mandatory = $true)]
    [object]$SeedManifest
  )

  $lines = @(
    "PatchCleaner Windows Sandbox Checklist",
    "",
    "Session: $SessionId",
    "Results folder: $ResultsRoot",
    "",
    "Initial seeded files:",
    "  Move target: $($script:MoveTargetName)",
    "  Collision target: $($script:CollisionTargetName)",
    "  Filler count: $($SeedManifest.initial_files.Count - 2)",
    "  Fresh delete target added later: $($script:LateDeleteTargetName)",
    "",
    "Manual flow:",
    "1. In PatchCleaner, click Scan and accept the relaunch prompt if shown.",
    "2. Confirm the seeded files appear, try sorting by both columns, then click Select All.",
    "3. Click Move to Temp on the initial seeded set and move or resize the window while it runs.",
    "4. After the script seeds a fresh delete target, scan again and delete it while moving or resizing the window.",
    "",
    "The PowerShell window records the pass/fail prompts and writes the results back to the mapped host folder."
  )

  Set-Content -LiteralPath $script:ChecklistPath -Value $lines -Encoding UTF8
}

function Wait-ForTester {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Prompt
  )

  Write-Host ""
  Write-Host $Prompt
  [void](Read-Host "Press Enter when ready")
}

function Read-YesNo {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Prompt
  )

  for (;;) {
    $response = (Read-Host "$Prompt [y/n]").Trim().ToLowerInvariant()
    if ($response -eq "y" -or $response -eq "yes") {
      return $true
    }
    if ($response -eq "n" -or $response -eq "no") {
      return $false
    }
  }
}

function ConvertFrom-KeyValueFile {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  $data = @{}
  foreach ($line in Get-Content -LiteralPath $Path) {
    if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith("#")) {
      continue
    }

    $separator = $line.IndexOf("=")
    if ($separator -lt 0) {
      continue
    }

    $name = $line.Substring(0, $separator).Trim()
    $value = $line.Substring($separator + 1).Trim()
    $data[$name] = $value
  }

  return $data
}

function Get-AutomationResultPath {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Phase
  )

  return Join-Path $ResultsRoot ("automation-{0}-{1}.txt" -f $Phase, $SessionId)
}

function ConvertTo-BoolFlag {
  param(
    [AllowNull()]
    [object]$Value
  )

  if ($null -eq $Value) {
    return $false
  }

  $text = [string]$Value
  return $text -eq "1" -or $text.Equals("true", [System.StringComparison]::OrdinalIgnoreCase)
}

function Invoke-AutomationDriver {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Phase,

    [int]$TimeoutSeconds = 240
  )

  if (-not (Test-Path $script:AutomationRuntimePath) -or -not (Test-Path $script:AutomationDriverPath)) {
    Write-SessionLog "Automation runtime or driver is unavailable; falling back to manual flow."
    return $null
  }

  $resultPath = Get-AutomationResultPath -Phase $Phase
  if (Test-Path $resultPath) {
    Remove-Item -LiteralPath $resultPath -Force
  }

  $argumentList = @(
    "`"$script:AutomationDriverPath`"",
    "`"$ResultsRoot`"",
    "`"$SessionId`"",
    "`"$Phase`"",
    "`"$script:InstallerDirectory`"",
    "`"$script:TempMoveDirectory`"",
    "`"$script:MoveTargetName`"",
    "`"$script:CollisionTargetName`"",
    "`"$script:LateDeleteTargetName`"",
    66
  )

  Write-SessionLog "Starting AutoHotkey automation for phase '$Phase'."
  $process = Start-Process -FilePath $script:AutomationRuntimePath `
                           -ArgumentList $argumentList `
                           -PassThru

  if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
    try {
      $process.Kill()
    } catch {
    }
    Write-SessionLog "Automation for phase '$Phase' timed out."
  }

  if (-not (Test-Path $resultPath)) {
    Write-SessionLog "Automation for phase '$Phase' did not produce a results file."
    return $null
  }

  $result = ConvertFrom-KeyValueFile -Path $resultPath
  $result["phase"] = $Phase
  $result["exit_code"] = if ($process.HasExited) { [string]$process.ExitCode } else { "timed_out" }
  return $result
}

function Add-CheckResult {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Name,

    [Parameter(Mandatory = $true)]
    [bool]$ManualPassed,

    [AllowNull()]
    [Nullable[bool]]$AutoPassed,

    [string]$Details = ""
  )

  $passed = $ManualPassed
  if ($AutoPassed.HasValue) {
    $passed = $passed -and $AutoPassed.Value
  }

  $result = [ordered]@{
    name = $Name
    manual_passed = $ManualPassed
    auto_passed = if ($AutoPassed.HasValue) { $AutoPassed.Value } else { $null }
    passed = $passed
    details = $Details
  }

  $script:CheckResults += [pscustomobject]$result
  Write-SessionLog ("Check '{0}' => {1}. {2}" -f $Name, $(if ($passed) { "PASS" } else { "FAIL" }), $Details)
}

function Test-MoveOutcome {
  $remainingSeeded = @(
    Get-ChildItem -LiteralPath $script:InstallerDirectory -File -Filter "PatchCleaner-Sandbox-*.*" -ErrorAction SilentlyContinue
  )
  $normalMoved = Test-Path (Join-Path $script:TempMoveDirectory $script:MoveTargetName)
  $collisionMatch = @(
    Get-ChildItem -LiteralPath $script:TempMoveDirectory -File -Filter "PatchCleaner-Sandbox-Collision*.msi" -ErrorAction SilentlyContinue |
      Where-Object {
        $_.Name -match "^PatchCleaner-Sandbox-Collision \[[^\]]+\]( \(\d+\))?\.msi$"
      }
  )

  $passed = ($remainingSeeded.Count -eq 0) -and $normalMoved -and ($collisionMatch.Count -ge 1)
  $details = "remaining_seeded={0}; normal_moved={1}; tokenized_collision_matches={2}" -f `
    $remainingSeeded.Count, $normalMoved, $collisionMatch.Count

  return [pscustomobject]@{
    passed = $passed
    normal_moved = $normalMoved
    tokenized_collision = ($collisionMatch.Count -ge 1)
    details = $details
  }
}

function Test-DeleteOutcome {
  $installerPath = Join-Path $script:InstallerDirectory $script:LateDeleteTargetName
  $tempCopies = @(
    Get-ChildItem -LiteralPath $script:TempMoveDirectory -File -Filter "PatchCleaner-Sandbox-Delete*.msp" -ErrorAction SilentlyContinue
  )

  $passed = (-not (Test-Path $installerPath)) -and ($tempCopies.Count -eq 0)
  $details = "installer_exists={0}; temp_delete_copies={1}" -f `
    (Test-Path $installerPath), $tempCopies.Count

  return [pscustomobject]@{
    passed = $passed
    details = $details
  }
}

try {
  Ensure-Directory -Path $ResultsRoot

  $script:SessionLogPath = Join-Path $ResultsRoot ("sandbox-session-{0}.log" -f $SessionId)
  $script:SeedManifestPath = Join-Path $ResultsRoot ("seed-manifest-{0}.json" -f $SessionId)
  $script:SummaryPath = Join-Path $ResultsRoot ("sandbox-summary-{0}.json" -f $SessionId)
  $script:ChecklistPath = Join-Path $ResultsRoot ("sandbox-checklist-{0}.txt" -f $SessionId)
  $script:AutomationDriverPath = Join-Path $BundleRoot "sandbox-driver.ahk"
  $script:AutomationRuntimePath = Join-Path $BundleRoot "AutoHotkey64_UIA.exe"

  Set-Content -LiteralPath $script:SessionLogPath -Value @("PatchCleaner Windows Sandbox session $SessionId") -Encoding UTF8

  if ($SeedInitialOnly -and $SeedDeleteOnly) {
    throw "SeedInitialOnly and SeedDeleteOnly cannot be used together."
  }

  if ($SeedInitialOnly) {
    if (-not (Test-IsAdministrator)) {
      throw "Initial seeding requires elevation."
    }

    Seed-InitialFiles
    exit 0
  }

  if ($SeedDeleteOnly) {
    if (-not (Test-IsAdministrator)) {
      throw "Delete-target seeding requires elevation."
    }

    Seed-LateDeleteFile
    exit 0
  }

  if (Test-IsAdministrator) {
    Write-SessionLog "Main sandbox session is already elevated; seeding directly before launching PatchCleaner."
    Seed-InitialFiles
  } else {
    Invoke-ElevatedSeeder -Mode initial
  }

  $seedManifest = Get-SeedManifest
  Write-Checklist -SeedManifest $seedManifest
  Start-Process -FilePath "notepad.exe" -ArgumentList "`"$script:ChecklistPath`"" | Out-Null

  Start-PatchCleaner

  $initialAutomation = Invoke-AutomationDriver -Phase initial
  $moveOutcome = Test-MoveOutcome
  if ($null -ne $initialAutomation) {
    Add-CheckResult -Name "admin_relaunch" `
                    -ManualPassed (ConvertTo-BoolFlag $initialAutomation["relaunch_window_found"]) `
                    -AutoPassed (ConvertTo-BoolFlag $initialAutomation["relaunch_prompt_accepted"]) `
                    -Details ("prompt_seen={0}; prompt_accepted={1}; relaunch_window_found={2}; note={3}" -f `
                        $initialAutomation["relaunch_prompt_seen"], `
                        $initialAutomation["relaunch_prompt_accepted"], `
                        $initialAutomation["relaunch_window_found"], `
                        $initialAutomation["note"])

    Add-CheckResult -Name "scan_lists_seeded_files" `
                    -ManualPassed (([int]($initialAutomation["post_scan_item_count"] | ForEach-Object { $_ }) ) -ge $seedManifest.initial_files.Count) `
                    -AutoPassed $null `
                    -Details ("post_scan_item_count={0}; expected_at_least={1}" -f `
                        $initialAutomation["post_scan_item_count"], $seedManifest.initial_files.Count)

    Add-CheckResult -Name "sorting_and_select_all" `
                    -ManualPassed ((ConvertTo-BoolFlag $initialAutomation["sort_interactions_sent"]) -and `
                                   (ConvertTo-BoolFlag $initialAutomation["select_all_sent"])) `
                    -AutoPassed $null `
                    -Details ("sort_interactions_sent={0}; select_all_sent={1}" -f `
                        $initialAutomation["sort_interactions_sent"], $initialAutomation["select_all_sent"])

    Add-CheckResult -Name "move_to_temp_and_responsiveness" `
                    -ManualPassed ((ConvertTo-BoolFlag $initialAutomation["move_completed"]) -and `
                                   (([int]($initialAutomation["responsiveness_moves"] | ForEach-Object { $_ })) -gt 0)) `
                    -AutoPassed $moveOutcome.passed `
                    -Details ("automation_move_completed={0}; responsiveness_moves={1}; {2}" -f `
                        $initialAutomation["move_completed"], $initialAutomation["responsiveness_moves"], $moveOutcome.details)

    Add-CheckResult -Name "collision_name_fallback" `
                    -ManualPassed (ConvertTo-BoolFlag $initialAutomation["collision_tokenized_name_found"]) `
                    -AutoPassed $moveOutcome.tokenized_collision `
                    -Details ("automation_collision_tokenized_name_found={0}; {1}" -f `
                        $initialAutomation["collision_tokenized_name_found"], $moveOutcome.details)
  } else {
    Wait-ForTester "In PatchCleaner, click Scan, accept the relaunch prompt if it appears, wait for the seeded files to appear, try sorting by both columns, then click Select All."

    Add-CheckResult -Name "admin_relaunch" `
                    -ManualPassed (Read-YesNo "Did PatchCleaner launch non-elevated, prompt for elevation on Scan, and reopen successfully") `
                    -AutoPassed $null `
                    -Details "Manual confirmation of the non-elevated launch and relaunch path."

    Add-CheckResult -Name "scan_lists_seeded_files" `
                    -ManualPassed (Read-YesNo "Did the scan list the seeded orphan files, including the move and collision targets") `
                    -AutoPassed $null `
                    -Details ("Expected at least {0} initial seeded files with the PatchCleaner-Sandbox-* prefix." -f $seedManifest.initial_files.Count)

    Add-CheckResult -Name "sorting_and_select_all" `
                    -ManualPassed (Read-YesNo "Did sorting by Name and Size work, and did Select All update counts and sizes correctly") `
                    -AutoPassed $null `
                    -Details "Manual confirmation of list sorting and action-rail totals."

    Wait-ForTester "With the initial seeded files selected, click Move to Temp. While it runs, move or resize the window to confirm the UI keeps repainting and shows the busy-state text."

    Add-CheckResult -Name "move_to_temp_and_responsiveness" `
                    -ManualPassed (Read-YesNo "Did Move to Temp complete and keep the UI responsive while actions were paused") `
                    -AutoPassed $moveOutcome.passed `
                    -Details $moveOutcome.details

    Add-CheckResult -Name "collision_name_fallback" `
                    -ManualPassed (Read-YesNo "Did the collision file land in C:\TempPatchCleanerFiles with a tokenized [batch-token] fallback name") `
                    -AutoPassed $moveOutcome.tokenized_collision `
                    -Details $moveOutcome.details
  }

  if (Test-IsAdministrator) {
    Seed-LateDeleteFile
  } else {
    Invoke-ElevatedSeeder -Mode delete
  }

  $deleteAutomation = Invoke-AutomationDriver -Phase delete
  $deleteOutcome = Test-DeleteOutcome
  if ($null -ne $deleteAutomation) {
    Add-CheckResult -Name "fresh_delete_target_visible" `
                    -ManualPassed (([int]($deleteAutomation["delete_scan_item_count"] | ForEach-Object { $_ })) -ge 1) `
                    -AutoPassed $null `
                    -Details ("delete_scan_item_count={0}; note={1}" -f `
                        $deleteAutomation["delete_scan_item_count"], $deleteAutomation["note"])

    Add-CheckResult -Name "delete_and_responsiveness" `
                    -ManualPassed ((ConvertTo-BoolFlag $deleteAutomation["delete_completed"]) -and `
                                   (([int]($deleteAutomation["responsiveness_moves"] | ForEach-Object { $_ })) -gt 0)) `
                    -AutoPassed $deleteOutcome.passed `
                    -Details ("automation_delete_completed={0}; responsiveness_moves={1}; {2}" -f `
                        $deleteAutomation["delete_completed"], $deleteAutomation["responsiveness_moves"], $deleteOutcome.details)
  } else {
    Wait-ForTester ("A fresh delete target ({0}) has been seeded. Click Scan again, confirm it appears, then delete it while moving or resizing the window." -f $script:LateDeleteTargetName)

    Add-CheckResult -Name "fresh_delete_target_visible" `
                    -ManualPassed (Read-YesNo ("Did the rescan show the fresh delete target {0}" -f $script:LateDeleteTargetName)) `
                    -AutoPassed $null `
                    -Details "Manual confirmation of the late delete-stage scan result."

    Add-CheckResult -Name "delete_and_responsiveness" `
                    -ManualPassed (Read-YesNo "Did Delete remove the fresh target and keep the UI responsive while actions were paused") `
                    -AutoPassed $deleteOutcome.passed `
                    -Details $deleteOutcome.details
  }

  $notes = Read-Host "Optional notes for the sandbox results log (press Enter to skip)"
  if (-not [string]::IsNullOrWhiteSpace($notes)) {
    Write-SessionLog "Tester notes: $notes"
  }

  $allPassed = ($script:CheckResults | Where-Object { -not $_.passed }).Count -eq 0
  $summary = [ordered]@{
    session_id = $SessionId
    completed_at = (Get-Date).ToString("o")
    bundle_root = $BundleRoot
    results_root = $ResultsRoot
    checklist_path = $script:ChecklistPath
    passed = $allPassed
    checks = $script:CheckResults
    notes = $notes
  }
  $summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $script:SummaryPath -Encoding UTF8

  Write-SessionLog "Wrote sandbox summary to $($script:SummaryPath)"
  if ($allPassed) {
    Write-SessionLog "Sandbox validation completed successfully."
    exit 0
  }

  Write-SessionLog "Sandbox validation completed with failures."
  exit 1
} catch {
  if ($script:SessionLogPath) {
    Write-SessionLog ("Sandbox validation failed: {0}" -f $_.Exception.Message)
  } else {
    Write-Host $_.Exception.Message
  }

  [void](Read-Host "Press Enter to close this window")
  exit 1
}
