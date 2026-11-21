const fs = require('fs');
const p = 'PubgExt/driver/driver_interface_kevent.h';
let raw = fs.readFileSync(p);
let crlf = raw.includes(Buffer.from('\r\n'));
let s = raw.toString('utf8');
if (crlf) s = s.replace(/\r\n/g, '\n');

// 1) KmpReqSig recebe ponteiro
const a = 'KmpReqSig(sh->Req, ';
if (!s.includes(a)) throw new Error('marca A nao encontrada');
s = s.replace(a, 'KmpReqSig(&sh->Req, ');

// 2) move o bloco LastGlobalStatus..EstatisticasKernel para antes do private:
const s1 = '    uint32_t LastGlobalStatus = 0;\n';
const e1 = '    bool Rodar()\n';
const i1 = s.indexOf(s1);
const i2 = s.indexOf(e1);
if (i1 < 0 || i2 < i1) throw new Error('blocos nao localizados i1=' + i1 + ' i2=' + i2);
const bloco = s.slice(i1, i2).replace(/\s+$/, '\n\n');
s = s.slice(0, i1) + s.slice(i2);

const priv = '    bool ParadaConfirmada() const { return paradaConfirmada; }\n';
const j1 = s.indexOf(priv);
if (j1 < 0) throw new Error('paradaConfirmada nao encontrada');
const ins = j1 + priv.length;
s = s.slice(0, ins) + bloco + s.slice(ins);

let out = Buffer.from(s, 'utf8');
if (crlf) out = out.toString('utf8').replace(/\n/g, '\r\n');
fs.writeFileSync(p, out);
console.log('ok');
