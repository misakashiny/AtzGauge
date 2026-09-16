@echo off
chcp 65001 >nul
echo ============================================================
echo   ATZ GAUGE - in-car serial logger (PowerShell, no Python)
echo   Log goes to:  ..\backup\car-log-YYYYMMDD-HHMMSS.txt
echo   Default 1 hour. Ctrl+C to stop early.
echo ============================================================
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0car-log.ps1" %*
echo.
echo ============================================================
echo   Done. Log file is in the backup folder next to tools\.
echo ============================================================
pause
