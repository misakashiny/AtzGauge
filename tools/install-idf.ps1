# install-idf.ps1 -- Install ESP-IDF v6.1 for xiaozhi-esp32 (no admin needed, self-contained in D:\esp)
#
# IMPORTANT: This file is intentionally pure ASCII.
#   Windows PowerShell 5.1 reads .ps1 files as ANSI/GBK when there is no UTF-8 BOM,
#   which corrupted Chinese literals and broke parsing (observed: "Missing closing '}'").
#   Keeping it ASCII avoids the whole class of problems.
#
# Why v6.1 and not 5.5.3:
#   xiaozhi-esp32 v2.5.0 README: "requires ESP-IDF v6.0.1 or later, v6.1 recommended, 5.x no longer supported".
#   5.5.3 is obd_brz_gauge's requirement (the gauge project) -- the two are NOT interchangeable.
#
# Why a portable git:
#   This machine has no git, and ESP-IDF cannot be installed from a ZIP alone (it needs submodules).
#
# Why downloads use node:
#   PowerShell / curl.exe / .NET all use Schannel here and fail with
#   "AcquireCredentialsHandle failed: SEC_E_NO_CREDENTIALS". Only Node (OpenSSL) works.
#
# Idempotent: every step checks whether it is already done, safe to re-run.
# Log: D:\AtzGauge\backup\idf-install.log
#
# Run with (needs write access outside the workspace because of D:\esp):
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File D:\AtzGauge\tools\install-idf.ps1

# NOTE: 'Continue' not 'Stop'.
#   With 'Stop', PowerShell treats ANY stderr output from a native command as a
#   terminating error -- git prints "Cloning into ..." on stderr, which aborted the
#   script even though the clone itself succeeded. Every native call below therefore
#   checks $LASTEXITCODE explicitly instead.
$ErrorActionPreference = 'Continue'
# Make git write everything to stdout so PowerShell never sees stderr from it.
$env:GIT_REDIRECT_STDERR = '2>&1'

$IDF_VERSION = 'v6.1'
$ESP_ROOT    = 'D:\esp'
$IDF_DIR     = "$ESP_ROOT\esp-idf"
$GIT_DIR     = "$ESP_ROOT\mingit"
$TOOLS_PATH  = "$ESP_ROOT\tools"
$LOG         = 'D:\AtzGauge\backup\idf-install.log'
$DL          = 'D:\AtzGauge\tools\dl.mjs'
$MINGIT_URL  = 'https://github.com/git-for-windows/git/releases/download/v2.55.0.windows.5/MinGit-2.55.0.5-64-bit.zip'
$MIRROR      = 'dl.espressif.cn/github_assets'

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

Log "ESP-IDF $IDF_VERSION install started" 'Green'
Log "ESP root  : $ESP_ROOT"
Log "Tools path: $TOOLS_PATH"
Log "Mirror    : $MIRROR"

# ---------- 1. directories ----------
Section 1 6 'Create directories'
foreach ($d in @($ESP_ROOT, $TOOLS_PATH)) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
    Log "OK $d"
}

# ---------- 2. portable git ----------
Section 2 6 'Obtain portable git (MinGit)'
$gitExe = "$GIT_DIR\cmd\git.exe"
if (Test-Path $gitExe) {
    Log "OK already present, skipping"
} else {
    $zip = "$ESP_ROOT\mingit.zip"
    if (-not (Test-Path $zip)) {
        Log "Downloading MinGit via node (Schannel paths are broken here) ..."
        & node $DL $MINGIT_URL $zip
        if ($LASTEXITCODE -ne 0) { throw "MinGit download failed (exit $LASTEXITCODE)" }
    }
    Log "Extracting to $GIT_DIR ..."
    Expand-Archive -Path $zip -DestinationPath $GIT_DIR -Force
    if (-not (Test-Path $gitExe)) { throw "git.exe not found after extraction" }
}
$gitVer = & $gitExe --version 2>&1
Log "OK $gitVer"

# ---------- 3. clone esp-idf ----------
Section 3 6 "Clone esp-idf $IDF_VERSION (with submodules, shallow)"
if (Test-Path "$IDF_DIR\components") {
    Log "OK already cloned, skipping"
} else {
    Log "Cloning (about 1 GB, this is the slow part) ..."
    & $gitExe clone -b $IDF_VERSION --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git $IDF_DIR 2>&1 |
        Tee-Object -FilePath $LOG -Append | ForEach-Object { $_.ToString() }
    if ($LASTEXITCODE -ne 0) { throw "git clone failed (exit $LASTEXITCODE)" }
    Log "OK clone complete"
}
Push-Location $IDF_DIR
try {
    $desc = (& $gitExe describe --tags 2>&1) -join ' '
    Log "git describe = $desc   (idf.py derives its version from this; should look like v6.1)"
} finally { Pop-Location }

# ---------- 4. toolchain ----------
Section 4 6 'Download and install toolchain (install.bat esp32s3)'
$env:IDF_TOOLS_PATH      = $TOOLS_PATH
$env:IDF_GITHUB_ASSETS   = $MIRROR
# Do NOT set IDF_PYTHON_ENV_PATH.
#   That variable must point at the venv ITSELF (e.g. ...\python_env\idf6.1_py3.13_env),
#   not at its parent. Setting it to the parent made idf_tools.py call os.listdir() on a
#   path that did not exist yet -> "FileNotFoundError: [WinError 3]".
#   Left unset, IDF derives it from IDF_TOOLS_PATH and creates it correctly -- still
#   entirely inside D:\esp, which is what we wanted.
# install.bat aborts with exit 9009 unless git is on PATH ("The following tools are
# not installed in your environment: git"). Our portable MinGit is not on PATH by default.
$env:PATH = "$GIT_DIR\cmd;$env:PATH"
$gitOnPath = (& cmd.exe /c "git --version" 2>&1) -join ' '
Log "git on PATH => $gitOnPath"
if ($gitOnPath -notmatch 'git version') { throw "git still not on PATH; install.bat will fail" }
Log "IDF_TOOLS_PATH=$env:IDF_TOOLS_PATH"
Log "IDF_GITHUB_ASSETS=$env:IDF_GITHUB_ASSETS"

$pyEnvDir = "$TOOLS_PATH\python_env"
$already  = $false
if (Test-Path $pyEnvDir) {
    # A completed env looks like python_env\idf<ver>_py<ver>_env\Scripts\python.exe
    $venvPy = Get-ChildItem $pyEnvDir -Recurse -Filter 'python.exe' -ErrorAction SilentlyContinue |
              Where-Object { $_.FullName -match 'Scripts' } | Select-Object -First 1
    if ($venvPy) { $already = $true }
}
if ($already) {
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

# ---------- 5. verify ----------
Section 5 6 'Verify: export env and query idf.py version'
$idfOut = ''
Push-Location $IDF_DIR
try {
    $idfOut = (& cmd.exe /c "call export.bat >nul 2>&1 & idf.py --version" 2>&1) -join ' | '
} finally { Pop-Location }
Log "idf.py --version => $idfOut"
if ($idfOut -match 'v6\.') {
    Log "OK ESP-IDF 6.x detected" 'Green'
} else {
    Log "WARNING: could not confirm ESP-IDF 6.x from output above" 'Yellow'
}

# ---------- 6. activation script ----------
Section 6 6 'Generate activation script'
$activate = "$ESP_ROOT\activate-idf.ps1"
$body = @(
    '# Activate ESP-IDF v6.1 environment (generated by install-idf.ps1)',
    "# Usage:  . '$activate'",
    '#',
    '# PYTHONUTF8 / chcp 65001: idf.py warns "Your environment is not configured to handle',
    '# Unicode characters" on this machine. The xiaozhi-esp32 tree contains Chinese assets and',
    '# locale data, so UTF-8 mode is set explicitly rather than relying on the console default.',
    'chcp 65001 > $null',
    '$env:PYTHONUTF8        = ''1''',
    '$env:PYTHONIOENCODING  = ''utf-8''',
    "`$env:IDF_TOOLS_PATH    = '$TOOLS_PATH'",
    "`$env:IDF_GITHUB_ASSETS = '$MIRROR'",
    "`$env:IDF_PATH          = '$IDF_DIR'",
    "& '$IDF_DIR\export.ps1'"
)
$body | Set-Content -Path $activate -Encoding utf8
Log "OK activation script: $activate"

Log ""
Log "DONE" 'Green'
Log "Next steps:"
Log "  1) . '$activate'"
Log "  2) cd D:\AtzGauge\xiaozhi-esp32\src"
Log "  3) python scripts/build.py esp32-s3-touch-lcd-1.85"
