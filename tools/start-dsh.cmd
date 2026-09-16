@echo off
REM start-dsh.cmd -- 双击入口：带护栏启动 dsh（会话体检+修复、残留进程提示、会话日志备份）
REM 说明见 tools\start-dsh.ps1 顶部注释。
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start-dsh.ps1" %*
if errorlevel 1 pause
