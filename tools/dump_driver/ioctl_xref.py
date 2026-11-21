#!/usr/bin/env python3
"""
ioctl_xref.py — acha as comparacoes de IOCTL num driver e mostra o contexto.

Metodo: varre o .text procurando as sequencias de instrucao que um switch de
IRP_MJ_DEVICE_CONTROL gera. Sao reconheciveis sem desassemblador completo:

    cmp  ecx, imm32        ; 81 F9 xx xx xx xx
    cmp  edx, imm32        ; 81 FA xx xx xx xx
    cmp  r32, imm32        ; 81 F8..FF xx xx xx xx
    mov  eax, imm32        ; B8 xx xx xx xx
    jz/jnz rel32           ; 0F 84/85 xx xx xx xx

Um desassemblador de verdade erra em cascata: um byte mal decodificado
desalinha todo o resto. Aqui cada sequencia e validada de forma independente,
entao um falso positivo custa uma linha de ruido, nao o resultado inteiro.

Uso:
    python ioctl_xref.py <driver.sys>
    python ioctl_xref.py <driver.sys> --family 0xF210,0xF220
    python ioctl_xref.py <driver.sys> --all
"""

import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_probe import PE  # noqa: E402

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass


def decode_ctl(v):
    return {
        'device': (v >> 16) & 0xFFFF,
        'func':   (v >> 2) & 0xFFF,
        'method': ['BUFFERED', 'IN_DIRECT', 'OUT_DIRECT', 'NEITHER'][v & 3],
        'access': ['ANY', 'READ', 'WRITE', 'RW'][(v >> 14) & 3],
    }


def find_cmp_imm32(blob, text_va):
    """Todas as instrucoes 'cmp r32, imm32' no blob.

    Formas de 6 bytes:
        81 F8..FF  imm32     cmp eax..edi, imm32
        81 F9 / 81 FA / 81 FB  (os mesmos, com ModRM)
        81 3D ...  (cmp [rip+disp], imm32) — 10 bytes, tratado a parte
    """
    out = []
    for i in range(len(blob) - 6):
        if blob[i] != 0x81:
            continue
        modrm = blob[i + 1]
        # mod = 11 (registrador) e reg = 7 (/7 e o campo de CMP)
        if (modrm & 0xC0) != 0xC0:
            continue
        if ((modrm >> 3) & 7) != 7:
            continue
        imm = struct.unpack_from('<I', blob, i + 2)[0]
        out.append((text_va + i, imm))
    return out


def find_mov_imm32(blob, text_va):
    """'mov r32, imm32' = B8..BF + imm32 (6 bytes)."""
    out = []
    for i in range(len(blob) - 5):
        b = blob[i]
        if not (0xB8 <= b <= 0xBF):
            continue
        # descarta se o byte anterior e um REX.W (viraria mov r64, imm64)
        if i > 0 and 0x48 <= blob[i - 1] <= 0x4F:
            continue
        imm = struct.unpack_from('<I', blob, i + 1)[0]
        out.append((text_va + i, imm))
    return out


def find_test_bit(blob, text_va):
    """Procura os pontos onde o resultado do cmp e usado.

    Nao decodifica: so reporta a densidade de saltos condicionais perto de
    cada cmp, que e o que indica um switch encadeado.
    """
    jumps = []
    for i in range(len(blob) - 6):
        if blob[i] == 0x0F and blob[i + 1] in (0x84, 0x85):
            jumps.append(text_va + i)
    return jumps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--family', default=None,
                    help='lista de device ids em hex separada por virgula '
                         '(ex: 0xF210,0xF220). Sem isso, mostra tudo.')
    ap.add_argument('--all', action='store_true',
                    help='mostra tambem os que nao passam no filtro de device')
    ap.add_argument('--window', type=int, default=0x40,
                    help='bytes de contexto ao redor de cada candidato')
    args = ap.parse_args()

    pe = PE(args.path)

    text = None
    for s in pe.sections:
        if s.name == '.text':
            text = s
            break
    if text is None:
        print('[err] sem secao .text')
        return 1

    blob = pe.data[text.raw_off:text.raw_off + text.raw_size]
    text_va = pe.image_base + text.vaddr

    families = None
    if args.family:
        families = {int(x, 16) for x in args.family.split(',')}

    cmps = find_cmp_imm32(blob, text_va)
    movs = find_mov_imm32(blob, text_va)

    # Filtra: so valores que decodificam como CTL_CODE plausivel
    def plausible(v):
        d = decode_ctl(v)
        return 0x0001 <= d['device'] <= 0xFFFF and d['func'] != 0

    cmps_c = [(a, v) for a, v in cmps if plausible(v)]
    movs_c = [(a, v) for a, v in movs if plausible(v)]

    print('=' * 88)
    print(f'IOCTL XREF — {os.path.basename(args.path)}')
    print('=' * 88)
    print(f'  .text VA 0x{text_va:X}  tamanho 0x{len(blob):X}')
    print(f'  cmp r32,imm32                : {len(cmps)}  ({len(cmps_c)} plausiveis)')
    print(f'  mov r32,imm32                : {len(movs)}  ({len(movs_c)} plausiveis)')

    if families:
        cmps_c = [(a, v) for a, v in cmps_c if decode_ctl(v)['device'] in families]
        movs_c = [(a, v) for a, v in movs_c if decode_ctl(v)['device'] in families]
        print(f'  filtrado por device          : {sorted(hex(f) for f in families)}')
        print(f'  cmp restantes                : {len(cmps_c)}')
        print(f'  mov restantes                : {len(movs_c)}')

    def dump(label, rows):
        print(f'\n-- {label} ' + '-' * max(0, 70 - len(label)))
        if not rows:
            print('  (nenhum)')
            return
        for va, v in rows:
            d = decode_ctl(v)
            off = va - text_va
            print(f'  0x{va:08X}  (off 0x{off:04X})  0x{v:08X}  '
                  f'dev=0x{d["device"]:04X} func=0x{d["func"]:03X} '
                  f'{d["method"]:<9} {d["access"]:<5}')

            # contexto cru em hex
            lo = max(0, off - args.window // 2)
            hi = min(len(blob), off + args.window // 2)
            ctx = blob[lo:hi]
            hexs = ' '.join(f'{b:02X}' for b in ctx)
            print(f'              ctx: {hexs}')

    dump('cmp r32, imm32', cmps_c)
    dump('mov r32, imm32', movs_c)

    # Densidade de saltos: um switch de IOCTL costuma ter varios jz/jnz juntos
    jumps = find_test_bit(blob, text_va)
    print(f'\n-- SALTOS CONDICIONAIS (0F 84/85) ' + '-' * 52)
    print(f'  total: {len(jumps)}')
    if len(jumps) > 1:
        # agrupa por proximidade
        groups = []
        cur = [jumps[0]]
        for j in jumps[1:]:
            if j - cur[-1] < 0x60:
                cur.append(j)
            else:
                groups.append(cur)
                cur = [j]
        groups.append(cur)
        big = [g for g in groups if len(g) >= 3]
        print(f'  {len(groups)} grupos, {len(big)} com 3+ saltos '
              f'(candidatos a switch):')
        for g in big[:12]:
            print(f'    0x{g[0]:08X} .. 0x{g[-1]:08X}  ({len(g)} saltos, '
                  f'{g[-1] - g[0]} bytes)')

    print()
    print('=' * 88)
    print('  Proximo passo: pegar os candidatos acima e olhar o dispatch de')
    print('  IRP_MJ_DEVICE_CONTROL em DriverEntry para confirmar quais sao o')
    print('  switch de verdade. O device family em uso fica claro ali.')
    print('=' * 88)
    return 0


if __name__ == '__main__':
    sys.exit(main())
