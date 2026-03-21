param(
  [ValidateSet("Debug", "Release")]
  [string]$Configuration = "Debug",

  [ValidateSet("x64", "Win32")]
  [string]$Platform = "x64",

  [switch]$Clean
)

$ErrorActionPreference = "Stop"

function Get-MsBuildPath {
  $canonical = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
  if (Test-Path $canonical) {
    return $canonical
  }

  throw "MSBuild.exe was not found at $canonical."
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

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$projectPath = Join-Path $repoRoot "patch_cleaner\PatchCleaner.vcxproj"
if (-not (Test-Path $projectPath)) {
  throw "Project file not found: $projectPath"
}

$msbuildPath = Get-MsBuildPath
$target = if ($Clean) { "Clean;Build" } else { "Build" }

$args = @(
  "`"$projectPath`""
  "/t:$target"
  "/p:Configuration=$Configuration"
  "/p:Platform=$Platform"
  "/p:SolutionDir=$repoRoot\"
  "/p:SolutionName=PatchCleaner"
  "/m"
)

Write-Host "MSBuild: $msbuildPath"
Write-Host "Project: $projectPath"
Write-Host "Target: $target ($Configuration|$Platform)"

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
