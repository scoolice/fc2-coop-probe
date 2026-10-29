<#
Starts one patched test copy, loads its save (presses Enter in its window until the probe log shows the
save being opened) and leaves it running for fc2-lua.ps1 / fc2-native.ps1. Do not use the keyboard
until it prints "ready".

  powershell -ExecutionPolicy Bypass -File scripts\fc2-session.ps1 -Copy <test copy> [-Title A]
#>
param(
    [Parameter(Mandatory = $true)][string]$Copy,
    [string]$Title = 'A',
    [int]$LoadTimeout = 240
)
$ErrorActionPreference = 'Stop'
$bin = Join-Path (Resolve-Path $Copy) 'bin'
$log = Join-Path $bin 'coop_probe.log'
$realMark = 'My Games\Far Cry 2\'

Add-Type @"
using System; using System.Runtime.InteropServices;
public static class FcSess {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool SetWindowText(IntPtr h, string s);
}
"@

# the probe appends to its log across runs, so only lines after $skip belong to this session
$skip = if (Test-Path $log) { @(Get-Content $log).Count } else { 0 }
function LogLines { if (Test-Path $log) { @(Get-Content $log -ErrorAction SilentlyContinue | Select-Object -Skip $skip) } else { @() } }
function Guard($p) {
    if ($p.HasExited) { throw "copy exited by itself (code $($p.ExitCode))" }
    $p.Refresh()
    if ($p.MainWindowHandle -ne 0 -and $p.MainWindowTitle -ne $Title) { [void][FcSess]::SetWindowText($p.MainWindowHandle, $Title) }
    if (LogLines | Select-String -SimpleMatch $realMark) { Stop-Process -Id $p.Id -Force; throw 'KILL SWITCH: real profile folder in the probe log' }
}

if (Get-Process FarCry2 -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$bin*" }) { throw "a copy from $bin is already running" }
Remove-Item (Join-Path $bin 'coop_cmd.lua'), (Join-Path $bin 'coop_native.txt') -ErrorAction SilentlyContinue
$p = Start-Process -FilePath (Join-Path $bin 'FarCry2.exe') -WorkingDirectory $bin -PassThru
"started pid $($p.Id)"
$t0 = Get-Date
while ($true) {
    if (((Get-Date) - $t0).TotalSeconds -gt $LoadTimeout) { throw 'timeout waiting for the save to open' }
    Guard $p
    if (LogLines | Select-String 'CreateFileW .*\\saved games\\.*\.sav') { break }
    if ($p.MainWindowHandle -ne 0) {
        [void][FcSess]::SetForegroundWindow($p.MainWindowHandle); Start-Sleep -Milliseconds 200
        $sc = [byte][FcSess]::MapVirtualKey(0x0D, 0)
        [FcSess]::keybd_event(0x0D, $sc, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
        [FcSess]::keybd_event(0x0D, $sc, 2, [UIntPtr]::Zero)
    }
    Start-Sleep 3
}
"save opened, waiting for the world"
$cmd = Join-Path $bin 'coop_cmd.lua'
while ($true) {
    if (((Get-Date) - $t0).TotalSeconds -gt $LoadTimeout) { throw 'timeout waiting for the player' }
    Guard $p
    $n = [guid]::NewGuid().ToString('N').Substring(0, 8)
    Set-Content $cmd "__out = '$n ' .. GetLocalPlayerId()" -Encoding ascii
    Start-Sleep 2
    $l = LogLines | Select-String "out\(\d+\)=$n (\d+)" | Select-Object -Last 1
    if ($l -and $l.Matches[0].Groups[1].Value -ne '18446744073709551615') { "ready: player id $($l.Matches[0].Groups[1].Value), pid $($p.Id)"; break }
}
