const fs = require('fs');
const p = 'SharedProto/km_ipc_driver.c';
let b = fs.readFileSync(p);

const OPEN = 'InterlockedCompareExchangePointer(';
const END = ', NULL);';
const ESPERADOS = ['SectionObject', 'ReqEvent', 'RspEvent', 'ReqEventHandle', 'RspEventHandle', 'ClientProcess', 'KeyHandle'];

// coleta todos os sites
let sites = [], q = 0;
while ((q = b.indexOf(OPEN, q)) !== -1) { sites.push(q); q += 1; }
console.log('sites:', sites.length);

// substitui da direita p/ esquerda (offsets estavel)
let n = 0;
for (let s = sites.length - 1; s >= 0; s--) {
  const i = sites[s];
  const tailStart = i + OPEN.length;
  const j = b.indexOf(END, tailStart);
  if (j === -1 || j - tailStart > 90) continue;           // nao e da forma alvo (ex.: a do ViewBase ja trocada)
  const seg = b.toString('utf8', tailStart, j);
  const k = seg.indexOf('&g_St.');
  if (k === -1) continue;
  const field = seg.slice(k + 1).replace(/\s+/g, '');
  if (!/^[A-Za-z0-9_.]+$/.test(field)) { console.log('campo suspeito:', field); continue; }
  const consumedInBuf = (j - tailStart) + END.length;
  const repl = 'InterlockedCompareExchangePointer(NULL, (PVOID)(ULONG_PTR)' + field + ')';
  b = Buffer.concat([b.slice(0, i), Buffer.from(repl, 'ascii'), b.slice(tailStart + consumedInBuf)]);
  console.log('ok', field);
  n++;
}
fs.writeFileSync(p, b);
console.log('replaced', n);
