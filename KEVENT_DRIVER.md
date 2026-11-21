# Driver de transporte — arquitetura, protocolo e pontos de revisão

**Arquivo:** `SharedProto/km_ipc_driver.c` (31 KB) · **Protocolo:** `SharedProto/km_ipc_proto.h` · **Mouse:** `SharedProto/km_mouse.h`
**Cliente:** `PubgExt/driver/driver_interface_kevent.h`
**Artefato:** `SharedProto/out/km_ipc_proto.sys` — 19.968 bytes, mapeado por kdmapper (manual mapping, sem serviço)
**Versão do protocolo:** `KMP_VERSION = 3`

---

## 1. O que este driver é (e o que ele não é)

É um **canal de transporte** entre um processo de user-mode e o kernel. Não é um driver de dispositivo:

| não existe | consequência |
|---|---|
| `IoCreateDevice` | nenhum objeto `\Device\...` |
| `IoCreateSymbolicLink` | nenhum `\DosDevices\...` |
| `MajorFunction[...]` / IRP | nenhum ponto de entrada atacável por `DeviceIoControl` |
| literal de nome (device, seção, evento, valor) | varredura de strings no `.sys` não acha nada |

Toda a comunicação é:

```
seção de memória compartilhada (pagefile-backed, SEM NOME)
   + 2 eventos SEM NOME
   + 1 thread de sistema no kernel
```

O único rastro no espaço de objetos do Windows é uma chave de registro **transitória** criada pelo cliente, usada no handshake e **apagada pelo driver** antes de começar a atender.

Escopo funcional: `READ`, `GET_BASE`, `MOUSE` (injeção por Mouclass) + 4 operações sintéticas (`NOP`/`ECHO`/`FILL`/`SUM`) que existem para medir o custo do round-trip.

---

## 2. Modelo de objetos

```
USER-MODE (cliente)                       KERNEL (driver)
┌──────────────────────────┐              ┌───────────────────────────────┐
│ CreateFileMappingW       │              │ ZwOpenSection? NÃO.           │
│   (pagefile, sem nome)   │              │ ObReferenceObjectByHandle +   │
│ MapViewOfFile ───────────┼──────────────┼─► MmMapViewInSystemSpace     │
│                          │   seção      │   (view lida/escrita pelo     │
│ CreateEventW x2 (sem nome)│             │    driver)                    │
│   ReqEvt / RspEvt ───────┼──────┐       │                               │
│                          │      │       │ ObReferenceObjectByHandle     │
│ WriteFile/Registry:      │      └───────┼─► KeWaitForSingleObject        │
│   5 valores REG_QWORD    │  handshake   │   (ReqEvt, 100 ms, timeout)   │
│   (Sig,Pid,Section,Evt1,2)│   por HKLM   │                               │
└──────────────────────────┘              │ PsCreateSystemThread           │
                                          │   └─ KmpServerThread           │
                                          └───────────────────────────────┘
```

**Por que objetos SEM NOME:** um objeto nomeado fica enumerável (`NtOpenDirectoryObject` em `\BaseNamedObjects`, `\KernelObjects`). Como os handles são passados por valor na chave, o objeto em si nunca precisa de nome para ser encontrado pelo kernel.

**Por que `MmMapViewInSystemSpace` e não `ZwMapViewOfSection`:** a segunda exige um `HANDLE` de **processo** como primeiro argumento. O driver roda em contexto de SYSTEM, então o handle do cliente não vale ali. A primeira recebe o **ponteiro do objeto seção** já referenciado.

---

## 3. Sequência de boot (o handshake)

A ordem é rígida — cada passo depende do anterior existir:

```
CLIENTE                                          DRIVER (DriverEntry)
1. CreateFileMappingW(INVALID_HANDLE_VALUE,…)    (ainda não existe)
   → hSec                                           |
2. CreateEventW x2 (sem nome) → hReq, hRsp          |
3. RegCreateKeyExW(HKLM\SOFTWARE\Classes\CLSID\     |
      <32 hex derivados do seed>)                    |
   RegSetValueExW x5 (Sig, Pid, hSec, hReq, hRsp)   |
   RegSetValueExW(<svc>\Parameters\Seed)            |
4. mapeia o driver (kdmapper, seed em param2) ──────┼──► 5. DriverEntry
                                                    |     ├─ KmpReadSeed(param2)
                                                    |     │  se NÃO parece caminho
                                                    |     │  de registro: seed = valor
                                                    |     ├─ KmpInit
                                                    |     │  ├─ caminho derivado do seed
                                                    |     │  ├─ lê os 5 REG_QWORD
                                                    |     │  ├─ duplica os 3 handles
                                                    |     │  │  (KmpDupFromClient, usando
                                                    |     │  │   o Pid da chave)
                                                    |     │  ├─ referência seção/eventos
                                                    |     │  ├─ publica Hdr (Magic, Version,
                                                    |     │  │  offsets, ImageBase)
                                                    |     │  ├─ PsCreateSystemThread
                                                    |     │  └─ APAGA a chave
6. espera Hdr.Ready == 1 (teto de 3 s) ◄────────────┘
7. a partir daqui: round-trips
```

**A ordem não é estilo, é correção.** Se o driver subir antes do passo 3, o `ZwOpenKey` falha e o `DriverEntry` devolve erro — o sintoma no cliente é `erro 2`, que parece "driver não carregou".

### 3.1 `KmpReadSeed` — o teste que já custou um bugcheck

Com kdmapper, `RegistryPath` **não é** `UNICODE_STRING`: é o **seed** passado em `param2`. Ler `p->Buffer` num valor não-canônico é `#GP` em kernel.

```c
static BOOLEAN KmpLooksLikeRegistryPath(PUNICODE_STRING p)
{
    ULONG_PTR v = (ULONG_PTR)p;
    if (p == NULL) return FALSE;
    if ((v & 7) != 0) return FALSE;         // UNICODE_STRING é alinhado a 8
    if (v < 0x10000) return FALSE;          // valor pequeno = seed
    if (!MmIsAddressValid((PVOID)p)) return FALSE;
    __try {                                  // só então tocar nos campos
        if (p->Buffer == NULL) return FALSE;
        if (p->Length == 0 || (p->Length & 1) || p->Length > 512) return FALSE;
        return (BOOLEAN)(p->Buffer[0] == L'\\');
    } __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
}
```

O teste é **numérico antes de qualquer desreferência** e a desreferência fica sob `__try`.

---

## 4. Layout da memória compartilhada

Tudo é publicado **por offset** no cabeçalho — os dois lados nunca usam `offsetof` próprio:

```c
typedef struct _KMP_HDR {
    kmp_u32 Magic;            // out 0x4B4D4950 ('KMIP')
    kmp_u32 Version;          // out 5
    volatile kmp_u32 Ready;   // out 1 quando o boot terminou
    volatile kmp_u32 Ativo;   // out 0 quando o servidor parou
    kmp_u64 Signature;        // out prova que é ESTA sessão
    kmp_u64 TotalBytes;
    kmp_u64 ReqOffset;        // out offset de KMP_REQ
    kmp_u64 RespOffset;       // out offset de KMP_RESP
    kmp_u64 DataOffset;       // out início do payload
    kmp_u64 DataBytes;        // out 256 KiB
    kmp_u64 ImageBase;        // out base do mapeamento (p/ MmFreeIndependentPages)
    kmp_u64 ImageSize;        // out reservado
    volatile kmp_u32 BadSigBatches;    // v5: lotes rejeitados por assinatura FNV
    volatile kmp_u32 ThrottledBatches; // v5: lotes que estouraram o teto de taxa
    volatile kmp_u32 WatchdogFires;    // v5: disparos do watchdog de 60 s
    kmp_u32  _vPad;
} KMP_HDR;                    // 104 bytes
```

| bloco | tamanho | dono |
|---|---|---|
| `KMP_HDR` | 104 B | kernel escreve, user lê |
| `KMP_REQ` (64 ops × 64 B + cabeçalho + `Sig`) | 4.128 B | **user escreve**, kernel lê |
| `KMP_RESP` (64 ops × 64 B + cabeçalho) | 4.152 B | **kernel escreve**, user lê |
| `Data` | 256 KiB | bidirecional, por offset |

Regra de ouro: **nenhum lado escreve no bloco do outro.** Isso elimina a classe de corrida de dado; o que sobra é ordenação, resolvida no §5.

Há `typedef char [cond ? 1 : -1]` (assert de tamanho em tempo de compilação) para `KMP_OP=64`, `KMP_REQ`, `KMP_RESP`, `KMP_HDR=104` e `KMP_SHARED`. Se alguém mudar um campo sem atualizar a conta, **não compila**.

---

## 5. Protocolo de concorrência

Duas threads, dois núcleos, mesma página de memória. Ordem é garantida por barreiras explícitas — não por sorte.

```
USER-MODE                                     KERNEL (KmpServerThread)
1. escreve Req.Ops[i] e o payload
2. Req.Sig = KmpReqSig(Req, KmpSessKey(Hdr.Signature))   ← v5, FNV-1a por lote
3. MemoryBarrier()
4. Req.Seq = ++seq            (publicação)
5. MemoryBarrier()
6. SetEvent(ReqEvt)  ───────────────────────► 7. KeWaitForSingleObject retorna
                                              8. KeMemoryBarrier()
                                              9. valida Magic/Version/Count/PayloadUsed
                                             10. revalida Req.Sig (FNV com a chave da sessão)
                                             11. if (Req.Magic == STOP) break
                                             12. if (Req.Seq == SequenceNumber) continue  (evento espúrio)
                                             13. processa o lote (KmpProcessBatch)
                                             14. KeMemoryBarrier()
                                             15. Resp.AckSeq = Req.Seq   (ÚLTIMO campo)
                                             16. KeMemoryBarrier()
                                             17. KeSetEvent(RspEvt)
18. WaitForSingleObject(RspEvt, 2000 ms)  ◄─┘
19. MemoryBarrier()
20. valida AckSeq == seq  →  só então lê Resp/Data
```

Detalhes que sustentam o desenho:

- Eventos são de **sincronização** (`KeInitializeEvent(..., FALSE, ...)`): cada `SetEvent` acorda exatamente uma espera, não acumula sinal — sem isso o mesmo lote poderia ser processado duas vezes.
- `AckSeq` é escrito **por último** e é o que autoriza o user-mode a ler a resposta. Sem essa ordem, o cliente leria payload pela metade.
- A checagem `Req.Seq == SequenceNumber` descarta eventos espúrios (reuso de evento / sinal perdido).
- **Assinatura por lote (v5):** `Sig` é FNV-1a sobre os campos do `KMP_REQ` (Count, PayloadUsed, Seq e os 64 ops) com chave derivada de `Hdr.Signature` — que vem do seed da carga e nunca transita pelo canal. Lote com assinatura errada é rejeitado (`KMP_ERR_BAD_SIGNATURE`) e contabilizado em `Hdr.BadSigBatches`. É a resposta ao item 5 antigo do §13: qualquer coisa que toque a shared memory sem conhecer a chave da sessão deixa rastro e não executa.

---

## 6. Operações

`KMP_OP` tem 64 bytes: `Kind, Pid, Address, Size, PayloadOff, Status, DataOff, Out0, Out1`.

| op | o que faz | validação antes de tocar em memória |
|---|---|---|
| `NOP` (0) | nada — mede o custo puro do round-trip | — |
| `ECHO` (1) | copia `Size` de `Data[PayloadOff]` para a saída | `KmpRangeOk(cursor, Size, DataBytes)` |
| `FILL` (2) | preenche `Size` bytes com `Out0 & 0xFF` | idem |
| `SUM` (3) | soma `Size` bytes → `Out0` | idem |
| `READ` (4) | `MmCopyVirtualMemory(proc, Address → view, Size)` | `Size != 0` **e** `KmpRangeOk` **e** `PsLookupProcessByProcessId` |
| `GET_BASE` (5) | `PsGetProcessSectionBaseAddress(proc)` → `Out0` | lookup do processo |
| `MOUSE` (6) | `KmInjectMouse(dx=Out0, dy=Out1)` | ponteiro do callback validado (§7) |

### 6.1 O filtro de faixa

```c
static BOOLEAN KmpRangeOk(ULONG64 off, ULONG64 size, ULONG64 limit)
{
    if (off > limit) return FALSE;
    if (size > limit) return FALSE;
    return (off + size <= limit);   // soma em ULONGLONG: não estoura
}
```

É a **linha de frente** contra escrita/leitura fora da seção. Um overflow de pool com tamanho controlado pelo chamador já derrubou a máquina seis vezes no projeto principal; aqui a soma é feita em 64 bits e comparada com o limite, antes de qualquer aritmética de ponteiro.

Também há clamp do lote: `if (req->Count > KMP_MAX_OPS) → KMP_ERR_BAD_PARAMETER`. `Resp.Ops[i]` só é escrito com `i < Count <= 64`.

---

## 7. Injeção de mouse (`km_mouse.h`, 35 KB)

Não usa `SendInput`. Resolve o `MouseClassServiceCallback` no `DeviceExtension` do driver de classe do mouse e chama direto, o que **não** marca o pacote com `LLMHF_INJECTED` (bit que qualquer hook `WH_MOUSE_LL` lê).

Caminho:

```
ObReferenceObjectByName(\Driver\Mouhid) → pilha de devices (até 8 níveis)
  ├─ para cada nível: varre DeviceExtension (até 0x400) procurando CONNECT_DATA
  │    {ClassDeviceObject, ClassService}
  ├─ aceita só se ClassService cair em seção EXECUTÁVEL do driver daquele device
  └─ escolhe o nível mais próximo da CLASSE (índice maior)
```

Execução:

```c
KeRaiseIrql(DISPATCH_LEVEL, &old);     // Mouclass exige DISPATCH_LEVEL
g_MouseCallback(device, &data, &data + 1, &consumed);
KeLowerIrql(old);
```

### 7.1 Blindagem (depois de três bugchecks)

Este bloco é o único do driver que **caminha por estruturas de outros drivers**. Cada ponteiro é validado antes de ser seguido:

```c
static __inline BOOLEAN KmPtrOk(const void* p)
{
    return (p != NULL && MmIsAddressValid((PVOID)p)) ? TRUE : FALSE;
}
```

Aplicado em: `dev`, `dev->AttachedDevice`, `cadeia[i]->DriverObject`, **cada leitura** de `DeviceExtension + off`, o cabeçalho PE (`e_magic`, `e_lfanew` ≤ 0x1000, `NumberOfSections` ≤ 96) e, por último, no próprio `g_MouseCallback` antes do salto — chamar ponteiro inválido em `DISPATCH_LEVEL` é bugcheck imediato, sem exceção tratável.

`MmIsAddressValid` não é garantia absoluta (a página pode sair entre o teste e o acesso), mas elimina o caso comum: campo nulo, ponteiro reciclado, endereço de user-mode.

---

## 8. Ciclo de vida e parada

O driver é mapeado à mão: **não existe unload**. Quem desliga é o handshake.

```
CLIENTE                                         KERNEL
1. Req.Magic = KMP_MAGIC_STOP ('STOP')
2. MemoryBarrier()
3. SetEvent(ReqEvt) ──────────────────────────► 4. acorda, vê Magic != KMP_MAGIC → break
                                                5. LifecycleState = DONE
                                                6. KeMemoryBarrier()
                                                7. Hdr.Ativo = 0        ← prova de parada
                                                8. KeSetEvent(ThreadDone)
                                                9. PsTerminateSystemThread
10. espera Hdr.Ativo == 0 (até 2 s)
11. UnmapViewOfFile / CloseHandle / RegDeleteKey
12. mapper: MmFreeIndependentPages(ImageBase, SizeOfImage)
```

**Por que `Ativo == 0` é obrigatório antes do passo 12:** as páginas do driver são alocadas por `MmAllocateIndependentPages`. Liberar enquanto a thread ainda executa aquele código derruba a máquina (`KMODE_EXCEPTION_NOT_HANDLED` com o IP na imagem que sumiu). Se o sinal não chegar em 2 s, o cliente **não libera** — aceita ~19 KB residentes e inertes em vez do bugcheck. O `SizeOfImage` é lido do PE embutido no cliente, porque a imagem mapeada tem os headers destruídos (`destroyHeader = true`).

---

## 9. Códigos de status

| código | significado |
|---|---|
| `KMP_OK` `0x00000000` | operação executada |
| `KMP_ERR_BAD_VERSION` `0xC0000059` | Magic/Version do lote divergente |
| `KMP_ERR_BAD_LENGTH` `0xC0000023` | `PayloadUsed > DataBytes` |
| `KMP_ERR_BAD_PARAMETER` `0xC000000D` | faixa inválida, leitura parcial, PID inexistente |
| `KMP_ERR_NOT_IMPLEMENTED` `0xC0000002` | op desconhecida, ou `MOUSE` em build sem o bloco |
| `KMP_ERR_TIMEOUT` `0xC0000120` | cliente não recebeu `RspEvt` |
| `KMP_ERR_SHUTDOWN` `0xC0000141` | lote chegou com o servidor parando |
| `KMP_ERR_BAD_SIGNATURE` `0xC000BADB` | v5: assinatura FNV do lote não bateu (alguém tocou na shared memory) |

---

## 10. Integração com o cliente

`DriverInterfaceKEvent` (header-only, ~380 linhas):

| método | transporte |
|---|---|
| `Initialize(svcName, loader)` | cria seção/eventos, escreve a chave, chama o loader, espera `Ready` |
| `ReadMemory(pid, addr, buf, size)` | 1 op `READ` |
| `BatchReadMemory(pid, entries, n)` | **fatia em blocos de 64** (`KMP_MAX_OPS`) e faz 1 round-trip por bloco |
| `GetModuleBase(pid, name)` | 1 op `GET_BASE` (só o módulo principal) |
| `GetProcessId(name)` | `CreateToolhelp32Snapshot` — não passa pelo driver |
| `EnviarUmPacote(dx, dy)` | 1 op `MOUSE` |
| `Cleanup()` | handshake de parada; fecha view/seção/eventos; apaga a chave |

**A armadilha do lote:** o cheat pede até 128 leituras por lote. O protocolo aceita 64. Sem o fatiamento, **metade das leituras sumia em silêncio** e o ESP mostrava metade dos jogadores. O fatiamento está em `BatchReadMemory`.

Timeout de 2 s por round-trip (`KMP_TIMEOUT_MS_DEFAULT`).

---

## 11. Build

```
cl /c /O2 /W3 /GS- /Gy /Z7          →  km_ipc_driver.obj
link /DRIVER /NODEFAULTLIB /SUBSYSTEM:NATIVE
     /ENTRY:DriverEntry /OPT:REF /OPT:ICF
     /DEBUG /DEBUGTYPE:CV /PDB:…\km_ipc_proto.pdb
     /MAP:…\km_ipc_proto.map
```

O `.pdb` e o `.map` **não são luxo**: um bugcheck aponta um RVA dentro do driver mapeado (endereço aleatório). Com o `.map`, `RVA 0x1A92` virou `KmpTraceStep+0x22` e o bug foi encontrado em minutos; sem eles, o mesmo bug custou três hipóteses erradas.

Os artefatos entram no `.exe` como bytes (`PubgExt/Mapper/driver_kevent_bytes.h`), gerados por `tools/gen_driver_kevent_bytes.ps1`, que **recusa** embutir um `.sys` mais velho que o fonte.

---

## 12. Fatos medidos (não estimativas)

| medição | valor |
|---|---|
| tamanho do driver | 17.408 bytes (v5) |
| imagem mapeada (exemplo real) | `0xFFFFB381F0C05000` |
| versão do protocolo em runtime | `0x00000005` |
| `DriverEntry` retornou | `0x0` |
| poke de imagem por round-trip | até 64 ops |
| payload por lote | até 256 KiB |
| timeout de parada / round-trip | 2 s / 2 s |
| teto de taxa (v5) | 20.000 ops/s (`KMP_MAX_OPS_PER_SEC`) |
| watchdog (v5) | 60 s sem lote → limpeza própria (`KMP_WDOG_MS`) |

---

## 13. Limites conhecidos (para a revisão)

Ditos sem enfeite — são o que um revisor deve atacar primeiro:

1. **Um cliente por sessão.** O `Pid` da chave autoriza *quem* abre; não há autorização por operação. Comprometido o processo cliente, o canal é dele.
2. **A view é memória do processo.** Qualquer coisa dentro do processo lê a seção mapeada. Não existe defesa em user-mode; o limite é arquitetural.
3. **A chave do registro é visível durante o boot.** Nome derivado do seed (32 hex), apagada logo depois — mas existe por uma janela de milissegundos.
4. **`MmIsAddressValid` é TOCTOU.** Entre o teste e a desreferência a página pode sair. Mitigação real seria `__try/__except` em volta de cada acesso (custo alto) ou `ProbeForRead` (não vale para memória de kernel).
5. ~~Sem autenticação do lote.~~ **RESOLVIDO na v5:** cada lote leva assinatura FNV-1a com chave derivada de `Signature` (seed da carga, fora do canal); rejeição vira `KMP_ERR_BAD_SIGNATURE` + contador em `Hdr.BadSigBatches`.
6. **`MOUSE` não valida `Pid`.** A op ignora o PID; injeta no device de classe global.
7. ~~Sem limite de taxa.~~ **RESOLVIDO na v5:** bucket de 1 s contra `KMP_MAX_OPS_PER_SEC`; excedente é contado em `Hdr.ThrottledBatches`.
8. ~~Shutdown depende do cliente.~~ **MELHORADO na v5:** watchdog de 60 s (`KmpWatchdog` + work item `KmpLimpezaAgendar`) dispara se o lote parar, e o teardown passou a pegar cada ponteiro com `InterlockedExchangePointer` (quem leva o ponteiro não-nulo libera) porque `KmpShutdown` agora pode rodar em dois contextos: o user-mode e a limpeza do watchdog. Se o processo morrer em silêncio o driver se limpa sozinho em até 1 min — antes era até o reboot.

---

## 14. Checklist de revisão

Perguntas objetivas para quem for analisar:

- [ ] O protocolo de sequência (§5) tem corrida entre `Req.Seq` e a leitura dos ops pelo kernel?
- [ ] `KmpRangeOk` cobre todos os caminhos que escrevem em `Data`? (`ECHO`, `FILL`, `SUM`, `READ`)
- [ ] `Resp.Ops[i]` está sempre dentro de `KMP_MAX_OPS`?
- [ ] `KmpDupFromClient` pode duplicar handle de um processo que não é o dono declarado?
- [ ] O `ObReferenceObjectByHandle` dos eventos aceita handles de tipos errados (seção passada como evento)?
- [ ] `PsTerminateSystemThread` é alcançado em **todos** os caminhos de erro depois do `PsCreateSystemThread`?
- [ ] O `KeWaitForSingleObject` com timeout de 100 ms em `Executive`/`KernelMode` é reentrante com `APC_LEVEL`?
- [ ] `MmCopyVirtualMemory` com destino em memória de kernel (view) é seguro em todos os builds?
- [ ] O IRQL em `KeRaiseIrql` está restaurado em todas as saídas de `KmInjectMouse`?
- [ ] A varredura de `DeviceExtension` (0x400) tem limite confiável para todas as versões do `mouclass.sys`?
- [ ] `Hdr.Ativo = 0` é publicado **antes** de `PsTerminateSystemThread` em todos os caminhos?
- [ ] O cliente pode confundir `AckSeq` de um lote anterior com o atual (wrap de 32 bits)?
