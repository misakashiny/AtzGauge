# make-laptop-kit.ps1 -- Build a copy of this workspace that runs on ANOTHER PC (e.g. a laptop).
#
# Why: this workspace is 1.34 GB, but a laptop that only needs to *flash firmware and
# capture logs in the car* needs ~66 MB (no ESP-IDF, no 735 MB dependency cache).
# This script copies exactly the right subset, then writes PORT.txt + a README so the
# target machine needs zero script edits.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-laptop-kit.ps1 -Dest E:\AtzGauge-kit
#   ... -Profile minimal   (default) flash + logs only, ~66 MB
#   ... -Profile build     + full sources + managed_components, ~850 MB (can compile)
#   ... -Profile full      + build\ (incremental build, only for the SAME path) , ~1.4 GB
#   ... -ComPort COM7      port to write into the kit's PORT.txt (default COM3)
#   ... -WhatIf            show the plan, copy nothing
#
# NOTE: pure ASCII on purpose (Windows PowerShell 5.1 without a BOM reads .ps1 as GBK).
#       Chinese text is written into the generated README/PORT.txt as UTF-8 files instead.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Dest,
    [ValidateSet('minimal', 'build', 'full')][string]$Profile = 'minimal',
    [string]$ComPort = 'COM3',
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
$Src = Split-Path -Parent $PSScriptRoot          # workspace root = parent of tools\
$Dest = $Dest.TrimEnd('\')

Write-Host ""
Write-Host "=== AtzGauge laptop kit ===" -ForegroundColor Cyan
Write-Host "  source : $Src"
Write-Host "  dest   : $Dest"
Write-Host "  profile: $Profile"
Write-Host ""

# ── 0. sanity checks ───────────────────────────────────────────────────────────
$bad = @()
if ($Dest -match '[^\x20-\x7E]') { $bad += "destination path contains non-ASCII characters" }
if ($Dest -match '\s')           { $bad += "destination path contains spaces" }
if ($Dest.ToLower().StartsWith($Src.ToLower())) { $bad += "destination is inside the source tree" }
if ($bad.Count) {
    Write-Host "REFUSING TO RUN:" -ForegroundColor Red
    foreach ($b in $bad) { Write-Host "  - $b" }
    Write-Host ""
    Write-Host "ESP-IDF's toolchain is unreliable with non-ASCII/space paths and this project's" -ForegroundColor Yellow
    Write-Host "docs record that a Chinese+space path ('D:\Atz 仪表台') broke a whole session." -ForegroundColor Yellow
    Write-Host "Pick something like  D:\AtzGauge  or  C:\AtzGauge ." -ForegroundColor Yellow
    exit 1
}

# ── 1. what to copy ────────────────────────────────────────────────────────────
# Each entry: name, source (relative), and whether the profile includes it.
$groups = @(
    @{ n = 'obd gauge firmware (release)';  p = 'obd_brz_gauge\firmware';                     prof = @('minimal','build','full') },
    @{ n = 'xiaozhi flash artifacts';       p = 'xiaozhi-esp32\src\build';                    prof = @('minimal','build','full'); files = @('xiaozhi.bin','generated_assets.bin','ota_data_initial.bin','bootloader\bootloader.bin','partition_table\partition-table.bin') },
    @{ n = 'xiaozhi merged single image';   p = 'xiaozhi-esp32\src\build';                    prof = @('build','full');           files = @('merged-binary.bin') },
    @{ n = 'tools (scripts)';               p = 'tools';                                      prof = @('minimal','build','full') },
    @{ n = 'docs';                          p = '开发参考';                                   prof = @('minimal','build','full') },
    @{ n = 'irreplaceable chip backups';    p = 'backup\archive\2026-09\firmware';            prof = @('minimal','build','full') },
    @{ n = 'workspace README/LICENSE';      p = '.';                                          prof = @('minimal','build','full'); files = @('README.md','LICENSE','.gitignore') },
    @{ n = 'obd gauge project (full)';      p = 'obd_brz_gauge';                              prof = @('build','full') },
    @{ n = 'xiaozhi source (main)';         p = 'xiaozhi-esp32\src\main';                     prof = @('build','full') },
    @{ n = 'xiaozhi managed_components';    p = 'xiaozhi-esp32\src\managed_components';       prof = @('build','full') },
    @{ n = 'xiaozhi build config';          p = 'xiaozhi-esp32\src';                          prof = @('build','full'); files = @('CMakeLists.txt','sdkconfig','sdkconfig.defaults','sdkconfig.ci','dependencies.lock','partitions.csv','scripts','version.txt') },
    @{ n = 'mods kit';                      p = 'xiaozhi-esp32\mods-atzgauge';                prof = @('build','full') },
    @{ n = 'xiaozhi build dir';             p = 'xiaozhi-esp32\src\build';                    prof = @('full') },
    @{ n = 'git history';                   p = '.git';                                       prof = @('full') }
)

function Get-GroupBytes {
    param($g)
    $sum = 0
    if ($g.files) {
        foreach ($f in $g.files) {
            $fp = Join-Path (Join-Path $Src $g.p) $f
            if (Test-Path $fp) {
                $i = Get-Item $fp -Force
                $sum += if ($i.PSIsContainer) { (Get-ChildItem $fp -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum } else { $i.Length }
            }
        }
    } elseif (Test-Path (Join-Path $Src $g.p)) {
        $sum = (Get-ChildItem (Join-Path $Src $g.p) -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    }
    return [int64]$sum
}

$plan = @()
foreach ($g in $groups) {
    if ($g.prof -notcontains $Profile) { continue }
    if (-not (Test-Path (Join-Path $Src $g.p))) { Write-Host "  skip (missing): $($g.p)" -ForegroundColor DarkYellow; continue }
    $b = Get-GroupBytes $g
    $plan += [PSCustomObject]@{ Name = $g.n; Path = $g.p; Files = $g.files; Bytes = $b }
}

$totalBytes = ($plan | Measure-Object Bytes -Sum).Sum
Write-Host "  will copy:" -ForegroundColor Cyan
foreach ($p in $plan) { Write-Host ("    {0,9:N1} MB  {1}" -f ($p.Bytes / 1MB), $p.Name) }
Write-Host ("    {0,9:N1} MB  TOTAL" -f ($totalBytes / 1MB)) -ForegroundColor Cyan
Write-Host ""

if ($WhatIf) { Write-Host "  -WhatIf: nothing copied." -ForegroundColor Yellow; exit 0 }

# free space check
$drive = (Split-Path -Qualifier $Dest)
if (-not (Test-Path $drive)) { Write-Host "Drive $drive does not exist." -ForegroundColor Red; exit 1 }
$free = (Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$drive'").FreeSpace
if ($free -lt ($totalBytes * 1.15)) {
    Write-Host ("Not enough free space on {0}: need ~{1:N0} MB, have {2:N0} MB" -f $drive, ($totalBytes * 1.15 / 1MB), ($free / 1MB)) -ForegroundColor Red
    exit 1
}

# ── 2. copy ────────────────────────────────────────────────────────────────────
if (-not (Test-Path $Dest)) { New-Item -ItemType Directory -Path $Dest -Force | Out-Null }

function Invoke-Robocopy {
    param([string]$From, [string]$To, [string[]]$Only)
    # /E all subdirs (incl. empty), /NFL /NDL quiet, /NJH /NJS no headers,
    # /R:1 /W:1 don't hang on locked files
    # NOTE: a bare list of names would be read as extra SOURCE folders, and /IF does
    # not accept paths with subdirectories -- both were real bugs here. So: copy the
    # subset file-by-file, creating destination subdirs first.
    if ($Only) {
        foreach ($rel in $Only) {
            $srcFile = Join-Path $From $rel
            if (-not (Test-Path $srcFile)) { continue }
            $dstFile = Join-Path $To $rel
            $dstDir = Split-Path -Parent $dstFile
            if (-not (Test-Path $dstDir)) { New-Item -ItemType Directory -Path $dstDir -Force | Out-Null }
            Copy-Item -LiteralPath $srcFile -Destination $dstFile -Force
        }
        return
    }
    $rcArgs = @($From, $To, '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/R:1', '/W:1')
    & robocopy @rcArgs | Out-Null
    # robocopy exit codes: 0-7 = success-ish, >=8 = real failure
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE) for $From" }
}

foreach ($item in $plan) {
    Write-Host ("  copying {0} ..." -f $item.Name) -NoNewline
    $from = Join-Path $Src $item.Path
    $to = Join-Path $Dest $item.Path
    if ($item.Files) {
        New-Item -ItemType Directory -Path $to -Force | Out-Null
        Invoke-Robocopy -From $from -To $to -Only $item.Files
    } else {
        Invoke-Robocopy -From $from -To $to
    }
    Write-Host " ok" -ForegroundColor Green
}

# ── 3. generate PORT.txt + README for the target machine ───────────────────────
$utf8 = New-Object System.Text.UTF8Encoding($true)   # BOM: Notepad/PS5.1 friendly

$portTxt = @"
# 这块工作区用哪个串口（所有脚本都读这里，改这一个文件即可）
#
# 优先级：脚本参数 -Port  >  环境变量 ATZ_PORT  >  本文件  >  COM3
# 改完存盘即可，不用动任何脚本。
$ComPort
"@
[System.IO.File]::WriteAllText((Join-Path $Dest 'PORT.txt'), $portTxt, $utf8)

$kitReadme = @"
# 笔记本/另一台电脑使用说明（由 tools\make-laptop-kit.ps1 生成）

生成时间：$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
方案：**$Profile**
串口：**$ComPort**（要改就编辑本目录的 ``PORT.txt``，所有脚本都会跟着变）

---

## 一、先改串口（必做）

打开 ``PORT.txt``，把最后一行的串口改成你笔记本上的实际串口，例如 ``COM5``。
怎么找：设备管理器 → 端口(COM 和 LPT) → 看 ``USB 串行设备 (COMx)``。

> 板子走 ESP32-S3 原生 USB（VID 303A），笔记本上一般免驱。
> **如果插上后设备管理器里没有串口**，去微雪官网装 USB 转串口驱动（CH343），
> 这一步最好出发前先试一次，别到了车上才发现连不上。

## 二、这台机器能做什么

| 想做 | 需要装 | 怎么做 |
|---|---|---|
| **抓串口日志** | 什么都不用装 | ``powershell -NoProfile -ExecutionPolicy Bypass -File tools\boot-log.ps1 -Seconds 60`` |
| **刷 OBD 仪表固件** | Python + ``pip install esptool`` | ``powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf-run.ps1 -Command "cmd /c tools\flash-obd-gauge.bat"`` |
| **刷回小智语音固件** | 同上 | 把上面命令里的 ``flash-obd-gauge.bat`` 换成 ``flash-atzgauge.bat`` |
| 换表情包 / 跑模拟台 | Node.js | ``tools\emoji-kit.cmd`` / ``tools\sim-console.cmd`` |
| **改代码重新编译** | ESP-IDF v6.1（需重新安装，约 6.75 GB） | 本方案（$Profile）$(if ($Profile -eq 'minimal') { '**不含**依赖缓存与 build，需要先跑一次 install.ps1 再全量编译' } else { '源码与依赖齐全，装好 IDF 后可直接 idf.py build' }) |

> ⚠️ **ESP-IDF 不能从台式机复制**（里面是编译好的工具链二进制 + 路径烙死），
> 必须在笔记本上重新跑一次官方 ``install.ps1``，而且它对**非 ASCII / 带空格的路径**很敏感 ——
> 所以这个工作区被放在 ``$Dest``（纯 ASCII、无空格），**别挪到中文路径下**。

## 三、上车时的推荐流程

1. 熄火，把 OBD 适配器插到车上 OBD 口
2. 板子用 USB 车充供电（**不要**从 OBD 取 12V 直接接板子）
3. 先抓日志确认它起来了：``boot-log.ps1 -Seconds 60``
4. 日志里应能看到 ``ESP-NOW MASTER up ... ch1``（主表）或从表的接收日志
5. 细节照 ``开发参考\15-上车首测清单.md`` 走

## 四、这个包里有什么

$(($plan | ForEach-Object { "- ``$($_.Path)`` — $($_.Name)（$([math]::Round($_.Bytes/1MB,1)) MB）" }) -join "`n")

特别提醒：``backup\archive\2026-09\firmware\`` 里那两个 16 MB 的整片备份
（``factory-16MB.bin`` 出厂固件、``obd-gauge-current-before-xiaozhi-16MB.bin``）
是**不可再生**的，别删。
"@
[System.IO.File]::WriteAllText((Join-Path $Dest '笔记本使用说明.md'), $kitReadme, $utf8)

# ── 4. verify the copy ─────────────────────────────────────────────────────────
Write-Host ""
Write-Host "  verifying ..." -ForegroundColor Cyan
$fail = 0
foreach ($item in $plan) {
    $from = Join-Path $Src $item.Path
    $to = Join-Path $Dest $item.Path
    if ($item.Files) {
        $sf = 0; $sb = 0
        foreach ($f in $item.Files) {
            $fp = Join-Path $from $f
            if (Test-Path $fp) {
                $i = Get-Item $fp -Force
                if ($i.PSIsContainer) { $sf += (Get-ChildItem $fp -Recurse -File -Force | Measure-Object).Count; $sb += (Get-ChildItem $fp -Recurse -File -Force | Measure-Object Length -Sum).Sum }
                else { $sf += 1; $sb += $i.Length }
            }
        }
        $df = 0; $db = 0
        foreach ($f in $item.Files) {
            $fp = Join-Path $to $f
            if (Test-Path $fp) {
                $i = Get-Item $fp -Force
                if ($i.PSIsContainer) { $df += (Get-ChildItem $fp -Recurse -File -Force | Measure-Object).Count; $db += (Get-ChildItem $fp -Recurse -File -Force | Measure-Object Length -Sum).Sum }
                else { $df += 1; $db += $i.Length }
            }
        }
    } else {
        $sf = (Get-ChildItem $from -Recurse -File -Force | Measure-Object).Count
        $sb = (Get-ChildItem $from -Recurse -File -Force | Measure-Object Length -Sum).Sum
        $df = (Get-ChildItem $to -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object).Count
        $db = (Get-ChildItem $to -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    }
    $ok = ($sf -eq $df) -and ($sb -eq $db)
    if (-not $ok) { $fail++ }
    Write-Host ("    {0}  {1,5}/{2,5} files  {3,8:N1}/{4,8:N1} MB  {5}" -f `
        $(if ($ok) { 'OK  ' } else { 'FAIL' }), $df, $sf, ($db / 1MB), ($sb / 1MB), $item.Name) `
        -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
}

$kitBytes = (Get-ChildItem $Dest -Recurse -File -Force | Measure-Object Length -Sum).Sum
Write-Host ""
if ($fail -eq 0) {
    Write-Host ("  DONE: {0}  ({1:N1} MB, {2} files)" -f $Dest, ($kitBytes / 1MB), (Get-ChildItem $Dest -Recurse -File -Force | Measure-Object).Count) -ForegroundColor Green
    Write-Host "  next: on the other PC, edit PORT.txt then read 笔记本使用说明.md" -ForegroundColor Cyan
} else {
    Write-Host "  $fail group(s) FAILED verification - re-run or copy manually." -ForegroundColor Red
    exit 1
}
