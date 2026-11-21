"""
probe_driver.py — testa o Ndu2 direto, sem precisar do RuntimeBroker.

Por que existe: o RuntimeBroker leva uma rodada do usuario (e abre janela)
para reportar. Isto aqui bate nos IOCTLs direto e diz na hora.

NAO usar -replace/regex neste arquivo: ele JA foi corrompido uma vez por um
`-replace` mal escapado do PowerShell que encheu o arquivo de espacos.

Layouts de PubgExt\driver\common.h (#pragma pack(push, 8)):

  KM_REQUEST         size 32   Pid@0 Pad@4 Address@8 Size@16 Status@24
  KM_MODULE_REQUEST  size 296  Pid@0 Pad@4 Name[260]@8 Base@272 Size@280 Status@288
  KM_PID_REQUEST     size 268  Name[260]@0 Pid@260 Status@264
  KM_CR3_REQUEST     size 24   Pid@0 Pad@4 Cr3@8 Status@16

REGRA DO CONTRATO: o driver valida `inLen >= sizeof(KM_*_REQUEST)`. O
tamanho reportado ao DeviceIoControl tem que ser o TAMANHO DA STRUCT, nao o
quanto foi preenchido. Mas o buffer alocado precisa caber o payload inteiro
(nos IOCTLs de leitura o payload vem depois do header).

Uso:
    python probe_driver.py
"""

import ctypes
import ctypes.wintypes as w
import struct
import sys

DEVICE = rb'\\.\Ndu2'

FILE_DEVICE_UNKNOWN = 0x22


def CTL_CODE(dev, func, method, access):
    return (dev << 16) | (access << 14) | (func << 2) | method


IOCTL_READ_MEMORY     = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, 0, 0)
IOCTL_GET_MODULE_BASE = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, 0, 0)
IOCTL_GET_PID         = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, 0, 0)
IOCTL_WRITE_MEMORY    = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, 0, 0)
IOCTL_GET_CR3         = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, 0, 0)
IOCTL_READ_VIRT_PHYS  = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, 0, 0)

SZ_REQUEST = 32
SZ_MODULE  = 296
SZ_PID     = 268
SZ_CR3     = 24

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.CreateFileA.restype = w.HANDLE
k32.CreateFileA.argtypes = [w.LPCSTR, w.DWORD, w.DWORD, ctypes.c_void_p,
                            w.DWORD, w.DWORD, w.HANDLE]
k32.DeviceIoControl.argtypes = [w.HANDLE, w.DWORD, ctypes.c_void_p, w.DWORD,
                                ctypes.c_void_p, w.DWORD,
                                ctypes.POINTER(w.DWORD), ctypes.c_void_p]
k32.GetModuleFileNameA.argtypes = [w.HANDLE, ctypes.c_char_p, w.DWORD]

passou = 0
falhou = 0


def report(nome, ok, det):
    global passou, falhou
    if ok:
        passou += 1
        print('  [+] %-32s %s' % (nome, det))
    else:
        falhou += 1
        print('  [-] %-32s %s' % (nome, det))


def NT_SUCCESS(s):
    return (s & 0x80000000) == 0


def open_device():
    for acesso in (0xC0000000, 0x80000000, 0):
        ctypes.set_last_error(0)
        h = k32.CreateFileA(DEVICE, acesso, 0, None, 3, 0, None)
        if h not in (w.HANDLE(-1).value, None):
            return h
    return None


def ioctl(h, code, inbuf, outsize, insize=None):
    if insize is None:
        insize = len(inbuf)
    aloc = max(len(inbuf), insize, 1)
    inb = ctypes.create_string_buffer(bytes(inbuf), aloc)
    out = ctypes.create_string_buffer(outsize)
    ret = w.DWORD(0)
    ok = k32.DeviceIoControl(h, code, inb, insize, out, outsize,
                             ctypes.byref(ret), None)
    return bool(ok), out.raw[:ret.value], ret.value


def main():
    print('=' * 70)
    print('  SONDA DIRETA DO DRIVER - device %s' % DEVICE.decode())
    print('=' * 70)
    print()

    h = open_device()
    if h is None:
        e = k32.GetLastError()
        print('  [X] nao abri o device (erro %d)' % e)
        if e == 2:
            print('      o symlink nao existe. O driver carregou?')
            print('      veja C:\\Windows\\Ndu_debug.txt')
        elif e == 5:
            print('      existe mas negou acesso')
        return 1
    print('  device aberto.\n')

    mypid = k32.GetCurrentProcessId()
    buf = ctypes.create_string_buffer(260)
    k32.GetModuleFileNameA(None, buf, 260)
    nome = buf.value.split(b'\\')[-1]

    # ---- GET_CR3 -----------------------------------------------------
    print('  --- GET_CR3 ---')
    req = struct.pack('<IIQ', mypid, 0, 0)
    req += b'\0' * (SZ_CR3 - len(req))
    ok, out, ret = ioctl(h, IOCTL_GET_CR3, req, SZ_CR3, SZ_CR3)
    if not ok:
        report('GET_CR3', False, 'DeviceIoControl falhou: %d' % k32.GetLastError())
    else:
        cr3 = struct.unpack_from('<Q', out, 8)[0]
        st = struct.unpack_from('<I', out, 16)[0]
        report('GET_CR3', NT_SUCCESS(st) and cr3 != 0,
               'status=0x%08X cr3=0x%016X' % (st, cr3))

    # ---- GET_PID -----------------------------------------------------
    print('\n  --- GET_PID (nos mesmos) ---')
    pkt = nome + b'\0' * (260 - len(nome))
    pkt += struct.pack('<II', 0, 0)
    pkt = pkt[:SZ_PID]
    ok, out, ret = ioctl(h, IOCTL_GET_PID, pkt, SZ_PID, SZ_PID)
    if not ok:
        report('GET_PID (%s)' % nome.decode(), False,
               'DeviceIoControl falhou: %d' % k32.GetLastError())
    else:
        pid_r = struct.unpack_from('<I', out, 260)[0]
        st = struct.unpack_from('<I', out, 264)[0]
        report('GET_PID (%s)' % nome.decode(),
               NT_SUCCESS(st) and pid_r == mypid,
               'status=0x%08X pid=%d (nosso=%d)' % (st, pid_r, mypid))

    # ---- GET_MODULE_BASE ---------------------------------------------
    # Exige outLen EXATAMENTE sizeof(KM_MODULE_REQUEST): com folga ele da
    # STATUS_BUFFER_TOO_SMALL (erro 122).
    print('\n  --- GET_MODULE_BASE ---')
    pkt = struct.pack('<II', mypid, 0)
    pkt += nome + b'\0' * (260 - len(nome))
    pkt += struct.pack('<QQI', 0, 0, 0)
    pkt = pkt[:SZ_MODULE]
    ok, out, ret = ioctl(h, IOCTL_GET_MODULE_BASE, pkt, SZ_MODULE, SZ_MODULE)
    if not ok:
        report('GET_MODULE_BASE', False, 'falhou: %d' % k32.GetLastError())
    else:
        base = struct.unpack_from('<Q', out, 272)[0]
        size = struct.unpack_from('<Q', out, 280)[0]
        st = struct.unpack_from('<I', out, 288)[0]
        report('GET_MODULE_BASE', NT_SUCCESS(st) and base != 0,
               'status=0x%08X base=0x%X size=0x%X' % (st, base, size))

    # ---- READ_MEMORY -------------------------------------------------
    # Le de um buffer NOSSO com padrao conhecido. Nao le o proprio modulo:
    # neste python.exe o GetModuleHandleA devolve um base que o processo nao
    # consegue ler (access violation no proprio memcpy, nada a ver com o
    # driver).
    tamanho = 0x1000
    alvo = ctypes.create_string_buffer(tamanho)
    padrao = bytes(((i * 7 + 0x5A) & 0xFF) for i in range(tamanho))
    ctypes.memmove(alvo, padrao, tamanho)
    endereco = ctypes.cast(alvo, ctypes.c_void_p).value
    print('\n  buffer nosso em 0x%X, 0x%X bytes de padrao conhecido'
          % (endereco, tamanho))

    def le(code, rotulo):
        req = struct.pack('<QQQ', mypid, endereco, tamanho)
        req += struct.pack('<I', 0)
        req += b'\0' * (SZ_REQUEST - len(req))
        ok, out, ret = ioctl(h, code, req + b'\0' * tamanho,
                             SZ_REQUEST + tamanho, SZ_REQUEST)
        if not ok:
            report(rotulo, False, 'falhou: %d' % k32.GetLastError())
            return
        st = struct.unpack_from('<I', out, 24)[0]
        payload = out[SZ_REQUEST:SZ_REQUEST + tamanho]
        igual = payload == padrao
        report(rotulo, NT_SUCCESS(st) and igual,
               'status=0x%08X  %s' % (st, 'IDENTICO' if igual else 'DIVERGE'))

    print('\n  --- leitura virtual de 0x1000 bytes ---')
    le(IOCTL_READ_MEMORY, 'READ_MEMORY (MmCopyVirtualMemory)')
    le(IOCTL_READ_VIRT_PHYS, 'READ_VIRT_PHYS (page walk)')

    # ---- WRITE_MEMORY ------------------------------------------------
    # inLen = header + payload (o write leva o payload junto).
    print('\n  --- WRITE_MEMORY ---')
    dst = ctypes.create_string_buffer(b'A' * 64, 64)
    addr = ctypes.cast(dst, ctypes.c_void_p).value
    marcador = b'DSHPROBE' * 8
    req = struct.pack('<QQQ', mypid, addr, len(marcador))
    req += struct.pack('<I', 0)
    req += b'\0' * (SZ_REQUEST - len(req))
    ok, out, ret = ioctl(h, IOCTL_WRITE_MEMORY, req + marcador, SZ_REQUEST,
                         SZ_REQUEST + len(marcador))
    if not ok:
        report('WRITE_MEMORY', False, 'falhou: %d' % k32.GetLastError())
    else:
        st = struct.unpack_from('<I', out, 24)[0]
        igual = dst.raw[:len(marcador)] == marcador
        report('WRITE_MEMORY', NT_SUCCESS(st) and igual,
               'status=0x%08X  %s'
               % (st, 'escreveu certo' if igual else 'nao escreveu'))

    # ---- recusa esperada ---------------------------------------------
    print('\n  --- recusa esperada ---')
    req = struct.pack('<QQQ', mypid, 0x1, 16)
    req += struct.pack('<I', 0)
    req += b'\0' * (SZ_REQUEST - len(req))
    ok, out, ret = ioctl(h, IOCTL_READ_MEMORY, req + b'\0' * 16,
                         SZ_REQUEST + 16, SZ_REQUEST)
    if ok:
        st = struct.unpack_from('<I', out, 24)[0]
        report('endereco 0x1 recusado', not NT_SUCCESS(st), 'status=0x%08X' % st)
    else:
        report('endereco 0x1 recusado', True, 'recusado pelo driver')

    k32.CloseHandle(h)

    print()
    print('=' * 70)
    print('  RESULTADO: %d passaram, %d falharam' % (passou, falhou))
    print('=' * 70)
    if falhou == 0:
        print('  *** O DRIVER FUNCIONA ***  le virtual, fisico e CR3.')
    else:
        print('  Ha falha. Ver as linhas [-] acima.')
    return 0 if falhou == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
