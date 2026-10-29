<#
Runs Lua 4.0 code in a running test copy through the probe and prints __out (newlines come back as \n).
The code must set the global __out (a string).

  powershell -File scripts\fc2-lua.ps1 -Copy <test copy> -Code "__out = tostring(GetLocalPlayerId())"
  powershell -File scripts\fc2-lua.ps1 -Copy <test copy> -File probe.lua
#>
param(
    [Parameter(Mandatory = $true)][string]$Copy,
    [string]$Code,
    [string]$File,
    [int]$Timeout = 20
)
$ErrorActionPreference = 'Stop'
$bin = Join-Path (Resolve-Path $Copy) 'bin'
$log = Join-Path $bin 'coop_probe.log'
$cmd = Join-Path $bin 'coop_cmd.lua'
if ($File) { $Code = Get-Content $File -Raw }
if (-not $Code) { throw 'pass -Code or -File' }
$n = [guid]::NewGuid().ToString('N').Substring(0, 8)
$t0 = Get-Date
while (Test-Path $cmd) { if (((Get-Date) - $t0).TotalSeconds -gt $Timeout) { throw 'lua channel busy' }; Start-Sleep -Milliseconds 200 }
# the nonce goes in front so the answer line can be found; newlines would split the log line.
# _ERRORMESSAGE is swapped for the run so a Lua error comes back as text, then restored.
$pre = "__fc2err = _ERRORMESSAGE _ERRORMESSAGE = function(m) _ERRORMESSAGE = __fc2err __out = '$n LUAERROR ' .. gsub(tostring(m), '\n', '\\n') end`n"
$post = "`n_ERRORMESSAGE = __fc2err __out = '$n ' .. gsub(tostring(__out), '\n', '\\n')"
Set-Content $cmd ($pre + $Code + $post) -Encoding ascii
# the probe deletes the file when it runs it; the channel is serial, so the last result line is ours
while (Test-Path $cmd) { if (((Get-Date) - $t0).TotalSeconds -gt $Timeout) { throw 'no answer (is the copy running and in the world?)' }; Start-Sleep -Milliseconds 200 }
Start-Sleep -Milliseconds 300
$last = (Get-Content $log | Select-String 'cmd executed' | Select-Object -Last 1).Line
if ($last -match "out\(\d+\)=$n ") { $last -replace ".*out\(\d+\)=$n ", '' }
elseif ($last -match 'status=(\d+)') { throw "lua error, status $($Matches[1]) (1 = run error, 3 = syntax error)" }
else { throw "unexpected log line: $last" }
