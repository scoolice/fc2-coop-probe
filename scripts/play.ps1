<#
Starts a co-op session with the settings Setup.bat saved, run by Play.bat in the kit. Checks first that the test copy
has this kit's probe (else: run Setup.bat), then runs start-coop.ps1 with the usual options: the other player as a
buddy, test settings (god mode, enemies ignore you), and on the join side waiting for the host's Steam invite.

  powershell -ExecutionPolicy Bypass -File play.ps1 [-Join wait|friend] [-Settings <file>]
#>
param(
    [ValidateSet('wait', 'friend')][string]$Join = 'wait',
    [string]$Settings
)
$ErrorActionPreference = 'Stop'
$kit = $PSScriptRoot
if (-not $Settings) { $Settings = Join-Path $env:LOCALAPPDATA 'fc2-coop\settings.json' }
try {
if (-not (Test-Path $Settings)) { throw 'No settings yet: double-click Setup.bat first.' }
$s = Get-Content $Settings -Raw | ConvertFrom-Json
$bin = Join-Path $s.testDir 'bin'
if (-not (Test-Path (Join-Path $bin 'Dunia.dll.orig'))) { throw "No test copy at $($s.testDir): double-click Setup.bat again." }
$probe = if (Test-Path (Join-Path $bin 'coop_probe.asi')) { Join-Path $bin 'coop_probe.asi' } else { Join-Path $bin 'dinput8.dll' }
$want = Get-Content (Join-Path $kit 'SHA256SUMS.txt') | Where-Object { $_ -match '^[0-9a-f]{64}\s+\*?dinput8\.dll$' } | ForEach-Object { $_.Substring(0, 64) }
if ((Get-FileHash $probe).Hash.ToLower() -ne $want) { throw 'The test copy has a different probe than this kit: double-click Setup.bat first.' }
$steam = Join-Path $kit 'steam\steam_api64.dll'
if (-not (Test-Path $steam)) { throw 'No Steam connector in this kit folder: double-click Setup.bat first.' }

$sa = @{ Role = $s.role; TestDir = $s.testDir; SteamDll = $steam; Python = $s.python; AiBody = $true; TestSettings = $true }
if ($s.role -eq 'join') { $sa.Join = $Join }
}
catch { Write-Output "[play] STOPPED: $_"; exit 1 }
Write-Output "[play] test copy $($s.testDir), role $($s.role). Don't touch the keyboard until it says 'in the world'."
& (Join-Path $kit 'start-coop.ps1') @sa
