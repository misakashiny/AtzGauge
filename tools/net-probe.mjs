// net-probe.mjs — 探测装 ESP-IDF 所需的各个下载源在本机是否可达
// PowerShell/Schannel 在本环境对部分站点握手失败，但 Node/OpenSSL 可能正常
const TARGETS = [
  ['GitHub raw',        'https://raw.githubusercontent.com/espressif/esp-idf/master/README.md'],
  ['GitHub codeload',   'https://codeload.github.com/espressif/esp-idf/zip/refs/tags/v5.5.3'],
  ['GitHub API',        'https://api.github.com/repos/espressif/esp-idf'],
  ['Espressif dl',      'https://dl.espressif.com/dl/esp-idf/'],
  ['Espressif tools',   'https://dl.espressif.com/dl/idf-installer/'],
  ['Espressif 中国镜像', 'https://dl.espressif.cn/dl/esp-idf/'],
  ['MinGit (git for win)', 'https://github.com/git-for-windows/git/releases/latest'],
  ['npm registry',      'https://registry.npmjs.org/'],
];

for (const [name, url] of TARGETS) {
  const t0 = Date.now();
  try {
    const ac = new AbortController();
    const timer = setTimeout(() => ac.abort(), 20000);
    const r = await fetch(url, { redirect: 'follow', signal: ac.signal, method: 'GET' });
    clearTimeout(timer);
    const len = r.headers.get('content-length');
    console.log(`✅ ${name.padEnd(22)} HTTP ${r.status}  ${Date.now() - t0}ms  ${len ? (len / 1048576).toFixed(1) + ' MB' : ''}`);
    // 不读 body，避免下载大文件
    if (r.body) { try { await r.body.cancel(); } catch {} }
  } catch (e) {
    console.log(`❌ ${name.padEnd(22)} ${Date.now() - t0}ms  ${e.name}: ${e.message}`);
  }
}
