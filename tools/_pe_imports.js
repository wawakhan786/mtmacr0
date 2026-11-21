const fs = require('fs');
const f = process.argv[2];
const b = fs.readFileSync(f);
const pe = b.readUInt32LE(0x3c);
const optOff = pe + 24;
const magic = b.readUInt16LE(optOff);
const is64 = magic === 0x20b;
if (!is64) console.log('AVISO: PE de 32 bits');
const nSec = b.readUInt16LE(pe + 6);
const secs = [];
for (let i = 0; i < nSec; i++) {
  const s = optOff + 20 + i * 40;
  secs.push({ name: b.toString('ascii', s, s + 8), rva: b.readUInt32LE(s + 12), off: b.readUInt32LE(s + 20), vsz: b.readUInt32LE(s + 8) });
}
function toOff(rva) {
  if (!rva) return -1;
  for (const s of secs) {
    const sz = s.vsz || 0x1000;
    if (rva >= s.rva && rva < s.rva + sz) return s.off + (rva - s.rva);
  }
  return -1;
}
function cstr(off) {
  if (off < 0 || off >= b.length) return '';
  let e = off;
  while (e < b.length && b[e]) e++;
  return b.toString('ascii', off, e);
}
console.log('secoes:', secs.map(s => `${s.name}@rva ${s.rva.toString(16)} off ${s.off.toString(16)} vsz ${s.vsz.toString(16)}`).join(' | '));

// imports
let impRva;
if (is64) impRva = b.readUInt32LE(optOff + 112); else impRva = b.readUInt32LE(optOff + 96);
const io = toOff(impRva);
console.log('\nIMPORTS (impRva=' + impRva.toString(16) + '):');
if (io >= 0) {
  let i = 0;
  while (i < 32) {
    const d = io + i * 20;
    const dllRva = b.readUInt32LE(d);
    if (!dllRva) break;
    const dll = cstr(toOff(dllRva));
    const iltRva = b.readUInt32LE(d + 12) || dllRva;
    const ilt = toOff(iltRva);
    const names = [];
    let j = 0;
    while (ilt >= 0 && j < 512) {
      const t = b.readUInt32LE(ilt + j * 4);
      if (!t) break;
      if (t & 0x80000000) names.push('ord#' + (t & 0xffff));
      else names.push(cstr(toOff(t) + 2));
      j++;
    }
    console.log('  [' + dll + '] ' + names.join(', '));
    i++;
  }
}
