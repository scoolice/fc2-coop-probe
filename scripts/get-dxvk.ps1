<#
Downloads DXVK 3.1.1 (Direct3D 8/9/10/11 on Vulkan; https://github.com/doitsujin/dxvk, zlib licence), checked by SHA-256:
release dxvk-3.1.1.tar.gz 40565b4a...e608, and the 32-bit DLLs Far Cry 2 uses: d3d10core.dll, d3d11.dll and dxgi.dll for its
D3D10 renderer (110c41a5...c2ba, f44543cb...1fd3, abd1449d...8bda), d3d9.dll for its D3D9 renderer (265888c3...97fa).
setup-copy.ps1 copies them into the test copy's bin folder when the dxvk folder next to it exists. Remove those four
DLLs from bin to go back to the system's Direct3D. DXVK writes FarCry2_d3d11.log / FarCry2_dxgi.log into bin when it runs.

  powershell -ExecutionPolicy Bypass -File get-dxvk.ps1 [-OutDir <folder>]
#>
param([string]$OutDir = (Join-Path $PSScriptRoot 'dxvk'))
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$url = 'https://github.com/doitsujin/dxvk/releases/download/v3.1.1/dxvk-3.1.1.tar.gz'
$tarHash = '40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608'
$fileHashes = @{
    'd3d10core.dll' = '110c41a5f67a0ab6d4eea586708883ce05aede204a0df6665e6362669079c2ba'
    'd3d11.dll'     = 'f44543cbe5c40202915a02ed6e9ac8140c3524e846ae67b01f6faa79485d1fd3'
    'dxgi.dll'      = 'abd1449dc28d536a4289c256ae7f4b6bbd15fa552792b7170c2ddece473c8bda'
    'd3d9.dll'      = '265888c31ca78dffa290c39cb7e50bfb02762590e41927906e46fb32f01497fa'
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
$tgz = Join-Path $OutDir 'dxvk-3.1.1.tar.gz'
Invoke-WebRequest $url -OutFile $tgz -UseBasicParsing
if ((Get-FileHash $tgz).Hash.ToLower() -ne $tarHash) { Remove-Item $tgz; throw 'tar.gz SHA-256 mismatch, deleted' }
$tmp = Join-Path $OutDir 'extract'
New-Item -ItemType Directory -Force $tmp | Out-Null
& (Join-Path $env:SystemRoot 'System32\tar.exe') -xzf $tgz -C $tmp
if ($LASTEXITCODE -ne 0) { throw "tar failed ($LASTEXITCODE)" }
foreach ($f in $fileHashes.Keys) {
    $src = Join-Path $tmp "dxvk-3.1.1\x32\$f"
    if ((Get-FileHash $src).Hash.ToLower() -ne $fileHashes[$f]) { throw "x32\$f SHA-256 mismatch" }
    Copy-Item $src (Join-Path $OutDir $f) -Force
}
Remove-Item $tmp -Recurse -Force
Write-Output "[dxvk] DXVK 3.1.1 (32-bit) ready: $OutDir (SHA-256 checked)"
