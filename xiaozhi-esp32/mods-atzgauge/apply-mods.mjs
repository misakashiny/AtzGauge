// apply-mods.mjs -- 把 AtzGauge 改动重新打到一棵（升级后的）xiaozhi 源码树上
//
// 做三件事，全部幂等（已就位就跳过，不会重复插入）：
//   1) 把 new-files/ 整份拷进目标树（我们的新增文件，上游不会有冲突）
//   2) 按锚点插入三处上游文件改动（blocks/ 里存的是原文）
//   3) 跑一遍校验（等价于 verify-mods.mjs）
//
// 用法：
//   node apply-mods.mjs                                  # 默认 D:/AtzGauge/xiaozhi-esp32/src
//   node apply-mods.mjs D:/path/to/new/xiaozhi/src       # 升级后的新树
//   node apply-mods.mjs D:/path/to/new/src --dry-run     # 只看会做什么，不落盘
//
// 退出码：0 = 应用后校验通过；1 = 有块插不进去（需要人工看一眼），或校验仍有缺失

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2).filter((a) => !a.startsWith('--'));
const dryRun = process.argv.includes('--dry-run');
const target = (args[0] || 'D:/AtzGauge/xiaozhi-esp32/src').replace(/\\/g, '/');

const manifest = JSON.parse(fs.readFileSync(path.join(HERE, 'manifest.json'), 'utf8'));

console.log(`目标源码树: ${target}`);
console.log(dryRun ? '模式      : 试运行（不写盘）' : '模式      : 实际写入');
console.log('='.repeat(72));

let failures = 0;
let copied = 0;
let inserted = 0;

// ── 1. 新增文件 ─────────────────────────────────────────────────────────────
console.log('\n[1/3] 拷贝新增文件');
for (const rel of manifest.addedFiles) {
  const src = path.join(HERE, manifest.newFilesDir, rel);
  const dst = path.join(target, rel);
  const srcBuf = fs.readFileSync(src);
  if (fs.existsSync(dst) && fs.readFileSync(dst).compare(srcBuf) === 0) {
    console.log(`  = 已是最新  ${rel}`);
    continue;
  }
  if (!dryRun) {
    fs.mkdirSync(path.dirname(dst), { recursive: true });
    fs.writeFileSync(dst, srcBuf);
  }
  console.log(`  ${fs.existsSync(dst) ? '~' : '+'} ${fs.existsSync(dst) ? '覆盖' : '新增'}      ${rel}`);
  copied++;
}

// ── 2. 上游文件的三处改动 ───────────────────────────────────────────────────
console.log('\n[2/3] 插入上游文件改动');
for (const anchor of manifest.anchors) {
  const file = path.join(target, anchor.file);
  if (!fs.existsSync(file)) {
    console.log(`  ✗ ${anchor.file} 不存在，跳过 [${anchor.id}]`);
    failures++;
    continue;
  }
  let text = fs.readFileSync(file, 'utf8');
  if (text.includes(anchor.startMarker)) {
    console.log(`  = 已就位    [${anchor.id}] ${anchor.description}`);
    continue;
  }
  // 只去掉文件末尾那一个换行，**不要**用 replace(/\n+$/)：块本身可能以空行结尾
  // （那是当前树里真实存在的空行），一起吃掉就不是逐字节重建了。
  let block = fs.readFileSync(path.join(HERE, anchor.block), 'utf8');
  if (block.endsWith('\n')) block = block.slice(0, -1);
  if (block.endsWith('\r')) block = block.slice(0, -1);
  // 保持目标文件的换行风格
  const eol = text.includes('\r\n') ? '\r\n' : '\n';
  const blockText = block.split('\n').join(eol);

  const lines = text.split(/\r?\n/);
  const at = lines.findIndex((l) => l.includes(anchor.insertBefore));
  if (at < 0) {
    console.log(`  ✗ 插不进去  [${anchor.id}]：在 ${anchor.file} 里找不到锚点行`);
    console.log(`              ${anchor.insertBefore}`);
    console.log('              上游可能改了这一带的写法 —— 手动插入即可，块原文在 ' + anchor.block);
    failures++;
    continue;
  }
  // 块原文已包含前后应有的空行（见 tools/gen-mods-kit.mjs 的 padBefore/padAfter），
  // 所以这里原样插入，不再额外补空行 —— 目标是重建结果与当前树逐字节一致。
  lines.splice(at, 0, blockText);
  if (!dryRun) fs.writeFileSync(file, lines.join(eol), 'utf8');
  console.log(`  + 已插入    [${anchor.id}] ${anchor.description}  (${anchor.file}:${at + 1})`);
  inserted++;
}

// ── 3. 校验 ─────────────────────────────────────────────────────────────────
console.log('\n[3/3] 应用后校验');
if (dryRun) {
  console.log('  试运行模式，跳过（真正执行时会自动跑 verify-mods.mjs）');
} else {
  const { execFileSync } = await import('node:child_process');
  try {
    const out = execFileSync(process.execPath, [path.join(HERE, 'verify-mods.mjs'), target], {
      encoding: 'utf8',
    });
    console.log(out.split('\n').map((l) => '  ' + l).join('\n'));
  } catch (err) {
    console.log((err.stdout || '').split('\n').map((l) => '  ' + l).join('\n'));
    failures++;
  }
}

console.log('='.repeat(72));
console.log(`拷贝 ${copied} 个文件，插入 ${inserted} 处改动，失败 ${failures} 处（试运行=${dryRun}）`);
process.exit(failures === 0 ? 0 : 1);
