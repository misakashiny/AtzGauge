@echo off
REM flash-obd-gauge.bat -- Flash the obd_brz_gauge OBD dashboard firmware to COM3
REM
REM Why a .bat: the @flash_args response-file syntax plus nested PowerShell quoting through
REM tools\idf-run.ps1 is unreliable on Windows. A batch file with explicit absolute paths
REM has no quoting ambiguity. Run it through idf-run.ps1 so the IDF env (esptool) is set up:
REM
REM   powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf-run.ps1 ^
REM       -Command "cmd /c tools\flash-obd-gauge.bat"
REM
REM Addresses come from obd_brz_gauge\firmware\release\flash_address_map.txt
REM (theme-upgrade / feature-theme-partition-system branch layout):
REM   bootmedia.bin is at 0xA20000, NOT 0x620000 (0x620000 is the 4MB theme_0 slot).
REM Getting this wrong is the single easiest way to brick the boot animation.
REM
REM This REPLACES whatever firmware is on the board (including the xiaozhi/AtzGauge
REM voice firmware). To go back:
REM   tools\flash-atzgauge.bat   (rebuild first with scripts\build.py if sources changed)
REM
REM NOTE: %~dp0 is this script's own directory, so it works from any cwd.

set RELEASE=%~dp0..\obd_brz_gauge\firmware\release

python -m esptool --chip esp32s3 --port COM3 --baud 460800 ^
  --before default-reset --after hard-reset ^
  write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB ^
  0x0      "%RELEASE%\bootloader\bootloader.bin" ^
  0x8000   "%RELEASE%\partition_table\partition-table.bin" ^
  0xf000   "%RELEASE%\ota_data_initial.bin" ^
  0x20000  "%RELEASE%\obd_brz_gauge.bin" ^
  0xA20000 "%RELEASE%\bootmedia.bin"

exit /b %ERRORLEVEL%
