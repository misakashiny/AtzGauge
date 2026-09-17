@echo off
REM gh-push.cmd -- double-click friendly: push this repo to GitHub
REM
REM ASCII only (cmd reads .cmd as ANSI/GBK when there is no BOM).
REM Chinese output comes from gh-push.mjs.
REM
REM Usage:
REM   gh-push.cmd --user <github-username>
REM   gh-push.cmd --user <github-username> --check     (dry run: privacy check only)
REM
REM Tip: set ATZ_GH_USER once and you can just double-click this file.

title AtzGauge push to GitHub
if "%ATZ_GH_USER%"=="" (
  if "%~1"=="" (
    echo.
    echo   No GitHub username given.
    echo   Run it like this instead:  gh-push.cmd --user your-name
    echo   Or set a user-level env var ATZ_GH_USER and double-click again.
    echo.
    pause
    exit /b 1
  )
)
node "%~dp0gh-push.mjs" %*
set RC=%ERRORLEVEL%
echo.
if "%RC%"=="0" (
  echo RESULT: OK - pushed.
) else (
  echo RESULT: FAILED - see messages above.
)
echo.
pause
exit /b %RC%
