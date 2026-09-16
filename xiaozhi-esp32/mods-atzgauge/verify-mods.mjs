// verify-mods.mjs -- 校验「AtzGauge 改动」在目标源码树里是否齐全
//
// 用途：官方小智固件升级后，把我们的改动重新打上去，然后跑一遍这个脚本，
//       它会逐项报告「已就位 / 缺失」，缺失项直接告诉你要插到哪里。
//
// 用法：
//   node verify-mods.mjs                          # 校验默认路径
//   node verify-mods.mjs D:/path/to/new/xiaozhi/src
//
// 退出码：0 = 全部就位；1 = 有缺失（可直接用于 CI / 批处理判断）

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const target = (process.argv[2] || 'D:/AtzGauge/xiaozhi-esp32/src').replace(/\\/g, '/');

const manifestPath = path.join(HERE, 'manifest.json');
if (!fs.existsSync(manifestPath)) {
  console.error('找不到 manifest.json；请先运行 tools/gen-mods-kit.mjs');
  process.exit(1);
}
const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));

let missing = 0;
let ok = 0;

function read(rel) {
  const p = path.join(target, rel);
  return fs.existsSync(p) ? fs.readFileSync(p, 'utf8') : null;
}

console.log(`目标源码树: ${target}`);
console.log(`基线      : ${manifest.baseline}`);
console.log('='.repeat(72));

// ── 1. 新增文件是否齐全 ─────────────────────────────────────────────────────
console.log('\n[1/3] 新增文件（整份拷贝，不涉及上游代码）');
for (const rel of manifest.addedFiles) {
  const content = read(rel);
  if (content === null) {
    console.log(`  ✗ 缺失  ${rel}`);
    missing++;
  } else {
    const expected = fs.readFileSync(path.join(HERE, manifest.newFilesDir, rel));
    const same = Buffer.from(content).compare(expected) === 0;
    console.log(`  ${same ? '✓' : '~'} ${same ? '一致' : '存在但与基线不同'}  ${rel}`);
    ok++;
  }
}

// ── 2. 上游文件里的锚点是否都在 ─────────────────────────────────────────────
console.log('\n[2/3] 上游文件改动（锚点检查）');
for (const anchor of manifest.anchors) {
  const content = read(anchor.file);
  if (content === null) {
    console.log(`  ✗ 文件不存在  ${anchor.file}`);
    missing++;
    continue;
  }
  const present = content.includes(anchor.startMarker) &&
    (!anchor.endMarker || content.includes(anchor.endMarker));
  const anchorExists = !anchor.insertBefore || content.includes(anchor.insertBefore);
  if (present) {
    console.log(`  ✓ 已就位  [${anchor.id}] ${anchor.description}`);
    ok++;
  } else if (!anchorExists) {
    console.log(`  ✗ 缺失    [${anchor.id}] ${anchor.description}`);
    console.log(`            且插入锚点也找不到，无法自动重打：${anchor.insertBefore}`);
    console.log(`            文件：${anchor.file}`);
    missing++;
  } else {
    console.log(`  ✗ 缺失    [${anchor.id}] ${anchor.description}`);
    console.log(`            插入位置：在 ${anchor.file} 里搜到下面这行的前面`);
    console.log(`              ${anchor.insertBefore}`);
    console.log(`            内容见 new-files 同名文件 / modified-upstream.patch`);
    missing++;
  }
}

// ── 3. 关键实现是否真的在（防止拷贝了空壳） ─────────────────────────────────
console.log('\n[3/3] 关键实现抽查');
for (const check of manifest.checks) {
  const content = read(check.file);
  if (content !== null && content.includes(check.contains)) {
    console.log(`  ✓ ${check.file}  含 "${check.contains}"`);
    ok++;
  } else {
    console.log(`  ✗ ${check.file}  缺少 "${check.contains}"`);
    missing++;
  }
}

console.log('\n' + '='.repeat(72));
console.log(`结果：${ok} 项就位，${missing} 项缺失`);
if (missing === 0) {
  console.log('改动齐全。可以开始编译：');
  console.log('  powershell -NoProfile -ExecutionPolicy Bypass -File D:\\AtzGauge\\tools\\idf-run.ps1 `');
  console.log('      -Command "python scripts/build.py waveshare/esp32-s3-touch-lcd-1.85-atzgauge"');
} else {
  console.log('有缺失 —— 按上面的提示重新打上，或直接跑 apply-mods.mjs');
}
process.exit(missing === 0 ? 0 : 1);
