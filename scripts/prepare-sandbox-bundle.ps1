param(
  [string]$OutputRoot = (Join-Path ([System.IO.Path]::GetTempPath()) "PatchCleanerSandbox"),

  [switch]$EnableLogonCommand,

  [switch]$Launch
)

$ErrorActionPreference = "Stop"

function New-RunDirectory {
  param(
    [Parameter(Mandatory = $true)]
    [string]$BaseRoot
  )

  New-Item -ItemType Directory -Force -Path $BaseRoot | Out-Null

  $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
  $candidate = Join-Path $BaseRoot $timestamp
  $suffix = 0
  while (Test-Path $candidate) {
    ++$suffix
    $candidate = Join-Path $BaseRoot ("{0}-{1:D2}" -f $timestamp, $suffix)
  }

  New-Item -ItemType Directory -Force -Path $candidate | Out-Null
  return $candidate
}

function Copy-RequiredItem {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Source,

    [Parameter(Mandatory = $true)]
    [string]$DestinationDirectory
  )

  if (-not (Test-Path $Source)) {
    throw "Required file not found: $Source"
  }

  Copy-Item -LiteralPath $Source -Destination $DestinationDirectory -Force
}

function ConvertTo-XmlText {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Value
  )

  return [System.Security.SecurityElement]::Escape($Value)
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildScript = Join-Path $PSScriptRoot "build-local.ps1"
$startupScript = Join-Path $PSScriptRoot "sandbox-startup.ps1"
$automationDriverScript = Join-Path $PSScriptRoot "sandbox-driver.ahk"
$sandboxExe = "C:\Windows\System32\WindowsSandbox.exe"
$autoHotkeyUiA = "C:\Program Files\AutoHotkey\v2\AutoHotkey64_UIA.exe"

if (-not (Test-Path $buildScript)) {
  throw "Build helper not found: $buildScript"
}
if (-not (Test-Path $startupScript)) {
  throw "Sandbox startup helper not found: $startupScript"
}
if (-not (Test-Path $automationDriverScript)) {
  throw "Sandbox automation driver not found: $automationDriverScript"
}
if (-not (Test-Path $autoHotkeyUiA)) {
  throw "AutoHotkey UIA runtime not found: $autoHotkeyUiA"
}

$runRoot = New-RunDirectory -BaseRoot $OutputRoot
$bundleRoot = Join-Path $runRoot "bundle"
$desktopRoot = Join-Path $runRoot "desktop"
$resultsRoot = Join-Path $runRoot "results"
$wsbPath = Join-Path $runRoot "PatchCleanerSandbox.wsb"
$manifestPath = Join-Path $runRoot "bundle-manifest.json"
$logonWrapperPath = Join-Path $bundleRoot "sandbox-logon.cmd"
$desktopLauncherPath = Join-Path $desktopRoot "Run PatchCleaner Sandbox.cmd"
$desktopReadmePath = Join-Path $desktopRoot "README.txt"

New-Item -ItemType Directory -Force -Path $bundleRoot, $desktopRoot, $resultsRoot | Out-Null

& $buildScript -Configuration Release -Platform x64

$releaseExe = Join-Path $repoRoot "Build\Release.x64\PatchCleaner.exe"
Copy-RequiredItem -Source $releaseExe -DestinationDirectory $bundleRoot
Copy-RequiredItem -Source $startupScript -DestinationDirectory $bundleRoot
Copy-RequiredItem -Source $automationDriverScript -DestinationDirectory $bundleRoot
Copy-RequiredItem -Source $autoHotkeyUiA -DestinationDirectory $bundleRoot

$runtimeDlls = @(
  "MSVCP140.dll",
  "VCRUNTIME140.dll",
  "VCRUNTIME140_1.dll"
)

foreach ($dll in $runtimeDlls) {
  $sourcePath = Join-Path $env:SystemRoot "System32\$dll"
  Copy-RequiredItem -Source $sourcePath -DestinationDirectory $bundleRoot
}

$bundleSandboxPath = "C:\PatchCleanerSandbox\Bundle"
$desktopSandboxPath = "C:\Users\WDAGUtilityAccount\Desktop\PatchCleaner Sandbox"
$resultsSandboxPath = "C:\PatchCleanerSandbox\Results"
$startupSandboxPath = Join-Path $bundleSandboxPath "sandbox-startup.ps1"
$logonWrapperSandboxPath = Join-Path $bundleSandboxPath "sandbox-logon.cmd"
$sandboxPowerShell = "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"
$sandboxLogPath = Join-Path $resultsSandboxPath "logon-command.log"
$desktopLauncherLogPath = Join-Path $resultsSandboxPath "desktop-launcher.log"
$desktopLauncherSandboxPath = Join-Path $desktopSandboxPath "Run PatchCleaner Sandbox.cmd"
$logonWrapperContent = @"
@echo off
setlocal
echo [%DATE% %TIME%] sandbox-logon.cmd started> "$sandboxLogPath"
"$sandboxPowerShell" -NoLogo -ExecutionPolicy Bypass -File "$startupSandboxPath" -BundleRoot "$bundleSandboxPath" -ResultsRoot "$resultsSandboxPath" >> "$sandboxLogPath" 2>&1
echo [%DATE% %TIME%] sandbox-logon.cmd exit code %ERRORLEVEL%>> "$sandboxLogPath"
exit /b %ERRORLEVEL%
"@
Set-Content -LiteralPath $logonWrapperPath -Value $logonWrapperContent -Encoding ASCII

$desktopLauncherContent = @"
@echo off
setlocal
echo [%DATE% %TIME%] Run PatchCleaner Sandbox.cmd started>> "$desktopLauncherLogPath"
"$sandboxPowerShell" -NoLogo -ExecutionPolicy Bypass -File "$startupSandboxPath" -BundleRoot "$bundleSandboxPath" -ResultsRoot "$resultsSandboxPath" >> "$desktopLauncherLogPath" 2>&1
echo [%DATE% %TIME%] Run PatchCleaner Sandbox.cmd exit code %ERRORLEVEL%>> "$desktopLauncherLogPath"
exit /b %ERRORLEVEL%
"@
Set-Content -LiteralPath $desktopLauncherPath -Value $desktopLauncherContent -Encoding ASCII

$desktopReadmeContent = @(
  "PatchCleaner Windows Sandbox Launcher",
  "",
  "If PatchCleaner does not start automatically after Windows Sandbox reaches the desktop:",
  "1. Open the 'PatchCleaner Sandbox' folder on the guest desktop.",
  "2. Run 'Run PatchCleaner Sandbox.cmd'.",
  "",
  "Results and logs are written to:",
  "  $resultsSandboxPath",
  "",
  "This launcher exists because some Windows Sandbox builds can reach the desktop but still fail when LogonCommand is present."
)
Set-Content -LiteralPath $desktopReadmePath -Value $desktopReadmeContent -Encoding UTF8

$logonCommand = 'cmd.exe /c "{0}"' -f $logonWrapperSandboxPath
$logonCommandXml = ""
if ($EnableLogonCommand) {
  $logonCommandXml = @"
  <LogonCommand>
    <Command>$(ConvertTo-XmlText $logonCommand)</Command>
  </LogonCommand>
"@
}

$wsbContent = @"
<Configuration>
  <VGpu>Disable</VGpu>
  <Networking>Enable</Networking>
  <MappedFolders>
    <MappedFolder>
      <HostFolder>$(ConvertTo-XmlText $bundleRoot)</HostFolder>
      <SandboxFolder>$(ConvertTo-XmlText $bundleSandboxPath)</SandboxFolder>
      <ReadOnly>true</ReadOnly>
    </MappedFolder>
    <MappedFolder>
      <HostFolder>$(ConvertTo-XmlText $resultsRoot)</HostFolder>
      <SandboxFolder>$(ConvertTo-XmlText $resultsSandboxPath)</SandboxFolder>
      <ReadOnly>false</ReadOnly>
    </MappedFolder>
    <MappedFolder>
      <HostFolder>$(ConvertTo-XmlText $desktopRoot)</HostFolder>
      <SandboxFolder>$(ConvertTo-XmlText $desktopSandboxPath)</SandboxFolder>
      <ReadOnly>true</ReadOnly>
    </MappedFolder>
  </MappedFolders>
$logonCommandXml
</Configuration>
"@

Set-Content -LiteralPath $wsbPath -Value $wsbContent -Encoding UTF8

$manifest = [ordered]@{
  created_at = (Get-Date).ToString("o")
  repo_root = $repoRoot
  run_root = $runRoot
  bundle_root = $bundleRoot
  desktop_root = $desktopRoot
  results_root = $resultsRoot
  wsb_path = $wsbPath
  runtime_dlls = $runtimeDlls
  bundle_files = @((Get-ChildItem -LiteralPath $bundleRoot -File).Name)
  desktop_files = @((Get-ChildItem -LiteralPath $desktopRoot -File).Name)
  desktop_launcher = $desktopLauncherSandboxPath
  logon_command_enabled = $EnableLogonCommand.IsPresent
  launch_command = if ($EnableLogonCommand) { $logonCommand } else { $null }
}

$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host "Windows Sandbox bundle prepared."
Write-Host "Run root: $runRoot"
Write-Host "Bundle:   $bundleRoot"
Write-Host "Results:  $resultsRoot"
Write-Host "WSB:      $wsbPath"

if ($Launch) {
  if (-not (Test-Path $sandboxExe)) {
    throw "Windows Sandbox executable not found: $sandboxExe"
  }

  Start-Process -FilePath $sandboxExe -ArgumentList "`"$wsbPath`"" | Out-Null
  Write-Host "Launched Windows Sandbox with $wsbPath"
  Write-Host "Guest desktop launcher: $desktopLauncherSandboxPath"
  if (-not $EnableLogonCommand) {
    Write-Host "If nothing starts automatically after the guest reaches desktop, open 'PatchCleaner Sandbox' on the guest desktop and run 'Run PatchCleaner Sandbox.cmd'."
  } else {
    Write-Host "If LogonCommand does not start PatchCleaner, use the guest desktop launcher as a fallback."
  }
}
