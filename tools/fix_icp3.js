const fs = require('fs');
const p = 'SharedProto/km_ipc_driver.c';
let b = fs.readFileSync(p);

const CAMPOS = ['ViewBase', 'SectionObject', 'ReqEvent', 'RspEvent', 'ReqEventHandle', 'RspEventHandle', 'ClientProcess', 'KeyHandle'];
let n = 0;
for (const f of CAMPOS) {
  const old = 'InterlockedExchangePointer((PVOID*)&g_St.' + f + ');';
  const nuv = 'InterlockedExchangePointer((PVOID*)&g_St.' + f + ', NULL);';
  const i = b.indexOf(old);
  if (i === -1) { console.log('FALTO:', f); continue; }
  b = Buffer.concat([b.slice(0, i), Buffer.from(nuv, 'ascii'), b.slice(i + old.length)]);
  console.log('ok', f);
  n++;
}
fs.writeFileSync(p, b);
console.log('replaced', n);
