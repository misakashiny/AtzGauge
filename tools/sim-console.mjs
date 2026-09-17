// sim-console.mjs -- PC 端「车况模拟台」：实时把模拟转速等数据推给设备，并同步看屏幕。
//
// 为什么要它：ESP-NOW 主表只有一块（车上），台架上想验证转速环/车况页/告警就得有数据源。
// 设备上的 /inject 端点把数据注入**与真主表完全相同的链路**
// （espnow_slave_inject_test_packet → obd_data_cache → 转速环/车况条/车况页/告警），
// 所以这个程序发什么，设备就当你真车在发什么。
//
// 形态：单文件 Node 程序 + 内置网页界面（不装任何依赖、不用联网装包）。
//   * Node 端只做三件事：发页面、代理设备端点（避开浏览器跨域）、转发注入请求
//   * 10Hz 实时流跑在浏览器里（关掉页面就停，不会偷偷发包）
//
// 用法：
//   node tools/sim-console.mjs                 # 界面 http://127.0.0.1:8123，连默认 IP
//   node tools/sim-console.mjs --device <设备IP> --port 8123
//   设备 IP 也可以用环境变量 ATZ_DEVICE 给（默认 192.168.1.100 只是占位）
//   或双击 tools\sim-console.cmd
import http from 'node:http';
import { exec } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const argv = process.argv.slice(2);
function arg(name, dflt) {
  const i = argv.indexOf(`--${name}`);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : dflt;
}
const DEVICE = arg('device', process.env.ATZ_DEVICE || '192.168.1.100');
// 调试端点鉴权 token：读板目录下的私密头 atz_local.h，其次 atz_ui_config.h
// （读不到就用占位值；也可以直接 --key=xxx 或设 ATZ_DEBUG_TOKEN 环境变量）
function readDebugToken() {
  if (process.env.ATZ_DEBUG_TOKEN) return process.env.ATZ_DEBUG_TOKEN;
  const board = join(dirname(fileURLToPath(import.meta.url)), '..', 'xiaozhi-esp32', 'src', 'main',
    'boards', 'waveshare', 'esp32-s3-touch-lcd-1.85-atzgauge');
  for (const f of ['atz_local.h', 'atz_ui_config.h']) {
    try {
      const txt = readFileSync(join(board, f), 'utf8');
      const m = txt.match(/#define\s+ATZ_DEBUG_TOKEN\s+"([^"]*)"/);
      if (m && m[1]) return m[1];
    } catch { /* 文件不存在就继续找 */ }
  }
  return 'atz-local-debug';
}
const KEY = arg('key', readDebugToken());
const PORT = Number(arg('port', '8123'));
const OPEN = !argv.includes('--no-open');

const PAGE = /* html */ `<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>车况模拟台 · AtzGauge</title>
<style>
  :root { --bg:#0f1216; --card:#171c22; --line:#242c36; --txt:#e6eaf0; --dim:#8b98a8; --hi:#00e5ff; --warn:#ffa000; --alarm:#ff3b30; --ok:#00cc66; }
  * { box-sizing:border-box; }
  body { margin:0; background:var(--bg); color:var(--txt); font:14px/1.5 "Segoe UI","Microsoft YaHei",sans-serif; }
  header { padding:14px 18px; border-bottom:1px solid var(--line); display:flex; gap:14px; align-items:center; flex-wrap:wrap; }
  h1 { font-size:16px; margin:0; font-weight:600; }
  .pill { font-size:12px; padding:3px 9px; border-radius:99px; border:1px solid var(--line); color:var(--dim); }
  .pill.ok { color:var(--ok); border-color:#1d4a33; } .pill.bad { color:var(--alarm); border-color:#4a1f1c; }
  main { display:grid; grid-template-columns: 1fr 380px; gap:16px; padding:16px 18px 40px; align-items:start; }
  @media (max-width:900px){ main { grid-template-columns:1fr; } }
  .card { background:var(--card); border:1px solid var(--line); border-radius:12px; padding:14px 16px; }
  .card h2 { font-size:13px; margin:0 0 10px; color:var(--dim); font-weight:600; letter-spacing:.04em; }
  .row { display:grid; grid-template-columns: 96px 1fr 74px; gap:10px; align-items:center; margin:8px 0; }
  .row label { color:var(--dim); }
  input[type=range] { width:100%; accent-color:var(--hi); }
  .val { text-align:right; font-variant-numeric:tabular-nums; font-weight:600; }
  .btns { display:flex; gap:8px; flex-wrap:wrap; margin-top:6px; }
  button { background:#1f2731; color:var(--txt); border:1px solid var(--line); border-radius:8px; padding:8px 13px; cursor:pointer; font-size:13px; }
  button:hover { background:#27313d; }
  button.primary { background:#0b5f6b; border-color:#0e7d8c; }
  button.danger { background:#5f1b17; border-color:#8a2a24; }
  button:disabled { opacity:.45; cursor:not-allowed; }
  input[type=text] { background:#12171d; color:var(--txt); border:1px solid var(--line); border-radius:8px; padding:7px 10px; width:140px; }
  #screen { width:100%; max-width:360px; border-radius:50%; border:1px solid var(--line); background:#000; display:block; margin:0 auto; }
  .kv { display:flex; justify-content:space-between; gap:10px; padding:3px 0; border-bottom:1px dashed #1d242c; }
  .kv:last-child { border:0; }
  .kv span:first-child { color:var(--dim); }
  .kv b { font-weight:600; font-variant-numeric:tabular-nums; }
  #log { height:132px; overflow:auto; background:#0b0e12; border:1px solid var(--line); border-radius:8px; padding:8px 10px;
         font:12px/1.5 Consolas,monospace; color:#9fb3c8; white-space:pre-wrap; }
  .hint { color:var(--dim); font-size:12px; margin-top:8px; }
  .alarmbar { margin-top:8px; padding:7px 10px; border-radius:8px; background:#241512; border:1px solid #4a2a20; color:#ffb4a8; font-size:12px; display:none; }
</style></head><body>
<header>
  <h1>车况模拟台</h1>
  <span>设备 <input type="text" id="dev" value="${DEVICE}"></span>
  <button id="probe">连接检查</button>
  <span class="pill" id="st">未连接</span>
  <span class="pill" id="fw">—</span>
  <span class="pill" id="fps">—</span>
</header>

<main>
  <div>
    <div class="card">
      <h2>实时数据（拖到哪就发到哪）</h2>
      <div id="rows"></div>
      <div class="btns">
        <button class="primary" id="stream">▶ 开始 10Hz 实时流</button>
        <button id="once">发送一次</button>
        <button id="stop" class="danger">■ 停止（回到无数据）</button>
      </div>
      <div class="alarmbar" id="alarmbar">⚠ 转速 ≥ 6500 会让设备**真的响**本地高转告警（设计行为）</div>
    </div>

    <div class="card" style="margin-top:14px">
      <h2>预设 / 演示</h2>
      <div class="btns">
        <button data-preset="idle">怠速 850</button>
        <button data-preset="cruise">巡航 2200 / 100km/h</button>
        <button data-preset="accel">加速 4800</button>
        <button data-preset="redline">红线 7000</button>
        <button data-preset="zero">全部归零</button>
      </div>
      <div class="btns">
        <button id="sweep">🔁 扫掠演示（怠速→拉转速→换挡，8 秒一循环）</button>
        <button id="sweepStop">停扫掠</button>
      </div>
      <div class="hint">扫掠/预设都只是把滑块按曲线改掉，仍然走 10Hz 实时流；想只看设备自带的模拟，也可以用
        <code>http://&lt;设备IP&gt;:8099/carbar?seconds=30&amp;mode=rev</code>。</div>
    </div>
  </div>

  <div>
    <div class="card">
      <h2>设备屏幕（约 1 帧/秒）</h2>
      <img id="screen" alt="设备截屏">
      <div class="hint" id="shoterr"></div>
    </div>
    <div class="card" style="margin-top:14px">
      <h2>设备状态</h2>
      <div class="kv"><span>固件</span><b id="k-fw">—</b></div>
      <div class="kv"><span>运行时长</span><b id="k-up">—</b></div>
      <div class="kv"><span>剩余内存</span><b id="k-heap">—</b></div>
      <div class="kv"><span>UI 帧率</span><b id="k-fps">—</b></div>
      <div class="kv"><span>单帧重绘</span><b id="k-px">—</b></div>
    </div>
    <div class="card" style="margin-top:14px">
      <h2>日志</h2>
      <div id="log"></div>
    </div>
  </div>
</main>

<script>
const FIELDS = [
  { k:'rpm',     name:'转速',   min:0,   max:8000, step:10,   unit:'rpm' },
  { k:'speed',   name:'车速',   min:0,   max:240,  step:1,    unit:'km/h' },
  { k:'coolant', name:'水温',   min:-40, max:130,  step:1,    unit:'°C' },
  { k:'oil',     name:'油温',   min:-100,max:150,  step:1,    unit:'°C' },
  { k:'intake',  name:'进气',   min:-40, max:80,   step:1,    unit:'°C' },
  { k:'load',    name:'负荷',   min:0,   max:100,  step:1,    unit:'%' },
  { k:'tps',     name:'节气门', min:0,   max:100,  step:1,    unit:'%' },
  { k:'volt',    name:'电压',   min:8,   max:16,   step:0.1,  unit:'V' },
];
const PRESETS = {
  idle:    { rpm:850,  speed:0,   coolant:88, oil:96,  intake:30, load:18, tps:9,  volt:14.2 },
  cruise:  { rpm:2200, speed:100, coolant:92, oil:104, intake:36, load:32, tps:22, volt:14.1 },
  accel:   { rpm:4800, speed:76,  coolant:94, oil:110, intake:44, load:78, tps:66, volt:13.9 },
  redline: { rpm:7000, speed:132, coolant:96, oil:118, intake:52, load:96, tps:92, volt:13.7 },
  zero:    { rpm:0,    speed:0,   coolant:20, oil:20,  intake:20, load:0,  tps:0,  volt:12.6 },
};
const DEFAULTS = { rpm:3200, speed:88, coolant:92, oil:100, intake:38, load:42, tps:27, volt:14.2 };

const S = { values:{...DEFAULTS}, streaming:false, sweeping:false, timer:null, sweepT:0, ok:false };
const $ = (id) => document.getElementById(id);
const dev = () => $('dev').value.trim();

function log(msg, cls) {
  const el = $('log');
  const t = new Date().toLocaleTimeString('zh-CN', { hour12:false });
  el.textContent = '[' + t + '] ' + msg + '\\n' + el.textContent;
  if (el.textContent.length > 4000) el.textContent = el.textContent.slice(0, 4000);
}

// ── 生成滑块 ─────────────────────────────────────────────────────────────
const rows = $('rows');
for (const f of FIELDS) {
  const div = document.createElement('div');
  div.className = 'row';
  div.innerHTML = '<label>' + f.name + '</label>' +
    '<input type="range" id="s-' + f.k + '" min="' + f.min + '" max="' + f.max + '" step="' + f.step + '">' +
    '<div class="val" id="v-' + f.k + '"></div>';
  rows.appendChild(div);
  const sl = div.querySelector('input');
  sl.value = S.values[f.k];
  sl.addEventListener('input', () => { S.values[f.k] = Number(sl.value); paint(); if (S.sweeping) stopSweep(); });
  $('v-' + f.k).textContent = S.values[f.k] + ' ' + f.unit;
}
function paint() {
  for (const f of FIELDS) {
    $('s-' + f.k).value = S.values[f.k];
    $('v-' + f.k).textContent = S.values[f.k] + ' ' + f.unit;
  }
  $('alarmbar').style.display = S.values.rpm >= 6500 ? 'block' : 'none';
}
function setAll(obj) { Object.assign(S.values, obj); paint(); }

// ── 与设备通信（走本地 Node 代理，避开跨域）──────────────────────────────
function qs() {
  const p = new URLSearchParams({ device: dev() });
  for (const f of FIELDS) p.set(f.k, String(S.values[f.k]));
  return p.toString();
}
async function injectOnce() {
  try {
    // seconds=2：告诉设备"这组值保持 2 秒"，设备内部 10Hz 复现 → 我们只要 3Hz 续命
    const r = await fetch('/api/inject?' + qs() + '&seconds=2', { cache:'no-store' });
    const t = await r.text();
    if (!S.ok) { S.ok = true; setStatus(true); }
    if (!S.streaming) log('注入: ' + t.trim());
  } catch (e) { setStatus(false); log('注入失败: ' + e.message); }
}
function setStatus(ok) {
  S.ok = ok;
  $('st').textContent = ok ? '已连接' : '连不上';
  $('st').className = 'pill ' + (ok ? 'ok' : 'bad');
}
async function probe() {
  try {
    const r = await fetch('/api/health?device=' + encodeURIComponent(dev()), { cache:'no-store' });
    const txt = await r.text();
    const body = txt.split('\\n');
    const fw = (body.find(l => l.startsWith('fw=')) || 'fw=?').slice(3);
    const up = (body.find(l => l.startsWith('uptime=')) || 'uptime=?').slice(7);
    const heap = (body.find(l => l.startsWith('heap=')) || '').match(/heap=(\\d+)/);
    $('k-fw').textContent = fw; $('fw').textContent = fw;
    $('k-up').textContent = up;
    $('k-heap').textContent = heap ? (Number(heap[1]) / 1024).toFixed(0) + ' KB' : '—';
    setStatus(true); log('设备在线: ' + fw);
  } catch (e) { setStatus(false); log('连接检查失败: ' + e.message); }
}
async function perf() {
  try {
    const r = await fetch('/api/perf?device=' + encodeURIComponent(dev()) + '&seconds=2', { cache:'no-store' });
    const t = await r.text();
    const fps = (t.match(/->\\s*([\\d.]+) fps/) || [])[1];
    const px = (t.match(/avg redraw=(\\d+) px/) || [])[1];
    $('k-fps').textContent = fps ? fps + ' fps' : '—';
    $('k-px').textContent = px ? px + ' px/帧' : '—';
    $('fps').textContent = fps ? fps + ' fps' : '—';
  } catch { /* 静默 */ }
}

// ── 实时流（10Hz，跑在浏览器里，关页面即停）──────────────────────────────
function startStream() {
  if (S.streaming) return;
  S.streaming = true;
  S.timer = setInterval(injectOnce, 330);   // 3Hz 足够：设备内部会按 10Hz 复现（见 /inject 的 seconds 参数）
  $('stream').textContent = '⏸ 暂停实时流'; $('stream').classList.remove('primary');
  log('开始 10Hz 实时流');
}
function stopStream() {
  if (!S.streaming) return;
  S.streaming = false;
  clearInterval(S.timer); S.timer = null;
  $('stream').textContent = '▶ 开始 10Hz 实时流'; $('stream').classList.add('primary');
  log('暂停实时流');
}
$('stream').onclick = () => (S.streaming ? stopStream() : startStream());
$('once').onclick = injectOnce;
$('probe').onclick = () => { probe(); perf(); };
$('stop').onclick = async () => {
  stopStream(); stopSweep();
  try { await fetch('/api/stop?device=' + encodeURIComponent(dev()), { cache:'no-store' }); log('已通知设备停止注入（2 秒后数据变陈旧，环归零变暗）'); }
  catch (e) { log('停止失败: ' + e.message); }
};
document.querySelectorAll('[data-preset]').forEach(b => b.onclick = () => {
  setAll(PRESETS[b.dataset.preset]); log('预设 ' + b.textContent.trim());
  if (!S.streaming) startStream();
});

// 扫掠：8 秒一循环，与固件里的曲线一致（怠速→拉转速→顶一下→松油门→再拉→回怠速）
function sweepTick() {
  const phase = S.sweepT % 80, top = 6400;
  let rpm;
  if (phase < 10) rpm = 850 + phase * 27;
  else if (phase < 40) rpm = 1120 + (phase - 10) * (top - 1120) / 30;
  else if (phase < 45) rpm = top;
  else if (phase < 50) rpm = top - (phase - 45) * (top - 2400) / 5;
  else if (phase < 70) rpm = 2400 + (phase - 50) * (top - 2400) / 20;
  else rpm = top - (phase - 70) * (top - 850) / 10;
  const load = Math.round(18 + (rpm - 800) * 62 / (top - 800));
  S.values.rpm = Math.max(800, Math.round(rpm));
  S.values.load = Math.max(0, Math.min(100, load));
  S.values.tps = Math.max(0, Math.min(100, Math.round(8 + (rpm - 800) * 74 / (top - 800))));
  S.values.speed = Math.min(240, 24 + Math.floor((S.sweepT % 80) / 4));
  S.values.volt = Math.round((14.2 - (rpm / 100) * 0.003) * 10) / 10;
  S.sweepT++;
  paint();
}
function startSweep() {
  if (S.sweeping) return;
  S.sweeping = true; S.sweepT = 0;
  startStream();
  S.sweepId = setInterval(sweepTick, 100);
  log('扫掠演示开始（8 秒一循环，峰值 6400，不触发红线告警）');
}
function stopSweep() {
  if (!S.sweeping) return;
  S.sweeping = false; clearInterval(S.sweepId); S.sweepId = null;
  log('扫掠演示停止');
}
$('sweep').onclick = startSweep;
$('sweepStop').onclick = stopSweep;

// ── 屏幕预览（经 Node 代理，避免跨域）────────────────────────────────────
setInterval(() => {
  const img = $('screen');
  const next = new Image();
  next.onload = () => { img.src = next.src; $('shoterr').textContent = ''; };
  next.onerror = () => { $('shoterr').textContent = '取不到截屏（设备离线？）'; };
  next.src = '/api/shot.jpg?device=' + encodeURIComponent(dev()) + '&t=' + Date.now();
}, 1000);   // 预览 1 帧/秒：再快只会白占设备 socket

setInterval(() => { if (S.ok) perf(); }, 4000);
paint();
probe().then(perf);
log('模拟台就绪。目标设备 ' + dev() + '；左上"连接检查"可复核。');
</script>
</body></html>`;

// ── 代理：把请求转给设备（顺带统一错误信息，前端只需看一段文本）──────────
async function proxy(res, url, asBinary) {
  try {
    const ctrl = new AbortController();
    const t = setTimeout(() => ctrl.abort(), 6000);
    const r = await fetch(url, { signal: ctrl.signal });
    clearTimeout(t);
    const buf = Buffer.from(await r.arrayBuffer());
    res.writeHead(r.status, asBinary
      ? { 'Content-Type': 'image/jpeg', 'Cache-Control': 'no-store' }
      : { 'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store' });
    res.end(buf);
  } catch (e) {
    res.writeHead(502, { 'Content-Type': 'text/plain; charset=utf-8' });
    res.end('设备不可达: ' + e.message + '\n（检查 IP、确认设备和 PC 在同一 WiFi、设备串口是否已连上路由）');
  }
}

const server = http.createServer(async (req, res) => {
  const u = new URL(req.url, `http://127.0.0.1:${PORT}`);
  const device = u.searchParams.get('device') || DEVICE;
  if (u.pathname === '/' || u.pathname === '/index.html') {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    return res.end(PAGE);
  }
  if (u.pathname === '/api/health') return proxy(res, `http://${device}:8099/health`);
  if (u.pathname === '/api/perf')   return proxy(res, `http://${device}:8099/perf?seconds=${u.searchParams.get('seconds') || 2}`);
  if (u.pathname === '/api/shot.jpg') return proxy(res, `http://${device}:8099/shot.jpg?t=${Date.now()}`, true);
  if (u.pathname === '/api/stop')   return proxy(res, `http://${device}:8099/inject?stop=1&key=${KEY}`);
  if (u.pathname === '/api/inject') {
    const q = new URLSearchParams(u.searchParams);
    q.delete('device');
    q.set('key', KEY);                 // 写操作鉴权（设备端 ATZ_DEBUG_TOKEN）
    return proxy(res, `http://${device}:8099/inject?${q.toString()}`);
  }
  res.writeHead(404); res.end('not found');
});

server.on('error', (e) => {
  if (e.code === 'EADDRINUSE') {
    console.error('');
    console.error(`端口 ${PORT} 已被占用 —— 模拟台**已经在运行**了（多半是你又双击了一次）。`);
    console.error(`本次不再启动第二个实例：多个实例会各自 10Hz 往设备发包，`);
    console.error(`而设备一共只有 10 个 socket，会被打爆（现象：喊"小智"没反应）。`);
    console.error(`直接打开已有的界面： http://127.0.0.1:${PORT}`);
    if (OPEN) exec(`start "" http://127.0.0.1:${PORT}`, () => {});
    setTimeout(() => process.exit(0), 800);
    return;
  }
  console.error('启动失败:', e.message);
  process.exit(1);
});

server.listen(PORT, '127.0.0.1', () => {
  const url = `http://127.0.0.1:${PORT}`;
  console.log(`车况模拟台已启动`);
  console.log(`  界面   : ${url}`);
  console.log(`  目标设备: ${DEVICE}:8099   (界面里也能改)`);
  console.log(`  停止   : Ctrl+C（或直接关掉这个窗口）`);
  console.log(`  提示   : 10Hz 实时流跑在浏览器页面里，关掉页面即停，不会在后台偷偷发包。`);
  if (OPEN) exec(`start "" ${url}`, () => {});
});
