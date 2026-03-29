param(
  [switch]$IncludeWin32 = $true,
  [switch]$IncludeRelease = $true,
  [switch]$PrepareSandboxBundle,
  [string]$SummaryPath
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildScript = Join-Path $PSScriptRoot "build-local.ps1"
$sandboxScript = Join-Path $PSScriptRoot "prepare-sandbox-bundle.ps1"

if (-not (Test-Path $buildScript)) {
  throw "Build helper not found: $buildScript"
}

if ($PrepareSandboxBundle -and -not (Test-Path $sandboxScript)) {
  throw "Sandbox helper not found: $sandboxScript"
}

$runs = @(
  @{ Configuration = "Debug"; Platform = "x64"; Label = "Debug|x64" }
)

if ($IncludeWin32) {
  $runs += @{ Configuration = "Debug"; Platform = "Win32"; Label = "Debug|Win32" }
}

if ($IncludeRelease) {
  $runs += @{ Configuration = "Release"; Platform = "x64"; Label = "Release|x64" }
}

$results = New-Object System.Collections.Generic.List[object]

foreach ($run in $runs) {
  $startedAt = Get-Date
  $status = "PASS"
  $details = ""

  Write-Host "==> Building $($run.Label)"
  try {
    & $buildScript -Configuration $run.Configuration -Platform $run.Platform
    if ($LASTEXITCODE -ne 0) {
      throw "build-local.ps1 exited with code $LASTEXITCODE"
    }
  } catch {
    $status = "FAIL"
    $details = $_.Exception.Message
  }

  $results.Add([pscustomobject]@{
      Check = "Build"
      Target = $run.Label
      Status = $status
      StartedAt = $startedAt.ToString("o")
      FinishedAt = (Get-Date).ToString("o")
      Details = $details
    })

  if ($status -ne "PASS") {
    break
  }
}

if ($PrepareSandboxBundle -and ($results | Where-Object { $_.Status -eq "FAIL" }).Count -eq 0) {
  $startedAt = Get-Date
  $status = "PASS"
  $details = ""

  Write-Host "==> Preparing Windows Sandbox bundle"
  try {
    & $sandboxScript
    if ($LASTEXITCODE -ne 0) {
      throw "prepare-sandbox-bundle.ps1 exited with code $LASTEXITCODE"
    }
  } catch {
    $status = "FAIL"
    $details = $_.Exception.Message
  }

  $results.Add([pscustomobject]@{
      Check = "SandboxBundle"
      Target = "Release|x64"
      Status = $status
      StartedAt = $startedAt.ToString("o")
      FinishedAt = (Get-Date).ToString("o")
      Details = $details
    })
}

if ([string]::IsNullOrWhiteSpace($SummaryPath)) {
  $SummaryPath = Join-Path $repoRoot "Build\validation-summary.json"
}

$summaryDirectory = Split-Path $SummaryPath -Parent
if (-not [string]::IsNullOrWhiteSpace($summaryDirectory)) {
  New-Item -ItemType Directory -Force -Path $summaryDirectory | Out-Null
}

$summary = [pscustomobject]@{
  generated_at = (Get-Date).ToString("o")
  repo_root = $repoRoot
  checks = $results
}

$summary | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 $SummaryPath
Write-Host "Validation summary: $SummaryPath"

if (($results | Where-Object { $_.Status -eq "FAIL" }).Count -gt 0) {
  exit 1
}

exit 0
