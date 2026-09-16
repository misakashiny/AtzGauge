# idf-run.ps1 -- Run a command inside the ESP-IDF v6.1 environment
#
# Why this exists:
#   The machine's execution policy blocks dot-sourcing .ps1 files from a command line,
#   so the usual `. activate-idf.ps1; idf.py build` does not work directly. This wrapper
#   is invoked with -ExecutionPolicy Bypass and does the activation internally.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf-run.ps1 `
#       -WorkDir "D:\AtzGauge\xiaozhi-esp32\src" -Command "python scripts/build.py --list-boards"
#
# Pure ASCII on purpose (Windows PowerShell 5.1 reads .ps1 as ANSI/GBK without a BOM).

param(
    [Parameter(Mandatory = $true)][string]$Command,
    [string]$WorkDir = 'D:\AtzGauge\xiaozhi-esp32\src'
)

$ErrorActionPreference = 'Continue'

. 'D:\AtzGauge\tools\activate-idf.ps1' *> $null

if ($WorkDir -and (Test-Path $WorkDir)) {
    Set-Location $WorkDir
} else {
    Write-Host "WARN: WorkDir '$WorkDir' not found; staying in $PWD" -ForegroundColor Yellow
}

Write-Host "== idf-run ==" -ForegroundColor DarkGray
Write-Host "  cwd : $PWD" -ForegroundColor DarkGray
Write-Host "  cmd : $Command" -ForegroundColor DarkGray
Write-Host ("-" * 70) -ForegroundColor DarkGray

Invoke-Expression $Command
$code = $LASTEXITCODE
Write-Host ("-" * 70) -ForegroundColor DarkGray
Write-Host "exit code: $code" -ForegroundColor $(if ($code -eq 0) { 'Green' } else { 'Red' })
exit $code
