// prune-backup.mjs -- backup\ 目录瘦身：把"过时的"移进 backup\archive\<年-月>\，不删除任何文件。
//
// 为什么需要：backup\ 40 MB 里 34 MB 是 09-13 那天的固件镜像（*.bin），
//   加上 76 个启动日志、81 张截图、以及启动护栏产生的会话备份 —— 会一直滚大。
//   这些东西是"证据"不是"资产"，归档即可，不该参与日常查看，也不该进 git（已在 .gitignore）。
//
// 保留规则（都可调，见下方 KEEP_*）：
//   *.bin / *.elf / *.map   全部归档（固件产物，需要时烧回即可）
//   boot-log-*.txt          保留最新 15 个
//   *.jpg / *.png           保留最新 30 张 + **开发参考 文档里点名引用过的**（证据照片永远留）
//   sessions\<时间戳>\      保留最新 5 次会话备份
//
// 用法：
//   node tools/prune-backup.mjs           # 空跑，只报告会移动什么
//   node tools/prune-backup.mjs --apply   # 真的移动
import { readdirSync, statSync, mkdirSync, renameSync, existsSync, readFileSync, cpSync } from 'node:fs';
import { join, extname, basename } from 'node:path';

const ROOT = process.argv.includes('--root')
  ? process.argv[process.argv.indexOf('--root') + 1]
  : 'D:\\AtzGauge';
const BACKUP = join(ROOT, 'backup');
const ARCHIVE = join(BACKUP, 'archive');
const DOCS = join(ROOT, '开发参考');
const APPLY = process.argv.includes('--apply');

const KEEP_LOGS = 15;
const KEEP_IMAGES = 30;
const KEEP_SESSION_SETS = 5;

// ── 收集文档里引用过的文件名（这些是"证据"，永不归档）────────────────────
const referenced = new Set();
if (existsSync(DOCS)) {
  for (const f of readdirSync(DOCS)) {
    if (!f.endsWith('.md')) continue;
    const text = readFileSync(join(DOCS, f), 'utf8');
    for (const m of text.matchAll(/[\w.\-\u4e00-\u9fa5]+\.(?:jpg|png|jpeg)/gi)) referenced.add(m[0].toLowerCase());
  }
}

if (!existsSync(BACKUP)) { console.log(`没有 ${BACKUP}`); process.exit(0); }

const moves = [];   // { from, to, why }
const keep = [];    // { name, why }

function plan(label, files, keepFn, whyFn) {
  const sorted = [...files].sort((a, b) => b.mtime - a.mtime);
  sorted.forEach((f, i) => {
    const k = keepFn(f, i);
    if (k) keep.push({ name: f.name, why: k });
    else moves.push({ from: f.path, to: join(ARCHIVE, monthDir(f.mtime), label, f.name), why: whyFn(f, i) });
  });
}

function monthDir(d) { return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}`; }

function stat1(p) { const s = statSync(p); return { path: p, name: basename(p), size: s.size, mtime: s.mtime }; }

const entries = readdirSync(BACKUP, { withFileTypes: true });
const rootFiles = entries.filter((e) => e.isFile()).map((e) => stat1(join(BACKUP, e.name)));
const dirs = entries.filter((e) => e.isDirectory() && e.name !== 'archive').map((e) => e.name);

// ① 固件产物：全归档
const bins = rootFiles.filter((f) => ['.bin', '.elf', '.map'].includes(extname(f.name).toLowerCase()));
plan('firmware', bins, () => null, (f) => `${(f.size / 1048576).toFixed(1)} MB 固件产物`);

// ② 启动日志：留最新 N 个
const logs = rootFiles.filter((f) => /\.(txt|log)$/i.test(f.name));
plan('logs', logs, (f, i) => (i < KEEP_LOGS ? `最新 ${KEEP_LOGS} 个日志之一` : null),
     (f, i) => `第 ${i + 1} 新的日志（只留 ${KEEP_LOGS} 个）`);

// ③ 截图：留最新 N 张 + 文档引用过的
const imgs = rootFiles.filter((f) => /\.(jpg|jpeg|png)$/i.test(f.name));
plan('shots', imgs, (f, i) => {
  if (referenced.has(f.name.toLowerCase())) return '开发参考 文档引用过的证据';
  if (i < KEEP_IMAGES) return `最新 ${KEEP_IMAGES} 张之一`;
  return null;
}, (f, i) => `第 ${i + 1} 新的截图且无文档引用`);

// ④ 会话备份目录：留最新 N 次
const sessRoot = join(BACKUP, 'sessions');
let sessDirs = [];
if (existsSync(sessRoot)) {
  sessDirs = readdirSync(sessRoot, { withFileTypes: true })
    .filter((e) => e.isDirectory())
    .map((e) => ({ name: e.name, path: join(sessRoot, e.name), mtime: statSync(join(sessRoot, e.name)).mtime }))
    .sort((a, b) => b.mtime - a.mtime);
  sessDirs.forEach((d, i) => {
    if (i < KEEP_SESSION_SETS) keep.push({ name: `sessions/${d.name}`, why: `最新 ${KEEP_SESSION_SETS} 次会话备份之一` });
    else moves.push({ from: d.path, to: join(ARCHIVE, monthDir(d.mtime), 'sessions', d.name), why: '更早的会话备份', isDir: true });
  });
}

// ── 报告 ────────────────────────────────────────────────────────────────
console.log(`backup 瘦身计划（${APPLY ? '★ 实际执行' : '空跑，不改动'}）`);
console.log(`  目录: ${BACKUP}`);
console.log(`  文档引用过的证据: ${referenced.size} 个文件名\n`);
console.log(`保留 ${keep.length} 项：`);
for (const k of keep.slice(0, 8)) console.log(`  ✓ ${k.name.padEnd(52)} ${k.why}`);
if (keep.length > 8) console.log(`  …（其余 ${keep.length - 8} 项同理）`);
console.log(`\n归档 ${moves.length} 项 → ${ARCHIVE}`);
let bytes = 0;
for (const m of moves) {
  let sz = 0;
  try { sz = m.isDir ? dirSize(m.from) : statSync(m.from).size; } catch { }
  bytes += sz;
  console.log(`  → ${m.from.slice(BACKUP.length + 1).padEnd(52)} ${(sz / 1048576).toFixed(2)} MB  ${m.why}`);
}
console.log(`\n合计可移出 ${(bytes / 1048576).toFixed(2)} MB（文件仍在 backup\\archive 下，未删除）`);

function dirSize(p) {
  let n = 0;
  for (const e of readdirSync(p, { withFileTypes: true })) {
    const q = join(p, e.name);
    n += e.isDirectory() ? dirSize(q) : statSync(q).size;
  }
  return n;
}

if (!APPLY) {
  console.log('\n这是空跑。确认无误后加 --apply 执行。');
  process.exit(0);
}
for (const m of moves) {
  mkdirSync(join(m.to, '..'), { recursive: true });
  try { renameSync(m.from, m.to); }
  catch (e) { cpSync(m.from, m.to, { recursive: true }); }
}
console.log(`\n完成：${moves.length} 项已移入 ${ARCHIVE}`);
