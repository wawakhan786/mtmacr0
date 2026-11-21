const fs = require('fs');
const c = fs.readFileSync('C:/Program Files (x86)/Windows Kits/10/Include/10.0.28000.0/km/wdm.h', 'utf8').split(/\r?\n/);
// localiza a linha do define e sobe ate encontrar o #if
let alvo = -1;
for (let i = 0; i < c.length; i++) {
  if (c[i].includes('define KeQueryInterruptTime')) { alvo = i; break; }
}
console.log('define na linha', alvo + 1);
let prof = 0;
for (let i = alvo; i >= 0; i--) {
  const t = c[i].trim();
  if (/^#(ifdef|ifndef|if)\b/.test(t)) {
    prof++;
    console.log((i + 1) + ': ' + t);
    if (prof >= 3) break;
  } else if (/^#endif/.test(t)) {
    prof--;
  } else if (/^#elif|^#else/.test(t) && prof >= 1) {
    console.log((i + 1) + ': ' + t);
  }
}
