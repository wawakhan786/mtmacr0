#!/usr/bin/env python3
"""
offset_audit.py â€” compara os offsets do codigo (Globals.h) com um dump de
referencia e aponta onde os dois discordam.

Por que isso importa: um dump bruto de offsets e o que se tem depois de
reverter um build do jogo. O `Globals.h` e o que o cheat usa de verdade. Se
os dois vierem de builds diferentes, metade do codigo esta lendo lixo â€” e o
sintoma nao e crash, e ESP que "quase funciona".

O script NAO sabe qual dos dois esta certo. Ele mostra a divergencia para
voce decidir com o jogo aberto.

## Como os nomes sao casados

O dump e o `Globals.h` nao usam os mesmos nomes (`DecryptNameIndexXorKey1`
vs `NameIndexXor1`, `Health_keys0` vs `HealthXorKey0`). O casamento e feito
em tres estagios, do mais seguro para o mais frouxo:

  1. normalizacao â€” minusculas, sem `_`/`-`/espaco. Pega ~100 dos 182.
  2. HINTS        â€” mapa explicito para os que a normalizacao nao pega.
  3. fuzzy        â€” >= FUZZY_MIN, e so para nomes com 8+ caracteres.

Tudo que nao casar em nenhum estagio entra em "sem casamento", **sem chute**.
Um mapeamento errado gera uma divergencia falsa, que e pior que dizer
"nao sei".

Uso:
    python offset_audit.py
    python offset_audit.py --dump caminho\\do\\dump.txt
    python offset_audit.py --verbose
"""

import argparse
import os
import re
import sys
from difflib import SequenceMatcher

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

# ---------------------------------------------------------------------------
# Nomes que so um mapa explicito resolve
# ---------------------------------------------------------------------------
HINTS = {
    # decrypt de indice de nome.
    #
    # Os campos do Globals.h dizem o que FAZEM. Foi a nomenclatura antiga —
    # `NameIndexRotr` recebendo o `Ror` e `NameIndexShlMask` recebendo um
    # `0xFFFF0000` que NAO existe na formula — que causou o bug do
    # `[STATE] Actors=0`. Na 2609.1.3.5:
    #
    #   DecryptNameIndexRor     = 0x1        -> NameIndexRor       (NAO usado)
    #   DecryptNameIndexXorKey1 = 0xB753B1E8 -> NameIndexXor1      (XOR inicial)
    #   DecryptNameIndexXorKey2 = 0xCAFC916  -> NameIndexXor2      (XOR final)
    #   DecryptNameIndexXorKey3 = 0x70000    -> NameIndexMaskRight (mask da direita)
    #   DecryptNameIndexRval    = 0x1D       -> NameIndexRotr      (rotacao)
    #   DecryptNameIndexSval    = 0xD        -> NameIndexShr       (shift direita)
    #   DecryptNameIndexDval    = 0x13       -> NameIndexShl       (shift esquerda)
    #
    # ATENCAO: `NameIndexShlMask` (0xFFFF0000) foi REMOVIDO do Globals.h — ele
    # nao existe nesta formula. O hint dele saiu junto.
    'DecryptNameIndexXorKey1': 'NameIndexXor1',
    'DecryptNameIndexXorKey2': 'NameIndexXor2',
    'DecryptNameIndexXorKey3': 'NameIndexMaskRight',
    'DecryptNameIndexRor':     'NameIndexRor',
    'DecryptNameIndexRval':    'NameIndexRotr',
    'DecryptNameIndexSval':    'NameIndexShr',
    'DecryptNameIndexDval':    'NameIndexShl',
    # base do modulo
    'GNamesPtr':       'GNames_offset',
    # a transcricao de 27/10 tinha batizado o GNames de "GNamesArray"; a lista
    # crua do Rei (28/10) usa GNames direto — o hint fica por seguranca
    'GNamesArray':     'GNames',
    'ChunkSize':       'ElementsPerChunk',
    'XenuineDecrypt':  'XenuineDecrypt',
    # chaves de health
    **{f'Health_keys{i}': f'HealthXorKey{i}' for i in range(16)},
    # objeto
    'ObjID': 'ObjectID',
    # o dump usa plural, o codigo singular
    'Offsets': 'Offset',
    # a lista usa minusculo
    'ping': 'Ping',
    # nomes diferentes para o mesmo conceito (conferidos a mao na 2609.1.3.5)
    'FloatingVehicleCommonComponent': 'FloatingComponent',
    'ItemsArray': 'EquipmentItems',
    # NOTA: `BulletTable` (0xA18) esta' na lista da build e o Globals.h NAO
    # tem campo proprio para ele. Candidato natural seria `TrajectoryConfig`,
    # mas os dois aparecem com valores DIFERENTES na mesma lista (0xA18 e
    # 0x100), entao nao sao o mesmo campo. Fica em "sem casamento" de
    # proposito, em vez de virar um par inventado.
}

# Pares que parecem iguais mas NAO sao o mesmo campo. O fuzzy nao pode casar.
BLOCKED = {
    # Nada aqui por enquanto. Servia para impedir o fuzzy de casar
    # `Durabilitymax` com `Durability` (dois campos distintos, 0x1E0 vs 0x1E4),
    # mas agora os dois existem no Globals.h e o casamento por nome resolve.
}

FUZZY_MIN = 0.95
FUZZY_MIN_LEN = 8


# ---------------------------------------------------------------------------
# Leitura
# ---------------------------------------------------------------------------

def parse_dump(path):
    """constexpr uint64_t NAME = 0xVAL;  (ou uint32_t)"""
    out = {}
    text = open(path, encoding='utf-8', errors='replace').read()
    for m in re.finditer(r'constexpr\s+uint(?:32|64)_t\s+(\w+)\s*=\s*'
                         r'(0x[0-9A-Fa-f]+|\d+)\s*;', text):
        v = m.group(2)
        out[m.group(1)] = int(v, 16) if v.lower().startswith('0x') else int(v)
    return out


def parse_globals(path):
    """struct OFFSET { uint32_t NAME = 0xVAL; ... } inline SDK;"""
    out = {}
    text = open(path, encoding='utf-8', errors='replace').read()
    m = re.search(r'struct\s+OFFSET\s*\{(.*?)\}\s*inline\s+SDK\s*;', text, re.S)
    body = m.group(1) if m else text
    body = re.sub(r'//[^\n]*', '', body)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    for mm in re.finditer(r'uint(?:32|64)_t\s+(\w+)\s*=\s*'
                          r'(0x[0-9A-Fa-f]+|\d+)\s*;', body):
        v = mm.group(2)
        out[mm.group(1)] = int(v, 16) if v.lower().startswith('0x') else int(v)
    return out


# ---------------------------------------------------------------------------
# Casamento de nomes
# ---------------------------------------------------------------------------

def norm(s):
    return re.sub(r'[^a-z0-9]', '', s.lower())


def match_names(dump_names, glob_names):
    """Retorna (mapa dump->globals, nao_casados, origem, colisoes)."""
    gnorm = {}
    for g in glob_names:
        gnorm.setdefault(norm(g), g)

    mapping, origin, unmatched = {}, {}, []

    for d in sorted(dump_names):
        # 1. normalizacao
        g = gnorm.get(norm(d))
        if g and (d, g) not in BLOCKED:
            mapping[d], origin[d] = g, 'exato'
            continue

        # 2. hint
        g = HINTS.get(d)
        if g and g in glob_names:
            mapping[d], origin[d] = g, 'hint'
            continue

        # 3. fuzzy conservador
        if len(d) >= FUZZY_MIN_LEN:
            best, score = None, 0.0
            for cand in glob_names:
                if (d, cand) in BLOCKED:
                    continue
                r = SequenceMatcher(None, norm(d), norm(cand)).ratio()
                if r > score:
                    best, score = cand, r
            if best and score >= FUZZY_MIN:
                mapping[d], origin[d] = best, f'fuzzy({score:.2f})'
                continue

        unmatched.append(d)

    collisions = {}
    for d, g in mapping.items():
        collisions.setdefault(g, []).append(d)
    collisions = {g: ds for g, ds in collisions.items() if len(ds) > 1}

    return mapping, unmatched, origin, collisions


# ---------------------------------------------------------------------------
# Relatorio
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))
    ap.add_argument('--dump', default=None,
                    help='dump de offsets. Padrao: PubgExt\\offsetdump.txt e, '
                         'se nao existir, _legacy\\offsetdump_OLD_build.txt')
    ap.add_argument('--globals', default=None)
    ap.add_argument('--verbose', action='store_true',
                    help='mostra o mapa de nomes e a origem de cada casamento')
    args = ap.parse_args()

    candidates = [
        args.dump,
        os.path.join(args.root, 'PubgExt', 'offsetdump.txt'),
        os.path.join(args.root, '_legacy', 'offsetdump_OLD_build.txt'),
    ]
    dump_p = next((p for p in candidates if p and os.path.isfile(p)), None)
    glob_p = args.globals or os.path.join(args.root, 'PubgExt', 'Globals.h')

    if dump_p is None:
        print('[err] nenhum dump de offsets encontrado. Procurei em:')
        for p in candidates[1:]:
            print(f'        {p}')
        print('      use --dump <arquivo> para apontar um.')
        return 1
    if not os.path.isfile(glob_p):
        print(f'[err] nao encontrei {glob_p}')
        return 1

    legacy = '_legacy' in dump_p.replace('/', '\\')

    dump = parse_dump(dump_p)
    glob = parse_globals(glob_p)
    if not dump:
        print(f'[err] {dump_p} nao tem nenhum `constexpr uintN_t NOME = VALOR;`')
        return 1

    mapping, unmatched, origin, collisions = match_names(dump.keys(), glob)

    print('=' * 84)
    print('OFFSET AUDIT')
    print('=' * 84)
    print(f'  dump   {dump_p}   ({len(dump)} simbolos)')
    print(f'  codigo {glob_p}   ({len(glob)} campos)')
    print()

    if legacy:
        print('  ATENCAO: este dump esta em _legacy â€” e de um build ANTERIOR ao')
        print('  que o codigo usa hoje (TslGame 2609.1.2.11). A divergencia')
        print('  abaixo e ESPERADA. Use --dump para apontar um dump atual.')
        print()

    by_origin = {}
    for o in origin.values():
        k = o.split('(')[0]
        by_origin[k] = by_origin.get(k, 0) + 1
    print(f'  nomes casados   : {len(mapping)}/{len(dump)}  '
          f'({", ".join(f"{k}={v}" for k, v in sorted(by_origin.items()))})')
    print(f'  nomes sem match : {len(unmatched)}')

    if collisions:
        print(f'\n  [!] {len(collisions)} campo(s) do codigo recebendo mais de um '
              f'nome do dump:')
        for g, ds in sorted(collisions.items()):
            print(f'      {g:<28} <- {", ".join(ds)}')
        print('      (pode ser legitimo â€” Ror e Rval sao o mesmo campo â€” mas')
        print('       confira antes de confiar no delta)')

    if args.verbose:
        print('\n-- MAPA DE NOMES ' + '-' * 67)
        for d in sorted(mapping):
            print(f'  {d:<30} -> {mapping[d]:<30} [{origin[d]}]')

    if unmatched:
        print('\n-- SEM CASAMENTO ({}) '.format(len(unmatched)) + '-' * 60)
        for d in unmatched:
            print(f'  0x{dump[d]:<12X} {d}')
        print('  Nao entram na comparacao â€” sem chute.')

    # -- comparacao --------------------------------------------------------
    same, diff = [], []
    for d, g in mapping.items():
        gv = glob.get(g)
        if gv is None:
            continue
        dv = dump[d]
        (same if dv == gv else diff).append((d, g, dv, gv))

    only_glob = [g for g in sorted(glob) if g not in set(mapping.values())]

    if same:
        print(f'\n-- IGUAIS ({len(same)}) ' + '-' * 62)
        for d, g, v, _ in same:
            label = d if d == g else f'{d} == {g}'
            print(f'  0x{v:<12X} {label}')

    if diff:
        print(f'\n-- DIVERGENTES ({len(diff)})   <-- olhe estes primeiro ' + '-' * 26)
        print(f'  {"nome no dump":<30} {"no codigo":<26} {"dump":<12} '
              f'{"codigo":<12} delta')
        for d, g, dv, gv in sorted(diff, key=lambda t: -abs(t[3] - t[2])):
            delta = gv - dv
            sign = f'{delta:+#x}' if delta else '0'
            print(f'  {d:<30} {g:<26} 0x{dv:<10X} 0x{gv:<10X} {sign}')

    if only_glob:
        print(f'\n-- SO NO CODIGO ({len(only_glob)}) â€” sem referencia no dump')
        for g in only_glob:
            print(f'  0x{glob[g]:<12X} {g}')

    # -- resumo ------------------------------------------------------------
    total = len(same) + len(diff)
    print('\n' + '=' * 84)
    if total:
        print(f'  {len(same)}/{total} dos nomes casados batem '
              f'({100.0 * len(same) / total:.0f}%)')
    print(f'  {len(diff)} divergencia(s), {len(unmatched)} sem casamento, '
          f'{len(only_glob)} so no codigo')
    if diff:
        print('  As divergencias sozinhas nao dizem quem esta certo â€” so que')
        print('  os dois arquivos nao sao do mesmo build.')
    print('=' * 84)
    return 0 if not diff else 1


if __name__ == '__main__':
    sys.exit(main())
