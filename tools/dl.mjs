// dl.mjs — 通用下载器（Node/OpenSSL）
//
// 为什么需要它：本机 PowerShell / curl.exe / .NET 全部走 Schannel，
// 实测报 `schannel: AcquireCredentialsHandle failed: SEC_E_NO_CREDENTIALS`，
// 三种方式都无法下载任何 https 资源。只有 Node(OpenSSL) 与 Python(OpenSSL) 可用。
// 这是本环境的既知坑（00 号文档里 gh-fetch.mjs 就是为绕开它而写的）。
//
// 用法：node tools/dl.mjs <url> <目标文件> [--expect-bytes=N]
// 特性：重试、断点大小校验、SHA256 输出、已存在则跳过（除非 --force）

import { writeFile, mkdir, stat } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { dirname, resolve } from 'node:path';

const [url, dest, ...flags] = process.argv.slice(2);
if (!url || !dest) {
  console.error('用法: node tools/dl.mjs <url> <目标文件> [--force] [--expect-bytes=N]');
  process.exit(2);
}
const force = flags.includes('--force');
const expectArg = flags.find((f) => f.startsWith('--expect-bytes='));
const expectBytes = expectArg ? Number(expectArg.split('=')[1]) : null;

const out = resolve(dest);

if (!force) {
  try {
    const s = await stat(out);
    if (s.size > 0 && (!expectBytes || s.size === expectBytes)) {
      console.log(`跳过（已存在 ${s.size} 字节）: ${out}`);
      process.exit(0);
    }
    console.log(`已有文件大小 ${s.size} 与期望 ${expectBytes} 不符，重新下载`);
  } catch { /* 不存在，继续 */ }
}

async function download(tries = 4) {
  let last;
  for (let i = 1; i <= tries; i++) {
    try {
      const ac = new AbortController();
      const timer = setTimeout(() => ac.abort(), 30 * 60 * 1000); // 大文件留足时间
      const r = await fetch(url, { redirect: 'follow', signal: ac.signal });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const buf = Buffer.from(await r.arrayBuffer());
      clearTimeout(timer);
      return buf;
    } catch (e) {
      last = e;
      console.error(`  第 ${i}/${tries} 次失败: ${e.name}: ${e.message}`);
      if (i < tries) await new Promise((r) => setTimeout(r, 1500 * i));
    }
  }
  throw last;
}

console.log(`下载: ${url}`);
const t0 = Date.now();
const buf = await download();
const secs = (Date.now() - t0) / 1000;

if (expectBytes && buf.length !== expectBytes) {
  console.error(`❌ 大小不符：得到 ${buf.length}，期望 ${expectBytes}`);
  process.exit(1);
}

await mkdir(dirname(out), { recursive: true });
await writeFile(out, buf);

const sha = createHash('sha256').update(buf).digest('hex');
console.log(`✅ ${out}`);
console.log(`   ${buf.length} 字节 (${(buf.length / 1048576).toFixed(1)} MB)  耗时 ${secs.toFixed(1)}s  ${(buf.length / 1048576 / secs).toFixed(1)} MB/s`);
console.log(`   sha256=${sha}`);
