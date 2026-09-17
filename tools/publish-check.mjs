// publish-check.mjs -- 推送到 GitHub 之前跑一遍：确认没有把秘密带出去
//
// 用法：
//   node tools/publish-check.mjs            # 体检（只读，不改任何东西）
//   node tools/publish-check.mjs --fix-hint # 多给一点修复建议
//
// 检查项：
//   1) 工作区是否干净（有没有忘记提交的文件）
//   2) 已跟踪文件里有没有：真口令 / 明文密码 / 内网 IP / 私密头 atz_local.h
//   3) .gitignore 是否仍然排除 atz_local.h
//   4) mods 套件快照里有没有私密头（防止经"派生副本"泄回仓库）
//   5) 跟踪内容总大小（超过 20MB 提醒，避免误把上游/产物提进来）
//
// 退出码：0 = 可以推；1 = 有问题，先按提示处理。

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const GIT = process.env.ATZ_GIT || 'D:\\esp\\mingit\\cmd\\git.exe';

// ── 要拦什么 ────────────────────────────────────────────────────────────────
// ★ 这个脚本自己也会被提进仓库，所以**不能把真口令写在这里**（否则检查工具自己就是泄漏源，
//   第一版就踩了这个坑）。做法：本地秘密从**不进 git 的文件**里读。
//
//   1) 板目录 atz_local.h 里的 ATZ_DEBUG_TOKEN —— 自动读，零配置
//   2) 本机真实网段 —— 环境变量 ATZ_PRIVATE_SUBNET（正则片段，如 "192\\.168\\.5\\."）
//   3) 额外的历史秘密 —— 环境变量 ATZ_EXTRA_SECRETS，逗号分隔
//
// 读不到就跳过对应检查（并打印提醒），不会假装通过。

const SUBNET = process.env.ATZ_PRIVATE_SUBNET || '';
const EXTRA = (process.env.ATZ_EXTRA_SECRETS || '').split(',').map((s) => s.trim()).filter(Boolean);

const BOARD_DIR = path.join(ROOT, 'xiaozhi-esp32', 'src', 'main', 'boards', 'waveshare',
  'esp32-s3-touch-lcd-1.85-atzgauge');

function localToken() {
  for (const f of ['atz_local.h', 'atz_ui_config.h']) {
    try {
      const t = fs.readFileSync(path.join(BOARD_DIR, f), 'utf8');
      const m = t.match(/#define\s+ATZ_DEBUG_TOKEN\s+"([^"]+)"/);
      if (m && m[1] && m[1] !== 'atz-local-debug') return m[1];
    } catch { /* 没有就继续 */ }
  }
  return '';
}

const TOKEN = localToken();
const PLACEHOLDER = 'atz-local-debug';
const PATTERNS = [
  { name: '调试真口令（本机 atz_local.h 的值）', re: TOKEN ? new RegExp(escapeRe(TOKEN)) : null },
  { name: '生效的 token #define（非占位值）', re: /^\s*#\s*define\s+ATZ_DEBUG_TOKEN\s+"(?!atz-local-debug")/ },
  { name: '本机网段的 IP', re: SUBNET ? new RegExp(SUBNET + '\\d+') : null },
  { name: '额外指定的秘密', re: EXTRA.length ? new RegExp(EXTRA.map(escapeRe).join('|')) : null },
  { name: '私密头本体', re: /atz_local\.h(?!\.example)/, filesOnly: true },
  // 占位值本身不算问题（它就是给公开仓库用的），但**设备上不该还在用占位值**，单独提示
];

function escapeRe(s) { return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'); }

function git(args) {
  return execFileSync(GIT, args, { cwd: ROOT, encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
}

function tracked() {
  return git(['ls-files']).split('\n').map((s) => s.trim()).filter(Boolean);
}

const problems = [];
const notes = [];

// ── 1. 工作区干净度 ─────────────────────────────────────────────────────────
const status = git(['status', '--porcelain']).split('\n').map((s) => s.trim()).filter(Boolean);
if (status.length) {
  problems.push(`工作区有 ${status.length} 项未提交：`);
  for (const s of status.slice(0, 15)) problems.push(`    ${s}`);
} else {
  notes.push('工作区干净（没有未提交改动）');
}

// ── 2/3. 已跟踪文件内容扫描 ─────────────────────────────────────────────────
const files = tracked();
let total = 0;
const sizeByTop = new Map();

for (const rel of files) {
  const abs = path.join(ROOT, rel);
  let text = '';
  try {
    const st = fs.statSync(abs);
    total += st.size;
    const top = rel.includes('/') ? rel.split('/')[0] : '(root)';
    sizeByTop.set(top, (sizeByTop.get(top) || 0) + st.size);
    if (st.size > 2 * 1024 * 1024) continue;    // 大二进制跳过文本扫描
    text = fs.readFileSync(abs, 'utf8');
  } catch { continue; }

  const lines = text.split(/\r?\n/);
  for (const p of PATTERNS) {
    if (!p.re) continue;
    if (p.filesOnly) {
      if (p.re.test(rel) && !rel.endsWith('.example')) {
        problems.push(`[${p.name}] ${rel}`);
      }
      continue;
    }
    lines.forEach((line, i) => {
      if (p.re.test(line)) {
        problems.push(`[${p.name}] ${rel}:${i + 1}: ${line.trim().slice(0, 100)}`);
      }
    });
  }
}

// 隐私检查的覆盖度（读不到秘密来源就明说，别让人以为"没报就是没问题"）
if (!TOKEN) {
  notes.push('⚠ 没读到本机 atz_local.h 的口令 —— "真口令是否泄漏"这一项没检查');
} else {
  notes.push(`已加载本机口令用于比对（值不回显，长度 ${TOKEN.length}）`);
}
if (!SUBNET) {
  notes.push('⚠ 未设置 ATZ_PRIVATE_SUBNET —— "本机网段 IP 是否泄漏"这一项没检查');
} else {
  notes.push(`已启用本机网段比对（${SUBNET}）`);
}

// ── 4. 私密头必须在 gitignore 里 ────────────────────────────────────────────
const probe = 'xiaozhi-esp32/src/main/boards/waveshare/esp32-s3-touch-lcd-1.85-atzgauge/atz_local.h';
try {
  git(['check-ignore', '-q', probe]);
  notes.push('atz_local.h 已被 .gitignore 排除');
} catch {
  problems.push('★ atz_local.h 没有被 .gitignore 排除（口令会进仓库）');
}

// ── 5. mods 套件不得含私密头 ────────────────────────────────────────────────
const kit = path.join(ROOT, 'xiaozhi-esp32', 'mods-atzgauge');
if (fs.existsSync(kit)) {
  const walk = (d) => fs.readdirSync(d, { withFileTypes: true }).flatMap((e) =>
    e.isDirectory() ? walk(path.join(d, e.name)) : [path.join(d, e.name)]);
  const leaked = walk(kit).filter((f) => /atz_local\.h$/.test(f));
  if (leaked.length) {
    problems.push('★ mods 套件里出现了私密头（会被当成新增文件提进仓库）：');
    for (const f of leaked) problems.push(`    ${path.relative(ROOT, f)}`);
  } else {
    notes.push('mods 套件里没有私密头（gen-mods-kit 的 IGNORE 生效）');
  }
}

// ── 输出 ────────────────────────────────────────────────────────────────────
console.log('\n══ 发布体检 ══\n');
for (const n of notes) console.log(`  ✓ ${n}`);
console.log(`\n  跟踪文件 ${files.length} 个，合计 ${(total / 1024 / 1024).toFixed(2)} MB`);
const top = [...sizeByTop.entries()].sort((a, b) => b[1] - a[1]).slice(0, 5);
for (const [k, v] of top) console.log(`    ${(v / 1024 / 1024).toFixed(2).padStart(7)} MB  ${k}`);

if (total > 20 * 1024 * 1024) {
  problems.push(`★ 跟踪内容 ${(total / 1024 / 1024).toFixed(1)} MB 偏大 —— 是不是把上游源码或构建产物提进来了？`);
}

if (!problems.length) {
  console.log('\n✓ 未发现隐私残留，可以推送。\n');
  process.exit(0);
}

console.log(`\n✗ 发现 ${problems.length} 个问题：\n`);
for (const p of problems) console.log(`  ${p}`);
console.log('\n处理建议：');
console.log('  · 真凭据 → 移进不进 git 的文件（如板目录 atz_local.h），并**换掉**已泄漏的那个值');
console.log('  · 内网 IP → 文档写 <设备IP>，代码用环境变量 + 公开占位值');
console.log('  · 改了 gen-mods-kit.mjs 的忽略规则后，记得重跑 node tools/gen-mods-kit.mjs\n');
process.exit(1);
