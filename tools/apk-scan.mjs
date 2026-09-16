// apk-scan.mjs — 扫描配套 App 的 web 包，找主题相关代码与远程地址
//
// 用法：node tools/apk-scan.mjs

import fs from 'node:fs';
import path from 'node:path';

const ROOT = 'D:/AtzGauge/android_app/extracted/assets';
const KEYWORDS = ['ota/theme', 'catalog', 'theme_store', 'themeStore', 'theme.bin', 'themes/', 'themeId', 'installTheme'];

const files = [];
(function walk(dir) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p);
    else if (/\.(js|json|html)$/.test(e.name)) files.push(p);
  }
})(ROOT);

console.log(`扫描 ${files.length} 个文件:`);
for (const f of files) console.log('   ' + path.relative(ROOT, f).replace(/\\/g, '/'));
console.log('');

for (const f of files) {
  const s = fs.readFileSync(f, 'latin1');
  for (const kw of KEYWORDS) {
    let i = s.indexOf(kw);
    let shown = 0;
    while (i !== -1 && shown < 3) {
      console.log(`[${kw}] ${path.basename(f)} @${i}`);
      console.log('   ' + JSON.stringify(s.slice(Math.max(0, i - 150), i + 150)));
      i = s.indexOf(kw, i + 1);
      shown++;
    }
  }
}
