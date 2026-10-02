# install-idf553.ps1 -- Install ESP-IDF v5.5.3 (required by obd_brz_gauge) into D:\esp553
#
# Why a second ESP-IDF:
#   xiaozhi-esp32 (the AtzGauge slave) needs v6.1 and is already installed at D:\esp.
#   obd_brz_gauge (the gauge master) pins ESP-IDF 5.5.3 and is NOT compatible with 6.x.
#   The two must live side by side.
#
# Why it shares D:\esp\tools as IDF_TOOLS_PATH:
#   Espressif explicitly supports several ESP-IDF versions sharing one IDF_TOOLS_PATH;
#   toolchain binaries are stored per-version (tools\<name>\<ver>\...), so 5.5.3 does not
#   disturb the working 6.1 install. Sharing avoids re-downloading cmake/ninja/python.
#
# Why downloads use node (tools\dl.mjs):
#   PowerShell / curl.exe / .NET use Schannel on this machine and fail with
#   "AcquireCredentialsHandle failed: SEC_E_NO_CREDENTIALS". Only Node (OpenSSL) works.
#
# Why the proxy:
#   Direct github.com:443 is reset on this machine; a local proxy listens on 127.0.0.1:7890.
#   Toolchain downloads use the Espressif China mirror instead (no proxy needed).
#
# Idempotent: every step skips when already done, safe to re-run.
# Log: D:\AtzGauge\backup\idf553-install.log
#
# NOTE: pure ASCII on purpose. Windows PowerShell 5.1 reads .ps1 as ANSI/GBK when the
# file has no UTF-8 BOM, which corrupts non-ASCII literals and breaks parsing.

$ErrorActionPreference = 'Continue'
$env:GIT_REDIRECT_STDERR = '2>&1'

$IDF_TAG     = 'v5.5.3'
$ESP_ROOT    = 'D:\esp553'
$IDF_DIR     = "$ESP_ROOT\esp-idf"
$GIT_DIR     = 'D:\esp\mingit'
$TOOLS_PATH  = 'D:\esp\tools'
$PROXY       = 'http://127.0.0.1:7890'
$MIRROR      = 'dl.espressif.cn/github_assets'
$LOG         = 'D:\AtzGauge\backup\idf553-install.log'

New-Item -ItemType Directory -Force -Path (Split-Path $LOG) | Out-Null

function Log($msg, $color = 'Gray') {
    $line = "[{0:HH:mm:ss}] {1}" -f (Get-Date), $msg
    Write-Host $line -ForegroundColor $color
    Add-Content -Path $LOG -Value $line -Encoding utf8
}
function Section($n, $total, $msg) {
    Log ""
    Log ("=" * 70)
    Log "STEP $n/$total : $msg" 'Cyan'
    Log ("=" * 70)
}

Log "ESP-IDF $IDF_TAG install started" 'Green'
Log "ESP root   : $ESP_ROOT"
Log "IDF dir    : $IDF_DIR"
Log "Tools path : $TOOLS_PATH  (shared with the v6.1 install)"
Log "Proxy      : $PROXY"
Log "Mirror     : $MIRROR"

# ---------- 1. directories ----------
Section 1 5 'Create directories'
foreach ($d in @($ESP_ROOT, $TOOLS_PATH)) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
    Log "OK $d"
}

# ---------- 2. portable git ----------
Section 2 5 'Locate portable git (reuse MinGit from the v6.1 install)'
$gitExe = "$GIT_DIR\cmd\git.exe"
if (-not (Test-Path $gitExe)) { throw "portable git not found at $gitExe -- run tools\install-idf.ps1 first" }
$gitVer = & $gitExe --version 2>&1
Log "OK $gitVer"
# MinGit needs its helper binaries reachable or it fails with
# "git: 'remote-https' is not a git command" during clone/fetch.
# Its helpers live in mingw64\libexec\git-core (exec path) and mingw64\bin.
$env:GIT_EXEC_PATH = "$GIT_DIR\mingw64\libexec\git-core"
$env:PATH = "$GIT_DIR\cmd;$GIT_DIR\mingw64\bin;$env:PATH"
Log "GIT_EXEC_PATH=$env:GIT_EXEC_PATH"

# ---------- 3. clone esp-idf ----------
Section 3 5 "Clone esp-idf $IDF_TAG (with submodules, shallow)"
if (Test-Path "$IDF_DIR\components") {
    Log "OK already cloned, skipping"
} else {
    Log "Cloning via proxy (about 1 GB, this is the slow part) ..."
    & $gitExe -c "http.proxy=$PROXY" -c "https.proxy=$PROXY" clone `
        -b $IDF_TAG --depth 1 --recursive --shallow-submodules `
        https://github.com/espressif/esp-idf.git $IDF_DIR 2>&1 |
        Tee-Object -FilePath $LOG -Append | ForEach-Object { $_.ToString() }
    if ($LASTEXITCODE -ne 0) { throw "git clone failed (exit $LASTEXITCODE)" }
    Log "OK clone complete"
}
Push-Location $IDF_DIR
try {
    $desc = (& $gitExe describe --tags 2>&1) -join ' '
    Log "git describe = $desc   (should look like v5.5.3)"
} finally { Pop-Location }

# ---------- 4. toolchain ----------
Section 4 5 'Download and install toolchain (install.bat esp32s3)'
$env:IDF_TOOLS_PATH    = $TOOLS_PATH
$env:IDF_GITHUB_ASSETS = $MIRROR
$env:PATH              = "$GIT_DIR\cmd;$env:PATH"
$gitOnPath = (& cmd.exe /c "git --version" 2>&1) -join ' '
Log "git on PATH => $gitOnPath"
if ($gitOnPath -notmatch 'git version') { throw "git not on PATH; install.bat will fail" }

# A completed env looks like python_env\idf5.5_py3.13_env\Scripts\python.exe
$wantEnv = Join-Path $TOOLS_PATH 'python_env\idf5.5_py3.13_env\Scripts\python.exe'
if (Test-Path $wantEnv) {
    Log "OK python env already present, skipping toolchain install"
} else {
    Log "Running install.bat (about 1.5 GB, may take 10-30 minutes) ..."
    Push-Location $IDF_DIR
    try {
        & cmd.exe /c "install.bat esp32s3" 2>&1 |
            Tee-Object -FilePath $LOG -Append | ForEach-Object { $_.ToString() }
        if ($LASTEXITCODE -ne 0) { throw "install.bat failed (exit $LASTEXITCODE)" }
    } finally { Pop-Location }
    Log "OK toolchain installed"
}

# ---------- 5. verify + activation script ----------
Section 5 5 'Verify and generate activation script'
$idfOut = ''
Push-Location $IDF_DIR
try {
    $idfOut = (& cmd.exe /c "call export.bat >nul 2>&1 & idf.py --version" 2>&1) -join ' | '
} finally { Pop-Location }
Log "idf.py --version => $idfOut"
if ($idfOut -match 'v5\.5') {
    Log "OK ESP-IDF 5.5.x detected" 'Green'
} else {
    Log "WARNING: could not confirm ESP-IDF 5.5.x from the output above" 'Yellow'
}

$activate = "$ESP_ROOT\activate-idf553.ps1"
$body = @(
    '# Activate ESP-IDF v5.5.3 environment (generated by tools\install-idf553.ps1)',
    "# Usage:  . '$activate'",
    'chcp 65001 > $null',
    '$env:PYTHONUTF8        = ''1''',
    '$env:PYTHONIOENCODING  = ''utf-8''',
    "`$env:IDF_TOOLS_PATH    = '$TOOLS_PATH'",
    "`$env:IDF_GITHUB_ASSETS = '$MIRROR'",
    "`$env:IDF_PATH          = '$IDF_DIR'",
    "if (Test-Path '$GIT_DIR\cmd') { if (`$env:PATH -notlike '*\esp\mingit\cmd*') { `$env:PATH = '$GIT_DIR\cmd;' + `$env:PATH } }",
    "& '$IDF_DIR\export.ps1'"
)
$body | Set-Content -Path $activate -Encoding utf8
Log "OK activation script: $activate"

Log ""
Log "DONE" 'Green'
