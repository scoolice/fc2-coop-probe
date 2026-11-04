@echo off
rem Sets up (or updates) the Far Cry 2 co-op test copy. Safe to run again after unzipping a new kit.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
echo.
pause
