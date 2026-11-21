# AsIO3.sys — contrato de IOCTL extraído

Analisado estaticamente com `tools\dump_driver\ioctl_xref.py`. **Nunca
carregado nesta máquina.**

```
arquivo        C:\Windows\System32\drivers\AsIO3.sys
tamanho        59344 bytes
versão         1.02.22
assinatura     ASUSTeK COMPUTER INC.  — Valid
cert expira    22/10/2024 (irrelevante, tem timestamp)
máquina        AMD64, Subsystem NATIVE
DllChar        0x41E0 [HIGH_ENTROPY_VA, DYNAMIC_BASE, FORCE_INTEGRITY,
                       NX_COMPAT, GUARD_CF]

Device         \Device\Asusgio3
Symlink        \DosDevices\Asusgio3   →  \\.\Asusgio3
Evento         \BaseNamedObjects\WaitForIoAccess
```

## Família de IOCTL

Tudo usa `FILE_DEVICE = 0xA040`. Nenhum usa o `FILE_DEVICE_UNKNOWN` (0x22)
típico de cheat — é device de fabricante.

### Leitura de baixo nível — `0xA0406400/6404/6408` (READ)

Três larguras. O contexto do dispatcher tem `0F 30` (`wrmsr`), `0F 32`
(`rdmsr`), `ED` (`in eax, dx`), `EE` (`out dx, al`), `66 EF` / `EF` — ou
seja, **MSR e porta I/O**, com a largura escolhida pelo IOCTL.

### Escrita de baixo nível — `0xA040A440/A444/A448` (WRITE)

O trio mais útil. Contexto mostra `__inbyte/__indword` e `__outbyte/__outdword`
selecionados por índice.

### Escrita de baixo nível, outra largura — `0xA040A540/A544/A548` (WRITE)

### Especiais — `0xA040A44C`, `0xA040A54C` (WRITE)

Aparecem isolados, um por família de largura.

## Layout do buffer

O dispatcher lê **bytes crus do `InputBuffer` em offsets fixos**:

| família | deslocamento do endereço | deslocamento do dado |
|---|---|---|
| `...A440/A444/A448` | +3 (3 bytes) | +1 (2 bytes, via `0F B7 / 66`) |
| `...A540/A544/A548` | +4 | +3 |

Ou seja, o cliente monta um buffer pequeno e o driver interpreta por posição:

```
[A540/A544/A548]  byte 0..3  = algo (opcode/flags)
                  byte 4..7  = endereço (porta ou MSR)
                  byte 8..11 = valor
```

**Os offsets exatos ainda precisam de confirmação em runtime.** O dump mostra
as leituras (`83 7D 40 01`, `0F B7 4D 38`, `8B 45 38`) mas o ponto de partida
do buffer depende de qual campo do IRP o dispatcher usou — não dá pra fechar
isso sem executar.

## A trava: checagem de chamador

O driver carrega o caminho de `\ASUS\AsusCertService\AsusCertService.exe`,
resolve `ProgramFilesDir (x86)` no registry, importa
`PsSetCreateProcessNotifyRoutineEx` e `PsGetProcessImageFileName`, e lê
`\ASUS\AsusCertService\` no registro.

Ou seja: **para os caminhos de memória física ele valida quem chama.** Para os
caminhos de porta/MSR a checagem pode não existir — não foi possível
confirmar sem seguir o fluxo até o fim.

## O que ele dá e o que não dá

| Primitivo | Disponível |
|---|---|
| MSR (leitura/escrita) | **sim** — `0F 30` / `0F 32` |
| Porta I/O (byte/word/dword) | **sim** — `ED/EE/66 EF/EF/EC` |
| Config space PCI | sim — `HalGetBusDataByOffset` / `HalSetBusDataByOffset` |
| Memória física | via `\Device\PhysicalMemory` (aberto com `ZwOpenSection` + `ZwMapViewOfSection`) e via `MmMapIoSpace` |
| **Memória virtual de um PID** | **não** — não importa `MmCopyVirtualMemory`, `ZwOpenProcess` nem `KeStackAttachProcess` |

Para chegar em memória virtual a partir daqui é preciso **page-table walk**.

## Caminho para usar

```
1. abrir \\.\Asusgio3
2. obter leitura de memória física (seção PhysicalMemory ou MmMapIoSpace)
3. achar a CR3 do processo alvo
4. andar PML4 -> PDPT -> PD -> PT
5. ler a página física
```

O passo 2 é o que a checagem de chamador protege. As saídas, em ordem de
esforço:

| # | Saída | Custo |
|---|---|---|
| 1 | `AsusCertService.exe` rodando (ASUS AI Suite / Armoury Crate instalado) | zero, se você tiver ASUS |
| 2 | patch do check na cópia mapeada do driver, depois do map | médio |
| 3 | mapper próprio que pula a validação | alto |

**O passo 3 do walk (`CR3`) já está resolvido no nosso `Ndu.sys`**
(`IOCTL_GET_CR3` → `_EPROCESS->DirectoryTableBase`), e o walk de 4 níveis
também. O que falta é só o passo 2 — o primitivo de física.

## Próximo passo concreto

Rodar `ioctl_xref.py --family 0xA040` no `Asusgio3` e seguir o fluxo do
dispatcher a partir do bloco em `0x1400029B1` (a família `A540/A544/A548`),
para confirmar:

1. se a checagem de chamador acontece antes ou depois do switch de IOCTL
2. os offsets exatos do buffer de entrada
3. se a seção `\Device\PhysicalMemory` é aberta no `DriverEntry` (o que
   significaria que já está mapeada quando abrimos o device)
