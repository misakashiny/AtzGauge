// gh-pull-theme.mjs — 拉取主题/动画相关的源码、文档与示例主题包
//
// 用法：node tools/gh-pull-theme.mjs

import { writeFile, mkdir } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';

const RAW = 'https://raw.githubusercontent.com/steveEcode/obd_brz_gauge/main';
const DEST = 'D:\\AtzGauge\\obd_brz_gauge\\src';

const FILES = [
  'themes/README.md',
  'themes/_TEMPLATE/theme.toml',
  'themes/registry.txt',
  'themes/example_boost_oil/layout.json',
  'themes/example_boost_oil/theme_manifest.json',
  'themes/builtin/default/theme.toml',
  'themes/builtin/amber/theme.toml',
  'themes/builtin/ocean/theme.toml',
  'tools/theme_packer/pack_theme.py',
  'tools/gen_themes.py',
  'tools/make_boot_block.py',
  'bootmedia/slot_a/boot_block.txt',
  'docs/THEMING.md',
  'theme_store/boost_oil_example/info.json',
  // 示例主题包本体（4MB，可直接烧到 0x620000）
  'theme_store/boost_oil_example/theme.bin',
  'theme_store/boost_oil_example/preview.png',
];

async function fetchRetry(url, tries = 5) {
  for (let i = 1; i <= tries; i++) {
    try {
      const res = await fetch(url, { redirect: 'follow' });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      return Buffer.from(await res.arrayBuffer());
    } catch (e) {
      if (i === tries) throw e;
      await new Promise((r) => setTimeout(r, 1500));
    }
  }
}

let ok = 0;
for (const rel of FILES) {
  const out = resolve(`${DEST}\\${rel.replace(/\//g, '\\')}`);
  try {
    const buf = await fetchRetry(`${RAW}/${rel}`);
    await mkdir(dirname(out), { recursive: true });
    await writeFile(out, buf);
    console.log(`OK   ${String(buf.length).padStart(9)} B  ${rel}`);
    ok++;
  } catch (e) {
    console.log(`FAIL ${rel} :: ${e.message}`);
  }
}
console.log(`\n已拉取 ${ok}/${FILES.length}`);
