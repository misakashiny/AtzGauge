@echo off
REM emoji-kit.cmd -- double-click launcher for tools\emoji-kit.mjs
REM
REM This wrapper is intentionally ASCII-only and does almost nothing:
REM   * looks for node
REM   * forwards every argument to the .mjs unchanged
REM All Chinese prompts live in emoji-kit.mjs, because cmd.exe mangles non-ASCII .cmd
REM files: no-BOM UTF-8 gets read as GBK and even full-width brackets break the parser
REM (hit for real on 2026-09-18 with start-dsh.cmd).
REM
REM Examples:
REM   emoji-kit.cmd                                   (double-click) asks for the folder
REM   emoji-kit.cmd "D:\emoji\v1"                     pack + push + reboot the device
REM   emoji-kit.cmd --builtin                         restore the built-in emoji
REM   emoji-kit.cmd --check "D:\emoji\v1"             check only
REM   emoji-kit.cmd --dir "D:\emoji\v1" --dry-run     pack only, no push
REM   emoji-kit.cmd --dir "D:\emoji\v1" --no-push     pack + serve, no auto push
setlocal
cd /d "%~dp0.."

where node >nul 2>nul
if errorlevel 1 (
  echo.
  echo [x] node not found. This tool needs Node.js.
  echo     Install an LTS build from https://nodejs.org/ then reopen this window.
  echo.
  pause
  exit /b 1
)

REM A bare folder path becomes --dir <path>; real flags are forwarded as-is.
set "A1=%~1"
if "%A1%"=="" (
  node "%~dp0emoji-kit.mjs" --ask
) else if "%A1:~0,1%"=="-" (
  node "%~dp0emoji-kit.mjs" %*
) else (
  node "%~dp0emoji-kit.mjs" --dir "%~1" %2 %3 %4 %5 %6
)
set RC=%ERRORLEVEL%

echo.
pause
exit /b %RC%
