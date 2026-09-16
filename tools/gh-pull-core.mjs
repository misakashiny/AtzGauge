// gh-pull-core.mjs — 拉取 obd_brz_gauge 的**核心源码**（非 UI）到本地留档
//
// 为什么需要它：gh-pull.mjs 只拉 main/export_path/ 下的 UI 文件，
// 从不拉 main/bsp_obd_dsp/ 与 main/app_obd_dsp/。
// 而 07 号文档的源码依据（espnow_link.c 的 MG_MAX_SLAVES / ESPNOW_CHANNEL 等）
// 正好在这两个目录里 —— 曾因此在本地无法离线复核。
//
// 落盘布局刻意镜像上游 main/ 结构（保留 bsp_obd_dsp/ app_obd_dsp/ 两级），
// 因为源码内部就是这么互相 include 的：
//     #include "bsp_obd_dsp/espnow_link.h"
//     #include "app_obd_dsp/obd_data_cache.h"
// 展平会破坏这些路径的可读性。
//
// 用法：node tools/gh-pull-core.mjs
// 产出：D:\AtzGauge\obd_brz_gauge\src\{bsp_obd_dsp,app_obd_dsp}\**
//       D:\AtzGauge\obd_brz_gauge\src\_provenance.json   ← 记录快照来源，便于日后核对

import { writeFile, mkdir } from 'node:fs/promises';
import { dirname, resolve, join } from 'node:path';

const REPO = 'steveEcode/obd_brz_gauge';
const API = `https://api.github.com/repos/${REPO}`;
const RAW = `https://raw.githubusercontent.com/${REPO}`;
const DEST = 'D:\\AtzGauge\\obd_brz_gauge\\src';

// 只拉这两个核心子树；UI 由 gh-pull.mjs 负责
const PREFIXES = ['main/bsp_obd_dsp/', 'main/app_obd_dsp/'];

// 带重试：GitHub raw 偶发 fetch failed（本工具实测踩过一次），
// 单次失败不该让整批 45 个文件白拉
async function fetchText(url, tries = 4) {
  let lastErr;
  for (let i = 1; i <= tries; i++) {
    try {
      const res = await fetch(url, { redirect: 'follow' });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      return await res.text();
    } catch (e) {
      lastErr = e;
      if (i < tries) await new Promise((r) => setTimeout(r, 400 * i));
    }
  }
  throw new Error(`${lastErr.message} :: ${url} (after ${tries} tries)`);
}

// 1) 解析默认分支 + 锁定 commit（快照可追溯）
const info = JSON.parse(await fetchText(API));
const BRANCH = info.default_branch || 'main';
const commit = JSON.parse(await fetchText(`${API}/commits/${BRANCH}`));
const SHA = commit.sha;
console.log(`repo=${REPO} branch=${BRANCH} sha=${SHA.slice(0, 12)} (${commit.commit.committer.date})`);

// 2) 取全树，筛出目标子树的 blob
const tree = JSON.parse(await fetchText(`${API}/git/trees/${BRANCH}?recursive=1`));
if (tree.truncated) console.log('⚠ tree truncated — 结果可能不完整');

const targets = tree.tree
  .filter((n) => n.type === 'blob' && PREFIXES.some((p) => n.path.startsWith(p)))
  .map((n) => n.path)
  .sort();

console.log(`目标文件数：${targets.length}`);

// 3) 逐个下载
const saved = [];
const failed = [];
for (const p of targets) {
  const rel = p.replace(/^main\//, '');          // bsp_obd_dsp/espnow_link.c
  const out = resolve(join(DEST, rel));
  try {
    const body = await fetchText(`${RAW}/${BRANCH}/${p}`);
    await mkdir(dirname(out), { recursive: true });
    await writeFile(out, body);
    saved.push({ path: rel, bytes: Buffer.byteLength(body) });
  } catch (e) {
    failed.push(`${p} :: ${e.message}`);
  }
}

// 4) 写快照来源，避免再出现「引用了源码但本地没有」
await writeFile(
  resolve(join(DEST, '_provenance.json')),
  JSON.stringify(
    {
      repo: REPO,
      branch: BRANCH,
      commit: SHA,
      commitDate: commit.commit.committer.date,
      pulledAt: new Date().toISOString(),
      pulledBy: 'tools/gh-pull-core.mjs',
      prefixes: PREFIXES,
      fileCount: saved.length,
      files: saved,
    },
    null,
    2
  ) + '\n'
);

console.log(`\n✅ 已保存 ${saved.length} 个文件到 ${DEST}`);
console.log(`✅ 快照来源已写入 ${join(DEST, '_provenance.json')}`);
if (failed.length) {
  console.log(`\n❌ 失败 ${failed.length} 个：`);
  for (const f of failed) console.log(`  ${f}`);
  process.exitCode = 1;
}
