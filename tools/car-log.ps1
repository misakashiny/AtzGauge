# car-log.ps1 - in-car serial logger, pure PowerShell (no Python / no pyserial needed)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\car-log.ps1
#   powershell -ExecutionPolicy Bypass -File tools\car-log.ps1 -Port COM3 -Seconds 3600
#
# Features:
#   - auto-detects the ESP32 USB serial port (skips the motherboard's COM1)
#   - writes raw log to  <tools-parent>\backup\car-log-YYYYMMDD-HHMMSS.txt
#   - prints a clean (ANSI-stripped) copy to the console
#   - auto-reconnects when the board reboots / USB re-enumerates
#   - copy the whole tools folder to another PC and it just works
#
# NOTE: kept ASCII-only on purpose. Windows PowerShell 5.1 reads .ps1 as ANSI
# unless the file has a UTF-8 BOM, so non-ASCII text breaks the parser there.

param(
    [string]$Port,
    [int]$Seconds = 3600,
    [int]$Baud = 115200
)

$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

# ---- pick a port ----
function Get-CandidatePorts {
    $list = @()
    foreach ($n in [System.IO.Ports.SerialPort]::GetPortNames()) {
        if ($n -eq 'COM1') { continue }   # motherboard port, skip
        $list += $n
    }
    return $list
}

if (-not $Port) {
    $cands = @(Get-CandidatePorts)
    if ($cands.Count -eq 0) {
        Write-Host "No usable serial port found. Please check:"
        Write-Host "  1) the board is connected with a USB *data* cable"
        Write-Host "  2) Device Manager -> Ports (COM & LPT) shows 'USB Serial Device'"
        Write-Host ""
        Write-Host "All serial ports on this system:"
        [System.IO.Ports.SerialPort]::GetPortNames() | ForEach-Object { Write-Host "   $_" }
        exit 1
    }
    $Port = $cands[0]
    Write-Host "Auto-detected port: $Port"
    if ($cands.Count -gt 1) {
        Write-Host ("Other candidates : " + ($cands[1..($cands.Count - 1)] -join ', '))
    }
}

# ---- output file ----
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$backupDir = Join-Path (Split-Path -Parent $scriptDir) 'backup'
if (-not (Test-Path $backupDir)) { New-Item -ItemType Directory -Path $backupDir | Out-Null }
$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$logPath = Join-Path $backupDir "car-log-$stamp.txt"

Write-Host "Port     : $Port"
Write-Host "Raw log  : $logPath"
Write-Host "Duration : $Seconds s  (Ctrl+C to stop early)"
Write-Host ("-" * 60)

$deadline = (Get-Date).AddSeconds($Seconds)
$total = 0
$sp = $null
$fs = $null
$ansiRe = [regex]("$([char]27)\[[0-9;]*m")

try {
    $fs = [System.IO.File]::Open($logPath,
                                 [System.IO.FileMode]::Create,
                                 [System.IO.FileAccess]::Write,
                                 [System.IO.FileShare]::Read)

    while ((Get-Date) -lt $deadline) {

        if ($null -eq $sp) {
            try {
                $sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
                $sp.ReadTimeout  = 500
                $sp.DtrEnable    = $false
                $sp.RtsEnable    = $false
                $sp.Open()
                $sp.DiscardInBuffer()
                Write-Host ""
                Write-Host "--- [connected: $Port] ---"
            } catch {
                $sp = $null
                Start-Sleep -Milliseconds 1000
                continue
            }
        }

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
                Start-Sleep -Milliseconds 40
            }
        } catch {
            Write-Host ""
            Write-Host "--- [serial interrupted, reconnecting] ---"
            try { $sp.Close() } catch {}
            $sp = $null
            Start-Sleep -Milliseconds 1000
        }
    }
} finally {
    if ($null -ne $sp) { try { $sp.Close() } catch {} }
    if ($null -ne $fs) { try { $fs.Close() } catch {} }
}

Write-Host ""
Write-Host ("-" * 60)
Write-Host "Received $total bytes, saved to: $logPath"
