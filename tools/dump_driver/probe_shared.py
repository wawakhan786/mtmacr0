"""
probe_shared.py — testa o canal de memoria compartilhada (transporte seguro).

MUDOU (depois de 3 BSOD 0x1E):
    ANTES: o driver CRIava a secao (ZwCreateSection + security descriptor a
           mao -> o SD absoluto virava offsets e dereferenciava lixo).
    AGORA: o USER-MODE cria a secao com CreateFileMapping (Win32, a prova de
           erro) e o driver SO ABRE (ZwOpenSection + ZwMapViewOfSection).

Este probe:
    1. cria a secao com CreateFileMapping
    2. mapeia com MapViewOfFile
    3. espera o driver abrir e escrever o magic number
    4. roda o protocolo ReqSeq/AckSeq

NAO precisa do jogo aberto: GET_PID/GET_CR3/GET_MODULE e leitura de um buffer
NOSSO ja exercitam o caminho inteiro.

Uso:
    python probe_shared.py
"""

import ctypes
import ctypes.wintypes as w
import struct
import sys
import time

SECTION_NAME = "Global\\Ndu2Shared"

MAGIC = 0x4E647532
VERSION = 1

KM_OP_NOP = 0
KM_OP_READ = 1
KM_OP_WRITE = 2
KM_OP_GET_PID = 3
KM_OP_GET_MODULE = 4
KM_OP_GET_CR3 = 5

MAX_OPS = 64
MAX_PAYLOAD = 256 * 1024

# Layout (espelha KmShared.h com #pragma pack(push,8))
OP_SIZE = 128
REQ_OFF = 80
RESP_OFF = REQ_OFF + 8 + MAX_OPS * OP_SIZE   # 8280
DATA_OFF = RESP_OFF + 48                     # 8328
TOTAL = DATA_OFF + MAX_PAYLOAD               # 270472

# Usa um pouco mais que o minimo, para nao depender de um sizeof exato.
# O driver mapeia KM_SHARED_SIZE (== TOTAL); a secao maior nao atrapalha.
SECTION_SIZE = 1024 * 1024   # 1 MiB

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.CreateFileMappingA.restype = w.HANDLE
k32.CreateFileMappingA.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD, w.DWORD,
                                   w.DWORD, w.LPCSTR]
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.MapViewOfFile.argtypes = [w.HANDLE, w.DWORD, w.DWORD, w.DWORD, ctypes.c_size_t]
k32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
k32.CloseHandle.argtypes = [w.HANDLE]
k32.GetCurrentProcessId.restype = w.DWORD
k32.GetModuleFileNameA.argtypes = [w.HANDLE, ctypes.c_char_p, w.DWORD]

INVALID_HANDLE_VALUE = w.HANDLE(-1).value
PAGE_READWRITE = 0x04
FILE_MAP_ALL_ACCESS = 0x000F001F


def create_and_map():
    """Cria a secao (user-mode) e mapeia. Devolve (handle, view, erro)."""
    ctypes.set_last_error(0)
    h = k32.CreateFileMappingA(INVALID_HANDLE_VALUE, None, PAGE_READWRITE,
                               0, SECTION_SIZE, SECTION_NAME.encode('ascii'))
    if h in (0, INVALID_HANDLE_VALUE):
        return None, None, "CreateFileMappingA erro %d" % ctypes.get_last_error()

    view = k32.MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, SECTION_SIZE)
    if not view:
        err = ctypes.get_last_error()
        k32.CloseHandle(h)
        return None, None, "MapViewOfFile erro %d" % err

    return h, view, None


def u32(buf, off):
    return struct.unpack_from('<I', buf, off)[0]


def u64(buf, off):
    return struct.unpack_from('<Q', buf, off)[0]


def put32(addr, off, v):
    ctypes.memmove(addr + off, struct.pack('<I', v), 4)


def put64(addr, off, v):
    ctypes.memmove(addr + off, struct.pack('<Q', v), 8)


class Channel:
    def __init__(self, view):
        self.view = view
        self.seq = 0

    def raw(self):
        return ctypes.string_at(self.view, TOTAL)

    def transact(self, ops, payload=b'', timeout_ms=5000):
        self.seq += 1
        seq = self.seq

        if payload:
            ctypes.memmove(self.view + DATA_OFF, payload, len(payload))

        put32(self.view, 0, len(ops))          # Req.Count
        for i, op in enumerate(ops):
            base = REQ_OFF + 8 + i * OP_SIZE
            kind, pid, addr, size, payoff, name = op
            put32(self.view, base + 0, kind)
            put32(self.view, base + 4, pid)
            put64(self.view, base + 8, addr)
            put64(self.view, base + 16, size)
            put64(self.view, base + 24, payoff)
            if name:
                nb = name.encode('ascii')[:63]
                ctypes.memmove(self.view + base + 32, nb, len(nb))

        put32(self.view, 8, seq)               # ReqSeq

        t0 = time.time()
        while (time.time() - t0) * 1000 < timeout_ms:
            ack = u32(self.raw(), 12)
            busy = u32(self.raw(), 16)
            if ack >= seq and busy == 0:
                return self.raw()
            time.sleep(0.0005)
        return None

    def read_resp(self, raw):
        cnt = u32(raw, RESP_OFF + 0)
        used = u64(raw, RESP_OFF + 8)
        return cnt, used


passou = 0
falhou = 0


def report(nome, ok, det):
    global passou, falhou
    if ok:
        passou += 1
        print('  [+] %-34s %s' % (nome, det))
    else:
        falhou += 1
        print('  [-] %-34s %s' % (nome, det))


def main():
    print('=' * 72)
    print('  CANAL SEGURO (user-mode cria, driver abre) — %s' % SECTION_NAME)
    print('=' * 72)
    print()

    h, view, err = create_and_map()
    if err:
        print('  [X] %s' % err)
        return 1
    print('  secao criada e mapeada em 0x%X' % view)

    # ------------------------------------------------------------------
    # Espera o driver abrir e escrever o magic number.
    # O driver tenta abrir a cada 500 ms por ate 2 min.
    # ------------------------------------------------------------------
    print('  aguardando o driver abrir a secao (magic number)...')
    t0 = time.time()
    while (time.time() - t0) < 60:
        if u32(ctypes.string_at(view, TOTAL), 0) == MAGIC:
            break
        time.sleep(0.2)
    else:
        print('  [X] timeout: o driver nao abriu a secao em 60 s.')
        print('      verifique C:\\Windows\\Ndu_debug.txt (procure "shared: ABERTO").')
        return 1

    raw = ctypes.string_at(view, TOTAL)
    ver = u32(raw, 4)
    report('magic/version', u32(raw, 0) == MAGIC and ver == VERSION,
           'magic=0x%08X ver=%u' % (u32(raw, 0), ver))

    ch = Channel(view)

    mypid = k32.GetCurrentProcessId()
    buf = ctypes.create_string_buffer(260)
    k32.GetModuleFileNameA(None, buf, 260)
    nome = buf.value.split(b'\\')[-1].decode('ascii')

    # ---- GET_PID ------------------------------------------------------
    raw = ch.transact([(KM_OP_GET_PID, 0, 0, 0, 0, nome)])
    if raw is None:
        report('transacao GET_PID', False, 'timeout: o driver nao respondeu')
    else:
        cnt, used = ch.read_resp(raw)
        o0 = u64(raw, REQ_OFF + 8 + 0 * OP_SIZE + 80)
        st = u32(raw, REQ_OFF + 8 + 0 * OP_SIZE + 76)
        report('GET_PID (%s)' % nome, cnt >= 1 and o0 == mypid,
               'status=0x%08X pid=%d (nosso=%d)' % (st, o0, mypid))

    # ---- GET_CR3 ------------------------------------------------------
    raw = ch.transact([(KM_OP_GET_CR3, mypid, 0, 0, 0, '')])
    if raw is None:
        report('transacao GET_CR3', False, 'timeout')
    else:
        o0 = u64(raw, REQ_OFF + 8 + 80)
        st = u32(raw, REQ_OFF + 8 + 76)
        report('GET_CR3', o0 != 0, 'status=0x%08X cr3=0x%X' % (st, o0))

    # ---- GET_MODULE ---------------------------------------------------
    raw = ch.transact([(KM_OP_GET_MODULE, mypid, 0, 0, 0, nome)])
    if raw is None:
        report('transacao GET_MODULE', False, 'timeout')
    else:
        o0 = u64(raw, REQ_OFF + 8 + 80)
        o1 = u64(raw, REQ_OFF + 8 + 88)
        st = u32(raw, REQ_OFF + 8 + 76)
        report('GET_MODULE (%s)' % nome, o0 != 0,
               'status=0x%08X base=0x%X size=0x%X' % (st, o0, o1))

    # ---- READ de um buffer nosso --------------------------------------
    tamanho = 0x1000
    alvo = ctypes.create_string_buffer(tamanho)
    padrao = bytes(((i * 7 + 0x5A) & 0xFF) for i in range(tamanho))
    ctypes.memmove(alvo, padrao, tamanho)
    end = ctypes.cast(alvo, ctypes.c_void_p).value

    raw = ch.transact([(KM_OP_READ, mypid, end, tamanho, 0, '')])
    if raw is None:
        report('transacao READ', False, 'timeout')
    else:
        o0 = u64(raw, REQ_OFF + 8 + 80)
        st = u32(raw, REQ_OFF + 8 + 76)
        dados = raw[DATA_OFF + o0: DATA_OFF + o0 + tamanho]
        report('READ_MEMORY 0x1000',
               (st & 0x80000000) == 0 and dados == padrao,
               'status=0x%08X  %s' % (st, 'IDENTICO' if dados == padrao else 'DIVERGE'))

    # ---- BATCH: varias leituras numa volta ----------------------------
    ops = []
    esperados = []
    bufs = []
    for i in range(8):
        b = ctypes.create_string_buffer(256)
        p = bytes(((j + i * 13) & 0xFF) for j in range(256))
        ctypes.memmove(b, p, 256)
        bufs.append(b)
        esperados.append(p)
        ops.append((KM_OP_READ, mypid, ctypes.cast(b, ctypes.c_void_p).value, 256, 0, ''))
    raw = ch.transact(ops)
    if raw is None:
        report('transacao BATCH (8 ops)', False, 'timeout')
    else:
        cnt, used = ch.read_resp(raw)
        okall = True
        for i in range(8):
            o0 = u64(raw, REQ_OFF + 8 + i * OP_SIZE + 80)
            if raw[DATA_OFF + o0: DATA_OFF + o0 + 256] != esperados[i]:
                okall = False
        report('BATCH 8 leituras numa volta', cnt == 8 and okall,
               'count=%d used=%d  %s' % (cnt, used, 'todas ok' if okall else 'DIVERGE'))

    # ---- recusa esperada ---------------------------------------------
    raw = ch.transact([(KM_OP_READ, mypid, 0x1, 16, 0, '')])
    if raw is None:
        report('endereco 0x1 recusado', False, 'timeout')
    else:
        st = u32(raw, REQ_OFF + 8 + 76)
        report('endereco 0x1 recusado', (st & 0x80000000) != 0,
               'status=0x%08X' % st)

    # ---- estado do alvo ----------------------------------------------
    raw = ctypes.string_at(view, TOTAL)
    tpid = u64(raw, RESP_OFF + 16)
    tbase = u64(raw, RESP_OFF + 32)
    treq = u64(raw, 24)
    print('\n  --- estado ---')
    print('      pedidos atendidos : %d' % treq)
    print('      alvo (TslGame.exe): pid=%d base=0x%X' % (tpid, tbase))
    if tpid == 0:
        print('      (jogo fechado — normal. O canal ja esta provado.)')

    k32.UnmapViewOfFile(view)
    k32.CloseHandle(h)

    print()
    print('=' * 72)
    print('  RESULTADO: %d passaram, %d falharam' % (passou, falhou))
    print('=' * 72)
    if falhou == 0:
        print('  *** CANAL FUNCIONANDO ***  sem device, sem SD, sem criar objeto.')
    else:
        print('  Ha falha. Ver as linhas [-] acima.')
    return 0 if falhou == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
