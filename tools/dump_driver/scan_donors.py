#!/usr/bin/env python3
"""
scan_donors.py — varre drivers instalados na maquina e ranqueia por
"utilidade como doador BYOVD".

O que ele faz:
  1. enumera .sys em System32\\drivers (e opcionalmente em Program Files)
  2. checa a assinatura Authenticode via PowerShell Get-AuthenticodeSignature
  3. faz parse do PE e lista as APIs de kernel que o driver importa
  4. pontua cada driver pelo potencial de primitivo:
       - leitura/escrita de memoria virtual  (MmCopyVirtualMemory, attach)
       - mapeamento fisico                   (MmMapIoSpace, MmMapMemoryDumpMdl)
       - MSR / I/O port                      (__readmsr, __inbyte, ..)
       - acesso a processo                   (ZwOpenProcess, PsLookupProcess)
       - callbacks de notificacao            (sinal de driver "vivo"/detectavel)
  5. imprime um ranking

Uso:
    python scan_donors.py
    python scan_donors.py --roots "C:\\Windows\\System32\\drivers" "C:\\Program Files"
    python scan_donors.py --json out.json
"""

import argparse
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_probe import PE  # noqa: E402

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

# ---------------------------------------------------------------------------
# Pesos
# ---------------------------------------------------------------------------
# (nome do import, peso, categoria)
PRIMITIVES = {
    # leitura/escrita de memoria virtual — o primitivo ideal
    'MmCopyVirtualMemory':              (100, 'VMMEM'),
    'KeStackAttachProcess':             ( 80, 'VMMEM'),
    'KeUnstackDetachProcess':           (  5, 'VMMEM'),
    'ZwOpenProcess':                    ( 40, 'PROC'),
    'NtOpenProcess':                    ( 40, 'PROC'),
    'PsLookupProcessByProcessId':       ( 35, 'PROC'),
    'PsGetProcessImageFileName':        ( 10, 'PROC'),
    'PsGetCurrentProcessId':            (  5, 'PROC'),

    # memoria fisica — permite page-table walk (mais trabalho)
    'MmMapIoSpace':                     ( 55, 'PHYS'),
    'MmMapIoSpaceEx':                   ( 55, 'PHYS'),
    'MmMapMemoryDumpMdl':               ( 45, 'PHYS'),
    'MmGetPhysicalAddress':             ( 25, 'PHYS'),
    'MmGetVirtualForPhysical':          ( 30, 'PHYS'),
    'MmAllocateContiguousMemory':       ( 10, 'PHYS'),

    # MDL / secao — outro caminho para memoria
    'MmMapLockedPagesSpecifyCache':     ( 40, 'MDL'),
    'MmProbeAndLockPages':              ( 20, 'MDL'),
    'IoAllocateMdl':                    ( 15, 'MDL'),
    'MmBuildMdlForNonPagedPool':        ( 15, 'MDL'),
    'ZwMapViewOfSection':               ( 30, 'MDL'),
    'ZwOpenSection':                    ( 20, 'MDL'),

    # registradores / portas — primitivo de "leitura arbitraria" so com
    # page-table walk, mas muito comum em driver de tuning
    '__readmsr':                        ( 40, 'MSR'),
    '__writemsr':                       ( 30, 'MSR'),
    'KeReadMsr':                        ( 40, 'MSR'),
    'KeWriteMsr':                       ( 30, 'MSR'),
    '__inbyte':                         ( 25, 'PORT'),
    '__outbyte':                        ( 20, 'PORT'),
    '__indword':                        ( 25, 'PORT'),
    '__outdword':                       ( 20, 'PORT'),
    'HalGetBusDataByOffset':            ( 20, 'PCI'),
    'HalSetBusDataByOffset':            ( 15, 'PCI'),

    # "vivo" — callback registrado = mais visivel para anti-cheat
    'ObRegisterCallbacks':              (-15, 'NOISE'),
    'PsSetCreateProcessNotifyRoutine':  (-15, 'NOISE'),
    'PsSetCreateProcessNotifyRoutineEx':(-15, 'NOISE'),
    'PsSetLoadImageNotifyRoutine':      (-15, 'NOISE'),
    'CmRegisterCallback':               (-15, 'NOISE'),
    'CmRegisterCallbackEx':             (-15, 'NOISE'),
    'IoCreateDevice':                   ( 10, 'IOCTL'),
    'IoCreateSymbolicLink':             ( 10, 'IOCTL'),
    'IoCreateDriver':                   ( 10, 'IOCTL'),
    'ZwOpenFile':                       (  5, 'MISC'),
    'ZwReadFile':                       (  5, 'MISC'),
    'ZwOpenKey':                        (  5, 'MISC'),
    'ZwQueryValueKey':                  (  5, 'MISC'),
    '__writecr3':                       ( 50, 'CR3'),
}


def sig_status(paths):
    """Assinatura Authenticode em lote (uma chamada PowerShell por lote)."""
    out = {}
    batch = 40
    for i in range(0, len(paths), batch):
        chunk = paths[i:i + batch]
        quoted = ','.join("'" + p.replace("'", "''") + "'" for p in chunk)
        ps = (
            f"$r = Get-AuthenticodeSignature -FilePath @({quoted});"
            "foreach ($s in $r) {"
            "  $subj = if ($s.SignerCertificate) { $s.SignerCertificate.Subject } else { '' };"
            "  Write-Output ($s.Path + '|' + $s.Status + '|' + $subj)"
            "}"
        )
        try:
            cp = subprocess.run(
                ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', ps],
                capture_output=True, text=True, timeout=300,
                encoding='utf-8', errors='replace')
            for line in (cp.stdout or '').splitlines():
                parts = line.split('|', 2)
                if len(parts) == 3:
                    out[parts[0]] = (parts[1], parts[2])
        except Exception as e:
            print(f'  [warn] assinatura lote {i}: {e}', file=sys.stderr)
    return out


def analyse(path):
    try:
        pe = PE(path)
    except Exception:
        return None

    apis = []
    score = 0
    cats = {}
    for dll, funcs in pe.imports.items():
        for f in funcs:
            apis.append(f)
            w, cat = PRIMITIVES.get(f, (0, None))
            if w:
                score += w
                cats[cat] = cats.get(cat, 0) + 1

    return {
        'path': path,
        'name': os.path.basename(path),
        'size': len(pe.data),
        'machine': pe.machine,
        'subsystem': pe.subsystem,
        'entry': pe.entry,
        'timestamp': pe.timestamp,
        'apis': sorted(set(apis)),
        'score': score,
        'cats': cats,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--roots', nargs='*', default=[
        r'C:\Windows\System32\drivers',
        r'C:\Windows\System32\DriverStore\FileRepository',
    ])
    ap.add_argument('--json')
    ap.add_argument('--min-score', type=int, default=1)
    args = ap.parse_args()

    files = []
    for root in args.roots:
        if not os.path.isdir(root):
            continue
        for dirpath, _dirnames, filenames in os.walk(root):
            for fn in filenames:
                if fn.lower().endswith('.sys'):
                    files.append(os.path.join(dirpath, fn))

    files = sorted(set(files))
    print(f'[i] {len(files)} arquivos .sys encontrados em {len(args.roots)} raiz(es)')

    print('[i] assinando (Authenticode)...')
    sigs = sig_status(files)
    valid = [f for f in files if sigs.get(f, ('', ''))[0] == 'Valid']
    print(f'[i] {len(valid)} com assinatura Valid')

    print('[i] analisando imports...')
    results = []
    with ThreadPoolExecutor(max_workers=8) as ex:
        for r in ex.map(analyse, valid):
            if r:
                results.append(r)

    results.sort(key=lambda r: -r['score'])

    print()
    print('=' * 100)
    print('RANKING DE DOADORES (por primitivo de kernel)')
    print('=' * 100)
    shown = 0
    for r in results:
        if r['score'] < args.min_score:
            continue
        shown += 1
        signer = sigs.get(r['path'], ('', ''))[1]
        signer = signer.split(',')[0].replace('CN=', '')[:44]
        cats = ' '.join(f'{k}:{v}' for k, v in sorted(r['cats'].items()))
        print(f"\n[{r['score']:>4}] {r['name']}")
        print(f"        {r['path']}")
        print(f"        signer: {signer}   size: {r['size']}")
        print(f"        categorias: {cats}")
        prim = [a for a in r['apis'] if a in PRIMITIVES and PRIMITIVES[a][0] > 0]
        print(f"        primitivos: {', '.join(sorted(prim))}")

    print(f'\n[i] {shown} drivers com score >= {args.min_score}')

    if args.json:
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump({'results': results, 'sigs': sigs}, f, indent=2)
        print(f'[i] json salvo em {args.json}')


if __name__ == '__main__':
    main()
