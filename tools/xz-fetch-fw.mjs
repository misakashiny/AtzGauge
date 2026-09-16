// xz-fetch-fw.mjs — 下载小智AI 微雪 1.85 三个版本的预编译固件（免 IDF 路线）
// 用法：node tools/xz-fetch-fw.mjs
// 产出：D:\AtzGauge\xiaozhi-esp32\firmware\vX.Y.Z_waveshare-esp32-s3-touch-lcd-1.85*.zip
//       D:\AtzGauge\xiaozhi-esp32\firmware\_provenance.json
//
// 为什么三个都下：三个版本是**不同硬件**（音频方案完全不同），
// 烧错会黑屏或没声音。三个一共约 7 MB，都留着可以现场逐个试。

import { writeFile, mkdir, readFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { resolve, join } from 'node:path';

const REPO = '78/xiaozhi-esp32';
const DEST = 'D:\\AtzGauge\\xiaozhi-esp32\\firmware';
const WANT = /^v[\d.]+_waveshare-esp32-s3-touch-lcd-1\.85[a-c]?\.zip$/;

const rel = await (async () => {
  const r = await fetch(`https://api.github.com/repos/${REPO}/releases/latest`, { redirect: 'follow' });
  if (!r.ok) throw new Error(`releases HTTP ${r.status}`);
  return await r.json();
})();

const assets = rel.assets.filter(a => WANT.test(a.name));
console.log(`release ${rel.tag_name} 匹配到 ${assets.length} 个固件\n`);

await mkdir(DEST, { recursive: true });
const saved = [];

for (const a of assets) {
  const out = resolve(join(DEST, a.name));
  let buf;
  for (let i = 1; i <= 4; i++) {
    try {
      const r = await fetch(a.browser_download_url, { redirect: 'follow' });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      buf = Buffer.from(await r.arrayBuffer());
      break;
    } catch (e) {
      if (i === 4) throw e;
      await new Promise(r => setTimeout(r, 500 * i));
    }
  }
  await writeFile(out, buf);
  const sha = createHash('sha256').update(buf).digest('hex');
  saved.push({ name: a.name, bytes: buf.length, sha256: sha });
  console.log(`✅ ${a.name}  ${(buf.length / 1048576).toFixed(2)} MB  sha256=${sha.slice(0, 16)}…`);
}

await writeFile(
  resolve(join(DEST, '_provenance.json')),
  JSON.stringify({
    repo: REPO, release: rel.tag_name, publishedAt: rel.published_at,
    fetchedAt: new Date().toISOString(), fetchedBy: 'tools/xz-fetch-fw.mjs',
    note: '微雪 ESP32-S3-Touch-LCD-1.85 三硬件版本预编译固件（免 ESP-IDF）；解压得 merged-binary.bin，烧录地址 0x0',
    files: saved,
  }, null, 2) + '\n'
);

console.log(`\n共保存 ${saved.length} 个到 ${DEST}`);
console.log(`来源记录：${join(DEST, '_provenance.json')}`);
