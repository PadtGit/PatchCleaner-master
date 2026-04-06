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

function New-SanitizedProcessStartInfo {
  param(
    [Parameter(Mandatory = $true)]
    [string]$FileName,

    [Parameter(Mandatory = $true)]
    [string]$Arguments,

    [Parameter(Mandatory = $true)]
    [string]$WorkingDirectory
  )

  $psi = New-Object System.Diagnostics.ProcessStartInfo
  [void]($psi.FileName = $FileName)
  [void]($psi.Arguments = $Arguments)
  [void]($psi.WorkingDirectory = $WorkingDirectory)
  [void]($psi.UseShellExecute = $false)
  [void]($psi.RedirectStandardOutput = $true)
  [void]($psi.RedirectStandardError = $true)
  [void]($psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8)
  [void]($psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8)

  $envMap = [System.Collections.Generic.Dictionary[string, string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase
  )
  Get-ChildItem Env: | ForEach-Object {
    [void]($envMap[$_.Name] = [string]$_.Value)
  }

  $pathValue = $null
  if ($envMap.ContainsKey("Path")) {
    $pathValue = $envMap["Path"]
    [void]$envMap.Remove("Path")
  }
  if ($envMap.ContainsKey("PATH")) {
    if ($null -eq $pathValue) {
      $pathValue = $envMap["PATH"]
    }
    [void]$envMap.Remove("PATH")
  }

  $psi.Environment.Clear()
  foreach ($pair in $envMap.GetEnumerator()) {
    [void]($psi.Environment[$pair.Key] = $pair.Value)
  }
  if ($null -ne $pathValue) {
    [void]($psi.Environment["Path"] = $pathValue)
  }

  return $psi
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

function Convert-MSBuildElapsedToMilliseconds {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Text
  )

  $match = [regex]::Match($Text, 'Time Elapsed\s+(?<hours>\d+):(?<minutes>\d{2}):(?<seconds>\d{2})(?:\.(?<fraction>\d+))?')
  if (-not $match.Success) {
    return $null
  }

  $hours = [int]$match.Groups["hours"].Value
  $minutes = [int]$match.Groups["minutes"].Value
  $seconds = [int]$match.Groups["seconds"].Value
  $fractionText = $match.Groups["fraction"].Value
  $milliseconds = 0.0
  if (-not [string]::IsNullOrWhiteSpace($fractionText)) {
    $milliseconds = [double]("0." + $fractionText) * 1000.0
  }

  return [Math]::Round((((($hours * 60) + $minutes) * 60) + $seconds) * 1000.0 + $milliseconds, 2)
}

function Get-PerformanceSummaryEntries {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Text,

    [Parameter(Mandatory = $true)]
    [string]$SectionHeader
  )

  $entries = @()
  $capture = $false
  foreach ($line in ($Text -split "`r?`n")) {
    if ($capture) {
      if ([string]::IsNullOrWhiteSpace($line)) {
        break
      }

      $match = [regex]::Match($line, '^\s*(?<duration>\d+)\s+ms\s+(?<name>.+?)\s+(?<calls>\d+)\s+calls$')
      if ($match.Success) {
        $entries += [pscustomobject]@{
          name = $match.Groups["name"].Value.Trim()
          duration_ms = [double]$match.Groups["duration"].Value
          calls = [int]$match.Groups["calls"].Value
        }
      }

      continue
    }

    if ($line.Trim() -eq $SectionHeader.Trim()) {
      $capture = $true
    }
  }

  return @($entries)
}

function Get-RelativeRepoPath {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,

    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  $resolvedRepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)
  $resolvedPath = [System.IO.Path]::GetFullPath($Path)
  if ($resolvedPath.StartsWith($resolvedRepoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    return $resolvedPath.Substring($resolvedRepoRoot.Length).TrimStart('\')
  }

  return $resolvedPath
}

function Get-SignalSummary {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Signal,

    [Parameter(Mandatory = $true)]
    [double]$CurrentValue,

    [AllowNull()]
    [double[]]$HistoryValues,

    [Parameter(Mandatory = $true)]
    [double]$RegressionThresholdPercent
  )

  $baselineMedian = $null
  $deltaPercent = $null
  $regressionDetected = $false

  if ($null -eq $HistoryValues) {
    $HistoryValues = @()
  }

  if ($HistoryValues.Count -ge 3) {
    $baselineMedian = Get-RollingMedian -Values $HistoryValues
    if ($null -ne $baselineMedian -and $baselineMedian -gt 0) {
      $deltaPercent = [Math]::Round((($CurrentValue - $baselineMedian) / $baselineMedian) * 100.0, 2)
      $regressionDetected = $deltaPercent -ge $RegressionThresholdPercent
    }
  }

  return [ordered]@{
    signal = $Signal
    current_value_ms = $CurrentValue
    baseline_sample_count = $HistoryValues.Count
    baseline_median_ms = $baselineMedian
    delta_percent_vs_median = $deltaPercent
    regression_threshold_percent = $RegressionThresholdPercent
    regression_detected = $regressionDetected
  }
}

function Get-HistorySignalValues {
  param(
    [Parameter(Mandatory = $true)]
    [object[]]$History,

    [Parameter(Mandatory = $true)]
    [string]$Configuration,

    [Parameter(Mandatory = $true)]
    [string]$Platform,

    [Parameter(Mandatory = $true)]
    [bool]$Clean,

    [Parameter(Mandatory = $true)]
    [string]$SignalKind,

    [string]$SignalName
  )

  $values = @()
  $matchingRuns = @(
    $History | Where-Object {
      $_.status -eq "passed" -and
      $_.configuration -eq $Configuration -and
      $_.platform -eq $Platform -and
      [bool]$_.clean -eq $Clean
    } | Select-Object -Last 7
  )

  foreach ($run in $matchingRuns) {
    $value = $null

    if ($SignalKind -eq "primary") {
      if ($null -ne $run.measurements -and $null -ne $run.measurements.msbuild_reported_elapsed_ms) {
        $value = [double]$run.measurements.msbuild_reported_elapsed_ms
      } elseif ($null -ne $run.duration_ms) {
        $value = [double]$run.duration_ms
      }
    } elseif ($null -ne $run.sections) {
      $sectionEntries = $run.sections.$SignalKind
      if ($null -ne $sectionEntries) {
        $entry = @($sectionEntries | Where-Object { $_.name -eq $SignalName } | Select-Object -First 1)
        if ($entry.Count -gt 0) {
          $value = [double]$entry[0].duration_ms
        }
      }
    }

    if ($null -ne $value) {
      $values += $value
    }
  }

  return [double[]]@($values)
}

function Get-OutputFileMetadata {
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$Paths,

    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
  )

  $results = @()
  foreach ($path in $Paths) {
    if (-not (Test-Path $path)) {
      continue
    }

    $item = Get-Item $path
    $results += [ordered]@{
      path = Get-RelativeRepoPath -RepoRoot $RepoRoot -Path $path
      size_bytes = [int64]$item.Length
      last_write_utc = $item.LastWriteTimeUtc.ToString("o")
    }
  }

  return @($results)
}

function Get-PreferredPerformanceEntry {
  param(
    [Parameter(Mandatory = $true)]
    [object[]]$Entries,

    [string[]]$PreferredNames = @()
  )

  foreach ($preferredName in $PreferredNames) {
    $match = @($Entries | Where-Object { $_.name -eq $preferredName } | Select-Object -First 1)
    if ($match.Count -gt 0) {
      return $match[0]
    }
  }

  $nonZero = @($Entries | Where-Object { [double]$_.duration_ms -gt 0 } | Sort-Object duration_ms -Descending | Select-Object -First 1)
  if ($nonZero.Count -gt 0) {
    return $nonZero[0]
  }

  $first = @($Entries | Select-Object -First 1)
  if ($first.Count -gt 0) {
    return $first[0]
  }

  return $null
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildRoot = Join-Path $repoRoot "Build"
$validationSummaryPath = Join-Path $buildRoot "validation-summary.json"
$perfHistoryPath = Join-Path $buildRoot "perf-build-history.json"
$buildScriptPath = Join-Path $PSScriptRoot "build-local.ps1"
$artifactTag = "{0}-{1}{2}" -f $Configuration.ToLowerInvariant(), $Platform.ToLowerInvariant(), ($(if ($Clean) { "-clean" } else { "" }))
$buildLogPath = Join-Path $buildRoot "perf-build-output-$artifactTag.log"
$binaryLogPath = Join-Path $buildRoot "perf-msbuild-$artifactTag.binlog"
$timingsPath = Join-Path $buildRoot "perf-build-timings-$artifactTag.json"
$tracePath = Join-Path $buildRoot "perf-build-trace-$artifactTag.json"
$flamegraphPath = Join-Path $buildRoot "perf-build-flamegraph-$artifactTag.folded"
$regressionThresholdPercent = 20.0

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
  "-PerformanceSummary"
  "-BinaryLogPath"
  $binaryLogPath
)

if ($Clean) {
  $buildArgs += "-Clean"
}

$startTimeUtc = [DateTime]::UtcNow
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
$psi = New-SanitizedProcessStartInfo -FileName (Get-HostPowerShellPath) -Arguments ($buildArgs -join " ") -WorkingDirectory $repoRoot
$process = [System.Diagnostics.Process]::Start($psi)
if ($null -eq $process) {
  throw "Failed to start test-local child process."
}

$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()
$process.WaitForExit()
[System.Threading.Tasks.Task]::WaitAll(@($stdoutTask, $stderrTask))
$stopwatch.Stop()

$stdout = $stdoutTask.Result
$stderr = $stderrTask.Result

if (-not [string]::IsNullOrWhiteSpace($stdout)) {
  Write-Host $stdout.TrimEnd()
}
if (-not [string]::IsNullOrWhiteSpace($stderr)) {
  Write-Error $stderr.TrimEnd()
}

$combinedOutput = @($stdout.TrimEnd(), $stderr.TrimEnd()) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
$combinedOutput -join [Environment]::NewLine | Set-Content -Path $buildLogPath

$wrapperDurationMs = [Math]::Round($stopwatch.Elapsed.TotalMilliseconds, 2)
$msbuildReportedDurationMs = Convert-MSBuildElapsedToMilliseconds -Text $stdout
$primaryDurationMs = if ($null -ne $msbuildReportedDurationMs) { $msbuildReportedDurationMs } else { $wrapperDurationMs }
$durationSource = if ($null -ne $msbuildReportedDurationMs) { "msbuild_reported_elapsed" } else { "wrapper_wall_clock" }
$status = if ($process.ExitCode -eq 0) { "passed" } else { "failed" }

$projectEvaluationEntries = Get-PerformanceSummaryEntries -Text $stdout -SectionHeader "Project Evaluation Performance Summary:"
$projectEntries = Get-PerformanceSummaryEntries -Text $stdout -SectionHeader "Project Performance Summary:"
$targetEntries = Get-PerformanceSummaryEntries -Text $stdout -SectionHeader "Target Performance Summary:"
$taskEntries = Get-PerformanceSummaryEntries -Text $stdout -SectionHeader "Task Performance Summary:"

$traceSections = [ordered]@{
  project_evaluation = @($projectEvaluationEntries)
  project = @($projectEntries)
  target = @($targetEntries)
  task = @($taskEntries)
}

$traceSpans = @()
foreach ($sectionName in $traceSections.Keys) {
  foreach ($entry in $traceSections[$sectionName]) {
    $traceSpans += [ordered]@{
      category = $sectionName
      name = $entry.name
      duration_ms = [double]$entry.duration_ms
      calls = [int]$entry.calls
    }
  }
}

$outputArtifacts = Get-OutputFileMetadata -RepoRoot $repoRoot -Paths @(
  (Join-Path $buildRoot "$Configuration.$Platform\PatchCleaner.exe"),
  (Join-Path $buildRoot "$Configuration.$Platform\PatchCleaner.pdb"),
  (Join-Path $buildRoot "PatchCleaner\$Configuration.$Platform\application.obj"),
  (Join-Path $buildRoot "PatchCleaner\$Configuration.$Platform\main_frame.obj")
)

$timingSummary = [ordered]@{
  generated_at_utc = [DateTime]::UtcNow.ToString("o")
  configuration = $Configuration
  platform = $Platform
  clean = [bool]$Clean
  status = $status
  exit_code = $process.ExitCode
  measurement_source = $durationSource
  wrapper_wall_clock_ms = $wrapperDurationMs
  msbuild_reported_elapsed_ms = $msbuildReportedDurationMs
  primary_duration_ms = $primaryDurationMs
  top_project_entries = @($projectEntries | Sort-Object duration_ms -Descending | Select-Object -First 10)
  top_target_entries = @($targetEntries | Sort-Object duration_ms -Descending | Select-Object -First 15)
  top_task_entries = @($taskEntries | Sort-Object duration_ms -Descending | Select-Object -First 15)
  output_artifacts = @($outputArtifacts)
  trace_artifact = Get-RelativeRepoPath -RepoRoot $repoRoot -Path $tracePath
  flamegraph_artifact = Get-RelativeRepoPath -RepoRoot $repoRoot -Path $flamegraphPath
  build_log = Get-RelativeRepoPath -RepoRoot $repoRoot -Path $buildLogPath
  binary_log = Get-RelativeRepoPath -RepoRoot $repoRoot -Path $binaryLogPath
}
$timingSummary | ConvertTo-Json -Depth 6 | Set-Content -Path $timingsPath

$traceDocument = [ordered]@{
  generated_at_utc = [DateTime]::UtcNow.ToString("o")
  configuration = $Configuration
  platform = $Platform
  clean = [bool]$Clean
  status = $status
  basis = "msbuild_performance_summary"
  spans = @($traceSpans)
}
$traceDocument | ConvertTo-Json -Depth 6 | Set-Content -Path $tracePath

$flamegraphLines = New-Object System.Collections.Generic.List[string]
foreach ($entry in $projectEntries) {
  $safeName = ($entry.name -replace ';', ':')
  [void]$flamegraphLines.Add("build;project;$safeName $([int][Math]::Round($entry.duration_ms))")
}
foreach ($entry in $targetEntries) {
  if ([double]$entry.duration_ms -le 0) {
    continue
  }
  $safeName = ($entry.name -replace ';', ':')
  [void]$flamegraphLines.Add("build;target;$safeName $([int][Math]::Round($entry.duration_ms))")
}
foreach ($entry in $taskEntries) {
  if ([double]$entry.duration_ms -le 0) {
    continue
  }
  $safeName = ($entry.name -replace ';', ':')
  [void]$flamegraphLines.Add("build;task;$safeName $([int][Math]::Round($entry.duration_ms))")
}
$flamegraphLines | Set-Content -Path $flamegraphPath

$history = @()
if (Test-Path $perfHistoryPath) {
  $rawHistory = Get-Content $perfHistoryPath -Raw
  if (-not [string]::IsNullOrWhiteSpace($rawHistory)) {
    $parsedHistory = ConvertFrom-Json $rawHistory
    if ($parsedHistory -is [System.Array]) {
      $history = @($parsedHistory)
    } elseif ($null -ne $parsedHistory) {
      $history = @($parsedHistory)
    }
    elseif ($null -ne $parsedHistory) {
      # ConvertFrom-Json returns a single PSCustomObject when the file has one sample.
      $history = @($parsedHistory)
    }
  }
}

$primaryHistoryValues = Get-HistorySignalValues -History $history -Configuration $Configuration -Platform $Platform -Clean ([bool]$Clean) -SignalKind "primary"
$primarySignal = Get-SignalSummary -Signal $durationSource -CurrentValue $primaryDurationMs -HistoryValues $primaryHistoryValues -RegressionThresholdPercent $regressionThresholdPercent

$additionalSignals = @()
$preferredTarget = Get-PreferredPerformanceEntry -Entries $targetEntries -PreferredNames @("ClCompile", "Link", "Manifest", "ResourceCompile")
if ($null -ne $preferredTarget) {
  $targetHistoryValues = Get-HistorySignalValues -History $history -Configuration $Configuration -Platform $Platform -Clean ([bool]$Clean) -SignalKind "target" -SignalName $preferredTarget.name
  $additionalSignals += Get-SignalSummary -Signal ("target:" + $preferredTarget.name) -CurrentValue ([double]$preferredTarget.duration_ms) -HistoryValues $targetHistoryValues -RegressionThresholdPercent $regressionThresholdPercent
}

$preferredTask = Get-PreferredPerformanceEntry -Entries $taskEntries -PreferredNames @("CL", "Link", "Mt", "RC")
if ($null -ne $preferredTask) {
  $taskHistoryValues = Get-HistorySignalValues -History $history -Configuration $Configuration -Platform $Platform -Clean ([bool]$Clean) -SignalKind "task" -SignalName $preferredTask.name
  $additionalSignals += Get-SignalSummary -Signal ("task:" + $preferredTask.name) -CurrentValue ([double]$preferredTask.duration_ms) -HistoryValues $taskHistoryValues -RegressionThresholdPercent $regressionThresholdPercent
}

$artifacts = [ordered]@{
  build_log = Get-RelativeRepoPath -RepoRoot $repoRoot -Path $buildLogPath
  binary_log = Get-RelativeRepoPath -RepoRoot $repoRoot -Path $binaryLogPath
  timing_summary = Get-RelativeRepoPath -RepoRoot $repoRoot -Path $timingsPath
  trace = Get-RelativeRepoPath -RepoRoot $repoRoot -Path $tracePath
  flamegraph = Get-RelativeRepoPath -RepoRoot $repoRoot -Path $flamegraphPath
}

$sample = [ordered]@{
  timestamp_utc = $startTimeUtc.ToString("o")
  configuration = $Configuration
  platform = $Platform
  clean = [bool]$Clean
  status = $status
  exit_code = $process.ExitCode
  measurements = [ordered]@{
    measurement_source = $durationSource
    wrapper_wall_clock_ms = $wrapperDurationMs
    msbuild_reported_elapsed_ms = $msbuildReportedDurationMs
    primary_duration_ms = $primaryDurationMs
  }
  sections = $traceSections
  artifacts = $artifacts
}

$updatedHistory = @($history + [pscustomobject]$sample | Select-Object -Last 30)
$updatedHistory | ConvertTo-Json -Depth 8 | Set-Content -Path $perfHistoryPath

$summary = [ordered]@{
  generated_at_utc = [DateTime]::UtcNow.ToString("o")
  repo_root = $repoRoot
  host_validation = [ordered]@{
    command = ".\\scripts\\test-local.ps1 -Configuration $Configuration -Platform $Platform" + ($(if ($Clean) { " -Clean" } else { "" }))
    source_of_truth = "Build/validation-summary.json"
    status = $status
    exit_code = $process.ExitCode
    duration_ms = $primaryDurationMs
    measurement_source = $durationSource
    wrapper_wall_clock_ms = $wrapperDurationMs
    msbuild_reported_elapsed_ms = $msbuildReportedDurationMs
    configuration = $Configuration
    platform = $Platform
  }
  measurements = [ordered]@{
    build_wall_clock_ms = $primarySignal.current_value_ms
    baseline_sample_count = $primarySignal.baseline_sample_count
    baseline_median_ms = $primarySignal.baseline_median_ms
    delta_percent_vs_median = $primarySignal.delta_percent_vs_median
    regression_threshold_percent = $primarySignal.regression_threshold_percent
    regression_detected = $primarySignal.regression_detected
    signal = $primarySignal.signal
    note = "Primary signal uses MSBuild-reported elapsed time when available. Additional target/task timings plus binary log, trace, and flamegraph artifacts are emitted for automation review."
    additional_signals = @($additionalSignals)
  }
  artifacts = $artifacts
}

$summary | ConvertTo-Json -Depth 8 | Set-Content -Path $validationSummaryPath

exit $process.ExitCode
