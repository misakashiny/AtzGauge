# activate-idf.ps1 -- Activate the ESP-IDF v6.1 install at D:\esp
#
# Lives in the workspace on purpose: USING the toolchain only READS D:\esp,
# so staying inside D:\AtzGauge avoids needing any out-of-workspace write permission.
#
# Usage (dot-source it so the environment lands in your current shell):
#     . D:\AtzGauge\tools\activate-idf.ps1
#
# NOTE: pure ASCII on purpose. Windows PowerShell 5.1 reads .ps1 as ANSI/GBK when there
# is no UTF-8 BOM, which corrupts non-ASCII literals and breaks parsing.

chcp 65001 > $null

# idf.py warns "Your environment is not configured to handle Unicode characters" here.
# The xiaozhi-esp32 tree carries Chinese assets and locale data, so force UTF-8 mode
# instead of relying on the console default.
$env:PYTHONUTF8       = '1'
$env:PYTHONIOENCODING = 'utf-8'

$env:IDF_TOOLS_PATH    = 'D:\esp\tools'
$env:IDF_GITHUB_ASSETS = 'dl.espressif.cn/github_assets'   # China mirror, ~38ms measured here
$env:IDF_PATH          = 'D:\esp\esp-idf'

# MinGit is not on PATH by default; ESP-IDF tooling expects git to be reachable.
if (Test-Path 'D:\esp\mingit\cmd') {
    if ($env:PATH -notlike '*\esp\mingit\cmd*') {
        $env:PATH = "D:\esp\mingit\cmd;$env:PATH"
    }
}

& 'D:\esp\esp-idf\export.ps1'
