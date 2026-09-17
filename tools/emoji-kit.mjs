#!/usr/bin/env node
// emoji-kit.mjs -- 「表情包 → 一键打包 → 本地服务器 → 推给小智」工具
//
// 为什么要有它：换表情包不该每次重新编译 + 刷固件。小智本身支持**运行时下载 assets**：
//   ① 往设备写一个下载地址（工具 self.assets.set_download_url → NVS "assets"/"download_url"）
//   ② 设备**重启**时 CheckAssetsVersion() 读这个地址 → Assets::Download() 流式写进 assets 分区
//   ③ 下载完 Apply() → 表情/字体/皮肤立刻换成新的（重启后生效）
// 这个工具把 ① 之前的活全包了：校验素材 → 生成 index.json → 打包 assets.bin → 起本地 HTTP → 推送。
//
// 用法（在 D:\AtzGauge 下）：
//   node tools/emoji-kit.mjs --dir D:\表情包\方案A            # 打包 + 起服务器 + 推送 + 重启
//   node tools/emoji-kit.mjs --dir .\my-emoji --dry-run      # 只校验 + 打包，不推送
//   node tools/emoji-kit.mjs --dir .\my-emoji --no-push      # 打包 + 起服务器（给你自己点）
//   node tools/emoji-kit.mjs --dir .\my-emoji --serve-only   # 只起服务器（推上一次的结果）
//   node tools/emoji-kit.mjs --check D:\表情包\方案A          # 只做素材体检
//   node tools/emoji-kit.mjs --list-builtins                 # 列出 21 个必需文件名
//
// 素材要求（详见 开发参考/22-表情包素材与一键推送.md）：
//   21 个 PNG，文件名必须与内置表情**同名**（工具会报缺哪个、多哪个）；
//   推荐 128×128（≥96 且 ≤512 都接受，会给出缩放建议）；带 alpha 通道；不要白底。
//
// ★ 产物格式（与上游 scripts/build_default_assets.py 一致，本工具自己实现以便加校验）：
//   [0..3]   文件数 u32le
//   [4..7]   校验和 u16le（= 表区+数据区所有字节之和 & 0xFFFF，写在 u32 里高 16 位为 0）
//   [8..11]  表区+数据区总长 u32le
//   然后每条 44 字节：name[32] + size u32le + offset u32le + width u16le + height u16le
//   数据区：每个文件前缀 0x5A 0x5A，offset 相对"数据区起点"
//   （设备端读的时候先验证 0x5A5A，再按 table 里的 size 取数据）

import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import os from 'node:os';
import zlib from 'node:zlib';
import { execFileSync } from 'node:child_process';

// ── 参数 ────────────────────────────────────────────────────────────────────
function arg(name, dflt = null) {
  const i = process.argv.indexOf(`--${name}`);
  if (i < 0) return dflt;
  const v = process.argv[i + 1];
  return (v === undefined || v.startsWith('--')) ? true : v;
}
const has = (name) => process.argv.includes(`--${name}`);

const REPO = path.resolve(path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1')), '..');
const BOARD = path.join(REPO, 'xiaozhi-esp32/src/main/boards/waveshare/esp32-s3-touch-lcd-1.85-atzgauge');
const BUILTIN_DIR = path.join(REPO, 'xiaozhi-esp32/src/managed_components/78__xiaozhi-fonts/png/noto-color-emoji_128');
const OUT_DIR = path.join(REPO, 'backup/emoji-kit');          // 产物落这里（不进 git）
const PORT = Number(arg('port', '8124'));
const DEVICE = String(arg('device', process.env.ATZ_DEVICE || '192.168.1.100'));
const KEY = readDebugToken();

// 素材规格（与固件显示一致：128 素材 × 125% 缩放 → 屏上 160px）
const SIZE_IDEAL = 128;
const SIZE_MIN = 96;
const SIZE_MAX = 512;
const MAX_ASSETS = Number(arg('max-assets', String(8 * 1024 * 1024)));   // assets 分区 8MB

// 调试 token：优先读板目录下的私密头 atz_local.h（不进仓库），
// 没有就退回 atz_ui_config.h 里的占位值。环境变量 ATZ_DEBUG_TOKEN 优先级最高。
function readDebugToken() {
  if (process.env.ATZ_DEBUG_TOKEN) return process.env.ATZ_DEBUG_TOKEN;
  for (const f of ['atz_local.h', 'atz_ui_config.h']) {
    try {
      const h = fs.readFileSync(path.join(BOARD, f), 'utf8');
      const m = h.match(/ATZ_DEBUG_TOKEN\s+"([^"]+)"/);
      if (m && m[1]) return m[1];
    } catch { /* 文件不存在就继续找 */ }
  }
  return 'atz-local-debug';
}

function die(msg) { console.error(`\n✗ ${msg}\n`); process.exit(1); }
function kb(n) { return `${(n / 1024).toFixed(1)} KB`; }

/** 中文提示全部放在这里（不放 .cmd 里）：cmd.exe 对非 ASCII 的 .cmd 解析很脆。 */
const USAGE = `
换表情包（不用重刷固件）

  · 双击 tools\\emoji-kit.cmd  → 会问你素材文件夹路径
  · 或把素材文件夹拖到 emoji-kit.cmd 上
  · 或在这里直接敲（三种常用）：
      node tools\\emoji-kit.mjs --ask                        问我路径
      node tools\\emoji-kit.mjs --dir "D:\\表情包\\方案A"       直接推送
      node tools\\emoji-kit.mjs --builtin                    换回内置表情

  其他开关：--check 只体检 ｜ --dry-run 只打包 ｜ --allow-missing 缺的用内置补
            --no-push 只起服务器 ｜ --device <IP> 换设备 ｜ --port <n> 换端口
`;

// ── 交互式问路径（双击流程用）───────────────────────────────────────────────
async function askFolder() {
  const rl = (await import('node:readline/promises')).createInterface({
    input: process.stdin, output: process.stdout,
  });
  console.log(USAGE);
  console.log(`素材文件夹里放 PNG（推荐 128×128、透明底）。`);
  console.log(`不确定文件名？先跑一次： node tools\\emoji-kit.mjs --list-builtins`);
  console.log(`\n把文件夹拖进这个窗口，然后回车 —— 只拖文件夹，不要再加 --参数。`);
  console.log(`★ 建议用"拖进来"而不是手打：Windows 控制台对**中文路径**的输入编码不可靠`);
  console.log(`  （拖进来是命令行参数，Node 直接拿到 Unicode，不会有编码问题）。\n`);
  const ans = (await rl.question('素材文件夹路径（留空=退出）: ')).trim().replace(/^"|"$/g, '');
  rl.close();
  if (!ans) { console.log('\n没有输入路径，已退出。'); process.exit(0); }
  return ans;
}

// ── 素材清单（内置 21 个名字 = 契约）─────────────────────────────────────────
function builtinNames() {
  if (!fs.existsSync(BUILTIN_DIR)) die(`找不到内置表情目录：${BUILTIN_DIR}`);
  return fs.readdirSync(BUILTIN_DIR).filter((f) => f.toLowerCase().endsWith('.png')).sort();
}

// ── PNG 头解析（只读 IHDR，不需要任何图像库）────────────────────────────────
function readPng(file) {
  const fd = fs.openSync(file, 'r');
  try {
    const buf = Buffer.alloc(33);
    const n = fs.readSync(fd, buf, 0, 33, 0);
    if (n < 33) return { error: '文件太小，不是 PNG' };
    const sig = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    if (!buf.subarray(0, 8).equals(sig)) return { error: '不是 PNG（签名不符）' };
    if (buf.toString('ascii', 12, 16) !== 'IHDR') return { error: '缺少 IHDR' };
    const width = buf.readUInt32BE(16);
    const height = buf.readUInt32BE(20);
    const bitDepth = buf[24];
    const colorType = buf[25];
    // colorType: 3 = 调色板, 4 = 灰度+A, 6 = RGBA
    const alpha = colorType === 4 || colorType === 6;
    const interlaced = buf[28] !== 0;
    return { width, height, bitDepth, colorType, alpha, interlaced };
  } finally { fs.closeSync(fd); }
}

// ── 素材体检 ────────────────────────────────────────────────────────────────
function check(dir) {
  const want = builtinNames().map((f) => path.basename(f, '.png'));
  const wantSet = new Set(want);
  const files = fs.existsSync(dir)
    ? fs.readdirSync(dir).filter((f) => f.toLowerCase().endsWith('.png'))
    : [];
  if (files.length === 0) {
    const shown = path.resolve(String(dir));
    die(`这个文件夹里没有 PNG 文件：\n  ${shown}\n` +
        (fs.existsSync(dir)
          ? '  （目录存在，但里面没有 .png —— 素材要直接放在这个目录下，不认子目录）'
          : '  （目录不存在：路径打错了吗？注意不要把 --参数 跟路径写在同一行）') +
        `\n\n  正确做法：把文件夹拖进窗口，只拖文件夹本身，然后回车。` +
        `\n  或者用参数：node tools\\emoji-kit.mjs --dir "${shown}"`);
  }

  const got = new Map();
  const problems = [];
  const notes = [];
  for (const f of files) {
    const name = path.basename(f, '.png');
    const full = path.join(dir, f);
    const info = readPng(full);
    if (info.error) { problems.push(`${f}: ${info.error}`); continue; }
    if (!wantSet.has(name)) {
      notes.push(`${f}: 这个名字固件用不到（会一起打进包，但不会显示）；文件名若写错请改名`);
    }
    if (info.interlaced) problems.push(`${f}: 隔行扫描 PNG，LVGL 解不了，请另存为非隔行`);
    if (!info.alpha) {
      notes.push(`${f}: 没有 alpha 通道（colorType=${info.colorType}）—— 圆屏是黑底，透明背景的图才自然`);
    }
    const ratio = Math.max(info.width, info.height) / Math.min(info.width, info.height);
    if (ratio > 1.08) {
      notes.push(`${f}: ${info.width}×${info.height} 长宽差 ${((ratio - 1) * 100).toFixed(0)}%，会被拉伸（内置素材是 128×121 ≈ 6%）`);
    }
    if (info.width > SIZE_MAX || info.height > SIZE_MAX) {
      problems.push(`${f}: ${info.width}×${info.height} 超过 ${SIZE_MAX}px，assets 分区放不下多少张`);
    }
    if (info.width < SIZE_MIN) {
      notes.push(`${f}: ${info.width}px 偏小（板子是 128px 素材放大到 160px 显示），会明显发糊`);
    }
    if (info.width > SIZE_IDEAL) {
      notes.push(`${f}: ${info.width}px 比推荐值 ${SIZE_IDEAL} 大，屏上最多用到 ${SIZE_IDEAL}px（多余像素只占空间）`);
    }
    got.set(name, { file: full, raw: f, ...info, bytes: fs.statSync(full).size });
  }

  const missing = want.filter((n) => !got.has(n));
  return { want, got, missing, problems, notes };
}

function printCheck(r) {
  console.log(`素材体检：应有 ${r.want.length} 个，实到 ${r.got.size} 个`);
  if (r.missing.length) {
    console.log(`\n缺这些（设备会退回内置表情，不会崩）：\n  ${r.missing.join(', ')}`);
  }
  if (r.problems.length) {
    console.log(`\n✗ 必须修的问题（${r.problems.length}）：`);
    r.problems.forEach((p) => console.log(`  - ${p}`));
  }
  if (r.notes.length) {
    console.log(`\n提示（${r.notes.length}，不拦：只是观感/体积）：`);
    r.notes.slice(0, 12).forEach((p) => console.log(`  - ${p}`));
    if (r.notes.length > 12) console.log(`  … 还有 ${r.notes.length - 12} 条`);
  }
  const provided = r.want.filter((n) => r.got.has(n)).length;
  const extra = [...r.got.keys()].filter((n) => !r.want.includes(n)).length;
  console.log(`\n自定义素材：${provided}/${r.want.length}` +
    (extra ? `（另有 ${extra} 个名字固件用不到，会一起打包但不显示）` : '') +
    (r.problems.length ? '（先修掉上面的问题再打包）' : ''));
}

// ── 解析一个已有的 assets.bin（用来做"基线"）────────────────────────────────
// ★ 为什么需要基线：设备端 Apply() 会从**同一个 assets 分区**里读 index.json、
//   text_font、srmodels.bin。如果我们的包只有表情，那几个就全没了 ——
//   唤醒词模型会加载失败、字体回退（甚至报错）。所以做法是：
//   **以当前固件里那份 assets.bin 为基线**，只替换 21 张表情图，其余原样带上。
function parsePack(file) {
  const buf = fs.readFileSync(file);
  const count = buf.readUInt32LE(0);
  const NAME_LEN = 32, ENTRY = 44;
  const tableLen = count * ENTRY;
  const out = new Map();
  for (let i = 0; i < count; i++) {
    const p = 12 + i * ENTRY;
    const name = buf.subarray(p, p + NAME_LEN).toString('utf8').replace(/\0.*$/, '');
    const size = buf.readUInt32LE(p + NAME_LEN);
    const off = buf.readUInt32LE(p + NAME_LEN + 4);
    const start = 12 + tableLen + off;
    if (buf[start] !== 0x5a || buf[start + 1] !== 0x5a) {
      die(`基线包损坏：${name} 缺少 0x5A5A 前缀`);
    }
    out.set(name, buf.subarray(start + 2, start + 2 + size));
  }
  return out;
}

const EMOJI_RE = /\.(png|gif|jpg)$/i;

// ── 打包 assets.bin（自实现，格式见文件顶部注释）────────────────────────────
function packAssets(outFile, entries) {
  const NAME_LEN = 32;
  // 上游按 (扩展名, 基名) 排序；这样 index.json 会排在 png 前面，与设备端查找顺序无关，但保持一致
  const key = (e) => {
    const ext = path.extname(e.name).toLowerCase();
    return `${ext}\u0000${path.basename(e.name, path.extname(e.name))}`;
  };
  const sorted = [...entries].sort((a, b) => (key(a) < key(b) ? -1 : key(a) > key(b) ? 1 : 0));
  const dataParts = [];
  let offset = 0;
  const table = Buffer.alloc(sorted.length * (NAME_LEN + 4 + 4 + 2 + 2));
  let tp = 0;
  for (const e of sorted) {
    const data = e.data !== undefined ? e.data : fs.readFileSync(e.file);
    const name = Buffer.alloc(NAME_LEN);
    // ★ 名字要带扩展名（设备端就是按 "angry.png" 这种全名查的）
    Buffer.from(e.name, 'utf8').copy(name, 0, 0, NAME_LEN - 1);
    name.copy(table, tp); tp += NAME_LEN;
    table.writeUInt32LE(data.length, tp); tp += 4;
    table.writeUInt32LE(offset, tp); tp += 4;
    table.writeUInt16LE(0, tp); tp += 2;   // width（固件不用，置 0 与上游一致）
    table.writeUInt16LE(0, tp); tp += 2;   // height
    dataParts.push(Buffer.from([0x5a, 0x5a]), data);
    offset += 2 + data.length;
  }
  const payload = Buffer.concat([table, ...dataParts]);
  const checksum = payload.reduce((s, b) => (s + b) & 0xffff, 0);
  const header = Buffer.alloc(12);
  header.writeUInt32LE(sorted.length, 0);
  header.writeUInt32LE(checksum, 4);
  header.writeUInt32LE(payload.length, 8);
  fs.mkdirSync(path.dirname(outFile), { recursive: true });
  fs.writeFileSync(outFile, Buffer.concat([header, payload]));
  return { outFile, files: sorted.length, bytes: 12 + payload.length, checksum };
}

// ── 自检：把刚打出来的包按设备端逻辑解一遍（防止格式写错到设备上才发现）──────
function verifyPack(file, expectedNames) {
  const buf = fs.readFileSync(file);
  const files = buf.readUInt32LE(0);
  const chk = buf.readUInt32LE(4);
  const len = buf.readUInt32LE(8);
  if (12 + len !== buf.length) die(`自检失败：header.len=${len} 与实际 ${buf.length - 12} 不符`);
  const payload = buf.subarray(12);
  const calc = payload.reduce((s, b) => (s + b) & 0xffff, 0);
  if (calc !== chk) die(`自检失败：校验和 0x${calc.toString(16)} != 0x${chk.toString(16)}`);
  const NAME_LEN = 32, ENTRY = 44;
  const tableLen = files * ENTRY;
  const seen = [];
  for (let i = 0; i < files; i++) {
    const p = i * ENTRY;
    const name = payload.subarray(p, p + NAME_LEN).toString('utf8').replace(/\0.*$/, '');
    const size = payload.readUInt32LE(p + NAME_LEN);
    const off = payload.readUInt32LE(p + NAME_LEN + 4);
    // ★ 绝对偏移 = 12(头) + 表长 + 相对偏移（第一版这里多算/少算了 12，导致误报）
    const abs = 12 + tableLen + off;
    if (payload[abs - 12] !== 0x5a || payload[abs - 12 + 1] !== 0x5a) {
      die(`自检失败：${name} 缺少 0x5A5A 前缀（off=${off}）`);
    }
    if (abs + 2 + size > buf.length) die(`自检失败：${name} 数据越界`);
    seen.push(name);
  }
  const missing = expectedNames.filter((n) => !seen.includes(n));
  if (missing.length) die(`自检失败：包里缺 ${missing.join(', ')}`);
  return { files, names: seen };
}

// ── 本地 HTTP：给设备下载 assets.bin ────────────────────────────────────────
function serve(binPath) {
  const stat = fs.statSync(binPath);
  const server = http.createServer((req, res) => {
    const u = new URL(req.url, `http://127.0.0.1:${PORT}`);
    if (u.pathname !== '/assets.bin') {
      res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
      return res.end('只提供 /assets.bin\n');
    }
    // ★ 设备用 network->CreateHttp 流式读，只发 200 + Content-Length 就够
    res.writeHead(200, {
      'Content-Type': 'application/octet-stream',
      'Content-Length': stat.size,
      'Cache-Control': 'no-store',
      'Connection': 'close',
    });
    const stream = fs.createReadStream(binPath);
    stream.pipe(res);
    stream.on('end', () => console.log(`  → 已发送 ${kb(stat.size)}`));
  });
  server.on('error', (e) => {
    if (e.code === 'EADDRINUSE') {
      die(`端口 ${PORT} 已被占用（可能已经有一个 emoji-kit 在跑）。\n` +
          `  查占用：Get-NetTCPConnection -LocalPort ${PORT} | Select OwningProcess\n` +
          `  换端口：--port 8125`);
    }
    die(`服务器错误：${e.message}`);
  });
  server.listen(PORT, '0.0.0.0', () => {
    const ips = Object.values(os.networkInterfaces()).flat()
      .filter((i) => i && i.family === 'IPv4' && !i.internal).map((i) => i.address);
    console.log(`\n本地服务器已启动（保持本窗口开着）：`);
    for (const ip of ips) console.log(`  http://${ip}:${PORT}/assets.bin`);
    console.log(`  设备侧将访问：http://${ips[0] ?? '127.0.0.1'}:${PORT}/assets.bin`);
    return ips[0];
  });
  return { server, ips: () => Object.values(os.networkInterfaces()).flat()
    .filter((i) => i && i.family === 'IPv4' && !i.internal).map((i) => i.address) };
}

// ── 推送：写下载地址 + 重启 ─────────────────────────────────────────────────
async function push(url) {
  const base = `http://${DEVICE}:8099`;
  console.log(`\n推送：让设备从 ${url} 下载（写完地址后重启，设备开机时自动下载）`);
  async function get(pathname) {
    const r = await fetch(`${base}${pathname}`);
    const t = await r.text();
    if (!r.ok) throw new Error(`${pathname} -> ${r.status} ${t.trim()}`);
    return t.trim();
  }
  const before = await get('/health').catch((e) => die(`设备连不上：${e.message}`));
  console.log(`  设备在线：${before.split('\n')[1] ?? ''}`);

  // ★ 走 MCP 的 tools/call（等价于对小智说"下载新素材"），复用上游那条被验证过的链路：
  //   它会写 NVS 的 assets/download_url，设备下次开机 CheckAssetsVersion() 自动下载。
  const body = {
    jsonrpc: '2.0', id: 1, method: 'tools/call',
    params: { name: 'self.assets.set_download_url', arguments: { url } },
  };
  const r = await fetch(`${base}/mcp`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
  });
  if (!r.ok) {
    die(`设备不接受 /mcp 调用（${r.status}）。本板固件的 MCP 只走云端 WebSocket，\n` +
        `  所以请改用语音让设备自己设地址：对小智说「把素材下载地址设成 ${url}」，然后重启。`);
  }
  console.log(`  已写入下载地址`);
  await get(`/reboot?key=${KEY}`).catch(() => {});
  console.log(`  设备重启中…开机后会自动下载并应用（约 10~30 秒）`);
}

// ── 主流程 ──────────────────────────────────────────────────────────────────
async function main() {
  if (has('list-builtins')) {
    console.log(`内置表情 ${builtinNames().length} 个（你的素材要同名）：\n`);
    console.log('  ' + builtinNames().map((f) => path.basename(f, '.png')).join('  '));
    return;
  }

  // --builtin：一键回到内置表情（= 用 backup\emoji-test 里那份内置素材副本）
  let dir = arg('dir');
  if (has('builtin')) {
    dir = path.join(REPO, 'backup/emoji-test');
    process.argv.push('--allow-missing', '--yes');
    if (!fs.existsSync(dir)) {
      // 副本没了就现做一份
      fs.mkdirSync(dir, { recursive: true });
      for (const f of fs.readdirSync(BUILTIN_DIR)) {
        fs.copyFileSync(path.join(BUILTIN_DIR, f), path.join(dir, f));
      }
    }
    console.log(`--builtin：用内置素材打包（${dir}）`);
  }
  // --ask：双击流程（.cmd 不带参数）→ 交互式问路径
  if (has('ask')) {
    dir = await askFolder();
    process.argv.push('--yes');
  }

  const checkOnly = arg('check');
  if (checkOnly) {
    const r = check(String(checkOnly));
    printCheck(r);
    process.exit(r.problems.length ? 1 : 0);
  }
  if (!dir && !has('serve-only')) {
    console.log(USAGE);
    process.exit(0);
  }

  const binPath = path.join(OUT_DIR, 'assets.bin');

  if (!has('serve-only')) {
    const r = check(String(dir));
    printCheck(r);
    if (r.problems.length) die('先修掉"必须修的问题"，再重新运行');
    if (r.missing.length) {
      console.log(`\n缺少 ${r.missing.length} 个 → 这些会**保留基线里的原图**（不会崩，但风格会不统一）`);
      if (!has('allow-missing') && !has('yes')) {
        console.log('  （要就这样打包，加 --allow-missing）');
        process.exit(2);
      }
    }

    // ── 基线：当前固件里那份 assets.bin（含 srmodels + 字体 + index.json）──────
    // ★ 必须带上，否则设备下载后会把唤醒词模型/字体弄丢（见 parsePack 的说明）。
    const basePath = String(arg('base', path.join(REPO, 'xiaozhi-esp32/src/build/generated_assets.bin')));
    if (!fs.existsSync(basePath)) {
      die(`找不到基线包：${basePath}\n` +
          `  它是构建产物，先编译一次：\n` +
          `  powershell -NoProfile -ExecutionPolicy Bypass -File tools\\idf-run.ps1 \`\n` +
          `      -Command "python scripts/build.py waveshare/esp32-s3-touch-lcd-1.85-atzgauge"\n` +
          `  （或者用 --base 指定别的 assets.bin）`);
    }
    const base = parsePack(basePath);
    const baseNames = [...base.keys()];
    const baseEmoji = baseNames.filter((n) => EMOJI_RE.test(n));
    if (baseEmoji.length === 0) die(`基线包里没有表情图，--base 指错了？(${basePath})`);

    // 基线里的 index.json：只改 emoji_collection，text_font / srmodels 原样保留
    let index = {};
    if (base.has('index.json')) {
      index = JSON.parse(base.get('index.json').toString('utf8'));
    } else {
      console.log('  ⚠ 基线里没有 index.json，将新建一个（只含表情）');
    }
    const entries = [];
    for (const [name, data] of base) {
      if (EMOJI_RE.test(name)) continue;           // 表情后面统一重建
      entries.push({ name, data });
    }
    // 表情：自定义优先，缺的用基线原图补上（保证 21 个齐全，不会出现"表情找不到"）
    const emojiList = [];
    for (const f of builtinNames()) {
      const bare = path.basename(f, '.png');
      const custom = r.got.get(bare);
      const fileName = `${bare}.png`;
      entries.push({ name: fileName, file: custom ? custom.file : path.join(BUILTIN_DIR, f) });
      emojiList.push({ name: bare, file: fileName });
    }
    index.emoji_collection = emojiList;
    entries.push({ name: 'index.json', data: Buffer.from(JSON.stringify(index, null, 4), 'utf8') });

    const customCount = builtinNames().filter((f) => r.got.has(path.basename(f, '.png'))).length;
    const packed = packAssets(binPath, entries);
    const expected = [...baseNames.filter((n) => !EMOJI_RE.test(n)), ...emojiList.map((e) => e.file)];
    verifyPack(binPath, expected);

    console.log(`\n打包完成：${packed.outFile}`);
    console.log(`  基线   ${path.basename(basePath)}（${baseNames.length} 个文件，含 srmodels/字体/index.json）`);
    console.log(`  文件数 ${packed.files}（自定义表情 ${customCount}/21 + 基线其余 ${packed.files - emojiList.length - 1} + index.json）`);
    console.log(`  体积   ${kb(packed.bytes)}（assets 分区上限 ${kb(MAX_ASSETS)}，占用 ${(packed.bytes / MAX_ASSETS * 100).toFixed(1)}%）`);
    if (packed.bytes > MAX_ASSETS) die('超过 assets 分区大小，设备会拒绝下载');
    console.log(`  校验和 0x${packed.checksum.toString(16)}（自检通过：${packed.files} 条全部可解析、0x5A5A 前缀齐全）`);
    if (has('dry-run')) { console.log('\n--dry-run：不推送。'); return; }
  }

  if (!fs.existsSync(binPath)) die(`还没有打包结果：${binPath}（先跑一次不带 --serve-only 的）`);
  const { server, ips } = serve(binPath);
  if (has('no-push') || has('serve-only')) {
    const ip = ips()[0];
    console.log(`\n不自动推送。你有两种方式让设备下载：`);
    console.log(`  A) 语音：对小智说「把素材下载地址设成 http://${ip}:${PORT}/assets.bin」，然后重启设备`);
    console.log(`  B) 再跑一次不带 --no-push 的命令让它自动写地址并重启`);
    console.log(`\n（本窗口关闭 = 服务器停；设备只在开机时下载一次）`);
    return;
  }
  const ip = ips()[0];
  await push(`http://${ip}:${PORT}/assets.bin`);
  console.log(`\n服务器保持运行中（设备下载完可以 Ctrl+C 关掉）`);
  void server;
}

main().catch((e) => die(e.stack || e.message));
