param(
  [ValidateSet("Debug", "Release")]
  [string]$Configuration = "Debug",

  [ValidateSet("x64", "Win32")]
  [string]$Platform = "x64",

  [switch]$Clean
)

$ErrorActionPreference = "Stop"

function Get-HostPowerShellPath {
  $command = Get-Command powershell.exe -ErrorAction SilentlyContinue
  if ($null -ne $command) {
    return $command.Source
  }

  throw "powershell.exe was not found in PATH."
}

function Get-RollingMedian {
  param(
    [Parameter(Mandatory = $true)]
    [double[]]$Values
  )

  if ($Values.Count -eq 0) {
    return $null
  }

  $sorted = $Values | Sort-Object
  $middle = [int]($sorted.Count / 2)
  if (($sorted.Count % 2) -eq 1) {
    return [double]$sorted[$middle]
  }

  return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildRoot = Join-Path $repoRoot "Build"
$validationSummaryPath = Join-Path $buildRoot "validation-summary.json"
$perfHistoryPath = Join-Path $buildRoot "perf-build-history.json"
$buildScriptPath = Join-Path $PSScriptRoot "build-local.ps1"

if (-not (Test-Path $buildRoot)) {
  New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
}

$buildArgs = @(
  "-NoProfile"
  "-ExecutionPolicy"
  "Bypass"
  "-File"
  $buildScriptPath
  "-Configuration"
  $Configuration
  "-Platform"
  $Platform
)

if ($Clean) {
  $buildArgs += "-Clean"
}

$startTimeUtc = [DateTime]::UtcNow
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
$process = Start-Process -FilePath (Get-HostPowerShellPath) `
  -ArgumentList $buildArgs `
  -WorkingDirectory $repoRoot `
  -NoNewWindow `
  -Wait `
  -PassThru
$stopwatch.Stop()

$durationMs = [Math]::Round($stopwatch.Elapsed.TotalMilliseconds, 2)
$status = if ($process.ExitCode -eq 0) { "passed" } else { "failed" }

$history = @()
if (Test-Path $perfHistoryPath) {
  $rawHistory = Get-Content $perfHistoryPath -Raw
  if (-not [string]::IsNullOrWhiteSpace($rawHistory)) {
    $parsedHistory = ConvertFrom-Json $rawHistory
    if ($parsedHistory -is [System.Collections.IEnumerable]) {
      $history = @($parsedHistory)
    }
  }
}

$sample = [ordered]@{
  timestamp_utc = $startTimeUtc.ToString("o")
  configuration = $Configuration
  platform = $Platform
  clean = [bool]$Clean
  status = $status
  exit_code = $process.ExitCode
  duration_ms = $durationMs
}

$matchingSuccesses = @(
  $history | Where-Object {
    $_.status -eq "passed" -and
    $_.configuration -eq $Configuration -and
    $_.platform -eq $Platform -and
    [bool]$_.clean -eq [bool]$Clean
  } | Select-Object -Last 7
)

$baselineMedian = $null
$deltaPercent = $null
$regressionDetected = $false
$regressionThresholdPercent = 20.0

if ($matchingSuccesses.Count -ge 3) {
  $baselineMedian = Get-RollingMedian -Values @($matchingSuccesses | ForEach-Object { [double]$_.duration_ms })
  if ($null -ne $baselineMedian -and $baselineMedian -gt 0) {
    $deltaPercent = [Math]::Round((($durationMs - $baselineMedian) / $baselineMedian) * 100.0, 2)
    $regressionDetected = $status -eq "passed" -and $deltaPercent -ge $regressionThresholdPercent
  }
}

$updatedHistory = @($history + [pscustomobject]$sample | Select-Object -Last 30)
$updatedHistory | ConvertTo-Json -Depth 4 | Set-Content -Path $perfHistoryPath

$summary = [ordered]@{
  generated_at_utc = [DateTime]::UtcNow.ToString("o")
  repo_root = $repoRoot
  host_validation = [ordered]@{
    command = ".\\scripts\\test-local.ps1 -Configuration $Configuration -Platform $Platform" + ($(if ($Clean) { " -Clean" } else { "" }))
    source_of_truth = "Build/validation-summary.json"
    status = $status
    exit_code = $process.ExitCode
    duration_ms = $durationMs
    configuration = $Configuration
    platform = $Platform
  }
  measurements = [ordered]@{
    build_wall_clock_ms = $durationMs
    baseline_sample_count = $matchingSuccesses.Count
    baseline_median_ms = $baselineMedian
    delta_percent_vs_median = $deltaPercent
    regression_threshold_percent = $regressionThresholdPercent
    regression_detected = $regressionDetected
    signal = "host_build_wall_clock"
    note = "This is a build-time signal only; runtime UI scan/move/delete timings are not automated yet."
  }
}

$summary | ConvertTo-Json -Depth 6 | Set-Content -Path $validationSummaryPath

exit $process.ExitCode
