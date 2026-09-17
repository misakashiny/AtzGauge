# atz-port.ps1 -- 串口的唯一来源（被其他脚本 dot-source）
#
# 为什么需要它：换电脑/换 USB 口后 COM 号会变，而 6 个脚本各自写死了 COM3。
# 现在所有脚本都从这里取串口，优先级：
#
#   1) 调用方显式传的 -Port 参数（脚本自己处理，最高优先）
#   2) 环境变量 ATZ_PORT        -- 会话级临时改，比如：$env:ATZ_PORT='COM5'
#   3) 文件 <工作区>\PORT.txt   -- 换电脑时改这一个文件，所有脚本跟着变
#   4) 兜底 'COM3'
#
# 用法（在被 dot-source 的脚本里）：
#   . "$PSScriptRoot\atz-port.ps1"
#   param 默认值写 [string]$Port = (Get-AtzPort)
#
# NOTE: 本文件必须存 UTF-8 **带 BOM** —— Windows PowerShell 5.1 读无 BOM 的 .ps1
#       会按 GBK 解析，中文注释会变成乱码并导致语法错误（本项目已踩过两次）。

function Get-AtzPort {
    param([string]$Default = 'COM3')

    # 2) 环境变量
    if ($env:ATZ_PORT) { return $env:ATZ_PORT.Trim() }

    # 3) 工作区根目录的 PORT.txt（第一行非注释内容）
    $root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
    $portFile = Join-Path $root 'PORT.txt'
    if (Test-Path $portFile) {
        $line = Get-Content $portFile -ErrorAction SilentlyContinue |
                Where-Object { $_ -and (-not $_.TrimStart().StartsWith('#')) } |
                Select-Object -First 1
        if ($line) { return $line.Trim() }
    }

    # 4) 兜底
    return $Default
}
