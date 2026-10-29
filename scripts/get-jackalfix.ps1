<#
Downloads FC2JackalFix V1 (borderless window, FOV, high-FPS fixes; https://github.com/TGP482/FC2JackalFix), checked by SHA-256:
release zip afe5ba18...c339e, bin\dinput8.dll = Ultimate ASI Loader 9.7.4 (d5a059aa...a2e6), bin\plugins\FC2JackalFix.asi
(79a7d34f...dcf3), bin\plugins\FC2JackalFix.ini (a0120d5f...faf3). setup-copy.ps1 installs it into the test copy when the
jackalfix folder next to it exists: the loader becomes bin\dinput8.dll and the probe bin\coop_probe.asi.

  powershell -ExecutionPolicy Bypass -File get-jackalfix.ps1 [-OutDir <folder>]
#>
param([string]$OutDir = (Join-Path $PSScriptRoot 'jackalfix'))
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$url = 'https://github.com/TGP482/FC2JackalFix/releases/download/V1/FC2JackalFix.zip'
$zipHash = 'afe5ba186ea68ecaf300f27944efa0346d06e324c636f92c6de935bad30c339e'
$fileHashes = @{
    'bin\dinput8.dll'                = 'd5a059aa467a7a7127c8f6169f79fa63ff0f55986ee9eb2fd9a281bebf2aa2e6'
    'bin\plugins\FC2JackalFix.asi' = '79a7d34fc4aea1d5126ba613f7845c8878c7c306d0b8e16858b119b80b97dcf3'
    'bin\plugins\FC2JackalFix.ini' = 'a0120d5fa7251acb0715bcbf5c4738a3b143bed03964ca2cf8a43fcbb106faf3'
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
$zip = Join-Path $OutDir 'FC2JackalFix-V1.zip'
Invoke-WebRequest $url -OutFile $zip -UseBasicParsing
if ((Get-FileHash $zip).Hash.ToLower() -ne $zipHash) { Remove-Item $zip; throw 'zip SHA-256 mismatch, deleted' }
Expand-Archive $zip -DestinationPath $OutDir -Force
foreach ($f in $fileHashes.Keys) {
    if ((Get-FileHash (Join-Path $OutDir $f)).Hash.ToLower() -ne $fileHashes[$f]) { throw "$f SHA-256 mismatch" }
}
Write-Output "[jackalfix] FC2JackalFix V1 ready: $OutDir (SHA-256 checked)"
