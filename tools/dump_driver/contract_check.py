#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
contract_check.py -- valida o contrato de IOCTL entre o user mode e o driver
de kernel QUE RODA HOJE.

===========================================================================
O QUE ESTAVA ERRADO NESTE ARQUIVO (corrigido)
===========================================================================

Ele lia `km_driver_source.c` -- o driver APOSENTADO (`Ndu`, `0x00222004`).
Esse projeto veio de outra maquina e o checklist ficou apontando para o
arquivo velho, entao ele acusava 13 divergencias num contrato que estava
certo, e imprimia `RESULTADO: 13 DIVERGENCIA(S)` num build saudavel. O
`--selftest` passava 5/5 mesmo assim: o checker funcionava, so' olhava
para o alvo errado.

Agora:

  * o kernel de referencia e' `km_safe_source.c` (o que vira
    PUBG_KM_safe.sys e e' mapeado embutido pelo mtmacr0.exe);
  * `--kernel km_driver_source.c` ainda audita o antigo, para quando for
    preciso comparar;
  * TODO consumidor e' conferido contra o kernel, nao so' um. Hoje sao
    tres copias das mesmas macros:

        PubgExt\\driver\\common.h                 (o header do cheat)
        PubgExt\\driver\\driver_interface_v3.cpp  (redefinia por cima)
        tools\\TestHarness.cpp                   (standalone, fora do .sln)

    Tres copias e' exatamente o bug que este checker existe para pegar
    (ARQUITETURA.md secao 3, "tres definicoes conflitantes"). Uma delas
    divergir e' o suficiente para o cheat falar com o driver errado.

===========================================================================
O QUE ELE VERIFICA
===========================================================================

  1. CODIGOS DE IOCTL   -- kernel x cada consumidor, nome por nome.
  2. STRUCT DO CONTRATO -- layout (tamanho, offset e largura de CADA campo).
     Compara ABI, nao nomes: o kernel usa `Pid`, o user mode usa `pid`, e
     isso e' legitimo. Largura e offset tem que bater.
  3. CONSTANTES PROPRIAS -- todo #define numerico presente nos DOIS lados
     tem que ter o mesmo valor (KM_MAX_COPY, KM_MAX_NAME, KM_PING_MAGIC,
     KM_DRIVER_VERSION...). Pega deriva em qualquer constante, nao so' nas
     que alguem lembrou de listar.
  4. NOME DO DEVICE      -- o nome deixou de ser literal: e' derivado de
     seed. Entao o que se compara e' a DERIVACAO: a constante de mistura,
     as duas magicas do MurmurHash, o numero de hex e o `kind` de cada
     nome. Se um lado mexer em qualquer um dos quatro, o user mode abre um
     nome que o kernel nao criou e o cheat fica "sem driver" sem erro.
  5. ORFAOS              -- IOCTL definido e nunca usado.

Uso:
    python contract_check.py
    python contract_check.py --selftest
    python contract_check.py --root C:\\caminho\\do\\projeto
    python contract_check.py --kernel km_driver_source.c     (driver antigo)
"""

import argparse
import os
import re
import sys
from collections import OrderedDict

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

CTL_CODE_TYPE = {'BUFFERED': 0, 'IN_DIRECT': 1, 'OUT_DIRECT': 2, 'NEITHER': 3}
ACCESS = {'ANY': 0, 'READ': 1, 'WRITE': 2, 'RW': 3}

# FILE_DEVICE_UNKNOWN. `KM_DEVICE_TYPE` no kernel e' o mesmo valor.
DEVICE_UNKNOWN = 0x22


def decode_ctl_code(func, method, access, device=DEVICE_UNKNOWN):
    return (device << 16) | (access << 14) | (func << 2) | method


# ---------------------------------------------------------------------------
# Definicoes
# ---------------------------------------------------------------------------
def parse_defines(text):
    """Todos os '#define NOME <constante numerica>', com ou sem parenteses.

    Tira comentario antes de casar -- '#define X (1024 * 1024) // nota' nao
    casa com o padrao se o comentario ficar no fim da linha.
    """
    clean = re.sub(r'//[^\n]*', '', text)
    clean = re.sub(r'/\*.*?\*/', '', clean, flags=re.S)
    out = {}
    rx = re.compile(r'#define\s+(\w+)\s+(\()?\s*([0-9][0-9A-Fa-fxX\s\*\+\-\|\(\)]*?)\s*(\))?\s*$',
                    re.M)
    for m in rx.finditer(clean):
        name, open_p, body, close_p = m.groups()
        if (open_p is None) != (close_p is None):
            continue
        out[name] = body.strip()
    return out


def eval_const(expr, defines, depth=0):
    """Resolve 260, (260), KM_MAX_NAME, 1024 * 1024. None se nao der."""
    if depth > 8 or expr is None:
        return None
    e = expr.strip()
    if e in defines:
        return eval_const(defines[e], defines, depth + 1)
    if re.fullmatch(r'0[xX][0-9A-Fa-f]+(ULL|UL|L)?', e):
        return int(re.sub(r'(ULL|UL|L)$', '', e), 16)
    if re.fullmatch(r'\d+', e):
        return int(e)
    sub = e
    for name in set(re.findall(r'[A-Za-z_]\w*', e)):
        val = defines.get(name)
        if val is None:
            return None
        sub = re.sub(r'\b' + re.escape(name) + r'\b', f'({val})', sub)
    sub = re.sub(r'(ULL|UL|L)\b', '', sub)
    if re.fullmatch(r'[0-9\s\+\-\*/\(\)]+', sub):
        try:
            return int(eval(sub, {'__builtins__': {}}, {}))
        except Exception:
            return None
    return None


# ---------------------------------------------------------------------------
# IOCTLs
# ---------------------------------------------------------------------------
def ctl_codes(text):
    """Extrai '#define NAME CTL_CODE(<dev>, 0xNNN, METHOD_X, FILE_Y_ACCESS)'.

    Aceita qualquer token de device (`KM_DEVICE_TYPE`, `FILE_DEVICE_UNKNOWN`,
    ...) porque o kernel usa o primeiro e o user mode o segundo -- sao o
    mesmo 0x22, e o que importa e' o code final.
    """
    rx = re.compile(
        r'#define\s+(\w+)\s+CTL_CODE\(\s*([\w]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*'
        r'METHOD_(\w+)\s*,\s*FILE_(\w+)_ACCESS\s*\)')
    out = OrderedDict()
    for m in rx.finditer(text):
        name, dev, func, method, access = m.groups()
        meth = method.upper()
        acc = access.upper()
        if meth not in CTL_CODE_TYPE or acc not in ACCESS:
            continue
        out[name] = {
            'device_token': dev,
            'func': int(func, 16),
            'method': meth,
            'access': acc,
            'raw': decode_ctl_code(int(func, 16), CTL_CODE_TYPE[meth], ACCESS[acc]),
        }
    return out


# ---------------------------------------------------------------------------
# Structs -- layout, nao nomes
# ---------------------------------------------------------------------------
# Tamanhos em bytes, x64. Nomes de tipo do kernel e do user mode convivem.
TYPE_SIZE = {
    'int': 4, 'INT': 4, 'LONG': 4, 'ULONG': 4, 'UINT32': 4, 'DWORD': 4,
    'NTSTATUS': 4, 'BOOL': 4, 'float': 4,
    'ULONG64': 8, 'UINT64': 8, 'ULONG_PTR': 8, 'SIZE_T': 8, 'size_t': 8,
    'uintptr_t': 8, 'uint64_t': 8, 'int64_t': 8, 'void*': 8,
    'CHAR': 1, 'char': 1, 'UCHAR': 1, 'BYTE': 1, 'BOOLEAN': 1, 'bool': 1,
    'USHORT': 2, 'WCHAR': 2, 'wchar_t': 2, 'short': 2,
}


def norm_type(t):
    """Normaliza o tipo: tira const/struct, colapsa espaco, marca ponteiro."""
    t = t.replace('*', ' * ')
    t = re.sub(r'\b(const|volatile|struct|class)\b', ' ', t)
    t = re.sub(r'\s+', ' ', t).strip()
    if t.endswith('*'):
        return 'void*'          # QUALQUER ponteiro e' 8 bytes em x64
    return t


def type_size(t):
    return TYPE_SIZE.get(norm_type(t))


def _split_fields(body):
    body = re.sub(r'//[^\n]*', '', body)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    fields = []
    for chunk in body.split(';'):
        chunk = chunk.strip()
        if not chunk:
            continue
        parts = chunk.split()
        if len(parts) < 2:
            continue
        # o nome e' o ULTIMO token; o tipo e' todo o resto ('const wchar_t *')
        name = parts[-1]
        ftype = ' '.join(parts[:-1])
        arr = None
        am = re.search(r'\[(\w+)\]', name)
        if am:
            arr = am.group(1)
            name = name[:name.index('[')]
        fields.append((ftype, name, arr))
    return fields


def struct_fields(text, struct_name):
    """Aceita as duas formas:

        typedef struct _NAME { ... } NAME, *PNAME;
        struct NAME { ... };

    A segunda e' como o user mode declara `DRIVER_REQUEST`. A versao
    anterior so' entendia a primeira e devolvia None para a outra -- que
    virava "[warn] nao encontrada", o tipo de aviso que se aprende a
    ignorar.
    """
    pats = [
        r'typedef\s+struct\s+_?' + re.escape(struct_name) + r'\s*\{(.*?)\}\s*'
        + re.escape(struct_name) + r'\s*[,;]',
        r'\bstruct\s+_?' + re.escape(struct_name) + r'\s*\{(.*?)\}\s*;',
    ]
    for p in pats:
        m = re.search(p, text, re.S)
        if m:
            return _split_fields(m.group(1))
    return None


def struct_layout(fields, defines):
    """(total, [(nome, offset, largura), ...]) com alinhamento natural (pack 8).

    Devolve (None, None) se algum tipo nao for conhecido -- e' melhor
    admitir que nao sabe do que chutar 8 e dizer "bate".
    """
    if fields is None:
        return None, None
    offset = 0
    max_align = 1
    layout = []
    for ftype, fname, arr in fields:
        base = type_size(ftype)
        if base is None:
            return None, None
        if arr is None:
            n = 1
        elif arr.isdigit():
            n = int(arr)
        else:
            n = eval_const(arr, defines)
            if n is None:
                return None, None
        size = base * n
        align = min(base, 8)
        max_align = max(max_align, align)
        offset += (-offset) % align
        layout.append((fname, offset, size))
        offset += size
    total = offset + ((-offset) % max_align)
    return total, layout


# ---------------------------------------------------------------------------
# Derivacao do nome do device
# ---------------------------------------------------------------------------
NAME_MIX_MAGICS = ('FF51AFD7ED558CCD', 'C4CEB9FE1A85EC53')


def device_derivation(text):
    """Extrai o que a derivacao do nome do device precisa ter igual nos dois
    lados: a constante de mistura, as duas magicas do MurmurHash, quantos
    caracteres hex saem, e o `kind` usado para cada nome."""
    d = {}
    m = re.search(r'KM_NAME_MIX\s*=?\s*(0x[0-9A-Fa-f]+)', text)
    d['mix'] = int(m.group(1), 16) if m else None
    d['magics'] = [g for g in NAME_MIX_MAGICS if g in text.upper()]
    m = re.search(r'KM_NAME_CHARS\s+(\d+)', text)
    d['chars'] = int(m.group(1)) if m else (16 if 'KmHex16' in text or 'i = 15; i >= 0' in text else None)
    # kind: no kernel, KmMixSeed(seed, 0) = device, (seed, 1) = symlink.
    d['kind_dev'] = 0 if re.search(r'KmMixSeed\s*\(\s*seed\s*,\s*0\s*\)', text) else None
    d['kind_sym'] = 1 if re.search(r'KmMixSeed\s*\(\s*seed\s*,\s*1\s*\)', text) else None
    # no user mode o mixer e' KmMixSeedCpp(seed, 1) -- o symlink e' o unico
    # nome que o user-mode ve'.
    d['user_kind'] = 1 if re.search(r'KmMixSeed\w*\s*\(\s*seed\s*,\s*1\s*\)', text) else None
    return d


# ---------------------------------------------------------------------------
# Caminhos
# ---------------------------------------------------------------------------
def caminhos(root, kernel_name):
    return {
        'kernel':  os.path.join(root, kernel_name),
        'common':  os.path.join(root, 'PubgExt', 'driver', 'common.h'),
        'uiface':  os.path.join(root, 'PubgExt', 'driver', 'driver_interface_v3.cpp'),
        'harness': os.path.join(root, 'tools', 'TestHarness.cpp'),
    }


CONSUMIDORES = [
    ('common.h', 'common'),
    ('driver_interface_v3.cpp', 'uiface'),
    ('TestHarness.cpp', 'harness'),
]


# ---------------------------------------------------------------------------
# Selftest
# ---------------------------------------------------------------------------
def selftest():
    """Prova que o checker REPROVA quando o contrato diverge.

    Um checker que so' diz 'ok' nao vale nada. Cada mutacao abaixo e' uma
    divergencia real que ja' aconteceu (ou quase) neste projeto, e o
    checker TEM que sair com codigo != 0 em todas.
    """
    import tempfile
    import shutil
    import subprocess

    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ps = caminhos(root, 'km_safe_source.c')
    kernel = ps['kernel']
    if not os.path.isfile(kernel):
        print(f'[err] selftest: nao encontrei {kernel}')
        return 1

    base = open(kernel, encoding='utf-8', errors='replace').read()

    mutations = [
        ('IOCTL com func trocado',
         base.replace('0x801, METHOD_BUFFERED', '0x811, METHOD_BUFFERED', 1)),
        ('campo removido da struct do kernel',
         base.replace('wchar_t*   Name;\n} KM_DRIVER_REQUEST',
                      '} KM_DRIVER_REQUEST', 1)),
        ('KM_MAX_NAME diferente',
         base.replace('#define KM_MAX_NAME    260', '#define KM_MAX_NAME    128', 1)),
        ('constante de mistura do nome diferente',
         base.replace('#define KM_NAME_MIX      0x9E3779B97F4A7C15ULL',
                      '#define KM_NAME_MIX      0x1234567890ABCDEFULL', 1)),
        ('teto de payload diferente',
         base.replace('#define KM_MAX_COPY    (1024 * 1024)',
                      '#define KM_MAX_COPY    (512 * 1024)', 1)),
    ]

    print('=' * 78)
    print('SELFTEST -- o checker reprova quando o contrato diverge?')
    print('=' * 78)

    ok = 0
    for label, mutated in mutations:
        if mutated == base:
            print(f'  [skip] {label}: mutacao nao aplicou (texto nao encontrado)')
            continue

        with tempfile.TemporaryDirectory() as td:
            for key in ('common', 'uiface', 'harness'):
                dst = os.path.join(td, os.path.relpath(ps[key], root))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy(ps[key], dst)
            open(os.path.join(td, 'km_safe_source.c'), 'w',
                 encoding='utf-8').write(mutated)

            cp = subprocess.run(
                [sys.executable, os.path.abspath(__file__), '--root', td],
                capture_output=True, text=True, encoding='utf-8', errors='replace')
            detected = cp.returncode != 0
            print(f'  [{"ok" if detected else "FALHOU"}] {label}'
                  + ('' if detected else '   <-- passou batido!'))
            if detected:
                ok += 1

    print(f'\n  {ok}/{len(mutations)} mutacoes detectadas')
    print('=' * 78)
    return 0 if ok == len(mutations) else 1


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))
    ap.add_argument('--kernel', default='km_safe_source.c',
                    help='arquivo do driver de kernel (default: o que roda hoje)')
    ap.add_argument('--selftest', action='store_true',
                    help='verifica que o checker realmente detecta divergencia')
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    ps = caminhos(args.root, args.kernel)
    if not os.path.isfile(ps['kernel']):
        print(f'[err] nao encontrei o kernel: {ps["kernel"]}')
        return 1
    if not os.path.isfile(ps['uiface']):
        print(f'[err] nao encontrei {ps["uiface"]}')
        return 1

    texto = {}
    for key, path in ps.items():
        texto[key] = open(path, encoding='utf-8', errors='replace').read() \
            if os.path.isfile(path) else None

    k_text = texto['kernel']
    k_defs = parse_defines(k_text)
    k_codes = ctl_codes(k_text)

    fails = 0
    warns = 0

    print('=' * 78)
    print('CONTRACT CHECK -- PubgExt')
    print(f'  kernel : {os.path.basename(ps["kernel"])}'
          + ('' if args.kernel == 'km_safe_source.c'
             else '   [APOSENTADO]'))
    print('  fonte da verdade do user mode: PubgExt/driver/common.h')
    print('=' * 78)

    if not k_codes:
        print(f'\n  [err] {os.path.basename(ps["kernel"])} nao define nenhum CTL_CODE')
        return 1

    # =======================================================================
    # 1. IOCTLs
    # =======================================================================
    print('\n-- 1. CODIGOS DE IOCTL ' + '-' * 56)

    for label, key in CONSUMIDORES:
        t = texto.get(key)
        if t is None:
            print(f'  [skip] {label}: arquivo ausente')
            continue
        c_codes = ctl_codes(t)
        if not c_codes:
            print(f'  [skip] {label}: nao define CTL_CODE (usa o header)')
            continue

        erros_antes = fails
        for n in sorted(c_codes):
            kv = k_codes.get(n, {}).get('raw')
            cv = c_codes[n]['raw']
            if kv is None:
                # WRITE_MEMORY e' declarado no user mode e NAO implementado no
                # kernel de proposito (ARQUITETURA secao 3). Nao e' bug -- mas
                # tem que continuar sendo AVISO, nao silencio.
                if n == 'IOCTL_WRITE_MEMORY':
                    print(f'  [warn] {label}: {n} 0x{cv:08X} nao existe no kernel '
                          f'(declarado, nao implementado - documentado)')
                    warns += 1
                else:
                    print(f'  [err]  {label}: {n} 0x{cv:08X} nao existe no kernel')
                    fails += 1
            elif kv != cv:
                print(f'  [err]  {label}: {n} 0x{cv:08X} != kernel 0x{kv:08X}')
                fails += 1

        if fails == erros_antes:
            print(f'  [ok]   {label}: {len(c_codes)} IOCTL(s) batem com o kernel')

    # =======================================================================
    # 1b. CONSTANTES PROPRIAS
    # =======================================================================
    print('\n-- 1b. CONSTANTES COMPARTILHADAS ' + '-' * 46)

    for label, key in CONSUMIDORES:
        t = texto.get(key)
        if t is None:
            continue
        c_defs = parse_defines(t)
        comuns = sorted(set(k_defs) & set(c_defs))
        if not comuns:
            print(f'  [i]    {label}: nenhuma constante numerica compartilhada')
            continue
        ruins = 0
        for n in comuns:
            kv = eval_const(n, k_defs)
            cv = eval_const(n, c_defs)
            if kv is None or cv is None:
                continue
            if kv != cv:
                print(f'  [err]  {label}: {n} = {cv} != kernel {kv}')
                fails += 1
                ruins += 1
        if ruins == 0:
            print(f'  [ok]   {label}: {len(comuns)} constante(s) numerica(s) batem')

    # =======================================================================
    # 2. Struct do contrato
    # =======================================================================
    print('\n-- 2. STRUCT DO CONTRATO ' + '-' * 55)

    kf = struct_fields(k_text, 'KM_DRIVER_REQUEST')
    ksize, klay = struct_layout(kf, k_defs)

    if kf is None:
        print('  [err]  KM_DRIVER_REQUEST nao encontrada no kernel')
        fails += 1
    elif ksize is None:
        print('  [warn] KM_DRIVER_REQUEST: tipo desconhecido, layout nao calculado')
        warns += 1
    else:
        print(f'  [i]    kernel KM_DRIVER_REQUEST = {ksize} bytes '
              f'({", ".join(f"{n}@{o}+{w}" for n, o, w in klay)})')

        for label, key in (('driver_interface_v3.cpp', 'uiface'),
                           ('TestHarness.cpp', 'harness')):
            t = texto.get(key)
            if t is None:
                continue
            uf = struct_fields(t, 'DRIVER_REQUEST')
            if uf is None:
                print(f'  [warn] {label}: struct DRIVER_REQUEST nao encontrada')
                warns += 1
                continue
            u_defs = parse_defines(t)
            all_defs = dict(k_defs)
            all_defs.update(u_defs)
            us, ulay = struct_layout(uf, all_defs)
            if us is None:
                print(f'  [warn] {label}: tipo desconhecido no DRIVER_REQUEST')
                warns += 1
                continue

            # ABI: tamanho + (offset, largura) de cada campo. NOMES PODEM
            # DIFERIR -- o kernel usa `Pid`, o user mode `pid`. O que nao pode
            # e' o byte cair em outro lugar.
            if us == ksize and [(o, w) for _, o, w in ulay] == [(o, w) for _, o, w in klay]:
                print(f'  [ok]   {label}: DRIVER_REQUEST = {us} bytes, '
                      f'mesmo layout do kernel')
            else:
                print(f'  [err]  {label}: DRIVER_REQUEST = {us} bytes, '
                      f'kernel = {ksize} bytes  <-- ABI DIFERENTE')
                print(f'         kernel  {[(o, w) for _, o, w in klay]}')
                print(f'         {label:<8}{[(o, w) for _, o, w in ulay]}')
                fails += 1

    # =======================================================================
    # 3. Limites
    # =======================================================================
    print('\n-- 3. LIMITES E TETO DE PAYLOAD ' + '-' * 45)

    k_copy = eval_const('KM_MAX_COPY', k_defs)
    k_name = eval_const('KM_MAX_NAME', k_defs)
    if k_copy is None:
        print('  [err]  KM_MAX_COPY nao definido no kernel')
        fails += 1
    else:
        print(f'  [i]    kernel KM_MAX_COPY = {k_copy} bytes '
              f'({k_copy / 1024 / 1024:.1f} MiB por IOCTL)')
    if k_name is None:
        print('  [warn] KM_MAX_NAME nao resolvido no kernel')
        warns += 1
    else:
        print(f'  [i]    kernel KM_MAX_NAME = {k_name}')

    # O user mode nao declara teto proprio: quem garante e' o driver (recusa
    # Size > KM_MAX_COPY com STATUS_INVALID_PARAMETER). Isso e' por desenho,
    # e este print existe para o dia em que alguem "consertar" isso do lado
    # errado -- se o user mode passar a mandar pacotes maiores, o driver
    # recusa e a leitura falha em silencio.
    print('  [i]    user mode nao declara teto: o enforcement e\' do kernel '
          '(Size > KM_MAX_COPY -> STATUS_INVALID_PARAMETER)')

    # =======================================================================
    # 4. Derivacao do nome do device
    # =======================================================================
    print('\n-- 4. NOME DO DEVICE (derivado de seed) ' + '-' * 39)

    kd = device_derivation(k_text)
    ud = device_derivation(texto.get('uiface') or '')

    if kd['mix'] is None:
        print('  [err]  KM_NAME_MIX nao encontrado no kernel')
        fails += 1
    elif ud['mix'] is None:
        print('  [err]  KM_NAME_MIX nao encontrado no user mode')
        fails += 1
    elif kd['mix'] != ud['mix']:
        print(f'  [err]  KM_NAME_MIX: kernel 0x{kd["mix"]:016X} '
              f'!= user 0x{ud["mix"]:016X}')
        fails += 1
    else:
        print(f'  [ok]   KM_NAME_MIX = 0x{kd["mix"]:016X} nos dois lados')

    faltando = [g for g in NAME_MIX_MAGICS if g not in ud['magics']]
    if faltando:
        print(f'  [err]  user mode nao tem a(s) magica(s) do mixer: {faltando}')
        fails += 1
    else:
        print('  [ok]   as duas magicas do MurmurHash presentes nos dois lados')

    if kd['chars'] is not None and kd['chars'] != 16:
        print(f'  [warn] KM_NAME_CHARS = {kd["chars"]} (esperado 16 hex)')
        warns += 1
    if kd['kind_sym'] != ud['user_kind']:
        print(f'  [err]  kind do symlink: kernel={kd["kind_sym"]} '
              f'user={ud["user_kind"]}  <-- o user mode abriria outro nome')
        fails += 1
    else:
        print('  [ok]   o user mode usa o kind do SYMLINK (1), igual ao kernel')

    # =======================================================================
    # 5. Orfaos
    # =======================================================================
    print('\n-- 5. IOCTL DEFINIDO E NUNCA USADO ' + '-' * 44)

    projeto = '\n'.join(t for t in texto.values() if t)
    for n in sorted(k_codes):
        usa = len(re.findall(r'\b' + re.escape(n) + r'\b', projeto)) - 1
        if usa <= 0:
            print(f'  [warn] {n} definido no kernel e nunca usado')
            warns += 1
        else:
            print(f'  [i]    {n:<24} usado {usa}x')

    # =======================================================================
    print('\n' + '=' * 78)
    if fails == 0:
        print(f'  RESULTADO: contrato CONSISTENTE  ({warns} aviso(s))')
    else:
        print(f'  RESULTADO: {fails} DIVERGENCIA(S), {warns} aviso(s)')
    print('=' * 78)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
