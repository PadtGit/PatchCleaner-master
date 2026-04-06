param(
  [ValidateSet("Debug", "Release")]
  [string]$Configuration = "Debug",

  [ValidateSet("x64", "Win32")]
  [string]$Platform = "x64",

  [switch]$Clean,

  [switch]$PerformanceSummary,

  [string]$BinaryLogPath
)

$ErrorActionPreference = "Stop"

function Get-MsBuildPath {
  $candidates = @(
    "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
    "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe"
    "C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
    "C:\Program Files\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
  )

  foreach ($candidate in $candidates) {
    if (Test-Path $candidate) {
      return $candidate
    }
  }

  throw "MSBuild.exe was not found in the expected Visual Studio 2026 locations."
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

  # Build a case-insensitive map, then write one canonical Path entry.
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

function Test-StaleIntermediatePaths {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,

    [Parameter(Mandatory = $true)]
    [string]$ProjectName,

    [Parameter(Mandatory = $true)]
    [string]$Configuration,

    [Parameter(Mandatory = $true)]
    [string]$Platform
  )

  $intDir = Join-Path $RepoRoot "Build\$ProjectName\$Configuration.$Platform"
  $manifestRcPath = Join-Path $intDir "${ProjectName}_manifest.rc"
  if (Test-Path $manifestRcPath) {
    $expectedManifestPath = (Join-Path $intDir "$ProjectName.exe.embed.manifest").Replace('\', '\\')
    $manifestContent = Get-Content $manifestRcPath -Raw
    if ($manifestContent -notlike "*$expectedManifestPath*") {
      return $true
    }
  }

  return $false
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$projectPath = Join-Path $repoRoot "patch_cleaner\PatchCleaner.vcxproj"
if (-not (Test-Path $projectPath)) {
  throw "Project file not found: $projectPath"
}

$projectName = [System.IO.Path]::GetFileNameWithoutExtension($projectPath)
$msbuildPath = Get-MsBuildPath
$forceClean = $Clean
if (-not $forceClean -and (Test-StaleIntermediatePaths -RepoRoot $repoRoot -ProjectName $projectName -Configuration $Configuration -Platform $Platform)) {
  Write-Host "Detected stale build intermediates for $Configuration|$Platform. Forcing Clean;Build."
  $forceClean = $true
}

$target = if ($forceClean) { "Clean;Build" } else { "Build" }

$args = @(
  "`"$projectPath`""
  "/t:$target"
  "/p:Configuration=$Configuration"
  "/p:Platform=$Platform"
  "/p:SolutionDir=$repoRoot\"
  "/p:SolutionName=PatchCleaner"
  "/m"
)

if ($PerformanceSummary) {
  $args += "/clp:Summary;PerformanceSummary"
}

if (-not [string]::IsNullOrWhiteSpace($BinaryLogPath)) {
  $binaryLogDirectory = Split-Path -Parent $BinaryLogPath
  if (-not [string]::IsNullOrWhiteSpace($binaryLogDirectory) -and -not (Test-Path $binaryLogDirectory)) {
    New-Item -ItemType Directory -Path $binaryLogDirectory -Force | Out-Null
  }

  $args += "/bl:`"$BinaryLogPath`""
}

Write-Host "MSBuild: $msbuildPath"
Write-Host "Project: $projectPath"
Write-Host "Target: $target ($Configuration|$Platform)"
if ($PerformanceSummary) {
  Write-Host "Performance summary: enabled"
}
if (-not [string]::IsNullOrWhiteSpace($BinaryLogPath)) {
  Write-Host "Binary log: $BinaryLogPath"
}

$psi = New-SanitizedProcessStartInfo -FileName $msbuildPath -Arguments ($args -join " ") -WorkingDirectory $repoRoot
$process = [System.Diagnostics.Process]::Start($psi)
if ($null -eq $process) {
  throw "Failed to start MSBuild process."
}

$stdout = $process.StandardOutput.ReadToEnd()
$stderr = $process.StandardError.ReadToEnd()
$process.WaitForExit()

if (-not [string]::IsNullOrWhiteSpace($stdout)) {
  Write-Host $stdout
}
if (-not [string]::IsNullOrWhiteSpace($stderr)) {
  Write-Error $stderr
}

exit $process.ExitCode
