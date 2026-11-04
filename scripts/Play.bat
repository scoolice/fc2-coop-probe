@echo off
rem Starts a co-op session with the settings Setup.bat saved.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0play.ps1" %*
echo.
pause
