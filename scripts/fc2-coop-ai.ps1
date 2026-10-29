<#
Starts a two-copy co-op session on this PC with AI bodies: both test copies load their newest save, AI ignores
the player and god mode are switched on (test settings), each copy spawns a buddy next to its player that follows
the other copy's player (probe `aifollow <buddy> remote`), and the two probes are linked over loopback.
Do not use the keyboard until it prints "session up".

  powershell -ExecutionPolicy Bypass -File scripts\fc2-coop-ai.ps1 -CopyA <test copy A> -CopyB <test copy B> [-Probe dinput8.dll] [-NoTestSettings]
#>
param(
    [Parameter(Mandatory = $true)][string]$CopyA,
    [Parameter(Mandatory = $true)][string]$CopyB,
    [string]$Probe,
    [string]$BuddyA = 'buddies.Buddies.Frank_Bilders_Unarmed',   # shown in A for B's player
    [string]$BuddyB = 'buddies.Buddies.Frank_Bilders_Unarmed',   # shown in B for A's player
    [switch]$NoTestSettings
)
$ErrorActionPreference = 'Stop'
$tools = $PSScriptRoot
function Lua($copy, $code) { (powershell -NoProfile -File (Join-Path $tools 'fc2-lua.ps1') -Copy $copy -Code $code | Select-Object -Last 1) }
function Native($copy, $text) { powershell -NoProfile -File (Join-Path $tools 'fc2-native.ps1') -Copy $copy -Text $text -Wait 500 | Out-Null }

$copies = @(
    @{ dir = (Resolve-Path $CopyA).Path; title = 'A'; port = 9101; peer = 9102; buddy = $BuddyA },
    @{ dir = (Resolve-Path $CopyB).Path; title = 'B'; port = 9102; peer = 9101; buddy = $BuddyB })

# only game processes started from the test copies are stopped
foreach ($c in $copies) {
    Get-Process FarCry2 -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$($c.dir)\*" } | Stop-Process -Force
}
Start-Sleep 3
if ($Probe) { foreach ($c in $copies) { Copy-Item $Probe (Join-Path $c.dir 'bin\dinput8.dll') -Force } }

foreach ($c in $copies) {
    $r = powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'fc2-session.ps1') -Copy $c.dir -Title $c.title | Select-Object -Last 1
    if ($r -notmatch 'ready: player id (\d+)') { throw "copy $($c.title) did not load: $r" }
    $c.player = $Matches[1]
    Write-Output "[coop] $($c.title): $r"
}
if (-not $NoTestSettings) {
    foreach ($c in $copies) { Write-Output ("[coop] {0}: {1}" -f $c.title, (Lua $c.dir "ai_IgnorePlayer = 1 cheat_GodMode = 1 __out = 'ignore=' .. tostring(ai_IgnorePlayer) .. ' god=' .. tostring(cheat_GodMode)")) }
}
foreach ($c in $copies) {
    # the buddy appears 3 m east of this copy's own player (both players start on the same spot)
    $pos = (powershell -NoProfile -File (Join-Path $tools 'fc2-native.ps1') -Copy $c.dir -Text "pos $($c.player)" -Wait 300 | Select-String "pos $($c.player): x=(\S+) y=(\S+) z=(\S+)").Matches[0]
    $inv = [Globalization.CultureInfo]::InvariantCulture
    $b = Lua $c.dir ([string]::Format($inv, "__out = tostring(SpawnEntityFromArchetype('{0}', {1}, {2}, {3}, 0, 0, 0))",
            $c.buddy, [double]$pos.Groups[1].Value + 3, [double]$pos.Groups[2].Value, [double]$pos.Groups[3].Value + 0.3))
    if ($b -notmatch '^\d+$' -or $b -eq '18446744073709551615') { throw "copy $($c.title): buddy spawn failed: $b" }
    $c.buddyId = $b
    Native $c.dir "aistall 1000 1.5\nailead 0.7\naifollow $b remote"
    Write-Output "[coop] $($c.title): buddy $b ($($c.buddy)) follows the other player"
}
foreach ($c in $copies) { Native $c.dir "netstart $($c.port) 127.0.0.1 $($c.peer) $($c.player)" }
Start-Sleep 4
foreach ($c in $copies) {
    $net = Get-Content (Join-Path $c.dir 'bin\coop_probe.log') | Select-String '\] net: tx=' | Select-Object -Last 1
    Write-Output "[coop] $($c.title): $($net.Line -replace '^\[[^\]]*\] ', '')"
}
Write-Output '[coop] session up: play in window A; window B shows A''s player as its buddy (and A shows B''s)'
