# Reestruturação da comunicação kernel ↔ user-mode — arquitetura proposta

> **Status:** proposta. Nada do projeto principal foi alterado.
> Entregáveis 1 (diagrama), 2 (análise dos arquivos atuais) e 4 (plano de
> testes e critérios). Os entregáveis 3 (protótipo), 5 (benchmark executado) e
> 6 (relatório de riscos medidos) vêm depois de o protótipo compilar e rodar
> numa VM de teste.

---

## 0. Duas correções à premissa, antes de tudo

A proposta assume que shared memory + evento é estritamente melhor. Duas coisas
precisam estar ditas antes de qualquer linha de código, porque mudam a decisão:

**(1) Custo de exposição — é o oposto do que a proposta sugere.**

| | device + IOCTL (hoje) | seção nomeada + KEVENT (proposto) |
|---|---|---|
| nome | `\Device\PUBG_KM_<16 hex do seed>` | `<nome fixo>` em `\BaseNamedObjects` |
| enumerável por outro processo | não (só por tentativa, e o espaço é 2^64) | **sim** (`NtOpenDirectoryObject`) |
| evento | nenhum objeto global | **`\KernelObjects\<nome>` também listável** |
| número de chamadas ao sistema por leitura | 1 (`DeviceIoControl`) | 0 se o evento já estiver sinalizado; 1 se precisar esperar |

Não existe, no escopo pedido, um nome de seção não-adivinhável *e* estável: a
seção precisa ser encontrada pelos dois lados. Com o seed, o nome é derivável —
então dá para manter a mesma propriedade do device **se** o nome da seção e do
evento também forem derivados do seed. Isso resolve a enumeração, ao custo de
reintroduzir o mecanismo de `KmDeriveNames` no lado da seção. **Decisão a
tomar:** nome derivado (equivalente ao atual) ou nome fixo (mais simples, mais
visível). O documento assume **derivado**, por consistência com o que existe.

**(2) O ganho esperado não é onde a intuição aponta.**

| caminho | cópias de payload | ida ao kernel |
|---|---|---|
| `IOCTL` `METHOD_BUFFERED`, 1 op | 2 (user→system, system→user) | 1 IRP + 1 transição |
| `IOCTL` buffered, 64 ops em lote | 2 (do lote) | idem |
| seção mapeada, 1 op | **0** | 0 se não precisar esperar |
| seção mapeada + evento, 1 op | **0** | **2 trocas de contexto** (sinaliza + espera) |

Consequência que o benchmark tem que provar ou derrubar: para **uma operação
pequena e isolada**, o round-trip por evento pode ser **mais lento** que um
IOCTL buffered, porque duas trocas de contexto custam na ordem de
microssegundos, e é exatamente o que um IOCTL evita. O ganho real tende a
aparecer em:

- **lote** (payload grande, uma única ida) — as duas cópias do buffered são o
  custo dominante e desaparecem;
- **polling** (o user-mode já tem o evento sinalizado e não espera nada).

Se a medição mostrar que o lote buffered atual já está perto do shared memory,
a conclusão honesta é **não migrar** — e isso é um resultado válido do projeto.

---

## 1. Diagrama da arquitetura proposta

```
                        PROCESSO DE TESTE (user mode)
┌──────────────────────────────────────────────────────────────────────────┐
│                                                                          │
│  TestDriverClient                                                        │
│    ├── open_channel(seed)     -> ZwOpenSection + ZwOpenEvent             │
│    ├── map_view()            -> ZwMapViewOfSection (1x, no boot)         │
│    ├── submit(Batch)         -> escreve Req, KeSetEvent(user)            │
│    ├── wait(deadline)        -> ZwWaitForSingleObject(AckEvent, timeout) │
│    └── read_results()        -> le Resp e Data direto do view            │
│                                                                          │
│  Telemetry: QPC por operacao -> media, P50, P95, P99, ops/s, CPU time   │
└───────────────┬──────────────────────────────────────┬───────────────────┘
                │                                      │
    ┌───────────▼───────────┐              ┌───────────▼────────────┐
    │ Seção  (dados)        │              │ Eventos  (sinal)        │
    │ Req  →  written by UM │              │ ReqEvt: UM sinaliza     │
    │ Resp →  written by KM │              │ AckEvt: KM sinaliza     │
    │ Data →  bidirecional  │              │                         │
    └───────────┬───────────┘              └───────────┬─────────────┘
                │                                      │
┌───────────────▼──────────────────────────────────────▼───────────────────┐
│                          KERNEL (driver do protótipo)                    │
│                                                                          │
│  Lifecycle Manager                                                       │
│    IoCreateDevice? NÃO. Este protótipo é só seção + evento (sem IRP).    │
│    ZwCreateSection / ZwMapViewOfSection / ObReferenceObjectByHandle      │
│    KeInitializeEvent (SynchronizationEvent) x2                           │
│    PsCreateSystemThread -> ServerLoop                                    │
│                                                                          │
│  ServerLoop                                                              │
│    KeWaitForSingleObject(ReqEvt, Executive, KernelMode, ...)             │
│       |                                                                  │
│       ├── valida cabeçalho: Magic, Version, Count, offsets, tamanhos      │
│       ├── para cada op: valida Kind/Pid/Size/PayloadOff  (Dispatcher)    │
│       ├── processa: leitura/escrita via o caminho já existente           │
│       └── publica Resp; KeSetEvent(AckEvt)                               │
│                                                                          │
│  Telemetry (kernel side): ciclos por lote, ops processadas, erros        │
│  Shutdown: flag Ativo=0, KeSetEvent(ReqEvt), KeWaitForSingleObject(thread)│
│            ObDereferenceObject(seção), ZwUnmapViewOfSection, ZwClose      │
└──────────────────────────────────────────────────────────────────────────┘
```

### Barreiras de memória — o ponto que derrubou o `KmShared.h`

Um `REQ_SEQ` em `ULONG` comum não ordena nada entre núcleos. O protocolo
precisa de barreira explícita nos dois lados:

```
user mode                                  kernel
------------------------------------------ --------------------------------
1. escreve corpo do Batch                    (dormindo no evento)
2. MemoryBarrier()            // _ReadWriteBarrier + barrier de compilador
3. InterlockedIncrement(&ReqSeq)            4. KeWaitForSingleObject retorna
                                            5. barreira: KeMemoryBarrier()
                                            6. le ReqSeq (volatile) == meu?
                                            7. processa
                                            8. KeMemoryBarrier()
                                            9. publica Resp + AckSeq (volatile)
                                           10. KeSetEvent(AckEvt)
11. KeWaitForSingleObject(AckEvt) retorna
12. KeMemoryBarrier()
13. le AckSeq (volatile) e Resp
```

Regra de ouro: **o payload nunca é escrito pelo lado que não é dono dele**, e o
campo de sequência é `volatile LONG` + `Interlocked`/`KeMemoryBarrier`. Sem
isso não há "protocolo", há torcida.

---

## 2. Análise dos arquivos existentes

### `PubgExt\driver\KmShared.h` — especificação útil, código inutilizável

| conceito | reaproveitar? |
|---|---|
| `KM_SHARED_OP` (Kind/Pid/Address/Size/PayloadOff/Status/DataOff/Out0/Out1) | **sim** — bem pensado, cobre read/write/pid/module/cr3 |
| layout de batch (`Req.Ops[64]` + `Resp` + `Data[256 KiB]`) | **sim** — separa cabeçalho de payload por offset |
| separação "UM escreve Req, KM escreve Resp" | **sim** — evita corrida de dado |
| `ReqSeq` / `AckSeq` / `Busy` em `km_u32` | **não** — sem `volatile`, sem barreira, sem atomicidade |
| `Busy` com spin do user-mode | **não** — queima CPU; substituir por evento |
| `Name[64]` ANSI | **não** — o resto do contrato é UNICODE |

Veredito: serve de **especificação**, e é isso que o cabeçalho do arquivo agora
declara. É código morto (o driver que roda não a inclui) e não deve ser
religado.

### `km_safe_source.c` — o que já está resolvido e não deve regredir

Coisas que o protótipo **tem que copiar**, porque custaram tela azul:

- validação por caso antes de qualquer acesso (`inLen`/`outLen`/`Size`);
- `MmCopyVirtualMemory` em vez de tocar VA do alvo;
- ordem do shutdown: parar atendimento → derrubar dispatch → symlink → device,
  e a contagem de handles em `Interlocked`;
- teto de 1 MiB por operação.

Coisas que **não** entram no protótipo: injeção de mouse por callback,
derivação de nome por seed, qualquer coisa de anti-cheat. O protótipo é só
transporte.

### `driver_interface_v3.cpp/.h` — de onde sai o número de comparação

`BatchReadMemory()` já agrupa leituras, mas faz **um `DeviceIoControl` por
operação** internamente. É esse o caminho a medir como "IOCTL individual", e a
comparação honesta exige também um "IOCTL em lote de verdade" (uma chamada,
N ops) — que hoje não existe. Sem ele, o benchmark estaria comparando shared
memory contra o pior caso do IOCTL, e o resultado não valeria nada.

### `common.h` — já centralizado

Fonte única dos IOCTLs, conferida contra o kernel pelo `contract_check.py`.
O protótipo não mexe nele.

---

## 3. Layout do protótipo (isolado, não toca no projeto)

```
SharedProto\
  README.md                     o que é, como compilar, como rodar na VM
  km_ipc_proto.h                PROTOCOLO (POD, sem C++, sem dependência)
  km_ipc_driver.c               driver do protótipo (seção + eventos + thread)
  km_ipc_client.h / .cpp        cliente user-mode
  bench_main.cpp                benchmark: IOCTL vs shm, 1 op vs lote
  build_driver.ps1              cl + link (WDK, /SUBSYSTEM:NATIVE)
  build_bench.ps1               cl + link do benchmark
  RELATORIO_ARQUITETURA.md      ESTE arquivo
  RESULTADOS.md                 preenchido só depois de rodar
```

Regras do protótipo:

- **não** entra na `PubgExt.sln`;
- **não** compartilha header com o cheat (o protocolo é independente);
- driver carregado por `sc create` + `NtLoadDriver` numa **VM com test
  signing**, não pelo kdmapper — mapeamento manual é justamente o que a
  proposta pede para não usar;
- processo de teste próprio, sem jogo e sem anti-cheat.

---

## 4. Plano de testes e critérios de aprovação

### 4.1 Configurações medidas (matriz)

| # | transporte | granularidade | ops por chamada |
|---|---|---|---|
| A | `IOCTL` buffered | individual | 1 |
| B | `IOCTL` buffered | lote (nova implementação) | 64 |
| C | shared memory + evento | individual | 1 |
| D | shared memory + evento | lote | 64 |

Mesmo payload sintético em todas: leitura de 4 KiB de um buffer conhecido no
próprio processo de teste (sem jogo, sem processo alheio). Cada célula com
N = 200 000 operações, 5 repetições, descartando a primeira (aquecimento).

### 4.2 Métricas

- latência por operação: média, P50, **P95, P99**, máximo;
- operações por segundo;
- tempo de CPU do processo de teste (`GetProcessTimes`) e do `System`;
- erros e timeouts;
- RSS do processo (nenhum vazamento de view/handle).

### 4.3 Critérios de aprovação — decididos ANTES de medir

A arquitetura nova só substitui a atual se **todos** valerem:

1. **Correção**: 100% das operações devolvem o byte esperado em 10 milhões de
   ops; zero divergência.
2. **Robustez**: com o servidor kernel congelado (`KeStallExecutionProcessor`),
   o user-mode sai por timeout em ≤ 2 s e o processo não trava.
3. **Sem vazamento**: 200 ciclos de abrir/fechar o canal → contagem de handles e
   de views volta ao valor inicial (conferido por `!handle` no WinDbg).
4. **Shutdown limpo**: 200 ciclos de carga/descarga com Driver Verifier ligado
   (Standard + Concurrency + Memory) sem bugcheck e sem "Driver Unload"
   pendente.
5. **Desempenho**: ganho de **≥ 25% no P99** da configuração de lote (B → D).
   Para operação individual (A → C) **não há meta**: o resultado é informativo,
   e "perdeu" é uma resposta aceitável.
6. **Estabilidade**: desvio-padrão do P99 entre as 5 repetições ≤ 15%.

Se (1)–(4) passarem e (5) não, a conclusão é **manter o IOCTL** e o documento
diz isso sem enfeite.

### 4.4 O que o benchmark NÃO prova

- Nada sobre detectabilidade. As duas arquiteturas são mensuráveis de fora por
  caminhos diferentes, e o número de latência não diz nada sobre isso.
- Nada sobre o jogo. O payload é sintético, no próprio processo de teste.
- Nada sobre anticheat. Fora de escopo por decisão da proposta.

---

## 5. Riscos conhecidos, antes de escrever código

| risco | por que | mitigação |
|---|---|---|
| **`KeSetEvent` + espera = 2 trocas de contexto** | é o custo estrutural do desenho | medir; se perder para IOCTL individual, é resultado |
| **seção visível no object namespace** | nome precisa ser encontrado | nome derivado do seed, como o device |
| **view mapeada é memória do processo** | qualquer coisa no processo lê o view | não é defesa possível em user-mode; registrar como limite |
| **`\KernelObjects` event é global** | evento nomeado é listável | nome derivado + `OBJ_KERNEL_HANDLE` onde couber |
| **driver de teste via `sc create`** | exige test signing ligado | VM dedicada; nunca na máquina principal |
| **`ExAllocatePoolWithTag` deprecado** | aviso `C4996` já no driver atual | no protótipo usar `ExAllocatePool2` |
| **shutdown com operação em voo** | é o bug que já custou BSOD neste projeto | flag `Ativo` + `KeWaitForSingleObject` na thread antes de desmapear |

---

## 6. Próximo passo

Ordem de construção, cada etapa compilando antes da seguinte:

1. `km_ipc_proto.h` — protocolo versionado, POD, com as barreiras documentadas.
2. `km_ipc_driver.c` — lifecycle + ServerLoop + dispatcher, sem nenhuma
   operação privilegiada além de leitura do próprio processo de teste.
3. `km_ipc_client.cpp` — cliente com timeout e telemetria.
4. `bench_main.cpp` — as 4 configurações da matriz.
5. Rodar na VM com Driver Verifier; preencher `RESULTADOS.md`.
6. Só então decidir sobre migrar o projeto principal.
