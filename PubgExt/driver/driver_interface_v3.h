#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include "common.h"
#include "driver_interface_kevent.h"

#define MAX_BATCH_ENTRIES 128

// ---------------------------------------------------------------------------
// ESTADO DA INJECAO DE MOUSE — definidos no .cpp, lidos por quem tem LOG.
//
// O .cpp loga em `std::cout` (nao inclui o Pch.h), e isso NAO chega no
// logs.txt. Estes contadores deixam o diagnostico atravessar essa parede: o
// Aimbot.cpp le' e escreve na linha [RCS].
//
//   inj=none  -> InjectMouseMove nunca foi chamado
//   k=N/f=0   -> N sucessos, 0 falhas no IOCTL 0x805
//   err=      -> ultimo GetLastError da falha
// ---------------------------------------------------------------------------
extern std::atomic<uint64_t> g_mouseInjOk;
extern std::atomic<uint64_t> g_mouseInjFail;
extern std::atomic<uint32_t> g_mouseInjLastErr;

// ---------------------------------------------------------------------------
// VERSAO ESPERADA DO DRIVER — tem que bater com KM_DRIVER_VERSION no
// km_safe_source.c. Ver o comentario grande em GetDriverVersion().
//
// Fica no header (e nao no .cpp) porque o Main.cpp precisa dela para REPORTAR
// a divergencia ao usuario. Um numero duplicado em dois arquivos e' uma mentira
// esperando acontecer; este e' o unico lugar.
//
//   0x00010001 = 1.1  PING/GET_PID/MODULE_BASE/READ/MOUSE/VERSION
//   0x00010002 = 1.2  + DEACTIVATE/SELFDESTRUCT + nomes derivados de seed
// ---------------------------------------------------------------------------
#define KM_DRIVER_VERSION_ESPERADA 0x00010002

class DriverInterfaceV3 {
public:
    struct BatchReadEntry {
        union { uintptr_t Address; uintptr_t address; };
        union { size_t Size; size_t size; };
        union { void* Buffer; void* buffer; };
        BatchReadEntry() : Address(0), Size(0), Buffer(nullptr) {}
        BatchReadEntry(uintptr_t a, size_t s, void* b) : Address(a), Size(s), Buffer(b) {}
    };

    DriverInterfaceV3();
    ~DriverInterfaceV3();

    bool Initialize();

    // -----------------------------------------------------------------------
    // InitializeKevent — ativa o TRANSPORTE ALTERNATIVO (shared memory + KEVENT).
    //
    // Chama o `loader` DEPOIS de criar a secao, os eventos e a chave do
    // rendezvous: essa ordem e' obrigatoria (ver o comentario no
    // driver_interface_kevent.h). A partir daqui ReadMemory/BatchReadMemory/
    // GetProcessId/GetModuleBase passam a ir por ele.
    //
    // ESCOPO: ESP puro. Nao ha' KMP_OP_MOUSE, entao aimbot e RCS ficam sem
    // injecao neste backend — por desenho.
    // -----------------------------------------------------------------------
    bool InitializeKevent(bool (*loader)(uint64_t seed));

    bool UsandoKevent() const { return kevent != nullptr && kevent->HandleValido(); }

    // Base da imagem publicada pelo driver de KEVENT (0 se nao houver).
    // Usada no unload para o mapper liberar as paginas.
    uint64_t KeventImageBase() const { return (kevent != nullptr) ? kevent->GetImageBase() : 0; }

    // -----------------------------------------------------------------------
    // KEYREAD DE KERNEL (driver v4) — estado ALT/SHIFT vindo do kbdclass.
    //
    // `KeyreadKernelOk` e' a chave de troca: o HotkeyHook confere uma vez no
    // start da sessao, nao a cada tecla (custo zero por frame). Se o driver
    // for v3 ou o install do kbdclass tiver falhado, tudo segue pelo hook LL
    // como sempre — os dois caminhos estao sempre corretos em OR.
    // -----------------------------------------------------------------------
    bool KeyreadKernelOk() const { return (kevent != nullptr) ? kevent->KeyreadKernelOk() : false; }
    uint32_t KeyMaskKernel() const { return (kevent != nullptr) ? kevent->KeyMaskAtual() : 0; }

    // O servidor do kernel CONFIRMOU a parada (Ativo == 0)?
    // Sem isto o unload NAO libera as paginas: liberar com a thread viva
    // e' bugcheck imediato.
    bool KeventParadaOk() const { return paradaKeventOk; }

    // Telemetria do transporte KEVENT (protocolo v5): ultimo status global
    // devolvido + contadores mantidos pelo proprio driver no Hdr.
    uint32_t KeventLastGlobalStatus() const { return (kevent != nullptr) ? kevent->LastGlobalStatus : 0; }
    void KeventEstatisticas(uint32_t* badSig, uint32_t* throttled, uint32_t* watchdog) const
    {
        if (badSig) *badSig = 0;
        if (throttled) *throttled = 0;
        if (watchdog) *watchdog = 0;
        if (kevent == nullptr) return;
        kevent->EstatisticasKernel(badSig, throttled, watchdog);
    }

    // Diagnostico de exposicao (KMP_OP_EXPOSE): 16 numeros crus.
    // false se o backend nao for KEVENT ou o op falhar. `porque`: 0 ok,
    // 1 canal nao pronto, 2 round trip falhou, 3 op rejeitada, 4 sem backend.
    bool DiagnosticoExposicao(uint64_t saida[16], uint64_t tamanhoImagem = 0,
                              uint32_t* porque = nullptr)
    {
        if (kevent == nullptr) { if (porque) *porque = 4; return false; }
        return kevent->ExposeDiagnostico(saida, tamanhoImagem, porque);
    }

    bool paradaKeventOk = false;   // resultado do ultimo Cleanup
    void Cleanup();
    int GetProcessId(const wchar_t* name);
    void SetCurrentPid(int pid);
    uintptr_t GetModuleBase(int pid, const wchar_t* module_name);
    void SetBaseAddress(uintptr_t base);
    uintptr_t FindRealCr3(int pid, uintptr_t base_address, uintptr_t data_anchor);
    uintptr_t GetProcessCr3(int pid);
    bool ReadMemory(int pid, uintptr_t address, void* buffer, size_t size, const char* debugName = nullptr);
    bool BatchReadMemory(int pid, BatchReadEntry* entries, size_t count);
    void TestRandomMouseMoveLoop(std::atomic_bool& running);

    // -----------------------------------------------------------------------
    // InjectMouseMove — caminho de kernel, com `SendInput` de reserva.
    //
    // `forcarSendInput` pula o kernel e vai direto no SendInput. Existe para
    // DIAGNOSTICO, um caso so': a calibracao de sensibilidade do RCS roda o
    // mesmo pulso pelos dois caminhos e compara o quanto a camera andou.
    //
    // Isso separa duas causas que dao o MESMO sintoma ("o RCS nao compensa"):
    //
    //   kernel move a camera, SendInput tambem  -> injecao OK, o bug e' no calculo
    //   kernel NAO move, SendInput move         -> a injecao chega no Windows mas
    //                                              NAO no que o jogo le' (raw input)
    //   nenhum dos dois move                    -> nao e' injecao: e' estado do
    //                                              jogo (foco, menu, tela de mapa)
    //
    // Sem isso a gente fica adivinhando entre tres causas com um teste so'.
    //
    // -----------------------------------------------------------------------
    // SUBDIVISAO (1.3) — o movimento grande sai em RAJADA, nao em um pacote.
    //
    // Ver o bloco longo no .cpp. O resumo: um mouse real reporta o delta do
    // intervalo de pooling enquanto esta' sendo movido; um salto de 40 pixels
    // num unico pacote nao existe em hardware. A funcao quebra o delta em
    // pacotes de 3-9 px com fronteiras aleatorias e ~1 ms entre eles — a
    // RAJADA passa a ter tamanho e timing plausiveis.
    //
    // Movimento pequeno (ate' 8 px por eixo, que e' o caso do RCS e das
    // correcoes finas) continua saindo em UM pacote: nao ha' o que subdividir,
    // e nao se paga nem o laco nem o Sleep.
    //
    // O PONTO FINAL e' exato em qualquer caso — o que nao couber no teto de
    // pacotes sai num pacote final.
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // EnviarUmPacote — UM movimento: kernel se der, SendInput se nao.
    //
    // Public porque `InjectMouseMove` a usa em laco e porque o
    // `TestRandomMouseMoveLoop` emite pacotes avulsos. Quem quer movimento
    // subdividido chama `InjectMouseMove`; quem quer UM pacote exato — teste,
    // calibracao — chama esta.
    //
    // Devolve true quando a injecao foi pelo caminho de KERNEL (o chamador usa
    // isso para os contadores de diagnostico: kernel vs SendInput).
    // -----------------------------------------------------------------------
    bool EnviarUmPacote(int x, int y, bool forcarSendInput = false);
    void InjectMouseMove(int x, int y, bool forcarSendInput = false);

    // -----------------------------------------------------------------------
    // GetDriverVersion — QUEM esta' carregado, em numero.
    //
    // Substitui o antigo `SupportsMouseIoctl()`, que era um proxy ruim: ele
    // respondia "o IOCTL de mouse existe?", sim ou nao. Nao dizia QUAL versao
    // respondeu, e a cada IOCTL novo o teste precisava ser reescrito.
    //
    // Este devolve o numero direto. Zero significa "o driver nao conhece o
    // IOCTL de versao" — ou seja, build anterior a 1.1, ou um driver alheio no
    // mesmo nome de device.
    //
    // `Initialize()` chama isto. E o `InitializeFeaturesThread` no Main.cpp
    // AGE sobre o resultado: se a versao nao bate, ele fecha o device antigo e
    // mapeia esta build, em vez de so' avisar. Ver o comentario la'.
    // -----------------------------------------------------------------------
    ULONG GetDriverVersion();

    // 0 enquanto nao perguntou, ou se a pergunta falhou.
    ULONG driverVersion;

    // true quando o driver respondeu exatamente a versao que este build espera.
        // KEVENT tem versao propria (KMP_VERSION); cobrar KM_DRIVER_VERSION_ESPERADA
    // dele reprovaria sempre.
    bool DriverVersionOk() const;

    // -----------------------------------------------------------------------
    // SEED — o que faz o nome do device ser imprevisivel.    //
    // A 1.1 tinha "\\.\Ndu2" escrito no .cpp. Isso significava que o nome do
    // ponto de entrada do driver estava DENTRO do mtmacr0.exe, achavel com um
    // `strings`. Na 1.2 o user-mode sorteia um seed, passa no param2 do
    // MapDriver, e o nome sai de uma mistura desse seed.
    //
    // A MESMA mistura existe no km_safe_source.c (KmMixSeed/KmHex16). Se um
    // lado mudar sem o outro, o CreateFile simplesmente nao acha o device — e o
    // sintoma seria "driver nao carregou", que e' um lugar ruim pra procurar.
    // Por isso as duas implementacoes ficam lado a lado, comentadas.
    // -----------------------------------------------------------------------
    void SetSeed(uint64_t s);
    uint64_t GetSeed() const;

    // -----------------------------------------------------------------------
    // Unmap — DESCARGA ORDENADA: deactivate -> fecha handle -> unmap.
    //
    // Existe porque o kdmapper nao descarrega, e sem isto cada execucao do
    // cheat deixava mais um driver residente. O ciclo era sempre
    // "reboot e tenta de novo", e a colisao de nome de driver object era o
    // sintoma que aparecia primeiro.
    //
    // ORDEM (revisada na 2.1 — ver UnmapComInfo logo abaixo):
    //   1. GET_IMAGE_INFO    -> o driver DIZ onde esta' (base + tamanho)
    //   2. DEACTIVATE        -> para o atendimento e sai do namespace
    //   3. Cleanup()         -> fecha O NOSSO handle
    //   4. o MAPPER libera as paginas (intel_driver::MmFreeIndependentPages)
    //
    // O passo 4 NAO e' feito por esta classe: ela nao tem acesso ao mapper.
    // Quem chama o mapper e' o `DescarregarDriver()` do Main.cpp. Por isso o
    // `UnmapComInfo` abaixo e' o metodo que o caminho de saida usa.
    // -----------------------------------------------------------------------
    bool Unmap();

    // So' o passo 1+2: para o atendimento sem tentar liberar paginas.
    // E' o caminho seguro, e o que o Main usa quando so' precisa destravar o
    // nome do device pra mapear de novo.
    bool Deactivate();

    // -----------------------------------------------------------------------
    // UnmapComInfo — o desligamento na versao 2.1.
    //
    // ==================== O QUE MUDOU, E POR QUE ====================
    //
    // Na 2.0 o driver se liberava sozinho: SELFDESTRUCT + uma thread interna que
    // resolvia MmFreeIndependentPages por VARREDURA DE BYTES no ntoskrnl.
    //
    // O He apertou END e levou KERNEL_SECURITY_CHECK_FAILURE. A causa: se o
    // padrao casa no lugar errado, chama-se uma funcao QUALQUER do kernel com a
    // assinatura errada — e o kernel derruba a maquina sozinho. E' o mesmo erro
    // que ja' custou 0xFC e 0xD1 na resolucao do callback de mouse.
    //
    // A 2.1 inverte a responsabilidade:
    //
    //   1. GET_IMAGE_INFO (0x809) -> o driver reporta { base, size }
    //   2. DEACTIVATE     (0x807) -> sai do namespace (seguro)
    //   3. Cleanup                -> fecha o NOSSO handle
    //   4. intel_driver::MmFreeIndependentPages(base, size)   <- O MAPPER
    //
    // O passo 4 usa a API que JA' EXISTIA no mapper desde a 1.1, chamando pela
    // "call gadget" do driver doador. Nao ha' mais varredura de assinatura
    // dentro do driver.
    //
    // `*base` e `*size` recebem o que o driver reportou. ZERO em qualquer um dos
    // dois significa "nao ha' como liberar" (mapper sem passAllocationPtr, ou
    // driver antigo) — o device sai do namespace mesmo assim, e o codigo fica
    // residente e INERTE: sem device, sem symlink, sem responder.
    //
    // Devolve true se o DEACTIVATE foi aceito.
    // -----------------------------------------------------------------------
    bool UnmapComInfo(uint64_t* base, uint64_t* size);

    // -----------------------------------------------------------------------
    // HandleValido — "o device esta' aberto AGORA?".
    //
    // Existe porque o caminho de saida do cheat (END / falha do overlay) chama
    // DescarregarDriver() e nao pode contar com o fato de que houve
    // mapeamento: o modo map-only, o gate bloqueado e a falha de mapeamento
    // deixam o cheat de pe' com hDriver == INVALID_HANDLE_VALUE.
    //
    // Sem esta checagem, o Unmap num processo sem driver emitiria DEACTIVATE
    // num handle invalido e sujaria o log com um erro que nao existe.
    // -----------------------------------------------------------------------
    bool HandleValido() const;

private:
    DriverInterfaceKEvent* kevent = nullptr;
    HANDLE hDriver;
    int currentPid;
    uintptr_t baseAddress;
    uint64_t seed;
};

// ---------------------------------------------------------------------------
// DeviceNameAtual — "\\.\<hex>" derivado do seed. Definido no .cpp.
//
// Serve ao LOG e as ferramentas de teste: o nome do device deixou de ser um
// literal ("\\.\Ndu2"), entao esta e' a unica forma de saber em quem apontar
// numa sessao. A derivacao passa a ser a MESMA nos tres lugares (driver,
// user-mode e teste), o que tambem serve de conferencia: se o teste nao achar o
// device com o seed logado, a derivacao saiu do lugar em algum dos dois lados.
// ---------------------------------------------------------------------------
std::string DeviceNameAtual(uint64_t seed);
