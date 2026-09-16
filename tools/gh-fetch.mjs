// gh-fetch.mjs — 用 Node 自带 fetch 下载文件（绕过沙箱内 Schannel 无法获取凭据的问题）
//
// 用法：node tools/gh-fetch.mjs
// 清单写在本文件底部的 MANIFEST 里，格式：{ url, out }
// 输出：每个文件的大小 + SHA256 前 16 位

import { writeFile, mkdir } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { dirname, resolve } from 'node:path';

const RAW = 'https://raw.githubusercontent.com/steveEcode/obd_brz_gauge/main';
const REL = `${RAW}/firmware/release`;
const DEST = 'D:\\AtzGauge\\obd_brz_gauge';

const MANIFEST = [
  // —— 烧录用的二进制 ——
  { url: `${REL}/bootloader/bootloader.bin`,          out: `${DEST}\\firmware\\release\\bootloader\\bootloader.bin` },
  { url: `${REL}/partition_table/partition-table.bin`, out: `${DEST}\\firmware\\release\\partition_table\\partition-table.bin` },
  { url: `${REL}/ota_data_initial.bin`,               out: `${DEST}\\firmware\\release\\ota_data_initial.bin` },
  { url: `${REL}/obd_brz_gauge.bin`,                  out: `${DEST}\\firmware\\release\\obd_brz_gauge.bin` },
  { url: `${REL}/firmware.bin`,                       out: `${DEST}\\firmware\\release\\firmware.bin` },
  { url: `${REL}/bootmedia.bin`,                      out: `${DEST}\\firmware\\release\\bootmedia.bin` },

  // —— 用来核对烧录地址的元数据 ——
  { url: `${REL}/flash_address_map.txt`,              out: `${DEST}\\firmware\\release\\flash_address_map.txt` },
  { url: `${REL}/latest.json`,                        out: `${DEST}\\firmware\\release\\latest.json` },
  { url: `${RAW}/partitions.csv`,                     out: `${DEST}\\partitions.csv` },
  { url: `${RAW}/firmware/README.md`,                 out: `${DEST}\\firmware\\README.md` },
  { url: `${RAW}/README.md`,                          out: `${DEST}\\README.md` },
];

let failed = 0;
for (const { url, out } of MANIFEST) {
  const path = resolve(out);
  try {
    const res = await fetch(url, { redirect: 'follow' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const buf = Buffer.from(await res.arrayBuffer());
    await mkdir(dirname(path), { recursive: true });
    await writeFile(path, buf);
    const sha = createHash('sha256').update(buf).digest('hex').slice(0, 16);
    console.log(`OK   ${String(buf.length).padStart(9)} B  sha256:${sha}  ${out}`);
  } catch (e) {
    failed++;
    console.log(`FAIL ${url} :: ${e.message}`);
  }
}
console.log(failed === 0 ? '\n全部下载成功。' : `\n${failed} 个文件失败。`);
process.exit(failed === 0 ? 0 : 1);
