"""
kdump_info.py — le o cabecalho de um kernel dump do Windows (.dmp) e extrai
o bugcheck, os parametros e o contexto da thread que falhou.

Por que existe: quando o nosso driver derruba a maquina, o UNICO dado que diz
o que aconteceu esta no dump. Sem isso so sobra adivinhar.

Nao precisa de WinDbg. O formato:

  0x000  "PAGEDU64"                 assinatura de kernel dump x64
  0x004  ULONG  BugCheckCode
  0x008  ULONG64 BugCheckParameter1..4   (4 * 8 bytes)
  0x038  ULONG  ValidDump / MajorVersion / MinorVersion
  ...
  0xF88  KDDEBUGGER_DATA64 *
  0xFA0  DumpType
  ...
  logo apos o cabecalho:  os "context records" — blocos de 0x800 (2048) bytes,
  cada um com o contexto (registradores) de uma thread, contiguos no arquivo.
  O primeiro que tiver um contexto x64 valido e o da thread que falhou.

Uso:
    python kdump_info.py C:\\Windows\\MEMORY.DMP
"""

import argparse
import struct
import sys

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

CONTEXT_SIZE = 0x800
MAX_CONTEXTS = 4096          # 8 MB de cabecalho, de sobra


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--contexts', type=int, default=8,
                    help='quantos contextos mostrar')
    args = ap.parse_args()

    with open(args.path, 'rb') as f:
        head = f.read(0x1000)

    sig = head[:8]
    print(f'arquivo   : {args.path}')
    print(f'assinatura: {sig!r}')
    if not sig.startswith(b'PAGEDU'):
        print('  [!] nao parece um kernel dump do Windows')
        return 1

    code = struct.unpack_from('<I', head, 0x04)[0]
    p1, p2, p3, p4 = struct.unpack_from('<QQQQ', head, 0x08)
    major, minor = struct.unpack_from('<II', head, 0x30)
    dump_type = struct.unpack_from('<I', head, 0xF98)[0] \
        if len(head) >= 0xF9C else 0

    print()
    print('=== BUGCHECK ===')
    print(f'  codigo      : 0x{code:08X}')
    print(f'  parametro 1 : 0x{p1:016X}')
    print(f'  parametro 2 : 0x{p2:016X}')
    print(f'  parametro 3 : 0x{p3:016X}')
    print(f'  parametro 4 : 0x{p4:016X}')
    print(f'  versao      : {major}.{minor}')
    print(f'  dump type   : {dump_type}')

    nomes = {
        0x0A: 'IRQL_NOT_LESS_OR_EQUAL',
        0x1E: 'KMODE_EXCEPTION_NOT_HANDLED',
        0x3B: 'SYSTEM_SERVICE_EXCEPTION',
        0x50: 'PAGE_FAULT_IN_NONPAGED_AREA',
        0x7E: 'SYSTEM_THREAD_EXCEPTION_NOT_HANDLED',
        0xC2: 'BAD_POOL_CALLER',
        0xC5: 'DRIVER_CORRUPTED_EXPOOL',
        0xD1: 'DRIVER_IRQL_NOT_LESS_OR_EQUAL',
        0xD6: 'DRIVER_PAGE_FAULT_IN_FREED_SPECIAL_POOL',
        0x139: 'KERNEL_SECURITY_CHECK_FAILURE',
    }
    if code in nomes:
        print(f'  nome        : {nomes[code]}')

    if code == 0x50:
        print()
        print('  --- leitura do 0x50 (PAGE_FAULT_IN_NONPAGED_AREA) ---')
        print(f'  endereco que faltou : 0x{p1:016X}')
        acesso = {0: 'LEITURA', 1: 'ESCRITA', 2: 'ESCRITA', 8: 'EXECUCAO'}
        print(f'  tipo de acesso      : {p2} = {acesso.get(p2, "?")}')
        print(f'  instrucao que falhou: 0x{p3:016X}')
        if p1 == 0xFFFFFFFFFFFFFFD0:
            print('  >>> 0xFFFFFFFFFFFFFFD0 == -0x30 a partir de ponteiro NULL.')

    f.seek(0x1000)
    print()
    print(f'=== CONTEXTOS (primeiros {args.contexts}) ===')
    achou = False
    for i in range(args.contexts):
        raw = f.read(CONTEXT_SIZE)
        if len(raw) < CONTEXT_SIZE:
            break
        # ContextFlags fica no inicio do CONTEXT x64; 0x00100000 = CONTEXT_AMD64
        ctx_flags = struct.unpack_from('<I', raw, 0x30)[0]
        rip = struct.unpack_from('<Q', raw, 0xF8)[0]
        rsp = struct.unpack_from('<Q', raw, 0x98)[0]
        rbp = struct.unpack_from('<Q', raw, 0xA0)[0]
        rax = struct.unpack_from('<Q', raw, 0x78)[0]
        rcx = struct.unpack_from('<Q', raw, 0x80)[0]
        rdx = struct.unpack_from('<Q', raw, 0x88)[0]
        r8  = struct.unpack_from('<Q', raw, 0xB8)[0]
        r9  = struct.unpack_from('<Q', raw, 0xC0)[0]

        valido = (0xFFFF000000000000 <= rip or rip == 0) and ctx_flags & 0x00100000
        if not valido and rip == 0:
            continue
        achou = True
        print(f'  --- contexto {i} (flags=0x{ctx_flags:08X}) ---')
        print(f'      rip = 0x{rip:016X}')
        print(f'      rsp = 0x{rsp:016X}')
        print(f'      rbp = 0x{rbp:016X}')
        print(f'      rax = 0x{rax:016X}   rcx = 0x{rcx:016X}')
        print(f'      rdx = 0x{rdx:016X}   r8  = 0x{r8:016X}   r9 = 0x{r9:016X}')

    if not achou:
        print('  nenhum contexto x64 valido nos primeiros blocos')
        print('  (o dump pode ter um cabecalho maior; aumente --contexts)')

    return 0


if __name__ == '__main__':
    sys.exit(main())
