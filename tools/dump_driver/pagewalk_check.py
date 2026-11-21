#!/usr/bin/env python3
"""
pagewalk_check.py â€” valida a aritmetica do page-table walk x64 do driver
contra tabelas de pagina sinteticas, em user mode.

Por que isso existe: o walk de 4 niveis do km_driver_source.c nao pode ser
testado sem carregar um driver de kernel. Mas os ERROS que importam nele nao
sao de kernel â€” sao de aritmetica de bits:

  * indice errado por nivel (39/30/21/12 trocado)
  * mascara de PFN errada (perde bit alto, endereco fisico truncado)
  * pagina grande de 2 MiB / 1 GiB com deslocamento de 4 KiB

Isso da pra testar em user mode. O script:

  1. le as constantes do PROPRIO km_driver_source.c (mascaras, flags, shifts)
  2. monta tabelas de pagina sinteticas em memoria (PML4 -> PDPT -> PD -> PT)
  3. roda o algoritmo equivalente em Python
  4. confere o resultado, casos de aliasing e casos negativos

Se o .c mudar mascara ou shift, este teste quebra â€” de proposito.

Uso:
    python pagewalk_check.py
    python pagewalk_check.py --src caminho\\km_driver_source.c
"""

import argparse
import os
import re
import sys

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

# ---------------------------------------------------------------------------
# Constantes, lidas do driver
# ---------------------------------------------------------------------------

def parse_driver(src_path):
    text = open(src_path, encoding='utf-8', errors='replace').read()

    def const(name, default=None):
        m = re.search(r'#define\s+' + re.escape(name) + r'\s+\(?\s*'
                      r'(0x[0-9A-Fa-f]+|\d+)\s*(?:ULL|UL|L)?\s*\)?', text)
        return int(m.group(1), 0) if m else default

    # shifts e mascaras por nivel, do jeito nomeado do driver.
    # Percorre os quatro niveis para que uma divergencia entre eles apareca.
    shifts = {}
    masks = {}
    for lvl in ('PML4', 'PDPT', 'PD', 'PT'):
        s = const(f'KM_IDX_SHIFT_{lvl}')
        m = const(f'KM_IDX_MASK_{lvl}')
        if s is not None:
            shifts[s] = m if m is not None else 0x1FF

    # fallback: forma literal "(va >> 39) & 0x1FF" dentro do walk
    if not shifts:
        for m in re.finditer(r'\(\s*va\s*>>\s*(\d+)\s*\)\s*&\s*(0x[0-9A-Fa-f]+|\d+)', text):
            shifts[int(m.group(1))] = int(m.group(2), 0)

    return {
        'PTE_PRESENT':  const('KM_PTE_PRESENT'),
        'PTE_PFN_MASK': const('KM_PTE_PFN_MASK'),
        'PTE_PS':       const('KM_PTE_PS'),
        'shifts':       shifts,
    }


# ---------------------------------------------------------------------------
# Memoria fisica sintetica
# ---------------------------------------------------------------------------

class PhysMem:
    def __init__(self):
        self.mem = {}

    def write(self, addr, data):
        for i, b in enumerate(data):
            self.mem[addr + i] = b

    def write_u64(self, addr, value):
        self.write(addr, value.to_bytes(8, 'little'))

    def read_u64(self, addr):
        out = bytearray()
        for i in range(8):
            if (addr + i) not in self.mem:
                return None
            out.append(self.mem[addr + i])
        return int.from_bytes(out, 'little')


# ---------------------------------------------------------------------------
# O algoritmo, espelhando KmWalkPageTables
# ---------------------------------------------------------------------------

class WalkError(Exception):
    pass


# Acima disto o endereco fisico nao existe em x64: o formato de pagina usa
# 52 bits (12..51). Espelha KM_MAX_PHYS_ADDR em km_driver_source.c.
MAX_PHYS_ADDR = 0x1000000000000      # 2^52


def walk(phys, cr3, va, c):
    """Modelo do KmWalkPageTables. Tem que recusar TUDO que o driver recusa.

    As validacoes de PFN aqui nao sao decoracao: sem elas o walk aceita uma
    entrada presente com PFN zerado, mapeia o endereco fisico 0, e o
    MmMapIoSpaceEx derruba a maquina com PAGE_FAULT_IN_NONPAGED_AREA.
    Foi exatamente esse o BSOD.
    """
    present = c['PTE_PRESENT']
    pfn     = c['PTE_PFN_MASK']
    ps      = c['PTE_PS']
    shifts  = c['shifts']

    if len(shifts) != 4:
        raise WalkError(f'esperava 4 shifts de indice no .c, achei {sorted(shifts)}')

    levels = sorted(shifts.keys(), reverse=True)     # 39, 30, 21, 12
    s_pml4, s_pdpt, s_pd, s_pt = levels

    def idx(v, s):
        return (v >> s) & shifts[s]      # mascara do proprio nivel

    def pfn_of(entry, name):
        """PFN de uma entrada de tabela, com as validacoes do driver."""
        p = entry & pfn
        if p == 0:
            raise WalkError(f'{name}: PFN zerado (mapearia o fisico 0)')
        if p >= MAX_PHYS_ADDR:
            raise WalkError(f'{name}: PFN 0x{p:X} acima do topo fisico')
        return p

    # CR3 so tem o PFN do PML4 nos bits 51:12
    if cr3 & pfn == 0:
        raise WalkError('CR3: PFN zerado')
    if (cr3 & pfn) >= MAX_PHYS_ADDR:
        raise WalkError(f'CR3: PFN 0x{cr3 & pfn:X} acima do topo fisico')

    table = cr3 & pfn
    for name, shift in (('PML4', s_pml4), ('PDPT', s_pdpt),
                        ('PD', s_pd), ('PT', s_pt)):
        entry = phys.read_u64(table + idx(va, shift) * 8)
        if entry is None:
            raise WalkError(f'{name}: endereco fisico nao mapeado')
        if (entry & present) != present:
            raise WalkError(f'{name}: entrada nao presente')
        if entry & ps:
            if name == 'PDPT':                       # pagina de 1 GiB
                base = entry & 0x000FFFFFC0000000
                if base == 0:
                    raise WalkError('PDPT: base da pagina grande zerada')
                return (base, va & 0x3FFFFFFF)
            if name == 'PD':                         # pagina de 2 MiB
                base = entry & 0x000FFFFFFFE00000
                if base == 0:
                    raise WalkError('PD: base da pagina grande zerada')
                return (base, va & 0x1FFFFF)
            raise WalkError(f'{name}: PS setado onde nao devia')
        table = pfn_of(entry, name)

    # o ultimo `table` veio da PT, ja validado por pfn_of
    return (table, va & 0xFFF)


# ---------------------------------------------------------------------------
# Montagem das tabelas
# ---------------------------------------------------------------------------
#
# CUIDADO com a escolha dos PFNs: as mascaras do walk cortam bits.
#   PFN de 4 KiB  -> 0x000FFFFFFFFFF000   (bits 12..51)
#   PFN de 2 MiB  -> 0x000FFFFFFFE00000   (bits 21..51, 2 MiB alinhado)
#   PFN de 1 GiB  -> 0x000FFFFFC0000000   (bits 30..51, 1 GiB alinhado)
# Se o PFN de teste cair fora da mascara, o walk devolve 0 e o teste "passa"
# sem testar nada. O script recusa base zero por isso.

RAM_BASE = 0x0000_1000_0000_0000


def table_page(n):
    return RAM_BASE + n * 0x1000


def big_page_2m(n):
    return RAM_BASE + n * 0x20_0000


def big_page_1g(n):
    return RAM_BASE + n * 0x4000_0000


def table_addrs(va):
    """Enderecos de tabela derivados do VA.

    Isolado numa funcao para que a montagem e os testes negativos olhem
    exatamente os mesmos enderecos.
    """
    pml4_i = (va >> 39) & 0x1FF
    pdpt_i = (va >> 30) & 0x1FF
    pd_i   = (va >> 21) & 0x1FF
    pt_i   = (va >> 12) & 0x1FF
    return {
        'pml4_i': pml4_i, 'pdpt_i': pdpt_i, 'pd_i': pd_i, 'pt_i': pt_i,
        'cr3':       table_page(0x10 + pml4_i),
        'pdpt_base': table_page(0x100 + pdpt_i),
        'pd_base':   table_page(0x400 + pd_i),
        'pt_base':   table_page(0x1000 + pt_i),
    }


def build_tables(phys, va, *, level, page=None):
    """Monta PML4 -> PDPT -> PD -> PT ate o nivel pedido.

    Retorna (cr3, expected_base, expected_offset).
    """
    a = table_addrs(va)

    phys.write_u64(a['cr3'] + a['pml4_i'] * 8, a['pdpt_base'] | 0x3)

    if level == 'pdpt':
        base = big_page_1g(0x20 + a['pdpt_i'])
        assert base & 0x3FFFFFFF == 0, 'PFN de 1 GiB mal alinhado'
        phys.write_u64(a['pdpt_base'] + a['pdpt_i'] * 8, base | 0x3 | 0x80)
        return a['cr3'], base, va & 0x3FFFFFFF

    phys.write_u64(a['pdpt_base'] + a['pdpt_i'] * 8, a['pd_base'] | 0x3)

    if level == 'pd':
        base = big_page_2m(0x40 + a['pd_i'])
        assert base & 0x1FFFFF == 0, 'PFN de 2 MiB mal alinhado'
        phys.write_u64(a['pd_base'] + a['pd_i'] * 8, base | 0x3 | 0x80)
        return a['cr3'], base, va & 0x1FFFFF

    phys.write_u64(a['pd_base'] + a['pd_i'] * 8, a['pt_base'] | 0x3)

    if page is None:
        page = table_page(0x8000 + (a['pt_i'] ^ a['pd_i']))
    assert page & 0xFFF == 0, 'PFN de 4 KiB mal alinhado'
    phys.write_u64(a['pt_base'] + a['pt_i'] * 8, page | 0x3)
    return a['cr3'], page, va & 0xFFF


def build_aliased(phys, va1, page1, va2, page2):
    """Constroi UMA estrutura de tabelas com os dois VAs mapeados.

    Regra: UM page table (PT) por entrada de PD. Dois VAs que caem no mesmo
    `pd_i` compartilham o MESMO PT, com entradas em `pt_i` diferentes — que e
    exatamente o que um gerenciador de memoria faz.

    A versao anterior alocava um PT por par (pd_i, pt_i) e reescrevia a
    entrada do PD no segundo VA. Resultado: o segundo mapeamento apagava o
    primeiro, e o teste acusava bug no driver quando o bug era do teste.

    Retorna (cr3, [(va, fisico_esperado), ...]).
    """
    a1, a2 = table_addrs(va1), table_addrs(va2)
    if a1['pml4_i'] != a2['pml4_i']:
        raise WalkError('VAs em PML4 diferentes: nao compartilham estrutura')

    cr3 = a1['cr3']

    pdpt_base = table_page(0x100 + a1['pdpt_i'])
    phys.write_u64(cr3 + a1['pml4_i'] * 8, pdpt_base | 0x3)

    pd_of_pdpt = {}           # pdpt_i -> base do PD (um PD por entrada de PDPT)
    pt_of_pd   = {}           # pd_i   -> base do PT (um PT por entrada de PD)
    next_pd, next_pt = 0x400, 0x1000

    for a, va, page in ((a1, va1, page1), (a2, va2, page2)):
        pdpti, pdi = a['pdpt_i'], a['pd_i']

        # uma entrada de PDPT por pdpt_i, apontando pro SEU PD
        if pdpti not in pd_of_pdpt:
            pd_of_pdpt[pdpti] = table_page(next_pd)
            next_pd += 1
            phys.write_u64(pdpt_base + pdpti * 8, pd_of_pdpt[pdpti] | 0x3)

        # uma entrada de PD por pd_i, apontando pro SEU PT
        if pdi not in pt_of_pd:
            pt_of_pd[pdi] = table_page(next_pt)
            next_pt += 1
            phys.write_u64(pd_of_pdpt[pdpti] + pdi * 8, pt_of_pd[pdi] | 0x3)

        # entrada do PT — varios VAs no mesmo PT, indices diferentes
        phys.write_u64(pt_of_pd[pdi] + a['pt_i'] * 8, page | 0x3)

    return cr3, [(va1, page1 + (va1 & 0xFFF)),
                 (va2, page2 + (va2 & 0xFFF))]


def clear_present(phys, va, level):
    """Monta a cadeia completa e limpa o bit PRESENT no nivel pedido."""
    cr3, _, _ = build_tables(phys, va, level='pt')
    a = table_addrs(va)
    addr = {
        'pml4': cr3 + a['pml4_i'] * 8,
        'pdpt': a['pdpt_base'] + a['pdpt_i'] * 8,
        'pd':   a['pd_base'] + a['pd_i'] * 8,
        'pt':   a['pt_base'] + a['pt_i'] * 8,
    }[level]
    val = phys.read_u64(addr)
    assert val is not None, f'tabela {level} nao montada em 0x{addr:X}'
    phys.write_u64(addr, val & ~0x1)
    return cr3, 0, 0


# ---------------------------------------------------------------------------
# Casos
# ---------------------------------------------------------------------------

WALK_CASES = [
    ('4 KiB  VA baixo        0x0000000000401ABC', 0x0000000000401ABC, 'pt',   table_page(0x8001)),
    ('4 KiB  VA do jogo      0x00007FF6AC8F3ABC', 0x00007FF6AC8F3ABC, 'pt',   table_page(0x8002)),
    ('4 KiB  VA kernel       0xFFFFF80012345ABC', 0xFFFFF80012345ABC, 'pt',   table_page(0x9000)),
    ('4 KiB  PT adjacente    0x00007FF6AC8F4000', 0x00007FF6AC8F4000, 'pt',   table_page(0x9001)),
    ('2 MiB  grande no PD    0x00007FF6AC8F3ABC', 0x00007FF6AC8F3ABC, 'pd',   None),
    ('1 GiB  grande no PDPT  0x00007FF600001000', 0x00007FF600001000, 'pdpt', None),
]

ALIAS_CASES = [
    ('mesmo PD, PT diferente',   0x00007FF6AC8F3000, 0x00007FF6AC8F4000),
    ('mesmo PDPT, PD diferente', 0x00007FF6AC8F0000, 0x00007FF6AD000000),
    ('mesmo PML4, PDPT igual',   0x00007FF6AC8F0000, 0x00007FF680000000),
]

def zero_pfn(p, va, level, page=None):
    """Entrada presente com o PFN zerado — o caso que derrubou a maquina.

    Uma entrada com PRESENT=1 e PFN=0 e "valida" pelo formato, mas mapeia o
    endereco fisico 0. Foi assim que o walk devolveu lixo para o
    MmMapIoSpaceEx e o Windows bugcheckou com 0x50.
    """
    a = table_addrs(va)
    # monta a cadeia inteira primeiro, para a entrada existir
    build_tables(p, va, level='pt', page=page)

    addr = {
        'pml4': a['cr3'] + a['pml4_i'] * 8,
        'pdpt': a['pdpt_base'] + a['pdpt_i'] * 8,
        'pd':   a['pd_base'] + a['pd_i'] * 8,
        'pt':   a['pt_base'] + a['pt_i'] * 8,
    }[level]
    val = p.read_u64(addr)
    assert val is not None, f'tabela {level} nao montada em 0x{addr:X}'
    # mantem PRESENT (e PS, onde houver), zera o PFN
    p.write_u64(addr, val & 0xFFF)
    return a['cr3'], 0, 0


NEG_CASES = [
    ('PML4 sem PRESENT',                lambda p, va: clear_present(p, va, 'pml4')),
    ('PDPT sem PRESENT',                lambda p, va: clear_present(p, va, 'pdpt')),
    ('PD sem PRESENT',                  lambda p, va: clear_present(p, va, 'pd')),
    ('PT sem PRESENT',                  lambda p, va: clear_present(p, va, 'pt')),
    ('CR3 apontando para o vazio',      lambda p, va: (0xDEAD_0000, 0, 0)),
    # -- validacoes de PFN, adicionadas depois do BSOD 0x50 -----------------
    ('CR3 com PFN zerado',              lambda p, va: (0x000, 0, 0)),
    ('PDPT presente com PFN zerado',    lambda p, va: zero_pfn(p, va, 'pdpt')),
    ('PD presente com PFN zerado',      lambda p, va: zero_pfn(p, va, 'pd')),
    ('PT presente com PFN zerado',      lambda p, va: zero_pfn(p, va, 'pt')),
]

# VA usado nos testes de discriminacao
DISC_VA = 0x00007FF6AC8F3ABC


def check_present_bit(phys, base_va, page):
    """Ida: entrada com PRESENT=1 mas bit WRITE=0 (0x5) tem que resolver.

    Se o driver testar errado o PRESENT (ex.: `entry & 2`), esta entrada e
    recusada — e o teste acusa.
    """
    cr3, exp_base, exp_off = build_tables(phys, base_va, level='pt', page=page)
    a = table_addrs(base_va)
    pt_entry_addr = a['pt_base'] + a['pt_i'] * 8
    cur = phys.read_u64(pt_entry_addr)
    phys.write_u64(pt_entry_addr, (cur & ~0x3) | 0x1)   # PRESENT=1, WRITE=0
    return cr3, exp_base, exp_off


def check_index_per_level(phys, base_va, page):
    """Discriminacao: mexe SO na entrada de PDPT, um nivel acima.

    Se o walk aplicar a mascara do nivel errado, esse caso e o do PT param de
    ser lidos no endereco certo e o resultado muda. Se o walk estiver
    correto, mexer no PDPT do VA usado nao afeta em nada.
    """
    cr3, exp_base, exp_off = build_tables(phys, base_va, level='pt', page=page)
    a = table_addrs(base_va)
    other_va = (base_va & ~(0x1FF << 30)) | ((a['pdpt_i'] ^ 0x155) << 30)
    b = table_addrs(other_va)
    phys.write_u64(a['pdpt_base'] + b['pdpt_i'] * 8, b['pd_base'] | 0x3)
    return cr3, exp_base, exp_off


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default=None)
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    src = args.src or os.path.join(root, 'km_driver_source.c')
    if not os.path.isfile(src):
        print(f'[err] nao encontrei {src}')
        return 1

    c = parse_driver(src)

    print('=' * 78)
    print('PAGE-TABLE WALK CHECK â€” aritmetica x64 contra tabelas sinteticas')
    print('=' * 78)
    print(f'  fonte: {src}')
    print()
    print('  constantes lidas do driver:')
    for k in ('PTE_PRESENT', 'PTE_PFN_MASK', 'PTE_PS'):
        v = c[k]
        print(f'    {k:<14} = {v if v is None else hex(v)}')
    sh = {k: v for k, v in sorted(c['shifts'].items(), reverse=True)}
    print('    shifts indices = ' + ', '.join(f'{k}->{hex(v)}' for k, v in sh.items()))

    problems = []

    if c['PTE_PRESENT'] is None: problems.append('KM_PTE_PRESENT ausente no .c')
    if c['PTE_PFN_MASK'] is None: problems.append('KM_PTE_PFN_MASK ausente no .c')
    if c['PTE_PS'] is None:      problems.append('KM_PTE_PS ausente no .c')
    if len(c['shifts']) != 4:
        problems.append(f'esperava 4 shifts de indice, achei {len(c["shifts"])}')
    if problems:
        print()
        for p in problems:
            print(f'  [err]  {p}')
        return 1

    # -- sanidade -----------------------------------------------------------
    print()
    print('-- SANIDADE DAS CONSTANTES ' + '-' * 51)
    checks = [
        ('PTE_PRESENT == 1',           c['PTE_PRESENT'] == 0x1),
        ('PTE_PS == 0x80',             c['PTE_PS'] == 0x80),
        ('PFN mask alinha em 4 KiB',   c['PTE_PFN_MASK'] & 0xFFF == 0),
        ('PFN mask cobre bits 12..51', (c['PTE_PFN_MASK'] >> 12) & ((1 << 40) - 1) == (1 << 40) - 1),
        ('shifts == {39,30,21,12}',    set(c['shifts']) == {39, 30, 21, 12}),
        ('mascara de indice 0x1FF',    all(v == 0x1FF for v in c['shifts'].values())),
    ]
    ok = 0
    for label, good in checks:
        print(f'  [{"ok " if good else "ERR"}] {label}')
        if good:
            ok += 1
        else:
            problems.append(label)
    print(f'  {ok}/{len(checks)}')

    # -- walk ---------------------------------------------------------------
    print()
    print('-- WALK EM TABELAS SINTETICAS ' + '-' * 48)
    passed = 0
    for label, va, level, page in WALK_CASES:
        phys = PhysMem()
        try:
            cr3, exp_base, exp_off = build_tables(phys, va, level=level, page=page)
            base, off = walk(phys, cr3, va, c)

            if base == 0:
                print(f'  [ERR] {label}')
                print('         base=0 â€” PFN de teste caiu fora da mascara, '
                      'o caso nao testa nada')
                problems.append(f'{label}: PFN fora da mascara')
                continue

            good = (base == exp_base and off == exp_off)
            print(f'  [{"ok " if good else "ERR"}] {label}')
            if not good:
                print(f'         esperado base=0x{exp_base:X} off=0x{exp_off:X}')
                print(f'         obtido   base=0x{base:X} off=0x{off:X}')
                problems.append(label)
            else:
                print(f'         base=0x{base:X}  off=0x{off:X}  '
                      f'-> fisico 0x{base + off:X}')
                passed += 1
        except (WalkError, AssertionError) as e:
            print(f'  [ERR] {label}: {e}')
            problems.append(f'{label}: {e}')

    # -- aliasing -----------------------------------------------------------
    print()
    print('-- ALIASING DE INDICE ' + '-' * 56)
    alias_ok = 0
    for label, va1, va2 in ALIAS_CASES:
        phys = PhysMem()
        try:
            p1, p2 = table_page(0xB000), table_page(0xC000)
            cr3, want = build_aliased(phys, va1, p1, va2, p2)

            got = []
            for va, _ in want:
                b, o = walk(phys, cr3, va, c)
                got.append(b + o)

            good = (got == [w for _, w in want]) and (got[0] != got[1])
            print(f'  [{"ok " if good else "ERR"}] {label}')
            for (va, w), g in zip(want, got):
                mark = '  ' if g == w else '<-'
                print(f'         va=0x{va:X}  esperado 0x{w:X}  obtido 0x{g:X} {mark}')
            if good:
                alias_ok += 1
            else:
                problems.append(f'aliasing: {label}')
        except (WalkError, AssertionError) as e:
            print(f'  [ERR] {label}: {e}')
            problems.append(f'aliasing {label}: {e}')

    # -- negativos ----------------------------------------------------------
    print()
    print('-- CASOS NEGATIVOS (o walk tem que recusar) ' + '-' * 33)
    neg_ok = 0
    va = 0x00007FF6AC8F3000
    for label, setup in NEG_CASES:
        phys = PhysMem()
        try:
            cr3, _, _ = setup(phys, va)
            walk(phys, cr3, va, c)
            print(f'  [ERR] {label}: o walk ACEITOU, devia recusar')
            problems.append(label)
        except (WalkError, AssertionError) as e:
            print(f'  [ok ] {label}  ({e})')
            neg_ok += 1

    # -- discriminacao ------------------------------------------------------
    # Cada caso abaixo existe para PEGAR um bug especifico do walk. Nao e
    # decoracao: sem eles, uma mudanca errada no driver passaria batido.
    print()
    print('-- DISCRIMINACAO (cada caso pega um bug especifico) ' + '-' * 26)

    discrim = [
        ('PRESENT=0x5 (sem WRITE) tem que resolver',
         check_present_bit, 0x00007FF6AC8F3ABC, table_page(0xD000)),
        ('mexer no PDPT de OUTRO VA nao pode afetar este',
         check_index_per_level, DISC_VA, table_page(0xD001)),
    ]

    disc_ok = 0
    for label, setup, va, page in discrim:
        phys = PhysMem()
        try:
            cr3, eb, eo = setup(phys, va, page)
            b, o = walk(phys, cr3, va, c)
            good = (b == eb and o == eo)
            print(f'  [{"ok " if good else "ERR"}] {label}')
            if good:
                print(f'         base=0x{b:X} off=0x{o:X}')
                disc_ok += 1
            else:
                print(f'         esperado base=0x{eb:X} off=0x{eo:X}')
                print(f'         obtido   base=0x{b:X} off=0x{o:X}')
                problems.append(label)
        except (WalkError, AssertionError) as e:
            print(f'  [ERR] {label}: {e}')
            problems.append(f'{label}: {e}')

    # -- selftest do modelo -------------------------------------------------
    # Muta a CONFIGURACAO do walk e exige que os casos de walk mudem de
    # resultado. E o que prova que os valores lidos do .c sao de fato usados
    # e que o teste discrimina — sem isso o script so sabe dizer "ok".
    print()
    print('-- SELFTEST DO MODELO (mutation) ' + '-' * 45)

    def run_walk_cases(cfg):
        okn = badn = 0
        for _, va, level, page in WALK_CASES:
            ph = PhysMem()
            try:
                cr3, eb, eo = build_tables(ph, va, level=level, page=page)
                b, o = walk(ph, cr3, va, cfg)
                if b == eb and o == eo:
                    okn += 1
                else:
                    badn += 1
            except WalkError:
                badn += 1
        return okn, badn

    base_ok, base_bad = run_walk_cases(c)
    print(f'  baseline: {base_ok} acertos, {base_bad} erros')

    s = dict(c['shifts'])
    # Mascara do PML4 com 8 bits de verdade (antes essa mutacao nao fazia
    # nada: o dict tinha as mesmas chaves, so reordenadas).
    s_narrow = dict(s)
    s_narrow[39] = 0x0FF

    mutations = [
        ('mascara de PFN truncada',
         {**c, 'PTE_PFN_MASK': 0xFFFFF000}),
        ('PS ignorado',
         {**c, 'PTE_PS': 0}),
        ('mascara do PML4 com 8 bits',
         {**c, 'shifts': s_narrow}),
        ('mascara do PD com buraco (0x1FE)',
         {**c, 'shifts': {**s, 21: 0x1FE}}),
    ]

    mut_ok = 0
    for label, bad in mutations:
        okn, badn = run_walk_cases(bad)
        detected = (badn > base_bad) or (okn < base_ok)
        print(f'  [{"ok " if detected else "ERR"}] {label:<38} '
              f'({okn} ok / {badn} erro)'
              + ('' if detected else '   <-- passou batido!'))
        if detected:
            mut_ok += 1
        else:
            problems.append(f'selftest: {label}')

    # -- resultado ----------------------------------------------------------
    print()
    print('=' * 78)
    if problems:
        print(f'  RESULTADO: {len(problems)} problema(s)')
        for p in problems:
            print(f'    - {p}')
    else:
        print('  RESULTADO: aritmetica OK')
        print(f'    sanidade      {ok}/{len(checks)}')
        print(f'    walk          {passed}/{len(WALK_CASES)}')
        print(f'    aliasing      {alias_ok}/{len(ALIAS_CASES)}')
        print(f'    negativos     {neg_ok}/{len(NEG_CASES)}')
        print(f'    discriminacao {disc_ok}/{len(discrim)}')
        print(f'    mutation      {mut_ok}/{len(mutations)}')
    print('=' * 78)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
