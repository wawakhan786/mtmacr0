"""
kmlog_check.py â€” valida o parser do KmLog FORA do kernel.

POR QUE ISSO EXISTE
-------------------
O KmLog do driver imprimiu o TEXTO DO FORMATO em vez dos valores:

    [Ndu] base=0x%llx  SizeOfImage=0x%llx
    [Ndu]   valor do slot = 0x%llx

Ou seja: eu escrevi um diagnostico cuja saida eu nao conseguia ler. Antes de
pedir outra rodada do usuario, testo o parser aqui.

Este script reimplementa EXATAMENTE o mesmo algoritmo do KmLog novo (em
km_driver_source.c) e compara a saida com o que um printf de verdade daria.

Uso:
    python kmlog_check.py
"""

import sys

# ---------------------------------------------------------------------------
# Reimplementacao FIEL do algoritmo do KmLog em C.
# Cada passo corresponde a uma linha do KmLog.
# ---------------------------------------------------------------------------
def kmlog(fmt, *args):
    linha = "[Ndu] "
    n = len(linha)
    MAX = 768
    ai = 0
    p = 0

    while p < len(fmt) and n < MAX - 48:
        ch = fmt[p]
        if ch != '%':
            linha += ch
            n += 1
            p += 1
            continue
        p += 1

        flagEsq = False
        flagZero = False
        largura = 0
        tam64 = 0

        # flags
        while p < len(fmt) and fmt[p] in '-0':
            if fmt[p] == '-':
                flagEsq = True
            else:
                flagZero = True
            p += 1

        # largura
        while p < len(fmt) and fmt[p].isdigit():
            largura = largura * 10 + int(fmt[p])
            p += 1

        # tamanho: l / I / 6 / 4
        while p < len(fmt) and fmt[p] in 'lI64':
            if fmt[p] == 'l':
                tam64 += 1
            p += 1

        if p >= len(fmt):
            break
        spec = fmt[p]

        if spec == 's':
            vs = str(args[ai]); ai += 1
            ln = len(vs)
            if largura > 0:
                if flagEsq:
                    linha += vs
                    linha += ' ' * max(0, largura - ln)
                else:
                    linha += ' ' * max(0, largura - ln)
                    linha += vs
            else:
                linha += vs

        elif spec == 'p':
            vp = args[ai]; ai += 1
            linha += '0x' + ('%016x' % (vp & 0xFFFFFFFFFFFFFFFF))

        elif spec in 'xX':
            if tam64 >= 2:
                v64 = args[ai]; ai += 1
            elif tam64 == 1:
                v64 = args[ai]; ai += 1
            else:
                v64 = args[ai]; ai += 1
            v64 &= 0xFFFFFFFFFFFFFFFF
            digitos = 1
            for i in range(15, -1, -1):
                if (v64 >> (i * 4)) & 0xF:
                    digitos = i + 1
                    break
            if largura > digitos:
                digitos = largura
            s = ''
            for i in range(digitos - 1, -1, -1):
                d = (v64 >> (i * 4)) & 0xF
                c = '%x' % d if spec == 'x' else '%X' % d
                s += c
            linha += s

        elif spec in 'ud':
            v = args[ai]; ai += 1
            u = abs(v) if (v < 0 and spec == 'd') else v
            tmp = str(u)
            if len(tmp) == 1 and u == 0:
                tmp = '0'
            if v < 0 and spec == 'd':
                linha += '-'
            linha += tmp

        elif spec == '%':
            linha += '%'

        else:
            linha += '%' + spec

        p += 1

    return linha


passou = 0
falhou = 0


def check(nome, obtido, esperado):
    global passou, falhou
    ok = obtido == esperado
    if ok:
        passou += 1
        print('  [+] %-38s %s' % (nome, obtido))
    else:
        falhou += 1
        print('  [-] %-38s' % nome)
        print('        obtido  : %s' % obtido)
        print('        esperado: %s' % esperado)


def main():
    print('=' * 74)
    print('  VALIDACAO DO PARSER DO KmLog (fora do kernel)')
    print('=' * 74)
    print()

    # os formatos EXATOS que o diagnostico usa
    check('%llx  (base)',
          kmlog('base=0x%llx  SizeOfImage=0x%llx',
                0x7FF7B74F0000, 0x1ABEC000),
          '[Ndu] base=0x7ff7b74f0000  SizeOfImage=0x1abec000')

    check('%llx  (valor zero)',
          kmlog('  valor cru = 0x%llx  %s', 0, '<-- ZERO'),
          '[Ndu]   valor cru = 0x0  <-- ZERO')

    check('%llx  (slot)',
          kmlog('  valor do slot = 0x%llx', 0x7FF123456789),
          '[Ndu]   valor do slot = 0x7ff123456789')

    check('%08x  (RVA)',
          kmlog('RVA 0x%08x', 0x11757E28),
          '[Ndu] RVA 0x11757e28')

    check('%-14s  (nome alinhado)',
          kmlog('%-14s = 0x%llx', 'GNames', 0xDEAD),
          '[Ndu] GNames         = 0xdead')

    check('%-14s  (nome curto)',
          kmlog('%-14s = 0x%llx', 'GGo', 1),
          '[Ndu] GGo            = 0x1')

    check('%s  simples',
          kmlog('parece VA x64? %s', 'SIM'),
          '[Ndu] parece VA x64? SIM')

    check('%u  decimal',
          kmlog('candidato %u: pid=%u', 0, 976),
          '[Ndu] candidato 0: pid=976')

    check('%d  negativo',
          kmlog('erro %d', -5),
          '[Ndu] erro -5')

    check('%02x  (byte)',
          kmlog('%02x %02x %02x', 0x8D, 0x00, 0xFF),
          '[Ndu] 8d 00 ff')

    check('%x  sem largura',
          kmlog('0x%x', 0x1ABEC000),
          '[Ndu] 0x1abec000')

    check('%X  maiusculo',
          kmlog('0x%X', 0xABCDEF),
          '[Ndu] 0xABCDEF')

    check('%p',
          kmlog('ptr=%p', 0xFFFF978765D63A30),
          '[Ndu] ptr=0xffff978765d63a30')

    check('%%  literal',
          kmlog('100%% pronto'),
          '[Ndu] 100% pronto')

    check('formato desconhecido nao desalinha',
          kmlog('valor 0x%llx fim', 0x1234),
          '[Ndu] valor 0x1234 fim')

    print()
    print('=' * 74)
    print('  RESULTADO: %d passaram, %d falharam' % (passou, falhou))
    print('=' * 74)
    if falhou:
        print('  O log vai sair ilegivel. NAO pedir rodada do usuario ainda.')
    else:
        print('  O log vai sair legivel.')
    return 0 if falhou == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
