const fs = require('fs');
const p = 'PubgExt/Main.cpp';
let s = fs.readFileSync(p, 'utf8');
const a = s.indexOf('    LOG("[*] %s: liberando as paginas residentes');
const b = s.indexOf('static void DescarregarDriver()');
if (a < 0 || b < 0 || a > b) { console.log('ANCHORS', a, b); process.exit(1); }
const tail = [
  '        origem, (unsigned long long)st.Base, (unsigned long long)st.PageBytes);',
  '',
  '    if (!NT_SUCCESS(intel_driver::Load())) {',
  '        LOG("[-] %s: nao consegui carregar o doador para a liberacao; nova tentativa no proximo start\\n", origem);',
  '        return;',
  '    }',
  '',
  '    const bool ok = intel_driver::MmFreeIndependentPages(st.Base, (uint32_t)st.PageBytes);',
  '    intel_driver::Unload();',
  '    if (ok) {',
  '        _wremove(caminho.c_str());',
  '        LOG("[+] %s: paginas da execucao anterior liberadas (0x%llX, 0x%llX bytes)\\n",',
  '            origem, (unsigned long long)st.Base, (unsigned long long)st.PageBytes);',
  '    } else {',
  '        LOG("[-] %s: MmFreeIndependentPages falhou na base antiga; registro descartado\\n", origem);',
  '        _wremove(caminho.c_str());',
  '    }',
  '}',
  '',
].join('\r\n') + '\r\n';
const rep = '    LOG("[*] %s: liberando as paginas residentes da execucao anterior (base=0x%llX size=0x%llX)...\\n"\r\n' + tail;
s = s.slice(0, a) + rep + s.slice(b);
fs.writeFileSync(p, s);
console.log('ok');
