"""
analyze_donor.py — disseca um .sys e diz se ele serve como doador BYOVD.

Junta o que os outros scripts fazem, num relatorio so:

  1. assinatura Authenticode
  2. imports de kernel com potencial de primitivo (peso por categoria)
  3. IOCTLs: varre o padrao do switch (81 F9 imm32 + 0F 84 rel32)
  4. handlers de dispatch (MajorFunction) — onde o driver trata CREATE/IOCTL
  5. se o driver valida o chamador (busca APIs de seguranca)

Uso:
    python analyze_donor.py <arquivo.sys>
"""

import argparse
import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_probe import PE                      # noqa: E402
from xref_import import parse_pe, list_imports, find_calls_to_slot  # noqa: E402

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

# peso, categoria
PRIMITIVES = {
    'MmCopyVirtualMemory':       (100, 'VMMEM'),
    'KeStackAttachProcess':      ( 80, 'VMMEM'),
    'ZwOpenProcess':             ( 40, 'PROC'),
    'NtOpenProcess':             ( 40, 'PROC'),
    'PsLookupProcessByProcessId':( 35, 'PROC'),
    'PsGetProcessImageFileName': ( 10, 'PROC'),
    'MmMapIoSpace':              ( 55, 'PHYS'),
    'MmMapIoSpaceEx':            ( 55, 'PHYS'),
    'MmMapMemoryDumpMdl':        ( 45, 'PHYS'),
    'MmGetPhysicalAddress':      ( 25, 'PHYS'),
    'MmGetVirtualForPhysical':   ( 30, 'PHYS'),
    'MmAllocateContiguousMemory':( 10, 'PHYS'),
    'MmMapLockedPagesSpecifyCache': (40, 'MDL'),
    'MmProbeAndLockPages':       ( 20, 'MDL'),
    'IoAllocateMdl':             ( 15, 'MDL'),
    'MmBuildMdlForNonPagedPool': ( 15, 'MDL'),
    'ZwMapViewOfSection':        ( 30, 'MDL'),
    'ZwOpenSection':             ( 20, 'MDL'),
    '__readmsr':                 ( 40, 'MSR'),
    '__writemsr':                ( 30, 'MSR'),
    'KeReadMsr':                 ( 40, 'MSR'),
    'KeWriteMsr':                ( 30, 'MSR'),
    '__inbyte':                  ( 25, 'PORT'),
    '__outbyte':                 ( 20, 'PORT'),
    '__indword':                 ( 25, 'PORT'),
    '__outdword':                ( 20, 'PORT'),
    'HalGetBusDataByOffset':     ( 20, 'PCI'),
    'HalSetBusDataByOffset':     ( 15, 'PCI'),
    'ObRegisterCallbacks':       (-15, 'NOISE'),
    'PsSetCreateProcessNotifyRoutine':   (-15, 'NOISE'),
    'PsSetCreateProcessNotifyRoutineEx': (-15, 'NOISE'),
    'PsSetLoadImageNotifyRoutine':       (-15, 'NOISE'),
    'CmRegisterCallback':        (-15, 'NOISE'),
    'CmRegisterCallbackEx':      (-15, 'NOISE'),
    'IoCreateDevice':            ( 10, 'IOCTL'),
    'IoCreateSymbolicLink':      ( 10, 'IOCTL'),
    'IoCreateDriver':            ( 10, 'IOCTL'),
}

# APIs que sinalizam validacao de chamador
VALIDACAO = ['SeAccessCheck', 'SeSinglePrivilegeCheck', 'SeExports',
             'PsReferencePrimaryToken', 'SeQueryInformationToken',
             'SeImpersonateClient', 'ZwQueryInformationToken',
             'RtlCompareUnicodeString', 'RtlEqualUnicodeString',
             '_stricmp', 'wcsstr', 'PsGetProcessImageFileName',
             'ZwQueryInformationProcess', 'SeTokenType',
             'ObOpenObjectByPointer']


def sign(path):
    try:
        ps = ('$s = Get-AuthenticodeSignature -LiteralPath '
              f"'{path}'; Write-Output ($s.Status.ToString() + '|' + "
              '$(if ($s.SignerCertificate) { $s.SignerCertificate.Subject } else { "" }))')
        r = subprocess.run(['powershell', '-NoProfile', '-Command', ps],
                           capture_output=True, text=True, timeout=60)
        out = r.stdout.strip()
        if '|' in out:
            st, subj = out.split('|', 1)
            m = re.search(r'CN=([^,]+)', subj)
            return st.strip(), (m.group(1) if m else subj.strip()[:40])
    except Exception:
        pass
    return '?', '?'


def find_ioctls(data, pe):
    """Switch de IOCTL: cmp ecx, imm32 (81 F9) seguido de je/jne (0F 84/85)."""
    out = {}
    text = next((s for s in pe['sections'] if s['name'] == '.text'), None)
    if not text:
        return out
    blob = data[text['ptr']:text['ptr'] + text['size']]
    base = pe['base'] + text['va']
    for i in range(len(blob) - 10):
        if blob[i] == 0x81 and blob[i + 1] == 0xF9:
            imm = struct.unpack_from('<I', blob, i + 2)[0]
            if 0x00020000 <= imm <= 0x00FFFFFF and blob[i + 6] in (0x0F,):
                out.setdefault(imm, []).append(base + i)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    args = ap.parse_args()

    path = os.path.abspath(args.path)
    data = open(path, 'rb').read()
    pe = parse_pe(data)
    if not pe:
        print('nao e um PE valido')
        return 1

    st, subj = sign(path)

    print('=' * 74)
    print(os.path.basename(path))
    print('=' * 74)
    print(f'  caminho   : {path}')
    print(f'  tamanho   : {len(data)} bytes')
    print(f'  assinatura: {st}   de: {subj}')

    try:
        peo = PE(path)
        print(f'  subsystem : {peo.subsystem} (1 = native/driver)')
    except Exception:
        pass
    print()

    imports = list_imports(data, pe)
    flat = [k.split('!')[-1] for k in imports]

    # --- primitivos ------------------------------------------------------
    score = 0
    cats = {}
    prims = []
    for f in flat:
        if f in PRIMITIVES:
            w, c = PRIMITIVES[f]
            score += w
            cats[c] = cats.get(c, 0) + 1
            prims.append((f, w, c))

    print(f'  score de doador: {score}')
    if cats:
        print('  categorias     : ' + '  '.join(f'{k}:{v}' for k, v in sorted(cats.items())))
    prims.sort(key=lambda x: -x[1])
    print()
    if prims:
        print('  --- primitivos (peso) ---')
        for f, w, c in prims:
            print(f'    {w:>4}  {c:<6} {f}')
    else:
        print('  nenhum primitivo conhecido')

    # --- validacao de chamador -------------------------------------------
    print()
    val = [f for f in flat if f in VALIDACAO]
    if val:
        print('  --- VALIDACAO DE CHAMADOR (obstaculo) ---')
        for f in sorted(set(val)):
            calls = find_calls_to_slot(data, pe, imports.get(f'ntoskrnl.exe!{f}', 0))
            print(f'    {f:<34} chamadas diretas: {len(calls)}')
    else:
        print('  --- nenhuma API de validacao de chamador ---')

    # --- IOCTLs ----------------------------------------------------------
    ioctls = find_ioctls(data, pe)
    print()
    print(f'  --- IOCTLs (padrao cmp ecx,imm32) : {len(ioctls)} ---')
    for v in sorted(ioctls):
        dev = (v >> 16) & 0xFFFF
        fn = (v >> 2) & 0xFFF
        method = ['BUFFERED', 'IN_DIRECT', 'OUT_DIRECT', 'NEITHER'][v & 3]
        print(f'    0x{v:08X}  dev=0x{dev:04X} func=0x{fn:03X} {method}')

    print()
    print('=' * 74)
    return 0


if __name__ == '__main__':
    sys.exit(main())
