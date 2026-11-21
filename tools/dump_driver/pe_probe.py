#!/usr/bin/env python3
"""
pe_probe.py — analise estatica basica de um PE (foco em drivers .sys).

Sem dependencias externas. Le:
  - headers (machine, subsystem, entry point, imagem base, DllCharacteristics)
  - secao por secao
  - diretorio de import (DLL + nomes de funcao)
  - diretorio de export (raramente usado em .sys)
  - strings ASCII/UTF-16 uteis
  - varredura por constantes que parecem IOCTL (0x0002xxxx / 0x0003xxxx ...)

Uso:
    python pe_probe.py <arquivo.sys> [--strings] [--ioctl]
"""

import struct
import sys
import re
import argparse
from collections import OrderedDict

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

# --------------------------------------------------------------------------
# PE
# --------------------------------------------------------------------------

MACHINE = {
    0x014C: 'I386',
    0x8664: 'AMD64',
    0xAA64: 'ARM64',
    0x01C4: 'ARMNT',
}

SUBSYSTEM = {
    0: 'UNKNOWN',
    1: 'NATIVE',      # driver
    2: 'WINDOWS_GUI',
    3: 'WINDOWS_CUI',
    9: 'WINDOWS_CE_GUI',
    10: 'EFI_APPLICATION',
    11: 'EFI_BOOT_SERVICE_DRIVER',
    12: 'EFI_RUNTIME_DRIVER',
    13: 'EFI_ROM',
    14: 'XBOX',
    16: 'WINDOWS_BOOT_APPLICATION',
}

DLLCHAR = [
    (0x0020, 'HIGH_ENTROPY_VA'),
    (0x0040, 'DYNAMIC_BASE'),
    (0x0080, 'FORCE_INTEGRITY'),
    (0x0100, 'NX_COMPAT'),
    (0x0200, 'NO_ISOLATION'),
    (0x0400, 'NO_SEH'),
    (0x0800, 'NO_BIND'),
    (0x1000, 'APPCONTAINER'),
    (0x2000, 'WDM_DRIVER'),
    (0x4000, 'GUARD_CF'),
    (0x8000, 'TERMINAL_SERVER_AWARE'),
]


class Section:
    def __init__(self, name, vaddr, vsize, raw_off, raw_size, chars):
        self.name = name
        self.vaddr = vaddr
        self.vsize = vsize
        self.raw_off = raw_off
        self.raw_size = raw_size
        self.chars = chars

    def __repr__(self):
        return (f'<Section {self.name!r} va=0x{self.vaddr:X} '
                f'vsz=0x{self.vsize:X} raw=0x{self.raw_off:X} '
                f'rawsz=0x{self.raw_size:X} chars=0x{self.chars:X}>')


class PE:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        self.path = path
        self.sections = []
        self.imports = OrderedDict()
        self.exports = []
        self._parse()

    def _u16(self, off):
        return struct.unpack_from('<H', self.data, off)[0]

    def _u32(self, off):
        return struct.unpack_from('<I', self.data, off)[0]

    def _u64(self, off):
        return struct.unpack_from('<Q', self.data, off)[0]

    def _parse(self):
        d = self.data
        if d[:2] != b'MZ':
            raise ValueError('nao e um PE (sem MZ)')
        e_lfanew = self._u32(0x3C)
        if d[e_lfanew:e_lfanew + 4] != b'PE\0\0':
            raise ValueError('nao e um PE (sem assinatura PE)')

        coff = e_lfanew + 4
        self.machine = self._u16(coff)
        self.nsections = self._u16(coff + 2)
        self.timestamp = self._u32(coff + 4)
        self.opt_size = self._u16(coff + 16)
        self.characteristics = self._u16(coff + 18)

        opt = coff + 20
        self.magic = self._u16(opt)
        self.is_pe32plus = (self.magic == 0x20B)
        self.entry = self._u32(opt + 16)
        if self.is_pe32plus:
            self.image_base = self._u64(opt + 24)
            self.subsystem = self._u16(opt + 0x44)
            self.dllchar = self._u16(opt + 0x46)
            self.size_of_image = self._u32(opt + 0x38)
            self.size_of_headers = self._u32(opt + 0x3C)
            ndd_off = opt + 0x70
        else:
            self.image_base = self._u32(opt + 28)
            self.subsystem = self._u16(opt + 0x44)
            self.dllchar = self._u16(opt + 0x46)
            self.size_of_image = self._u32(opt + 0x38)
            self.size_of_headers = self._u32(opt + 0x3C)
            ndd_off = opt + 0x60

        self.import_rva = self._u32(ndd_off + 1 * 8)
        self.import_size = self._u32(ndd_off + 1 * 8 + 4)
        self.export_rva = self._u32(ndd_off + 0 * 8)
        self.export_size = self._u32(ndd_off + 0 * 8 + 4)
        self.exception_rva = self._u32(ndd_off + 3 * 8)
        self.exception_size = self._u32(ndd_off + 3 * 8 + 4)
        self.security_rva = self._u32(ndd_off + 4 * 8)
        self.security_size = self._u32(ndd_off + 4 * 8 + 4)
        self.debug_rva = self._u32(ndd_off + 6 * 8)
        self.debug_size = self._u32(ndd_off + 6 * 8 + 4)

        sec_off = opt + self.opt_size
        for i in range(self.nsections):
            o = sec_off + i * 40
            name = d[o:o + 8].rstrip(b'\0').decode('ascii', 'replace')
            vsize = self._u32(o + 8)
            vaddr = self._u32(o + 12)
            raw_size = self._u32(o + 16)
            raw_off = self._u32(o + 20)
            chars = self._u32(o + 36)
            self.sections.append(Section(name, vaddr, vsize, raw_off, raw_size, chars))

        self._parse_imports()
        self._parse_exports()

    # ------------------------------------------------------------------
    def rva_to_off(self, rva):
        for s in self.sections:
            if s.vaddr <= rva < s.vaddr + max(s.vsize, s.raw_size):
                delta = rva - s.vaddr
                if delta < s.raw_size:
                    return s.raw_off + delta
                return None
        if rva < self.size_of_headers:
            return rva
        return None

    def off_to_rva(self, off):
        for s in self.sections:
            if s.raw_off <= off < s.raw_off + s.raw_size:
                return s.vaddr + (off - s.raw_off)
        return None

    def _cstr(self, off, limit=512):
        if off is None or off >= len(self.data):
            return ''
        end = self.data.find(b'\0', off, min(off + limit, len(self.data)))
        if end < 0:
            end = min(off + limit, len(self.data))
        return self.data[off:end].decode('ascii', 'replace')

    def _parse_imports(self):
        if not self.import_rva:
            return
        off = self.rva_to_off(self.import_rva)
        if off is None:
            return
        idx = 0
        while True:
            o = off + idx * 20
            if o + 20 > len(self.data):
                break
            oft = self._u32(o + 0)
            name_rva = self._u32(o + 12)
            first_thunk = self._u32(o + 16)
            if oft == 0 and name_rva == 0 and first_thunk == 0:
                break
            dll = self._cstr(self.rva_to_off(name_rva))
            thunk_rva = oft if oft else first_thunk
            funcs = []
            t = self.rva_to_off(thunk_rva)
            if t is not None:
                step = 8 if self.is_pe32plus else 4
                k = 0
                while True:
                    to = t + k * step
                    if to + step > len(self.data):
                        break
                    v = self._u64(to) if self.is_pe32plus else self._u32(to)
                    if v == 0:
                        break
                    if self.is_pe32plus and (v & 0x8000000000000000):
                        funcs.append(f'#{v & 0xFFFF}')
                    elif not self.is_pe32plus and (v & 0x80000000):
                        funcs.append(f'#{v & 0xFFFF}')
                    else:
                        hint_name_off = self.rva_to_off(v & 0x7FFFFFFF if not self.is_pe32plus else (v & 0x7FFFFFFFFFFFFFFF))
                        if hint_name_off is not None:
                            funcs.append(self._cstr(hint_name_off + 2))
                    k += 1
            self.imports[dll] = funcs
            idx += 1

    def _parse_exports(self):
        if not self.export_rva:
            return
        off = self.rva_to_off(self.export_rva)
        if off is None:
            return
        n_names = self._u32(off + 24)
        names_rva = self._u32(off + 32)
        no = self.rva_to_off(names_rva)
        if no is None:
            return
        for i in range(min(n_names, 512)):
            nr = self._u32(no + i * 4)
            self.exports.append(self._cstr(self.rva_to_off(nr)))

    # ------------------------------------------------------------------
    def section_data(self, name):
        for s in self.sections:
            if s.name == name:
                return self.data[s.raw_off:s.raw_off + s.raw_size]
        return b''

    def ascii_strings(self, min_len=6):
        out = []
        for m in re.finditer(rb'[\x20-\x7e]{%d,}' % min_len, self.data):
            out.append((m.start(), m.group().decode('ascii')))
        return out

    def utf16_strings(self, min_len=6):
        out = []
        pat = rb'(?:[\x20-\x7e]\x00){%d,}' % min_len
        for m in re.finditer(pat, self.data):
            out.append((m.start(), m.group().decode('utf-16-le')))
        return out

    def report(self):
        print('=' * 74)
        print(f'ARQUIVO      {self.path}')
        print(f'TAMANHO      {len(self.data)} bytes')
        print('=' * 74)
        print(f'Machine      {MACHINE.get(self.machine, hex(self.machine))}')
        print(f'Subsystem    {SUBSYSTEM.get(self.subsystem, hex(self.subsystem))}')
        print(f'PE32+        {self.is_pe32plus}')
        print(f'ImageBase    0x{self.image_base:X}')
        print(f'EntryPoint   0x{self.entry:X}   (VA 0x{self.image_base + self.entry:X})')
        print(f'SizeOfImage  0x{self.size_of_image:X}')
        print(f'DllChar      0x{self.dllchar:04X}  '
              f'[{", ".join(n for b, n in DLLCHAR if self.dllchar & b)}]')
        print(f'Timestamp    0x{self.timestamp:08X}')
        print(f'Checksum/security dir: rva=0x{self.security_rva:X} size={self.security_size}')
        print('\n-- SECOES ' + '-' * 63)
        print(f'{"nome":<10} {"VA":>10} {"VSize":>10} {"RawOff":>10} {"RawSize":>10}  chars')
        for s in self.sections:
            print(f'{s.name:<10} 0x{s.vaddr:08X} 0x{s.vsize:08X} '
                  f'0x{s.raw_off:08X} 0x{s.raw_size:08X}  0x{s.chars:08X}')

        print('\n-- IMPORTS ' + '-' * 62)
        if not self.imports:
            print('  (nenhum)')
        for dll, funcs in self.imports.items():
            print(f'\n  {dll}   ({len(funcs)} funcoes)')
            for f in funcs:
                print(f'      {f}')

        if self.exports:
            print('\n-- EXPORTS ' + '-' * 62)
            for e in self.exports:
                print(f'      {e}')

    def ioctl_candidates(self):
        """Constantes no .text/.rdata que batem com o formato CTL_CODE."""
        hits = {}
        for s in self.sections:
            if s.name not in ('.text', '.rdata', '.data'):
                continue
            blob = self.data[s.raw_off:s.raw_off + s.raw_size]
            for m in re.finditer(rb'[\x00-\xff]{4}', blob):
                v = struct.unpack_from('<I', blob, m.start())[0]
                dev = (v >> 16) & 0xFFFF
                func = (v >> 2) & 0xFFF
                method = v & 3
                access = (v >> 14) & 3
                # device 0x0001..0xFFFF (exclui lixo), func != 0
                if 0x8000 <= v <= 0xFFFFFFFF and func and dev >= 0x0001:
                    if method in (0, 1, 2, 3):
                        hits.setdefault(v, []).append((s.name, m.start()))
        return hits

    def api_hits(self):
        """APIs de kernel que importam para avaliar o primitivo do driver."""
        interesting = [
            'MmCopyVirtualMemory', 'MmMapIoSpace', 'MmMapIoSpaceEx',
            'MmGetPhysicalAddress', 'MmGetVirtualForPhysical',
            'PsLookupProcessByProcessId', 'PsGetProcessImageFileName',
            'ZwOpenProcess', 'NtOpenProcess', 'PsGetCurrentProcessId',
            'KeStackAttachProcess', 'KeUnstackDetachProcess',
            '__writecr3', 'KeBugCheckEx',
            'MmAllocateContiguousMemory', 'MmAllocateContiguousMemorySpecifyCache',
            'ExAllocatePool', 'ExAllocatePoolWithTag', 'ExFreePool',
            'IoCreateDevice', 'IoCreateSymbolicLink', 'IoDeleteDevice',
            'IoCreateDriver', 'IoDeleteDriver',
            'ZwMapViewOfSection', 'MmMapLockedPagesSpecifyCache',
            'MmProbeAndLockPages', 'IoAllocateMdl', 'MmBuildMdlForNonPagedPool',
            'KeInitializeMutex', 'KeWaitForSingleObject',
            'RtlInitUnicodeString', 'RtlCopyMemory', 'memcpy', 'memmove',
            'ObRegisterCallbacks', 'PsSetCreateProcessNotifyRoutine',
            'PsSetLoadImageNotifyRoutine', 'CmRegisterCallback',
            'KeWriteMsr', 'KeReadMsr', '__readmsr', '__writemsr',
            '__inbyte', '__outbyte', '__indword', '__outdword',
            'HalGetBusDataByOffset', 'HalSetBusDataByOffset',
            'IoWMIWriteEvent', 'ZwWriteFile', 'ZwReadFile',
            'ZwOpenKey', 'ZwQueryValueKey', 'ZwSetValueKey',
        ]
        found = []
        for dll, funcs in self.imports.items():
            for f in funcs:
                for i in interesting:
                    if f.lower() == i.lower():
                        found.append((f, dll))
        return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--strings', action='store_true')
    ap.add_argument('--wide', action='store_true')
    ap.add_argument('--ioctl', action='store_true')
    args = ap.parse_args()

    pe = PE(args.path)
    pe.report()

    print('\n-- APIs DE KERNEL RELEVANTES ' + '-' * 45)
    hits = pe.api_hits()
    if not hits:
        print('  (nenhuma das APIs rastreadas)')
    for f, dll in hits:
        print(f'  {f:<40} {dll}')

    if args.ioctl:
        print('\n-- CANDIDATOS A IOCTL ' + '-' * 53)
        cands = pe.ioctl_candidates()
        for v in sorted(cands):
            locs = cands[v]
            dev = (v >> 16) & 0xFFFF
            fn = (v >> 2) & 0xFFF
            method = ['BUFFERED', 'IN_DIRECT', 'OUT_DIRECT', 'NEITHER'][v & 3]
            access = ['ANY', 'READ', 'WRITE', 'RW'][(v >> 14) & 3]
            secs = ', '.join(f'{s}+0x{o:X}' for s, o in locs[:4])
            print(f'  0x{v:08X}  dev=0x{dev:04X} func=0x{fn:03X} '
                  f'{method:<9} {access:<5}  [{secs}]')

    if args.strings:
        print('\n-- STRINGS ASCII ' + '-' * 57)
        for off, s in pe.ascii_strings():
            rva = pe.off_to_rva(off)
            r = f'0x{rva:08X}' if rva is not None else '   ?    '
            print(f'  {off:08X}  {r}  {s}')

    if args.wide:
        print('\n-- STRINGS UTF-16 ' + '-' * 56)
        for off, s in pe.utf16_strings():
            print(f'  {off:08X}  {s}')


if __name__ == '__main__':
    main()
