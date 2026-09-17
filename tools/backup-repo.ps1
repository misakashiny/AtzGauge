# backup-repo.ps1 -- 把 D:\AtzGauge 仓库备份到独立物理盘（E:）的裸仓库
#
# 为什么要它：这套项目（22+ 篇文档、工具链、板型源码、全部踩坑记录）以前**只在本机**，
# 硬盘坏了就全没了。本脚本把它推到一个**独立物理盘**上的裸仓库：
#   D:\AtzGauge  →  E:\backup\AtzGauge.git   （E: 与 D: 是两块不同的 NVMe）
#
# 用法（在 D:\AtzGauge 下）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\backup-repo.ps1
#   powershell ... -File tools\backup-repo.ps1 -Message "feat: 加了数据落盘"
#   powershell ... -File tools\backup-repo.ps1 -Check        # 只体检，不改动
#   powershell ... -File tools\backup-repo.ps1 -NoCommit     # 只推送已有提交
#
# ★ 这个 .ps1 里有中文，**必须存成 UTF-8 带 BOM**（PowerShell 5.1 否则乱码）。

[CmdletBinding()]
param(
    [string]$Message = "",
    [switch]$Check,
    [switch]$NoCommit,
    [string]$Remote = "E:\backup\AtzGauge.git",
    [string]$Repo = "D:\AtzGauge"
)

$ErrorActionPreference = "Stop"
$Git = "D:\esp\mingit\cmd\git.exe"      # 本机 git 不在 PATH 里，用绝对路径
if (-not (Test-Path $Git)) { $Git = "git" }   # 万一以后进了 PATH

function Say($t) { Write-Host $t }
function Bad($t) { Write-Host $t -ForegroundColor Red }
function Ok($t) { Write-Host $t -ForegroundColor Green }

Say ""
Say "=================================================================="
Say " 仓库备份 -> $Remote"
Say "=================================================================="

# ── 0. 前提检查 ─────────────────────────────────────────────────────────────
if (-not (Test-Path (Join-Path $Repo ".git"))) { Bad "找不到仓库：$Repo"; exit 1 }

if (-not (Test-Path $Remote)) {
    Bad "备份仓库不存在：$Remote"
    Say "  先建一个裸仓库："
    Say "    & '$Git' init --bare --initial-branch=master `"$Remote`""
    exit 1
}

# 备份盘是不是独立物理盘？（同一块盘上备份挡不住盘坏）
try {
    $dSrc = (Get-Partition -DriveLetter ([System.IO.Path]::GetPathRoot($Repo)[0])).DiskNumber
    $dDst = (Get-Partition -DriveLetter ([System.IO.Path]::GetPathRoot($Remote)[0])).DiskNumber
    if ($dSrc -eq $dDst) {
        Write-Host "  ⚠ 源和备份在**同一块物理盘**（Disk $dSrc）—— 挡不住盘坏，只挡误删" -ForegroundColor Yellow
    } else {
        Ok  "  ✓ 跨物理盘备份（源 Disk $dSrc -> 备份 Disk $dDst）"
    }
} catch { Say "  (跳过物理盘检查：$($_.Exception.Message))" }

# ── 1. 本地状态 ─────────────────────────────────────────────────────────────
$dirty = & $Git -C $Repo status --porcelain
$head  = (& $Git -C $Repo rev-parse master).Trim()
Say ""
Say "[1/4] 本地状态"
Say "  HEAD   : $($head.Substring(0,7))"
if ($dirty) {
    $n = @($dirty).Count
    Say "  未提交 : $n 个文件有改动"
    if ($Check) { Say "  （-Check：只体检）" }
} else {
    Ok  "  未提交 : 无（工作区干净）"
}

if ($Check) {
    Say ""
    Say "[2/4] 备份端状态"
    $remoteHead = (& $Git -C $Remote rev-parse master 2>$null)
    Say "  备份HEAD: $(if($remoteHead){$remoteHead.Substring(0,7)}else{'(空)'})"
    if ($remoteHead -and $remoteHead.Trim() -eq $head) { Ok "  ✓ 与本地一致，无需备份" }
    else { Write-Host "  ⚠ 与本地不一致 -> 需要跑一次备份（去掉 -Check）" -ForegroundColor Yellow }
    exit 0
}

# ── 2. 提交（可选）───────────────────────────────────────────────────────────
Say ""
Say "[2/4] 提交本地改动"
if ($NoCommit) {
    Say "  （-NoCommit：跳过，只推已有提交）"
} elseif (-not $dirty) {
    Say "  没有需要提交的改动"
} else {
    $msg = $Message
    if ([string]::IsNullOrWhiteSpace($msg)) {
        $msg = "wip: 自动备份 " + (Get-Date -Format "yyyy-MM-dd HH:mm")
    }
    & $Git -C $Repo add -A
    & $Git -C $Repo commit -q -m $msg
    if ($LASTEXITCODE -ne 0) { Bad "  提交失败"; exit 1 }
    Ok  "  ✓ 已提交：$msg"
}

# ── 3. 推送 ─────────────────────────────────────────────────────────────────
Say ""
Say "[3/4] 推送到备份盘"
$out = & $Git -C $Repo push origin master 2>&1
$out | ForEach-Object { Say "  $_" }
if ($LASTEXITCODE -ne 0) { Bad "  推送失败 —— 上面有原因"; exit 1 }

# ── 4. 校验（备份最容易被忽略的一步）────────────────────────────────────────
Say ""
Say "[4/4] 校验备份"
$head2   = (& $Git -C $Repo rev-parse master).Trim()
$remote2 = (& $Git -C $Remote rev-parse master 2>$null)
if ($remote2) { $remote2 = $remote2.Trim() }
$nLocal  = (& $Git -C $Repo rev-list --count master).Trim()
$nRemote = (& $Git -C $Remote rev-list --count master 2>$null)

Say "  本地  : $($head2.Substring(0,7))  ($nLocal 条历史)"
Say "  备份  : $(if($remote2){$remote2.Substring(0,7)}else{'(空)'})  ($nRemote 条历史)"
if ($remote2 -eq $head2) {
    Ok  "  ✓ 备份与本地完全一致"
} else {
    Bad "  ✗ 不一致！备份没成功"
    exit 1
}

$sizeMB = [math]::Round(((Get-ChildItem $Remote -Recurse -File | Measure-Object Length -Sum).Sum / 1MB), 1)
Say "  备份体积: $sizeMB MB"
Say ""
Ok "备份完成。"
