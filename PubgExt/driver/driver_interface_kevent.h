#pragma once
// ===========================================================================
// driver_interface_kevent.h — backend de transporte KEVENT (shared memory)
//
// Espelha a interface pública do DriverInterfaceV3 para o `Memory` poder trocar
// de backend sem saber. A diferença é só o transporte: aqui não existe
// DeviceIoControl, existe uma seção compartilhada e dois eventos SEM NOME.
//
// ---------------------------------------------------------------------------
// ESCOPO DESTE BACKEND: ESP PURO
//
//   ReadMemory / BatchReadMemory  -> KMP_OP_READ   (implementado no driver)
//   GetModuleBase                 -> KMP_OP_GET_BASE
//   GetProcessId                  -> Toolhelp32 (user mode, documentado)
//   InjectMouseMove               -> KMP_OP_MOUSE (kernel, sem LLMHF_INJECTED)
//
// O driver de KEVENT NÃO tem injeção de mouse (KMP_OP_MOUSE ainda não existe).
// Quem escolher este backend fica sem aimbot e sem RCS de propósito — o ESP,
// o radar, o loot e os espectadores funcionam.
//
// ---------------------------------------------------------------------------
// A ORDEM DO Initialize() É O QUE MAIS IMPORTA
//
//   1. seção + 2 eventos SEM NOME
//   2. chave derivada do seed + 5 valores REG_QWORD
//   3. SÓ ENTÃO carrega o driver (callback do chamador)
//   4. espera Hdr.Ready == 1, com timeout
//
// Se o driver subir antes do passo 2, o DriverEntry morre no ZwOpenKey e o
// `StartService`/MapDriver devolve erro 2 — já aconteceu, e o sintoma parece
// "driver não carregou".
//
// ---------------------------------------------------------------------------
// A ARMADILHA DO LOTE
//
// O cheat pede lotes de até MAX_BATCH_ENTRIES = 128 leituras; o protocolo
// aceita KMP_MAX_OPS = 64. Sem fatiar, METADE DAS LEITURAS SOME EM SILÊNCIO e o
// ESP mostra metade dos jogadores. `BatchReadMemory` fatia em blocos de 64.
// ===========================================================================

#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstring>
#include <string>

#include "../../SharedProto/km_ipc_proto.h"
#include "../Mapper/km_boot_patch.h"
// kdmLog: diagnostico do backend que CHEGA ao logs.txt (o cheat registra o
// sink em kdmUtils::g_logSink). Sem ele, a falha do primeiro round trip era
// silenciosa e o unico sintoma era um "base=0x0" sem causa visivel.
#include "../Mapper/utils.hpp"

#define KEVENT_MAX_BATCH_ENTRIES 128

// ---------------------------------------------------------------------------
// Diagnostico de round trip / base (28/10/2026) — estado compartilhado.
//
// Este header entra em varias unidades de traducao; `inline` garante UMA
// copia por processo. Cada caso (pid x motivo) aparece UMA vez no log: quem
// chama GetModuleBase em loop (o InitProcess durante a manutencao) nao pode
// transformar o logs.txt num segundo loop de espera. O teto das linhas de
// Rodar e' o mesmo criterio: 3 linhas e depois a cada 128 falhas.
// ---------------------------------------------------------------------------
namespace kmbDiag {
struct Estado {
    volatile LONG timeouts   = 0;
    volatile LONG respostas  = 0;
    uint32_t      pids[8]    = { 0 };
    uint32_t      motivos[8] = { 0 };
    int           rot        = 0;
};
inline Estado& E() { static Estado e; return e; }
}

class DriverInterfaceKEvent {
public:
    // Mesmo layout do DriverInterfaceV3::BatchReadEntry, e com os dois nomes
    // (Address/address) pelo mesmo motivo: o código existente usa os dois.
    struct BatchReadEntry {
        union { uintptr_t Address; uintptr_t address; };
        union { size_t Size; size_t size; };
        union { void* Buffer; void* buffer; };
        BatchReadEntry() : Address(0), Size(0), Buffer(nullptr) {}
        BatchReadEntry(uintptr_t a, size_t s, void* b) : Address(a), Size(s), Buffer(b) {}
    };

    // Callback de carga do driver. O chamador decide COMO mapear (kdmapper com
    // os bytes embutidos, `StartService`, o que for). Devolve true se a carga
    // foi aceita. Recebe o seed porque o contrato do kdmapper passa o seed em
    // param2 — e porque o serviço lê o seed de Parameters (o adapter grava).
    typedef bool (*LoaderFn)(uint64_t seed);

    DriverInterfaceKEvent() = default;
    ~DriverInterfaceKEvent() { Cleanup(); }

    void SetSeed(uint64_t s) { seed = s; }
    uint64_t GetSeed() const { return seed; }

    // -----------------------------------------------------------------------
    // O CANAL E'' A SECAO, NAO O SERVICO.
    //
    // Era `sh != nullptr && hSvc != nullptr`. So'' que quem mapeia pelo kdmapper
    // NAO TEM SERVICO: o `hSvc` fica nulo e este teste dava false — ou seja,
    // `UsandoKevent()` mentia e TODO o caminho caia no IOCTL (que num backend
    // sem device falha em tudo: leitura, base e mouse).
    //
    // O que precisa existir e'' a view da secao; o servico e'' so'' um atalho
    // opcional para STOP/QUERY quando o driver foi carregado como servico.
    // -----------------------------------------------------------------------
    bool HandleValido() const { return sh != nullptr; }

    ULONG GetDriverVersion() {
        if (sh && sh->Hdr.Ready) driverVersion = sh->Hdr.Version;
        return driverVersion;
    }
    bool DriverVersionOk() const { return driverVersion == KMP_VERSION; }

    ULONG driverVersion = 0;

    // -----------------------------------------------------------------------
    // Initialize — faz a coisa toda. `svcName` é o nome do serviço que o
    // chamador criou/mapeou; `loader` faz a carga.
    // -----------------------------------------------------------------------
    bool Initialize(const char* svcName, LoaderFn loader)
    {
        if (svcName == nullptr || loader == nullptr) return false;
        strncpy_s(svcNome, svcName, sizeof(svcNome));

        if (seed == 0) seed = (uint64_t)GetTickCount64() ^ ((uint64_t)GetCurrentProcessId() << 32);

        const SIZE_T tam = (SIZE_T)KMP_SHARED_BYTES;

        // 1) seção e eventos SEM NOME
        hSec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  (DWORD)(tam >> 32), (DWORD)tam, nullptr);
        if (!hSec) return false;
        sh = (PKMP_SHARED)MapViewOfFile(hSec, FILE_MAP_ALL_ACCESS, 0, 0, tam);
        if (!sh) return false;
        memset(sh, 0, tam);

        hReq = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hRsp = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!hReq || !hRsp) return false;

        // 2) entrega o canal ao loader (sem registro)
        if (!EntregarCanalAoDriver()) return false;

        // 3) carga
        if (!loader(seed)) return false;

        // 4) espera o Ready, com teto
        for (int i = 0; i < 300; ++i) {
            MemoryBarrier();
            if (sh->Hdr.Ready == 1 && sh->Hdr.Magic == KMP_MAGIC) {
                driverVersion = sh->Hdr.Version;
                return DriverVersionOk();
            }
            Sleep(10);
        }
        return false;   // subiu mas não publicou Ready: falha de boot no driver
    }

    bool ReadMemory(int pid, uintptr_t address, void* buffer, size_t size,
                    const char* = nullptr)
    {
        if (!Pronto() || buffer == nullptr || size == 0) return false;
        if (size > sh->Hdr.DataBytes) return false;

        LimparReq();
        sh->Req.Count = 1;
        sh->Req.PayloadUsed = 0;
        sh->Req.Ops[0].Kind = KMP_OP_READ;
        sh->Req.Ops[0].Pid = (kmp_u32)pid;
        sh->Req.Ops[0].Address = (kmp_u64)address;
        sh->Req.Ops[0].Size = (kmp_u64)size;

        if (!Rodar()) return false;
        if (sh->Resp.Ops[0].Status != KMP_OK) return false;

        memcpy(buffer, Data() + (SIZE_T)sh->Resp.Ops[0].DataOff, size);
        return true;
    }

    // -----------------------------------------------------------------------
    // BatchReadMemory — fatia em blocos de KMP_MAX_OPS. Ver a nota do topo.
    // -----------------------------------------------------------------------
    bool BatchReadMemory(int pid, BatchReadEntry* entries, size_t count)
    {
        if (!Pronto() || entries == nullptr || count == 0) return false;

        bool tudo = true;
        for (size_t off = 0; off < count; off += KMP_MAX_OPS) {
            size_t n = count - off;
            if (n > (size_t)KMP_MAX_OPS) n = KMP_MAX_OPS;

            LimparReq();
            sh->Req.Count = (kmp_u32)n;
            sh->Req.PayloadUsed = 0;

            uint64_t cursor = 0;
            for (size_t i = 0; i < n; ++i) {
                sh->Req.Ops[i].Kind = KMP_OP_READ;
                sh->Req.Ops[i].Pid = (kmp_u32)pid;
                sh->Req.Ops[i].Address = (kmp_u64)entries[off + i].Address;
                sh->Req.Ops[i].Size = (kmp_u64)entries[off + i].Size;
                cursor += entries[off + i].Size;
            }
            // O driver limita a saída a DataBytes; um lote maior que a seção
            // seria truncado silenciosamente, então recusa antes de mandar.
            if (cursor > sh->Hdr.DataBytes ||
                cursor + (uint64_t)n * 8 > sh->Hdr.DataBytes) { tudo = false; break; }

            if (!Rodar()) { tudo = false; break; }

            for (size_t i = 0; i < n; ++i) {
                if (sh->Resp.Ops[i].Status != KMP_OK) { tudo = false; continue; }
                memcpy(entries[off + i].Buffer,
                       Data() + (SIZE_T)sh->Resp.Ops[i].DataOff,
                       entries[off + i].Size);
            }
        }
        return tudo;
    }

    int GetProcessId(const wchar_t* name)
    {
        if (name == nullptr) return 0;

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        int pid = 0;

        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, name) == 0) { pid = (int)pe.th32ProcessID; break; }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return pid;
    }

    // Só o módulo PRINCIPAL — é o que o TslGame precisa. O driver de KEVENT não
    // enumera módulos, então um pedido de outro nome devolve 0 em vez de lixo.
    uintptr_t GetModuleBase(int pid, const wchar_t* module_name)
    {
        if (!Pronto()) return 0;
        if (module_name && _wcsicmp(module_name, L"TslGame.exe") != 0) {
            // ainda permite: devolve a base da imagem, que é o que existe
        }

        LimparReq();
        sh->Req.Count = 1;
        sh->Req.PayloadUsed = 0;
        sh->Req.Ops[0].Kind = KMP_OP_GET_BASE;
        sh->Req.Ops[0].Pid = (kmp_u32)pid;

        if (!Rodar()) return 0;   // Rodar ja' loga o motivo, com teto

        const kmp_u32   st     = sh->Resp.Ops[0].Status;
        const uint64_t  via    = sh->Resp.Ops[0].Out1;
        const uintptr_t base   = (uintptr_t)sh->Resp.Ops[0].Out0;

        // Um registro por (pid, motivo): 0 normal/secao, 1 op rejeitada,
        // 2 base zero nos dois caminhos, 3 recuperada pela PEB.
        const uint32_t motivo =
            (st != KMP_OK) ? 1u : ((base == 0) ? 2u : (via ? 3u : 0u));
        kmbDiag::Estado& d = kmbDiag::E();
        int iV = -1;
        for (int i = 0; i < 8; ++i)
            if (d.pids[i] == (uint32_t)pid) { iV = i; break; }
        if (iV < 0) iV = d.rot;
        d.rot = (d.rot + 1) & 7;
        if (!(d.pids[iV] == (uint32_t)pid && d.motivos[iV] == motivo)) {
            d.pids[iV] = (uint32_t)pid;
            d.motivos[iV] = motivo;
            switch (motivo) {
            case 1:
                kdmLog(L"[kmb] pid=" << pid << L" GET_BASE rejeitada pelo driver (status=0x"
                       << std::hex << st << L")\n");
                break;
            case 2:
                kdmLog(L"[kmb] pid=" << pid
                       << L" base=0 — NAO consta na secao do kernel NEM na PEB"
                          L" (processo em estado de manutencao/zumbi?)\n");
                break;
            case 3:
                kdmLog(L"[kmb] pid=" << pid << L" base=0x" << std::hex << base
                       << L" recuperada pela PEB (a secao do kernel estava zerada)\n");
                break;
            default:
                break;
            }
        }
        return base;
    }

    // -----------------------------------------------------------------------
    // MOUSE (KMP_OP_MOUSE) — dx em Out0, dy em Out1.
    //
    // Este era um NO-OP: o transporte nao tinha injecao, e por isso "full
    // KEVENT" custaria o aimbot e o RCS. Agora o driver de KEVENT inclui o
    // `km_mouse.h` (o mesmo resolvedor do Mouclass do driver IOCTL), entao o
    // caminho de kernel funciona nos dois backends.
    //
    // Devolve FALSE quando o op falha (driver antigo, sem a op). O chamador
    // cai no SendInput — e o log diz que caiu.
    // -----------------------------------------------------------------------
    // EXPOSURE — pede ao driver o retrato do que um AC de kernel veria.
    //
    // Devolve 16 numeros crus (ver KMP_OP_EXPOSE no protocolo). Quem formata
    // e' o chamador: o driver nao tem printf, e o log daqui ja' e' texto.
    // -----------------------------------------------------------------------
    // `porque` (opcional): 0 ok, 1 canal nao pronto, 2 round trip falhou,
    // 3 op rejeitada pelo driver. 28/10: antes toda falha virava "driver sem
    // KMP_OP_EXPOSE?" — quando, no caso da manutencao, era o round trip que
    // morria e o driver estava ate' mais novo do que o log dizia.
    bool ExposeDiagnostico(uint64_t saida[16], uint64_t tamanhoImagem = 0,
                           uint32_t* porque = nullptr)
    {
        if (porque) *porque = 0;
        if (!Pronto() || saida == nullptr) { if (porque) *porque = 1; return false; }

        LimparReq();
        sh->Req.Count = 1;
        sh->Req.PayloadUsed = 0;
        sh->Req.Ops[0].Kind = KMP_OP_EXPOSE;
        sh->Req.Ops[0].Size = tamanhoImagem;   // headers do driver foram destruidos no map

        if (!Rodar()) { if (porque) *porque = 2; return false; }
        if (sh->Resp.Ops[0].Status != KMP_OK) { if (porque) *porque = 3; return false; }

        memcpy(saida, Data(), sizeof(uint64_t) * 16);
        return true;
    }

    // -----------------------------------------------------------------------
    bool EnviarUmPacote(int dx, int dy, bool = false)
    {
        if (!Pronto()) return false;
        if (dx == 0 && dy == 0) return false;

        LimparReq();
        sh->Req.Count = 1;
        sh->Req.PayloadUsed = 0;
        sh->Req.Ops[0].Kind = KMP_OP_MOUSE;
        sh->Req.Ops[0].Pid  = (kmp_u32)GetCurrentProcessId();
        sh->Req.Ops[0].Out0 = (kmp_i64)(LONG)dx;
        sh->Req.Ops[0].Out1 = (kmp_i64)(LONG)dy;

        if (!Rodar()) return false;
        return sh->Resp.Ops[0].Status == KMP_OK;
    }

    void InjectMouseMove(int x, int y, bool forcarSendInput = false)
    {
        (void)forcarSendInput;
        EnviarUmPacote(x, y);
    }

    bool Deactivate()
    {
        bool ok = true;
        if (hSvc) {
            SERVICE_STATUS ss{};
            ok = ControlService(hSvc, SERVICE_CONTROL_STOP, &ss) != FALSE;
        }
        Cleanup();
        return ok;
    }

    void Cleanup()
    {
        // -----------------------------------------------------------------
        // PARADA DO SERVIDOR ANTES DE FECHAR OS HANDLES.
        //
        // O driver roda uma THREAD DE KERNEL esperando no ReqEvent. Num
        // mapeamento manual nao existe unload que a derrube: se o cliente
        // apenas fechasse os handles, a thread continuaria viva para sempre
        // (uma por sessao), com os eventos e a secao ainda referenciados.
        //
        // O sinal e' `Req.Magic = KMP_MAGIC_STOP` + SetEvent. O laco do
        // driver acorda, ve' que nao e' lote e sai; ele publica
        // `Hdr.Ativo = 0` antes de terminar, e e' isso que a espera aqui
        // confirma.
        // -----------------------------------------------------------------
        if (sh != nullptr && sh->Hdr.Ready == 1) {
            MemoryBarrier();
            sh->Req.Magic = KMP_MAGIC_STOP;
            MemoryBarrier();
            if (hReq) SetEvent(hReq);
            for (int i = 0; i < 200; ++i) {          // ate 2 s
                MemoryBarrier();
                if (sh->Hdr.Ativo == 0) { paradaConfirmada = true; break; }
                Sleep(10);
            }

            // ------------------------------------------------------------------
            // GRAÇA DE 100 ms ANTES DE LIBERAR AS PAGINAS.
            //
            // O servidor agora roda em WORK ITEM. Ele publica `Ativo = 0` e
            // AINDA executa o proprio epilogo (retorno ao worker do sistema).
            // Liberar as paginas nesse intervalo e' o bugcheck de sempre — a
            // rotina retornando para uma imagem que nao existe mais.
            //
            // 100 ms e' folga de sobra: o retorno leva microssegundos.
            // ------------------------------------------------------------------
            if (paradaConfirmada) Sleep(100);
        }

        if (hSvc) { CloseServiceHandle(hSvc); hSvc = nullptr; }
        if (hScm) { CloseServiceHandle(hScm); hScm = nullptr; }
        if (sh)   { UnmapViewOfFile(sh); sh = nullptr; }
        if (hSec) { CloseHandle(hSec); hSec = nullptr; }
        if (hReq) { CloseHandle(hReq); hReq = nullptr; }
        if (hRsp) { CloseHandle(hRsp); hRsp = nullptr; }
        // (nao ha' mais chave de registro: o canal foi entregue no slot de boot)
    }

    // Base da imagem mapeada, publicada pelo driver (para o mapper liberar
    // as paginas no fim da sessao). 0 = o driver nao informou.
    uint64_t GetImageBase() const { return (sh != nullptr) ? (uint64_t)sh->Hdr.ImageBase : 0; }

    // -----------------------------------------------------------------------
    // KEYREAD DE KERNEL (v4) — estado ALT/SHIFT publicado no cabecalho.
    //
    // `KeyreadKernelOk`: o driver conectou o kbdclass nesta carga (flag
    // KMP_KM_KROK). Driver v3 nem tem o campo: a versao ja' separa, mas a flag
    // cobre o caso "v4 com o install do kbdclass que falhou".
    //
    // `KeyMaskAtual`: os bits de tecla (KMP_KM_*). Latencia maxima ~100 ms
    // (um ciclo do work item). Quem consome: o HotkeyHook, kernel-first com
    // fallback para o hook LL (ver la').
    // -----------------------------------------------------------------------
    bool KeyreadKernelOk() const
    {
        if (!sh || !sh->Hdr.Ready) return false;
        return (sh->Hdr.KeyMask & KMP_KM_KROK) != 0;
    }
    uint32_t KeyMaskAtual() const
    {
        if (!sh || !sh->Hdr.Ready) return 0;
        return sh->Hdr.KeyMask & KMP_KM_MASK;
    }

    // A parada do servidor foi CONFIRMADA pelo cabecalho?
    bool ParadaConfirmada() const { return paradaConfirmada; }
    uint32_t LastGlobalStatus = 0;

    // ------------------------------------------------------------------
    // Telemetria do kernel (v5): contadores que o proprio driver mantem
    // no Hdr. So' tem sentido ler enquanto a sessao esta' viva (Ativo=1);
    // fora disso devolve zero.
    //
    //   badSig    -> lotes rejeitados por assinatura FNV (alguem tocou na
    //                shared memory que nao era a gente — o driver se protegeu
    //                e o numero sobe). Nao deve subir em sessao sadia.
    //   throttled -> lotes que estouraram KMP_MAX_OPS_PER_SEC (protecao de
    //                rate; sobe quando a taxa de leitura ficou agressiva).
    //   watchdog  -> disparos do watchdog de 60 s (lote parado demais).
    // ------------------------------------------------------------------
    void EstatisticasKernel(uint32_t* badSig, uint32_t* throttled, uint32_t* watchdog)
    {
        if (badSig) *badSig = 0;
        if (throttled) *throttled = 0;
        if (watchdog) *watchdog = 0;
        if (sh == nullptr) return;
        if (!sh->Hdr.Ativo) return;
        if (badSig)    *badSig    = sh->Hdr.BadSigBatches;
        if (throttled) *throttled = sh->Hdr.ThrottledBatches;
        if (watchdog)  *watchdog  = sh->Hdr.WatchdogFires;
    }

private:
    HANDLE      hSec = nullptr, hReq = nullptr, hRsp = nullptr;
    SC_HANDLE   hSvc = nullptr, hScm = nullptr;
    PKMP_SHARED sh = nullptr;
    uint32_t    seq = 0;
    uint64_t    seed = 0;
    bool        chaveCriada = false;
    // TRUE quando o servidor do kernel confirmou a parada (`Hdr.Ativo == 0`).
    // O unload USA isto: liberar as paginas sem confirmacao e' liberar
    // enquanto a thread ainda executa o codigo — o crash e' imediato.
    bool        paradaConfirmada = false;
    wchar_t     keyRel[128] = { 0 };
    char        svcNome[128] = { 0 };

    bool Pronto() const { return sh != nullptr; }
    uint8_t* Data() const { return (uint8_t*)sh + (SIZE_T)sh->Hdr.DataOffset; }

    void LimparReq()
    {
        sh->Req.Magic = KMP_MAGIC;
        sh->Req.Version = KMP_VERSION;
        sh->Req.Count = 0;
        sh->Req.PayloadUsed = 0;
    }

    // ---------------------------------------------------------------
    // Um round trip. Publica `Sig` e `Seq` por ÚLTIMO e confere o `AckSeq`
    // na volta: sem as duas barreiras, o user mode pode ler a resposta
    // antes do driver terminar de escrever.
    //
    // A assinatura (v5) e' calculada DEPOIS que todos os campos do lote
    // estao escritos (ops + PayloadUsed + o NOVO Seq) e o driver recalcula
    // a mesma FNV com a chave derivada da Signature da carga — rejeitando
    // qualquer byte que nao seja nosso (ver KmpReqSig no proto).
    //
    // 28/10/2026 — BUG DO BADB EM 100% DOS LOTES: a FNV dobra o campo
    // `r->Seq`, mas ela era calculada ANTES de publicar o novo Seq. O
    // lote ia assinado com N-1 e o kernel conferia com N:
    // `GlobalStatus=0xc000badb` em toda chamada, base=0x0 sem causa.
    // Corrigindo a ordem: escreve o Seq, calcula a FNV (agora cobrindo o
    // valor certo) e RE-publica o Seq por ultimo — ele continua sendo o
    // ultimo campo de sinal do lote. Escrever o Seq antes da FNV e' seguro:
    // o kernel so' le o Req ao acordar pelo evento (SetEvent abaixo), e um
    // acorde com Seq == ultimo processado e tratado como espúrio; o lote
    // anterior ja' terminou quando chegamos aqui (esperamos o Rsp dele).
    //
    // `LastGlobalStatus` fica publico para quem quer distinguir
    // "tempo esgotado" de "assinatura rejeitada" no log.
    // ---------------------------------------------------------------
    bool Rodar()
    {
        ++seq;
        sh->Req.Seq = seq;   // valor correto dentro da FNV (ver comentario)
        sh->Req.Sig = KmpReqSig(&sh->Req, KmpSessKey(sh->Hdr.Signature));
        MemoryBarrier();
        sh->Req.Seq = seq;   // republicado por ultimo: e' o campo de sinal
        MemoryBarrier();
        SetEvent(hReq);

        if (WaitForSingleObject(hRsp, 2000) != WAIT_OBJECT_0) {
            // 28/10/2026: a primeira falha real de round trip era o unico
            // indicio de que o servidor do kernel tinha parado — antes, toda
            // operacao seguinte devolvia 0 em silencio (era o "base=0x0" sem
            // causa). Agora o motivo chega ao logs.txt, com teto para o loop
            // de manutencao nao engordar o arquivo.
            volatile LONG& fT = kmbDiag::E().timeouts;
            long nT = InterlockedIncrement(&fT);
            if (nT <= 3 || (nT % 128) == 0)
                kdmLog(L"[kmb] rodar: TIMEOUT — Rsp sem sinal em 2000 ms (seq=" << seq
                       << L", tentativas=" << nT << L")\n");
            return false;
        }
        MemoryBarrier();
        LastGlobalStatus = sh->Resp.GlobalStatus;
        bool ok = (sh->Resp.AckSeq == seq) && (sh->Resp.GlobalStatus == KMP_OK);
        if (!ok) {
            // Respondeu, mas nao foi para nos: ack velho (servidor atrasado
            // ou lote perdido) ou status global diferente de KMP_OK
            // (assinatura rejeitada, watchdog, throttle).
            volatile LONG& fR = kmbDiag::E().respostas;
            long nR = InterlockedIncrement(&fR);
            if (nR <= 3 || (nR % 128) == 0)
                kdmLog(L"[kmb] rodar: resposta invalida (ack=" << sh->Resp.AckSeq
                       << L", seq=" << seq << L", GlobalStatus=0x"
                       << std::hex << sh->Resp.GlobalStatus << L")\n");
        }
        return ok;
    }

    static uint64_t Mix(uint64_t s, uint32_t kind)
    {
        uint64_t k = s ^ (KMP_MIX * (uint64_t)(kind + 1));
        k ^= k >> 33; k *= 0xFF51AFD7ED558CCDULL;
        k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ULL;
        k ^= k >> 33;
        return k;
    }

    static void HexDig(wchar_t* out, uint64_t v, int d)
    {
        static const wchar_t h[] = L"0123456789abcdef";
        for (int i = d - 1; i >= 0; --i) { out[i] = h[v & 0xF]; v >>= 4; }
    }

    // -----------------------------------------------------------------------
    // ENTREGA DO CANAL AO DRIVER — SEM REGISTRO.
    //
    // Ate 21/10/2026 esta funcao criava
    // `HKLM\SOFTWARE\Classes\CLSID\<32 hex>` + 5 REG_QWORD e deixava o driver
    // ler de la'. Funcionava, e era uma impressao digital: qualquer driver com
    // CmRegisterCallback enxerga "chave CLSID aleatoria, 5 QWORD, apagada em
    // ~1 ms" — padrao igual para todo usuario do build, ou seja, assinatura de
    // FAMILIA (o que da' ban em massa).
    //
    // Agora os numeros vao para a estrutura `BootPatch()`, e o LOADER grava
    // esses numeros dentro da imagem, no campo `g_BootSlot`, antes de mapear.
    // O driver le' a si mesmo: nao existe objeto de rendezvous para observar.
    //
    // Ganho colateral: nao precisa de elevacao para gravar em HKLM.
    // -----------------------------------------------------------------------
    bool EntregarCanalAoDriver()
    {
        if (hSec == nullptr || hReq == nullptr || hRsp == nullptr) return false;
        if (svcNome[0] == 0) return false;   // o loader precisa saber QUEM mapear

        KmpBootPatch& p = BootPatch();
        p.ativo = true;
        p.pid   = (uint32_t)GetCurrentProcessId();
        p.sig   = Mix(seed, KMP_MIX_SIG);
        p.hSec  = (uint64_t)(uintptr_t)hSec;
        p.hReq  = (uint64_t)(uintptr_t)hReq;
        p.hRsp  = (uint64_t)(uintptr_t)hRsp;
        return true;
    }
};
