#!/usr/bin/env python3
"""
km_strings.py — varre um .sys procurando os padroes que importam numa
auditoria de driver de kernel, sem precisar de desassemblador.

Procura:
  * nomes de device/symlink      \\Device\\... / \\DosDevices\\...
  * caminhos \\BaseNamedObjects\\... (marcadores de acesso, mutex)
  * secao \\Device\\PhysicalMemory
  * caminhos de executavel      \\...\\Nome.exe  (checagem de chamador!)
  * nomes de API resolvidos por MmGetSystemRoutineAddress (string + offset)
  * pares de byte que parecem CTL_CODE em .text/.rdata

O padrao "caminho de executavel" e o mais importante: driver que compara o
nome do processo chamador costuma carregar a string do executavel legitimo.

Uso:
    python km_strings.py <arquivo.sys>
    python km_strings.py <arquivo.sys> --ioctl
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

PATTERNS = [
    ('DEVICE',      re.compile(r'\\Device\\[A-Za-z0-9_\-\.]+')),
    ('SYMLINK',     re.compile(r'\\DosDevices\\[A-Za-z0-9_\-\.]+')),
    ('BASENAMED',   re.compile(r'\\BaseNamedObjects\\[A-Za-z0-9_\-\.]+')),
    ('PHYSMEM',     re.compile(r'\\Device\\PhysicalMemory')),
    ('REGISTRY',    re.compile(r'\\Registry\\[A-Za-z0-9_\\\-\. ]+')),
    ('EXE',         re.compile(r'\\(?:[A-Za-z0-9_\-\. ]+\\)+[A-Za-z0-9_\-\.]+\.exe')),
    ('GLOBALROOT',  re.compile(r'\\\?\?\\[A-Za-z0-9_\\\-\.]+')),
    ('API',         re.compile(r'^[A-Z][A-Za-z0-9_]{4,40}$')),
]


def find_all(pe):
    hits = {k: [] for k, _ in PATTERNS}

    for off, s in pe.utf16_strings(min_len=4):
        rva = pe.off_to_rva(off)
        for label, rx in PATTERNS:
            for m in rx.finditer(s):
                hits[label].append((rva, off, m.group(0), 'W'))

    for off, s in pe.ascii_strings(min_len=4):
        rva = pe.off_to_rva(off)
        for label, rx in PATTERNS:
            for m in rx.finditer(s):
                hits[label].append((rva, off, m.group(0), 'A'))

    return hits


def ioctl_scan(pe):
    """DEPRECADO. Nao use isto para achar IOCTLs.

    Desmontar bytes de codigo x64 como uint32 produz milhares de falsos
    positivos. Rodei isto no AsIO3.sys e ele apontou familias 0xF210 e 0xF684,
    que NAO existem — o device real do driver e 0xA040.

    O jeito que funciona e `ioctl_xref.py`, que reconhece as sequencias de
    instrucao do switch (`81 F9 imm32` seguido de `0F 84 rel32`) em vez de
    varrer bytes soltos.

    Mantido so para nao quebrar quem ja chama a funcao. Retorna vazio.
    """
    return {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--ioctl', action='store_true')
    args = ap.parse_args()

    pe = PE(args.path)
    hits = find_all(pe)

    print('=' * 78)
    print(f'AUDITORIA DE STRINGS — {os.path.basename(args.path)}')
    print('=' * 78)

    order = ['EXE', 'DEVICE', 'SYMLINK', 'BASENAMED', 'PHYSMEM', 'GLOBALROOT', 'REGISTRY']
    for label in order:
        items = sorted(set(hits[label]))
        if not items:
            continue
        print(f'\n-- {label} ' + '-' * (72 - len(label)))
        for rva, off, s, enc in items:
            r = f'0x{rva:08X}' if rva is not None else '   ?    '
            print(f'  {r}  {enc}  {s}')

    if args.ioctl:
        print('\n-- CANDIDATOS A IOCTL ' + '-' * 56)
        print('  DESATIVADO. Esta funcao varria bytes de codigo x64 como uint32')
        print('  e produzia milhares de falsos positivos — no AsIO3.sys apontou')
        print('  as familias 0xF210 e 0xF684, que nao existem (o device real e')
        print('  0xA040). Use ioctl_xref.py, que reconhece o padrao de instrucao')
        print('  do switch em vez de varrer bytes soltos.')

    print('\n-- APIs RESOLVIDAS POR STRING ' + '-' * 46)
    known = {'MmGetPhysicalMemoryRangesEx2', 'ZwQueryInformationProcess',
             'RtlQueryModuleInformation'}
    for off, s in pe.ascii_strings(min_len=4):
        if s in known:
            print(f'  0x{off:08X}  {s}')


if __name__ == '__main__':
    main()
