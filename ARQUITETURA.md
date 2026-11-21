# PubgExt — Arquitetura e Mapa do Projeto

Este arquivo é o mapa do projeto: o que existe, o que está ligado em quê, o que
está morto, o que está quebrado e **por quê** — as decisões e os becos sem saída
valem tanto quanto o código. Começou como "estado do repositório depois de um PC
formatado" e virou o diário técnico do projeto.

**Seções marcadas `HISTÓRICO` descrevem o que foi tentado e abandonado.** Não
são o estado atual. O driver antigo (`Ndu.sys`), a rota BYOVD do `AsIO3`, o
overlay via DWM/`explorer.exe` e o caminho DMA/MemProcFS estão todos
documentados lá, cada um com o motivo de ter morrido.

**Quando este documento conflitar com uma seção antiga, §0 e §11 ganham.**

---

## 0. Estado atual — 27/10/2026

Snapshot do que roda hoje. O resto do documento explica o porquê; as seções
marcadas `HISTÓRICO` descrevem o que foi tentado e abandonado.

| | |
|---|---|
| Alvo | **TslGame 2609.1.3.1** (offsets + decrypt de nome aplicados) |
| OS de teste | Windows 10 Pro 22H2, build 19045.6456 |
| VBS / HVCI | **desligados** (verificado) |
| Driver | **produção**: `SharedProto\km_ipc_driver.c` → `Build\Release\SharedProto\out\km_ipc_proto.sys` (17.408 B), **embutida** no exe via `PubgExt\Mapper\driver_kevent_bytes.h` (regenerado automaticamente; rejeita .sys mais antiga que o fonte). Driver IOCTL safe (`km_safe_source.c` → `PUBG_KM_safe.sys`) continua compilando só para o banco de testes |
| Build do driver | `tools\build_all.ps1 -Target RuntimeBroker` (WDK 10.0.28000.0); teste standalone: `tools\RunSafeTest.cmd` |
| Carga do driver | **BYOVD KEVENT** (`Mapper\intel_driver.cpp` + `kdmapper.cpp`, doador `iqvw64e.sys` assinado pela Intel): páginas independentes com proteção por seção (`.text`→X/R, `.data`→R/W); o doador é descarregado logo após o mapeamento |
| Device | nenhum (KEVENT + `MmGetPhysicalAddress`); escrita no jogo via porta `MmMapIoSpace` transitória do IQV (mapa → memcpy → unmap) |
| Anti-cheat no caminho | **BattlEye v8.3 + Zakynthos** (AC interno da Krafton) — detecção fail-open roda **antes** de qualquer mapeamento |
| Overlay | `DirectOverlay.h` — janela topmost no **próprio processo** |
| Menu | **HOME** · 8 abas (VISUALS, AIMBOT, MISC, WORLD, PLAYERS, SETTINGS, DEBUG, LOGS) |
| Panic key | **END** — encerra o cheat |
| Config | `%USERPROFILE%\Documents\Hunt\Default.json`, auto-salva quando o menu fecha |

### Ordem de carga (o procedimento que funciona)

```
1. TslGame.exe fechado
2. .\mtmacr0.exe                        (o cheat — mapeia o driver embutido)
3. abre o PUBG numa partida
```

O `kdmapper.exe` standalone **não entra na fila**: o mapeamento do driver é
feito de dentro do próprio cheat (ver "Artefatos que importam").

Sem reboot entre testes: cada mapeamento novo faz cleanup completo da sessão
anterior (`KMP_MAGIC_STOP` → confirmação de que o servidor parou **antes** de
liberar as páginas). Os detalhes estão em §3.

### Páginas órfãs — rastreamento em disco (hardening-p4, 27/10)

O mapa BYOVD vive em páginas independentes do kernel, fora de qualquer imagem ou
módulo registrado (executáveis via `MmSetPageProtection`, não `ZwMapViewOfSection`).
Se uma sessão morre sem descarga confirmada — crash do cheat, `taskkill /f`,
queda de energia, watchdog no kernel — essas páginas ficam alocadas e
executáveis na RAM, um achado clássico de walk físico de páginas. O
antídoto: **gravar no mapa, apagar só depois de liberar**. `%LOCALAPPDATA%\RenderCache\kmp_pages.bin`
(base, tamanho, bootId) é escrito dentro de `SubirDriverKevent`, assim que o
driver responde READY, e só é apagado quando `MmFreeIndependentPages` volta
com sucesso. Toda inicialização com o jogo fechado tenta a liberação primeiro
(doador IQV → free → unload); se falhar, o registro permanece e a próxima
tentativa refaz. Liberar as próprias páginas de dentro do kernel foi rejeitado:
o retorno cai nas próprias páginas já liberadas (v2.0 pagou uma
`KERNEL_SECURITY_CHECK_FAILURE` nessa rota), e pool do sistema não serve de
pousada porque página de pool é NX.

### Artefatos que importam

O cheat em execução precisa de **1 arquivo** em `Build\Release\PubgExt\`:

```
mtmacr0.exe       1.039.360 bytes   o cheat (overlay + leitura + ESP + RCS)
```

**Ele carrega o driver embutido.** O `.sys` está dentro do binário
(`PubgExt\Mapper\driver_bytes.h` — 16.896 bytes, os mesmos do
`PUBG_KM_safe.sys`) e é mapeado no `InitializeFeaturesThread` por
`LoadDriverInternal()` ([AutoLoader.h](PubgExt/Mapper/AutoLoader.h)), quando o
`TargetProcess.driver.Initialize()` não acha device já aberto. O `kdmapper.exe`
standalone **não é necessário**.

O `PUBG_KM_safe.sys` continua em disco em `Build\Release\PubgExt\` (16.896
bytes) para **desenvolvimento/depuração** — é ele que o `DriverSelfTest` e o
`TestHarness` exercitam quando o driver é carregado à mão.

Mais o `imgui.ini` e o `logs.txt`, que o app gera sozinho.

### O driver ANTIGO não existe mais — não use como referência

`km_driver_source.c` (→ `PUBG_KM.sys`, `Ndu.sys`, `Ndu_com_canal.sys`)
está **aposentado**. Ele derrubou a máquina **6 vezes**, todas
`PAGE_FAULT_IN_NONPAGED_AREA (0x50)`, e os binários foram para `_lixo\`.
O fonte continua na raiz só como referência histórica.

Qualquer trecho deste documento que fale do `Ndu.sys` como "o driver" é passado
— estão marcados como `HISTÓRICO`.

### Verificações antes de rodar

O `verify.ps1` **não existe mais**. Ele ainda é citado em vários pontos deste
documento como se existisse — é resíduo, e este parágrafo é a correção. A
verificação de hoje são três scripts que existem de fato:

```powershell
# 1. o driver compila
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_safe_driver.ps1

# 2. o app compila (as duas variantes)
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_all.ps1 -Target RuntimeBroker -Variant both

# 3. as duas linguagens estão completas
powershell -NoProfile -ExecutionPolicy Bypass -File tools\check_lang.ps1
```

Para **exercitar o driver já carregado**, sem buildar nada:

```powershell
tools\RunSafeTest.cmd
```

O `RunSafeTest.cmd` confere que o `TslGame.exe` está fechado, carrega o
`PUBG_KM_safe.sys` com o kdmapper, mostra o **último** bloco do
`C:\Windows\PkmSafe.txt` e roda o `TestHarness.exe`.

Para testar **só o IOCTL de mouse**, o `tools\probe_driver.ps1` é mais direto —
`-UnitId` e `-Flags` são parametrizáveis justamente para varrer as combinações
sem recompilar o driver:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\probe_driver.ps1 -UnitId 1 -Flags 0
```

### Sonda SEM driver — o teste seguro

Existe porque o único teste em runtime que tínhamos exigia carregar o
`Ndu.sys` — e o `Ndu.sys` derrubou a máquina com `0x50`. Hoje o teste com driver
é o `TestHarness.exe` (§3), mas esta sonda continua valendo como o teste que
**não pode** derrubar nada. O
[DriverlessProbe.cpp](PubgExt/DriverlessProbe.cpp) roda **sem carregar nada**:

```powershell
cd Build\Release\PubgExt
.\mtmacr0.exe
type logs.txt
```

Ele testa, contra o próprio `mtmacr0.exe`:

| teste | o que prova |
|---|---|
| `ParsePattern` 9/9 | a sintaxe do padrão de assinatura está certa |
| `PE: MZ / PE\0\0 / SizeOfImage / machine` | o cabeçalho é lido corretamente |
| `FindSignature` (espelho local) | o **algoritmo** de scan acha o que tem que achar |
| scan negativo | e **recusa** o que não existe — sem isto, um scan que sempre devolve "achei" passaria |
| `e_lfanew` memória == disco | a referência local é a verdade |
| `__readcr3()` | PULADO em modo usuário (privilegiada) |

Resultado no estado atual:

```
[+] ParsePattern (puro)                9/9 padroes ok
[+] PE: assinatura MZ                  e_magic=0x5A4D e_lfanew=0x100
[+] PE: assinatura PE\0\0              sig=0x00004550
[+] PE: SizeOfImage plausivel          0xBE000 bytes
[+] PE: machine x64                    0x8664
[+] FindSignature (MZ..PE)             offset=0x0 (esperado 0)
[+] scan negativo (recusa)             nao achou, correto
[+] e_lfanew: memoria == arquivo       mem=0x100 disco=0x100
[~] __readcr3() do processador         PULADO (instrucao privilegiada)
RESULTADO (sem driver): 8 passaram, 0 falharam, 1 pulados
```

**O que ela NÃO prova:** nada sobre leitura de memória de outro processo. O
`READ_VIRT_PHYS` (page-table walk) e o `GET_CR3` só o self test completo
prova, e ele precisa do driver.

#### Três bugs que ela achou em si mesma

A primeira execução dela **crashou** e reportou falha falsa. Os três eram
meus, no teste:

1. **`strlen` em string com `\x00`.** O caso `"?? ??"` tem bytes esperados
   `"\x00\x00"`, então `strlen` devolvia **0** e a comparação virava
   vazio-contra-vazio. O `ParsePattern` estava certo; o teste é que era um
   no-op. Agora os bytes esperados têm tamanho explícito.

2. **`__readcr3()` é privilegiada em modo usuário.** Levanta `0xC0000096`
   (privileged instruction) e mata o processo. Precisou de `__try/__except`
   — e o `__try` teve que ir para uma **função separada**, porque o
   compilador recusa SEH em funções com objetos que precisam de destrutor
   (`C2712`). Isso obrigou a adicionar `/EHa` ao vcxproj.

3. **`memória == arquivo` nos 4 KiB inteiros do cabeçalho.** O loader do
   Windows **modifica** os headers em memória (realoca imports, aplica
   relocations, marca seções), então a comparação nunca ia bater. Trocado por
   `e_lfanew`, que é estável.

Os checkers individualmente:

```powershell
python tools\dump_driver\contract_check.py             # contrato user<->kernel
python tools\dump_driver\contract_check.py --selftest  # prova que o checker funciona
python tools\dump_driver\pagewalk_check.py             # aritmetica do walk
python tools\dump_driver\asio3_check.py                # IOCTLs do AsIO3 vs o binario
python tools\dump_driver\pubg_decrypt.py --selftest    # decrypt de FName e health
python tools\dump_driver\offset_audit.py               # Globals.h vs dump
```

Contrato tem que dar `CONSISTENTE` e os selftests `5/5`, `9/9` e `8/8`. Se não
der, **não rode**: os dois lados compilam felizes com números trocados, e foi
esse o bug que causou o overflow de pool descrito em §3.

Depois de carregar o driver, o teste em runtime é rodar o `mtmacr0.exe`
**sem o jogo aberto** — ele cai no self test automaticamente e escreve o
resultado no `logs.txt`. Ver §3, "Banco de teste em runtime".

---

## 1. Visão geral

```
                       ┌──────────────────────────┐
                       │      PUBG (TslGame.exe)  │
                       └────────────┬─────────────┘
                                    │ memória virtual (só leitura)
                       ┌────────────▼─────────────┐
                       │  PUBG_KM_safe.sys (km)   │  MmCopyVirtualMemory
                       │  \Device\PUBG_KM_<rand>  │  (AutoLoader embutido)
                       └────────────▲─────────────┘
                                    │ DeviceIoControl
                       ┌────────────┴─────────────┐
                       │  mtmacr0.exe             │  loop de memória + ImGui
                       │  (PubgExt\Main.cpp)      │  overlay D3D11 transparente
                       └──────────────────────────┘
```

Cadeia de artefatos:

| # | Artefato | Fonte | Build |
|---|----------|-------|-------|
| 1 | `PUBG_KM_safe.sys` | `km_safe_source.c` | `tools\build_safe_driver.ps1` (cl+link, WDK 28000) |
| 2 | `PubgExt\Mapper\driver_bytes.h` (o `.sys` embutido) | `PUBG_KM_safe.sys` (16.896 bytes) | gerado |
| 3 | `mtmacr0.exe` | `PubgExt.sln` → `PubgExt\*.cpp` | MSBuild (`build_all.ps1 -Target RuntimeBroker`) |
| 4 | `TestHarness.exe` | `tools\TestHarness.cpp` | cl+link (**fora do runtime**) |

O alvo de verdade é o **3**: o `mtmacr0.exe` faz overlay, ESP e RCS **e mapeia o
`.sys` embutido** — o item 2 não é um arquivo solto em runtime. O
`PUBG_KM_safe.sys` faz a leitura de memória. O `TestHarness.exe` não participa
da operação — ele existe para provar que o driver responde e que os casos
perigosos são recusados em vez de derrubar a máquina.

**HISTÓRICO:** a cadeia antiga tinha `Loader.exe` + `DwmRenderer.dll`
(manual map dentro do `explorer.exe`, named pipe `\\.\pipe\PubgDwmPipe`) e o
`HardenStub.sys` (experimento de assinatura por doador). Nada disso é usado
hoje — está em `_lixo\_legacy\alt_overlays\` e `_lixo\driver_stub\`, com o
motivo em §3b e §5.

---

## 2. mtmacr0.exe

> O binário sai como `mtmacr0.exe` (`<TargetName>` no `PubgExt.vcxproj`). O
> `-Target RuntimeBroker` do `tools\build_all.ps1` é só o **rótulo** do script e
> continua valendo — ele builda o `PubgExt.sln`, cujo projeto se chama
> `PubgExt`.

### Fluxo

```
WinMain → Run()
  timeBeginPeriod(1)
  SetUpConfig()                         Config\ConfigUtilities.cpp
  DirectOverlay::Run(callback)          OS-ImGui\DirectOverlay.h
  │   ├─ CreateOverlay()   janela primeiro, identidade aleatoria
  │   ├─ CreateDevice()    D3D11 + swapchain + RTV
  │   └─ loop de render
  │        └─ primeiro frame dispara InitializeFeaturesThread():
  │             TargetProcess.Init("TslGame.exe")   Memory\Memory.cpp → driver
  │             TargetProcess.FixCr3()              ↳ hoje: só um teste de leitura
  │             EngineInstance->Cache()             SDK\Engine.cpp
  │             std::thread(MemoryLoop)
```

**Mudança de ordem (importante).** Antes o overlay só subia *depois* de
`InitializeFeatures()` + `FixCr3()`, e os dois têm `while()` de espera. Se o
jogo não estava aberto, o processo ficava preto e sem explicação. Agora:

1. a janela abre primeiro, com banner de status no canto;
2. a inicialização roda numa thread separada (não trava a UI);
3. `RenderFrame` não desreferencia mais `EngineInstance` sem checar nulo
   (era a causa do `[FATAL] 0xC0000005` do `logs.txt`).

`MemoryLoop` roda em FPS configurável (padrão 144 Hz):

```
Cache()
  UpdatePlayers()        SDK\ActorEntity.cpp
  UpdateGrenades()
  RefreshViewMatrix()    SDK\Camera.cpp
  → copia o snapshot para GSharedData sob GDataMutex
  → Aimbot::Tick()       SDK\Aimbot.cpp
```

A thread de render copia o snapshot, resolve a câmera, desenha menu ImGui
(**8 abas** — ver §2b), ESP (`ESP\PlayerEsp.cpp`) e radar (`ESP\Radar.cpp`).

### Acesso à memória

`Memory` é um singleton (`inline Memory TargetProcess`). Toda leitura passa
por `Memory::Read` → `DriverInterfaceV3::ReadMemory` → `DeviceIoControl`.

Ponto importante: **`Memory` não fala MemProcFS**. Não existe `vHandle`, não
existe `VMMDLL_*`. A DMA foi substituída pelo driver KM em algum momento e a
camada `vmm`/`leechcore` acabou arquivada em `_lixo\_legacy\dma_paths\` (§4).

### Offsets — build 2609.1.3.1

`Globals.h` → `struct OFFSET` é a fonte única. **Build de referência:
TslGame 2609.1.3.1.**

Os únicos símbolos que são RVA dentro do módulo do jogo (o resto é
deslocamento de campo dentro de uma struct):

```
XenuineDecrypt    0x10EC3828   funcao de decrypt de ponteiro
UWorld            0x128271E8
GNames            0x12ABD1B0
```

O `PhysxSDK = 0x123BE088` já esteve nesta lista (Fase 1 do visible check via
PhysX) e foi **removido** junto com o PhysX — ver §14. Nenhum offset de PhysX
continua no código.

**`XenuineDecrypt` é o offset que mais dói errar.** No build anterior ele era
`0x11757E28`; com o valor velho o `UWorld` não decifra, nenhum ator passa no
`IsPlayer` e o **ESP fica vazio** — sem erro, sem log, sem crash. Offset e
decrypt andam juntos: os dois mudaram neste build.

`GNames` tem indireção própria: `GNames_offset = 0x10` (onde está o ponteiro do
array de chunks) e `ElementsPerChunk = 0x4164` (quantos FName por chunk).

Os campos que o ESP/RCS/AIMBOT realmente usam:

| Campo | Offset |
|---|---|
| `ObjectID` | `0x28` |
| `GameInstance` | `0x7F8` |
| `LocalPlayers` | `0x40` |
| `Actors` | `0xB0` |
| `CurrentLevel` | `0x8D0` |
| `Mesh` | `0x5B0` |
| `RootComponent` | `0x3E8` |
| `AcknowledgedPawn` | `0x4A8` |
| `PlayerController` | `0x38` |
| `PlayerCameraManager` | `0x4D0` |
| `CameraFov` | `0x10C0` |
| `CameraPos` | `0x10C4` |
| `CameraRot` | `0x10B4` |
| `PlayerState` | `0x428` |
| `PlayerName` | `0x420` |
| `TeamNumber` | `0xB80` |
| `bIsABot` | `0x468` |
| `HeaFlag` | `0x110` |
| `WeaponProcessor` | `0x958` |
| `EquippedWeapons` | `0x210` |
| `DroppedItem` | `0x470` |
| `ItemTable` | `0xB0` |
| `ItemID` | `0x274` |
| `RecoilValueVector` | `0x11B0` |
| `VerticalRecovery` | `0x11B8` |
| `ControlRotation_CP` | `0x674` |
| `RecoilADSRotation_CP` | `0x824` |
| `bIsScoping_CP` | `0x865` |
| `bIsDBNO_CP` | `0x8F2` |

**Inconsistência conhecida, no código:** o comentário de cabeçalho do
`Globals.h` ainda diz `2609.1.2.11` enquanto vários campos já trazem
`// dump 2609.1.3.1`. É "só" comentário, mas é exatamente o tipo de coisa que
já custou rodadas neste projeto (ver §11). Corrigir.

### O decrypt de índice de nome — 2609.1.3.5 (o que roda hoje)

A fórmula correta veio do **dump do usuário** (não foi deduzida):

```cpp
DWORD DecryptCIndex(DWORD valor)
{
    DWORD xored = valor ^ 0xB753B1E8u;
    return ((xored << 19) | ((xored >> 13) & 0x70000u))
           ^ _rotr(xored, 29) ^ 0x0CAFC916u;
}
```

| parâmetro da lista | campo no `Globals.h` | o que faz |
|---|---|---|
| `DecryptNameIndexXorKey1` = `0xB753B1E8` | `NameIndexXor1` | XOR inicial |
| `DecryptNameIndexDval` = `0x13` = 19 | `NameIndexShl` | **shift esquerda** |
| `DecryptNameIndexSval` = `0xD` = 13 | `NameIndexShr` | **shift direita** |
| `DecryptNameIndexXorKey3` = `0x00070000` | `NameIndexMaskRight` | máscara do termo da direita |
| `DecryptNameIndexRval` = `0x1D` = 29 | `NameIndexRotr` | rotação |
| `DecryptNameIndexXorKey2` = `0x0CAFC916` | `NameIndexXor2` | XOR final |
| `DecryptNameIndexRor` = `0x1` | `NameIndexRor` | **não usado** nesta forma |

#### O erro que custou uma rodada (leitura errada da mesma lista)

O código usava `NameIndexRotr` para o `Ror` = 1, tratava o `Rval` = 0x1D como
"não usado", **deduzia** `shl = (48 - rotr) & 31 = 15`, **mascarava** o termo da
esquerda com um `0xFFFF0000` (campo `NameIndexShlMask`) que não existe nesta
fórmula, e **ignorava** o `>>` (Sval). Os campos foram renomeados para dizer o
que fazem, e o `NameIndexShlMask` foi **removido** do `Globals.h`.

O sintoma tinha **duas caras, uma causa**:

* `[CENSO] cls='WHƒì H‹ÚH…ÒtH‹H‹ÊÿÐ'` — índice de FName caindo em memória
  aleatória, nome de classe = bytecode x86;
* `[STATE] Actors=0` **sempre** (`FrameErr=0`, `PCM` válido, `InMatch=1`) — como
  nenhum nome casa, `Engine::IsPlayer` (`Engine.cpp`, chamado no laço de
  `Engine::Cache`) devolve 0 para todo ator, o `continue` dispara e a lista
  `actors` nunca é preenchida.

#### A lição: rank 32/32 NÃO prova correção

O autoteste antigo media só o **rank da parte linear sobre GF(2)** e imprimia
`rank=32/32 -> ok` — e a fórmula errada **também** dava 32/32. O
`SelfTestDecryptCIndex` agora cobre três coisas e diz no log o que cada uma
prova:

1. **rank 32/32** — bijeção (não há colisão de índice), *só isso*;
2. **ida-e-volta** — a inversa da parte linear é derivada por Gauss-Jordan e
   confere `inversa(DecryptCIndex(x)) == x` em 544 amostras (pega erro de
   implementação, não pega constante trocada);
3. **forma do dump** — compara com a fórmula literal do dump escrita com as
   constantes na mão. **É o único que pega Dval/Sval/Rval/Ror trocados** — o erro
   que aconteceu. O `pubg_decrypt.py --selftest` faz o mesmo, e ainda roda um
   **controle negativo** com a leitura antiga, que tem rank 32/32 e diverge do
   dump em 552/552 amostras.

Linha de boot esperada:

```
[+] DecryptCIndex build 2609.1.3.5: k1=0xB753B1E8 shl=19 shr=13 mask=0x00070000 rotr=29 k2=0x0CAFC916 | Ror=0x1 nao usado | rank=32/32 (so' bijecao) round-trip=544/544 (inversa derivada) ref-dump=ok (forma do dump) -> ok
```

Nada disso prova que as constantes da lista batem com o binário do jogo: **só o
jogo prova**.

### HISTÓRICO — o decrypt de índice de nome na 2609.1.3.1

`DecryptCIndex` decifra o índice de FName. Neste build (2609.1.3.1) ele mudou
**de novo**, e mudou em bloco — não adianta trocar só uma constante:

```
xored = value ^ 0x4CC9D1B1;
return ((xored << 9) & 0xFFFF0000) ^ _rotr(xored, 7) ^ 0x58564FDF;
```

| | build anterior (2609.1.2.11) | build atual (2609.1.3.1) |
|---|---|---|
| xor de entrada | `0xB7CCC906` | `0x4CC9D1B1` |
| shift (`<<`) | 29 | **9** |
| máscara | `0x1FFF0000` | `0xFFFF0000` |
| rotação (`_rotr`) | 19 | **7** |
| xor final | `0xBAD3F020` | `0x58564FDF` |

Os parâmetros vivem no `struct OFFSET` e o `SelfTestDecryptCIndex()` roda no
boot e loga. No `logs.txt` real daquele build:

```
[+] DecryptCIndex: shl=41 shr=0 mask=0xFFFF0000 rotr=7 k1=0x4CC9D1B1 k2=0x58564FDF -> ok
```

(o `shl=41` do log era `48 - rotr`, ou seja "de onde os bits saem"; o
deslocamento aplicado era 9. **Esse `48 - rotr` era um invariante inferido de
dois builds — a 2609.1.3.5 o derrubou**, ver a seção acima: lá o `shl` é o
`Dval` = 19 e a rotação é o `Rval` = 29. Não deduza `shl` de `rotr` de novo.)

**Sem isto o `GetNames` não resolve NENHUM nome** → `IsPlayer` falha em tudo →
**ESP vazio**. Junto com o `XenuineDecrypt`, é o par que anda a cada patch.

### HISTÓRICO — o bug do `48 - rotr` (build 2609.1.2.11, corrigido)

O `DecryptCIndex` estava com o shift **errado** — e o erro não era de
constante desatualizada, era de fórmula. O código tinha:

```cpp
const uint32_t shlBits = 32u - rotr;      // 32 - 19 = 13   <-- ERRADO
```

O correto é **`48 - rotr`**. Dois builds confirmam:

| build | rotr | shl | mask | bits do `<<` | bits da mask |
|---|---|---|---|---|---|
| 2609.1.2.11 | 19 | **29** | `0x1FFF0000` | 29..31 | 16..28 |
| anterior | 20 | **28** | `0x0FFF0000` | 28..31 | 16..27 |

Nos dois, os bits do `<<` e os da mask são **disjuntos** — é assim que o OR
faz sentido. Com `32 - rotr` (= 13) o `xored << 13` produz bits 32..44, que a
máscara de 32 bits **corta**: o termo vira zero.

Consequência medida (`tools\dump_driver\pubg_decrypt.py`):

```
com 32 - rotr (13):   rank da parte linear = 13/32
                     0x00000000 e 0x00000001 COLIDEM
com 48 - rotr (29):   rank da parte linear = 32/32
                     8/8 ida-e-volta, 0 colisoes
```

Ou seja: **o decrypt era insensível a 19 dos 32 bits** da entrada. Mais da
metade dos índices de FName colidia, e os nomes sairiam errados mesmo com o
`GNames` certo. É o tipo de bug que não estoura — só faz o ESP mostrar nome
trocado e você culpar os offsets.

A verificação agora é automática:

```powershell
python tools\dump_driver\pubg_decrypt.py --selftest
```

e o `Engine::SelfTestDecryptCIndex()` roda no boot: rank (bijeção),
ida-e-volta (inversa derivada) e a comparação com a **forma literal do dump** —
que é o que pega constante trocada. Rank 32/32 sozinho **não** provava nada:
foi exatamente o que deixou passar a leitura errada da 2609.1.3.5 (ver a seção
"O decrypt de índice de nome — 2609.1.3.5").

A implementação de referência em Python também lê os parâmetros do
`Globals.h`, então quando o jogo atualizar basta trocar as constantes lá.

O `PubgExt\offsetdump.txt` que existia aqui era de um build **anterior** e
divergia em 33 de 39 offsets — foi movido para
`_legacy\offsetdump_OLD_build.txt`. Não use como referência.

Para comparar um dump novo contra o que está no código:

```powershell
python tools\dump_driver\offset_audit.py --dump caminho\do\dump_novo.txt
python tools\dump_driver\offset_audit.py --verbose     # mostra o mapa de nomes
```

O casamento de nomes entre o dump e o `Globals.h` é automático (normalização →
hints explícitos → fuzzy conservador). Cobre **182 de 182** símbolos do dump
antigo, **sem chute**: o que não casar entra em "sem casamento" em vez de
gerar uma divergência falsa.

Discriminação verificada:

| entrada | resultado |
|---|---|
| dump idêntico ao código | 203/203 batem, **0 divergências**, exit 0 |
| dump do build anterior | 47/182 batem, **135 divergências**, exit 1 |

### Campos não verificados (lista do 2609.1.2.11 — continua valendo)

A lista desse build **não trouxe todos** os campos que o SDK antigo tinha. Os
que faltaram continuam no `Globals.h`, numa seção marcada `NAO VERIFICADOS`,
com o valor do build anterior:

```
WorldToMap, ActorsForGC, FeatureRepObject, ItemsArray, Durabilitymax,
MatchId, Ping, GObjects,
bIsInVehicle_CP, bIsParachuting_CP, bIsFreefalling_CP,
bEmergencyPickup_Flying_CP, bIsReviving_CP, bIsSwimming_CP,
MortarEntity, MortarRotation
```

São 16 campos. `SubInstances`, `MortarElevation` e `MortarYaw` **estão**
atualizados — vieram na lista nova.

**Visibilidade do ESP.** O `LastRenderTimeOnScreen` do build antigo (`0x75C`)
não tinha fonte em dump nenhum e foi **substituído**: o certo é o bitfield
`bRecentlyRendered` — `RecentlyRendered = 0xBE8`, em `UPrimitiveComponent`,
1 = o componente foi renderizado neste frame. É o que o aimbot precisa para
decidir visibilidade. O `LastSubmitTime = 0x758` veio do dump 2609.1.3.1.
Sintoma se estiver errado: todo mundo invisível ou todo mundo visível no ESP.

---

## 2b. Nome real do loot — a cadeia que funciona

O ator no chão é um `DroppedItemGroup` genérico: a classe dele **não** diz o
que é o item. O nome de verdade está depois de dois ponteiros e uma tabela:

```
ator + DroppedItem (0x470)    -> ponteiro Xe-criptografado
xe_decrypt(...)               -> o UItem
    UItem + ItemTable (0xB0)  -> ponteiro da tabela
        + ItemID (0x274)      -> índice de FName **CRU**
GetNamesFromList(rawId)       -> 'Item_Weapon_M16A4_C'
```

**O erro que travou isso por várias rodadas:** aplicar `DecryptCIndex` no
`ItemID`. Esse decrypt é para o **`ObjectID`** (que a Xenuine ofusca); o
`ItemID` é índice **cru** de FName. Decifrar corrompia **100% dos itens**, tudo
saía como `UNKOWNNAME` — sintoma que parece offset errado e era cifra onde não
havia cifra.

O `Engine::UpdateLoot` faz os dois caminhos:

1. **medido e confirmado** — `ItemTable` → `ItemID` → `GetNamesFromList`
   **sem** decrypt (é o que dá `Item_Weapon_M16A4_C`);
2. **fallback** — nome da classe do `UItem`, via `ObjectID` + `DecryptCIndex`,
   para quando a tabela não responde.

O log `[LOOTNAME]` (ligado por `Debug logs` em DEBUG) imprime cada nome **uma
vez por objId**, não por frame — a versão anterior repetia a mesma linha
dezenas de vezes por segundo e enterrava o log.

---

## 2c. RCS (no-recoil) — feed-forward do acumulador

O RCS **não** é feedback de câmera. É feed-forward do acumulador de recuo que o
próprio jogo drena:

- a arma tem `RecoilValueVector` (FVector2D, `0x11B0`) e `VerticalRecovery`
  (float, `0x11B8`);
- a cada frame o jogo **drena** uma fatia do `RecoilValueVector` para o
  `ControlRotation`. **A fatia que sumiu entre duas leituras nossas é
  exatamente o que a câmera acabou de levar.**

```
dropY  = max(0, prev_rvv.y - rvv.y)
dropR  = max(0, prev_vr   - vr  )
deg    = InputPitchScale (2.5) * (dropY - dropR)
counts = deg / degPerCount        // degPerCount varia com a FOV
```

O eixo X (yaw) sai do `dropX` do `RecoilValueVector`, com o mesmo tratamento.

**O que foi tentado e NÃO funciona** (medido, não suposto):

| tentativa | por que falhou |
|---|---|
| usar `RecoilADSRotation_CP` (`0x824`) como ângulo | **não é ângulo** — é fator de blend: reseta em `1.0` e decai para ~`0.65` |
| feedback puro sobre a câmera | oscila e **briga com a mão do jogador** |
| congelar a adaptação | impede a correção do erro do próprio jogo |

**Calibração:** é **um número só**, o `Deg per Count`. O teto do integrador
deve ficar folgado — ele é rede de segurança contra pico de leitura, não ajuste
fino. O log `[RCS]` (também sob `Debug logs`) mostra `rvv`, `vr`,
`dropY/dropR/dropX`, `deg`, `fov`, `dpc` e os counts aplicados:

```
[RCS] weapon=0x231D45A1590 rvv=(0.017,0.306) vr=0.683 | dropY=0.0537 dropR=0.0407 dropX=0.0019 | degY=0.032 degX=0.005 | fov=72.1 dpc=0.0365 -> (0,1)
```

---

## 2d. Menu e configuração

**8 abas**, agrupadas com `ImGui::SeparatorText` para não virar uma parede de
checkboxes:

| Aba | Conteúdo |
|---|---|
| VISUALS | ESP de jogador, mundo, cores |
| AIMBOT | geral, predição, **recoil (RCS)** |
| MISC | espectadores, radar, overlay, mira central custom |
| WORLD | loot ESP + **catálogo de itens** |
| PLAYERS | lista/jogadores |
| SETTINGS | configuração, keybinds, performance |
| DEBUG | **Debug logs (CENSO / LOOTDIAG)** e diagnósticos |
| LOGS | o `logs.txt` visto de dentro do menu |

### Aba WORLD — catálogo de loot que se auto-popula

Não dá para ter lista fixa de itens no código: o PUBG tem centenas e o nome só
aparece em runtime (via GNames). Então o catálogo **se auto-popula** conforme o
cheat vê itens no chão:

- `G_LootCatalog` em `ESP\LootCatalog.h` — mapa `nome BRUTO -> visível`;
- **`G_LootCatalogMutex` é obrigatório**: o mapa é escrito na **thread de
  memória** (`Engine::UpdateLoot`) e lido/iterado na **thread de render** (aba
  WORLD). Inserção concorrente com iteração em `std::map` rebalanceia a árvore,
  invalida o iterador e **crasha**. A renderização tira um snapshot e solta o
  lock antes de chamar ImGui;
- **10 categorias** (`LootCategory`): Weapons, Ammo, Attachments, Health, Boost,
  Helmet, Backpack, Vest, Throwables, Other — cada uma com checkbox geral +
  itens individuais;
- a **chave persistida é o nome bruto** (`Item_Weapon_M16A4_C`); o **label** é o
  nome limpo. O nome limpo troca, traduz e corta prefixo, então não serve como
  chave estável;
- a classificação é por prefixo medido em log real, e a **ordem importa**:
  `Item_Attach_` é testado **antes** de arma, senão
  `Item_Attach_Weapon_Lower_ThumbGrip_C` vira arma e desaparece quando o
  usuário desliga "Weapons".

### Auto-save

A config salva sozinha quando o menu fecha (**HOME**):

```
%USERPROFILE%\Documents\Hunt\Default.json
```

O auto-save tem throttle interno (2 s) para não escrever em disco a cada frame.

**Regra de ouro:** adicionar campo de config tem **TRÊS partes** — a struct, o
`ToJson` e o `FromJson`. Esquecer save/load faz o valor **desaparecer sem erro
nenhum** (ver §11).

### Panic key — END

O `END` encerra o cheat na hora. Ele existe porque **`taskkill` /
`Stop-Process` não conseguem matar o processo elevado** — numa partida, é a
única saída sem apelar para o gerenciador de tarefas.

---

## 2e. Filtros de entidade — tirados de um CENSO real

Os filtros de veículo e de loot **não** são suposição: saíram do CENSO de
classes (aba DEBUG → `Debug logs`), que lista toda classe de ator do mapa.

### Veículo

O filtro antigo era por substring e estava **ao contrário do certo**:

```
PEGAVA (errado):  P_VehicleImpact_Wood_BP_C         (efeito de impacto)
                  P_Vehicle_Muffler_BP_Motorcycle_C (escapamento)
                  BP_VehicleTrunk_C                 (porta-malas)
PERDIA (todos):   BP_Dirtbike_C  BP_PicoBus_C  BP_Food_Truck_C
                  BP_Bicycle_C   BP_Blanc_C
```

O que separa os dois grupos é o **prefixo**: `BP_` é ator real do mundo, `P_`
é partícula/efeito/peça. A regra atual:

1. exclui prefixo `P_`;
2. exclui nomes com `Trunk/Wheel/Door/Seat/Bumper/Window/Component/Muffler`
   (e afins) — são atores separados que **pertencem** ao veículo;
3. casa contra nomes reais: `Dirtbike`, `PicoBus`, `Food_Truck`, `Bicycle`,
   `Blanc`, `Vehicle`, `UAZ`, `Dacia`, `Buggy`, etc.

### Loot

Casa com `DroppedItem`, `ItemPackage`, `Weap*`, `Carapackage`,
`TslDropMagazine`… e o detalhe que custou tempo: **o airdrop é
`Carapackage_RedBox_C`, com A**. O filtro antigo procurava `CarePackage` com E
e **nunca** casava com nada.

---

## 2f. Onde o ESP desenha cada coisa — mapa medido (17/10/2026)

Existe uma lenda de que **caixa e esqueleto não estão no `PlayerEsp.cpp`** ("lá
só tem barras e texto"). Medido com `grep` no fonte que compila:

```
PubgExt\ESP\PlayerEsp.cpp
  643-694   DrawPlayerEsp   -> CAIXA   (3x Gui.Rectangle)
  898-955   DrawPlayerEsp   -> ESQUELETO (14x Gui.Line)
  807-895   DrawPlayerEsp   -> barra de vida (drawList->AddRectFilled direto)
  959-963   DrawPlayerEsp   -> texto (EspStrokeText por elemento)
 1142-1231  DrawVehicleEsp  -> veiculo: painel + texto (SEM caixa)
 1239-1345  DrawLootEsp     -> loot: painel + texto (SEM caixa)
 1373-1516  DrawGrenadeEsp  -> granada: circulo + rotulo
```

O que **não** é óbvio e explica a confusão: o ESP de jogador desenha **3
primitivas diferentes em 3 camadas**, e só a primeira mora no `PlayerEsp.cpp`:

| Camada | Onde | O que recebe |
|---|---|---|
| chamada | `PlayerEsp.cpp` (`Gui.Rectangle` / `Gui.Line`) | posição, tamanho, cor, **espessura** |
| primitiva | `OS-ImGui\OS-ImGui.cpp` `OSImGui::Rectangle`/`Line` | guarda de não-finito → `AddRect` / `AddLine` |
| backend | `imgui_draw.cpp` | antialias e rasterização |

É por isso que "procurar onde a caixa é desenhada" pode dar três respostas
certas em arquivos diferentes. A espessura **vem do config** e é argumento da
chamada (camada 1) — a primitiva só repassa.

### Campos novos (todos em `Config/PlayerConfig.h`)

| Campo | Default | Onde é usado |
|---|---|---|
| `BoxThickness` | **1.0** px (era literal `1.5f`) | as 3 `Gui.Rectangle` da caixa |
| `SkeletonThickness` | **1.0** px (era literal `1.0f`) | as 14 `Gui.Line` do esqueleto |
| `MaxDistItem` | **150** m | `DrawLootEsp`, **antes** de `ClassifyLootRaw`/`snprintf` |
| `MaxDistVehicle` | **500** m | `DrawVehicleEsp`, antes do `snprintf` |
| `MaxDistance` | 1000 m | jogador — chave histórica do JSON, mantida |
| `LootSubFilters` | vazio | filtro por substring (aba WORLD) |
| `LootSubFilterMode` | 0 | 0 = esconde o que casa, 1 = mostra só o que casa |

O traço da caixa afinou porque a caixa mudou: ela deixou de ser
`width = height / 2` e passou a ser a projeção dos **ossos** (+15% de folga). Em
1080p, `AddRect` com espessura 1.5 produz uma faixa de 2 px com antialias; com
1.0 sai 1 px nítido. `EspTraco()` (PlayerEsp.cpp) é a segunda barreira contra
JSON editado à mão (NaN/0/9999 não somem com a caixa).

### Filtro por substring — um mecanismo, todas as categorias

`LootSubFilters` é um `vector` de `{ sub, on }`. A regra está em **um** lugar
(`PlayerConfig::LootRawPassaSubFiltro`) e é chamada pelo desenho **e** pelo
painel — se fossem duas cópias, o painel diria "3 itens" e o ESP esconderia 4.

Ordem no `DrawLootEsp` (a que importa para performance):

```
distancia (MaxDistItem) -> nome bruto + ClassifyLootRaw -> filtro individual
-> filtro substring -> WorldToScreen -> CleanEntityName -> snprintf
```

Item a 400 m não passa do primeiro `if`. Sem substring ativa o filtro é
**fail-open** (lista vazia não pode esvaziar o mapa).

Verificação sem abrir o jogo:

```cmd
powershell -ExecutionPolicy Bypass -File tools\build_config_check.ps1
%TEMP%\config_check.exe      -> RESULTADO: 41/41 verificacoes passaram
```

O checker compila **contra o `PlayerConfig.h` e o `AimbotConfig.h` reais** e
cobre: round-trip do JSON (as três partes), os dois modos do filtro,
case-insensitive, fail-open, JSON torto (substring sem `s`, modo 99, espessura
999, distância 0) e config antiga sem os campos novos.

---

## 2g. Visibilidade e humanização do RCS (build 2610.1.1.65)

### O sinal: `mesh + 0x75C` (e NÃO o `Class + 0x750`)

**Correção de 18/10/2026, depois do teste em partida.** O campo que discrimina é
o **`LastRenderTimeOnScreen` do componente de mesh** — exatamente o `0x75C` que a
MEDICAO #1 tinha descartado. O `Class + 0x750` (slot do `Eyes`) **não é
timestamp**: o resultado em jogo foi "todo mundo visível", porque `Eyes` é outra
estrutura e o valor lido não é um tempo de render.

Por que a MEDICAO #1 errou: ela foi feita com **`mesh = 0x0`** — o próprio log
diz `[VIS] LOCAL mesh=0x0 INVALIDO - leitura pulada`. Media um ponteiro
quebrado. A §14 registra a medição com mesh **válido**:

```
[VIS] LOCAL  1m  v75C=1014.557
[VIS] INIM  56m  v75C=1014.557     <- acompanha o local (VISÍVEL)
[VIS] INIM  72m  v75C=1006.179     <- congelado (OCLUSÃO)
[VIS] INIM 218m  v75C=-1000.000    <- sentinela "nunca desenhado"
```

**Custo zero:** o `meshBulk` já lê `0xC00` bytes do mesh por ator — o `0x75C` já
está no buffer. Nenhuma leitura nova por ator.

```
o campo mudou nos ultimos N ms?   -> o motor desenhou agora    -> VISIVEL
ficou parado mais que N ms?       -> nao esta sendo desenhado  -> OCLUIDO
```

**A base de tempo saiu da conta** (3ª versão, 18/10/2026). As duas primeiras
falharam exatamente nela:

| versão | "agora" | por que falhou |
|---|---|---|
| 1 | `TimeSeconds` do GameState | GameState não publicado (lobby, pós-mapeamento) → fail-open → todo mundo verde |
| 2 | `mesh+0x75C` do **pawn local** | em FPP o corpo do local não é renderizado: vale **-1000** → delta negativo gigante → todo mundo verde |
| **3** | **nenhum** — o campo se auto-datou | o valor só avança quando o pawn é desenhado, então "parou de mudar" já é a resposta |

### O failsafe: `campo: vivo` / `campo: MORTO`

Se **nenhum** ator mudar o campo em 2 s, o offset não serve nesta build (mudou de
lugar, ou o componente escolhido não é o desenhado). Nesse caso o veredito volta
para **fail-open** e a seção VISIBILIDADE mostra:

```
campo: MORTO (fail-open) - o offset nao muda nesta build
```

É a diferença entre "a feature não funciona" e **"a feature mente"**: a primeira
aparece no menu como diagnóstico; a segunda pintaria o mapa inteiro de "oculto"
e o usuário concluiria que o ESP quebrou.

O registro da falha do 0x75C continua no `Globals.h` — com a ressalva de que ela
foi medida com o mesh inválido. Não é para apagar: é o que impede alguém de
repetir a conclusão errada sem ler a nota.


**Quando a conta roda:** `VisCheck` (ESP) **ou** aimbot ligado com
`VisibilityCheck` (default true). Se o aimbot sozinho não ativasse, o checkbox
dele seria campo sem leitor; se o ESP não tivesse o seu, não daria para testar a
cor sem mexer no aimbot.

**Onde fica o controle:** aba **VISUAL → seção "Visibilidade"** (entre "Texto do
ESP" e "Mundo"). Ele nasceu junto de "Cores" com a justificativa de que "o
efeito é cor" — e essa justificativa custou achar o controle: o usuário abriu a
aba, viu que a seção "Visibilidade" tinha sido apagada e concluiu que o recurso
havia sido removido. Controle de visibilidade mora na seção de visibilidade,
mesmo que o efeito apareça nas cores.

**A base de tempo tem fallback** (era a causa de "liguei e nada mudou"):

| Ordem | Fonte | Quando |
|---|---|---|
| 1 | `TimeSeconds` do GameState | partida em andamento |
| 2 | `LastTimeScreenRenderered` do **pawn local** | GameState ainda não publicado (lobby, logo após mapear) — em TPP o local está sempre na tela, então o valor dele **é** um "agora" válido, na mesma base |
| 3 | — | sem nenhuma das duas: fail-open |

Antes existia só a (1): com `GameState = 0` a conta caía em fail-open e **não
havia sinal nenhum**, silenciosamente.

**Os números ficam na tela.** A seção mostra, ao vivo (publicado do Engine via
`SharedData`, no mesmo lock das listas):

```
base: GameState (t=312.41) | 47 atores, 12 ocultos
local   lr=312.40  dly=8 ms       -> visivel
inimigo lr=298.53  dly=13870 ms   -> OCULTO
```

Isso separa, sem abrir o log no meio da partida, as quatro causas de "a cor não
muda": toggle desligado (`desligado: ...`), base inexistente (`sem base`), campo
que não avança para inimigo (`dly` grande **sempre**, inclusive na frente) e
tolerância errada (`dly` plausível mas `visivel`). Se o **local** der `OCULTO`,
o problema é de base/offset — não do inimigo.

**A regra é uma só:** `ESP/VisCheck.h` (`VisCheck::Veredito`), chamada pelo
Engine e exercitada pelo `config_check` (51/51): frente, congelado, sentinela
−1000, borda exata `dly == tolerância` e os fail-open (sem base, NaN, infinito).

Fail-open em três casos (leitura não veio = `NaN`, `worldTime` inválido,
delta não-finito): `IsVisible = true`. A alternativa pintaria o mapa inteiro de
"oculto" e deixaria o aimbot sem alvo por causa de um tick ruim.

| Consumidor | Efeito com `VisCheck` ligado |
|---|---|
| ESP (`PlayerEsp.cpp`) | caixa/esqueleto de quem está atrás de parede cai em `TextColour` ("Oculto") |
| Aim bot (`Aimbot.cpp`) | filtro `VisibilityCheck` passa a filtrar de verdade; o bônus de 35 pontos do score idem |

**Como medir em partida** (é o que decide se o sinal presta): ligar
`Debug logs` na aba DEBUG e observar as colunas novas do `[VIS]` (1x/s):

```
[VIS] INIM  ... | lr=  312.418  dly=      8.2ms vis=1     <- na frente
[VIS] INIM  ... | lr=  298.530  dly=  13900.0ms vis=0     <- atrás de parede
```

Se `dly` ficar grande nos dois casos, o slot está errado para esta build e o
`VisCheck` deve voltar para OFF — não é para "ajustar a tolerância" até parecer
certo.

### Humanização do RCS — v1 (três camadas cosméticas) e por que não bastava

| Camada | Parâmetro | O que muda |
|---|---|---|
| **Reação** | `RCSReactionMs` (90 ms) | rampa `smoothstep` no início da rajada; o coice desses 90 ms **não é reposto** (pessoa também não recupera) |
| **Jitter** | `RCSHumanizeStrength` (35 %) | ±18 % no ganho por frame, na força máxima; a média continua correta, a variância é que aparece |
| **Dithering + tremor** | idem | ~10 % dos frames têm o envio **adiado** (o valor fica no acumulador e sai no seguinte — nada se perde); tremor de ±0.35 count entra no acumulador, nunca direto na injeção |

**O diagnóstico que derrubou a v1** (o "por que" importa mais que a tabela):

1. **A compensação saía 100 % no mesmo frame em que o recuo era drenado.** A vista nunca chegava a subir. Quem amostrasse o pitch durante a rajada via uma **linha reta**; a de um jogador é **dente de serra** (sobe no tiro, é puxada de volta). É assinatura de telemetria, e nenhuma quantidade de jitter apaga: jitter é ruído em cima de um sinal que continua reto.
2. **O jitter era ruído branco** — média zero, novo a cada frame. Qualquer análise que faça média numa janela de 100–300 ms vê o teórico perfeito e o ruído **some**. Pior dos dois mundos: incomoda o jogador e não esconde nada.
3. **"Adiar o envio" só mudava o instante**, nunca a quantidade devida. A razão `counts injetados / drop do acumulador` seguia **invariante no tempo**, frame a frame — assinatura de *correlação*, que sobrevive a qualquer ruído simétrico.

### Humanização do RCS — v2 (modelo de mão, build atual)

Estado da rajada em `SDK/Aimbot.cpp`; a matemática testável em `SDK/RcsNoise.h`.

| Camada | Parâmetro | O que faz |
|---|---|---|
| **Latência sorteada** | `RCSReactionMs` (90 ms) | a latência é **sorteada por rajada** (30–70 % da janela). Duas rajadas nunca têm a mesma reação — 90 ms fixo é um número, e número fixo é padrão |
| **Sub-ganho** | `RCSHumanizeStrength` (35 %) | ganho < 1 no começo, fechando para 1 com `tau ≈ 420 ms`. **Sobra residual de verdade** em todo spray — a v1 nunca deixava sobra |
| **Servo lag** | `RCSLagMs` (**45 ms**) | o devido é **acumulado** (`s_hzOwedY/X`) e liberado por filtro de 1ª ordem, `alpha = dt/(lag+dt)`. Nada se perde ao longo da rajada; muda **quando** sai. É o dente de serra |
| **Ruído correlacionado** | `RCSNoiseCounts` (**1.5 counts**) | Ornstein-Uhlenbeck com `tau ≈ 200 ms` (170 no yaw). O desvio tem **inércia**, então uma média em janela ainda o vê — ao contrário do branco |
| **Cadência** | — | jitter fino (±6 %) + ~10 % dos frames adiados, agora operando sobre um sinal que **também varia em quantidade** |

**A garantia central:** `owed` preserva o total. O servo muda *quando* o count sai, nunca *quanto* sai ao longo da rajada — fora o sub-ganho, que é sub-compensação **deliberada** e fecha sozinha. A única coisa que o servo pode perder é o que a trava `4 × RCSMaxCounts` corta de propósito (rede de segurança contra hitch).

**Detalhes que não são detalhe:**

- **`dt` é medido, não suposto** (`steady_clock`). O lag tem que ser em milissegundos: a 60 Hz, "uma fração por frame" liberaria 2.4× mais devagar em tempo de parede. Hitch `> 50 ms` é cortado, senão um travamento de 300 ms vira um flick de 300 ms num pacote único.
- **O cap é aplicado ANTES de debitar a dívida.** Cortar depois perderia counts em silêncio.
- **Soltar o gatilho drena a dívida** com o mesmo filtro (`RecoilControl`, early return). Sair zerado deixaria uma puxada órfã esperando a próxima rajada. A borda de subida é fechada por **sensor**, não por botão — senão a rajada seguinte começaria sem latência e sem sub-ganho.
- **Desligar devolve a v1 byte a byte** (o toggle zera `owed`, o passeio e o estado da rajada).
- **O painel "RCS ao vivo" mostra a mão:** `lat` (latência sorteada), `ganho`, `passo`, `devendo` e `ruido`. `devendo` cravado em `0.00` com `Suavidade > 0` significa que o servo não está sendo alimentado — sensor morto, arma não lida ou toggle.

**O limite honesto, de novo:** detecção por padrão de rajada no servidor olha a trajetória agregada de pitch/yaw em N ms. A v2 ataca isso — a vista passa a subir e ser puxada, o resíduo deixa de ser zero e o erro tem inércia. O que continua **fora** é comportamento: erro por arma, over-flick ocasional, curva de aprendizado. Outro desenho, não um slider.

**Verificação sem abrir o jogo:**

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_rcs_hand_check.ps1
%TEMP%\rcs_hand_check.exe        # 14/14
```

O checker inclui o **header real** (`SDK/RcsNoise.h`), não uma cópia — a mesma regra do `LootRawPassaSubFiltro` (§2f): a conta mora num lugar só, e um teste que reimplementa a fórmula testa a cópia. Ele cobre desvio estacionário do OU, independência de `dt` (60 vs 144 Hz), autocorrelação (o que separa passeio de chiado), trava de 4 σ, conservação do servo, `lag = 0` como caminho antigo, o cap que atrasa e não corta, a constante de tempo em ms, e imprime o **traço do dente de serra** lado a lado com o da v1.

**O que ele NÃO prova:** nada sobre a compensação em jogo. Só o jogo prova.

---

## 3. Contrato com o kernel driver

### Fonte da verdade (o que roda hoje)

`PubgExt\driver\common.h` e `PubgExt\driver\driver_interface_v3.cpp` definem os
IOCTLs e a request do lado user; **`km_safe_source.c` replica os mesmos valores**
do lado kernel. Os dois têm que mudar juntos.

```c
struct DRIVER_REQUEST {          // 40 bytes, #pragma pack(8)
    int       Pid;               // +0
    uintptr_t Address;           // +8
    void*     Buffer;            // +16  (não usado: METHOD_BUFFERED)
    size_t    Size;              // +24
    wchar_t*  Name;              // +32
};
```

| IOCTL | Código | Buffer |
|---|---|---|
| `IOCTL_GET_PID` | `0x801` | in: `DRIVER_REQUEST` (por `Name`) — out: `int` |
| `IOCTL_GET_MODULE_BASE` | `0x802` | in: `DRIVER_REQUEST` (por `Pid`) — out: `uintptr_t` |
| `IOCTL_READ_MEMORY` | `0x803` | in: `DRIVER_REQUEST` — out: payload de `Size` |
| `IOCTL_KM_PING` | `0x8FF` | in: nada — out: `ULONG` `0x504B4D53` ("PKMS") |
| `IOCTL_WRITE_MEMORY` | `0x804` | **declarado no user mode, NÃO implementado no driver** |

`KM_MAX_COPY` = **1 MiB** por IOCTL; `KM_MAX_NAME` = 260. O user mode abre
`\\.\PUBG_KM` — o **symlink** é fixo, só o nome do **device** muda a cada carga
(ver "recarregar sem reboot", abaixo).

**O `PING` existe por segurança, não por estilo.** Os dois drivers (o antigo e o
novo) criam o **mesmo** symlink `\DosDevices\PUBG_KM`. Sem uma impressão
digital, o `TestHarness.exe` não teria como saber qual `.sys` o kdmapper acabou
de carregar — e rodar os casos perigosos contra o driver antigo derrubaria a
máquina de novo. Só o driver novo responde ao `0x8FF`; sem a assinatura, o
harness **não** roda os testes perigosos.

### HISTÓRICO — o contrato `KM_REQUEST` / `\Device\Ndu`

O desenho intermediário (driver `Ndu.sys`, hoje em `_lixo\`) usava um header
escalar com o payload no mesmo buffer e um device próprio:

```
[ KM_REQUEST (Pid, Pad, Address, Size, Status) ][ payload de Size bytes ]

IOCTL_READ_MEMORY       0x00222004
IOCTL_GET_MODULE_BASE   0x00222008
IOCTL_GET_PID           0x0022200C
IOCTL_WRITE_MEMORY      0x00222010
Device: \Device\Ndu -> \DosDevices\Ndu   (caminho cifrado por LIT())
```

**Isso não é o que roda hoje.** A seção "O que estava errado antes" logo abaixo
explica por que o header escalar foi adotado — o motivo continua valendo, o
formato não.

### O que estava errado antes (e foi corrigido)

**1. Três definições conflitantes.** `driver_interface_v3.cpp` definia
`IOCTL_GET_PID = 0x801` — que é o código de *read* no driver.
`common.h` definia `0x803`. `km_driver_source.c` usava `0x801 = READ`. O `.cpp`
vencia pela ordem de include e o build cuspia `warning C4005`. Funcionava por
acidente.

**2. Só 1 dos 4 IOCTLs implementados.** O driver só tinha handler de
`READ_MEMORY`. `GET_PID` e `GET_MODULE_BASE` falhavam e o user mode caía em
`Toolhelp32` sem ninguém saber. `WRITE_MEMORY` não existia — `Memory::Write`
retornava `false` fixo em `Memory.cpp:153`.

**3. Overflow de pool não-paginado — o bug sério.**

```c
// ANTES
struct DRIVER_REQUEST { int pid; uintptr_t address; void* buffer;
                        size_t size; const wchar_t* name; };   // 40 bytes

DeviceIoControl(h, IOCTL_READ_MEMORY, &req, sizeof(req), req.buffer, req.size, ...);
//                                     ^ input 40 bytes   ^ output
```

Com `METHOD_BUFFERED` o I/O manager aloca `SystemBuffer` do tamanho do
**input** (40 bytes, `PagedPool`). O driver fazia
`KeReadVirtualMemory(Process, address, req->buffer, req->size)` — escrevendo
`size` bytes naquele pool. `Memory::Read` pede structs de ator, listas,
buffers grandes. **Overflow de pool com tamanho controlado pelo chamador.**

**4. Ponteiro de user mode no header.** O `void* buffer` era o destino da
cópia no espaço do processo *atual*. Só funcionava porque o IOCTL era
despachado na thread do RuntimeBroker.

**5. O contrato novo (herdado pelo driver atual).** Header escalar de tamanho
fixo, payload no próprio buffer do IOCTL, e validação de
`InputBufferLength` / `OutputBufferLength` **antes** de qualquer acesso, com teto
de 1 MiB por IOCTL. No `km_safe_source.c` a validação é **por caso**, porque o
PING não manda entrada nenhuma — uma checagem global de tamanho mínimo recusava
o PING com `ERROR_INSUFFICIENT_BUFFER (122)` antes de chegar no `switch`:

```c
if (sysbuf == NULL)                                     -> STATUS_INVALID_PARAMETER

GET_PID / GET_MODULE_BASE / READ:
    if (inLen < sizeof(KM_DRIVER_REQUEST))              -> STATUS_BUFFER_TOO_SMALL
READ:
    if (req->Size == 0 || req->Size > KM_MAX_COPY)      -> STATUS_INVALID_PARAMETER
    if (req->Size > outLen)                             -> STATUS_BUFFER_TOO_SMALL
```

E usa `MmCopyVirtualMemory`, que resolve o endereço no contexto do processo
alvo por conta própria — não depende mais de "processo atual".

**6. O item 1 não estava resolvido de verdade — e o checker escondia isso.**
O `contract_check.py` continuava apontando para `km_driver_source.c`, o
**driver aposentado**: o projeto veio de outra máquina e o checklist ficou
preso no arquivo velho. Resultado: ele comparava `common.h` com o contrato do
`Ndu` (`0x00222004`), acusava **13 divergências** num build saudável, e o
`--selftest` dava 5/5 — o checker funcionava, só olhava o alvo errado.

Com o alvo certo, o item 1 apareceu **inteiro**:

| arquivo | `IOCTL_GET_PID` | estado |
|---|---|---|
| `km_safe_source.c` | `0x801` | o kernel real |
| `TestHarness.cpp` | `0x801` | já certo, cópia própria justificada (compila fora do `.sln`) |
| `common.h` | **`0x803`** | **errado** — nomes e valores no padrão do `Ndu` |
| `driver_interface_v3.cpp` | `0x801` | certo, mas **redefinia por cima** do header (daí o `warning C4005`) |

O header errado nunca causou sintoma porque o `.cpp` passava por cima dele.
Se alguém tivesse apagado aquelas quatro linhas "por limpeza", o cheat passaria
a mandar `0x801` (que o kernel trata como `GET_PID`) achando que era
`READ_MEMORY` — leitura de memória devolvendo PID, sem erro claro.

**Correção:** `common.h` virou a fonte única do lado C++ (10 IOCTLs + as
constantes compartilhadas: `KM_MAX_COPY`, `KM_MAX_NAME`, `KM_PING_MAGIC`,
`KM_DESTRUCT_MAGIC`, `KM_DESTRUCT_UNMAP`, `KM_DRIVER_VERSION`), as duas
structs mortas do `Ndu` saíram, e **todas** as `#define IOCTL_*` saíram do
`driver_interface_v3.cpp` — inclusive um `IOCTL_KM_DEACTIVATE` que estava
duplicado no mesmo arquivo e um `IOCTL_GET_VERSION` com nome trocado em
relação ao kernel (`IOCTL_KM_GET_VERSION`, mesmo `0x806`). O `C4005` sumiu.

**O checker agora audita o contrato real**, e não só nomes:

1. **IOCTLs** — kernel × cada consumidor (header, cheat, harness).
2. **Layout da struct** — tamanho, offset e largura de cada campo. Compara
   **ABI**, não nomes: o kernel chama `Pid`, o user mode `pid`, e isso é certo.
3. **Constantes compartilhadas** — todo `#define` numérico presente nos dois
   lados tem que ter o mesmo valor. Pega deriva em qualquer constante, não só
   nas que alguém lembrou de listar.
4. **Derivação do nome do device** — o nome não é mais literal, é derivado de
   seed; então compara a constante de mistura, as duas mágicas do MurmurHash,
   a contagem de hex e o `kind` de cada nome.
5. **Órfãos** — IOCTL definido e nunca usado.

`--kernel km_driver_source.c` ainda audita o antigo, para comparação.

### Implementação do lado kernel — `km_safe_source.c` (atual)

| IOCTL | Como |
|---|---|
| `READ` | `PsLookupProcessByProcessId` + `MmCopyVirtualMemory` |
| `GET_PID` | `ZwQuerySystemInformation(KmSystemProcessInformation)` + comparação de nome |
| `GET_MODULE_BASE` | `PsGetProcessSectionBaseAddress` — o caminho direto, **não** anda mais o PEB (menos código, menos coisa pra dar errado) |
| `PING` | devolve `0x504B4D53` |

O log do driver (`C:\Windows\PkmSafe.txt`) grava, por chamada que importa,
`inLen` / `outLen` / `pid` / `PsLookup stat` / `section base` / `g_InitStatus`.

### HISTÓRICO — implementação do `km_driver_source.c` (aposentado)

| IOCTL | Como |
|---|---|
| `READ` | `PsLookupProcessByProcessId` + `MmCopyVirtualMemory` → buffer validado |
| `WRITE` | idem, invertendo origem/destino |
| `GET_PID` | `ZwQuerySystemInformation(KmSystemProcessInformation)` + comparação de nome |
| `GET_MODULE_BASE` | `KeStackAttachProcess` + walk de `PEB->Ldr->InLoadOrderModuleList`, com `__try/__except` |
| `GET_CR3` | `_EPROCESS->DirectoryTableBase` (offset fixo, ver abaixo) |
| `READ_VIRT_PHYS` | page-table walk de 4 níveis + `MmMapIoSpaceEx` na página física |

`KM_MAX_NAME` = 260. `ZwQuerySystemInformation` e `MmCopyVirtualMemory` não
estão nos headers públicos do WDK 28000 — estão declarados à mão no topo do
arquivo, junto com `KM_SYSTEM_PROCESS_INFORMATION` e `KM_PEB`.

### HISTÓRICO — offsets e máscaras do page-table walk (`km_driver_source.c`)

O driver atual **não** faz page-table walk: usa `MmCopyVirtualMemory`. Isto
aqui vale pelo padrão, não pelo estado.

O walk usava `0x1FF` literal nos quatro níveis e reusava a máscara do PML4
para todos. Depois da correção cada nível tem a sua constante:

```c
#define KM_IDX_SHIFT_PML4 39
#define KM_IDX_SHIFT_PDPT 30
#define KM_IDX_SHIFT_PD   21
#define KM_IDX_SHIFT_PT   12
#define KM_IDX_MASK_PML4  0x1FFULL
#define KM_IDX_MASK_PDPT  0x1FFULL
#define KM_IDX_MASK_PD    0x1FFULL
#define KM_IDX_MASK_PT    0x1FFULL
```

Não é que os valores tenham mudado — é que agora uma divergência entre
níveis é visível em vez de silenciosa. `tools\dump_driver\pagewalk_check.py`
lê essas constantes do próprio `.c` e valida a aritmética contra tabelas de
página sintéticas, em user mode.

### HISTÓRICO — `_EPROCESS->DirectoryTableBase`

Só o driver antigo usava isto; o atual não lê CR3 nem anda tabela de páginas.

`KM_EPROCESS_DTB_OFFSET = 0x28` (Windows 10 22H2, 19045, x64) está
**hardcoded**. Não dá pra resolver sem carregar o driver. Se o walk devolver
lixo ou `GET_CR3` devolver 0, este é o primeiro suspeito — é o offset que
muda com mais frequência entre builds do Windows.

### Status dos testes

Todas as verificações estáticas passam:

| Verificação | Estado |
|---|---|
| `contract_check.py` | `CONSISTENTE`, 1 aviso (o `WRITE_MEMORY` declarado e não implementado, de propósito) |
| `contract_check.py --selftest` | 5/5 mutações detectadas |
| `pagewalk_check.py` sanidade | 6/6 constantes |
| `pagewalk_check.py` walk | 6/6 (4 KiB, 2 MiB, 1 GiB, VA kernel, PT adjacente) |
| `pagewalk_check.py` aliasing | 3/3 (mesmo PD / mesmo PDPT / mesmo PML4) |
| `pagewalk_check.py` negativos | 5/5 recusados corretamente |
| `pagewalk_check.py` discriminação | 2/2 |
| `pagewalk_check.py` mutation | 4/4 |

O `pagewalk_check.py` **já pagou o investimento**: foi ele que achou o
`idxmask` fixo do PML4 sendo aplicado aos quatro níveis.

### Auditoria externa de 2026 — o que era verdade, o que não era

Um relatório externo apontou 8 problemas. **Cinco eram reais, um já estava
corrigido e dois estavam errados.** O registro fica porque auditoria sem
conferência é opinião.

| # | Alegação | Veredito |
|---|---|---|
| 1 | `KM_MOUSE_CALL_ENABLED` ligado enquanto o comentário diz desligado | **REAL**, mas era bug de *comentário*, não de código: o guard foi removido de propósito (`tools\gen_driver_bytes.ps1`). Comentário corrigido, chamada mantida — desligá-la devolveria `LLMHF_INJECTED` ao RCS |
| 2 | `DEACTIVATE` chama `KmQuiesce` mas não `KmShutdownDevice`; a função não tem chamador | **REAL e pior que o relatado**: o comentário dizia que "a thread de liberação" chamava, mas essa thread (`KmUnmapWorker`) foi deletada na 2.1. Device + symlink vazavam uma unidade **por carga, para sempre** |
| 3 | `UnmapComInfo` usa espera fixa de 150 ms | **REAL** — trocado por verificação observável (ver abaixo) |
| 4 | `FILE_ANY_ACCESS` sem autorização explícita no dispatch | **REAL** — agora há política de dono por PID |
| 5 | IOCTLs conflitantes entre `common.h` e `driver_interface_v3.cpp` | **JÁ CORRIGIDO** antes desta rodada (ver item 6 em §3) |
| 6 | Resolução do callback com "questões de gerenciamento de referências" | **REAL**: `ObReferenceObjectByName` sem `ObDereferenceObject` — o driver object do Mouhid ficava referenciado para sempre. Era o único `ObReference*` sem par no arquivo |
| 7 | `KmShared.h` sem sincronização atômica e sem uso | **REAL** — é código morto (só o `km_driver_source.c` aposentado o inclui). Ganhou banner de MORTO |
| 8 | `syscall.asm` com números de syscall fixos | **REAL** — e o relatório errou ao chamar `SyscallNtFreeVirtualMemory` de morto: ele é usado em `Engine::~Engine` |

#### O ciclo de desligamento, corrigido

```
IOCTL_KM_DEACTIVATE  -> KmQuiesce()      para o atendimento, namespace de pé
CloseHandle          -> IRP_MJ_CLOSE     contagem cai a zero
                     -> KmShutdownDevice()  (agora COM chamador)
                         1. solta o callback do mouse + deref do device
                         2. MajorFunction[*] = NULL
                         3. IoDeleteSymbolicLink
                         4. IoDeleteDevice
```

Detalhes que decidem se é seguro:

- a demolição roda **depois** de `IoCompleteRequest` — o `FILE_OBJECT` do handle
  que está fechando segura uma referência ao `DEVICE_OBJECT` até o CLOSE
  completar; completar antes libera essa referência;
- `InterlockedExchange` em `g_DeviceDestroyed` impede demolição dupla;
- a contagem virou **decrementa-e-confere-o-resultado**: o `if (g_OpenHandles > 0)
  InterlockedDecrement(...)` anterior não era atômico e dois CLOSE simultâneos
  podiam decrementar duas vezes.

#### Autorização do solicitante

O primeiro processo a abrir o device vira dono (`InterlockedCompareExchangePointer`),
e **CREATE e DEVICE_CONTROL** de outro PID recebem `STATUS_ACCESS_DENIED`. Conferir
só no CREATE deixaria um handle herdado/capturado passar sem checagem.

Não cria "driver fantasma": o Windows fecha todos os handles quando o processo
morre, então o CLOSE sempre chega e o CLOSE zera o dono.

Limite honesto: o controle real continua sendo o **nome** (16 hex derivados de um
seed de 64 bits). Um `SD` restrito exigiria `IoCreateDeviceSecure` + `wdmsec.lib`,
que o build `/NODEFAULTLIB` não liga hoje.

#### A espera de 150 ms virou verificação

O nome do device é derivado do seed, então dá para **provar** daqui: depois do
deactivate, tenta-se `CreateFile` de novo, em poll de até 500 ms.

| resultado | significado |
|---|---|
| `ERROR_FILE_NOT_FOUND` / `PATH_NOT_FOUND` | demolição **completa** — nome fora do namespace |
| `ACCESS_DENIED` / qualquer outra recusa | device quiescido: não aceita mais ninguém |
| **abriu** | o driver **não** foi demolido — é o caso que o sleep escondia |

#### O driver embutido podia estar desatualizado

Bug encontrado no caminho, **pré-existente**: o `mtmacr0.exe` não lê o `.sys` do
disco, ele carrega uma cópia em bytes gerada em `PubgExt\Mapper\driver_bytes.h`.
E o `tools\gen_driver_bytes.ps1` **não estava no `build_all.ps1`**.

Medido: o `driver_bytes.h` embutia um `.sys` de **16.384** bytes enquanto o
compilado na hora tinha **11.776**. Dava para "compilar o cheat com a correção" e
o `.exe` sair rodando o driver **velho**, sem aviso. O `build_all.ps1` agora
regenera o header antes do MSBuild e imprime o tamanho embutido.

#### `syscall.asm`

`DoSyscall` foi **removido**: estava morto e errado — recebia o índice em `ecx` e
fazia `mov r10, rcx`, ou seja, colocava o **índice** onde a convenção do `syscall`
exige o **primeiro argumento**. Qualquer uso com argumento chamaria a função certa
com lixo no 1º parâmetro.

Os índices deixaram de ser immediate (`0x18` / `0x1E`). `EnsureNtSyscallIndices()`
os resolve em runtime lendo o imm32 do stub do `ntdll` (`4C 8B D1 B8 xx xx xx xx`),
com fallback para os valores de build se o padrão não casar. Sem API nova.

### Banco de teste em runtime — `DriverSelfTest.cpp`

Os testes acima são estáticos. Eles provam a aritmética, não provam que o
driver **carregado** responde. Para isso existe `PubgExt\DriverSelfTest.cpp`,
que roda dentro do `mtmacr0.exe` e aponta o teste para **o próprio
`mtmacr0.exe`** — sem precisar abrir o jogo.

O truque que faz valer: ler os próprios bytes do módulo por três caminhos
independentes e exigir que os três batam.

| Caminho | Como |
|---|---|
| A. referência | `memcpy` local, verdade absoluta |
| B. `ReadMemory` | `MmCopyVirtualMemory` |
| C. `ReadMemoryViaPhysical` | page-table walk + `MmMapIoSpaceEx` |

Se B ou C divergir de A, o caminho correspondente está quebrado — e o log
diz qual. Além disso o teste confere:

| Teste | Comparação |
|---|---|
| `GET_PID` | contra `GetCurrentProcessId()` — exato |
| `GET_MODULE_BASE` | contra `GetModuleHandleA(NULL)` — exato |
| `GET_CR3` | contra `__readcr3()`, o CR3 real do registrador |
| walk cruzando 3 páginas | leitura começando em `base+0xF00` |
| `WRITE_MEMORY` | escreve em `VirtualAlloc` próprio e confere |
| endereço inválido | tem que ser recusado, sem bugcheck |

**O teste do `GET_CR3` é o mais importante do arquivo.** `KM_EPROCESS_DTB_OFFSET
= 0x28` está hardcoded, muda com o build do Windows, e se estiver errado o
walk devolve lixo silenciosamente. `__readcr3()` funciona em user mode no
Windows, então dá para comparar com a verdade na hora.

Como rodar:

```
1. opcional: carregue o PUBG_KM_safe.sys (kdmapper) ou tools\RunSafeTest.cmd
   — o mtmacr0.exe mapeia o driver embutido sozinho, sem precisar disto
2. rode o Build\Release\PubgExt\mtmacr0.exe SEM o jogo aberto
3. olhe o logs.txt
```

Sem o jogo, o `InitializeFeaturesThread` cai no self test automaticamente.
O resultado sai no log como `DRIVER SELF TEST` com `N passaram, M falharam`.
(O `RunAsIO3Probe()` que roda junto é história — ver §3c.)

### O harness do driver — `TestHarness.exe`

O `DriverSelfTest.cpp` roda dentro do cheat; o `TestHarness.exe`
(`tools\TestHarness.cpp`) roda sozinho e é o que **prova** o driver sem abrir o
jogo. Sequência:

1. **PING** primeiro. Sem a assinatura `0x504B4D53` ele se recusa a continuar —
   é assim que ele sabe que não está falando com o driver antigo;
2. leitura válida contra o próprio processo (o caminho do `MmCopyVirtualMemory`);
3. os **casos perigosos**, um por um: ponteiro não-canônico, endereço
   não-mapeado, tamanho absurdo, PID inválido.

Cada um desses casos, no driver antigo, era um BSOD. No novo são um `NTSTATUS`.
Resultado medido: **11 passed, 0 failed**, com a máquina de pé.

---

## 3a. Lições do driver mapeado pelo kdmapper

Tudo aqui foi aprendido com BSOD ou com `err=433`, não por leitura. Se for mexer
no driver, leia esta seção antes.

**1. `DriverObject` é NULL.** O kdmapper faz manual-map e chama
`DriverEntry(DriverObject, NULL)`. O primeiro parâmetro **não** é um
`_DRIVER_OBJECT` válido — é NULL. Por isso o driver atual cria o **próprio**
driver object com `IoCreateDriver`, em vez de usar o do mapper.

**2. `__try/__except` NÃO funciona num driver mapeado.** O kernel acha os
handlers pelo `.pdata` da imagem, e o `.pdata` **nunca é registrado** num mapa
manual. Resultado: o `__try` não protege nada e o fault vira
`KMODE_EXCEPTION_NOT_HANDLED (0x1E)`.

> **Solução: `MmCopyVirtualMemory`.** O tratamento de fault dela vive **dentro
> do ntoskrnl**, que tem SEH registrado — a exceção é resolvida onde o `.pdata`
> existe.

**3. `PAGE_FAULT_IN_NONPAGED_AREA (0x50)` é bugcheck, não exceção.** Nenhum SEH
pega. Só prevenção: validar o ponteiro antes de pedir a leitura (é o que a macro
`IsAddrValid` do `Globals.h` faz) e deixar o resto com o
`MmCopyVirtualMemory`.

**4. `DO_DEVICE_INITIALIZING` (0x40) — o flag que ninguém limpa.** O
`IoCreateDevice` seta esse bit; num driver carregado normalmente o I/O manager
limpa depois do `DriverEntry`, mas num driver **mapeado ninguém limpa**. O
sintoma é `CreateFile` falhando **para sempre** com
`err=433 ERROR_NO_SUCH_DEVICE` — o device existe e o symlink aponta para ele,
mas o objeto fica "em inicialização" e o I/O manager recusa a abertura. Fix de
uma linha:

```c
g_Device->Flags &= ~DO_DEVICE_INITIALIZING;
```

**5. `IoCreateDriver` IGNORA o retorno da init function.** Ela devolve o status
*dela* (criar o driver object), não o do seu `IoCreateDevice`. Uma falha no
device fica invisível, o kdmapper imprime "DriverEntry returned 0x0" e você
procura no lugar errado. Por isso o driver guarda `g_InitStatus` e **retorna
ele**.

**6. Recarregar sem reboot.** O kdmapper **não descarrega** o driver que mapeou,
então cada carga deixa lixo no namespace do kernel (`\Driver\PUBG_KM_Drv`,
`\Device\PUBG_KM`, `\DosDevices\PUBG_KM`). Sem tratar isso, **todo teste exige
um reboot**. O que o driver faz:

- o **device** ganha nome único por carga (`\Device\PUBG_KM_<seed>`) — não
  colide nunca;
- o **symlink** `\DosDevices\PUBG_KM` é apagado e recriado; é o ponto de entrada
  do user mode, que continua abrindo `\\.\PUBG_KM` e não sabe de nada disso;
- se `\Driver\PUBG_KM_Drv` colidir (`0xC0000035`), cai no fallback
  `IoCreateDriver(NULL, ...)`; se nem isso devolver driver object, desiste e
  loga.

**7. Log em arquivo, não `DbgPrint`.** `DbgPrint` exige depurador de kernel
anexado (`bcdedit /debug` + reboot + `kd`). Sem isso o driver é uma caixa preta.
O `KmLog` usa `ZwCreateFile`/`ZwWriteFile` em `\SystemRoot\PkmSafe.txt`
(`C:\Windows\PkmSafe.txt`), em **append**, e cada carga abre um bloco:

```
===== DriverEntry =====
   DriverObject do mapper = 0x00000000
-- KmDriverInit --
   drv = 0xAD7A1B60
   dispatch montado
-- KmCreateDevice --
   IoCreateDevice = 0x00000000
   IoCreateSymLink= 0x00000000
   g_InitStatus   = 0x00000000
```

`ZwCreateFile`/`ZwWriteFile` rodam no contexto do processo System e **não
dependem do I/O manager, do driver object nem do device object** — funcionam
exatamente quando todo o resto falhou.

**8. `METHOD_BUFFERED`: `sysbuf` é entrada E saída no MESMO buffer.** Um caso que
escreve a saída antes de ler a entrada **destrói os próprios campos**:

```c
*(ULONG64*)sysbuf = 0;      // "limpa a saida" do GET_MODULE_BASE
... req->Pid ...            // e le a entrada JA ZERADA
```

O sintoma era `GET_BASE pid : 0x00000000` em **toda** chamada (enquanto o
`GET_PID`, que não zerava antes, funcionava) — e o dump de diagnóstico saía todo
zerado, porque **o instrumento estava contaminado pela doença**. Levou várias
rodadas para achar.

**Fix:** copiar a request para uma struct local **no topo do dispatch**, antes do
`switch`. Assim nenhum caso consegue mais destruir a entrada.

---

## 3b. Overlay: identidade e por que não precisa de DWM

### O que fazia o overlay ser detectável
| Onde | O quê | Gravidade |
|---|---|---|
| `DirectOverlay.h` | classe de janela `"PUBG_OVERLAY"` e título `"PUBG_OVERLAY"` | string de blacklist literal no PE; um `EnumWindows` resolve |
| `DirectOverlay.h` | `DwmExtendFrameIntoClientArea(Overlay, {-1,-1,-1,-1})` | assinatura clássica de overlay D3D |
| `Renderer\OverlayWindow.cpp:54` | `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` | API cujo único uso real é esconder overlay de captura |
| `Renderer\OverlayWindow.cpp:31` | classe `"DwmRendererLayer"` | idem |

### O que está agora

`DirectOverlay.h`:

* classe montada em runtime a partir de prefixos de classes **de sistema
  reais** (`ApplicationFrameWindow`, `Shell_TrayWnd`, `WorkerW`, `Progman`,
  `DirectUIHWND`…) + sufixo aleatório de 4 dígitos hex;
* título escolhido de uma lista de nomes de janelas de shell do Windows;
* `WS_EX_NOACTIVATE` adicionado, `WS_EX_TRANSPARENT` só alterna com o menu;
* `DwmExtendFrameIntoClientArea` e `SetWindowDisplayAffinity` removidos;
* `Run()` retorna `bool` e destrói o que criou em qualquer falha, em vez de
  entrar num loop de render sem device.

Verificação no binário (`findstr` em `mtmacr0.exe`):

```
PUBG_OVERLAY         ausente
PUBG_KM              ausente
HardenStub           ausente
DwmRendererLayer     ausente
```

### Verificado em execução

O overlay **foi executado** e sobrevive com o log completo. Sem o jogo, sem
driver carregado:

```
[*] Setup configuration loader...
[*] Ready | Menu Key: HOME
[*] Initializing direct transparent overlay...
[ov] tela 1920x1080
[ov] janela criada: 0x00000000000304DA  visible=1
[ov] D3D11CreateDeviceAndSwapChain(HD) -> 0x00000000
[ov] RegisterHotKey HOME=1 (err=0)  END=1 (err=0)
[ov] Run: entrando no loop de render
[-] TslGame.exe nao encontrado! Abra o jogo e reinicie o RuntimeBroker.
```

Ou seja: janela criada **e visível**, device D3D11, hotkey registrada, thread
de init rodando, self test e sonda do AsIO3 executando. Sem crash. O loop de
render foi confirmado girando pelo CPU do processo (0.83 → 0.86s em 3s).

### Os dois bugs que impediam o menu de abrir

Ambos foram introduzidos numa reescrita do `DirectOverlay.h` e os dois só
apareciam em runtime — o build passava nos dois casos.

**1. Faltava `WS_VISIBLE`.**

```cpp
WS_POPUP,             // ANTES: janela nasce ESCONDIDA
WS_POPUP | WS_VISIBLE // DEPOIS
```

Uma janela `WS_POPUP` sem `WS_VISIBLE` não aparece. Pior: swapchain de janela
invisível **não apresenta**, então o loop de render pendura — o processo ficava
vivo com 38 threads e sem nenhuma janela na tela. O sintoma exato era "o log
inteiro roda mas nada aparece".

**2. `GetAsyncKeyState` não funciona numa janela sem foco.**

```cpp
if (GetAsyncKeyState(VK_HOME) & 1)   // ANTES: nunca dispara
```

A janela é criada com `WS_EX_NOACTIVATE` — ela **nunca recebe foco de input**.
`GetAsyncKeyState` só vê tecla se o processo tiver foco. Trocado por
`RegisterHotKey` + `WM_HOTKEY`, que o Windows entrega independente de foco e
que funciona com o jogo em fullscreen.

### Console de diagnóstico — só quando pedido

Uma janela de console preta com `[*] aguardando TslGame.exe` é marcador de
cheat, e é por isso que ela não existe por padrão: tudo vai pro `logs.txt`.

Para testar, liga por variável de ambiente:

```powershell
set PUBGEXT_CONSOLE=1
mtmacr0.exe
```

Aí a janela de console aparece espelhando o `BaseLog` — o `printf` já existia
na linha 65 do `Main.cpp`, não precisou de nada novo. Sem a variável, nenhuma
janela: **não existe risco de esquecer o console ligado numa sessão real**.

Detalhe: só aloca console se `GetStdHandle(STD_OUTPUT_HANDLE)` **não** for
válido. Se alguém já redirecionou o stdout (`app.exe > out.txt`), apontar o
handle pro `CONOUT$` de um console recém-criado quebra o processo.

### O crash de startup que estava escondido

Antes disso o processo morria com:

```
[*] Setup configuration loader...
[FATAL] Unhandled Exception: 0xE06D7363
```

`0xE06D7363` é o código do MSVC para **exceção C++** — não access violation. E
vinha de `SetUpConfig()`:

```cpp
// ANTES
std::filesystem::path filePath = std::filesystem::path(
    getenv(LIT("USERPROFILE"))) / LIT("Documents") / LIT("Hunt") / LIT("Default.json");
std::filesystem::create_directories(filePath.parent_path());
```

`std::filesystem` **lança** em caso de erro. `USERPROFILE` ausente, `Documents`
sem permissão, caminho longo — qualquer um vira exceção que sobe pelo `Run()` e
mata o processo **antes do overlay**. E o `CrashHandler` não imprime stack nem
`what()`, então o log só mostrava o código.

Agora falha em voz alta e segue com os defaults:

```
[cfg] nao consegui preparar C:\Users\Bruno\Documents\Hunt: ... Acesso negado.
[cfg] seguindo com a configuracao padrao
```

**Um erro de configuração não pode derrubar o cheat.** O overlay e o ESP não
dependem do JSON.

### Por que o caminho DWM/explorer.exe está errado

`Loader.exe` faz manual map de `DwmRenderer.dll` dentro do `explorer.exe` e
conversa por `\\.\pipe\PubgDwmPipe`. Isso resolve um problema que não existe:
desenhar dentro da composição do Windows. O custo é trocar "processo próprio
com janela normal" por "manual map num processo do sistema" — que é
**ativamente mais suspeito**.

Anti-cheat não bane por existir uma janela topmost. Bane por hook de
`Present`/`IDXGISwapChain` dentro do processo do jogo, por DLL injetada em
`TslGame.exe`, por leitura de memória fora de um driver conhecido, e por
driver em blacklist. A janela `WS_EX_TOPMOST | WS_EX_LAYERED` + ImGui D3D11 em
processo separado não está em nenhuma dessas categorias.

Por isso `Loader`, `Renderer`, `overlay.cpp` (Discord hijack),
`OS-ImGui_External.cpp` (attach em janela do Discord) e `DirectOverlay.h`
original são todos caminhos alternativos — e o escolhido é o mais simples.

O bloqueio que ele encontrou no Windows 11 25H2 foi a mudança de arquitetura
do DWM (o `dwm.exe` no 24H2+ roda com mitigações que impedem a injeção
externa do jeito antigo). No Windows 10 22H2 isso não se aplica — mas a rota
continua desnecessária.

### Widgets do fork do ImGui — três bugs de layout, medidos no probe (17/10/2026)

O menu usa o fork de ImGui que vem com o projeto (`OS-ImGui/imgui`), e **três
widgets dele estavam errados**. Todos foram encontrados com print do usuário e
fechados com medida em pixel, não por inspeção.

| # | Widget | Sintoma | Causa | Correção |
|---|---|---|---|---|
| 1 | `Checkbox` | toggles "desalinhados"; em duas colunas, um em cima do outro | trilho posicionado por `GetContentRegionMax()` — a largura da **região de conteúdo**, não do item; `total_bb` cobria a linha inteira (hitbox da linha inteira também) | `CheckboxW(label, v, width)`; o trilho vai para a borda direita **do item** |
| 2 | `ButtonEx` | "letras maiores que o botão" (MAP NOW / VERIFICAR AC) | texto centrado em `size_arg.x`, que é **0** quando o botão é automático → rótulo começava meia largura à esquerda; e o `CalcItemSize` deste fork **não soma `FramePadding`**, então `bb` = exatamente a caixa do texto | centrar em `bb` (tamanho real) e somar `FramePadding*2` quando `size_arg` vem 0 |
| 3 | `ButtonEx` | MAP NOW não ficava vermelho | o widget nunca leu `ImGuiCol_Button`: usava `c::button::background` (26,26,26), quase a cor do painel, e ignorava `PushStyleColor` | cor do fundo vem de `ImGuiCol_Button{Active,Hovered}` |

O `Checkbox` não mudou de aparência nas seções de uma coluna: `Checkbox`
continua chamando `CheckboxW(label, v, 0)` = "linha inteira", que é o toggle
colado na direita do painel. As duas colunas passam por `CheckboxPair`
(Main.cpp), que dá **metade da largura** para cada uma e 16 px de vão.

#### Como isso foi medido (e como medir de novo)

```cmd
powershell -ExecutionPolicy Bypass -File tools\build_frameprobe.ps1
Build\frameprobe\probe.exe menu        :: 4 s, desenha o chrome do menu
:: com ele na tela, capturar 140,140 760x700
```

`Build\frameprobe\probe.cpp` reproduz o menu (sidebar + cabeçalho + corpo da aba
VISUALS) com o **mesmo código de layout** do `Main.cpp` e a paleta do
`ApplyProfessionalStyle`. Ele **não substitui rodar o cheat** — é uma cópia, e
cópia diverge; serve para olhar geometria em 4x e comparar antes/depois sem
abrir o jogo. As capturas ficam em `Build\frameprobe\shot_new.png` e
`zoom_cb.png` / `zoom_btn.png`.

#### O que ficou DE FORA (e é decisão, não esquecimento)

O acento dos controles (toggle ligado, grab do slider, scrollbar) é
`c::accent` = **lavanda (130,143,234)**, do tema original do fork, enquanto o
resto do menu é **ciano (26,204,179)**. Não foi trocado porque é gosto, não
defeito de layout: a troca é uma linha em `imgui_settings.h`
(`inline ImVec4 accent`) e atinge 28 pontos do fork.

---

## 3c-bis. LOADER — carregar, descarregar, iniciar (18/10/2026)

O cheat deixou de ter boot automático. A janela inicial é um **loader** com três
ações separadas, e o END passou a significar "fim de SESSÃO", não "fim de
processo".

| Fase | O que aparece | `ShowMenu` |
|---|---|---|
| `FASE_LOADER` | painel com Carregar/Descarregar driver + Iniciar PUBG | **true** (senão não há clique — a janela fica `WS_EX_TRANSPARENT`) |
| `FASE_AGUARDANDO` | aguardando `TslGame.exe`, com Cancelar | true |
| `FASE_RODANDO` | menu + ESP | controlado pelo HOME |

`END` levanta `DirectOverlay::PedidoFimSessao` (o WndProc não pode chamar
`PararSessao`: o UNMAP tem `Sleep` e travaria a fila de mensagens). O
`RenderFrame` lê a flag: com sessão viva, `PararSessao()` e volta ao loader; no
loader, sai do processo.

`PararSessao()` **não toca no driver**: para a MemoryLoop (espera
`GMemThreadViva` cair), derruba a Engine e volta para `FASE_LOADER`. O UNMAP é
decisão explícita do botão **Descarregar driver**.

### O BUG DO SEED — invariante perdida ao mover código

Primeira versão do loader: clicar em **Carregar driver** mostrava "falha ao
mapear" e o `logs.txt` **parava** em "anti-cheat limpo — mapeando agora".

Causa: o seed era sorteado **dentro de `InitializeFeaturesThread`**, e o loader
não passa mais por lá. `GetSeed()` devolvia **0** → o nome do device era
derivado de zero → o `DriverEntry` recusava a carga com
`STATUS_INVALID_PARAMETER` → e `LoadDriverInternal` **engolia o status**: o
`MapDriver` devolve `true` quando a *chamada* funciona, mesmo com o `DriverEntry`
falhando. Resultado: falha alta e silenciosa.

Consequência no código:

1. `GarantirSeed()` — idempotente, chamado por **todo** caminho que mapeia
   (loader, sessão, `InitializeFeaturesThread`);
2. `LoadDriverInternal` agora **loga o status do `DriverEntry`** e nomeia os
   dois casos que importam: `0xC000000D` (seed inválido) e `0xC00000A1`
   (`STATUS_DEVICE_BUSY`, carga anterior viva).

Lição registrada: mover um bloco de código **não** move as invariantes dele. O
seed não era "uma linha no init", era um pré-requisito do mapeamento — e agora
mora na função que o garante.

---
## 3d. HISTÓRICO — o BSOD de 0x50 no `DriverEntry` do driver antigo

> **Seção histórica.** Descreve o driver **antigo** (`Ndu.sys` /
> `PUBG_KM.sys`), hoje aposentado. O driver atual (`km_safe_source.c`) já nasceu
> com `IoCreateDriver` e nunca caiu aqui. Estas três quedas fazem parte das
> **6** que aposentaram o driver antigo.
>
> (A numeração das subseções de §3 é cronológica, não alfabética: o `3d` foi
> escrito antes do `3c`.)

> **LEIA ISTO PRIMEIRO.** A seção logo abaixo, "endurecimento do caminho
> físico", foi escrita quando eu **supus** que o crash era leitura de memória
> física. **Estava errado.** O crash era no `DriverEntry`, antes disso. O
> endurecimento não era inútil — mas não era a causa. A causa está aqui.

**Aconteceu TRÊS vezes, com a mesma assinatura:**

```
PAGE_FAULT_IN_NONPAGED_AREA (0x50)
  Arg1 (endereço)   : 0xFFFFFFFFFFFFFFD0     (-0x30 a partir de NULL)
  Arg2 (acesso)     : 0x0000000000000002     ESCRITA
  Arg3 (instrução)  : nt+0x...b964

PROCESS_NAME: kdmapper.exe
FAILURE_BUCKET_ID: AV_W_(null)_nt!MiSystemFault
```

Ocorrências: 30/09 18:08, 30/09 23:40, 01/10 00:26 — as duas primeiras com
`Arg1 = -0x30`, a terceira com `-0x20`. Mesmo defeito, deslocamento variando.

### Como a causa foi encontrada

`C:\Windows\MEMORY.DMP` tem ACL do SYSTEM e não dá para copiar. Mas o
**minidump** em `C:\Windows\Minidump` é legível (1,6 MB). E o **WDK 28000
instalado traz o `kd.exe`**:

```
C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\kd.exe
```

```
kd -z <dump> -y "srv*C:\symbols*https://msdl.microsoft.com/download/symbols" \
   -c "!analyze -v; .ecxr; kb; q"
```

Stack trace que saiu:

```
nt!KeBugCheckEx
nt!MiSystemFault+0x1b70a3
nt!MmAccessFault+0x400
nt!KiPageFault+0x36d
nt!ObfReferenceObject+0x24        <-- falta de página AQUI
nt!IoCreateDevice+0x2bb
<nosso DriverEntry>
```

### A causa

O código fazia:

```c
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObj, PUNICODE_STRING RegistryPath)
{
    IoCreateDevice(DriverObj, 0, &g_DeviceName, ...);   // <-- BSOD
    DriverObj->MajorFunction[IRP_MJ_CREATE] = ...;      // <-- e isto
    DriverObj->DriverUnload = ...;                      // <-- e isto
}
```

O `kdmapper` monta o `_DRIVER_OBJECT` **à mão**, e ele não tem todos os campos
que o I/O manager espera. `IoCreateDevice` chama `ObfReferenceObject` em
`DriverObject->DriverExtension->ServiceKey`. Com o objeto do kdmapper esse
campo é **NULL**, a referência escreve em `NULL - 0x30`, e **`0x50` não é pego
por `__try/__except`** — bugcheck imediato.

Escrever em `DriverObj->MajorFunction[]` e `DriverObj->DriverUnload` também é
arriscado: a memória desse objeto pode ser read-only.

### O fix

`DriverEntry` agora **cria o próprio driver object** com `IoCreateDriver`, que
aloca e inicializa tudo direito (incluindo `DriverExtension->ServiceKey`). O
`DriverObj` do kdmapper é descartado. Este é o padrão para driver mapeado
manualmente.

```c
static NTSTATUS KmDriverInit(PDRIVER_OBJECT DriverObj, PUNICODE_STRING Reg)
{
    // sanidade barata: transforma um BSOD num DbgPrint
    if (!DriverObj || !DriverObj->DriverExtension) return STATUS_INVALID_PARAMETER;
    IoCreateDevice(DriverObj, ...);
    ...
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObj, PUNICODE_STRING RegistryPath)
{
    return IoCreateDriver(NULL, KmDriverInit);
}
```

A checagem de `DriverExtension` é a parte que importa: **é a ausência dela que
derruba o `IoCreateDevice`**, e agora isso vira erro em vez de tela azul.

`Ndu.sys` compilava, importava `IoCreateDriver` (confirmado por leitura da
tabela de imports), e o `verify.ps1` dava `TUDO VERDE` — na época. Hoje nem o
driver nem o `verify.ps1` existem mais: o `Ndu.sys` foi para `_lixo\` (pasta
apagada) e o `verify.ps1` foi removido.

**O fix NÃO foi testado em runtime naquele momento.** Três BSODs no mesmo dia
foram motivo suficiente para parar e achar a causa antes de tentar de novo. Ele
foi validado depois, no `km_safe_source.c`: o `g_InitStatus` no
`C:\Windows\PkmSafe.txt` mostra `IoCreateDriver = 0x00000000` e
`g_InitStatus = 0x00000000` na carga boa.

### Como depurar um BSOD deste projeto

```powershell
# 1. o código do bugcheck
Get-WinEvent -FilterHashtable @{LogName='System'; Id=1001;
    ProviderName='Microsoft-Windows-WER-SystemErrorReporting'} -MaxEvents 5 |
  ForEach-Object { $_.Message }

# 2. o minidump e legivel; o MEMORY.DMP nao (ACL do SYSTEM)
Get-ChildItem C:\Windows\Minidump

# 3. analise com o kd.exe do WDK
$kd = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\kd.exe'
& $kd -z <caminho.dmp> -y 'srv*C:\symbols*https://msdl.microsoft.com/download/symbols' `
      -c '!analyze -v; .ecxr; kb; q'
```

Ferramenta própria para o cabeçalho, sem depender do kd:
[tools\dump_driver\minidump_info.py](tools/dump_driver/minidump_info.py).

**O `kd.exe` é a peça que faltava.** Sem ele eu passei duas rodadas supondo a
causa do `0x50` e errei nas duas. Com o stack trace, a causa apareceu numa
leitura.

---

## 3d-bis. HISTÓRICO — o endurecimento do caminho físico (suposição errada)

> Mesma observação da seção anterior: é o driver **antigo**. O driver atual não
> faz page-table walk nem `MmMapIoSpace`, então nada disto está no caminho
> crítico hoje. Vale como registro de raciocínio — inclusive do erro.

**Aconteceu duas vezes, com o mesmo bugcheck:**

```
PAGE_FAULT_IN_NONPAGED_AREA (0x50)
  endereço que faltou : 0xFFFFFFFFFFFFFFD0      (= -0x30 a partir de NULL)
  tipo de acesso      : 0x2 = ESCRITA
  instrução           : dentro de nt
```

O evento está no log do sistema (`Id 1001`,
`Microsoft-Windows-WER-SystemErrorReporting`). O `MEMORY.DMP` foi salvo mas
**não é legível sem privilégio de SYSTEM** — `Copy-Item` e `copy` deram
"acesso negado". O
[tools\dump_driver\kdump_info.py](tools/dump_driver/kdump_info.py) existe para
ler esse cabeçalho quando o dump estiver acessível.

### A causa

```c
// ANTES — sem validação nenhuma
PVOID mapped = MmMapIoSpaceEx(pa, size, PAGE_READONLY);
if (!mapped) mapped = MmMapIoSpace(pa, size, MmNonCached);
if (!mapped) return STATUS_INSUFFICIENT_RESOURCES;
RtlCopyMemory(buffer, mapped, size);
MmUnmapIoSpace(mapped, size);
```

`MmMapIoSpaceEx` aceita **qualquer `ULONG64`** como endereço físico. Se o
page-table walk devolver lixo — PFN zerado, bit de flag sobrando, endereço
acima do topo da RAM — o mapeamento é criado sobre memória que não existe.

E o detalhe que torna isso fatal:

> **`PAGE_FAULT_IN_NONPAGED_AREA` (0x50) NÃO é pego por `__try/__except`.**
> É bugcheck imediato, não exceção. Por isso o `__try` do `KmFindModuleBase`
> não ajudou em nada — a falha era no `KmReadPhysical`, e ali não havia
> proteção nenhuma.

### O que mudou

1. **`KmReadPhysical` valida antes de mapear:** `physical != 0`,
   `physical < 2^52`, sem overflow em `physical + size`.

2. **`KmReadPhysical` inteiro dentro de um `__try`** (mapeamento + cópia).
   Um 0x50 real ainda bugchecka, mas falha de página paginável, endereço não
   canônico e violação de proteção viram `STATUS` e o driver continua vivo.
   **Isso transforma um BSOD num log**, que é a diferença que importa num
   teste.

3. **`KmWalkPageTables` recusa PFN zerado em todos os níveis** — CR3, PDPT,
   PD e PT. Mapear o físico 0 é o caminho mais direto para o bugcheck.

### Honestidade sobre o que a validação cobre

O teste de teto (`< 2^52`) é **redundante** para valores que já passaram pelo
`KM_PTE_PFN_MASK`, porque a máscara recorta em 52 bits. Está anotado assim no
fonte. Quem protege de verdade é o teste `== 0` e o `__try`. O teto fica
porque `KmReadPhysical` também é chamado com `PageBase + PageOffset`, que é
soma e pode estourar.

**Os PFNs intermediários (PML4/PDPT/PD) não são validados contra o topo real
da RAM** — isso exigiria `MmGetPhysicalMemoryRanges` e não há fonte confiável
do topo sem ele. O `__try` cobre o resto.

### Verificação

O [pagewalk_check.py](tools/dump_driver/pagewalk_check.py) agora espelha as
validações do driver e tem **9 casos negativos** (eram 5):

```
CR3 com PFN zerado
PDPT presente com PFN zerado
PD presente com PFN zerado
PT presente com PFN zerado
```

E um erro meu tirado no caminho: a primeira versão do limite era **1 TB**, e
as tabelas sintéticas do teste vivem em ~17 TB — plausível numa máquina real
x64. O limite certo é 2^52, que é o que o formato de página endereça.

### O que a análise estática do AsIO3 NÃO conseguiu provar

Tentativa de achar a checagem de chamador com
[xref_import.py](tools/dump_driver/xref_import.py) e
[xref_string.py](tools/dump_driver/xref_string.py):

```
PsGetProcessImageFileName          chamadas diretas: 0
_stricmp                           chamadas diretas: 0
wcsstr                             chamadas diretas: 0
caminho do servico (0x140007B50)   0 referencias (lea rip-rel)
```

Zero em tudo. Ou seja: **a hipótese "ele compara o nome do processo com
AsusCertService" está errada, ou o driver resolve esses endereços de um jeito
que não dá para seguir estaticamente.**

Suspeita (não confirmada): as entradas da IAT podem estar sendo
sobrescritas em runtime, ou o driver monta as chamadas por outro caminho.
O `_EPROCESS` também pode estar sendo lido direto, sem passar por
`PsGetProcessImageFileName`.

**Lição:** análise estática deu a direção, mas a resposta veio de teste em
runtime. Não confundir as duas.

### O que o teste em runtime provou

Sonda de `CreateFileA` em `\\.\Asusgio3`, três níveis de acesso:

| estado do serviço | `R+W` | `GENERIC_READ` | acesso `0` |
|---|---|---|---|
| **Stopped** | erro 2 | erro 2 | erro 2 |
| **Running** | erro 5 | erro 5 | erro 5 |

Duas conclusões:

1. **O device só existe com o serviço rodando.** Com o serviço parado,
   `IRP_MJ_CREATE` nem chega no driver — o symlink não está lá. Isso explica
   o erro 2.

2. **Com o serviço rodando, o acesso é negado em TODOS os níveis de
   acesso** — inclusive `dwDesiredAccess = 0`. Pedir zero acesso não pede
   permissão nenhuma; se ainda assim dá `ACCESS_DENIED`, então não é ACL de
   objeto. É o driver recusando dentro do `IRP_MJ_CREATE`.

### Tentativas que NÃO destravaram o AsIO3

Todas medidas, todas ainda com erro 5:

| tentativa | resultado |
|---|---|
| `Asusgio3` parado | erro 2 (device nem existe) |
| `Asusgio3` rodando, sem o serviço chamador | **erro 5** |
| registrar e subir `AsusCertService` (`sc.exe create ... start= auto`) | `Running`, PID 8660 — **erro 5** |
| `Asusgio3` + `AsusCertService` os dois `Running` | **erro 5** |
| renomear o nosso exe para `AsusCertService.exe` e rodar | **erro 5** |
| `AsusCertService` **parado** (contraprova) | **erro 5** |

A última linha é a mais informativa: **erro 5 com o serviço parado E com ele
rodando.** Ou seja, o serviço não é o que o driver checa. Isso está
descartado com dado, não com opinião.

**E o driver não faz checagem de segurança nenhuma** — confirmado por
varredura de imports:

```
--search Token       nenhuma API
--search Privilege   nenhuma API
--search Sign        nenhuma API
--search Cert        nenhuma API
--search Integrity   nenhuma API
--search Access      nenhuma API
--search Security    nenhuma API
```

Não há `SeAccessCheck`, `SeSinglePrivilegeCheck`, nem validação de token ou
assinatura. As strings de CA que aparecem no binário são **o certificado de
assinatura do próprio driver**, não verificação.

### O que também não funcionou como teste

Tentei apontar o `AsusCertService` para o nosso próprio `RuntimeBroker.exe`
(`sc.exe config AsusCertService binPath= ...`) para testar a hipótese de que
o driver olha o **caminho da imagem** em vez do nome. Resultado:

```
[SC] StartService FALHA 1053
```

**Erro meu:** `1053` é "o serviço não respondeu no tempo". O
`RuntimeBroker.exe` é um app de GUI — ele não chama
`StartServiceCtrlDispatcher`, então o SCM desiste depois de 30 segundos e
**o processo nunca chega a rodar**. O teste não testou nada.

Para testar essa hipótese de verdade o serviço precisa de um binário que
**responda ao SCM** (ou seja, um wrapper de serviço de verdade), rodando no
caminho exato que o driver armazena.

### Estado final do AsIO3

**O critério de aceitação do `AsIO3.sys` não foi extraído, e a rota não
fechou.**

Tentativa de achar o handler de `IRP_MJ_CREATE` pela tabela de dispatch
(`drv_dispatch.py`) produziu falsos positivos em massa e **foi removida** —
não achou o handler. Não vou entregar número inventado.

Tudo que sobrou exige **elevação** (escrever em `Program Files (x86)`,
`sc.exe config`, `sc.exe start`) e a sessão do agente é **não elevada**:

```
[SC] StartService: OpenService FALHA 5: Acesso negado.
```

**Primitiva confirmada que continua valendo:** se o device algum dia abrir,
`ZwOpenSection` (4 chamadas) + `ZwMapViewOfSection` (5 chamadas) dão memória
física, que é metade do que o cheat precisa.

### Varredura de doadores — o resultado que decide a opção 2

`scan_donors.py` sobre **799 drivers** instalados, cruzando assinatura e
primitivos:

```
=== NAO-MICROSOFT com primitivo de memoria: 1 de 799 ===
   210  AsIO3.sys   assinatura Valid, CN=ASUSTeK
        PHYS:3  MDL:2  IOCTL:2  MISC:4  PCI:2  PROC:1  NOISE:1
```

**O `AsIO3.sys` é o único doador instalado nesta máquina.** Todo o resto é
Microsoft. Os da NVIDIA estão no `DriverStore` e o scanner não os pegou.

Estado dos bloqueios — a opção 2 (`gdrv.sys`) é tecnicamente viável:

| verificação | resultado |
|---|---|
| Windows Defender ativo | **não** (`AntivirusEnabled: False`) |
| HVCI / integridade de memória | **desligado** (chave não existe) |
| `VulnerableDriverBlocklistEnable` | não forçado |
| driver de terceiro não-WHQL carrega? | **sim** — o próprio `AsIO3` prova |

**Mas o risco é real e eu não recomendo baixar de site de terceiro.** Um
`.sys` é código de kernel; uma cópia adulterada roda em ring 0. O caminho
defensável é o Gigabyte Control Center oficial, e **não há como verificar** se
a versão atual ainda traz o `gdrv.sys` vulnerável — a Gigabyte publicou
[aviso de segurança](https://www.gigabyte.com/hu/security/security-advisory/1801)
sobre isso. Se vier corrigido, a instalação foi em vão.

---

## 3c. HISTÓRICO — a rota BYOVD (`AsIO3`) e por que ela morreu

> **Rota descartada.** Toda esta seção é a investigação de uma alternativa ao
> kdmapper. Ela **não fechou**: o `AsIO3.sys` dá memória **física**, MSR e porta
> I/O, mas **não** dá memória virtual, e a checagem de chamador dele nunca foi
> satisfeita. O que fechou foi **kdmapper + `PUBG_KM_safe.sys`** (§3a), e é o
> que roda hoje. O texto fica porque o método ("medir antes de concluir") e os
> dados desta máquina continuam válidos.

O objetivo era achar um **doador BYOVD**: um driver com assinatura válida de
fabricante, já instalado, que exponha um primitivo de kernel utilizável. Isso
evita carregar código nosso no kernel (que é o que o kdmapper faz e o que
deixa rastro).

Ferramentas em `tools\dump_driver\`:

| Arquivo | Função |
|---|---|
| `pe_probe.py` | parser PE sem dependências: headers, seções, imports, strings, candidatos a IOCTL |
| `scan_donors.py` | varre todos os `.sys` da máquina, checa assinatura, pontua por primitivo |
| `copy_sys.c` | copia um `.sys` de `System32\drivers` para o projeto |

### Resultado (this machine)

```
799 .sys analisados
574 com assinatura Valid
```

**Nenhum driver de terceiro oferece leitura de memória virtual de um PID
alvo.** Os candidatos que pontuaram alto por `MmCopyVirtualMemory` são todos
Microsoft-signed e são exatamente `dxgkrnl.sys`, `Vid.sys`, `storport.sys`,
`fvevol.sys` — drivers cujo uso do primitivo é interno e protegido por
verificação de chamador.

O único terceiro com score relevante:

```
[210] AsIO3.sys    ASUSTeK COMPUTER INC.    C:\Windows\System32\drivers\AsIO3.sys
      categorias: PHYS:3 IOCTL:2 MDL:2 MISC:4 NOISE:1 PROC:1 PCI:2
```

Analisado com `pe_probe.py`:

```
Subsystem   NATIVE
DllChar     0x41E0 [HIGH_ENTROPY_VA, DYNAMIC_BASE, FORCE_INTEGRITY, NX_COMPAT, GUARD_CF]

ntoskrnl.exe (45 funcoes):
  MmMapIoSpace, MmUnmapIoSpace, MmGetPhysicalAddress, MmAllocateContiguousMemory
  ZwOpenSection, ZwMapViewOfSection, ZwUnmapViewOfSection
  PsSetCreateProcessNotifyRoutineEx, PsGetProcessImageFileName
  IoCreateDevice, IoCreateSymbolicLink, ObReferenceObjectByHandle
  KeBugCheckEx, ...
HAL.dll (3 funcoes):
  HalGetBusDataByOffset, HalSetBusDataByOffset, HalTranslateBusAddress
```

**Conclusão: `AsIO3.sys` não serve para o primitivo que precisamos.** Ele dá
acesso a memória **física** (`MmMapIoSpace`), MSR/porta e config space PCI.
Não importa `MmCopyVirtualMemory`, `ZwOpenProcess`, `KeStackAttachProcess` nem
`__writecr3`. Para ler a memória virtual do `TslGame.exe` com isso seria
preciso fazer page-table walk na mão (achar a CR3 do processo, andar
PML4/PDPT/PD/PT, mapear cada página física) — viável, mas é um projeto inteiro
e frágil.

### Caminhos que sobraram

| Opção | Custo | Risco |
|---|---|---|
| **A. Page-table walk via `AsIO3`** | alto (achar CR3, walker, tratamento de páginas grandes) | baixo — driver ASUS assinado, nada mapeado por nós |
| **B. kdmapper + driver próprio corrigido** | baixo — **foi o caminho escolhido**: é o `PUBG_KM_safe.sys` de hoje (§3a) | médio-alto — o kdmapper deixa header PE no pool e tag conhecida |
| **C. Mapper próprio (manual map)** | alto | médio — controlamos o rastro, mas cada passo tem sinal |
| **D. Driver doador de fora** (`RTCore64.sys` da MSI, `iqvw64e.sys` da Intel) | baixo | médio — precisam estar na blacklist do BE ou não |

`B` **foi** o escolhido — o `PUBG_KM_safe.sys` (§3a) resolve o "não derrubar a
máquina" e está em uso. `A` nunca fechou (o `CreateFile` no `AsIO3` continua
dando erro 5). A recomendação de "parar de depender de mapper" ficou sem
caminho viável nesta máquina.

### Auditoria de strings do `AsIO3.sys`

`tools\dump_driver\km_strings.py` extrai os caminhos e nomes que denunciam a
lógica do driver:

```
\Device\Asusgio3
\DosDevices\Asusgio3
\BaseNamedObjects\WaitForIoAccess
\Device\PhysicalMemory
\Registry\Machine\HARDWARE\RESOURCEMAP\System Resources\Physical Memory
\Registry\Machine\SOFTWARE\MICROSOFT\WINDOWS\CURRENTVERSION   -> ProgramFilesDir (x86)
\ASUS\AsusCertService\AsusCertService.exe
MmGetPhysicalMemoryRangesEx2   (resolvido por string)
RtlQueryModuleInformation      (resolvido por string)
ZwQueryInformationProcess      (resolvido por string)
```

E o contrato de IOCTL foi extraído por completo com
`tools\dump_driver\ioctl_xref.py`. Documento dedicado:
[`tools/dump_driver/AsIO3_IOCTL.md`](tools/dump_driver/AsIO3_IOCTL.md).

Resumo: família `FILE_DEVICE = 0xA040`, com `0xA0406400/6404/6408` para
leitura de baixo nível (MSR via `0F 30`/`0F 32`, porta via `ED/EE/EF`),
`0xA040A440/A444/A448` e `0xA040A540/A544/A548` para escrita em três larguras.

Três coisas importantes saem daí:

**1. Ele dá porta I/O e MSR, e acesso a memória física.** Abre
`\Device\PhysicalMemory` com `ZwOpenSection` + `ZwMapViewOfSection`, e importa
`MmMapIoSpace`. Nas versões modernas do Windows o objeto `PhysicalMemory` tem
DACL restritiva e é negado para user mode — um driver de kernel assinado
consegue.

**2. Ele valida quem chama.** Carrega o caminho de
`\ASUS\AsusCertService\AsusCertService.exe` e resolve `ProgramFilesDir (x86)`
no registry, além de importar `PsGetProcessImageFileName` e
`PsSetCreateProcessNotifyRoutineEx`. Não é driver aberto.

**3. Ele NÃO dá memória virtual.** Não importa `MmCopyVirtualMemory`,
`ZwOpenProcess` nem `KeStackAttachProcess`. Para ler a memória do
`TslGame.exe` com ele ainda é preciso page-table walk — que já está
implementado do nosso lado.

### Onde a rota A parou (não destravou)

> O título original desta subseção era "destravada". **Não destravou.** Com o
> serviço rodando e o `AsusCertService.exe` no caminho que o driver espera, o
> `CreateFile` continuou devolvendo **erro 5** em todos os níveis de acesso — ver
> "Tentativas que NÃO destravaram o AsIO3", abaixo. O que está verificado nesta
> máquina é o estado do serviço e da assinatura:

Verificado nesta máquina:

```
servico   Asusgio3   Type=1 (kernel)   Start=3 (manual)   Stopped
binario   C:\Windows\System32\drivers\AsIO3.sys      59344 bytes
assinado  ASUSTeK COMPUTER INC.   Status: Valid
device    \Device\Asusgio3  ->  \DosDevices\Asusgio3

chamador esperado pelo driver:
  C:\Program Files (x86)\ASUS\AsusCertService\AsusCertService.exe
  --> EXISTE, assinado por ASUSTeK, versao 1.02.22 (a MESMA do driver)
```

O driver está instalado como **serviço legítimo de kernel** e o binário que
satisfaz a checagem de chamador está no caminho exato que ele espera
(`ProgramFilesDir (x86)` + `\ASUS\AsusCertService\`). Não precisa de kdmapper,
nem patch, nem spoof de nome de processo.

| Passo | Estado |
|---|---|
| 1. abrir `\\.\Asusgio3` | **bloqueado** — erro 5 em todos os níveis de acesso; o cliente `AsIO3::Client` existe em `PubgExt\driver\AsIO3_Client.h` |
| 2. primitivo de memória física | **esperado funcionar** — a sonda nunca chegou a exercitá-lo, porque o passo 1 não passa |
| 3. achar a CR3 do alvo | era `IOCTL_GET_CR3` do `Ndu.sys` — **o driver atual não expõe CR3** |
| 4. andar PML4→PDPT→PD→PT | implementado no driver antigo, validado por `pagewalk_check.py` |
| 5. ler a página física | era `IOCTL_READ_VIRT_PHYS` — **também só no driver antigo** |

O contrato está fechado e **validado contra o binário**:
`tools\dump_driver\asio3_check.py` recalcula as macros de `AsIO3_Client.h` e
compara com os valores extraídos do `.text`. Resultado: **33 declarados, 33
confirmados**. (Sobraram 10 IOCTLs no driver que o header não cobre — não são
necessários.)

### A sonda

`RunAsIO3Probe()` em `PubgExt\DriverSelfTest.cpp` roda junto do self test
quando não há jogo aberto. Ela **não carrega nada** — só:

1. consulta o estado do serviço `Asusgio3`
2. confere se o `AsusCertService.exe` está no caminho esperado
3. abre `\\.\Asusgio3`
4. bate em 9 IOCTLs de INFO/CFG (os inofensivos) e conta quantos respondem

Se algum responder, a checagem de chamador passou e o primitivo está
acessível. Se nenhum responder, o log aponta a checagem como suspeita.

Como rodar:

```
sc start Asusgio3                                  (precisa de admin)
Build\Release\PubgExt\mtmacr0.exe            (sem o jogo aberto)
type Build\Release\PubgExt\logs.txt                (procure "SONDA AsIO3")
```

### Verificação estática do contrato

`tools\dump_driver\contract_check.py` compara os dois lados do contrato e
reprova se divergirem. Cobre:

1. códigos de IOCTL (nome → valor, método, acesso);
2. campos e **tamanho** das structs `KM_*`, campo a campo com offset;
3. `KM_MAX_REQUEST_PAYLOAD` e `KM_MAX_NAME`;
4. nomes de device/symlink, e se o user mode usa o macro em vez de literal.

Ele tem `--selftest`, que roda contra cópias mutadas do contrato e exige que
cada mutação seja detectada:

```
5/5 mutacoes detectadas
  IOCTL com func trocado / campo removido / KM_MAX_NAME diferente
  device name diferente / payload maximo diferente
```

Isso importa porque **compilar não prova nada nesse ponto**: os dois lados
compilam felizes com números trocados. Foi exatamente esse o bug original.

Estado atual:

```
RESULTADO: contrato CONSISTENTE  (0 aviso(s))
  IOCTL_READ_MEMORY        0x00222004
  IOCTL_GET_MODULE_BASE    0x00222008
  IOCTL_GET_PID            0x0022200C
  IOCTL_WRITE_MEMORY       0x00222010
  KM_REQUEST               32 bytes
  KM_MODULE_REQUEST       296 bytes
  KM_PID_REQUEST          268 bytes
```

---

## 4. MemProcFS/DMA — resolvido (arquivado)

A decisão que estava pendente foi tomada: **não volta DMA**. O que sobrou da
arquitetura original foi para a quarentena, não para o lixo:

```
_lixo\_legacy\dma_paths\InputManager.cpp    gafAsyncKeyState via VMMDLL_*
_lixo\_legacy\dma_paths\Registry.cpp        VMMDLL_WinReg_QueryValueExU
_lixo\_legacy\dma_paths\vmmdll.h, leechcore.h
_lixo\_legacy\dma_paths\build_loader.bat, build_renderer.bat
```

Nada disso está no `PubgExt.vcxproj` — é código morto e não quebra o build.
O `Memory` atual não fala `VMMDLL_*`: não existe `vHandle` nem
`VMMDLL_Map_GetEATU`.

Por que não voltar: o caminho DMA exige `vmm.dll` + `leechcore.dll` +
`dbghelp.dll` + `symsrv.dll` ao lado do exe. E o `InputManager.cpp` decide o
método de leitura de input por versão do Windows (`CurrentBuild > 22000` →
`win32ksgd.sys`; senão → EAT de `win32kbase.sys`). Nesta máquina (build
**19045**, Windows 10 22H2) cairia no `win32kbase.sys`, que depende de
`symsrv.dll`/`dbghelp.dll`.

O driver KM faz o mesmo trabalho com 4 arquivos e zero dependência externa.

---

## 5. Código morto — tudo em quarentena

A limpeza recente esvaziou o repositório de overlay alternativo. Nada abaixo
está no `PubgExt.vcxproj`; tudo foi para `_lixo\`:

| Arquivo | Situação |
|---|---|
| `_lixo\_legacy\alt_overlays\overlay_dir\overlay.cpp` | `ExcludedFromBuild`; incluía `../game/entity.h`, `../SDL2`, `../discord` — pastas que não existem |
| `_lixo\_legacy\alt_overlays\overlay_dir\esp_renderer.h` | mesmo caso; `#include "../game/entity.h"` quebrado |
| `_lixo\_legacy\alt_overlays\DiscordOverlay.h`, `DiscordHijack.h`, `NvidiaOverlay.h` | headers de overlay alternativo. O `DirectOverlay.h` **é** o caminho usado |
| `_lixo\_legacy\alt_overlays\` | injeção em `explorer.exe`/DWM — ver §3b |
| `_lixo\driver_stub\stub_driver.c` + `HardenStub.sys` | experimento de assinatura por doador — ver §7 |
| `_lixo\` (raiz) | `.obj`/`.exe`/`.lib`/`.exp` de compilações manuais, `.bak` de fontes, `NOTES.md`, drivers antigos |
| `km_driver_source.c` | driver **aposentado**, mantido na raiz como referência histórica (ver §7) |

---

## 6. Limpeza e estrutura de pastas

Os `.obj`/`.exe`/`.lib`/`.exp` que caíam na raiz vinham de `build_loader.bat` e
`build_renderer.bat`, que chamavam `cl.exe` **sem `/Fo`** — o diretório de
trabalho era a raiz do repo. `tools\build_all.ps1` passou a usar
`/Fo"$Root\Build\obj\<Projeto>\\"`.

Depois de uma limpeza mais recente, tudo o que não é fonte viva está em `_lixo\`
(**102 MB, pode apagar**): `_legacy\` (overlays alternativos, caminhos DMA,
becos de assinatura), `driver_stub\`, `sig\`, `hv\`, `asiolab\`,
`release_junk\`, `_offsets\`, `_samples\`, `_tools\`, `logs\`, mais os `.bak` de
`Aimbot.cpp`, `Engine.cpp`, `Globals.h`, `driver_interface_v3.cpp`,
`km_driver_source.c` e os drivers antigos (`Ndu.sys`, `Ndu_com_canal.sys`,
`PUBG_KM.sys`, `HyperVisor.sys`).

A raiz hoje:

```
C:\Users\Bruno\PubgExt\
  PubgExt\               (4 MB)  codigo-fonte (SDK, ESP, Config, Memory, OS-ImGui, driver/)
  tools\                 (1 MB)  build_all.ps1, build_safe_driver.ps1,
                                 TestHarness.cpp, RunSafeTest.cmd
  Build\                (323 MB) Release\PubgExt\ (o que roda) + obj\ (regeneravel)
  _lixo\                (102 MB) quarentena — pode apagar
  km_safe_source.c               FONTE DO DRIVER ATUAL
  km_driver_source.c             driver ANTIGO (referencia historica)
  ARQUITETURA.md                 este documento
  PubgExt.sln
  .git\   secrets\   .gitignore
```

**Duas inconsistências no `.gitignore`** (pequenas, mas chatas):

- ele **não cobre `_lixo/`** — que são 102 MB de quarentena;
- as exceções (linhas `!Build/Release/PubgExt/...`) ainda apontam para
  `PUBG_KM.sys`, `vmm.lib` e `leechcore.lib`, arquivos que **não existem mais**.
  A exceção que importa hoje (`kdmapper.exe`) está lá; falta a do
  `PUBG_KM_safe.sys` se ele precisar ser versionado.

---

## 7. Os dois drivers: o atual e o aposentado

| | `km_safe_source.c` (**atual**) | `km_driver_source.c` (aposentado) |
|---|---|---|
| Artefato | `Build\Release\PubgExt\PUBG_KM_safe.sys` (16.896 bytes) | `PUBG_KM.sys` / `Ndu.sys` / `Ndu_com_canal.sys` |
| Build | `tools\build_safe_driver.ps1` (cl+link, WDK 10.0.28000.0, `/NODEFAULTLIB /SUBSYSTEM:NATIVE`) | manual, cl+link com WDK |
| Device | `\Device\PUBG_KM_<único por carga>` + symlink `\DosDevices\PUBG_KM` | `\Device\Ndu` → `\DosDevices\Ndu` (e `\Device\PUBG_KM`) |
| IOCTLs | `PING 0x8FF`, `GET_PID 0x801`, `GET_MODULE_BASE 0x802`, `READ 0x803` | contrato `KM_REQUEST` + `GET_CR3` + `READ_VIRT_PHYS` |
| Leitura | `MmCopyVirtualMemory` | `MmCopyVirtualMemory` (+ page-table walk) |
| Estado | **em uso** — 11/11 no harness | **6 BSODs** — binários em `_lixo\`, fonte só como referência |
| Log | `C:\Windows\PkmSafe.txt` | idem (mesmo `KmLog`) |

O `driver_stub\stub_driver.c` (→ `HardenStub.sys`, o experimento de assinatura
por doador) está em `_lixo\driver_stub\`. Nada disso é usado hoje.

`NOTES.md` (agora em `_lixo\`) diz que a máquina de dev tem Windows 11 25H2 com
HVCI **ON** e Smart App Control **ON**. Estado real desta máquina: **Windows 10
Pro 22H2, build 19045.6456**, VBS/HVCI **desligados**. A divergência importa —
HVCI/SAC e `win32ksgd` dependem da versão.

---

## 8. Toolchain

### Instalado e verificado

| Item | Versão / caminho |
|---|---|
| Visual Studio | Community **2026**, 18.10.12224.181, `C:\Program Files\Microsoft Visual Studio\18\Community` |
| MSBuild | `MSBuild\Current\Bin\MSBuild.exe` |
| MSVC | 14.51.36231 (**v145**) e 14.44.35207 (**v143**) — `cl.exe`, `link.exe`, `lib.exe`, `ml64.exe` |
| ATL | presente (`atlmfc\include\atlbase.h`) |
| MASM customizations | `MSBuild\Microsoft\VC\v180\BuildCustomizations` |
| Windows SDK | **10.0.28000.0** e 10.0.26100.0 — `windows.h`, `kernel32.lib`, `rc.exe`, `signtool.exe` |
| WDK | **10.0.28000.0** e 10.0.26100.0 — `build\<ver>\WindowsDriver.Default.props`, `km\ntifs.h`, `km\x64\ntoskrnl.lib` |
| Debugging Tools | `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll` |

Todos os quatro artefatos compilam. Intermediários vão para `Build\obj\<Projeto>\`.

### Por que o WDK 28000 e não o 26100

O toolset `WindowsKernelModeDriver10.0` do VS 2026 importa as build tasks do
WDK e pede `Microsoft.DriverKit.Build.Tasks.**18.0**.dll`:

```xml
<!-- MSBuild\Microsoft\VC\v180\Platforms\x64\PlatformToolsets\WindowsKernelModeDriver10.0\Toolset.props -->
<UsingTask TaskName="ValidateNTTargetVersion"
           AssemblyFile="$(WDKContentRoot)build\$(WDKBuildFolder)\bin\Microsoft.DriverKit.Build.Tasks.$(VisualStudioVersion).dll"/>
```

* WDK 10.0.26100.0 → traz `...Tasks.**17.0**.dll` → `error MSB4062` no VS 2026.
  Esse WDK funciona com o **VS 2022** (`VisualStudioVersion` = 17.0).
* WDK 10.0.28000.0 → traz `...Tasks.**18.0**.dll` → build limpo no VS 2026.

`driver_stub\stub_driver.vcxproj` fixa
`WindowsTargetPlatformVersion = 10.0.28000.0` por causa disso. Sem fixar, o
MSBuild resolve `$(WDKBuildFolder)` pelo SDK mais novo instalado e pode apontar
para um `build\<ver>` que não existe.

### Falta

| Item | Impacto |
|---|---|
| `git` | o repo `.git` existe no disco, mas o binário não está instalado |
| `vmm.dll`, `leechcore.dll`, `dbghelp.dll`, `symsrv.dll` | **não** são mais necessários — o caminho DMA foi arquivado (§4) |
| dumper de offsets por sig scan | hoje o `offset_audit.py` depende de um dump manual a cada patch (§9) |

### Ajuste feito no projeto

`PubgExt.vcxproj` fixava `PlatformToolset = v143`. Agora cai para `v145`
automaticamente quando `v143` não está presente:

```xml
<PlatformToolset Condition="'$(PlatformToolset)'==''">v143</PlatformToolset>
<PlatformToolset Condition="'$(PlatformToolset)'=='v143' and !Exists('...v143\Microsoft.Cpp.$(Platform).v143.props')">v145</PlatformToolset>
```

### Scripts

| Script | Função |
|---|---|
| `tools\build_safe_driver.ps1` | **compila o driver atual**: `km_safe_source.c` → `Build\Release\PubgExt\PUBG_KM_safe.sys` |
| `tools\RunSafeTest.cmd` | um comando: confere que o jogo está fechado → kdmapper → mostra o último bloco do `PkmSafe.txt` → roda o `TestHarness.exe` |
| `tools\TestHarness.cpp` | o harness do driver: PING + leitura válida + casos perigosos |
| `tools\build_all.ps1` | build unificado: `PubgExt.sln` → `mtmacr0.exe`. Alvo `-Target RuntimeBroker` (**rótulo do script**, continua valendo) e `Driver` (obsoleto: `driver_stub\` não existe mais). `-Config`, `-Clean` |
| `tools\build_kernel_driver.ps1` | compila o driver **antigo** (`km_driver_source.c` → `Ndu.sys`). Só histórico |
| `tools\bootstrap_toolchain.ps1` | verifica VS / SDK / WDK / git / runtime. `-VerifyOnly` |
| `tools\install_vs_components.ps1` | adiciona SDK, toolset v143 e extensão WDK na instância do VS |
| `tools\install_wdk.ps1` | baixa e instala o WDK (headers km + props do MSBuild) |
| `PubgExt\DriverlessProbe.cpp` | sonda SEM driver: scan e cabeçalho PE contra nós mesmos. Segura de rodar |
| `PubgExt\DriverSelfTest.cpp` | sonda COM driver, dentro do cheat: 3 caminhos de leitura, CR3, page walk. Precisa do `PUBG_KM_safe.sys` carregado |
| `tools\dump_driver\contract_check.py` | compara user mode × kernel: IOCTLs, structs, offsets, limites. `--selftest` (**escrito para o contrato antigo**) |
| `tools\dump_driver\pagewalk_check.py` | valida a aritmética do page-table walk contra tabelas sintéticas (driver antigo) |
| `tools\dump_driver\offset_audit.py` | compara `Globals.h` com um dump de offsets. `--dump` aponta outro |
| `tools\dump_driver\pubg_decrypt.py` | decrypt de FName em Python; lê as constantes do `Globals.h`. `--selftest` |
| `tools\dump_driver\pe_probe.py` | parser PE (headers, imports, strings, candidatos a IOCTL) |
| `tools\dump_driver\km_strings.py` | auditoria de strings/caminhos de um `.sys` |
| `tools\dump_driver\ioctl_xref.py` | acha as comparações de IOCTL no dispatcher |
| `tools\dump_driver\AsIO3_IOCTL.md` | contrato de IOCTL do `AsIO3.sys` extraído (§3c) |
| `build_phase0.bat`, `build_loader.bat`, `build_renderer.bat`, `build_driver.bat` | scripts antigos, todos em `_lixo\_legacy\` |

Todos exigem `-ExecutionPolicy Bypass` (a policy da máquina é `Restricted`):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\bootstrap_toolchain.ps1 -VerifyOnly
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_all.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_safe_driver.ps1
```

### Armadilhas do instalador do VS

* `setup.exe modify` **não** aceita `--wait` (retorna 87). Use
  `Start-Process -Wait`.
* O caminho de `--installPath` tem espaços. Passar array para `Start-Process`
  corta em `C:\Program`; tem que ir como uma única string citada.
* O componente `Component.Microsoft.Windows.DriverKit` instala **só a extensão
  do VS** (templates + PlatformToolsets). Headers de kernel, libs e os `.props`
  do MSBuild vêm do `wdksetup.exe` separado.

---

## 9. Pendências concretas

Ordenadas por impacto. O que já foi feito continua listado (riscado), como
histórico do que **não** precisa ser refeito.

### Driver — o que falta no `PUBG_KM_safe.sys`

1. **`WRITE_MEMORY` (0x804) não existe no driver.** O user mode declara o IOCTL
   e o `default` do dispatch devolve `STATUS_INVALID_DEVICE_REQUEST`. Enquanto
   isso, nada que dependa de escrita funciona.
2. `MmCopyVirtualMemory` exige `PASSIVE_LEVEL`. O dispatch atual está OK, mas
   **não** mover isso para worker thread sem checar o IRQL.
3. **Não existe verificador automático.** O `verify.ps1` foi removido e não
   voltou. Hoje a verificação é manual e em três passos (`build_safe_driver.ps1`
   → `build_all.ps1` → `check_lang.ps1`), sem um comando único que diga "está
   tudo certo" — e essa lacuna é real, não cosmética.
4. O `contract_check.py` valida o contrato `KM_REQUEST`, que **não** é o
   contrato atual (`DRIVER_REQUEST`). Vale reescrever o checker **ou** aceitar
   que a garantia passou a ser o `TestHarness` + o `PING`.

### Camada de memória

5. `Memory::FixCr3` não corrige CR3 — é um teste de leitura de 2 bytes no base
   address. Implementar de verdade ou renomear para `ProbeReadable`.
6. `Memory::FindSignature`, `DumpMemory`, `GetModuleList`,
   `GetExportTableAddress`, `GetImportTableAddress` são stubs que retornam 0.
7. `GetBaseSize` devolve `0x500000`; `current_process.base_size` é setado em
   `0x5000000` em `Memory.cpp`. Dois valores para a mesma coisa.
8. `BatchReadMemory` faz N IOCTLs sequenciais — não há batching real no kernel.
   O nome promete mais do que entrega. (O `KmShared.h`, canal por seção nomeada,
   foi a tentativa de resolver isso e não vingou.)

### Offsets

9. Vale um **dumper por sig scan** em vez de constante hardcoded: o
   `XenuineDecrypt` e o `DecryptCIndex` mudam a cada build e são exatamente as
   duas coisas que deixam o ESP mudo quando erram (§2).
10. O comentário de cabeçalho do `Globals.h` ainda diz `2609.1.2.11`, enquanto
    vários campos já trazem `// dump 2609.1.3.1`.

### Limpeza

11. ~~Mover os `.obj`/`.exe`/`.lib`/`.exp` da raiz.~~ Feito — `_lixo\` + `/Fo`
    no `build_all.ps1`.
12. ~~Decidir o destino de `Memory\InputManager.cpp`/`Registry.cpp`, de
    `PubgExt\overlay\*`, `Loader\`, `Renderer\` e `driver_stub\`.~~ Feito — tudo
    em `_lixo\` (§4, §5).
13. `_lixo\` tem **102 MB**. Quando não houver mais nada a consultar, apagar.
14. `.gitignore`: cobrir `_lixo/` e limpar as exceções que apontam para
    `PUBG_KM.sys`, `vmm.lib` e `leechcore.lib` — arquivos que não existem mais
    (§6).

### Consistência

15. `NOTES.md` (agora em `_lixo\`) descreve Windows 11 25H2 + HVCI ON. A máquina
    é Windows 10 22H2, com VBS/HVCI **desligados** (verificado).
16. Comentários de fontes antigas falam do driver antigo como se fosse o
    atual — o mesmo padrão de bug do §11. O `verify.ps1`, que fazia exatamente
    isso, foi removido em vez de corrigido.

### Contrato do driver — feito (histórico)

17. ~~Unificar os IOCTLs num único header.~~ Feito — `common.h` é a fonte da
    verdade.
18. ~~Reescrever a request sem ponteiros de user mode; validar
    `InputBufferLength`/`OutputBufferLength`.~~ Feito no desenho intermediário; o
    `km_safe_source.c` herdou a validação por caso (§3).
19. ~~Implementar `GET_PID`, `GET_MODULE_BASE`.~~ Feito — e os dois foram os que
    mais custaram: o `GET_MODULE_BASE` ficou várias rodadas devolvendo pid `0`
    por causa do `sysbuf` sobrescrito (§3a, item 8).
20. ~~Garantir que os dois lados não voltem a divergir.~~ Feito no desenho antigo
    (`contract_check.py`); no driver atual a garantia é o `TestHarness`.

### Overlay — feito

21. ~~Classe/título `PUBG_OVERLAY`.~~ Feito — gerados em runtime a partir de
    nomes de janelas do sistema (§3b).
22. ~~`SetWindowDisplayAffinity` + `DwmExtendFrameIntoClientArea`.~~ Removidos.
23. ~~Crash `0xC0000005` logo após "Initializing direct transparent overlay".~~
    Causa: `RenderFrame` desreferenciava `EngineInstance` (nulo até o Engine
    subir) no tab DEBUG. Agora tem guard, e a inicialização roda fora da thread
    de render.
24. ~~`DirectOverlay::Run` engolia falha de device e entrava em loop.~~ Retorna
    `bool` e limpa o que criou.
25. ~~Menu com 5 abas.~~ Agora são **8**, com `SeparatorText` (§2d).

### RCS — em uso, calibrável

26. O `Deg per Count` precisa de calibração por arma/FOV — é para isso que o log
    `[RCS]` existe (§2c).

---

## 10. Ordem sugerida de trabalho

1. **Criar a verificação automática** (§9.3). O `verify.ps1` citado pelo §0 não
   existe mais; hoje são três comandos manuais, sem um "tudo verde" no fim.
2. **`WRITE_MEMORY` no `PUBG_KM_safe.sys`** (§9.1). É a única peça funcional que
   falta no kernel, e o código do IOCTL (`0x804`) já está reservado no user mode.
3. **Offsets por sig scan** (§9.9). `XenuineDecrypt` e `DecryptCIndex` mudam a
   cada patch e são o que mais dói quando erram.
4. **Calibrar o RCS** com o log `[RCS]` ligado (§2c).
5. **Limpeza** (§9.13–9.14): apagar `_lixo\` quando não fizer mais falta.

---

## 11. Lição transversal — "assumir sem verificar" apareceu 5 vezes

Estes cinco bugs são da **mesma família**: eu escrevi com base no que achava que
era verdade, sem medir. Nenhum quebra o build — por isso valem mais que a lista
das correções.

| # | O que eu assumi | O que era de verdade | Sintoma |
|---|---|---|---|
| 1 | `CameraFov` seria o campo lido pelo ESP | **dois nomes para o mesmo conceito** (`CameraFov` e `CameraCacheFOV`); eu tinha atualizado o que ninguém lê | câmera errada / lixo |
| 2 | bastava o offset do `XenuineDecrypt` | a função **não estava mapeada** — uma ponta sem par | `UWorld` não decifra, ESP vazio |
| 3 | declarar o campo na struct de config bastava | faltava no `ToJson`/`FromJson` → o valor **sumia em silêncio** | config não persistia |
| 4 | casar o prefixo `"Weap"` era seguro | `"WeaponItem"` também começa com `Weap` → virava `onItem` | nome de arma errado |
| 5 | `ItemID` precisava de `DecryptCIndex` | `ItemID` é índice **cru**; o decrypt é do `ObjectID` | 100% dos itens `UNKOWNNAME` |

**As duas regras que ficaram:**

1. **Adicionar campo de config tem TRÊS partes** — a struct, o `ToJson` e o
   `FromJson`. Esquecer save/load faz o valor desaparecer **sem erro nenhum**.
2. **Medir antes de escrever.** O CENSO e os LOOTDIAG resolveram em **2 rodadas**
   o que 10 chutes não resolveram. E instrumento primeiro: quando o diagnóstico
   mente junto com o bug (§3a, item 8 — o dump saía contaminado pela doença),
   você fica sem o bug **e** sem a medida.

---

## 12. Ferramentas de diagnóstico — como usar

| Onde | O que tem |
|---|---|
| `C:\Windows\PkmSafe.txt` | log do **driver**, em append; cada carga abre um bloco `===== DriverEntry =====`. Mostra `inLen`/`outLen`/`pid`/`PsLookup stat`/`section base`/`g_InitStatus`. O `RunSafeTest.cmd` imprime só o último bloco |
| `Build\Release\PubgExt\logs.txt` | log do **cheat**, truncado a cada execução. Tags: `[STATE]`, `[SPEC]`, `[RCS]`, `[LOOTNAME]`, `[CENSO]`, `[MB]` |
| aba DEBUG → `Debug logs` | liga o **CENSO de classes** e os **LOOTDIAG** (logs pesados, desligados por padrão) |
| `TestHarness.exe` | valida o driver: PING + leitura válida + casos perigosos (ponteiro não-canônico, não-mapeado, tamanho absurdo, PID inválido) |
| `DriverlessProbe` | roda sem driver nenhum; não pode causar BSOD |

**O CENSO e os LOOTDIAG são a resposta para "o PUBG atualizou e o ESP quebrou".**
O CENSO lista **toda classe de ator do mapa** (foi ele que revelou os nomes reais
dos veículos e que o airdrop é `Carapackage_RedBox_C`); o LOOTDIAG testa **3
caminhos de offset de uma vez** e loga os nomes resolvidos lado a lado. É
atualização de offset resolvida em 2 medições em vez de 10 chutes.

### Roteiro quando algo quebrar

| Sintoma | Onde olhar |
|---|---|
| o cheat não acha o jogo | `logs.txt`, linhas `[MB]` e `[+] Found PID` |
| ESP vazio | aba DEBUG → `Debug logs` e olhe `[STATE]`: se `UWorld=0x0` ou `GNames=0x0`, o problema é **decrypt/offset**, não o ESP |
| `CreateFile` do driver falhando (`err=433 ERROR_NO_SUCH_DEVICE`) | `DO_DEVICE_INITIALIZING` (§3a, item 4) |
| nome de item errado, loot vazio | `[LOOTNAME]`; confira que o `DecryptCIndex` **não** está no caminho do `ItemID` (§2b) |
| veículo somando efeito/escapamento | filtro do CENSO (§2e) |
| recuo não compensado | `[RCS]`: confira `dropY`/`dropR`/`dpc` e a FOV (§2c) |

---

## 13. Fechamento — estado em 09/10/2026

### O que funciona hoje

| Área | Estado |
|---|---|
| Driver | `PUBG_KM_safe.sys` carrega pelo kdmapper, responde ao `PING` e lê memória de outro processo; **11/11** no `TestHarness`, com a máquina de pé |
| Recarga sem reboot | funciona — device com nome único por carga + symlink recriado + fallback de driver object |
| Overlay/menu | `DirectOverlay` no próprio processo, **8 abas**, HOME alterna, **END** encerra (panic key) |
| ESP de jogadores | lê atores e resolve `UWorld`/`GNames` com o decrypt novo do **2609.1.3.1** |
| ESP de veículos | filtro tirado do CENSO (exclui `P_` e componentes, casa nomes reais) — pega `PicoBus`, `Dirtbike`, etc. |
| ESP de loot | nome real do item por `DroppedItem → ItemTable → ItemID` (**sem** `DecryptCIndex`), com catálogo auto-populável na aba WORLD |
| Airdrop | `Carapackage_RedBox_C` reconhecido |
| RCS | feed-forward do acumulador drenado (`RecoilValueVector`/`VerticalRecovery`), calibração por `Deg per Count` |
| Config | auto-save ao fechar o menu em `Documents\Hunt\Default.json` |
| Diagnóstico | `PkmSafe.txt` (driver), `logs.txt` (`[STATE]`, `[SPEC]`, `[RCS]`, `[LOOTNAME]`, `[CENSO]`), CENSO/LOOTDIAG na aba DEBUG |

### O que ainda não funciona ou está frágil

- **`WRITE_MEMORY` não existe no driver** — o `0x804` só está declarado no user
  mode. Qualquer recurso que dependa de escrita não funciona (§9.1).
- **`contract_check.py` fala do driver antigo** — rodá-lo hoje confunde
  mais do que ajuda (§9.4). O `verify.ps1`, que tinha o mesmo problema,
  foi removido em vez de corrigido.
- **Offsets continuam sensíveis a patch.** Quando o PUBG atualizar, a ordem é
  `XenuineDecrypt` + `DecryptCIndex` primeiro, depois o resto (§2, §2b).
- **`MmCopyVirtualMemory` exige `PASSIVE_LEVEL`** — não mover para worker thread
  sem conferir o IRQL (§9.2).
- **`Memory::FixCr3`** tem um nome que promete o que não faz (§9.5).
- **O driver deixa rastro** (kdmapper em ring 0). É decisão consciente, não
  surpresa — a alternativa BYOVD não fechou (§3c) e a assinatura própria não
  compensou (§5).

---

## 14. VISIBLE CHECK VIA PHYSX — INVESTIGADO E REMOVIDO (build 2609.1.3.1)

**Status: REMOVIDO do código (11/10/2026).** Não há mais **nenhuma** leitura de
PhysX no cheat: a sonda `Engine::DiagPhysx()` foi apagada de `Engine.cpp`, a
declaração saiu de `Engine.h`, a chamada saiu de `UpdatePlayers` e os offsets
saíram do `Globals.h`. O ESP, o RCS, o desenho e as validações ficaram
**idênticos** — `IsVisible` continua fail-open (uma cor só), como já estava.

Esta seção existe para o caminho **não ser repetido às cegas**: o que foi provado
fica, o que falhou fica escrito, e uma retomada tem ponto de partida.

### A cadeia PROVADA (é o valor que fica)

```
base + 0x123BE088  -> NpPhyscis*              CONFIRMADO no jogo
                      (o 0x12C520C8 antigo do projeto era FALSO: cai em .pdata,
                       tabela de unwind — provado offline mapeando as seções do
                       TslGame.exe em disco; e no jogo devolveu lixo)
NpPhyscis + 0x8    -> mSceneArray (TArray)    CONFIRMADO
mSceneArray[i]     -> NpScene (2 cenas)       CONFIRMADO
NpScene + 0x2D0    -> mSQManager              CONFIRMADO (o 0x2450 do artigo do
                                               Notion era do FORTNITE, não do PUBG)
  mSQManager + 0x00 -> PrunerExt[0].mPruner (ESTÁTICO)  +0x2C ts
  mSQManager + 0x30 -> PrunerExt[1].mPruner (DINÂMICO)  +0x5C ts   [stride 0x30]
  -> os DOIS pruners são ponteiros de heap VÁLIDOS (log `[PHYSX] CENA[0]`)
```

**Custo medido:** leitura da cena **~1–5 ms**, **~10–40 IOCTLs**. Barato.

> **NOTA de custo — vale para qualquer plano futuro:** o `BatchRead` do nosso
> driver **não é batch**. É **1 `DeviceIoControl` por entrada**
> (`DriverInterfaceV3::BatchReadMemory`), então o custo é o **número de
> chamadas**, não os bytes. O driver aceita **1 MiB por chamada** — quem decide é
> o *agrupamento* do dado, não o tamanho dele.

### O que FALHOU (e por que o caminho parou)

O **`PruningPool`** — a lista de `{Shape, Actor}`, sem a qual não há geometria e
não há raycast — **não foi localizável de forma estável neste build**.

| Fonte do offset `AABBPruner -> PruningPool` | Resultado |
|---|---|
| `+0x1A0` (repo de referência `_physx_ref\PhysxPUBG`) | **0 candidatos** nos dois pruners |
| offsets do artigo do Notion (Fortnite) | não valem neste build |
| varredura de **32 KB** no pruner, **4 layouts**, com validação física | só falsos positivos |

Os dois falsos positivos que caíram:

| Candidato | O que *parecia* | O que o derrubou |
|---|---|---|
| `+0x3058` layout B | `N = 16.352`, `max = 7.045.889` | **span de 2,5 TB** (2.568.583 MB) — impossível num processo de 4 GB, e `max` não é potência de 2 |
| `+0x1168` layout D | **4083 objetos** (parecia ótimo) | achado em **1 de 5 runs** e **não reproduziu**; `max = 4181` e `23` **não são potência de 2** (o PhysX aloca dobrando) |

E o sinal que condena a varredura inteira: em **todos** os candidatos o teste de
payload deu **`pares 0/16`** — **nenhum** dos 16 primeiros `{Shape, Actor}` tinha
Shape **e** Actor de heap — e **`max = 2818` aparecia em TODOS** os candidatos com
`N` variando (581, 426, 103, 1047, 512, 82, 3...). Não são pools diferentes: é **a
mesma estrutura lida de offsets diferentes**.

### POR QUE REMOVEMOS (a razão, não só o fato)

Sem o pool não há lista de shapes; sem a lista não há geometria; sem geometria
não há raycast. Forçar o offset do run que "funcionou" produziria um visible check
que **MENTE** (diz "visível" com base em dado não confiável) — e **feature que
mente é pior que feature ausente**. Foi o critério do usuário do começo ao fim.

Somou-se a isso o pedido explícito: **ele não quer o cheat lendo memória do
PhysX**. Manter a sonda "só medindo" seria manter exatamente a leitura que ele
mandou tirar.

**Custo do reconhecimento: ~5 runs de 20 s.** A Fase 1 existia para isso —
descobrir a inviabilidade em horas, não em 30 — e foi o que aconteceu.

### SE ALGUÉM FOR RETOMAR (o caminho que sobra)

1. Os **dois pruners já são conhecidos e válidos** (a cadeia acima). O caminho
   **não é varredura heurística**: é **dump direto desses dois pruners** e achar o
   pool por **inspeção**. Foi a varredura que produziu os dois falsos positivos.
2. O instrumento que fazia esse dump existe no **histórico do git**
   (`Engine::DiagPhysx()`, ~2.300 linhas, removida em 11/10/2026). Reativá-la é
   mais barato que reescrever a sonda.
3. **Aviso:** o `.text` do `TslGame.exe` **em disco está cifrado (Xenuine)** — não
   dá para resolver offset por análise estática do arquivo. O `OffsetDumper` do
   projeto dumpa a imagem **viva** e serve para xref. Os scripts que sobraram em
   `_lixo\physx_*.py` foram o que provou o `.pdata` do `0x12C520C8`: servem de
   histórico, não de ferramenta.

### ALTERNATIVA REGISTRADA (próximo passo natural, NÃO implementado)

O campo **`0x75C`** lido no diagnóstico `[VIS]` mostrou **sinal real** — o "último
frame renderizado" do mesh:

```
[VIS] LOCAL  1m  v75C=1014.557
[VIS] INIM  56m  v75C=1014.557     <- acompanha o local
[VIS] INIM  72m  v75C=1006.179     <- diferença
[VIS] INIM 218m  v75C=-1000.000    <- sentinela
```

**Custo zero** (o campo já era lido). É **heurístico** (não geométrico), então não
é um visible check de verdade — é o candidato natural para substituí-lo. (O §2 já
registra que o `0x75C` foi trocado pelo bitfield `bRecentlyRendered = 0xBE8` no
caminho do aimbot; aqui ele reaparece como *sinal medido*, não como offset novo.)

**Fica como próximo passo registrado; NADA foi implementado.**

