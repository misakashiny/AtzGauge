# verify-idf.ps1 -- Acceptance check for the ESP-IDF v6.1 install at D:\esp
#
# Run:
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File D:\AtzGauge\tools\verify-idf.ps1
#
# Pure ASCII on purpose (Windows PowerShell 5.1 reads .ps1 as ANSI/GBK without a UTF-8 BOM).

$ErrorActionPreference = 'Continue'

. 'D:\AtzGauge\tools\activate-idf.ps1' *> $null

function Show($label, $value) {
    Write-Host ("  {0,-22} {1}" -f $label, $value)
}

Write-Host "ESP-IDF environment check" -ForegroundColor Cyan
Write-Host ("-" * 60)

Show 'IDF_PATH'      $env:IDF_PATH
Show 'IDF_TOOLS_PATH' $env:IDF_TOOLS_PATH
Show 'PYTHONUTF8'    $env:PYTHONUTF8

$idfPy = Get-Command idf.py -ErrorAction SilentlyContinue
Show 'idf.py'        $(if ($idfPy) { $idfPy.Source } else { 'NOT FOUND' })

$gcc = Get-Command xtensa-esp32s3-elf-gcc -ErrorAction SilentlyContinue
Show 'xtensa gcc'    $(if ($gcc) { $gcc.Source } else { 'NOT FOUND' })

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
Show 'cmake'         $(if ($cmake) { $cmake.Source } else { 'NOT FOUND' })

$ninja = Get-Command ninja -ErrorAction SilentlyContinue
Show 'ninja'         $(if ($ninja) { $ninja.Source } else { 'NOT FOUND' })

$git = Get-Command git -ErrorAction SilentlyContinue
Show 'git'           $(if ($git) { $git.Source } else { 'NOT FOUND' })

Write-Host ("-" * 60)
Write-Host "ESP-IDF version:" -ForegroundColor Cyan
# IMPORTANT: call the real tools/idf.py through the IDF python.
#   The idf-exe shim (idf.py.exe) answers '--version' with ITS OWN version (v1.0.3),
#   which is misleading -- it is not the ESP-IDF version.
$verOut = (& python "$env:IDF_PATH\tools\idf.py" --version 2>&1) -join ' '
$verClean = ($verOut -replace '\s+', ' ').Trim()
Write-Host "  $verClean"

Write-Host ""
Write-Host "xtensa-esp32s3-elf-gcc --version:" -ForegroundColor Cyan
& xtensa-esp32s3-elf-gcc --version 2>&1 | Select-Object -First 1 | ForEach-Object { "  $_" }

Write-Host ""
if ($verOut -match 'ESP-IDF v6\.') {
    Write-Host "RESULT: ESP-IDF 6.x OK" -ForegroundColor Green
} else {
    Write-Host "RESULT: could not confirm ESP-IDF 6.x" -ForegroundColor Yellow
}
