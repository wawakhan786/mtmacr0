#!/usr/bin/env python3
"""
pubg_decrypt.py — implementacao de referencia dos algoritmos de decrypt do
PUBG (build TslGame 2609.1.3.5) em Python puro.

Por que isso existe: as funcoes equivalentes em C++ (Engine::xe_decrypt,
Engine::DecryptCIndex, Engine::DecryptHealth) nao podem ser testadas sem o
jogo aberto. Aqui elas sao reimplementadas a partir do SDK, com a assinatura
dos parametros lida do PROPRIO Globals.h.

O valor pratico: quando o jogo atualizar, os parametros de decrypt mudam.
Este modulo le os novos do Globals.h e tudo que depende dele (o dumper, os
scripts de offset) continua funcionando sem alteracao.

Uso como modulo:
    from pubg_decrypt import Sdk, decrypt_cindex, decrypt_health
    sdk = Sdk.from_globals(r'...\\PubgExt\\Globals.h')
    idx = decrypt_cindex(0x12345678, sdk)

Uso como script:
    python pubg_decrypt.py --selftest
    python pubg_decrypt.py --globals <caminho> --dump
"""

import argparse
import ctypes
import os
import re
import sys

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

M32 = 0xFFFFFFFF


# ---------------------------------------------------------------------------
# SDK: le os parametros de decrypt do Globals.h
# ---------------------------------------------------------------------------

class Sdk:
    """Parametros de decrypt, lidos do Globals.h.

    Ler do header em vez de hardcodar aqui e o ponto: quando o build do jogo
    muda, so o Globals.h muda.
    """

    FIELDS = {
        # decrypt de indice de nome (FName).
        # Os nomes dizem o que o campo FAZ (ver Globals.h). O `NameIndexRor` e'
        # o unico registrado e NAO usado na formula desta build.
        'NameIndexXor1':      None,   # XOR inicial   <- DecryptNameIndexXorKey1
        'NameIndexXor2':      None,   # XOR final     <- DecryptNameIndexXorKey2
        'NameIndexShl':       None,   # shift ESQ  19 <- DecryptNameIndexDval
        'NameIndexShr':       None,   # shift DIR  13 <- DecryptNameIndexSval
        'NameIndexMaskRight': None,   # mask 0x70000  <- DecryptNameIndexXorKey3
        'NameIndexRotr':      None,   # rotr 29       <- DecryptNameIndexRval
        'NameIndexRor':       None,   # 0x1, NAO usado <- DecryptNameIndexRor
        # base do modulo (RVA) — util para o dumper
        'XenuineDecrypt':   None,
        'UWorld':           None,
        'GNames':           None,
        'ElementsPerChunk': None,
        # health
        'HeaFlag':          None,
        'Health1':          None,
        'Health2':          None,
        'Health3':          None,
        'Health4':          None,
        'Health5':          None,
        'Health6':          None,
    }

    def __init__(self, values):
        for k, v in values.items():
            setattr(self, k, v)

    def __repr__(self):
        return ('Sdk(' + ', '.join(
            f'{k}=0x{getattr(self, k):X}'
            for k in sorted(self.FIELDS) if getattr(self, k, None) is not None
        ) + ')')

    @classmethod
    def from_globals(cls, path):
        text = open(path, encoding='utf-8', errors='replace').read()
        # so o corpo do struct OFFSET
        m = re.search(r'struct\s+OFFSET\s*\{(.*?)\}\s*inline\s+SDK\s*;', text, re.S)
        body = m.group(1) if m else text
        body = re.sub(r'//[^\n]*', '', body)
        body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)

        values = {}
        for fm in re.finditer(r'uint(?:32|64)_t\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;', body):
            name, val = fm.group(1), fm.group(2)
            values[name] = int(val, 16) if val.lower().startswith('0x') else int(val)

        missing = [k for k in cls.FIELDS if k not in values]
        if missing:
            raise ValueError(f'Globals.h sem os campos: {", ".join(missing)}')

        # as chaves de health sao 16 campos separados
        keys = []
        for i in range(16):
            n = f'HealthXorKey{i}'
            if n not in values:
                raise ValueError(f'Globals.h sem {n}')
            keys.append(values[n] & M32)
        values['HealthKeys'] = keys

        return cls(values)


# ---------------------------------------------------------------------------
# Primitivas
# ---------------------------------------------------------------------------

def _rotr32(v, n):
    v &= M32
    n &= 31
    if n == 0:
        return v
    return ((v >> n) | (v << (32 - n))) & M32


def _rotl32(v, n):
    v &= M32
    n &= 31
    if n == 0:
        return v
    return ((v << n) | (v >> (32 - n))) & M32


# ---------------------------------------------------------------------------
# Decrypt de indice de nome (FName)
#
# Equivalente a Engine::DecryptCIndex. A formula DESTA build (2609.1.3.5) veio
# do dump fornecido pelo usuario:
#
#     DWORD DecryptCIndex(DWORD valor)
#     {
#         DWORD xored = valor ^ 0xB753B1E8u;
#         return ((xored << 19) | ((xored >> 13) & 0x70000u))
#                ^ _rotr(xored, 29) ^ 0x0CAFC916u;
#     }
#
# Mapa lista(dump) -> campo do Globals.h (os mesmos nomes usados aqui):
#
#     DecryptNameIndexXorKey1 -> NameIndexXor1       (XOR inicial)
#     DecryptNameIndexDval    -> NameIndexShl        (SHIFT ESQUERDA  0x13 = 19)
#     DecryptNameIndexSval    -> NameIndexShr        (SHIFT DIREITA   0xD  = 13)
#     DecryptNameIndexXorKey3 -> NameIndexMaskRight  (MASCARA do termo da direita)
#     DecryptNameIndexRval    -> NameIndexRotr       (ROTACAO 0x1D = 29)
#     DecryptNameIndexXorKey2 -> NameIndexXor2       (XOR final)
#     DecryptNameIndexRor     -> NameIndexRor        (NAO usado nesta build)
#
# OS TRES ERROS DA LEITURA ANTERIOR (todos invisiveis para o rank 32/32):
#   1. usava `Ror` (0x1) como rotacao e deduzia `shl = (48 - rotr) & 31 = 15`,
#      no lugar do `Dval` (0x13 = 19);
#   2. MASCARAVA o termo da esquerda com um `0xFFFF0000` (o campo
#      NameIndexShlMask, que NAO existe nesta formula e foi REMOVIDO do
#      Globals.h) em vez de OR'AR os dois termos;
#   3. ignorava o `>>` (Sval): o `(xored >> 13) & 0x70000` nao existia.
#   Resultado medido no jogo: indice de FName caia em memoria aleatoria, o nome
#   de classe saia como bytecode (`cls='WH...H‹ÚH...'`), o `IsPlayer` dava falso
#   para TODO ator e o ESP ficava vazio (`[STATE] Actors=0`).
# ---------------------------------------------------------------------------


def decrypt_cindex(value, sdk):
    """Fiel a Engine::DecryptCIndex do build 2609.1.3.5 (forma do dump)."""
    xored = (value ^ (sdk.NameIndexXor1 & M32)) & M32
    left  = (xored << (sdk.NameIndexShl & 31)) & M32
    right = (xored >> (sdk.NameIndexShr & 31)) & (sdk.NameIndexMaskRight & M32)
    return ((left | right)                        # OR, nao AND
            ^ _rotr32(xored, sdk.NameIndexRotr & 31)
            ^ (sdk.NameIndexXor2 & M32)) & M32


def decrypt_cindex_dump_literal(value):
    """A forma do dump escrita LITERALMENTE, com as constantes na mao.

    E' a referencia contra a qual o selftest compara `decrypt_cindex`: se um
    campo do Globals.h for trocado (Dval <-> Sval <-> Rval <-> Ror) ou se
    alguem reintroduzir o `0xFFFF0000`, esta funcao NAO muda e a comparacao
    acusa. Foi o erro que o rank 32/32 deixou passar.
    """
    xored = (value ^ 0xB753B1E8) & M32
    return (((xored << 19) | ((xored >> 13) & 0x70000))
            ^ _rotr32(xored, 29) ^ 0x0CAFC916) & M32


def decrypt_cindex_leitura_antiga(value):
    """CONTROLE NEGATIVO: a leitura ERRADA que estava no codigo.

        ((xored << 15) & 0xFFFF0000) ^ _rotr(xored, 1) ^ 0x0CAFC916

    As constantes vao na mao de proposito: o `0xFFFF0000` vinha do campo
    NameIndexShlMask, removido porque nao existe na formula. Esta funcao existe
    para provar que o autoteste PEGA esse erro: ela tinha rank 32/32 e
    ida-e-volta 100% (era o "ok" falso do boot) e mesmo assim estava errada.
    """
    xored = (value ^ 0xB753B1E8) & M32
    return (((xored << 15) & M32) & 0xFFFF0000
            ^ _rotr32(xored, 1)
            ^ 0x0CAFC916) & M32


def decrypt_cindex_shape(x, sdk):
    """A parte linear (shift/rotate/OR), sem os XOR de chave.

    Sendo so shift, rotate e OR, isto e uma transformacao LINEAR sobre GF(2).
    E dai que vem a inversa exata: monta-se a matriz 32x32 e inverte-se.

    Tem que espelhar EXATAMENTE a forma de `decrypt_cindex` — senao a inversa
    (e a prova de bijecao) valem para uma funcao que nao e' a do cheat.
    """
    shl  = sdk.NameIndexShl & 31
    shr  = sdk.NameIndexShr & 31
    mask = sdk.NameIndexMaskRight & M32
    merged = (((x << shl) & M32) | ((x >> shr) & mask)) & M32
    return (merged ^ _rotr32(x, sdk.NameIndexRotr & 31)) & M32





def _gf2_invert_matrix(cols):
    """Inverte uma matriz 32x32 sobre GF(2).

    `cols[b]` e a imagem do vetor unitario e_b, ou seja a coluna b.
    Gauss-Jordan sobre os bits, com a identidade ao lado.

    Retorna a lista de colunas da inversa, ou None se for singular.
    """
    # linha i = bits das colunas; trabalhamos com (matriz | identidade)
    mat = [0] * 32          # mat[i] = linha i da matriz original, como bits
    for b in range(32):
        col = cols[b] & M32
        for i in range(32):
            if (col >> i) & 1:
                mat[i] |= (1 << b)

    inv = [1 << i for i in range(32)]   # identidade, linha i = e_i

    for i in range(32):
        # pivo
        piv = -1
        for r in range(i, 32):
            if (mat[r] >> i) & 1:
                piv = r
                break
        if piv < 0:
            return None
        mat[i], mat[piv] = mat[piv], mat[i]
        inv[i], inv[piv] = inv[piv], inv[i]

        for r in range(32):
            if r != i and ((mat[r] >> i) & 1):
                mat[r] ^= mat[i]
                inv[r] ^= inv[i]

    # inv[i] e a linha i da inversa; converte para colunas
    out = [0] * 32
    for i in range(32):
        for b in range(32):
            if (inv[i] >> b) & 1:
                out[b] |= (1 << i)
    # sanity: out deve ser a inversa de verdade
    for b in range(32):
        img = apply_cols(cols, 1 << b)
        if apply_cols(out, img) != (1 << b):
            return None
    return out


def apply_cols(cols, x):
    """Aplica a transformacao linear definida por `cols` a x."""
    out = 0
    for b in range(32):
        if (x >> b) & 1:
            out ^= cols[b]
    return out & M32


_CACHE = {}


def _inverse_cols(sdk):
    key = (sdk.NameIndexShl, sdk.NameIndexShr, sdk.NameIndexMaskRight,
           sdk.NameIndexRotr)
    if key not in _CACHE:
        cols = [decrypt_cindex_shape(1 << b, sdk) for b in range(32)]
        _CACHE[key] = _gf2_invert_matrix(cols)
    return _CACHE[key]


def decrypt_cindex_inverse(value, sdk):
    """Inversa EXATA de decrypt_cindex.

    O decrypt e:

        xored = x ^ k1
        y     = shape(xored) ^ k2        onde shape = shifted ^ rotr

    Entao k2 sai primeiro e k1 sai por ultimo — a transformacao linear age
    sobre `xored`, NAO sobre `x`:

        shape(xored) = y ^ k2
        xored        = shape^-1(y ^ k2)
        x            = xored ^ k1

    (A versao anterior XORava k1 no lugar errado e devolvia exatamente k1
    como resposta para x = 0. Ficou obvio no log.)

    Retorna None se a transformacao for singular — o que significaria que o
    decrypt nao e bijetivo e os indices de FName colidiriam.
    """
    inv = _inverse_cols(sdk)
    if inv is None:
        return None
    target = (value ^ (sdk.NameIndexXor2 & M32)) & M32    # tira k2
    xored = apply_cols(inv, target)                       # inverte shape
    return (xored ^ (sdk.NameIndexXor1 & M32)) & M32      # tira k1


def shape_rank(sdk):
    """Quantos bits independentes a parte linear preserva (rank sobre GF(2))."""
    cols = [decrypt_cindex_shape(1 << b, sdk) for b in range(32)]
    mat = [0] * 32
    for b in range(32):
        for i in range(32):
            if (cols[b] >> i) & 1:
                mat[i] |= (1 << b)
    rank = 0
    for i in range(32):
        piv = -1
        for r in range(rank, 32):
            if (mat[r] >> i) & 1:
                piv = r
                break
        if piv < 0:
            continue
        mat[rank], mat[piv] = mat[piv], mat[rank]
        for r in range(32):
            if r != rank and ((mat[r] >> i) & 1):
                mat[r] ^= mat[rank]
        rank += 1
    return rank


# ---------------------------------------------------------------------------
# Health
#
# Equivalente a Engine::HealthDecipher + Engine::DecryptHealth.
# ---------------------------------------------------------------------------

def health_decipher(buf, seed, sdk):
    """XOR de 4 bytes com a tabela de chaves, indexada por (i + seed) & 0x3F."""
    keys = sdk.HealthKeys
    key_bytes = b''.join((k & M32).to_bytes(4, 'little') for k in keys)
    out = bytearray(buf[:4])
    for i in range(4):
        v1 = (i + seed) & 0x3F
        out[i] ^= key_bytes[v1]
    return bytes(out)


def decrypt_health(buffer, sdk):
    """Le o health de um bloco de memoria do ator.

    `buffer` e o bloco cru. Os offsets do SDK sao relativos a HeaFlag, entao
    tudo aqui e ajustado por essa base â€” igual ao codigo C++.
    """
    base = sdk.HeaFlag
    o_flag  = 0
    o_h1    = sdk.Health1 - base
    o_h2    = sdk.Health2 - base
    o_h3    = sdk.Health3 - base
    o_h4    = sdk.Health4 - base
    o_h5    = sdk.Health5 - base
    o_h6    = sdk.Health6 - base

    need = max(o_flag, o_h1, o_h2, o_h3, o_h4, o_h5, o_h6) + 8 + 0x100
    if len(buffer) < need:
        raise ValueError(f'buffer curto: {len(buffer)} < {need}')

    # o C++ faz: if (buffer[HeaFlag_local] != 3 && *(uint32*)&buffer[Health1_local])
    if buffer[o_flag] != 3 and int.from_bytes(buffer[o_h1:o_h1 + 4], 'little'):
        v2 = buffer[o_h3]
        if v2 + o_h4 + 4 > 0x900:
            return 0.0
        raw = buffer[o_h4 + v2: o_h4 + v2 + 4]
        if len(raw) < 4:
            return 0.0
        if buffer[o_h5] != 0:
            seed = int.from_bytes(buffer[o_h6:o_h6 + 4], 'little')
            raw = health_decipher(raw, seed, sdk)
        import struct
        return struct.unpack('<f', raw)[0]
    return 0.0


# ---------------------------------------------------------------------------
# Selftest
# ---------------------------------------------------------------------------

def selftest(sdk):
    """Testa o decrypt: bijecao, ida-e-volta e FIDELIDADE a forma do dump.

    O rank e a ida-e-volta provam que a funcao e' bijetiva e que a inversa
    fecha — mas a leitura ERRADA desta build passava nos dois. Quem pega
    constante trocada e' a comparacao com `decrypt_cindex_dump_literal`.

    Nada aqui prova que as constantes batem com o binario do jogo — isso so o
    jogo prova.
    """
    problems = []

    print('-- SANIDADE DOS PARAMETROS ' + '-' * 51)
    rotr = sdk.NameIndexRotr
    checks = [
        ('NameIndexRotr (Rval) em 0..31',     0 <= rotr < 32),
        ('NameIndexShl  (Dval) em 0..31',     0 <= sdk.NameIndexShl < 32),
        ('NameIndexShr  (Sval) em 0..31',     0 <= sdk.NameIndexShr < 32),
        ('NameIndexMaskRight (XorKey3) != 0', sdk.NameIndexMaskRight != 0),
        ('chaves de health = 16',             len(sdk.HealthKeys) == 16),
    ]
    for label, good in checks:
        print(f'  [{"ok " if good else "ERR"}] {label}')
        if not good:
            problems.append(label)

    print(f'\n  parametros em uso:')
    print(f'    xored  = value ^ 0x{sdk.NameIndexXor1 & M32:08X}')
    print(f'    left   = xored << {sdk.NameIndexShl}                  (Dval)')
    print(f'    right  = (xored >> {sdk.NameIndexShr}) & 0x{sdk.NameIndexMaskRight & M32:08X}    (Sval + XorKey3)')
    print(f'    result = (left | right) ^ _rotr(xored, {rotr}) ^ 0x{sdk.NameIndexXor2 & M32:08X}')
    print(f'    (registrado e NAO usado nesta forma: NameIndexRor = '
          f'0x{sdk.NameIndexRor & M32:X}  <- DecryptNameIndexRor)')

    # dominancia: entrada diferente -> saida diferente
    print('\n-- DISCRIMINACAO ' + '-' * 60)
    probes = [0x00000000, 0x00000001, 0xFFFFFFFF, 0x12345678,
              0xDEADBEEF, 0x0000FFFF, 0xFFFF0000, 0x80000000]
    results = [decrypt_cindex(p, sdk) for p in probes]
    dups = len(results) - len(set(results))
    print(f'  [{"ok " if dups == 0 else "ERR"}] entradas distintas -> saidas '
          f'distintas ({len(probes)} probes, {dups} colisoes)')
    if dups:
        problems.append(f'{dups} colisao(oes) em decrypt_cindex')

    # rank da parte linear: prova se o decrypt e bijetivo (NAO prova se as
    # constantes estao certas — a leitura errada tambem dava 32/32).
    rank = shape_rank(sdk)
    print(f'  [{"ok " if rank == 32 else "ERR"}] rank da parte linear: {rank}/32'
          + ('' if rank == 32 else '  <-- o decrypt NAO e bijetivo'))
    print(f'  [i]   rank 32/32 so prova que NAO HA COLISAO (bijecao) — nao prova')
    print(f'        que as constantes batem com o binario. Quem cobre isso e a')
    print(f'        comparacao com a forma literal do dump, mais abaixo.')

    # ida-e-volta nos probes
    print('\n-- IDA E VOLTA ' + '-' * 60)
    ok_rt = 0
    for p in probes:
        y = decrypt_cindex(p, sdk)
        back = decrypt_cindex_inverse(y, sdk)
        if back == p:
            ok_rt += 1
        else:
            print(f'    [ERR] 0x{p:08X} -> 0x{y:08X} -> {back if back is None else hex(back)}')
    print(f'  [{ "ok " if ok_rt == len(probes) else "ERR"}] {ok_rt}/{len(probes)} '
          f'ida-e-volta')
    if ok_rt != len(probes):
        problems.append(f'ida-e-volta {ok_rt}/{len(probes)}')

    # health: um bloco sintetico montado de proposito
    #
    # A janela do buffer e' a mesma que o cheat le': `Engine::UpdatePlayers`
    # faz um batch a partir do ATOR e o `HeaFlag` local e' 0. Ou seja, os
    # offsets aqui sao `SDK.HealthN - SDK.HeaFlag`, nao o valor absoluto.
    print('\n-- HEALTH ' + '-' * 60)
    import struct
    buf = bytearray(0x1000)
    buf[0] = 0                                        # flag != 3
    buf[sdk.Health1 - sdk.HeaFlag: sdk.Health1 - sdk.HeaFlag + 4] = (1).to_bytes(4, 'little')
    buf[sdk.Health3 - sdk.HeaFlag] = 0                # v2
    buf[sdk.Health5 - sdk.HeaFlag] = 0                # sem decipher
    struct.pack_into('<f', buf, sdk.Health4 - sdk.HeaFlag, 73.5)
    got = decrypt_health(bytes(buf), sdk)
    good = abs(got - 73.5) < 1e-4
    print(f'  [{"ok " if good else "ERR"}] leitura direta: {got} (esperado 73.5)')
    if not good:
        problems.append('health leitura direta')

    # com decipher
    seed = 0x11
    plain = struct.pack('<f', 41.25)
    enc = health_decipher(plain, seed, sdk)
    buf2 = bytearray(buf)
    buf2[sdk.Health4 - sdk.HeaFlag: sdk.Health4 - sdk.HeaFlag + 4] = enc
    buf2[sdk.Health5 - sdk.HeaFlag] = 1
    buf2[sdk.Health6 - sdk.HeaFlag: sdk.Health6 - sdk.HeaFlag + 4] = seed.to_bytes(4, 'little')
    got2 = decrypt_health(bytes(buf2), sdk)
    good2 = abs(got2 - 41.25) < 1e-4
    print(f'  [{"ok " if good2 else "ERR"}] com health_decipher: {got2} (esperado 41.25)')
    if not good2:
        problems.append('health com decipher')

    # o health_decipher tem que ser involutivo
    rt = health_decipher(health_decipher(plain, seed, sdk), seed, sdk)
    good3 = rt == plain
    print(f'  [{"ok " if good3 else "ERR"}] health_decipher e involutivo')
    if not good3:
        problems.append('health_decipher nao involutivo')

    # -----------------------------------------------------------------------
    # A prova que o rank NAO da': a forma do dump.
    #
    # O rank 32/32 e a ida-e-volta provam que a funcao e' bijetiva e que a
    # inversa fecha — e a leitura ERRADA desta build (shl=15, `& 0xFFFF0000`,
    # `>>` ignorado, rotr=1) PASSAVA nas duas. O que ela nao passa e' isto:
    # comparar, entrada por entrada, com a forma literal do dump. Se um campo
    # do Globals.h estiver trocado (Dval <-> Sval <-> Rval <-> Ror) o rank
    # continua 32 e a comparacao acusa.
    # -----------------------------------------------------------------------
    print('\n-- FORMA DO DUMP (a prova que o rank nao da) ' + '-' * 34)
    amostra = list(probes) + [(1 << b) for b in range(32)] + \
              [((i * 0x9E3779B1) ^ (i << 7)) & M32 for i in range(512)]

    div = [x for x in amostra
           if decrypt_cindex(x, sdk) != decrypt_cindex_dump_literal(x)]
    good = not div
    print(f'  [{"ok " if good else "ERR"}] decrypt_cindex == forma literal do dump '
          f'em {len(amostra)} amostras ({len(div)} divergencia(s))')
    if div:
        print(f'        ex: 0x{div[0]:08X} -> 0x{decrypt_cindex(div[0], sdk):08X} '
              f'(dump: 0x{decrypt_cindex_dump_literal(div[0]):08X})')
        problems.append('decrypt_cindex diverge da forma do dump')

    # controle negativo: a leitura antiga TEM que divergir do dump (e o rank
    # dela era 32/32 — era exatamente o "ok" falso do boot).
    old_div = sum(1 for x in amostra
                  if decrypt_cindex_leitura_antiga(x)
                  != decrypt_cindex_dump_literal(x))
    print(f'  [{"ok " if old_div else "ERR"}] controle negativo (a leitura antiga, '
          f'a que estava no codigo): diverge do dump em {old_div}/{len(amostra)} '
          f'amostras')
    print(f'        ela TINHA rank 32/32 e ida-e-volta 100% — e estava errada.')
    if not old_div:
        problems.append('controle negativo nao divergiu do dump')

    print(f'\n  build 2609.1.3.5 (a que o Globals.h tem agora):')
    print(f'         decrypt_cindex(0) = 0x{decrypt_cindex(0x00000000, sdk):08X}   '
          f'decrypt_cindex_leitura_antiga(0) = '
          f'0x{decrypt_cindex_leitura_antiga(0x00000000):08X}')
    print(f'         rotr(Rval)={sdk.NameIndexRotr}  shl(Dval)={sdk.NameIndexShl}  '
          f'shr(Sval)={sdk.NameIndexShr}  mask(XorKey3)=0x{sdk.NameIndexMaskRight & M32:X}  '
          f'Ror={sdk.NameIndexRor} (nao usado)')

    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--globals', default=None)
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--dump', action='store_true')
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    globals_path = args.globals or os.path.join(root, 'PubgExt', 'Globals.h')

    print('=' * 78)
    print('PUBG DECRYPT â€” implementacao de referencia')
    print('=' * 78)
    print(f'  Globals.h: {globals_path}')

    try:
        sdk = Sdk.from_globals(globals_path)
    except Exception as e:
        print(f'[err] {e}')
        return 1

    if args.dump:
        print()
        for k in sorted(Sdk.FIELDS):
            v = getattr(sdk, k, None)
            if v is not None:
                print(f'  {k:<20} = 0x{v:08X}  ({v})')
        print(f'  {"HealthKeys":<20} = {len(sdk.HealthKeys)} chaves')

    if args.selftest or not args.dump:
        print()
        problems = selftest(sdk)
        print()
        print('=' * 78)
        if problems:
            print(f'  RESULTADO: {len(problems)} problema(s)')
            for p in problems:
                print(f'    - {p}')
        else:
            print('  RESULTADO: decrypt coerente')
        print('=' * 78)
        return 1 if problems else 0

    return 0


if __name__ == '__main__':
    sys.exit(main())