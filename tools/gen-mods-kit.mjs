// gen-mods-kit.mjs -- 生成「AtzGauge xiaozhi 改动套件」
//
// 产物（全部落在 mods-atzgauge/ 下）：
//   modified-upstream.patch  对上游原版文件的最小 diff（可 git apply -p1）
//   new-files/               我们新增的全部文件快照（整份拷贝，零冲突）
//   manifest.json            机器可读清单（verify-mods.mjs 用）
//
// 用法：node gen-mods-kit.mjs
// 依赖：git（用 D:\esp\mingit\cmd\git.exe；没有 git 时跳过 patch 生成但不报错）

import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';

const CUR = 'D:/AtzGauge/xiaozhi-esp32/src';
const UPSTREAM = 'D:/AtzGauge/upstream/xiaozhi-esp32-2.5.0';
const KIT = 'D:/AtzGauge/xiaozhi-esp32/mods-atzgauge';
const GIT = 'D:/esp/mingit/cmd/git.exe';

// 与上游对比时要忽略的生成物 / 缓存
const IGNORE = [
  /^build\//, /^managed_components\//, /^sdkconfig/, /^dependencies\.lock$/,
  /^main\/assets\/lang_config\.h$/, /^scripts\/__pycache__\//, /\.pyc$/,
  // ★ 本机私密覆盖头（含调试口令）：绝不能进套件，否则会经套件泄回仓库
  /^main\/boards\/.*\/atz_local\.h$/,
];

function walk(root, rel = '') {
  const out = [];
  for (const entry of fs.readdirSync(path.join(root, rel), { withFileTypes: true })) {
    const r = rel ? `${rel}/${entry.name}` : entry.name;
    if (IGNORE.some((re) => re.test(r))) continue;
    if (entry.isDirectory()) out.push(...walk(root, r));
    else out.push(r);
  }
  return out;
}

fs.mkdirSync(KIT, { recursive: true });

const curFiles = walk(CUR);
const upSet = new Set(fs.existsSync(UPSTREAM) ? walk(UPSTREAM) : []);

const modified = [];
const added = [];
const deleted = [];

for (const rel of curFiles) {
  const a = path.join(CUR, rel);
  if (!upSet.has(rel)) { added.push(rel); continue; }
  const b = path.join(UPSTREAM, rel);
  if (fs.readFileSync(a).compare(fs.readFileSync(b)) !== 0) modified.push(rel);
}
for (const rel of upSet) if (!curFiles.includes(rel)) deleted.push(rel);

// ── 1. patch：对每个被改动的上游文件生成 hunk ────────────────────────────────
let patch = '';
let patchOk = true;
const tmp = path.join(KIT, '.tmp-diff');
fs.rmSync(tmp, { recursive: true, force: true });
for (const rel of modified) {
  fs.mkdirSync(path.join(tmp, 'a', path.dirname(rel)), { recursive: true });
  fs.mkdirSync(path.join(tmp, 'b', path.dirname(rel)), { recursive: true });
  fs.copyFileSync(path.join(UPSTREAM, rel), path.join(tmp, 'a', rel));
  fs.copyFileSync(path.join(CUR, rel), path.join(tmp, 'b', rel));
  try {
    const out = execFileSync(GIT, ['diff', '--no-index', '--no-color', `a/${rel}`, `b/${rel}`], {
      cwd: tmp, encoding: 'utf8', maxBuffer: 64 * 1024 * 1024,
    });
    patch += out;
  } catch (err) {
    // git diff --no-index 有差异时退出码为 1，stdout 依然是有效 patch
    if (err.stdout) patch += err.stdout;
    else { patchOk = false; console.error(`! 生成 ${rel} 的 diff 失败: ${err.message}`); }
  }
}
fs.rmSync(tmp, { recursive: true, force: true });
if (patch) fs.writeFileSync(path.join(KIT, 'modified-upstream.patch'), patch, 'utf8');

// ── 2. new-files：新增文件整份快照 ──────────────────────────────────────────
const newRoot = path.join(KIT, 'new-files');
fs.rmSync(newRoot, { recursive: true, force: true });
for (const rel of added) {
  const dst = path.join(newRoot, rel);
  fs.mkdirSync(path.dirname(dst), { recursive: true });
  fs.copyFileSync(path.join(CUR, rel), dst);
}

// ── 3. blocks：把三个追加块原文抽出来，供 apply-mods.mjs 自动重打 ────────────
const blocksDir = path.join(KIT, 'blocks');
fs.rmSync(blocksDir, { recursive: true, force: true });
fs.mkdirSync(blocksDir, { recursive: true });

const blockDefs = [
  {
    id: 'cmake-board-branch',
    file: 'main/CMakeLists.txt',
    out: 'cmake-board-branch.txt',
    startMatch: 'AtzGauge 追加块 1/2：板型选择分支',
    endMatch: 'AtzGauge 追加块 1/2 结束',
    padBefore: 1, // 连同上面那行 ═ 分隔线一起取
    padAfter: 1,
  },
  {
    id: 'cmake-sources-includes',
    file: 'main/CMakeLists.txt',
    out: 'cmake-sources-includes.txt',
    startMatch: 'AtzGauge 追加块 2/2：ESP-NOW',
    endMatch: 'list(APPEND MAIN_PRIV_REQUIRES_EXTRA esp_wifi)',
    padBefore: 2, // 空行 + ═ 分隔线
    padAfter: 2,  // ═ 分隔线 + 块后的空行（当前树里 idf_component_register 前有一个空行）
  },
  {
    id: 'kconfig-board',
    file: 'main/Kconfig.projbuild',
    out: 'kconfig-board.txt',
    startMatch: 'config BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_1_85_ATZGAUGE',
    endMatch: 'firmware silently overwrite this build.',
    padBefore: 0,
    padAfter: 0,
  },
];

const blocks = [];
for (const def of blockDefs) {
  const lines = fs.readFileSync(path.join(CUR, def.file), 'utf8').split(/\r?\n/);
  const si = lines.findIndex((l) => l.includes(def.startMatch));
  const ei = lines.findIndex((l, i) => i > si && l.includes(def.endMatch));
  if (si < 0 || ei < 0) {
    console.error(`! 抽取失败: ${def.id}`);
    continue;
  }
  const from = Math.max(0, si - def.padBefore);
  const to = Math.min(lines.length - 1, ei + def.padAfter);
  const text = lines.slice(from, to + 1).join('\n');
  fs.writeFileSync(path.join(blocksDir, def.out), text + '\n', 'utf8');
  blocks.push({ id: def.id, file: def.file, file: `blocks/${def.out}`, lines: to - from + 1 });
  console.log(`  块 ${def.id}: ${to - from + 1} 行 -> ${def.out}`);
}

// ── 4. manifest.json ────────────────────────────────────────────────────────
const manifest = {
  generatedAt: new Date().toISOString(),
  baseline: '78/xiaozhi-esp32 v2.5.0 (codeload tar.gz)',
  currentTree: CUR,
  upstreamTree: UPSTREAM,
  modifiedUpstreamFiles: modified,
  addedFiles: added,
  deletedUpstreamFiles: deleted,
  patchFile: patch ? 'modified-upstream.patch' : null,
  newFilesDir: 'new-files',
  blocks,
  anchors: [
    {
      file: 'main/CMakeLists.txt',
      id: 'cmake-board-branch',
      description: 'AtzGauge 板型选择分支（追加块 1/2）',
      startMarker: 'AtzGauge 追加块 1/2：板型选择分支',
      insertBefore: 'elseif(CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_1_85)',
      block: 'blocks/cmake-board-branch.txt',
      required: true,
    },
    {
      file: 'main/CMakeLists.txt',
      id: 'cmake-sources-includes',
      description: 'ESP-NOW 源码路径 / include / esp_wifi 依赖（追加块 2/2）',
      startMarker: 'AtzGauge 追加块 2/2：ESP-NOW',
      insertBefore: 'idf_component_register(SRCS ${SOURCES}',
      block: 'blocks/cmake-sources-includes.txt',
      required: true,
    },
    {
      file: 'main/Kconfig.projbuild',
      id: 'kconfig-board',
      description: 'AtzGauge 板型 Kconfig 条目',
      startMarker: 'config BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_1_85_ATZGAUGE',
      insertBefore: 'config BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_3_49',
      block: 'blocks/kconfig-board.txt',
      required: true,
    },
  ],
  checks: [
    { file: 'main/boards/waveshare/esp32-s3-touch-lcd-1.85-atzgauge/config.json', contains: 'esp32-s3-touch-lcd-1.85-atzgauge' },
    { file: 'main/boards/waveshare/esp32-s3-touch-lcd-1.85-atzgauge/esp32-s3-touch-lcd-1.85-atzgauge.cc', contains: 'DECLARE_BOARD(AtzGaugeBoard)' },
    { file: 'main/boards/waveshare/esp32-s3-touch-lcd-1.85-atzgauge/atz_ui.cc', contains: 'atz_ui_register_themes' },
    { file: 'main/espnow_slave/espnow_link.c', contains: 'espnow_slave_start' },
  ],
};
fs.writeFileSync(path.join(KIT, 'manifest.json'), JSON.stringify(manifest, null, 2), 'utf8');

console.log('改动套件已生成 →', KIT);
console.log('  改动上游文件:', modified.length, modified);
console.log('  新增文件    :', added.length);
console.log('  上游被删文件:', deleted.length, deleted);
console.log('  patch       :', patch ? `${patch.split('\n').length} 行` : '(未生成)', patchOk ? '' : '(有失败)');
