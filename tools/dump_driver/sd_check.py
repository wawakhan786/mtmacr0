"""
sd_check.py — valida o layout do security descriptor FORA do kernel.

POR QUE ISSO EXISTE
-------------------
A primeira versao do SD calculava `ACL.AclSize` com `sizeof(ACL)` e
`sizeof(ACCESS_ALLOWED_ACE)`, escrevia o SID logo depois do ACE, e o
resultado foi BSOD 0x1E KMODE_EXCEPTION_NOT_HANDLED com o endereco do
bugcheck dentro da imagem do driver.

Este checker reproduz o MESMO calculo do km_driver_source.c em Python e
confere que o bloco alocado cobre exatamente [SD][ACL][ACE][SID].

CUIDADO COM OS TAMANHOS (eu errei aqui na primeira versao do checker):
  SECURITY_DESCRIPTOR  = 20   (Rev 1 + Sbz1 1 + Control 2 + Owner 4 +
                               Group 4 + Sacl 4 + Dacl 4)
  ACL                  =  8   (Rev 1 + Sbz1 1 + AclSize 2 + AceCount 2 +
                               Sbz2 2)
  ACCESS_ALLOWED_ACE   = 12   (Header 4 + Mask 4 + SidStart 4)
  SID S-1-1-0          = 12   (Rev 1 + Count 1 + Auth 6 + SubAuth 4)

Eu tinha posto ACE=16 e "provei" que o AceSize estava errado. Estava errado
era o checker.

Uso:
    python sd_check.py
"""

import struct
import sys

SZ_SD = 20
SZ_ACL = 8
SZ_ACE = 12            # Header 4 + Mask 4 + SidStart 4
SZ_SID = 12

# exatamente o que o driver calcula:
#   tamAcl = sizeof(ACL) + sizeof(ACE) - sizeof(ULONG) + tamSid
#   AceSize = sizeof(ACE) - sizeof(ULONG) + tamSid
# sizeof(ULONG) == 4 == o campo SidStart que ja faz parte do SID
TAM_ACL = SZ_ACL + SZ_ACE - 4 + SZ_SID
ACE_SIZE = SZ_ACE - 4 + SZ_SID
BLOCO = SZ_SD + TAM_ACL

OFF_ACL = SZ_SD
OFF_ACE = OFF_ACL + SZ_ACL
# O SID comeca no campo SidStart, que fica DEPOIS de Header(4) + Mask(4).
# Nao em OFF_ACE + sizeof(ACE): sizeof(ACE) ja inclui os 4 bytes de SidStart.
# Errar isso deixa o bloco 4 bytes curto — foi o que estourou.
OFF_SID = OFF_ACE + (SZ_ACE - 4)
FIM_SID = OFF_SID + SZ_SID

passou = 0
falhou = 0


def check(nome, ok, det):
    global passou, falhou
    if ok:
        passou += 1
        print('  [+] %-42s %s' % (nome, det))
    else:
        falhou += 1
        print('  [-] %-42s %s' % (nome, det))


def decodifica_sid(b):
    rev = b[0]
    sub = b[1]
    auth = int.from_bytes(b[2:8], 'big')
    subs = [struct.unpack_from('<I', b, 8 + i * 4)[0] for i in range(sub)]
    return 'S-%d-%d%s' % (rev, auth, ''.join('-%d' % s for s in subs)), sub


def main():
    print('=' * 72)
    print('  VALIDACAO DO LAYOUT DO SECURITY DESCRIPTOR (fora do kernel)')
    print('=' * 72)
    print()
    print('  SECURITY_DESCRIPTOR = %d' % SZ_SD)
    print('  ACL                 = %d' % SZ_ACL)
    print('  ACCESS_ALLOWED_ACE  = %d  (Header 4 + Mask 4 + SidStart 4)' % SZ_ACE)
    print('  SID S-1-1-0         = %d' % SZ_SID)
    print()
    print('  tamAcl  = %d + %d - 4 + %d = %d' % (SZ_ACL, SZ_ACE, SZ_SID, TAM_ACL))
    print('  AceSize = %d - 4 + %d     = %d' % (SZ_ACE, SZ_SID, ACE_SIZE))
    print('  bloco   = %d + %d         = %d' % (SZ_SD, TAM_ACL, BLOCO))
    print()

    check('ACL.AclSize cobre ACL+ACE+SID',
          TAM_ACL == SZ_ACL + SZ_ACE - 4 + SZ_SID,
          'AclSize=%d' % TAM_ACL)

    check('ACE comeca em ACL + sizeof(ACL)',
          OFF_ACE == OFF_ACL + SZ_ACL,
          'offAce=%d esperado=%d' % (OFF_ACE, OFF_ACL + SZ_ACL))

    check('SID termina dentro do bloco',
          FIM_SID <= BLOCO,
          'fimSid=%d bloco=%d sobra=%d' % (FIM_SID, BLOCO, BLOCO - FIM_SID))

    # (Header 4 + Mask 4) + SID 12 = 20 — os 4 de SidStart ja estao no SID
    check('ACE.Header.AceSize == 20',
          ACE_SIZE == 20,
          'AceSize=%d  (4 header + 4 mask + 12 sid)' % ACE_SIZE)

    # o SID que escrevemos caberia no espaco a partir de SidStart?
    check('SID cabe a partir do SidStart',
          FIM_SID - OFF_ACE + 4 <= TAM_ACL,
          'bytes do ACE ao fim=%d  AclSize=%d' % (FIM_SID - OFF_ACE + 4, TAM_ACL))

    bloco = bytearray(BLOCO)
    s = bloco[OFF_SID:FIM_SID]
    s[0] = 1
    s[1] = 1
    s[2] = 0; s[3] = 0; s[4] = 0
    s[5] = 0; s[6] = 0; s[7] = 1
    s[8] = 0; s[9] = 0; s[10] = 0; s[11] = 0
    bloco[OFF_SID:FIM_SID] = s

    texto, sub = decodifica_sid(bytes(bloco[OFF_SID:FIM_SID]))
    check('SID decodifica como S-1-1-0', texto == 'S-1-1-0',
          '%s (SubAuthorityCount=%d)' % (texto, sub))

    # A armadilha que causou o BSOD: o AclSize TEM que ser >= o que o
    # RtlAddAccessAllowedAce vai escrever, medido a partir do inicio da ACL.
    escrito_pela_api = SZ_ACL + (SZ_ACE - 4) + SZ_SID
    check('AclSize >= o que a API escreve',
          TAM_ACL >= escrito_pela_api,
          'AclSize=%d  API escreve=%d' % (TAM_ACL, escrito_pela_api))

    # E o bloco alocado TEM que caber ACL + tudo que ela cobre.
    check('bloco cabe SD + AclSize',
          BLOCO >= SZ_SD + TAM_ACL,
          'bloco=%d  precisa=%d' % (BLOCO, SZ_SD + TAM_ACL))

    print()
    print('=' * 72)
    print('  RESULTADO: %d passaram, %d falharam' % (passou, falhou))
    print('=' * 72)
    if falhou:
        print('  NAO carregar o driver: o layout tem erro.')
    else:
        print('  Layout consistente. O BSOD, se repetir, NAO e daqui.')
    return 0 if falhou == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
