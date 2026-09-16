// check-sessions.mjs -- 逐帧校验 ~/.dsh/sessions 下的会话文件（按 zstd 帧格式手工走，跨帧）
//
// 为什么要自己走帧：Node 的 createZstdDecompress / zstdDecompressSync **只解第一帧**，
// 用它检查"多帧拼接"的会话文件会得出"一切正常"的假结论（我第一版就踩了这个坑）。
//
// 机制（读 dsh-session-persistence-jsonl 源码得到）：
//   * session.v3.jsonl.zstd = 多行 JSON 日志，由**多个独立 zstd 帧**拼接，每批追加 = 一个新帧。
//   * 进程被杀时最后一帧可能只写了一半 → torn tail（撕裂尾巴）。
//   * 加载时 scanZstdFrames() 逐帧扫；尾巴残缺 → 截掉即可打开；
//     但**中间**出现坏帧 → 抛 "invalid frame magic at byte N" → 该会话读不出来。
//
// 用法：
//   node tools/check-sessions.mjs            只检查
//   node tools/check-sessions.mjs --repair   把每个文件截断到"最后一个完整帧"（会先备份 .bak）
import { readdirSync, statSync, readFileSync, writeFileSync, copyFileSync, openSync, ftruncateSync, closeSync } from 'node:fs';
import { join } from 'node:path';

const home = process.env.USERPROFILE ?? '.';
const sessRoot = join(home, '.dsh', 'sessions');
const cacheRoot = join(home, '.dsh', 'storages', 'session_projcache', 'sessions');
const REPAIR = process.argv.includes('--repair');

const MAGIC = 0xfd2fb528;

/** 走完一个 zstd 帧，返回帧结束偏移；不完整/非法则返回 { bad, reason }。 */
function frameLen(buf, off) {
  if (off + 4 > buf.length) return { bad: 'truncated-magic' };
  if (buf.readUInt32LE(off) !== MAGIC) return { bad: `bad-magic@${off}` };
  let p = off + 4;
  if (p >= buf.length) return { bad: 'truncated-fhd' };
  const fhd = buf.readUInt8(p++);
  const fcsFlag = fhd >>> 6;
  const singleSegment = (fhd & 0x20) !== 0;
  const checksum = (fhd & 0x04) !== 0;
  const dictFlag = fhd & 0x03;
  if ((fhd & 0x08) !== 0) return { bad: 'reserved-bit' };
  if (!singleSegment) p += 1;                       // window descriptor
  p += [0, 1, 2, 4][dictFlag];                      // dictionary id
  p += fcsFlag === 0 ? (singleSegment ? 1 : 0) : [0, 2, 4, 8][fcsFlag];  // frame content size
  if (p > buf.length) return { bad: 'truncated-header' };
  // 数据块
  for (;;) {
    if (p + 3 > buf.length) return { bad: 'truncated-block-header', frameStart: off };
    const h = buf.readUInt8(p) | (buf.readUInt8(p + 1) << 8) | (buf.readUInt8(p + 2) << 16);
    p += 3;
    const last = h & 1;
    const type = (h >>> 1) & 3;
    const size = (h >>> 3) & 0x1fffff;
    if (type === 0) p += size;            // Raw
    else if (type === 1) p += 1;          // RLE
    else if (type === 2) p += size;       // Compressed
    else return { bad: 'reserved-block-type', frameStart: off };
    if (p > buf.length) return { bad: 'truncated-block', frameStart: off };
    if (last) break;
  }
  if (checksum) p += 4;
  if (p > buf.length) return { bad: 'truncated-checksum', frameStart: off };
  return { end: p };
}

function scan(path) {
  const buf = readFileSync(path);
  let off = 0, frames = 0, lastGood = 0;
  while (off < buf.length) {
    const r = frameLen(buf, off);
    if (r.bad) return { bytes: buf.length, frames, lastGood, bad: r.bad, badAt: off };
    off = r.end;
    frames++;
    lastGood = off;
  }
  return { bytes: buf.length, frames, lastGood, bad: null, badAt: null };
}

function walk(dir, out = []) {
  let ents = [];
  try { ents = readdirSync(dir, { withFileTypes: true }); } catch { return out; }
  for (const e of ents) {
    const p = join(dir, e.name);
    if (e.isDirectory()) walk(p, out); else out.push(p);
  }
  return out;
}

const files = walk(sessRoot).filter((f) => f.endsWith('.zstd'));
console.log(`逐帧校验 ${files.length} 个会话文件（${sessRoot}）`);
console.log(`${REPAIR ? '【修复模式】' : '【只读检查】'}\n`);
console.log('状态 会话                                                           大小      完整帧   坏点'.padEnd(96));
let bad = 0, repaired = 0;
for (const f of files) {
  const rel = f.slice(sessRoot.length + 1).replace(/\\session\.v3\.jsonl\.zstd$/, '');
  const r = scan(f);
  const ok = !r.bad && r.frames > 0;
  if (!ok) bad++;
  let note = r.bad ? `${r.bad} @ ${r.badAt}` : '';
  if (!ok && REPAIR && r.lastGood > 0) {
    copyFileSync(f, f + '.bak');
    const fd = openSync(f, 'r+');
    ftruncateSync(fd, r.lastGood);
    closeSync(fd);
    repaired++;
    note += ` → 已截断到 ${r.lastGood} B（原文件备份为 .bak）`;
  } else if (!ok && REPAIR && r.lastGood === 0) {
    note += ' → 无法修复（第一个帧就坏了）';
  }
  console.log(`${ok ? '✓' : '✗'}    ${rel.padEnd(60)} ${String(r.bytes).padStart(9)} ${String(r.frames).padStart(7)}   ${note}`);
}

let cbad = 0;
for (const f of walk(cacheRoot)) {
  try { JSON.parse(readFileSync(f, 'utf8')); } catch (e) { cbad++; console.log(`  ✗ 派生缓存损坏: ${f.slice(cacheRoot.length + 1)}  ${e.message.slice(0, 80)}`); }
}
console.log(`\n汇总：会话文件异常 ${bad} 个${REPAIR ? `（已修复 ${repaired} 个）` : ''}，派生缓存异常 ${cbad} 个`);
if (bad && !REPAIR) console.log('提示：加 --repair 可把坏文件截断到最后一个完整帧（会先写 .bak 备份）。');
