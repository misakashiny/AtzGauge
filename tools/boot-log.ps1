# boot-log.ps1 -- Reset the board and capture serial output FROM THE VERY FIRST LINE.
#
# Why this exists (and why car-log.ps1 is not enough):
#   car-log.ps1 only *attaches* to COM3. To see a boot banner you must reset the board
#   AFTER the logger owns the port -- but whoever holds COM3 blocks esptool from
#   resetting it ("port is busy"). Two processes cannot share it.
#   So the reset has to be done BY the process holding the port. That is what this
#   script does: it asserts RTS (wired to EN on the board) itself, then reads.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\boot-log.ps1 -Seconds 100
#
# Output: console + backup\boot-log-YYYYMMDD-HHMMSS.txt
#
# NOTE: pure ASCII on purpose. Windows PowerShell 5.1 reads .ps1 as ANSI/GBK when the
# file has no UTF-8 BOM, which corrupts non-ASCII literals and breaks parsing.
#
# Port resolution (so another PC needs no script edits): -Port argument, then
# $env:ATZ_PORT, then <workspace>\PORT.txt, then COM3. See tools\atz-port.ps1.
# NOTE: param() must be the first statement in the script, so the port is resolved
# right after it (dot-sourcing the helper above param() would be a parse error).

param(
    [string]$Port,
    [int]$Seconds = 100,
    [int]$Baud = 115200,
    [int]$ResetDelayMs = 300,
    [switch]$NoReset        # attach only, do NOT pulse RTS -- inspect a running device
)

. "$PSScriptRoot\atz-port.ps1"
if (-not $Port) { $Port = Get-AtzPort }

$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$backupDir = Join-Path (Split-Path -Parent $scriptDir) 'backup'
if (-not (Test-Path $backupDir)) { New-Item -ItemType Directory -Path $backupDir | Out-Null }
$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$logPath = Join-Path $backupDir "boot-log-$stamp.txt"

Write-Host "Port     : $Port"
Write-Host "Raw log  : $logPath"
Write-Host "Duration : $Seconds s"
Write-Host ("-" * 60)

$sp = $null
$fs = $null
$total = 0
$ansiRe = [regex]("$([char]27)\[[0-9;]*m")
$deadline = (Get-Date).AddSeconds($Seconds)

try {
    $sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
    $sp.ReadTimeout  = 500
    $sp.DtrEnable    = $false
    $sp.RtsEnable    = $false
    $sp.Open()

    # --- hardware reset: pulse RTS (EN) low, then release ---
    $sp.DtrEnable = $false
    if ($NoReset) {
        Write-Host "--- [attach only, no reset -- reading a RUNNING device] ---"
    } else {
        $sp.RtsEnable = $true
        Start-Sleep -Milliseconds $ResetDelayMs
        $sp.RtsEnable = $false
        Write-Host "--- [reset pulsed, capturing from boot] ---"
    }

    $fs = [System.IO.File]::Open($logPath,
                                 [System.IO.FileMode]::Create,
                                 [System.IO.FileAccess]::Write,
                                 [System.IO.FileShare]::Read)

    while ((Get-Date) -lt $deadline) {
        try {
            $avail = $sp.BytesToRead
            if ($avail -gt 0) {
                $buf = New-Object byte[] $avail
                $n = $sp.Read($buf, 0, $avail)
                if ($n -gt 0) {
                    $total += $n
                    $fs.Write($buf, 0, $n)
                    $fs.Flush()
                    $text = [System.Text.Encoding]::UTF8.GetString($buf, 0, $n)
                    Write-Host -NoNewline $ansiRe.Replace($text, '')
                }
            } else {
                Start-Sleep -Milliseconds 30
            }
        } catch {
            Write-Host ""
            Write-Host "--- [read error: $($_.Exception.Message)] ---"
            Start-Sleep -Milliseconds 500
        }
    }
} finally {
    if ($null -ne $sp) { try { $sp.Close() } catch {} }
    if ($null -ne $fs) { try { $fs.Close() } catch {} }
}

Write-Host ""
Write-Host ("-" * 60)
Write-Host "Received $total bytes, saved to: $logPath"
