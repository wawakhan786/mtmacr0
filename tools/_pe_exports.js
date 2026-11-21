const fs = require('fs');

function peView(b) {
  const pe = b.readUInt32LE(0x3c);
  const optOff = pe + 24;
  const is64 = b.readUInt16LE(optOff) === 0x20b;
  const nSec = b.readUInt16LE(pe + 6);
  const secStart = optOff + 20;
  const secs = [];
  for (let i = 0; i < nSec; i++) {
    const s = secStart + i * 40;
    secs.push({ rva: b.readUInt32LE(s + 12), off: b.readUInt32LE(s + 20), vsz: b.readUInt32LE(s + 8) });
  }
  return { is64, optOff, secs };
}

function rvaToOff(v, rva) {
  if (!rva) return -1;
  for (const s of v.secs)
    if (rva >= s.rva && rva < s.rva + Math.max(s.vsz, s.rva ? 0x1000000 : 0)) return s.off + (rva - s.rva);
  // fallback: virtual size can be 0 for very small sections
  for (const s of v.secs)
    if (rva >= s.rva && rva < s.rva + 0x1000000) return s.off + (rva - s.rva);
  return -1;
}

function cstrAt(b, off) {
  if (off < 0 || off >= b.length) return '';
  let e = off;
  while (e < b.length && b[e]) e++;
  return b.toString('ascii', off, e);
}

function dumpExports(f) {
  const b = fs.readFileSync(f);
  const v = peView(b);
  const expRva = v.is64 ? b.readUInt32LE(v.optOff + 112) : b.readUInt32LE(v.optOff + 96);
  const o = rvaToOff(v, expRva);
  if (o < 0) { console.log(f, ': sem export directory'); return; }
  const numFns = b.readUInt32LE(o + 16);
  const numNames = b.readUInt32LE(o + 20);
  const fnRva = b.readUInt32LE(o + 24);
  const noRva = b.readUInt32LE(o + 28);
  const nmRva = b.readUInt32LE(o + 32);
  console.log('=== ' + f + ' (' + numFns + ' fns, ' + numNames + ' named)');
  const names = [];
  for (let i = 0; i < numNames; i++) {
    const n = cstrAt(b, rvaToOff(v, b.readUInt32LE(nmRva - (expRva) + noRva + i * 4)) );
    // note: nmRva/noRva are RVAs; need their file offsets directly
  }
  // simpler second pass done correctly below
  const res = [];
  for (let i = 0; i < numNames; i++) {
    const ordIndex = b.readUInt16LE(rvaToOff(v, noRva) + i * 2);
    const nameStr = cstrAt(b, rvaToOff(v, b.readUInt32LE(rvaToOff(v, nmRva) + i * 4)));
    res.push({ name: nameStr, ord: ordIndex });
  }
  for (const e of res) if (/kbd|mou/i.test(e.name)) console.log('  ' + e.name);
}

for (const f of process.argv.slice(2)) dumpExports(f);
