# idf553-run.ps1 -- Run a command inside the ESP-IDF v5.5.3 environment
#
# Why this exists:
#   obd_brz_gauge (the gauge master firmware) pins ESP-IDF 5.5.3 and is NOT compatible
#   with the v6.1 used by the xiaozhi/AtzGauge slave build. Both toolchains live side by
#   side: v6.1 in D:\esp (see tools\activate-idf.ps1) and v5.5.3 in D:\esp553.
#   The machine's execution policy blocks dot-sourcing .ps1 files, so this wrapper is
#   invoked with -ExecutionPolicy Bypass and activates the environment internally.
#
# Why the environment is applied by hand instead of calling export.ps1:
#   ESP-IDF's export.ps1 runs `python tools/activate.py --export` and then dot-sources the
#   .ps1 that activate.py prints. That indirection silently did nothing here (PATH came out
#   without ninja/cmake/xtensa-esp-elf, and cmake then failed with
#   "unable to find a build program corresponding to Ninja" + "Could NOT find Git").
#   Doing the two steps explicitly is both observable and debuggable: the script prints
#   which activation file it used and whether ninja/git are on PATH afterwards.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf553-run.ps1 `
#       -Command "idf.py build"
#
# Pure ASCII on purpose (Windows PowerShell 5.1 reads .ps1 as ANSI/GBK without a BOM).

param(
    [Parameter(Mandatory = $true)][string]$Command,
    [string]$WorkDir = 'D:\AtzGauge\obd_brz_gauge\repo'
)

$ErrorActionPreference = 'Continue'

$IDF_ROOT   = 'D:\esp553'
$IDF_DIR    = "$IDF_ROOT\esp-idf"
$TOOLS_PATH = 'D:\esp\tools'          # shared with the v6.1 install (tools are versioned)
$GIT_DIR    = 'D:\esp\mingit'
$PY         = "$TOOLS_PATH\python_env\idf5.5_py3.13_env\Scripts\python.exe"

if (-not (Test-Path "$IDF_DIR\tools\activate.py")) {
    Write-Host "ERROR: ESP-IDF v5.5.3 not found at $IDF_DIR" -ForegroundColor Red
    Write-Host "       Run tools\install-idf553.ps1 first." -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $PY)) {
    Write-Host "ERROR: python env not found at $PY" -ForegroundColor Red
    exit 1
}

chcp 65001 > $null
$env:PYTHONUTF8        = '1'
$env:PYTHONIOENCODING  = 'utf-8'
$env:IDF_TOOLS_PATH    = $TOOLS_PATH
$env:IDF_GITHUB_ASSETS = 'dl.espressif.cn/github_assets'
$env:IDF_PATH          = $IDF_DIR

# MinGit is not on PATH by default; ESP-IDF tooling expects git to be reachable.
# GIT_EXEC_PATH is required too, otherwise git cannot find git-remote-https.
if (Test-Path "$GIT_DIR\cmd") {
    if ($env:PATH -notlike '*\esp\mingit\cmd*') {
        $env:PATH = "$GIT_DIR\cmd;$GIT_DIR\mingw64\bin;$env:PATH"
    }
    $env:GIT_EXEC_PATH = "$GIT_DIR\mingw64\libexec\git-core"
}

# Apply the ESP-IDF environment explicitly (see the note at the top).
$actFile = $null
try {
    $raw = & $PY "$IDF_DIR\tools\activate.py" --export 2>$null
    $actFile = @($raw) | Where-Object { $_ -match '\.ps1\s*$' } | Select-Object -Last 1
} catch {
    Write-Host "WARN: activate.py failed: $($_.Exception.Message)" -ForegroundColor Yellow
}
if ($actFile -and (Test-Path $actFile)) {
    . $actFile
} else {
    Write-Host "ERROR: could not obtain the ESP-IDF activation script" -ForegroundColor Red
    exit 1
}

# The activation script prepends the toolchain directories with `$Env:PATH = "...;$Env:PATH"`.
# On Windows that assignment can land in a *second*, differently-cased variable while the
# process environment block keeps exposing the original `Path` to child processes. Symptom
# observed here: this wrapper reported "ninja on PATH: True", yet cmake (a child process)
# failed with "unable to find a build program corresponding to Ninja" and "Could NOT find
# Git". Other variables (IDF_PATH, IDF_TOOLS_PATH, GIT_EXEC_PATH) propagated fine -- only
# PATH was affected, which is what pointed at the case mismatch.
# Fix: pick whichever representation actually carries the IDF toolchain and write it back
# through the .NET API, which updates the real process environment block.
$idfMarker = '\tools\ninja\'
$candidate = $env:PATH
$fromNet   = [System.Environment]::GetEnvironmentVariable('Path', 'Process')
if (($candidate -notlike "*$idfMarker*") -and ($fromNet -like "*$idfMarker*")) {
    $candidate = $fromNet
}
[System.Environment]::SetEnvironmentVariable('Path', $candidate, 'Process')

$effPath  = [System.Environment]::GetEnvironmentVariable('Path', 'Process')
$hasNinja = ($effPath -like '*\tools\ninja\*')
$hasGit   = ($effPath -like '*mingit*')
Write-Host "== idf553-run ==" -ForegroundColor DarkGray
Write-Host "  idf : $IDF_DIR" -ForegroundColor DarkGray
Write-Host "  act : $actFile" -ForegroundColor DarkGray
Write-Host "  ninja on PATH: $hasNinja   git on PATH: $hasGit" -ForegroundColor DarkGray
if (-not $hasNinja) {
    Write-Host "  WARN: ninja still missing -> cmake will fail" -ForegroundColor Yellow
}

if ($WorkDir -and (Test-Path $WorkDir)) {
    Set-Location $WorkDir
} else {
    Write-Host "WARN: WorkDir '$WorkDir' not found; staying in $PWD" -ForegroundColor Yellow
}
Write-Host "  cwd : $PWD" -ForegroundColor DarkGray
Write-Host "  cmd : $Command" -ForegroundColor DarkGray
Write-Host ("-" * 70) -ForegroundColor DarkGray

Invoke-Expression $Command
$code = $LASTEXITCODE
Write-Host ("-" * 70) -ForegroundColor DarkGray
Write-Host "exit code: $code" -ForegroundColor $(if ($code -eq 0) { 'Green' } else { 'Red' })
exit $code
