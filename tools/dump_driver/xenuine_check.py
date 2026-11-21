"""
xenuine_check.py â€” analisa o XenuineDecrypt no TslGame.exe EM DISCO.

POR QUE ISSO EXISTE
-------------------
O log do jogo diz:

    [Ndu] Cache: UWorld lido OK
        rawUWorld    = 0x0 <-- ZERO

e o `InitDecrypt` falha. Antes de mexer em qualquer coisa, quero responder
DUAS perguntas que decidem o caminho:

  1. O offset 0x11757E28 esta DENTRO da secao que contem o UWorld
     (0x1327BFE8)? Se nao estiver, a leitura vai pra memoria nao-mapeada e
     nao ha o que depurar.

  2. O conteudo NAQUELE OFFSET no disco parece codigo (uma instrucao `lea` /
     `mov` que o InitDecrypt procura), ou parece um PONTEIRO?

O `InitDecrypt` assume a SEGUNDA hipotese: ele faz

    DecryptPtr = Read(base + XenuineDecrypt)     <- trata como PONTEIRO
    Read(DecryptPtr, shellcode, 1024)            <- segue o ponteiro
    procura lea/mov na shellcode

Se no disco aquilo ja e CODIGO (e nao um ponteiro), entao `DecryptPtr` e lixo
e a cadeia inteira falha â€” que e exatamente o sintoma.

NAO precisa do jogo aberto. So le o arquivo.

Uso:
    python xenuine_check.py [caminho_do_TslGame.exe]
"""

import struct
import sys
import os

PADRAO = r'C:\Program Files (x86)\Steam\steamapps\common\PUBG\TslGame\Binaries\Win64\TslGame.exe'

# Os offsets da lista que o usuario passou (RVA no modulo).
XENUINE_DECRYPT = 0x11757E28
UWORLD = 0x1327BFE8
GNAMES = 0x13511EA0
PHYSXSDK = 0x123BE088
GOBJECTS = 0x1323CD60
UNREAL_ENGINE = 0x13278060


def rva_para_offset(rva, secoes):
    """Converte RVA em offset no arquivo usando a tabela de secoes."""
    for nome, va, vsize, rawptr, rawsize in secoes:
        if va <= rva < va + max(vsize, rawsize):
            delta = rva - va
            if delta < rawsize:
                return rawptr + delta, nome
            return None, nome            # em memoria mas nao no arquivo (.bss)
    return None, None


def nome_limpo(n):
    """Alguns nomes de secao do PUBG tem bytes nao-imprimiveis."""
    return ''.join(c if 32 <= ord(c) < 127 else '?' for c in n)


def ler_secoes(d, pe_off):
    nsec = struct.unpack_from('<H', d, pe_off + 6)[0]
    opt_size = struct.unpack_from('<H', d, pe_off + 20)[0]
    sec_off = pe_off + 24 + opt_size
    out = []
    for i in range(nsec):
        b = sec_off + i * 40
        nome = d[b:b + 8].rstrip(b'\0').decode('ascii', 'replace')
        vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', d, b + 8)
        out.append((nome, va, vsize, rawptr, rawsize))
    return out


def tamanho_imagem(d, pe_off):
    # SizeOfImage esta no optional header: +56 em PE32+ (offset 0x38)
    return struct.unpack_from('<I', d, pe_off + 24 + 56)[0]


def hexdump(b, base_rva, n=48):
    linhas = []
    for i in range(0, min(len(b), n), 16):
        pedaco = b[i:i + 16]
        hexs = ' '.join('%02X' % x for x in pedaco)
        asc = ''.join(chr(x) if 32 <= x < 127 else '.' for x in pedaco)
        linhas.append('      0x%08X  %-47s  %s' % (base_rva + i, hexs, asc))
    return linhas


def main():
    caminho = sys.argv[1] if len(sys.argv) > 1 else PADRAO
    print('=' * 74)
    print('  ANALISE DO XENUINE / UWORLD â€”', os.path.basename(caminho))
    print('=' * 74)
    print()

    if not os.path.exists(caminho):
        print('  [X] nao achei:', caminho)
        return 1

    d = open(caminho, 'rb').read()
    print('  arquivo: %d bytes (%.0f MiB)' % (len(d), len(d) / 1048576.0))

    if d[:2] != b'MZ':
        print('  [X] nao e um PE (sem MZ)')
        return 1
    pe_off = struct.unpack_from('<I', d, 0x3C)[0]
    if d[pe_off:pe_off + 4] != b'PE\0\0':
        print('  [X] assinatura PE errada em 0x%X' % pe_off)
        return 1

    secoes = ler_secoes(d, pe_off)
    size_of_image = tamanho_imagem(d, pe_off)

    print('  PE em 0x%X' % pe_off)
    print('  SizeOfImage = 0x%X (%.1f MiB)' % (size_of_image, size_of_image / 1048576.0))
    print()
    print('  secoes:')
    for nome, va, vsize, rawptr, rawsize in secoes:
        marca = ''
        if va <= XENUINE_DECRYPT < va + max(vsize, rawsize):
            marca = '  <-- contem XenuineDecrypt'
        if va <= UWORLD < va + max(vsize, rawsize):
            marca += '  <-- contem UWorld'
        print('    %-8s VA=0x%08X vsize=0x%08X raw=0x%08X..0x%08X%s'
              % (nome, va, vsize, rawptr, rawptr + rawsize, marca))
    print()

    # ---------------------------------------------------------------------
    # 1. os offsets estao dentro do modulo?
    # ---------------------------------------------------------------------
    print('  --- 1. os offsets cabem no modulo? ---')
    for rotulo, rva in [('XenuineDecrypt', XENUINE_DECRYPT), ('UWorld', UWORLD),
                        ('GNames', GNAMES), ('PhysxSDK', PHYSXSDK),
                        ('GObjects', GOBJECTS), ('UnrealEngine', UNREAL_ENGINE)]:
        dentro = rva < size_of_image
        off, sec = rva_para_offset(rva, secoes)
        onde = 'secao %s, arquivo +0x%X' % (sec, off) if off is not None else \
               ('secao %s (so em memoria)' % sec if sec else 'FORA de qualquer secao')
        print('    %-16s 0x%08X  %s  %s'
              % (rotulo, rva, 'dentro' if dentro else '*** FORA ***', onde))
    print()

    # ---------------------------------------------------------------------
    # 2. o conteudo no offset do XenuineDecrypt
    # ---------------------------------------------------------------------
    print('  --- 2. o que tem no offset do XenuineDecrypt? ---')
    off, sec = rva_para_offset(XENUINE_DECRYPT, secoes)
    if off is None:
        print('    nao esta no arquivo (secao %s, so em memoria)' % sec)
    else:
        print('    secao %s, offset no arquivo 0x%X' % (sec, off))
        print()
        bruto = d[off:off + 32]
        for l in hexdump(bruto, XENUINE_DECRYPT):
            print(l)
        print()
        q = struct.unpack_from('<Q', d, off)[0]
        print('    como QWORD (ponteiro) : 0x%016X' % q)
        print('    plausivel como VA?      %s'
              % ('SIM (0x7FF... ou 0xFFFF...)' if (q >> 48) in (0x7FF0, 0x7FF1, 0x7FF2, 0x7FF3, 0xFFFF) or (q & 0xFFFF000000000000) == 0x7FF0000000000000 or q > 0xFFFF000000000000 else 'NAO (nao parece endereco)'))
        print()
        print('    como CODIGO, os primeiros bytes:')
        for i in range(0, 12):
            print('      +%02d  0x%02X' % (i, bruto[i]))
        print()
        # o InitDecrypt procura lea/mov no byte de indice 3 da "shellcode",
        # que no layout dele vem DEPOIS de 2 nops â€” entao olha aqui:
        print('    o InitDecrypt monta a shellcode assim:')
        print('      [0]=0x90 [1]=0x90 [2..]= <lido de DecryptPtr>')
        print('      e depois testa ShellcodeBuff[3] (== lido[1])')
        print('      lido[1] aqui = 0x%02X  -> %s'
              % (bruto[1],
                 '0x8D (LEA)' if bruto[1] == 0x8D else
                 '0x8B (MOV)' if bruto[1] == 0x8B else
                 'NEM LEA NEM MOV -> g_IsLeaInstruction = false'))

        # se o conteudo ja parece codigo (comum em inicio de funcao:
        # 48 89/8B/83, 55, 40 53, sub rsp), entao NAO e um ponteiro.
        parece_codigo = bruto[0] in (0x48, 0x4C, 0x55, 0x40, 0x53, 0x57, 0x56, 0x41) \
            or (bruto[0] == 0xCC)
        print()
        print('    o conteudo PARECE CODIGO? %s' % ('SIM' if parece_codigo else 'nao'))
        if parece_codigo and not (q > 0xFFFF000000000000):
            print('    >>> Se e CODIGO, entao tratar como PONTEIRO esta errado.')
            print('        O InitDecrypt faz Read(DecryptPtr, ...), ou seja:')
            print('        le o qword aqui e o usa como endereco. Se aqui ja e')
            print('        a funcao (e nao um ponteiro para ela), o qword e')
            print('        lixo e a cadeia falha inteira.')

    print()
    print('  --- 3. e no UWorld? (deve ser um PONTEIRO, zerado antes do jogo) ---')
    off, sec = rva_para_offset(UWORLD, secoes)
    if off is None:
        print('    nao esta no arquivo (secao %s)' % sec)
    else:
        print('    secao %s, offset no arquivo 0x%X' % (sec, off))
        bruto = d[off:off + 16]
        for l in hexdump(bruto, UWORLD):
            print(l)
        print('    no DISCO normalmente e 0 (o valor so existe em runtime).')
        print('    Se o disco mostra um PONTEIRO aqui, ele aponta pra imagem â€”')
        print('    e o loader vai RELOCAR esse valor ao carregar.')

    print()
    print('=' * 74)
    return 0


if __name__ == '__main__':
    sys.exit(main())
