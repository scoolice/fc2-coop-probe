<#
Sends native probe commands (one per line, see the probe README) to a running test copy and prints the
probe log lines written while they ran.

  powershell -File scripts\fc2-native.ps1 -Copy <test copy> -Text "pos 1307..."
#>
param(
    [Parameter(Mandatory = $true)][string]$Copy,
    [Parameter(Mandatory = $true)][string]$Text,
    [int]$Wait = 1000
)
$ErrorActionPreference = 'Stop'
$bin = Join-Path (Resolve-Path $Copy) 'bin'
$log = Join-Path $bin 'coop_probe.log'
$nat = Join-Path $bin 'coop_native.txt'
while (Test-Path $nat) { Start-Sleep -Milliseconds 200 }
$before = @(Get-Content $log).Count
Set-Content $nat ($Text -replace '\\n', "`r`n") -Encoding ascii
while (Test-Path $nat) { Start-Sleep -Milliseconds 200 }
Start-Sleep -Milliseconds $Wait
Get-Content $log | Select-Object -Skip $before | Where-Object { $_ -notmatch '\] net: tx=' }
