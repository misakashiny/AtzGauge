@echo off
REM publish-check.cmd -- double-click friendly: run the pre-publish privacy check
REM
REM ASCII only on purpose (Windows cmd reads .cmd as ANSI/GBK without a BOM).
REM All Chinese output comes from publish-check.mjs.

title AtzGauge publish check
node "%~dp0publish-check.mjs" %*
set RC=%ERRORLEVEL%
echo.
if "%RC%"=="0" (
  echo RESULT: OK - safe to push.
) else (
  echo RESULT: PROBLEMS FOUND - fix them before pushing.
)
echo.
pause
exit /b %RC%
