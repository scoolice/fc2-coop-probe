<#
One-step setup for a co-op tester, run by Setup.bat in the kit. Safe to run again: after a new kit is unzipped it only
puts the new probe into the existing test copy.

  1. checks that 64-bit Python 3 is installed, unless the kit has fc2_relay.exe (the relay without Python)
  2. unblocks the kit's files (Windows marks files from a downloaded zip) and downloads what is missing next to this
     script: FC2JackalFix (get-jackalfix.ps1), DXVK (get-dxvk.ps1), the Steam connector (get-steam-api.ps1)
  3. makes the test copy or updates its probe (setup-copy.ps1 with the kit's private folder; the copy found in the
     settings file, else the default <game drive>\fc2-coop\test-<letter>)
  4. writes the settings file Play.bat reads: test copy, Python command, role

  powershell -ExecutionPolicy Bypass -File setup.ps1 [-Role join|host] [-TestDir <folder>] [-NoDxvk] [-NoJackalFix]
#>
param(
    [ValidateSet('join', 'host')][string]$Role = 'join',
    [string]$TestDir,
    [switch]$NoDxvk,
    [switch]$NoJackalFix,
    [string]$Settings,
    [string]$Documents
)
$ErrorActionPreference = 'Stop'
$kit = $PSScriptRoot
if (-not $Settings) { $Settings = Join-Path $env:LOCALAPPDATA 'fc2-coop\settings.json' }
function Say($m) { Write-Output ("[setup] " + $m) }

try {
# 1. Python
$python = $null
if (Test-Path (Join-Path $kit 'fc2_relay.exe')) { $python = 'none' }
else {
foreach ($c in 'py', 'python') {
    $pa = [string[]]@('-c', 'import struct; print(struct.calcsize(chr(80)) * 8)')
    if ($c -eq 'py') { $pa = [string[]]@('-3') + $pa }
    try { $bits = & $c $pa 2>$null } catch { $bits = $null }
    if ($bits -eq '64') { $python = $c; break }
}
if (-not $python) {
    throw 'No 64-bit Python 3 found. Install it from https://www.python.org/downloads/ (tick "py launcher"), then run Setup again.'
}
Say "Python: $python"
}

# 2. kit files and downloads
Get-ChildItem $kit -Recurse -File | Unblock-File
if (-not $NoJackalFix -and -not (Test-Path (Join-Path $kit 'jackalfix\bin\plugins\FC2JackalFix.asi'))) { & (Join-Path $kit 'get-jackalfix.ps1') }
if (-not $NoDxvk -and -not (Test-Path (Join-Path $kit 'dxvk\d3d11.dll'))) { & (Join-Path $kit 'get-dxvk.ps1') }
if (-not (Test-Path (Join-Path $kit 'steam\steam_api64.dll'))) { & (Join-Path $kit 'get-steam-api.ps1') }
$relay = Join-Path $kit 'fc2_relay.exe'
if (Test-Path $relay) {
    $c = & $relay --check --steam-dll (Join-Path $kit 'steam\steam_api64.dll') 2>&1
    if ($LASTEXITCODE -ne 0) { throw "the relay cannot use the Steam connector: $c" }
    Say ($c -replace '^\S+ check: .*, ', 'relay ok, ')
}

# 3. test copy
$old = $null
if (Test-Path $Settings) { $old = Get-Content $Settings -Raw | ConvertFrom-Json }
if (-not $TestDir -and $old -and $old.testDir -and (Test-Path (Join-Path $old.testDir 'bin\Dunia.dll.orig'))) { $TestDir = $old.testDir }
$letter = if ($Role -eq 'host') { 'A' } else { 'B' }
$sa = @{ PrivateDir = (Join-Path $kit 'private'); Letter = $letter }
if ($TestDir) { $sa.TestDir = $TestDir }
if ($NoJackalFix) { $sa.JackalFix = Join-Path $kit 'no-jackalfix' }
if ($NoDxvk) { $sa.Dxvk = Join-Path $kit 'no-dxvk' }
if ($Documents) { $sa.Documents = $Documents }
& (Join-Path $kit 'setup-copy.ps1') @sa | Tee-Object -Variable out
$m = [regex]::Match(($out -join "`n"), '(?m)^\[setup\] done\. Test copy: (.+)$')
if (-not $m.Success) { throw 'setup-copy did not finish' }
$TestDir = $m.Groups[1].Value.Trim()

# 4. settings
New-Item -ItemType Directory -Force (Split-Path $Settings) | Out-Null
[ordered]@{ testDir = $TestDir; python = $python; role = $Role } | ConvertTo-Json | Set-Content $Settings -Encoding utf8
Say "settings saved: $Settings"
Say 'All set. Double-click Play.bat to play.'
}
catch { Write-Output ''; Write-Output "[setup] STOPPED: $_"; exit 1 }
