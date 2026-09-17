@echo off
chcp 65001 >nul
REM start-dsh.cmd -- 双击入口：带护栏启动 dsh（会话体检+修复、残留进程提示、会话日志备份）
REM 说明见 tools\start-dsh.ps1 顶部注释。
REM ★ 本文件必须存成 UTF-8 *带 BOM*：cmd.exe 读 .cmd 时按当前代码页解释字节，
REM   无 BOM 的 UTF-8 中文注释会被按 GBK 拆成乱码，中文里的全角括号会让 cmd 报
REM   "'xxx' is not recognized as an internal or external command"（2026-09-18 实际踩到）。
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start-dsh.ps1" %*
if errorlevel 1 pause
