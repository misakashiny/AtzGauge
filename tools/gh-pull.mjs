// gh-pull.mjs — 拉取 obd_brz_gauge 的 UI 源码到本地，便于检索导航路径
//
// 用法：node tools/gh-pull.mjs

import { writeFile, mkdir } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';

const RAW = 'https://raw.githubusercontent.com/steveEcode/obd_brz_gauge/main';
const DEST = 'D:\\AtzGauge\\obd_brz_gauge\\src';

const SCREENS = [
  'BLEScan', 'ChartAlarm', 'ChartConfig', 'EasterEgg', 'Gear', 'Info', 'InfoCustom',
  'Intro', 'Logo', 'MultiGauge', 'Needle', 'ODBProtocal', 'OTAMode', 'OilPressure',
  'OilWarn', 'Rpm', 'RpmWarn', 'Settings', 'Speed', 'Temp', 'TempCustom',
  'ThemeGauge',
];

const PATHS = [
  ...SCREENS.map((s) => `main/export_path/screens/ui_ScreenPage${s}.c`),
  'main/export_path/ui.c',
  'main/export_path/ui.h',
  'main/export_path/ui_ext.c',
];

let ok = 0;
for (const p of PATHS) {
  const out = resolve(`${DEST}\\${p.replace(/^main\/export_path\//, '')}`);
  try {
    const res = await fetch(`${RAW}/${p}`, { redirect: 'follow' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const buf = Buffer.from(await res.arrayBuffer());
    await mkdir(dirname(out), { recursive: true });
    await writeFile(out, buf);
    ok++;
  } catch (e) {
    console.log(`FAIL ${p} :: ${e.message}`);
  }
}
console.log(`已拉取 ${ok}/${PATHS.length} 个文件到 ${DEST}`);
