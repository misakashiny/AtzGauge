@echo off
REM sim-console.cmd -- 双击启动「车况模拟台」（PC 端实时发模拟转速等数据给设备）
REM 默认设备 IP 在 tools\sim-console.mjs 顶部；也可带参数：
REM   sim-console.cmd --device 192.168.5.130 --port 8123
title AtzGauge 车况模拟台
node "%~dp0sim-console.mjs" %*
if errorlevel 1 (
  echo.
  echo 启动失败：请确认已安装 Node.js（node -v 能看到版本号）。
  pause
)
