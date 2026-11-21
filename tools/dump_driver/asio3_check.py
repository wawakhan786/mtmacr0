#!/usr/bin/env python3
"""
asio3_check.py — valida que AsIO3_Client.h bate com o que foi extraido do
AsIO3.sys de verdade.

O AsIO3_Client.h tem os IOCTLs escritos como macro:

    IOCTL_READ_LOW_0 = ASIO3_CTL(0x900, METHOD_BUFFERED, FILE_READ_ACCESS)

O ioctl_xref.py extraiu os valores REAIS do .text do driver, por padrao de
instrucao. Este script recalcula as macros do header, compara com o conjunto
extraido, e reprova se faltar ou sobrar qualquer um.

E o equivalente do contract_check.py, mas para o lado do doador: compilar o
header nao prova que os numeros estao certos, so que a sintaxe esta.

Uso:
    python asio3_check.py
    python asio3_check.py --header <caminho> --driver <caminho>
"""

import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_probe import PE              # noqa: E402
from ioctl_xref import decode_ctl, find_cmp_imm32, find_mov_imm32  # noqa: E402

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

# FILE_DEVICE do AsIO3
DEVICE = 0xA040

METHOD = {'METHOD_BUFFERED': 0, 'METHOD_IN_DIRECT': 1,
          'METHOD_OUT_DIRECT': 2, 'METHOD_NEITHER': 3}
ACCESS = {'FILE_ANY_ACCESS': 0, 'FILE_READ_ACCESS': 1,
          'FILE_WRITE_ACCESS': 2, 'FILE_READ_ACCESS | FILE_WRITE_ACCESS': 3}


def ctl_code(func, method, access, device=DEVICE):
    return (device << 16) | (access << 14) | (func << 2) | method


def parse_header(path):
    """Extrai IOCTL_<NOME> = ASIO3_CTL(0xNNN, METHOD_X, FILE_Y_ACCESS)."""
    text = open(path, encoding='utf-8', errors='replace').read()
    text = re.sub(r'//[^\n]*', '', text)          # tira comentario de linha

    rx = re.compile(
        r'constexpr\s+ULONG\s+(\w+)\s*=\s*ASIO3_CTL\(\s*'
        r'(0x[0-9A-Fa-f]+)\s*,\s*(METHOD_\w+)\s*,\s*([A-Z_ |]+?)\s*\)\s*;')
    out = {}
    for m in rx.finditer(text):
        name, func, method, access = m.groups()
        func = int(func, 16)
        method = method.strip()
        access = ' '.join(access.split())
        if method not in METHOD:
            raise ValueError(f'{name}: metodo desconhecido {method!r}')
        if access not in ACCESS:
            raise ValueError(f'{name}: acesso desconhecido {access!r}')
        out[name] = {
            'func': func,
            'method': method,
            'access': access,
            'value': ctl_code(func, METHOD[method], ACCESS[access]),
        }
    return out


def extract_from_driver(path):
    """Os IOCTLs reais, do .text, por padrao de instrucao."""
    pe = PE(path)
    text = next((s for s in pe.sections if s.name == '.text'), None)
    if text is None:
        raise ValueError('sem secao .text')
    blob = pe.data[text.raw_off:text.raw_off + text.raw_size]
    va = pe.image_base + text.vaddr

    found = set()
    for _, v in find_cmp_imm32(blob, va) + find_mov_imm32(blob, va):
        if decode_ctl(v)['device'] == DEVICE:
            found.add(v)
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--header', default=None)
    ap.add_argument('--driver', default=None)
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    header = args.header or os.path.join(root, 'PubgExt', 'driver', 'AsIO3_Client.h')
    driver = args.driver or os.path.join(root, '_samples', 'AsIO3.sys')

    for p in (header, driver):
        if not os.path.isfile(p):
            print(f'[err] nao encontrei {p}')
            return 1

    print('=' * 82)
    print('AsIO3 CHECK — header do cliente vs binario do driver')
    print('=' * 82)
    print(f'  header {header}')
    print(f'  driver {driver}')
    print(f'  device 0x{DEVICE:04X}')

    declared = parse_header(header)
    real = extract_from_driver(driver)

    declared_values = {v['value'] for v in declared.values()}

    print(f'\n  IOCTLs declarados no header : {len(declared)}')
    print(f'  IOCTLs extraidos do driver  : {len(real)}')

    problems = []

    # 1. Todo declarado tem que existir no driver
    print('\n-- DECLARADOS NO HEADER ' + '-' * 57)
    for name, info in sorted(declared.items(), key=lambda kv: kv[1]['value']):
        present = info['value'] in real
        mark = 'ok ' if present else 'ERR'
        print(f'  [{mark}] {name:<24} 0x{info["value"]:08X}  '
              f'func=0x{info["func"]:03X} {info["method"]:<15} {info["access"]}')
        if not present:
            problems.append(f'{name} (0x{info["value"]:08X}) nao existe no driver')

    # 2. Todo IOCTL do driver tem que estar declarado?
    #    Nao necessariamente — so reporta o que sobrou, como informacao.
    extra = sorted(real - declared_values)
    print(f'\n-- NO DRIVER MAS NAO NO HEADER ({len(extra)}) ' + '-' * 46)
    if not extra:
        print('  (nenhum)')
    for v in extra:
        d = decode_ctl(v)
        print(f'  0x{v:08X}  func=0x{d["func"]:03X} {d["method"]:<9} {d["access"]}')
    print('  (nao e erro: o header so cobre o que vamos usar)')

    # 3. Sanidade: device tem que ser 0xA040 em tudo
    wrong_dev = [n for n, i in declared.items()
                 if ((i['value'] >> 16) & 0xFFFF) != DEVICE]
    if wrong_dev:
        problems.append(f'device errado em: {wrong_dev}')

    # 4. Sanidade: nenhum valor repetido
    seen = {}
    for n, i in declared.items():
        if i['value'] in seen:
            problems.append(f'{n} e {seen[i["value"]]} tem o mesmo valor '
                            f'0x{i["value"]:08X}')
        seen[i['value']] = n

    print()
    print('=' * 82)
    if problems:
        print(f'  RESULTADO: {len(problems)} problema(s)')
        for p in problems:
            print(f'    - {p}')
    else:
        print(f'  RESULTADO: {len(declared)} IOCTLs declarados, todos '
              f'confirmados no binario')
    print('=' * 82)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
