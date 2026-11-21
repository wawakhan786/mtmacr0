"""
minidump_info.py — le um kernel minidump do Windows e extrai o contexto e o
STACK da thread que falhou.

Por que existe: o MEMORY.DMP tem ACL do SYSTEM e nao da para copiar. O
minidump em C:\\Windows\\Minidump e menor (1,6 MB) e legivel. E ele traz o
MINIDUMP_EXCEPTION_STREAM, que tem o CONTEXT com o RSP — e com o RSP da para
ler o stack e achar QUEM chamou a instrucao que falhou.

Formato (MINIDUMP_HEADER, 32 bytes):
   0x00 ULONG32 Signature        ('MDMP' = 0x504D444D)
   0x04 ULONG32 Version
   0x08 ULONG32 NumberOfStreams
   0x0C ULONG32 StreamDirectoryRva
   0x10 ULONG32 CheckSum
   ...

Cada MINIDUMP_DIRECTORY (12 bytes): StreamType, DataSize, Rva.

Stream que interessa:
   7  = ExceptionStream  -> MINIDUMP_EXCEPTION_STREAM
        0x00 ThreadId
        0x04 align
        0x08 MINIDUMP_EXCEPTION (152 bytes): ExceptionCode, ExceptionFlags,
             ExceptionRecord, ExceptionAddress, NumberParameters,
             ExceptionInformation[15]
        0xA0 ThreadContext (LOCATION_DESCRIPTOR: size, rva)

Uso:
    python minidump_info.py C:\\Windows\\Minidump\\xxx.dmp
"""

import argparse
import struct
import sys

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

STREAM_EXCEPTION = 7
STREAM_MODULE_LIST = 4
STREAM_MEMORY_LIST = 5


def read_header(data):
    sig, ver, nstreams, dir_rva, checksum, ts, flags = \
        struct.unpack_from('<IIIIIIQ', data, 0)
    return {
        'sig': sig, 'nstreams': nstreams, 'dir_rva': dir_rva,
        'timestamp': ts, 'flags': flags,
    }


def streams(data):
    h = read_header(data)
    out = []
    for i in range(h['nstreams']):
        off = h['dir_rva'] + i * 12
        st, size, rva = struct.unpack_from('<III', data, off)
        out.append((st, size, rva))
    return h, out


NAMES = {
    3: 'ThreadList', 4: 'ModuleList', 5: 'MemoryList', 6: 'Exception(alt)',
    7: 'Exception', 9: 'CommentA', 10: 'CommentW', 11: 'HandleData',
    12: 'FunctionTable', 13: 'UnloadedModuleList', 15: 'MiscInfo',
    16: 'MemoryInfoList', 17: 'ThreadInfoList', 18: 'HandleOperationList',
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    args = ap.parse_args()

    data = open(args.path, 'rb').read()
    h, sts = streams(data)

    print(f'arquivo    : {args.path}')
    print(f'  tamanho  : {len(data)} bytes')
    print(f'  assinatura: 0x{h["sig"]:08X}  (0x504D444D = "MDMP")')
    print(f'  streams  : {h["nstreams"]}')
    for st, size, rva in sts:
        print(f'     {st:>3}  {NAMES.get(st, "?"):<22} size={size:<8} rva=0x{rva:X}')

    exc = next(((s, r) for s, sz, r in sts if s == STREAM_EXCEPTION), None)
    if not exc:
        print('  sem ExceptionStream')
        return 1

    _, rva = exc
    tid, _, code, flags, rec, addr, nparams = struct.unpack_from('<IIIIQQI', data, rva)
    params = struct.unpack_from('<15Q', data, rva + 0x28)

    print()
    print('=== EXCECAO ===')
    print(f'  thread id        : 0x{tid:X}')
    print(f'  exception code   : 0x{code:08X}')
    print(f'  exception flags  : 0x{flags:X}')
    print(f'  exception address: 0x{addr:016X}')
    print(f'  numero de params : {nparams}')
    for i in range(min(nparams, 4)):
        print(f'    param[{i}]       : 0x{params[i]:016X}')

    # ThreadContext fica no offset 0xA0 do ExceptionStream
    ctx_size, ctx_rva = struct.unpack_from('<II', data, rva + 0xA0)
    print()
    print(f'=== CONTEXT (size={ctx_size}, rva=0x{ctx_rva:X}) ===')

    if ctx_rva and ctx_size >= 0x100:
        # CONTEXT x64: ContextFlags 0x30, Rsp 0x98, Rip 0xF8
        rip = struct.unpack_from('<Q', data, ctx_rva + 0xF8)[0]
        rsp = struct.unpack_from('<Q', data, ctx_rva + 0x98)[0]
        rbp = struct.unpack_from('<Q', data, ctx_rva + 0xA0)[0]
        rax = struct.unpack_from('<Q', data, ctx_rva + 0x78)[0]
        rcx = struct.unpack_from('<Q', data, ctx_rva + 0x80)[0]
        rdx = struct.unpack_from('<Q', data, ctx_rva + 0x88)[0]
        rbx = struct.unpack_from('<Q', data, ctx_rva + 0x90)[0]
        rsi = struct.unpack_from('<Q', data, ctx_rva + 0xA8)[0]
        rdi = struct.unpack_from('<Q', data, ctx_rva + 0xB0)[0]
        r8  = struct.unpack_from('<Q', data, ctx_rva + 0xB8)[0]
        r9  = struct.unpack_from('<Q', data, ctx_rva + 0xC0)[0]

        for n, v in (('rip', rip), ('rsp', rsp), ('rbp', rbp),
                     ('rax', rax), ('rcx', rcx), ('rdx', rdx), ('rbx', rbx),
                     ('rsi', rsi), ('rdi', rdi), ('r8', r8), ('r9', r9)):
            print(f'  {n} = 0x{v:016X}')

    print()
    print('=== MODULOS CARREGADOS (para resolver os enderecos do stack) ===')
    ml = next(((s, r) for s, sz, r in sts if s == STREAM_MODULE_LIST), None)
    if ml:
        _, mrva = ml
        nmod = struct.unpack_from('<I', data, mrva)[0]
        print(f'  {nmod} modulos')
        mods = []
        for i in range(nmod):
            off = mrva + 4 + i * 108
            base = struct.unpack_from('<Q', data, off)[0]
            size = struct.unpack_from('<I', data, off + 8)[0]
            name_rva = struct.unpack_from('<I', data, off + 0x14)[0]
            if name_rva:
                nlen = struct.unpack_from('<I', data, name_rva)[0]
                nm = data[name_rva + 4:name_rva + 4 + nlen].decode('utf-16-le', 'replace')
                mods.append((base, size, nm))
        mods.sort()
        # so os que interessam: nt e qualquer coisa nao-Microsoft
        for base, size, nm in mods:
            if 'ntoskrnl' in nm.lower() or 'ntkrnl' in nm.lower() or \
               not nm.lower().startswith('c:\\windows\\system32'):
                print(f'    0x{base:016X}  {size:>9}  {nm}')
        print()
        print('  --- resolvendo o stack ---')
        if ctx_rva and ctx_size >= 0x100:
            # o stack nao esta mapeado no dump inteiro; procura a faixa que
            # contem rsp na MemoryList
            mem = next(((s, r) for s, sz, r in sts if s == STREAM_MEMORY_LIST), None)
            ranges = []
            if mem:
                _, r2 = mem
                n = struct.unpack_from('<I', data, r2)[0]
                for i in range(n):
                    off = r2 + 4 + i * 16
                    start = struct.unpack_from('<Q', data, off)[0]
                    msize = struct.unpack_from('<I', data, off + 8)[0]
                    mr = struct.unpack_from('<I', data, off + 12)[0]
                    ranges.append((start, msize, mr))
            print(f'    {len(ranges)} faixas de memoria no dump')

            def read_q(va):
                for start, msize, mr in ranges:
                    if start <= va < start + msize:
                        o = mr + (va - start)
                        if o + 8 <= len(data):
                            return struct.unpack_from('<Q', data, o)[0]
                return None

            def resolve(va):
                for base, size, nm in mods:
                    if base <= va < base + size:
                        short = nm.split('\\')[-1]
                        return f'{short}+0x{va - base:X}'
                return None

            rsp_val = struct.unpack_from('<Q', data, ctx_rva + 0x98)[0]
            print(f'    rsp = 0x{rsp_val:X}')
            for k in range(24):
                va = rsp_val + k * 8
                q = read_q(va)
                if q is None:
                    continue
                r = resolve(q)
                if r:
                    print(f'      [rsp+0x{k*8:02X}] 0x{q:016X}  {r}')

    return 0


if __name__ == '__main__':
    sys.exit(main())
