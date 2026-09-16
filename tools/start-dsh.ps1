# start-dsh.ps1 -- 带护栏地启动 dsh（会话防丢）
#
# 为什么需要它（2026-09-16 排查结论）：
#   dsh 的会话日志 session.v3.jsonl.zstd 是"多段 zstd 帧拼接"的追加日志，
#   每写一批追加一个新帧。进程被杀时最后一帧可能只写了一半（源码里叫 torn tail）：
#     * 残缺在文件末尾 → dsh 下次追加时会自动截掉，一般能恢复；
#     * 坏帧落在文件中间 → 加载时直接抛
#         "corrupt Zstandard session log: invalid frame magic at byte N"
#       → 这个会话读不出来 = 你看到的"启动报错 + 历史记录不见了"。
#   而且 dsh **没有任何修复命令**，那个修复只在"再次追加"时才触发。
#
# 这个脚本在启动前做三件事：
#   ① 体检 + 自动修复本工作区的会话文件（截断到最后一个完整帧，先备份 .bak）
#   ② 警告是否有残留的 dsh/node 进程还占着会话（写锁是 Windows 内核命名信号量，
#      进程活着就不放 → 新实例会报 already owned）
#   ③ 把当前会话日志复制一份到 backup\sessions\<时间戳>\（崩溃了也能回溯）
#
# 用法：start-dsh.cmd            （同目录的 .cmd 只是双击入口）
#       powershell -File tools\start-dsh.ps1 -NoBackup -DryRun

param(
    [string]$Workspace = 'D:\AtzGauge',
    [switch]$NoBackup,
    [switch]$DryRun,
    [switch]$NoRepair,
    [switch]$NoLaunch
)

$ErrorActionPreference = 'Continue'
$gitless = $true

function Say($msg, $color = 'Gray') { Write-Host $msg -ForegroundColor $color }

Say ("=" * 74) 'DarkGray'
Say " dsh 启动护栏 — $Workspace" 'Cyan'
Say ("=" * 74) 'DarkGray'

# ── ① 会话体检 + 修复 ──────────────────────────────────────────────────────
$checker = Join-Path $Workspace 'tools\check-sessions.mjs'
if (-not $NoRepair -and (Test-Path $checker)) {
    Say "`n[1/3] 会话日志体检…" 'White'
    if ($DryRun) {
        Say "  (DryRun) 将执行: node `"$checker`" --repair" 'DarkGray'
    } else {
        $out = & node $checker --repair 2>&1
        $bad = $out | Select-String -Pattern '^\s*✗' 
        $sum = $out | Select-String -Pattern '汇总：' | Select-Object -Last 1
        if ($bad) {
            Say "  发现并处理了异常会话：" 'Yellow'
            $bad | ForEach-Object { Say "    $_" 'Yellow' }
        }
        if ($sum) { Say "  $($sum.Line.Trim())" 'Green' }
    }
} else {
    Say "`n[1/3] 跳过会话体检" 'DarkGray'
}

# ── ② 残留进程检查 ────────────────────────────────────────────────────────
Say "`n[2/3] 检查是否有残留的 dsh 进程…" 'White'
$procs = Get-CimInstance Win32_Process -Filter "Name='node.exe'" -ErrorAction SilentlyContinue |
         Where-Object { $_.CommandLine -match 'deepseek-ai\\dsh' }
if ($procs) {
    $servers = $procs | Where-Object { $_.CommandLine -match 'dsh\\lib\\bin\.js' }
    $workers = $procs | Where-Object { $_.CommandLine -notmatch 'dsh\\lib\\bin\.js' }
    if ($servers) {
        Say "  发现 $(@($servers).Count) 个 dsh 服务进程（★ 持有会话写锁的就是它们）：" 'Yellow'
        foreach ($p in $servers) { Say ("    PID {0,-7} {1}" -f $p.ProcessId, 'dsh ...\bin.js web') 'Yellow' }
        Say "  → 若确认是上次没退干净的残留，先结束它；否则新实例会报 'session already owned' 而打不开历史。" 'Yellow'
        Say "    结束命令: Stop-Process -Id <PID>" 'DarkGray'
    }
    if ($workers) { Say "  另有 $(@($workers).Count) 个 dsh 子进程（worker，随主进程结束而退出，无需处理）" 'DarkGray' }
} else {
    Say "  没有残留 dsh 进程 ✓" 'Green'
}

# ── ③ 备份当前会话日志 ────────────────────────────────────────────────────
Say "`n[3/3] 备份会话日志…" 'White'
if ($NoBackup) {
    Say "  已按 -NoBackup 跳过" 'DarkGray'
} else {
    # 会话目录名是 dsh 自己按路径"消毒"出来的（例如 D:\AtzGauge → --D-AtzGauge--，
    # 非 ASCII 会变成 ~HEX，规则可能随版本变）→ **不推算**，直接按工作区末级目录名去匹配。
    $leaf = Split-Path $Workspace -Leaf
    $sessRoot = Join-Path $env:USERPROFILE '.dsh\sessions'
    $sessDir = $null
    if (Test-Path $sessRoot) {
        $hit = Get-ChildItem $sessRoot -Directory | Where-Object { $_.Name -like "*$leaf*" } | Select-Object -First 1
        if ($hit) { $sessDir = $hit.FullName }
    }
    if (-not $sessDir) {
        Say "  没找到本工作区的会话目录（在 $sessRoot 下按 `"$leaf`" 匹配）→ 跳过" 'DarkGray'
    } else {
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        $dest = Join-Path $Workspace "backup\sessions\$stamp"
        if ($DryRun) {
            Say "  (DryRun) 将把 $sessDir 下的会话文件复制到 $dest" 'DarkGray'
        } else {
            New-Item -ItemType Directory -Path $dest -Force | Out-Null
            $files = Get-ChildItem $sessDir -Recurse -File -Filter 'session.v3.jsonl.zstd'
            $total = 0
            foreach ($f in $files) {
                $sub = Split-Path $f.DirectoryName -Leaf
                Copy-Item $f.FullName (Join-Path $dest "$sub.jsonl.zstd") -Force
                $total += $f.Length
            }
            Say ("  已备份 {0} 个会话文件，共 {1:N2} MB → {2}" -f $files.Count, ($total / 1MB), $dest) 'Green'
        }
    }
}

# ── 启动 dsh ──────────────────────────────────────────────────────────────
Say "`n" 'DarkGray'
if ($DryRun) {
    Say "DryRun：不启动 dsh。正常用法是 start-dsh.cmd（或去掉 -DryRun）。" 'Cyan'
    exit 0
}
if ($NoLaunch) {
    Say "-NoLaunch：维护步骤已完成，不启动 dsh。" 'Cyan'
    exit 0
}
Say "启动 dsh web …（本窗口保持打开即为 dsh 进程）" 'Cyan'
Set-Location $Workspace
& dsh web
