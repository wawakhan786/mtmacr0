"""
xref_import.py — acha quem CHAMA uma funcao importada (via IAT).

Por que existe: para saber se o driver valida o chamador por NOME de processo
ou por CAMINHO completo, preciso ver onde ele chama `_stricmp` / `wcsstr` e
com que argumentos. `xref_string.py` acha referencias a strings; este acha
referencias a funcoes.

Como funciona:

  1. Le a Import Address Table e acha o endereco (slot) de cada import.
  2. No .text, procura `ff 15 <rip-rel>` (call qword ptr [rip+disp32]) que
     resolva para o slot daquele import.

So isso. Nao tenta desmontar o resto — a mesma ideia do ioctl_xref.py.

Uso:
    python xref_import.py <arquivo.sys> --func _stricmp
    python xref_import.py <arquivo.sys> --list
"""

import argparse
import struct
import sys

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass


def parse_pe(data):
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
    pe64 = (magic == 0x20B)
    image_base = struct.unpack_from('<Q', data, opt + 24)[0] if pe64 \
        else struct.unpack_from('<I', data, opt + 28)[0]

    # data directory 1 = import
    dd = opt + (112 if pe64 else 96)
    imp_rva, imp_size = struct.unpack_from('<II', data, dd + 8)

    sec = opt + opt_size
    sections = []
    for i in range(nsec):
        s = sec + i * 40
        name = data[s:s + 8].rstrip(b'\0').decode('ascii', 'replace')
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', data, s + 8)
        sections.append({'name': name, 'va': vaddr, 'vsize': vsize,
                         'ptr': rawptr, 'size': rawsize})
    return {'base': image_base, 'sections': sections,
            'imp_rva': imp_rva, 'imp_size': imp_size, 'pe64': pe64}


def rva_to_off(pe, rva):
    for s in pe['sections']:
        if s['va'] <= rva < s['va'] + max(s['size'], s['vsize']):
            return s['ptr'] + (rva - s['va'])
    return None


def list_imports(data, pe):
    """Retorna { 'dll!func': iat_slot_va }."""
    out = {}
    off = rva_to_off(pe, pe['imp_rva'])
    if off is None:
        return out
    idx = 0
    while True:
        desc = off + idx * 20
        ilt, _, _, name_rva, iat_rva = struct.unpack_from('<IIIII', data, desc)
        if name_rva == 0 and iat_rva == 0:
            break
        no = rva_to_off(pe, name_rva)
        dll = data[no:data.index(b'\0', no)].decode('ascii', 'replace') \
            if no is not None else '?'
        thunk_rva = ilt if ilt else iat_rva
        to = rva_to_off(pe, thunk_rva)
        k = 0
        if to is not None:
            step = 8 if pe['pe64'] else 4
            while True:
                val = struct.unpack_from('<Q' if pe['pe64'] else '<I',
                                         data, to + k * step)[0]
                if val == 0:
                    break
                if not (val & (1 << (63 if pe['pe64'] else 31))):
                    ho = rva_to_off(pe, val & 0x7FFFFFFF)
                    if ho is not None:
                        fn = data[ho + 2:data.index(b'\0', ho + 2)] \
                            .decode('ascii', 'replace')
                        slot_va = pe['base'] + iat_rva + k * step
                        out[f'{dll}!{fn}'] = slot_va
                k += 1
        idx += 1
    return out


def find_calls_to_slot(data, pe, slot_va, max_hits=80):
    """Procura `ff 15 <rip-rel>` (call [rip+disp]) que resolva para slot_va."""
    hits = []
    text = next((s for s in pe['sections'] if s['name'] == '.text'), None)
    if not text:
        return hits
    blob = data[text['ptr']:text['ptr'] + text['size']]
    base_va = pe['base'] + text['va']
    for i in range(len(blob) - 6):
        if blob[i] != 0xFF or blob[i + 1] != 0x15:
            continue
        disp = struct.unpack_from('<i', blob, i + 2)[0]
        if base_va + i + 6 + disp == slot_va:
            hits.append(base_va + i)
            if len(hits) >= max_hits:
                break
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--func', help='nome da funcao, ex.: _stricmp')
    ap.add_argument('--list', action='store_true', help='lista todos os imports')
    ap.add_argument('--search', help='procura por substring no nome da API')
    ap.add_argument('--context', type=int, default=48)
    args = ap.parse_args()

    data = open(args.path, 'rb').read()
    pe = parse_pe(data)
    if not pe:
        print('nao e um PE valido')
        return 1

    imports = list_imports(data, pe)

    if args.search:
        hits = [k for k in imports if args.search.lower() in k.lower()]
        if not hits:
            print(f'nenhuma API com "{args.search}" na tabela de imports')
            return 2
        for k in sorted(hits):
            calls = find_calls_to_slot(data, pe, imports[k])
            print(f'{k}')
            print(f'  IAT 0x{imports[k]:X}   chamadas diretas: {len(calls)}')
            for h in calls[:8]:
                print(f'    call em 0x{h:X}')
        return 0

    if args.list:
        print(f'{len(imports)} imports:')
        for k in sorted(imports):
            print(f'  0x{imports[k]:X}  {k}')
        return 0

    if not args.func:
        print('use --func NOME, --search SUBSTR ou --list')
        return 1

    alvos = [k for k in imports if k.split('!')[-1].lower() == args.func.lower()]
    if not alvos:
        print(f'import "{args.func}" nao encontrado')
        return 2

    for k in alvos:
        slot = imports[k]
        hits = find_calls_to_slot(data, pe, slot)
        print(f'{k}   IAT em 0x{slot:X}   chamadas: {len(hits)}')
        for h in hits:
            rva = h - pe['base']
            o = rva_to_off(pe, rva)
            ctx = data[o:o + args.context].hex(' ') if o is not None else ''
            print(f'    call em 0x{h:X}  (rva 0x{rva:X})')
            print(f'      antes : {data[max(0,o-24):o].hex(" ")}')
            print(f'      call  : {ctx}')
    return 0 if alvos else 2


if __name__ == '__main__':
    sys.exit(main())
