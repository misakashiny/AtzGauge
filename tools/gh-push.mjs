// gh-push.mjs -- 把本地仓库推送到 GitHub 私有仓库（顺带先做一次发布体检）
//
// 用法（推荐双击 tools\gh-push.cmd）：
//   node tools/gh-push.mjs --user <GitHub用户名>
//   node tools/gh-push.mjs --user <用户名> --repo AtzGauge --check
//
// 做四件事：
//   1) 先跑 tools/publish-check.mjs（隐私体检），没过就停 —— 绝不带着秘密推送
//   2) 记录/更新名为 github 的远端（不动你原来的 origin = E: 备份盘）
//   3) 备份盘同步一次（可选，--no-backup 跳过）
//   4) 推送 master 分支；凭据交给 Windows 凭据管理器（第一次会弹窗要 token）
//
// 第一次推送需要 Personal Access Token（不是 GitHub 登录密码）：
//   GitHub → 头像 → Settings → Developer settings → Personal access tokens
//   → Tokens (classic) → Generate new token (classic) → 勾 repo → 生成后立刻复制
//
// 退出码：0 = 推送成功；1 = 有阻塞，按提示处理。

import { execFileSync, spawnSync } from 'node:child_process';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const GIT = process.env.ATZ_GIT || 'D:\\esp\\mingit\\cmd\\git.exe';
const REMOTE = 'github';

// ── 代理探测（★ 2026-09-17 实测踩坑）────────────────────────────────────────
// 现象：`fatal: unable to access 'https://github.com/...': Recv failure: Connection was reset`
//      但 `curl https://api.github.com` 是通的，`Test-NetConnection github.com -Port 443` 不通。
// 真相：这台机器直连 github.com:443 被重置，必须走本地代理（系统代理设了 127.0.0.1:7890），
//      而 **git 默认不读 Windows 系统代理**（只认 http_proxy/https_proxy 环境变量）。
// 所以这里主动探测：环境变量优先，其次系统代理设置，再验证端口真的在监听才用。
function detectProxy() {
  for (const k of ['HTTPS_PROXY', 'https_proxy', 'HTTP_PROXY', 'http_proxy', 'ALL_PROXY', 'all_proxy']) {
    if (process.env[k]) return process.env[k];
  }
  if (process.platform !== 'win32') return '';
  try {
    const out = execFileSync('reg', ['query',
      'HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings',
      '/v', 'ProxyServer'], { encoding: 'utf8' });
    const m = out.match(/ProxyServer\s+REG_SZ\s+(\S+)/);
    if (!m) return '';
    let v = m[1].trim();
    if (!/^https?:\/\//.test(v)) v = 'http://' + v;     // 常见写法是 127.0.0.1:7890
    return v;
  } catch { return ''; }
}

// 端口在监听才算可用（系统代理开了但代理软件没启动是很常见的情况）
function portListening(url) {
  try {
    const u = new URL(url);
    const port = Number(u.port || (u.protocol === 'https:' ? 443 : 80));
    return new Promise((resolve) => {
      const s = net.connect({ host: u.hostname, port, timeout: 1500 });
      s.on('connect', () => { s.destroy(); resolve(true); });
      s.on('error', () => resolve(false));
      s.on('timeout', () => { s.destroy(); resolve(false); });
    });
  } catch { return Promise.resolve(false); }
}

const argv = process.argv.slice(2);
function arg(name, dflt) {
  const i = argv.indexOf(`--${name}`);
  return i >= 0 && argv[i + 1] && !argv[i + 1].startsWith('--') ? argv[i + 1] : dflt;
}
const USER = arg('user', process.env.ATZ_GH_USER || '');
const REPO = arg('repo', 'AtzGauge');
const CHECK_ONLY = argv.includes('--check');
const NO_BACKUP = argv.includes('--no-backup');
const BRANCH = arg('branch', 'master');

function git(args, opts = {}) {
  return execFileSync(GIT, args, { cwd: ROOT, encoding: 'utf8', stdio: opts.inherit ? 'inherit' : 'pipe' });
}
function tryGit(args) {
  try { return { ok: true, out: git(args) }; } catch (e) { return { ok: false, out: (e.stdout || '') + (e.stderr || '') }; }
}

function die(msg) {
  console.log(`\n✗ ${msg}\n`);
  process.exit(1);
}

console.log('\n══ 推送到 GitHub ══\n');

// ── 0. 参数检查 ─────────────────────────────────────────────────────────────
if (!USER) {
  console.log('  需要 GitHub 用户名。两种给法：');
  console.log('    node tools/gh-push.mjs --user <用户名>');
  console.log('    或先设环境变量 ATZ_GH_USER=<用户名>\n');
  // 已经配过远端的话，可以从远端地址里取
  const r = tryGit(['remote', 'get-url', REMOTE]);
  const m = r.ok && r.out.match(/github\.com[/:]([^/]+)\//);
  if (m) {
    console.log(`  （检测到已配置的远端用户：${m[1]}，可以直接用 --user ${m[1]}）\n`);
  }
  process.exit(1);
}
const URL = `https://github.com/${USER}/${REPO}.git`;

// ── 1. 代理（直连不通时必须先解决，否则卡在 connection reset）────────────────
// 注意：**不要**把代理地址写进仓库配置（`git config --local http.proxy`），
// 那会变成"这台机器专用"的提交内容。这里只影响当前进程的环境变量。
let PROXY = '';
{
  const cand = detectProxy();
  if (cand && (await portListening(cand))) {
    PROXY = cand;
    process.env.HTTPS_PROXY = cand;
    process.env.HTTP_PROXY = cand;
    console.log(`  [1/5] 代理：使用 ${cand}（直连 github.com 常被重置，git 不读系统代理，故显式设置）`);
  } else if (cand) {
    console.log(`  [1/5] 代理：检测到 ${cand} 但端口没在监听 → 按直连尝试`);
  } else {
    console.log('  [1/5] 代理：未检测到（按直连尝试）');
  }
}

// ── 2. 隐私体检 ─────────────────────────────────────────────────────────────
console.log('\n  [2/5] 隐私体检 …');
const check = spawnSync(process.execPath, [path.join(ROOT, 'tools', 'publish-check.mjs')], {
  cwd: ROOT, stdio: 'inherit', env: process.env,
});
if (check.status !== 0) {
  die('体检没过 —— 先按上面的提示处理完再推送（这是故意的：宁可推不上去，也别把秘密推上去）');
}

// ── 3. 工作区干净度 ─────────────────────────────────────────────────────────
const status = git(['status', '--porcelain']).trim();
if (status) {
  console.log('\n  注意：工作区还有未提交的内容（不会被推送）：');
  for (const l of status.split('\n').slice(0, 10)) console.log(`    ${l.trim()}`);
}

// ── 4. 远端 ─────────────────────────────────────────────────────────────────
console.log(`\n  [3/5] 远端 ${REMOTE} → ${URL}`);
const existing = tryGit(['remote', 'get-url', REMOTE]);
if (existing.ok) {
  const cur = existing.out.trim();
  if (cur !== URL) {
    console.log(`        原地址 ${cur} 不一致，改为 ${URL}`);
    git(['remote', 'set-url', REMOTE, URL]);
  } else {
    console.log('        已存在且地址一致');
  }
} else {
  git(['remote', 'add', REMOTE, URL]);
  console.log('        已添加');
}

if (CHECK_ONLY) {
  console.log('\n  --check 模式：只体检 + 配置远端，不推送。\n');
  process.exit(0);
}

// ── 5. 顺带同步备份盘 ───────────────────────────────────────────────────────
if (!NO_BACKUP) {
  console.log('\n  [4/5] 同步本地备份盘（origin，走本地盘不需要代理）…');
  const saved = { h: process.env.HTTPS_PROXY, p: process.env.HTTP_PROXY };
  if (/^https?:\/\/(127\.0\.0\.1|localhost)/i.test(PROXY)) {
    // 本地裸仓库走代理反而可能失败，临时摘掉
    delete process.env.HTTPS_PROXY; delete process.env.HTTP_PROXY;
  }
  const bk = tryGit(['push', 'origin', BRANCH]);
  process.env.HTTPS_PROXY = saved.h; process.env.HTTP_PROXY = saved.p;
  console.log(bk.ok ? '        ok' : '        跳过（备份盘远端不可用，不影响 GitHub 推送）');
} else {
  console.log('\n  [4/5] 跳过备份盘同步（--no-backup）');
}

// ── 6. 推送 ─────────────────────────────────────────────────────────────────
console.log(`\n  [5/5] 推送 ${BRANCH} → ${REMOTE}（第一次会弹窗要 token）…\n`);
const push = spawnSync(GIT, ['push', '-u', REMOTE, BRANCH], { cwd: ROOT, stdio: 'inherit' });

if (push.status === 0) {
  console.log(`\n✓ 推送完成：https://github.com/${USER}/${REPO}\n`);
  process.exit(0);
}

console.log('\n✗ 推送失败。常见原因：');
console.log('  · 网络被重置（`Recv failure: Connection was reset`）→ 需要代理：');
console.log('      先启动代理软件，再设 HTTPS_PROXY=http://127.0.0.1:7890 后重跑（脚本会自动探测系统代理）');
console.log('      自测：Test-NetConnection github.com -Port 443（不通就是这个问题）');
console.log('  · 仓库还没建 → 先在 GitHub 网页建一个**空**仓库（不要勾 README/LICENSE）');
console.log('  · 用户名/仓库名拼错 → 用 --user/--repo 改');
console.log('  · 认证失败 → 密码栏要填 **Personal Access Token**（勾 repo 权限），不是登录密码');
console.log('  · 已存在同名提交历史 → 别 force push；先确认是不是推错仓库了\n');
process.exit(1);
