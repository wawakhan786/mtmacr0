// ===========================================================================
// km_ipc_driver.c — protótipo de transporte: OBJETOS SEM NOME + KEVENT
//
// Carregado pelo kdmapper com o SEED em param2 (mesma convenção do driver de
// produção). Não cria device, não cria symbolic link, não registra
// MajorFunction, não trata IRP. Não existe NADA dele no namespace de objetos
// do Windows além da chave de registro transitória, que ele apaga.
//
// Ver RELATORIO_ARQUITETURA.md, seção "Rendezvous", para a ordem completa.
//
// ---------------------------------------------------------------------------
// ESCOPO, DITO SEM ENFEITE
//
// FAZ: transporte em lote de 4 operacoes inocuas (NOP / ECHO / FILL / SUM)
//      sobre o payload da propria secao, MAIS as duas operacoes que o cheat
//      usa de verdade: KMP_OP_READ (MmCopyVirtualMemory) e KMP_OP_GET_BASE
//      (PsGetProcessSectionBaseAddress).
//
//      NOTA: este bloco dizia "NAO FAZ: ler ou escrever memoria". Ficou
//      desatualizado quando READ/GET_BASE entraram — e comentario que mente
//      e' pior que comentario ausente: quem le conclui que o transporte e'
//      inerte e nao investiga a falha no lugar certo.
//
// ---------------------------------------------------------------------------
// PRÉ-REQUISITOS DE CARGA (operação, não código)
//
//   * VM de teste; NUNCA a máquina principal;
//   * test signing ligado (bcdedit /set testsigning on) ou o kdmapper existente;
//   * o processo de teste precisa estar ELEVADO: o boot duplica handles de
//     outro processo, e isso exige PROCESS_DUP_HANDLE.
// ===========================================================================

#include <ntifs.h>
#include <ntimage.h>   // IMAGE_* para o filtro de secao executavel (km_mouse.h)
#include <intrin.h>   // __rdtsc: ciclos de CPU, sem import nenhum

#include "km_ipc_proto.h"
// ---------------------------------------------------------------------------
// MOUSE OPCIONAL NO DRIVER (KMP_WITH_MOUSE).
//
// O bloco do Mouclass traz imports novos (MmIsAddressValid, IoDriverObjectType,
// PsLoadedModuleList) e ~600 linhas de resolucao de ponteiro. Depois de um
// bugcheck 0x1E com salto para endereco NAO-CANONICO, a primeira pergunta e:
// "o transporte carrega?" — e essa pergunta tem que ser respondida SEM o mouse
// no caminho, senao a resposta vem contaminada.
//
// 0 = driver sem mouse (KMP_OP_MOUSE responde NOT_IMPLEMENTED; o cliente cai
//     no SendInput e todo o resto funciona).
// 1 = com mouse.
// ---------------------------------------------------------------------------
#ifndef KMP_WITH_MOUSE
#define KMP_WITH_MOUSE 1

// Esconder o StartAddress da thread (ETHREAD->StartAddress). Escreve em
// memoria de outro objeto do kernel: fica DESLIGADO ate ser provado. Ver o
// comentario em KmpServerThread.
#ifndef KMP_HIDE_THREAD_START
#define KMP_HIDE_THREAD_START 0

// Servidor em WORK ITEM (1) ou em thread propria (0).
//
// 1 = a rotina roda numa thread de worker do sistema: o StartAddress dela e'
//     do ntoskrnl, e a nossa imagem deixa de aparecer em ETHREAD nenhum.
// 0 = `PsCreateSystemThread`, que grava o entry point em ETHREAD->StartAddress
//     (enumeravel, e o unico sinal que o [expose] ainda acusava).
#ifndef KMP_SERVER_WORKITEM
#define KMP_SERVER_WORKITEM 1
#endif
#endif
#endif
#if KMP_WITH_MOUSE
#include "km_mouse.h"   // KmInjectMouse: aimbot/RCS sem passar pelo driver IOCTL
#endif
#include "km_keyread.h" // KrInstall/KrPublish: estado ALT/SHIFT via kbdclass

#define KMP_TAG 'pmiK'

// `NtCurrentProcess` nem sempre vem como macro no ntifs do WDK 28000.
#ifndef NtCurrentProcess
#define NtCurrentProcess() ((HANDLE)-1)
#endif

// Tipos de objeto usados no ObReferenceObjectByHandle. O ntifs não declara
// todos; são símbolos exportados pelo ntoskrnl.
extern POBJECT_TYPE* ExEventObjectType;
extern POBJECT_TYPE* MmSectionObjectType;

// O ntifs do WDK 28000 nao declara as duas. Declaracao identica a do driver de
// producao (km_safe_source.c), que ja' usa as mesmas duas.
NTKERNELAPI NTSTATUS NTAPI MmCopyVirtualMemory(
    PEPROCESS SourceProcess, PVOID SourceAddress,
    PEPROCESS TargetProcess, PVOID TargetAddress,
    SIZE_T BufferSize, KPROCESSOR_MODE PreviousMode,
    PSIZE_T ReturnSize);

NTKERNELAPI PVOID NTAPI PsGetProcessSectionBaseAddress(PEPROCESS Process);

// PEB do processo (fallback de KMP_OP_GET_BASE). Exportada pelo ntoskrnl.
NTKERNELAPI PVOID NTAPI PsGetProcessPeb(PEPROCESS Process);

// Enumeracao de threads e start address. O ntifs do WDK 28000 nao declara
// as duas; sao exportadas pelo ntoskrnl.
NTKERNELAPI PETHREAD NTAPI PsGetNextProcessThread(PEPROCESS Process, PETHREAD Thread);
NTKERNELAPI NTSTATUS NTAPI PsGetThreadStartAddress(PETHREAD Thread, PVOID* StartAddress);

// ---------------------------------------------------------------------------
// ESTADO — espelha o RegCommState do driver de referência.
//
// Os campos são os mesmos de propósito: quando este protótipo for comparado
// com o do amigo, a diferença tem de ser de LÓGICA, não de nomenclatura.
// ---------------------------------------------------------------------------
typedef struct _KMP_STATE {
    HANDLE   KeyHandle;        // chave do rendezvous (temporária)
    PKEVENT  ReqEvent;         // evento de pedido  (SEM NOME, referenciado)
    PKEVENT  RspEvent;         // evento de resposta (SEM NOME, referenciado)
    HANDLE   ReqEventHandle;   // handle no kernel (OBJ_KERNEL_HANDLE)
    HANDLE   RspEventHandle;
    HANDLE   ThreadHandle;     // thread do servidor
    KEVENT   ThreadDone;       // sinalizado ANTES de PsTerminateSystemThread
    ULONG    LifecycleState;   // KMP_LC_*
    ULONG    SequenceNumber;   // último Seq processado

    HANDLE   ClientProcess;    // dono dos handles, só durante o boot
    PVOID    SectionObject;    // objeto seção, referenciado
    PVOID    ViewBase;         // view mapeada no kernel
    SIZE_T   ViewSize;
    PKMP_SHARED Shared;
    ULONG    TargetPid;
    ULONG64  Signature;
    volatile LONG Stop;
} KMP_STATE, *PKMP_STATE;

static KMP_STATE g_St;

// ---------------------------------------------------------------------------
// WATCHDOG E TETO DE TAXA (v5) — ver KMP_WDOG_MS / KMP_MAX_OPS_PER_SEC no proto.
//
// `g_LoteQpc` e' o KeQueryInterruptTime (100 ns) do ultimo lote processado.
// Zero = nenhuma atividade ainda, e e' isso que impede o watchdog de atirar
// em quem so' subiu. O bucket de taxa reinicia a cada segundo cheio.
// ---------------------------------------------------------------------------
static volatile LONGLONG g_LoteQpc = 0;
static volatile LONGLONG g_RateQpc = 0;
static volatile ULONG    g_RateOps = 0;

static void KmpLimpezaWorkItem(PVOID contexto);
static void KmpWatchdog(PKMP_STATE st);
static void KmpRateContar(PKMP_SHARED sh, ULONG ops);
static void KmpShutdown(void);

// Passo em que o boot parou. Escrito na chave do rendezvous quando algo falha:
// sem isso, TODA falha vira o mesmo STATUS_INVALID_HANDLE e nao da' para saber
// se foi o evento, a secao ou o processo.
static ULONG g_Step = 0;

// Base e tamanho da imagem mapeada, guardados no boot para o diagnostico
// de exposicao (KMP_OP_EXPOSE) não depender de re-leitura do cabecalho.
static ULONG64 g_ImageBaseKm = 0;
static ULONG64 g_ImageSizeKm = 0;

// ---------------------------------------------------------------------------
// SLOT DE BOOT — o cliente grava os handles AQUI, nos bytes do driver,
// antes de mapear. Inicializado com o magic para ser localizavel no arquivo.
// `volatile` porque o compilador nao ve ninguem escrever nele em runtime.
// ---------------------------------------------------------------------------
static volatile KMP_BOOT_SLOT g_BootSlot = { KMP_BOOT_MAGIC, 0, 0, 0, 0, 0 };

// Prototipos do que o diagnostico de exposicao usa e esta' definido mais
// abaixo: a thread do servidor e o estado da troca de StartAddress.
static VOID KmpServerThread(PVOID contexto);
static volatile LONG g_StartEscondido;
static ULONG64 g_StartFalso;
static PUNICODE_STRING g_ServicePath = NULL;

// Prototipo: `KmpInit` usa esta funcao para decidir se o param2 e' seed ou
// caminho de servico, e a definicao esta' mais abaixo (ordem do arquivo).
static BOOLEAN KmpLooksLikeRegistryPath(PUNICODE_STRING p);


static WCHAR          g_KeyPath[192];
static UNICODE_STRING g_KeyName;

// ===========================================================================
// DERIVAÇÃO DE NOMES — nenhum literal de nome de sessão no binário
// ===========================================================================
#define KMP_MIX 0x9E3779B97F4A7C15ULL

static ULONG64 KmpMix(ULONG64 seed, ULONG kind)
{
    ULONG64 k = seed ^ (KMP_MIX * (ULONG64)(kind + 1));
    k ^= k >> 33; k *= 0xFF51AFD7ED558CCDULL;
    k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ULL;
    k ^= k >> 33;
    return k;
}

static void KmpHex(WCHAR* out, ULONG64 v, int digits)
{
    static const WCHAR h[] = L"0123456789abcdef";
    int i;
    for (i = digits - 1; i >= 0; --i) { out[i] = h[v & 0xF]; v >>= 4; }
}

// A RAIZ é o único literal de caminho, e é um caminho de SISTEMA
// (`...\Classes\CLSID\`), não um marcador: ali já existem milhares de GUIDs, e
// o nosso é derivado do seed — indistinguível para quem não tem o seed.




// ---------------------------------------------------------------------------
// DUPLICAÇÃO DE HANDLE
//
// O handle foi criado no processo do cliente. O DriverEntry roda em contexto de
// SISTEMA (é assim que o kdmapper chama), então o valor numérico não vale aqui:
// só vale DENTRO do processo que o criou. Duplicar traz o objeto para o nosso
// lado — e é por isso que a chave carrega o Pid.
// ---------------------------------------------------------------------------
static NTSTATUS KmpDupFromClient(HANDLE client, HANDLE src, ACCESS_MASK mask, PHANDLE out)
{
    return ZwDuplicateObject(client, src, NtCurrentProcess(), out,
                             mask, 0, OBJ_KERNEL_HANDLE);
}

// ===========================================================================
// FILTRO DE FAIXA — a linha de frente
//
// Nenhum acesso a Data acontece sem `off + size <= limite`, com a soma em
// ULONGLONG para não estourar. Foi um overflow de pool com tamanho controlado
// pelo chamador que derrubou a máquina seis vezes no projeto principal.
// ===========================================================================
static BOOLEAN KmpRangeOk(ULONG64 off, ULONG64 size, ULONG64 limit)
{
    return (((ULONGLONG)off + (ULONGLONG)size) <= (ULONGLONG)limit);
}

static ULONG64 KmpAlign8(ULONG64 v) { return (v + 7ULL) & ~7ULL; }

// ===========================================================================
// PROCESSAMENTO DO LOTE
// ===========================================================================
// ===========================================================================
// ---------------------------------------------------------------------------
// As duas APIs NAO estao na lib de importacao do WDK. Em vez de linkar por
// nome (que falha) ou declarar um import fantasma, resolvemos por
// `MmGetSystemRoutineAddress` na primeira chamada do diagnostico.
//
// Efeito colateral bom: o `.sys` nao carrega import apontando para
// enumeracao de threads — que e' justamente o tipo de import que chama
// atencao numa analise estatica.
// ---------------------------------------------------------------------------
typedef PETHREAD (NTAPI *KMPFN_GETNEXT)(PEPROCESS, PETHREAD);
typedef NTSTATUS (NTAPI *KMPFN_GETSTART)(PETHREAD, PVOID*);

static KMPFN_GETNEXT  g_pGetNextProcessThread = NULL;
static KMPFN_GETSTART g_pGetThreadStartAddress = NULL;

static void KmpResolverAPIsDeThread(void)
{
    UNICODE_STRING nome;
    if (g_pGetNextProcessThread == NULL) {
        RtlInitUnicodeString(&nome, L"PsGetNextProcessThread");
        g_pGetNextProcessThread = (KMPFN_GETNEXT)MmGetSystemRoutineAddress(&nome);
    }
    if (g_pGetThreadStartAddress == NULL) {
        RtlInitUnicodeString(&nome, L"PsGetThreadStartAddress");
        g_pGetThreadStartAddress = (KMPFN_GETSTART)MmGetSystemRoutineAddress(&nome);
    }
}

// KMP_OP_EXPOSE — DIAGNOSTICO DE EXPOSICAO
//
// A pergunta: "o que um anti-cheat de kernel veria ao olhar para nos?".
// O que conta nao e' o que nos sabemos do driver, e' o que sobra no sistema
// depois que ele esta' rodando.
//
// O QUE ELE MEDE
//
//   1. IMAGEM FORA DE MODULO
//      `PsLoadedModuleList` e' a lista oficial de imagens de kernel. Uma imagem
//      mapeada a mao NAO esta' nela — fato observavel por qualquer driver, e o
//      primeiro item que um AC verifica.
//
//   2. THREAD DE SISTEMA COM START ADDRESS FORA DE MODULO
//      `PsCreateSystemThread` grava o entry point em ETHREAD->StartAddress. Uma
//      thread de sistema cujo start nao pertence a nenhum modulo carregado e'
//      assinatura de injecao manual — o vetor mais usado contra drivers assim.
//
//   3. QUANTAS THREADS JA' ESTAO NESSA CONDICAO
//      Sem esse numero, "fora de modulo" parece unico. Com ele, da' para
//      comparar ruido de sistema com a nossa assinatura.
//
// SAIDA: NUMEROS, nao texto.
//
// O primeiro rascunho formatava uma string gigante em kernel (concatenacao
// manual de digitos). Numero cru e' menos codigo, menos chance de erro e o
// cliente formata com `%llX`, que ja' existe. A `Data` recebe um vetor de
// kmp_u64:
//
//   [0]  nossa imagem: base
//   [1]  nossa imagem: tamanho (SizeOfImage publicado)
//   [2]  nossa imagem aparece em PsLoadedModuleList? (1/0)
//   [3]  start da NOSSA thread
//   [4]  o start da nossa thread esta' dentro de algum modulo? (1/0)
//   [5]  total de threads do processo de sistema
//   [6]  threads cujo start esta' fora de qualquer modulo
//   [7]  quantos exemplos vieram em [8..]
//   [8..15] ate' 8 starts suspeitos
// ===========================================================================

// Nome do modulo que contem `addr` (opcional). TRUE se achou.
static BOOLEAN KmpModuloDeAddress(ULONG_PTR addr, char* nome, int tamNome)
{
    PLIST_ENTRY atual;

    if (nome && tamNome > 0) nome[0] = 0;
    if (PsLoadedModuleList == NULL) return FALSE;
    if (!KmPtrOk(PsLoadedModuleList)) return FALSE;

    for (atual = PsLoadedModuleList->Flink;
         KmPtrOk(atual) && atual != PsLoadedModuleList;
         atual = atual->Flink) {

        KM_LDR_ENTRY* e = (KM_LDR_ENTRY*)atual;
        ULONG_PTR base, fim;

        if (!KmPtrOk(e)) return FALSE;             // lista corrompida: para
        if (e->DllBase == NULL || e->SizeOfImage == 0) continue;
        if (!KmPtrOk(e->DllBase)) continue;

        base = (ULONG_PTR)e->DllBase;
        fim  = base + (ULONG_PTR)e->SizeOfImage;
        if (addr < base || addr >= fim) continue;

        if (nome && tamNome > 0) {
            int i = 0;
            USHORT n = (USHORT)(e->BaseDllName.Length / sizeof(WCHAR));
            if (n > (USHORT)(tamNome - 1)) n = (USHORT)(tamNome - 1);
            for (; i < (int)n; ++i) {
                WCHAR c = e->BaseDllName.Buffer[i];
                nome[i] = (c < 128) ? (char)c : '?';
            }
            nome[i] = 0;
        }
        return TRUE;
    }
    return FALSE;
}

// Preenche o vetor de saida. `imageBase`/`imageSize` vem do estado do driver.
static void KmpDiagnosticoExposicao(PKMP_SHARED sh, ULONG64 imageBase, ULONG64 imageSize,
                                     ULONG64 tamInformadoPeloCliente)
{
    kmp_u64* saida = (kmp_u64*)((kmp_u8*)sh + (SIZE_T)sh->Hdr.DataOffset);
    ULONG64  nossoStart = (ULONG64)(ULONG_PTR)KmpServerThread;
    ULONG64  startAgora = 0;
    PVOID*   campos;
    ULONG    i;

    // O driver tem os headers destruidos pelo mapper (destroyHeader), entao
    // ele NAO consegue ler o proprio SizeOfImage. Quem sabe o tamanho e' o
    // cliente, que tem os bytes originais — por isso ele vem no `op->Size`.
    if (imageSize == 0) imageSize = tamInformadoPeloCliente;

    saida[0] = imageBase;
    saida[1] = imageSize;
    saida[2] = KmpModuloDeAddress((ULONG_PTR)imageBase, NULL, 0) ? 1 : 0;

    // ---------------------------------------------------------------------
    // O START ADDRESS, LIDO DO PROPRIO ETHREAD (sem API de enumeracao).
    //
    // `PsGetThreadStartAddress` e `PsGetNextProcessThread` NAO resolveram por
    // `MmGetSystemRoutineAddress` nesta build — o diagnostico anterior imprimiu
    // "threads 0" e "indisponivel", que e' pior que nao medir: parece medido.
    // Agora a leitura e' do nosso proprio ETHREAD, pelo mesmo criterio da
    // funcao que esconde.
    // ---------------------------------------------------------------------
    campos = (PVOID*)PsGetCurrentThread();
    if (KmPtrOk(campos)) {
        for (i = 0; i < (0x600 / sizeof(PVOID)); ++i) {
            if (!KmPtrOk(&campos[i])) break;
            if ((ULONG64)(ULONG_PTR)campos[i] == nossoStart) { startAgora = nossoStart; break; }
        }
    }

    saida[3] = startAgora ? nossoStart : g_StartFalso;
    saida[4] = startAgora ? 0 : 1;                      // 1 = nao aponta mais para nos
    saida[5] = (kmp_u64)g_StartEscondido;               // a troca foi feita?
    saida[6] = (kmp_u64)KmpModuloDeAddress((ULONG_PTR)saida[3], NULL, 0);  // 1 = dentro de modulo
    saida[7] = 0;                                        // reservado

    // Census de threads do sistema: NAO disponivel sem as APIs de enumeracao.
    for (i = 8; i < 16; ++i) saida[i] = 0;
}

static ULONG64 KmpProcessBatch(PKMP_SHARED sh)
{
    PKMP_REQ  req = &sh->Req;
    PKMP_RESP rsp = &sh->Resp;
    kmp_u8*   data = (kmp_u8*)sh + (SIZE_T)sh->Hdr.DataOffset;
    ULONG64   cursor;
    ULONG     i;
    ULONG     falhas = 0;

    /* ---------------------------------------------------------------------
     * PROCESSO DO LOTE — UMA UNICA RESOLUCAO PARA O LOTE INTEIRO.
     *
     * Antes cada KMP_OP_READ fazia o proprio PsLookupProcessByProcessId +
     * ObDereferenceObject. Um lote de 64 ops pagava 64 buscas na tabela de
     * processos (hash + lock), e o ESP manda ~1.500 ops por tick. Todos os
     * reads de uma sessao apontam para o MESMO pid (o jogo), entao resolver
     * uma vez no primeiro read, manter a referencia durante o lote e soltar
     * no fim nao muda semantica: e' a mesma chamada, a mesma referencia,
     * apenas compartilhada. Medir antes de escrever: o que cai e' o custo
     * da busca por op, nao o da copia.
     * --------------------------------------------------------------------- */
    PEPROCESS batchProc = NULL;
    ULONG     batchPid  = 0;

    cursor = KmpAlign8(req->PayloadUsed);

    rsp->Magic    = KMP_MAGIC;
    rsp->Version  = KMP_VERSION;
    rsp->Count    = 0;
    rsp->DataUsed = 0;

    if (req->Magic != KMP_MAGIC || req->Version != KMP_VERSION) {
        rsp->GlobalStatus = KMP_ERR_BAD_VERSION;
        goto done;
    }
    if (req->Count > KMP_MAX_OPS) {
        rsp->GlobalStatus = KMP_ERR_BAD_PARAMETER;
        goto done;
    }
    if (req->PayloadUsed > sh->Hdr.DataBytes) {
        rsp->GlobalStatus = KMP_ERR_BAD_LENGTH;
        goto done;
    }

    // ---------------------------------------------------------------------
    // ASSINATURA DO LOTE (v5) — confere ANTES de executar qualquer op.
    //
    // O driver recompute o que o user-mode prometeu (KmpReqSig, mesma funcao
    // do header). Nao confere: o lote morre aqui, NENHUMA op roda, e o
    // contador Hdr.BadSigBatches sobe para o cliente ver. O AckSeq continua
    // sendo publicado pelo chamador, entao o cliente nao fica pendurado no
    // evento — ele recebe um GlobalStatus explicando por que nada aconteceu.
    //
    // Custo medido na pratica: ~10 FNV por op a 144 Hz e' inexistente perto
    // do MmCopyVirtualMemory que segue. Nao e' otimizacao, e' prova.
    // ---------------------------------------------------------------------
    if (KmpReqSig(req, KmpSessKey(g_St.Signature)) != req->Sig) {
        rsp->GlobalStatus = KMP_ERR_BAD_SIGNATURE;
        InterlockedIncrement((LONG*)&(sh->Hdr.BadSigBatches));
        goto done;
    }

    for (i = 0; i < req->Count; ++i) {
        PKMP_OP op = &req->Ops[i];
        KMP_OP  o  = req->Ops[i];       // cópia: a saída não corrompe a entrada

        o.Status  = KMP_OK;
        o.DataOff = 0;
        o.Out0    = 0;
        o.Out1    = 0;

        switch (op->Kind) {

        case KMP_OP_NOP:
            break;

        case KMP_OP_ECHO:
            if (!KmpRangeOk(op->PayloadOff, op->Size, req->PayloadUsed) ||
                !KmpRangeOk(cursor, op->Size, sh->Hdr.DataBytes)) {
                o.Status = KMP_ERR_BAD_PARAMETER; ++falhas; break;
            }
            RtlCopyMemory(data + cursor, data + (SIZE_T)op->PayloadOff, (SIZE_T)op->Size);
            o.DataOff = cursor;
            cursor = KmpAlign8(cursor + op->Size);
            break;

        case KMP_OP_FILL:
            if (!KmpRangeOk(cursor, op->Size, sh->Hdr.DataBytes)) {
                o.Status = KMP_ERR_BAD_PARAMETER; ++falhas; break;
            }
            RtlFillMemory(data + cursor, (SIZE_T)op->Size, (UCHAR)(op->Out0 & 0xFF));
            o.DataOff = cursor;
            cursor = KmpAlign8(cursor + op->Size);
            break;

        case KMP_OP_SUM: {
            ULONG64 s = 0, k;
            if (!KmpRangeOk(op->PayloadOff, op->Size, req->PayloadUsed)) {
                o.Status = KMP_ERR_BAD_PARAMETER; ++falhas; break;
            }
            for (k = 0; k < op->Size; ++k)
                s += data[(SIZE_T)op->PayloadOff + (SIZE_T)k];
            o.Out0 = s;
            break;
        }

        // LEITURA DE MEMORIA — o que o cheat realmente usa.
        //
        // O alvo e' o processo de `op->Pid`, e a escrita e' no NOSSO buffer, que
        // e' a view da secao. MmCopyVirtualMemory resolve o endereco no contexto
        // do processo alvo sozinho, entao nao precisa anexar.
        case KMP_OP_READ: {
            PEPROCESS src = NULL;
            SIZE_T    copiado = 0;
            NTSTATUS  st2 = STATUS_INVALID_PARAMETER;

            if (op->Size == 0 || !KmpRangeOk(cursor, op->Size, sh->Hdr.DataBytes)) {
                o.Status = KMP_ERR_BAD_PARAMETER; ++falhas; break;
            }

            // -----------------------------------------------------------------
            // O ENDERECO DE ORIGEM E' VALIDADO ANTES DE QUALQUER COISA — E ISSO
            // CUSTOU UM BUGCHECK 0x50 NO MEIO DE UMA PARTIDA (22/10/2026).
            //
            // O dump mostrou a cadeia exata:
            //
            //     MmCopyVirtualMemory            <- nosso, caso KMP_OP_READ
            //       MiProbeAndLockPages
            //         MiFaultInProbeAddress      <- probe de endereco invalido
            //           KeBugCheckEx  (0x50, P1 = 0x6575727465736C71)
            //
            // P1 em ASCII = "qlsetrue": o cliente passou como ENDERECO um pedaco
            // de TEXTO que ele leu de um campo do jogo. Sem `__try`, o probe de
            // um endereco nao-canonico NAO vira excecao tratavel — vira bugcheck.
            //
            // Duas defesas, nesta ordem:
            //   1. numero: so' aceita VA canonical de MODO USUARIO (a leitura e'
            //      do processo do jogo). Isso descarta texto, ponteiro de kernel e
            //      NULL de uma vez, sem custo.
            //   2. `__try`: se ainda assim o probe falhar (pagina nao residente e
            //      endereco que passa no teste), o lote devolve erro em vez de
            //      derrubar a maquina.
            //
            // A consequencia funcional e' a certa: uma leitura ruim vira
            // `KMP_ERR_BAD_PARAMETER`, o cliente descarta aquela entidade e o ESP
            // segue desenhando o resto.
            // -----------------------------------------------------------------
            {
                const ULONG64 alvo = (ULONG64)op->Address;
                const ULONG64 tam  = (ULONG64)op->Size;

                // VA de modo usuario em x64: canonicos ate' 0x00007FFFFFFFFFFF.
                // Abaixo de 0x10000 e' pagina nula / nao mapeavel por desenho.
                if (alvo < 0x10000ULL || alvo > 0x00007FFFFFFFFFFFULL) {
                    o.Status = KMP_ERR_BAD_PARAMETER; ++falhas; break;
                }
                // Soma com estouro = endereco atravessando o fim do espaco.
                if (alvo + tam < alvo || (alvo + tam) > 0x0000800000000000ULL) {
                    o.Status = KMP_ERR_BAD_PARAMETER; ++falhas; break;
                }
            }

            /* Processo do lote: reusa a referencia ja' resolvida se o pid e'
             * o mesmo do op anterior (e' sempre — a sessao le' so' o jogo).
             * Pid diferente solta a antiga e re-resolve (caminho raro). */
            if (!batchProc || batchPid != (ULONG)op->Pid) {
                if (batchProc) ObDereferenceObject(batchProc);
                batchProc = NULL;
                if (!NT_SUCCESS(PsLookupProcessByProcessId(
                        (HANDLE)(ULONG_PTR)op->Pid, &batchProc))) {
                    o.Status = KMP_ERR_BAD_PARAMETER; ++falhas; break;
                }
                batchPid = (ULONG)op->Pid;
            }
            src = batchProc;

            __try {
                st2 = MmCopyVirtualMemory(src, (PVOID)(ULONG_PTR)op->Address,
                                          PsGetCurrentProcess(), data + cursor,
                                          (SIZE_T)op->Size, KernelMode, &copiado);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                st2 = STATUS_INVALID_PARAMETER;
                copiado = 0;
            }
            /* A referencia e' unica para o lote inteiro: solta so' no fim,
             * antes do `done:` (ver la' embaixo). */

            if (!NT_SUCCESS(st2) || copiado != (SIZE_T)op->Size) {
                o.Status = KMP_ERR_BAD_PARAMETER; ++falhas; break;
            }
            o.DataOff = cursor;
            cursor = KmpAlign8(cursor + op->Size);
            break;
        }

#if KMP_WITH_MOUSE
        case KMP_OP_MOUSE: {
            const int dx = (int)(LONG)op->Out0;
            const int dy = (int)(LONG)op->Out1;
            const NTSTATUS stm = KmInjectMouse(dx, dy);
            o.Status = NT_SUCCESS(stm) ? KMP_OK : KMP_ERR_NOT_IMPLEMENTED;
            break;
        }
#else
        case KMP_OP_MOUSE: {
            // Sem o bloco do Mouclass neste build: o cliente recebe
            // NOT_IMPLEMENTED e cai no SendInput. Falha ALTA, nunca silenciosa.
            o.Status = KMP_ERR_NOT_IMPLEMENTED;
            ++falhas;
            break;
        }
#endif
        // -----------------------------------------------------------------
        // EXPOSE — o que um AC de kernel veria. So' leitura de estruturas do
        // sistema; nao altera nada. Ver o bloco KmpDiagnosticoExposicao.
        // -----------------------------------------------------------------
        case KMP_OP_EXPOSE: {
            if (!KmpRangeOk(0, sizeof(kmp_u64) * 16, sh->Hdr.DataBytes)) {
                o.Status = KMP_ERR_BAD_PARAMETER; ++falhas; break;
            }
            KmpDiagnosticoExposicao(sh, g_ImageBaseKm, g_ImageSizeKm, (ULONG64)op->Size);
            o.DataOff = 0;
            o.Size    = sizeof(kmp_u64) * 16;
            break;
        }

        case KMP_OP_GET_BASE: {
            PEPROCESS src = NULL;
            ULONG_PTR base = 0;
            ULONG     via = 0;
            if (!NT_SUCCESS(PsLookupProcessByProcessId(
                    (HANDLE)(ULONG_PTR)op->Pid, &src))) {
                o.Status = KMP_ERR_BAD_PARAMETER; ++falhas;
                o.Out1 = 0xFF;   // processo nao existe mais
                break;
            }
            /* Caminho 0: a secao — o metodo classico. Em processos presos em
             * estado "estranho" (tela de manutencao do PUBG, zumbis de
             * atualizacao interrompida) ele devolve ZERO mesmo com a imagem
             * carregada. 28/10/2026: o TslGame em manutencao deu exatamente
             * isso, nos DOIS pids da maquina. */
            base = (ULONG_PTR)PsGetProcessSectionBaseAddress(src);
            if (!base) {
                /* Caminho 1: PEB->ImageBaseAddress (offset 0x10 no PEB x64).
                 * E' o MESMO endereco que uma ferramenta user mode leria de
                 * "image base". O EPROCESS esta' referenciado e o processo
                 * vivo: a referencia direta e' legitima, mas fica dentro de
                 * __try para um fault nunca escapar da work item. */
                __try {
                    void* peb = PsGetProcessPeb(src);
                    if (peb) {
                        ULONG_PTR pebBase = *(ULONG_PTR*)((UCHAR*)peb + 0x10);
                        if (pebBase) { base = pebBase; via = 1; }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    base = 0;
                }
            }
            o.Out0 = (ULONG64)base;
            o.Out1 = via;   // 0 = secao, 1 = fallback PEB, 0xFF = proc ausente
            ObDereferenceObject(src);
            break;
        }

        default:
            o.Status = KMP_ERR_NOT_IMPLEMENTED; ++falhas; break;
        }

        rsp->Ops[i] = o;
        rsp->Count  = i + 1;
    }

    rsp->DataUsed     = cursor;
    rsp->GlobalStatus = KMP_OK;

    /* Fim do lote: a referencia de EPROCESS ficou presa em `batchProc`
     * durante todos os reads. Soltar AQUI (e nao por op) e' o que fecha o
     * par com o PsLookup — o caminho antecipado do `done:` chega aqui com
     * batchProc == NULL, que a guarda descarta. */
    if (batchProc) ObDereferenceObject(batchProc);

done:
    rsp->OpsFailed += falhas;
    return 0;
}

// ===========================================================================
// THREAD DO SERVIDOR
//
// Evento de SINCRONIZAÇÃO (`FALSE` no KeInitializeEvent): cada SetEvent acorda
// exatamente uma espera e não acumula sinal — o que impediria processar o mesmo
// lote duas vezes.
// ===========================================================================
// ---------------------------------------------------------------------------
// KmpEsconderStartAddress — TIRA O START DA THREAD DA NOSSA IMAGEM.
//
// O `PsCreateSystemThread` grava o entry point em ETHREAD->StartAddress. Como a
// nossa imagem NAO esta em PsLoadedModuleList, uma thread de sistema cujo start
// aponta para ela e' o sinal classico de mapeamento manual — e o primeiro que
// uma varredura de AC de kernel procura.
//
// COMO ESCONDE: procura o ponteiro da funcao DENTRO do proprio ETHREAD (o
// deslocamento de KTHREAD->StartAddress muda entre builds do Windows, e chutar
// offset e' como chutar tudo o mais). Se o valor aparecer EXATAMENTE UMA VEZ
// nos primeiros 0x600 bytes, troca por um endereco legitimo do ntoskrnl.
//
// POR QUE EXIGIR UNICIDADE: se o ponteiro aparecer 2+ vezes (contexto salvo,
// campo duplicado), reescrever todos poderia corromper o estado da thread. Com
// ambiguidade, NAO MEXE e o diagnostico reporta que nao mexeu — falha visivel,
// nunca correcao no escuro.
// ---------------------------------------------------------------------------
static BOOLEAN KmpEsconderStartAddress(PVOID nossoStart)
{
    PETHREAD      self;
    PVOID*        campos;
    PVOID         falso = NULL;
    UNICODE_STRING nome;
    ULONG         i, achados = 0, pos = 0;

    if (nossoStart == NULL) return FALSE;
    self = PsGetCurrentThread();
    if (!KmPtrOk(self)) return FALSE;

    campos = (PVOID*)self;
    for (i = 0; i < (0x600 / sizeof(PVOID)); ++i) {
        if (!KmPtrOk(&campos[i])) break;
        if (campos[i] == nossoStart) { ++achados; pos = i; }
    }
    if (achados != 1) return FALSE;

    RtlInitUnicodeString(&nome, L"KeWaitForSingleObject");
    falso = MmGetSystemRoutineAddress(&nome);
    if (falso == NULL || falso == nossoStart) return FALSE;

    InterlockedExchangePointer((PVOID volatile*)&campos[pos], falso);
    return TRUE;
}

// Estado para o diagnostico: se a troca aconteceu, e para onde.
static volatile LONG g_StartEscondido = 0;
static ULONG64 g_StartFalso = 0;

static VOID KmpServerThread(PVOID contexto)
{
    PKMP_STATE st = (PKMP_STATE)contexto;
    PKMP_SHARED sh = st->Shared;
    LARGE_INTEGER cemMs;

    cemMs.QuadPart = -10000LL * 100;   // 100 ms, relativo
    // ANTES de qualquer trabalho: tira o StartAddress da nossa imagem.
    // Se falhar (ambiguidade), o diagnostico reporta — nao ha' correcao cega.
    // ------------------------------------------------------------------
    // DESLIGADO POR PADRAO (KMP_HIDE_THREAD_START = 0).
    //
    // Primeira versao ligava sempre, e o teste de 20/10 21:37 deu
    // ATTEMPTED_WRITE_TO_READONLY_MEMORY (0xBE) — bugcheck que SO'
    // acontece em ESCRITA. Esta funcao e' a unica escrita nova no
    // driver; ate' estar provada, ela nao roda sem pedido explicito.
    //
    // Para testar: compilar com /DKMP_HIDE_THREAD_START=1.
    // ------------------------------------------------------------------
#if KMP_HIDE_THREAD_START
    if (KmpEsconderStartAddress((PVOID)KmpServerThread)) {
        InterlockedExchange(&g_StartEscondido, 1);
        {
            UNICODE_STRING n2;
            RtlInitUnicodeString(&n2, L"KeWaitForSingleObject");
            g_StartFalso = (ULONG64)(ULONG_PTR)MmGetSystemRoutineAddress(&n2);
        }
    }
#endif

    InterlockedExchange((volatile LONG*)&st->LifecycleState, KMP_LC_WAITING);

    for (;;) {
        NTSTATUS stWait;

        if (st->Stop) break;

        InterlockedExchange((volatile LONG*)&st->LifecycleState, KMP_LC_WAITING);

        stWait = KeWaitForSingleObject(st->ReqEvent, Executive, KernelMode, FALSE, &cemMs);
        if (st->Stop) break;
        if (stWait != STATUS_SUCCESS) {          // timeout: reavalia Stop
            KmpWatchdog(st);                     // v5: cliente morto? limpeza
            if (st->Stop) break;
            continue;
        }

        KeMemoryBarrier();

        // Evento espúrio, ou lote já processado: o Seq não avançou.
        // -----------------------------------------------------------------
        // PEDIDO DE PARADA (KMP_MAGIC_STOP, ver o protocolo).
        //
        // Vem ANTES da checagem de Seq porque a parada nao e' um lote: o
        // cliente escreve o Magic de parada e sinaliza, sem sequencia. Sem
        // esta saida, num driver mapeado a mao (onde NAO existe unload) o
        // servidor ficaria esperando para sempre — uma thread de kernel por
        // sessao, com os eventos e a secao ainda referenciados.
        // -----------------------------------------------------------------
        if (sh->Req.Magic == KMP_MAGIC_STOP) break;

        // Evento espurio, ou lote ja' processado: o Seq nao avancou.
        if (sh->Req.Seq == st->SequenceNumber) continue;

        InterlockedExchange((volatile LONG*)&st->LifecycleState, KMP_LC_BUSY);

        {
            // `__rdtsc` e nao KeQueryPerformanceCounter/KeQueryInterruptTime:
            // os dois primeiros nao resolvem o import com
            // /NODEFAULTLIB + ntoskrnl.lib neste WDK, e o campo se chama
            // CyclesLast — ciclos de CPU e' literalmente o que se quer medir.
            // Intrinseco: nao gera import.
            const ULONG64 t0 = __rdtsc();
            KmpProcessBatch(sh);
            const ULONG64 t1 = __rdtsc();
            sh->Resp.CyclesLast = (kmp_i64)(t1 - t0);
        }

        sh->Resp.OpsProcessed += sh->Resp.Count;

        // PUBLICAÇÃO: barreira ANTES do AckSeq, e o Ack é o ÚLTIMO campo
        // escrito. O user-mode só pode confiar no Resp depois de ver o Ack
        // igual ao Seq que ele mandou.
        KeMemoryBarrier();
        sh->Resp.AckSeq = sh->Req.Seq;
        KeMemoryBarrier();

        st->SequenceNumber = sh->Req.Seq;

        g_LoteQpc = KeQueryInterruptTime();        // v5: relogio do watchdog
        KmpRateContar(sh, sh->Resp.Count);         // v5: teto de taxa

        KeSetEvent(st->RspEvent, IO_NO_INCREMENT, FALSE);
    }

    // ANTES do DONE: o user mode espera `Hdr.Ativo == 0` para saber que
    // o servidor morreu e que nenhum handle daqui e' mais usado.
    KeMemoryBarrier();
    sh->Hdr.Ativo = 0;
    KeMemoryBarrier();
    InterlockedExchange((volatile LONG*)&st->LifecycleState, KMP_LC_DONE);

    // Sinaliza ANTES de terminar: o shutdown espera aqui, e depois deste ponto
    // nenhum handle daqui é tocado.
    KeSetEvent(&st->ThreadDone, IO_NO_INCREMENT, FALSE);

    PsTerminateSystemThread(STATUS_SUCCESS);
}
// ===========================================================================
// SERVIDOR EM WORK ITEM — a troca que apaga o StartAddress
//
// ==================== POR QUE ISTO EXISTE ====================
//
// O `PsCreateSystemThread` grava o entry point em ETHREAD->StartAddress. Como a
// nossa imagem NAO esta' em PsLoadedModuleList, a thread fica sendo uma thread
// de sistema cujo start aponta para memoria sem dono — e isso e' enumeravel:
//
//     PsGetNextProcessThread(SystemProcess, NULL) -> ETHREAD -> StartAddress
//
// Foi o que o diagnostico `[expose]` mediu, e a unica resposta NAO-VAZIA que o
// Windows dava sobre o nosso driver. Todo o resto (modulo, device, servico,
// driver object) respondia "nao existe".
//
// `ExQueueWorkItem` roda a rotina numa thread de worker do SISTEMA. O
// `StartAddress` dessa thread e' do ntoskrnl — legitimo por construcao, e igual
// ao de qualquer driver que use work item. Nao ha' nada para esconder: a thread
// nao e' nossa.
//
// O PRECO, dito sem enfeite:
//
//   1. work item nao tem join. A parada vira flag + `Ativo = 0`.
//   2. o `KeWaitForSingleObject` de 100 ms bloqueia um worker do sistema por
//      ciclo. E' aceitavel (DelayedWorkQueue, prioridade baixa), mas nao e'
//      elegante.
//   3. depois de `Ativo = 0` a rotina ainda executa o proprio epilogo. O cliente
//      espera 100 ms antes de liberar as paginas — sem isso, liberar enquanto o
//      worker retorna e' o bugcheck de sempre.
//
// Duas WORK_ITEM estaticas, alternadas: re-enfileirar a MESMA estrutura que
// acabou de rodar funciona, mas e' pratica desaconselhada. Com duas, o item que
// esta' na fila nunca e' o que esta' executando.
// ===========================================================================
#define KMP_WORKITEM_ESPERA_MS 100

static WORK_QUEUE_ITEM g_Wi[2];
static volatile LONG g_WiProximo = 0;

static VOID KmpServidorWorkItem(PVOID contexto);

// ---------------------------------------------------------------------------
// LIMPEZA AUTONOMA (v5) — o watchdog manda parar o servidor; ESTE item e' quem
// executa o KmpShutdown num worker DIFERENTE, porque o shutdown espera no
// `ThreadDone` que so' o proprio servidor sinaliza: chamado por ele mesmo, seria
// um deadlock garantido em 2 s de teto virando 2 s de nada.
//
// Antes da limpeza, espera o `Ativo = 0` do epilogo do servidor (o mesmo sinal
// que o user-mode confere): unmap com o servidor ainda executando e' o
// bugcheck classico de "codigo voltando para uma imagem ja' liberada".
// ---------------------------------------------------------------------------
static WORK_QUEUE_ITEM g_WiLimpeza;

static VOID KmpLimpezaWorkItem(PVOID contexto)
{
    PKMP_STATE st = (PKMP_STATE)contexto;
    LARGE_INTEGER dezMs;
    int i;

    dezMs.QuadPart = -10000LL * 10;
    for (i = 0; i < 200; ++i) {
        if (st == NULL || st->Shared == NULL || st->Shared->Hdr.Ativo == 0) break;
        KeDelayExecutionThread(Executive, FALSE, &dezMs);
    }
    KmpShutdown();
}

static void KmpLimpezaAgendar(PKMP_STATE st)
{
    ExInitializeWorkItem(&g_WiLimpeza, KmpLimpezaWorkItem, st);
    ExQueueWorkItem(&g_WiLimpeza, DelayedWorkQueue);
}

// WATCHDOG (v5) — roda no caminho de TIMEOUT do loop do servidor.
// Condicao: houve atividade pelo menos uma vez (`g_LoteQpc != 0`) e o ultimo
// lote tem mais de KMP_WDOG_MS. Quem decide a parada e' so' o PRIMEIRO caller
// (CAS no Stop); os demais voltam sem duplicar o item de limpeza.
static void KmpWatchdog(PKMP_STATE st)
{
    LONGLONG agora;

    if (st->Stop) return;
    agora = KeQueryInterruptTime();
    if (g_LoteQpc == 0) return;
    if ((ULONG64)(agora - g_LoteQpc) <= ((ULONG64)KMP_WDOG_MS * 10000ULL)) return;

    if (InterlockedCompareExchange(&st->Stop, 1, 0) != 0) return;
    if (st->Shared != NULL)
        InterlockedIncrement((LONG*)&(st->Shared->Hdr.WatchdogFires));
    KmpLimpezaAgendar(st);
}

// TETO DE TAXA (v5) — telemetry pura, nunca descarta. Bucket de 1 s.
static void KmpRateContar(PKMP_SHARED sh, ULONG ops)
{
    LONGLONG agora = KeQueryInterruptTime();

    if ((ULONG64)(agora - g_RateQpc) >= 10000000ULL) {   // 1 s em 100 ns
        g_RateQpc = agora;
        g_RateOps = 0;
    }
    g_RateOps += ops;
    if (sh != NULL && g_RateOps > KMP_MAX_OPS_PER_SEC)
        InterlockedIncrement((LONG*)&(sh->Hdr.ThrottledBatches));
}

static VOID KmpAgendarServidor(PKMP_STATE st)
{
    const LONG p = InterlockedIncrement(&g_WiProximo) & 1;
    ExInitializeWorkItem((PWORK_QUEUE_ITEM)&g_Wi[p], KmpServidorWorkItem, st);
    ExQueueWorkItem((PWORK_QUEUE_ITEM)&g_Wi[p], DelayedWorkQueue);
}

static VOID KmpServidorWorkItem(PVOID contexto)
{
    PKMP_STATE  st = (PKMP_STATE)contexto;
    PKMP_SHARED sh;
    LARGE_INTEGER cemMs;
    NTSTATUS    stWait;

    if (st == NULL) return;
    sh = st->Shared;

#if KMP_WITH_KEYREAD
    // ------------------------------------------------------------------
    // PUBLICACAO DO KEYMASK A CADA WAKE (timeout ou evento).
    //
    // O callback do kbdclass so' toca a imagem (DISPATCH-safe); EScrever na
    // view e' papel deste work item (PASSIVE_LEVEL). Custo: uma copia de u32
    // por ciclo de 100 ms — e e' isso que limita a latencia da tecla a uma
    // rotacao do work item, no pior caso.
    // ------------------------------------------------------------------
    sh->Hdr.KeyMask = KrPublish();
#endif

    cemMs.QuadPart = -10000LL * KMP_WORKITEM_ESPERA_MS;

    if (st->Stop) goto parar;

    InterlockedExchange((volatile LONG*)&st->LifecycleState, KMP_LC_WAITING);

    // PASSIVE_LEVEL: e' aqui que um work item roda, e o `KeWaitForSingleObject`
    // com timeout e' valido neste nivel.
    stWait = KeWaitForSingleObject(st->ReqEvent, Executive, KernelMode, FALSE, &cemMs);

    if (st->Stop) goto parar;
    if (stWait != STATUS_SUCCESS) {
        KmpWatchdog(st);                   // v5: cliente morto? limpeza
        if (st->Stop) goto parar;
        goto reagendar;                    // timeout: reavalia
    }

    KeMemoryBarrier();

    if (sh->Req.Magic == KMP_MAGIC_STOP) goto parar;   // parada pedida pelo cliente
    if (sh->Req.Seq == st->SequenceNumber) goto reagendar;   // evento espurio

    {
        ULONG64 t0, t1;

        InterlockedExchange((volatile LONG*)&st->LifecycleState, KMP_LC_BUSY);

        t0 = __rdtsc();
        KmpProcessBatch(sh);
        t1 = __rdtsc();
        sh->Resp.CyclesLast = (kmp_i64)(t1 - t0);

        sh->Resp.OpsProcessed += sh->Resp.Count;

        KeMemoryBarrier();
        sh->Resp.AckSeq = sh->Req.Seq;        // publicacao: AckSeq e' o ULTIMO
        KeMemoryBarrier();

        st->SequenceNumber = sh->Req.Seq;

        g_LoteQpc = KeQueryInterruptTime();        // v5: relogio do watchdog
        KmpRateContar(sh, sh->Resp.Count);         // v5: teto de taxa

        KeSetEvent(st->RspEvent, IO_NO_INCREMENT, FALSE);
    }

reagendar:
    KmpAgendarServidor(st);
    return;

parar:
    // A ORDEM IMPORTA: `Ativo = 0` e' o que o user-mode espera para saber que
    // nenhum handle daqui e' mais usado.
    KeMemoryBarrier();
    sh->Hdr.Ativo = 0;
    KeMemoryBarrier();
    InterlockedExchange((volatile LONG*)&st->LifecycleState, KMP_LC_DONE);
    KeSetEvent(&st->ThreadDone, IO_NO_INCREMENT, FALSE);
    // Sem `PsTerminateSystemThread`: o worker e' do sistema, e devolve-lo e' o
    // ponto. O cliente espera 100 ms antes de liberar as paginas.
}


// ===========================================================================
// SHUTDOWN — a ordem é a mesma que já custou tela azul
// ===========================================================================
static void KmpShutdown(void)
{
    if (InterlockedExchange(&g_St.Stop, 1) == 0) {
        InterlockedExchange((volatile LONG*)&g_St.LifecycleState, KMP_LC_STOPPING);

        if (g_St.ReqEvent) KeSetEvent(g_St.ReqEvent, IO_NO_INCREMENT, FALSE);

        // -------------------------------------------------------------------
        // ESPERA COM TETO — `NULL` aqui era deadlock esperando acontecer.
        //
        // Com `NULL` (infinito), se o servidor NUNCA tivesse sido agendado
        // (falha antes do `KmpAgendarServidor`, no caminho `fail:` do KmpInit),
        // este `KeWaitForSingleObject` esperaria para sempre por um evento que
        // ninguem setaria — travando o driver DENTRO do DriverEntry.
        //
        // 2 s e' o mesmo teto que o cliente usa. Se estourar, seguimos: a
        // alternativa (esperar eterno) e' pior que nao esperar.
        // -------------------------------------------------------------------
        if (g_St.ThreadHandle || KMP_SERVER_WORKITEM) {
            LARGE_INTEGER teto;
            teto.QuadPart = -10000LL * 2000;      // 2 s
            KeWaitForSingleObject(&g_St.ThreadDone, Executive, KernelMode, FALSE, &teto);
        }
    }
    // Só agora: a thread já sinalizou e já saiu.
#if KMP_WITH_KEYREAD
    // ---------------------------------------------------------------------
    // REMOVE O SERVICOS DO KBDCCLASS ANTES DE DESMAPEAR A VIEW.
    //
    // KbdClassInstallService(NULL) remove a entrada baixo o lock interno do
    // kbdclass: quando retorna, NENHUM callback nosso esta' em voo, e sem
    // callback em voo nao ha' execicao a partir das paginas que o user mode
    // vai liberar em seguida. A ordem esta' documentada no km_keyread.h.
    // ---------------------------------------------------------------------
    KrUninstall();
#endif
    if (g_St.Shared) {
        g_St.Shared->Hdr.Ativo = 0;
        g_St.Shared->Hdr.Ready = 0;
    }

    // ---------------------------------------------------------------------
    // LIBERACAO ATOMICA (v5).
    //
    // KmpShutdown agora pode rodar em DOIS contextos de uma vez: o user-mode
    // (Cleanup/Deactivate) e o item de limpeza do watchdog. O `if (p) release`
    // classico deixaria os dois chamarem ObDereference/MmUnmapView/ZwClose no
    // MESMO objeto — double free em nonpaged, o fim. O padrao e tomar o
    // ponteiro atomicamente: quem leva o ponteiro nao-nulo e' quem libera.
    // ---------------------------------------------------------------------
    {
        PVOID vb = (PVOID)(ULONG_PTR)InterlockedExchangePointer((PVOID*)&g_St.ViewBase, NULL);
        if (vb) {
            MmUnmapViewInSystemSpace(vb);
            g_St.Shared = NULL;
        }

        {
            PKEVENT ev = (PKEVENT)(ULONG_PTR)InterlockedExchangePointer((PVOID*)&g_St.SectionObject, NULL);
            if (ev) ObDereferenceObject(ev);
        }
        {
            PKEVENT ev = (PKEVENT)(ULONG_PTR)InterlockedExchangePointer((PVOID*)&g_St.ReqEvent, NULL);
            if (ev) ObDereferenceObject(ev);
        }
        {
            PKEVENT ev = (PKEVENT)(ULONG_PTR)InterlockedExchangePointer((PVOID*)&g_St.RspEvent, NULL);
            if (ev) ObDereferenceObject(ev);
        }

        HANDLE h;
        h = (HANDLE)(ULONG_PTR)InterlockedExchangePointer((PVOID*)&g_St.ReqEventHandle, NULL);
        if (h) ZwClose(h);
        h = (HANDLE)(ULONG_PTR)InterlockedExchangePointer((PVOID*)&g_St.RspEventHandle, NULL);
        if (h) ZwClose(h);
        h = (HANDLE)(ULONG_PTR)InterlockedExchangePointer((PVOID*)&g_St.ClientProcess, NULL);
        if (h) ZwClose(h);
        h = (HANDLE)(ULONG_PTR)InterlockedExchangePointer((PVOID*)&g_St.KeyHandle, NULL);
        if (h) ZwClose(h);
    }
}

// ===========================================================================
// BOOT
// ===========================================================================
static NTSTATUS KmpInit(ULONG64 seed, PUNICODE_STRING servicePath, PVOID imageBase)
{
    NTSTATUS          st;
    ULONG64           sig = 0, pid = 0, hSec = 0, hReq = 0, hRsp = 0;
    HANDLE            hSecKernel = NULL;
    PVOID             obj = NULL;
    SIZE_T            viewSize = 0;

    // -----------------------------------------------------------------------
    // (sem caminho de servico: os handles vem do g_BootSlot, nao de registro)

    RtlZeroMemory(&g_St, sizeof(g_St));
    KeInitializeEvent(&g_St.ThreadDone, NotificationEvent, FALSE);
    InterlockedExchange((volatile LONG*)&g_St.LifecycleState, KMP_LC_INIT);

    // -----------------------------------------------------------------------
    // SLOT DE BOOT — LEITURA DE SI MESMO, SEM REGISTRO.
    //
    // O `g_BootSlot` e' um campo estatico DESTE driver. O cliente grava os
    // handles nele enquanto o driver ainda e' um arquivo (os bytes embutidos no
    // .exe), antes de mapear. Quando o codigo comeca a rodar, os valores ja'
    // estao aqui.
    //
    // POR QUE ISSO SUBSTITUI O REGISTRO: o rendezvous por
    // `Classes\CLSID\<hex>` + 5 REG_QWORD apagados em ~1 ms e' um padrao que
    // qualquer driver ve' com CmRegisterCallback — igual para todo usuario do
    // build. Aqui nao ha' nada para observar: nenhuma escrita em HKLM, nenhum
    // nome de chave, nenhuma chamada de registro.
    // -----------------------------------------------------------------------
    if (g_BootSlot.Magic != KMP_BOOT_MAGIC) {
        // Sem o slot preenchido, nao ha' como achar a secao: falha alta e
        // visivel, em vez de tentar um caminho alternativo que o AC veria.
        return STATUS_INVALID_PARAMETER;
    }

    sig  = (ULONG64)g_BootSlot.Signature;
    pid  = (ULONG64)g_BootSlot.Pid;
    hSec = (ULONG64)g_BootSlot.Section;
    hReq = (ULONG64)g_BootSlot.ReqEvent;
    hRsp = (ULONG64)g_BootSlot.RspEvent;

    if (pid == 0 || pid > 0xFFFFFFFFULL || hSec == 0 || hReq == 0 || hRsp == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    g_St.Signature = sig;
    g_St.TargetPid = (ULONG)pid;

    // ---- 3) trazer os handles para o kernel ----
    //
    // A versao anterior duplicava os handles com ZwDuplicateObject e devolvia
    // STATUS_INVALID_HANDLE mesmo com os valores corretos no registro. Duas
    // causas provaveis: `NtCurrentProcess()` como destino (pseudo-handle -1 nao
    // e' resolvido de forma confiavel em chamada de kernel) e OBJ_KERNEL_HANDLE
    // passado como flag de duplicacao, que nao e' flag de duplicacao.
    //
    // Aqui NAO se duplica nada. Os handles 0xAC/0xB0/0xB4 so' valem DENTRO do
    // processo dono, entao a gente ANEXA a esse processo, referencia os objetos
    // pelo handle no contexto certo, e desanexa. E' o caminho documentado para
    // kernel obter objeto a partir de handle de user mode, e dispensa tambem o
    // PROCESS_DUP_HANDLE.
    {
        PEPROCESS  pep = NULL;
        KAPC_STATE apc;

    st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)g_St.TargetPid, &pep);
        if (!NT_SUCCESS(st)) goto fail;

        KeStackAttachProcess(pep, &apc);

        st = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)hReq, EVENT_MODIFY_STATE,
                                       *ExEventObjectType, UserMode, &obj, NULL);
        if (NT_SUCCESS(st)) { g_St.ReqEvent = (PKEVENT)obj; obj = NULL; }

        if (NT_SUCCESS(st)) {
            st = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)hRsp, EVENT_MODIFY_STATE,
                                           *ExEventObjectType, UserMode, &obj, NULL);
            if (NT_SUCCESS(st)) { g_St.RspEvent = (PKEVENT)obj; obj = NULL; }
        }

        if (NT_SUCCESS(st)) {
            st = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)hSec,
                                           SECTION_MAP_READ | SECTION_MAP_WRITE,
                                           *MmSectionObjectType, UserMode, &obj, NULL);
            if (NT_SUCCESS(st)) { g_St.SectionObject = obj; obj = NULL; }
        }

        KeUnstackDetachProcess(&apc);
        ObDereferenceObject(pep);

        if (!NT_SUCCESS(st)) goto fail;
    }
    // ---- 4) mapear a view ----
    // MmMapViewInSystemSpace e nao ZwMapViewOfSection:
    //
    //   ZwMapViewOfSection(exibicao) exige um HANDLE DE PROCESSO. Eu passava
    //   `NtCurrentProcess()` = (HANDLE)-1, que e' pseudo-handle de USER mode —
    //   o kernel nao resolve, e a chamada devolve STATUS_INVALID_HANDLE. Foi
    //   exatamente o FailStep=5.
    //
    //   MmMapViewInSystemSpace mapeia a secao no espaco de SISTEMA e e' a API
    //   documentada para driver fazer isso. Nao precisa de handle de processo
    //   nenhum, e as paginas sao as MESMAS que o user mode mapeia — o
    //   compartilhamento continua valendo.
    st = MmMapViewInSystemSpace(g_St.SectionObject, &g_St.ViewBase, &viewSize);
    if (!NT_SUCCESS(st)) goto fail;
    g_St.ViewSize = viewSize;
    g_St.Shared   = (PKMP_SHARED)g_St.ViewBase;

    // A seção tem que caber o cabeçalho. É o TESTE que importa: a seção pode
    // ter sido criada com tamanho arbitrário pelo cliente, e escrever 260 KB
    // num view de 4 KB é bugcheck, não erro.
    if (viewSize < (SIZE_T)KMP_SHARED_BYTES) {
        st = STATUS_BUFFER_TOO_SMALL;
        goto fail;
    }

    // ---- 5) publicar o cabeçalho ANTES de acordar o cliente ----
    g_St.Shared->Hdr.Magic       = KMP_MAGIC;
    g_St.Shared->Hdr.Version     = KMP_VERSION;
    g_St.Shared->Hdr.Signature   = sig;
    g_St.Shared->Hdr.TotalBytes  = (ULONG64)sizeof(KMP_SHARED);
    g_St.Shared->Hdr.ReqOffset   = (ULONG64)FIELD_OFFSET(KMP_SHARED, Req);
    g_St.Shared->Hdr.RespOffset  = (ULONG64)FIELD_OFFSET(KMP_SHARED, Resp);
    g_St.Shared->Hdr.DataOffset  = (ULONG64)sizeof(KMP_SHARED);
    g_St.Shared->Hdr.DataBytes   = KMP_DATA_BYTES;
    g_ImageBaseKm = (ULONG64)(ULONG_PTR)imageBase;
    g_ImageSizeKm = 0;   // preenchido abaixo, se o PE estiver legivel
    if (g_ImageBaseKm && KmPtrOk((const void*)g_ImageBaseKm)) {
        __try {
            PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)g_ImageBaseKm;
            if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
                PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(g_ImageBaseKm + dos->e_lfanew);
                if (nt->Signature == IMAGE_NT_SIGNATURE)
                    g_ImageSizeKm = (ULONG64)nt->OptionalHeader.SizeOfImage;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_ImageSizeKm = 0; }
    }
    g_St.Shared->Hdr.ImageBase   = (ULONG64)(ULONG_PTR)imageBase;
    KeMemoryBarrier();
    g_St.Shared->Hdr.Ativo = 1;
    KeMemoryBarrier();
    g_St.Shared->Hdr.Ready = 1;

    // ---- 6) KEYREAD — conecta o kbdclass ANTES de subir o servidor ----
    //
    // Falha NAO e' fatal: o mask fica zero/sem a flag KROK e o user-mode cai
    // no fallback do hook LL (que continua instalado). O trabalho pesado (a
    // publicacao na secao) fica no work item, que so' existe daqui para la'.
    // ----------------------------------------------------------------------
#if KMP_WITH_KEYREAD
    KrInstall(g_St.ReqEvent);
#endif

    // ---- 7) subir o servidor ----
#if KMP_SERVER_WORKITEM
    // WORK ITEM: nenhuma thread nossa, nenhum StartAddress apontando para a
    // imagem. `ThreadHandle` fica NULL — o KmpShutdown sabe disso.
    KmpAgendarServidor(&g_St);
    st = STATUS_SUCCESS;
#else
    st = PsCreateSystemThread(&g_St.ThreadHandle, 0, NULL,
                              NULL, NULL, KmpServerThread, &g_St);
    if (!NT_SUCCESS(st)) goto fail;
#endif

    // A CHAVE SO' MORRE AGORA, depois da thread. Antes ela era apagada antes do
    // PsCreateSystemThread; quando ele falhava, o `fail:` tentava gravar o passo
    // numa chave que nao existia mais — e o diagnostico sumia junto.
    // (a chave do rendezvous nao existe mais: os handles vem do g_BootSlot)

    return STATUS_SUCCESS;

fail:
    KmpShutdown();
    return st;
}

// ===========================================================================
// ENTRADA
//
// O SEED CHEGA POR DOIS CAMINHOS DIFERENTES, e os dois têm que funcionar:
//
//   1. kdmapper -> `RegistryPath` é o SEED (contrato do AutoLoader).
//   2. serviço  -> `RegistryPath` é
//                  `\Registry\Machine\System\CurrentControlSet\Services\kmproto`
//                  e o seed vem do valor `Seed` (REG_QWORD) em
//                  `...\kmproto\Parameters`, gravado pelo cliente ANTES do
//                  StartService.
//
// O caminho 2 existe porque `sc create` + `StartService` é a forma DOCUMENTADA
// de carregar isto numa VM com test signing — sem kdmapper, sem driver doador,
// e sem o `0xC000016A` que o antivírus provoca ao gravar o iqvw64e no TEMP.
//
// A distinção é pela FORMA: caminho de registro começa em `\` e tem menos de
// 256 caracteres. Um seed (ponteiro) não começa em `\`.
//
// ATENÇÃO: no caminho 1, `DriverObject` NÃO é um DRIVER_OBJECT — é a base real
// da alocação (passAllocationPtr = true). Escrever `DriverUnload` nela
// corromperia a imagem, então o unload só é registrado no caminho 2.
// ===========================================================================
static VOID KmpUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    KmpShutdown();
}

// ---------------------------------------------------------------------------
// KmpLooksLikeRegistryPath — O TESTE QUE DERRUBOU A MAQUINA (0x1E).
//
// A versao anterior fazia:
//
//     if (p == NULL || p->Buffer == NULL) return FALSE;
//     if (p->Length == 0 || p->Length > 512) return FALSE;
//
// ...ou seja, DESREFERENCIAVA `p` antes de saber se `p` era ponteiro. Pelo
// kdmapper, `RegistryPath` NAO e' UNICODE_STRING nenhum: e' o seed cru no
// (KmpLooksLikeRegistryPath removida: nao ha' mais RegistryPath no boot.)


NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    // -----------------------------------------------------------------------
    // NAO HA' MAIS REGISTRO NESTE CAMINHO.
    //
    // Ate 21/10/2026 o boot lia cinco REG_QWORD de
    // `HKLM\SOFTWARE\Classes\CLSID\<32 hex>` — rendezvous, e uma impressao
    // digital de familia (ver o comentario do KMP_BOOT_SLOT no protocolo).
    //
    // Agora os handles vem de `g_BootSlot`, um campo DESTA imagem que o cliente
    // preencheu antes de mapear. `RegistryPath` (quando existe) e' ignorado de
    // proposito: o kdmapper passa o seed em param2, entao o valor NAO e' um
    // UNICODE_STRING e qualquer leitura dele seria acesso invalido — foi assim
    // que a maquina caiu com 0x1E duas vezes.
    // -----------------------------------------------------------------------
    UNREFERENCED_PARAMETER(RegistryPath);

    // `DriverObject` NAO e' um DRIVER_OBJECT no caminho do kdmapper com
    // passAllocationPtr: e' a BASE do mapeamento. Vai para o cabecalho para o
    // user-mode poder liberar as paginas no fim da sessao.
    return KmpInit(0, NULL, (PVOID)DriverObject);
}
