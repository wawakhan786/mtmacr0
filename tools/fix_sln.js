const fs = require('fs');
const p = 'PubgExt.sln';
let s = fs.readFileSync(p, 'utf8');
const crlf = s.includes('\r\n');
if (crlf) s = s.replace(/\r\n/g, '\n');

const blocoProjeto = 'Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "OffsetDumper", "OffsetDumper\\OffsetDumper.vcxproj", "{A3479B41-C182-4DA1-8B23-9D27BC5E9944}"\nEndProject\n';
if (!s.includes(blocoProjeto)) throw new Error('bloco do projeto nao encontrado');
s = s.replace(blocoProjeto, '');

// as 5 linhas de config da GUID do OffsetDumper
let n = 0;
s = s.replace(/^ {4}\{A3479B41-C182-4DA1-8B23-9D27BC5E9944\}\.[^\n]*\n/gm, () => { n++; return ''; });
console.log('linhas de config removidas:', n);

fs.writeFileSync(p, crlf ? s.replace(/\n/g, '\r\n') : s);
console.log('ok');
