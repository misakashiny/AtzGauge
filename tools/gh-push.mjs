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
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const GIT = process.env.ATZ_GIT || 'D:\\esp\\mingit\\cmd\\git.exe';
const REMOTE = 'github';

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

// ── 1. 隐私体检 ─────────────────────────────────────────────────────────────
console.log('  [1/4] 隐私体检 …');
const check = spawnSync(process.execPath, [path.join(ROOT, 'tools', 'publish-check.mjs')], {
  cwd: ROOT, stdio: 'inherit', env: process.env,
});
if (check.status !== 0) {
  die('体检没过 —— 先按上面的提示处理完再推送（这是故意的：宁可推不上去，也别把秘密推上去）');
}

// ── 2. 工作区干净度 ─────────────────────────────────────────────────────────
const status = git(['status', '--porcelain']).trim();
if (status) {
  console.log('\n  注意：工作区还有未提交的内容（不会被推送）：');
  for (const l of status.split('\n').slice(0, 10)) console.log(`    ${l.trim()}`);
}

// ── 3. 远端 ─────────────────────────────────────────────────────────────────
console.log(`\n  [2/4] 远端 ${REMOTE} → ${URL}`);
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

// ── 4. 顺带同步备份盘 ───────────────────────────────────────────────────────
if (!NO_BACKUP) {
  console.log('\n  [3/4] 同步本地备份盘（origin）…');
  const bk = tryGit(['push', 'origin', BRANCH]);
  console.log(bk.ok ? '        ok' : '        跳过（备份盘远端不可用，不影响 GitHub 推送）');
} else {
  console.log('\n  [3/4] 跳过备份盘同步（--no-backup）');
}

// ── 5. 推送 ─────────────────────────────────────────────────────────────────
console.log(`\n  [4/4] 推送 ${BRANCH} → ${REMOTE}（第一次会弹窗要 token）…\n`);
const push = spawnSync(GIT, ['push', '-u', REMOTE, BRANCH], { cwd: ROOT, stdio: 'inherit' });

if (push.status === 0) {
  console.log(`\n✓ 推送完成：https://github.com/${USER}/${REPO}\n`);
  process.exit(0);
}

console.log('\n✗ 推送失败。常见原因：');
console.log('  · 仓库还没建 → 先在 GitHub 网页建一个**空**仓库（不要勾 README/LICENSE）');
console.log('  · 用户名/仓库名拼错 → 用 --user/--repo 改');
console.log('  · 认证失败 → 密码栏要填 **Personal Access Token**（勾 repo 权限），不是登录密码');
console.log('  · 已存在同名提交历史 → 别 force push；先确认是不是推错仓库了\n');
process.exit(1);
