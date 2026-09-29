// Dev-only: renders c/tests/canvas_pattern.inc with Chromium's Canvas 2D (headless) and writes
// the PNG, for tests/test_canvas.c's comparison.
//   node c/tools/ref/canvas-ref.mjs OUT.png
import { chromium } from 'playwright';
import fs from 'node:fs';

const out = process.argv[2] || 'c/build/canvas_ref.png';
const lines = fs.readFileSync('c/tests/canvas_pattern.inc', 'utf8').split('\n');
const js = ['const g0 = null; let g = null;'];
const q = (s) => JSON.stringify(s);
for (const raw of lines) {
  if (!raw.trim() || raw.startsWith('/')) continue;
  const a = raw.match(/"[^"]*"|\S+/g).map((t) => (t.startsWith('"') ? t.slice(1, -1) : t));
  const [op, ...r] = a;
  const n = (i) => +r[i];
  switch (op) {
    case 'fillColor': js.push(`c.fillStyle = ${q(r[0])};`); break;
    case 'strokeColor': js.push(`c.strokeStyle = ${q(r[0])};`); break;
    case 'fillRect': js.push(`c.fillRect(${n(0)}, ${n(1)}, ${n(2)}, ${n(3)});`); break;
    case 'strokeRect': js.push(`c.strokeRect(${n(0)}, ${n(1)}, ${n(2)}, ${n(3)});`); break;
    case 'lineWidth': js.push(`c.lineWidth = ${n(0)};`); break;
    case 'lineJoin': js.push(`c.lineJoin = ${q(r[0])};`); break;
    case 'lineCap': js.push(`c.lineCap = ${q(r[0])};`); break;
    case 'beginPath': js.push('c.beginPath();'); break;
    case 'moveTo': js.push(`c.moveTo(${n(0)}, ${n(1)});`); break;
    case 'lineTo': js.push(`c.lineTo(${n(0)}, ${n(1)});`); break;
    case 'closePath': js.push('c.closePath();'); break;
    case 'fill': js.push('c.fill();'); break;
    case 'stroke': js.push('c.stroke();'); break;
    case 'clip': js.push('c.clip();'); break;
    case 'arc': js.push(`c.arc(${n(0)}, ${n(1)}, ${n(2)}, ${n(3)}, ${n(4)}, ${r[5]});`); break;
    case 'ellipse': js.push(`c.ellipse(${r.slice(0, 7).join(', ')}, ${r[7]});`); break;
    case 'bezierTo': js.push(`c.bezierCurveTo(${r.join(', ')});`); break;
    case 'rect': js.push(`c.rect(${r.join(', ')});`); break;
    case 'linearGradient': js.push(`g = c.createLinearGradient(${r.join(', ')});`); break;
    case 'radialGradient': js.push(`g = c.createRadialGradient(${r.join(', ')});`); break;
    case 'stop': js.push(`g.addColorStop(${n(0)}, ${q(r[1])});`); break;
    case 'fillGradient': js.push('c.fillStyle = g;'); break;
    case 'save': js.push('c.save();'); break;
    case 'restore': js.push('c.restore();'); break;
    case 'translate': js.push(`c.translate(${n(0)}, ${n(1)});`); break;
    case 'scale': js.push(`c.scale(${n(0)}, ${n(1)});`); break;
    case 'filterBlur': js.push(`c.filter = 'blur(${n(0)}px)';`); break;
    case 'putImageData': {
      const [x, y, w, h] = r.map(Number);
      js.push(`{ const d = c.createImageData(${w}, ${h}); for (let j = 0; j < ${h}; j++) for (let i = 0; i < ${w}; i++) { const k = (j * ${w} + i) * 4; d.data[k] = i * 6; d.data[k + 1] = j * 25; d.data[k + 2] = 128; d.data[k + 3] = 200; } c.putImageData(d, ${x}, ${y}); }`);
      break;
    }
    default: throw new Error('unknown op ' + op);
  }
}
const browser = await chromium.launch({ headless: true, args: ['--ignore-gpu-blocklist', '--enable-gpu', '--use-angle=vulkan', '--enable-features=Vulkan', '--disable-gpu-rasterization', '--disable-accelerated-2d-canvas'] });
const page = await browser.newPage();
const url = await page.evaluate((code) => {
  const cv = document.createElement('canvas');
  cv.width = 512; cv.height = 400;
  const c = cv.getContext('2d', { willReadFrequently: true });
  new Function('c', code)(c);
  return cv.toDataURL('image/png');
}, js.join('\n'));
fs.writeFileSync(out, Buffer.from(url.split(',')[1], 'base64'));
console.log('wrote', out);
await browser.close();
