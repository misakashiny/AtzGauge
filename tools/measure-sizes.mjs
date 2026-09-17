// 量测脚本：把 /size 设成不同值，各截一张图，量出「表情」「顶部时间」的真实像素尺寸
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const IP = process.env.ATZ_DEVICE || '192.168.1.100';
const OUT = fileURLToPath(new URL('../backup/', import.meta.url));
const PS = 'powershell';

function fetchTo(url, file) {
  execFileSync(PS, ['-NoProfile', '-Command',
    `Invoke-WebRequest -Uri '${url}' -OutFile '${file}' -TimeoutSec 25 -UseBasicParsing`],
    { stdio: 'ignore' });
}
function text(url) {
  return execFileSync(PS, ['-NoProfile', '-Command',
    `(Invoke-WebRequest -Uri '${url}' -TimeoutSec 20 -UseBasicParsing).Content`],
    { encoding: 'utf8' }).trim();
}

// 用 PowerShell 的 System.Drawing 量包围盒（与背景色差 > 阈值）
function measure(file, x0, y0, x1, y1, bgX, bgY, thresh) {
  const script = `
Add-Type -AssemblyName System.Drawing
$img=[System.Drawing.Bitmap]::FromFile('${file}')
$bg=$img.GetPixel(${bgX},${bgY})
$minx=99999;$miny=99999;$maxx=-1;$maxy=-1
for($y=${y0};$y -le ${y1};$y++){ for($x=${x0};$x -le ${x1};$x++){
  $c=$img.GetPixel($x,$y)
  if(([math]::Abs($c.R-$bg.R)+[math]::Abs($c.G-$bg.G)+[math]::Abs($c.B-$bg.B)) -gt ${thresh}){
    if($x -lt $minx){$minx=$x}; if($x -gt $maxx){$maxx=$x}
    if($y -lt $miny){$miny=$y}; if($y -gt $maxy){$maxy=$y} } } }
$img.Dispose()
if($maxx -lt 0){ '0 0' } else { "$($maxx-$minx+1) $($maxy-$miny+1)" }
`;
  const out = execFileSync(PS, ['-NoProfile', '-Command', script], { encoding: 'utf8' }).trim().split(/\s+/);
  return { w: parseInt(out[0], 10), h: parseInt(out[1], 10) };
}

const cases = [
  { emoji: 100, clock: 100, tag: 'base' },
  { emoji: 125, clock: 140, tag: 'cur' },
  { emoji: 150, clock: 160, tag: 'big' },
];

console.log('表情素材原始 128x128 px；顶部时间原始字号 20 px（line_height 28）\n');
console.log('设定值            顶栏时间(实测)      中央表情(实测)');
for (const c of cases) {
  console.log('  set ->', text(`http://${IP}:8099/size?emoji=${c.emoji}&clock=${c.clock}`));
  execFileSync(PS, ['-NoProfile', '-Command', 'Start-Sleep -Milliseconds 1200']);
  const file = `${OUT}/size-${c.tag}.jpg`;
  fetchTo(`http://${IP}:8099/shot.jpg`, file);
  const clock = measure(file, 150, 0, 359, 48, 20, 200, 60);      // 右上角时间
  const emoji = measure(file, 60, 60, 299, 299, 15, 200, 40);     // 中央表情
  console.log(`emoji=${String(c.emoji).padStart(3)}% clock=${String(c.clock).padStart(3)}%   ` +
              `clock ${String(clock.w).padStart(3)}x${String(clock.h).padStart(2)} px      ` +
              `emoji ${String(emoji.w).padStart(3)}x${String(emoji.h).padStart(3)} px   -> ${file}`);
}
