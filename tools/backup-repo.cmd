@echo off
REM backup-repo.cmd -- double-click launcher for tools\backup-repo.ps1
REM ASCII-only on purpose: cmd.exe mangles non-ASCII .cmd files (see start-dsh.cmd).
REM
REM Double-click  -> commit pending changes (auto message) and back up to E:
REM With args    -> forwarded to the .ps1, e.g.
REM                   backup-repo.cmd -Check
REM                   backup-repo.cmd -Message "feat: xxx"
REM                   backup-repo.cmd -NoCommit
setlocal
cd /d "%~dp0.."

if "%~1"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0backup-repo.ps1"
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0backup-repo.ps1" %*
)
set RC=%ERRORLEVEL%

echo.
if %RC% NEQ 0 echo [x] backup failed (exit %RC%), see the messages above.
echo.
pause
exit /b %RC%
