"""
xenuine_fix_check.py — confirma a hipotese do RVA no XenuineDecrypt.

HIPOTESE
--------
No disco, o slot XenuineDecrypt (RVA 0x11757E28, secao .rdata) contem:

    0x00000001591DE7B0

Isso NAO e um VA x64 (que comecaria com 0x00007FF... ou 0xFFFF...). E um RVA,
e cai dentro da secao .pubg0 (VA 0x13AC3000 .. 0x18E02000).

O `InitDecrypt` do Engine.cpp faz:

    DecryptPtr = Read(base + 0x11757E28)     -> 0x1591DE7B0
    Read(DecryptPtr, shellcode, 1024)        -> le de 0x1591DE7B0 (ENDERECO
                                                INVALIDO! falta somar base)

Se a hipotese estiver certa, o conteudo em .pubg0 + (0x1591DE7B0 - 0x13AC3000)
deve ser CODIGO comecando com uma instrucao `lea` (0x8D) — que e exatamente o
byte que o InitDecrypt procura em ShellcodeBuff[3].

Este script:
  1. converte o RVA do slot para offset no arquivo
  2. le os bytes la
  3. diz se parece codigo e se comeca com lea/mov

Uso:
    python xenuine_fix_check.py
"""

import struct
import sys
import os

PADRAO = r'C:\Program Files (x86)\Steam\steamapps\common\PUBG\TslGame\Binaries\Win64\TslGame.exe'
XENUINE_SLOT = 0x11757E28


def secoes(d, pe_off):
    nsec = struct.unpack_from('<H', d, pe_off + 6)[0]
    opt_size = struct.unpack_from('<H', d, pe_off + 20)[0]
    sec_off = pe_off + 24 + opt_size
    out = []
    for i in range(nsec):
        b = sec_off + i * 40
        nome = d[b:b + 8].rstrip(b'\0').decode('ascii', 'replace')
        vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', d, b + 8)
        out.append((nome, va, vsize, rawptr, rawsize))
    return out


def acha(rva, secs):
    for nome, va, vsize, rawptr, rawsize in secs:
        if va <= rva < va + max(vsize, rawsize):
            delta = rva - va
            if delta < rawsize:
                return rawptr + delta, nome, va
            return None, nome, va
    return None, None, None


def dump(b, rva, n=64):
    for i in range(0, min(len(b), n), 16):
        p = b[i:i + 16]
        print('      0x%08X  %-47s  %s'
              % (rva + i, ' '.join('%02X' % x for x in p),
                 ''.join(chr(x) if 32 <= x < 127 else '.' for x in p)))


def main():
    caminho = sys.argv[1] if len(sys.argv) > 1 else PADRAO
    d = open(caminho, 'rb').read()
    pe_off = struct.unpack_from('<I', d, 0x3C)[0]
    secs = secoes(d, pe_off)

    print('=' * 74)
    print('  CONFIRMACAO DA HIPOTESE DO RVA — XenuineDecrypt')
    print('=' * 74)
    print()

    off, sec, _ = acha(XENUINE_SLOT, secs)
    if off is None:
        print('  [X] o slot 0x%X nao esta no arquivo' % XENUINE_SLOT)
        return 1

    valor = struct.unpack_from('<Q', d, off)[0]
    print('  1. slot XenuineDecrypt')
    print('     RVA    0x%08X   secao %s' % (XENUINE_SLOT, sec))
    print('     valor  0x%016X' % valor)
    print()

    como_va = (valor >> 48) in (0x7FF0, 0x7FF1, 0x7FF2, 0x7FF3) or valor > 0xFFFF000000000000
    print('     parece um VA x64? %s' % ('SIM' if como_va else 'NAO'))
    print('     ---> se NAO parece VA, e um RVA (offset dentro do modulo)')
    print()

    rva_alvo = valor
    off2, sec2, va2 = acha(rva_alvo, secs)
    print('  2. tratando como RVA: 0x%X' % rva_alvo)
    if off2 is None:
        print('     cai na secao %s mas NAO no arquivo (so em memoria, .bss)' % sec2)
        return 1
    print('     secao %s (VA base 0x%08X), offset no arquivo 0x%X'
          % (sec2, va2, off2))
    print()
    bruto = d[off2:off2 + 64]
    dump(bruto, rva_alvo)
    print()

    # ------------------------------------------------------------------
    # 3. parece codigo? comeca com lea/mov?
    # ------------------------------------------------------------------
    print('  3. analise')
    print('     primeiro byte 0x%02X' % bruto[0])
    print('     segundo  byte 0x%02X' % bruto[1])

    tem_rex = bruto[0] in (0x48, 0x4C)
    if tem_rex:
        opcode = bruto[1]
        print('     REX.W presente (0x%02X) -> opcode = 0x%02X' % (bruto[0], opcode))
        if opcode == 0x8D:
            print('     *** LEA! *** exatamente o que o InitDecrypt procura')
            disp = struct.unpack_from('<i', bruto, 3)[0]
            print('     displacement em +3: 0x%X (%d)' % (disp & 0xFFFFFFFF, disp))
            print('     destino se for RIP-relativo: 0x%X + 7 + %d = 0x%X'
                  % (rva_alvo, disp, rva_alvo + 7 + disp))
        elif opcode == 0x8B:
            print('     MOV (0x8B) — o InitDecrypt trata como nao-LEA')
        else:
            print('     outro opcode')
    elif bruto[0] == 0x8D:
        print('     LEA sem REX')
    elif bruto[0] == 0x8B:
        print('     MOV sem REX')
    else:
        print('     nao comeca com lea/mov')

    # ------------------------------------------------------------------
    # 4. o InitDecrypt le a "shellcode" a partir do ENDERECO, nao do RVA.
    #    Se o valor e um RVA e ele le em [RVA] em vez de [base+RVA], o que
    #    ele ve e memoria do proprio processo em endereco baixo — lixo.
    # ------------------------------------------------------------------
    print()
    print('  4. o bug do InitDecrypt')
    print('     ele faz:  DecryptPtr = Read(base + 0x11757E28)  = 0x%X' % valor)
    print('               Read(DecryptPtr, shellcode, 1024)     <- le de 0x%X' % valor)
    print()
    print('     Endereco 0x%X fica em memoria BAIXA do processo.' % valor)
    print('     Num alvo x64 o modulo carrega em ~0x7FFxxxxxxxxx, entao')
    print('     0x%X NAO e o modulo — a leitura traz lixo (ou falha).' % valor)
    print()
    print('     A correcao: somar a base do modulo.')
    print('        DecryptPtr = base + valor')
    print('        Read(DecryptPtr, shellcode, 1024)')
    print()
    print('     Com base=0x7FF700000000 (exemplo), o destino vira')
    print('        0x%X  ->  cai em .pubg0, que e onde o codigo esta.'
          % (0x7FF700000000 + valor))

    print()
    print('=' * 74)
    print('  VEREDITO: se acima apareceu "LEA", a hipotese esta CONFIRMADA.')
    print('=' * 74)
    return 0


if __name__ == '__main__':
    sys.exit(main())
