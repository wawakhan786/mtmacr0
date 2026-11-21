"""
xref_string.py — acha referencias (lea rip-rel) a um dado endereco num PE.

Resolve o problema que o disasm_min.py tinha: em vez de tentar desmontar o
.text inteiro (e dessincronizar em bytes do meio de instrucoes), procura o
PADRAO do encoding `lea reg, [rip+disp32]` que aponta pro alvo. E a mesma
ideia do ioctl_xref.py.

    lea r64, [rip+disp32]   =  48/4C 8D <modrm>  <disp32>
                                ^modrm reg=0 (mod=00, rm=101)

Uso:
    python xref_string.py <arquivo.sys> <endereco-imagem> [--len N]
"""

import argparse
import struct
import sys

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass


def parse_pe_sections(data):
    """Retorna (image_base, secao_texto) ou None."""
    if data[:2] != b'MZ':
        return None
    e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b'PE\0\0':
        return None
    coff = e_lfanew + 4
    nsec = struct.unpack_from('<H', data, coff + 2)[0]
    opt_size = struct.unpack_from('<H', data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from('<H', data, opt)[0]
    image_base = struct.unpack_from('<Q', data, opt + 24)[0] if magic == 0x20B \
        else struct.unpack_from('<I', data, opt + 28)[0]
    sec = opt + opt_size
    out = []
    for i in range(nsec):
        s = sec + i * 40
        name = data[s:s + 8].rstrip(b'\0').decode('ascii', 'replace')
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', data, s + 8)
        out.append({'name': name, 'va': vaddr, 'vsize': vsize,
                    'ptr': rawptr, 'size': rawsize})
    return image_base, out


def find_lea_xrefs(data, image_base, sections, target_va, max_hits=60):
    """Procura `lea reg,[rip+disp32]` que resolve para target_va."""
    hits = []
    for s in sections:
        if s['name'] != '.text':
            continue
        blob = data[s['ptr']:s['ptr'] + s['size']]
        base_va = image_base + s['va']
        for i in range(len(blob) - 7):
            # REX.W (48/4C) + 8D + modrm com reg=000 e rm=101
            if blob[i] not in (0x48, 0x4C):
                continue
            if blob[i + 1] != 0x8D:
                continue
            modrm = blob[i + 2]
            if (modrm & 0xC7) != 0x05:      # mod=00, reg=000, rm=101
                continue
            disp = struct.unpack_from('<i', blob, i + 3)[0]
            insn_va = base_va + i
            if insn_va + 7 + disp == target_va:
                hits.append(insn_va)
                if len(hits) >= max_hits:
                    return hits
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('target', help='VA do alvo (hex), ex.: 0x14000A400')
    ap.add_argument('--len', type=int, default=32, help='bytes de contexto')
    args = ap.parse_args()

    data = open(args.path, 'rb').read()
    parsed = parse_pe_sections(data)
    if not parsed:
        print('nao e um PE valido')
        return 1
    image_base, sections = parsed
    target = int(args.target, 16)

    print(f'arquivo    : {args.path}')
    print(f'image base : 0x{image_base:X}')
    print(f'alvo       : 0x{target:X}  (RVA 0x{target - image_base:X})')
    print()

    name = '?'
    for s in sections:
        if s['va'] <= target - image_base < s['va'] + max(s['vsize'], s['size']):
            name = s['name']
            off = s['ptr'] + (target - image_base - s['va'])
            if name == '.rdata':
                raw = data[off:off + 200]
                # string unicode?
                try:
                    txt = raw.decode('utf-16-le')
                    txt = txt.split('\x00')[0]
                    if txt.isprintable() and len(txt) > 2:
                        print(f'conteudo   : "{txt}"  (unicode)')
                        break
                except Exception:
                    pass
                try:
                    txt = raw.split(b'\x00')[0].decode('ascii')
                    if txt.isprintable():
                        print(f'conteudo   : "{txt}"  (ascii)')
                except Exception:
                    pass
            break
    print(f'secao      : {name}')
    print()

    hits = find_lea_xrefs(data, image_base, sections, target)
    print(f'referencias (lea rip-rel): {len(hits)}')
    for h in hits:
        rva = h - image_base
        ptr = None
        for s in sections:
            if s['va'] <= rva < s['va'] + s['size']:
                ptr = s['ptr'] + (rva - s['va'])
                break
        ctx = data[ptr:ptr + args.len].hex(' ') if ptr else ''
        print(f'  0x{h:X}  (rva 0x{rva:X})  {ctx}')
    return 0 if hits else 2


if __name__ == '__main__':
    sys.exit(main())
