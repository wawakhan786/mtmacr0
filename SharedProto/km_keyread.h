#pragma once
// ===========================================================================
// km_keyread.h — KEYREAD DE KERNEL via IOCTL_INTERNAL_KEYBOARD_CONNECT
//
// ---------------------------------------------------------------------------
// O QUE RESOLVE
//
// O estado das teclas (ALT/SHIFT, os modificadores dos hotkeys) vinha de um
// WH_KEYBOARD_LL no processo do cheat. Um hook LL de teclado e' um ponto
// classico de deteccao (a cadeia e' caminavel por qualquer processo; cada
// tecla do SISTEMA passa pela nossa rotina). Pior: no ambiente de VM o
// GetAsyncKeyState falha, entao o hook era a unica fonte.
//
// Conectando ao kbdclass como "consumidor" da fila de teclado, o estado nasce
// ONDE a tecla nasce: nenhum hook user-mode, nenhum polling, e funcionando
// mesmo com a cadeia de hooks do desktop quebrada.
//
// ---------------------------------------------------------------------------
// POR QUE SÓ KEYBOARD — E NÃO MOUSE
//
// O kbdclass faz BROADCAST: todos os conectados a uma unidade de teclado
// recebem a mesma copia de cada pacote. O mouclass NAO faz isso: cada
// conectado recebe APENAS os eventos da propria conexao. Por isso botao de
// mouse continua pelo WH_MOUSE_LL (que ja' existe e funciona).
//
// ---------------------------------------------------------------------------
// POR QUE NAO KbdClassInstallService
//
// A API classica ("install service" no kbdclass) NAO e' mais exportada: em
// builds recentes do ntoskrnl o simbolo sumiu das exports (verificado com
// dumpbin em Win11 24H2+), e nem os headers WDK atuais a declaram. O caminho
// documentado e estavel — o MESMO que o kbdhid usa para se conectar e o que
// drivers publicos de injecao leem (IOCTL_INTERNAL_KEYBOARD_CONNECT em
// \\Device\\KeyboardClassN) continua vivo desde o NT 4: enviá-lo sincrono
// para o device da classe devolve o service callback do kbdclass, e cada
// pacote de hardware e' fan-out'ado para todos os conectados, chamando a
// nossa PSERVICE_CALLBACK_ROUTINE. Zero scan de padrao, zero export fragil.
//
// ---------------------------------------------------------------------------
// REGRAS DE SEGURANÇA
//
// 1. O callback roda ate' DISPATCH_LEVEL (o kbdclass despacha dos DPCs da
//    porta). DENTRO dele so' se faz o que vale em DISPATCH:
//      - InterlockedOr/And num LONG estatico da IMAGEM (nonpaged);
//      - KeSetEvent (seguro ate DISPATCH);
//    NAO se escreve na secao compartilhada ai: a publicacao fica no work item
//    do servidor (PASSIVE_LEVEL). Escrever na view em DISPATCH ja' custou 0x50
//    neste projeto.
//
// 2. O DEVICE cartorio e' criado sob um DriverObject REAL emprestado de um
//    driver do sistema (ACPI e outros, resolvido por nome). APRENDIZADO DO
//    BSOD 0x18 DE 22/10/26: um DRIVER_OBJECT fabricado em static da imagem
//    NAO tem _OBJECT_HEADER — qualquer operacao do Object Manager por
//    ponteiro (a que o proprio IoCreateDevice/kbdclass fazem nele) le o
//    header fantasma e bugchecka REFERENCE_BY_POINTER no boot. O objeto
//    emprestado carrega +1 referencia nossa ate' o uninstall; o device
//    continua sem nome e sem fila aberta (puro cartorio para o CONNECT_DATA).
//
// 3. Uninstall ANTES de MmFreeIndependentPages: o DISCONNECT sincrono para
//    cada unidade tira a gente da lista do kbdclass BAIXO o lock interno dele;
//    quando todos retornam, nenhum callback novo entra em voo — e sem
//    callback em voo, nenhuma execucao a partir das paginas que o user mode
//    esta' para liberar.
//
// 4. Falha de install NAO e' fatal: nada conectado = mask zero + sem a flag
//    KROK na secao = o user-mode cai no fallback do hook LL (que continua
//    instalado de proposito). NADA aqui pode bugcheckar o boot.
// ===========================================================================

#include <ntifs.h>
#include <kbdmou.h>   // CONNECT_DATA, PSERVICE_CALLBACK_ROUTINE, IOCTL_INTERNAL_KEYBOARD_*
#include <ntddkbd.h>  // KEYBOARD_INPUT_DATA, KEY_MAKE/KEY_BREAK/KEY_E0
#include "km_ipc_proto.h"   // KMP_KM_*

// O ntifs do WDK 28000 nao declara estes dois (mesmo motivo que no km_mouse.h):
// sem declaracao o cl assume retorno int (C4013) e perde a checagem de tipo.
NTKERNELAPI NTSTATUS NTAPI ObReferenceObjectByName(
    PUNICODE_STRING ObjectName, ULONG Attributes, PACCESS_STATE PassedAccessState,
    ACCESS_MASK DesiredAccess, POBJECT_TYPE ObjectType, KPROCESSOR_MODE AccessMode,
    PVOID ParseContext, PVOID* Object);
extern POBJECT_TYPE* IoDriverObjectType;

#ifndef KMP_WITH_KEYREAD
#define KMP_WITH_KEYREAD 1
#endif

#if KMP_WITH_KEYREAD

// ---------------------------------------------------------------------------
// MASK VIVO — atualizado pelo callback em qualquer IRQL.
//
// Definicoes (nao extern): o header e' incluido so' pelo km_ipc_driver.c
// (mesmo padrao das estaticas abaixo); o valor mora na imagem do driver
// (nonpaged enquanto ela estiver mapeada), e' o que queremos.
//
// `volatile` + Interlocked*: a escrita e' 32 bits alinhada (atomica por
// construto no x86-64), mas os Interlocked* impedem que o otimizador cacheie
// a leitura quando o work item publica.
// ---------------------------------------------------------------------------
static volatile LONG KrMaskLive   = 0;
static volatile LONG KrInstalled  = 0;    // 1 quando ha pelo menos uma unidade conectada

// Evento que desperta o work item do servidor (o ReqEvent). Atribuido no
// install ANTES de qualquer unidade ser conectada; lido no callback
// (DISPATCH-safe: leitura de ponteiro + KeSetEvent).
static PKEVENT g_KrWakeEvent = NULL;

// ---------------------------------------------------------------------------
// HOST DRIVER_OBJECT EMPRESTADO + nosso DEVICE_OBJECT.
//
// O CONNECT_DATA entregue ao kbdclass aponta para UM device object nosso
// (ClassDeviceObject). Tanto o I/O manager quanto o kbdclass fazem operacoes
// de Object Manager POR PONTEIRO nessa regiao — e por ponteiro exige
// _OBJECT_HEADER legitimo. Logo:
//
//   * o device e' criado de verdade (IoCreateDevice), nunca NULL;
//   * o DriverObject que vai no IoCreateDevice e' REAL: emprestamos por nome
//     (+1 ref nossa) o de um driver do sistema que sempre esta' carregado.
//     Fabricar o nosso em static (o que o BSOD 0x18 de 22/10/26 provou)
//     coloca header fantasma na frente do OM.
//
// O device e' puro cartorio: sem nome, sem link, sem abridor — ninguem envia
// IRP a ele fora do fan-out de callback do kbdclass.
// ---------------------------------------------------------------------------
static PDRIVER_OBJECT g_KrHostDrv = NULL;   // +1 ref nossa (ObjRefByName)
static PDEVICE_OBJECT g_KrDevObj  = NULL;

#define KMP_KR_MAX_UNITS 8   // KEYBOARD_PORTS_MAXIMUM (kbdmou.h)

// Uma linha por unidade de teclado conectada.
typedef struct _KR_UNIT {
    BOOLEAN       Connected;
    PFILE_OBJECT  FileObj;    // ponteiro EMPRESTADO (ver KrConectarUnidade)
    PDEVICE_OBJECT DevObj;    // idem
    CONNECT_DATA  Conn;       // copia do buffer que o kbdclass preencheu
} KR_UNIT;

static KR_UNIT g_KrUnits[KMP_KR_MAX_UNITS];

// ---------------------------------------------------------------------------
// RESOLVER O HOST REAL — mesmo padrao do km_mouse.h (ObReferenceObjectByName
// em \\Driver\Mouhid): pega +1 referencia NUM driver object de OM, com header
// legítimo, e devolve o ponteiro. A lista e' ordem de preferencia; em qualquer
// desktop Win10/11 x64 o primeiro (ACPI) ja' esta' ali. Sem host = keyread
// desliga (falha benigna), o fallback LL cobre.
// ---------------------------------------------------------------------------
static PDRIVER_OBJECT KrPegarHostReal(void)
{
    static const WCHAR *hosts[] = {
        L"\\Driver\\ACPI",    // presente em todo Win10/11 x64 desktop
        L"\\Driver\\CNG",
        L"\\Driver\\volmgr",
        L"\\Driver\\atapi",
    };
    ULONG i;

    for (i = 0; i < (ULONG)(sizeof(hosts) / sizeof(hosts[0])); i++) {
        UNICODE_STRING us;
        RtlInitUnicodeString(&us, hosts[i]);
        if (NT_SUCCESS(ObReferenceObjectByName(&us, OBJ_CASE_INSENSITIVE,
                                              NULL, 0, *IoDriverObjectType,
                                              KernelMode, NULL,
                                              (PVOID*)&g_KrHostDrv))) {
            return g_KrHostDrv;
        }
        g_KrHostDrv = NULL;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// O CALLBACK DE TECLADO (assinatura PSERVICE_CALLBACK_ROUTINE de kbdmou.h):
//
//     NormalContext    = nosso ClassDeviceObject (guardado no connect)
//     SystemArgument1  = primeiro KEYBOARD_INPUT_DATA do lote
//     SystemArgument2  = um-passa-do-ultimo
//     SystemArgument3  = PULONG "quantos pacotes foram consumidos"
//
// Scancodes (set/2) que interessam — espelhando os indices do HotkeyHook:
//
//     0x38 = Alt (e AltGr, prefixado E0: o scancode continua 0x38)
//     0x2A = Shift esquerdo   0x36 = Shift direito
//
// Flags usa KEY_MAKE(0)/KEY_BREAK(1): pressao adiciona o bit ao mask,
// soltura tira. Teclas fora do interesse nem tocam na memoria.
//
// IRQL: ate' DISPATCH (ver regra 1 do topo). Nada de pool, nada de secao,
// nada de impressao — a memoria apontada pelos argumentos pertence ao
// kbdclass (nonpaged enquanto ele estiver nela).
// ---------------------------------------------------------------------------
static VOID KrPortCallback(PVOID NormalContext, PVOID SystemArgument1,
                           PVOID SystemArgument2, PVOID SystemArgument3)
{
    PKEYBOARD_INPUT_DATA start = (PKEYBOARD_INPUT_DATA)SystemArgument1;
    PKEYBOARD_INPUT_DATA end   = (PKEYBOARD_INPUT_DATA)SystemArgument2;
    PULONG pConsumed           = (PULONG)SystemArgument3;
    PKEYBOARD_INPUT_DATA p;

    UNREFERENCED_PARAMETER(NormalContext);

    if (!start || !end || end <= start)
        goto fim;

    for (p = start; p < end; p++) {
        const USHORT sc  = p->MakeCode;
        const BOOLEAN up = (p->Flags & KEY_BREAK) != 0;
        volatile LONG* mask = &KrMaskLive;

        switch (sc) {
        case 0x38:      // Alt / AltGr
            if (up) InterlockedAnd(mask, (LONG)~KMP_KM_ALT);
            else    InterlockedOr (mask, (LONG)KMP_KM_ALT);
            break;
        case 0x2A:      // Shift esquerdo
        case 0x36:      // Shift direito
            // Soltura so' apaga se NENHUM shift estiver mais pressionado:
            // sem dois bits por tecla a soltura cruzada (soltar o d. com o
            // e. segurado) apaga a toa. O custo de errar e' o hotkey pedir
            // mais um frame — nao um crash, e o hook LL cobre a falha.
            if (up) InterlockedAnd(mask, (LONG)~KMP_KM_SHIFT);
            else    InterlockedOr (mask, (LONG)KMP_KM_SHIFT);
            break;
        default:
            continue;   // o resto do teclado nao interessa: proximo pacote
        }
    }

fim:
    if (pConsumed != NULL)
        *pConsumed = (ULONG)((end <= start) ? 0 : (end - start));

    // Acorda o servidor para publicar o mask na secao. O evento e' o ReqEvent
    // do servidor, referenciado ate' o fim do shutdown — KeSetEvent nele
    // continua valido mesmo enquanto estamos desconectando.
    if (g_KrWakeEvent != NULL)
        KeSetEvent(g_KrWakeEvent, IO_NO_INCREMENT, FALSE);
}

// ---------------------------------------------------------------------------
// CONECTAR UMA UNIDADE — IoGetDeviceObjectPointer("\\Device\\KeyboardClassN")
// + IOCTL_INTERNAL_KEYBOARD_CONNECT sincrono. E' exatamente o fluxo do
// kbdhid com a classe (mesmo IOCTL, mesmo METHOD_NEITHER): entregamos nosso
// ClassDeviceObject + ClassService; em troca o kbdclass passa a chamar a
// nossa rotina em todo pacote daquela unidade (fan-out para todos os
// conectados). Sem QUERY_ATTRIBUTES/IDENTIFICATION: a versao simplificada
// usada por drivers de injecao atuais conecta direto e o kbdclass aceita.
// ---------------------------------------------------------------------------
static BOOLEAN KrConectarUnidade(ULONG idx)
{
    KR_UNIT* u = &g_KrUnits[idx];
    UNICODE_STRING nome;
    WCHAR          buf[32];   // "\\Device\\KeyboardClass" tem 22 chars + 1 digito + NUL
    PFILE_OBJECT fo = NULL;
    PDEVICE_OBJECT dno = NULL;
    CONNECT_DATA conn;
    IO_STATUS_BLOCK iosb;
    KEVENT ev;
    PIRP irp;
    NTSTATUS st;

    RtlCopyMemory(buf, L"\\Device\\KeyboardClass", 22 * sizeof(WCHAR));
    buf[22] = (WCHAR)(L'0' + idx);   // idx < 8: um digito
    buf[23] = L'\0';
    RtlInitUnicodeString(&nome, buf);

    st = IoGetDeviceObjectPointer(&nome, FILE_READ_DATA, &fo, &dno);
    if (!NT_SUCCESS(st))
        return FALSE;          // unidade inexistente: segue para o proximo indice

    // IMPORTANTE (lição do BSOD 0x18): os ponteiros que
    // IoGetDeviceObjectPointer devolve sao EMPRESTADOS — sem referencia nossa.
    // Jamais fazer ObDereferenceObject neles: isso tiraria uma contagem que
    // nao e' nossa e bugcheckaria no fechamento alheio (o mesmo bugcheck).

    RtlZeroMemory(&conn, sizeof(conn));
    conn.ClassDeviceObject = g_KrDevObj;
#pragma warning(push)
#pragma warning(disable:4152)   // conversao de ponteiro de funcao para PVOID (padrao kbfiltr)
    conn.ClassService      = (PVOID)(PSERVICE_CALLBACK_ROUTINE)KrPortCallback;
#pragma warning(pop)

    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_KEYBOARD_CONNECT, dno,
                                        &conn, sizeof(conn),
                                        &conn, sizeof(conn),
                                        TRUE, &ev, &iosb);
    if (irp == NULL) {
        return FALSE;   // sem pool: ponteiros emprestados ficam com o sistema
    }

    st = IoCallDriver(dno, irp);
    if (st == STATUS_PENDING)
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);

    if (!NT_SUCCESS(iosb.Status)) {
        return FALSE;   // idem: nao dereference o que nao e' nosso
    }

    u->Connected = TRUE;
    u->FileObj   = fo;
    u->DevObj    = dno;
    u->Conn      = conn;
    return TRUE;
}

// ---------------------------------------------------------------------------
// DESCONECTAR UMA UNIDADE — IRP sincrona de disconnect para a unidade.
// Ao completar, o kbdclass ja' nao nos chama mais para aquele teclado (a
// remocao da lista acontece sob o lock interno dele, dentro do dispatch).
// ---------------------------------------------------------------------------
static VOID KrDesconectarUnidade(KR_UNIT* u)
{
    IO_STATUS_BLOCK iosb;
    KEVENT ev;
    PIRP irp;
    NTSTATUS st;

    if (!u->Connected)
        return;

    RtlZeroMemory(&iosb, sizeof(iosb));
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_KEYBOARD_DISCONNECT, u->DevObj,
                                        NULL, 0, NULL, 0,
                                        TRUE, &ev, &iosb);
    if (irp != NULL) {
        st = IoCallDriver(u->DevObj, irp);
        if (st == STATUS_PENDING)
            KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        // Erro (ex.: unidade ja' desconectada fisicamente) e' esperado e OK:
        // ou o kbdclass nos removeu, ou nunca nos chamou mais.
    }

    // Ponteiros EMPRESTADOS: nao se dereference (ver KrConectarUnidade).
    RtlZeroMemory(u, sizeof(*u));
}

// ---------------------------------------------------------------------------
// INSTALL. Chamado do boot do driver (PASSIVE_LEVEL). Nenhuma falha pode
// escapar: erros voltam como status, e ate' uma EXCECAO vira falha benigna
// (o ultimo recurso contra "nada aqui pode bugcheckar o boot").
// ---------------------------------------------------------------------------
static NTSTATUS KrInstall(PKEVENT wakeEvent)
{
    volatile NTSTATUS st = STATUS_SUCCESS;
    ULONG i;
    int conectadas = 0;

    if (InterlockedCompareExchange(&KrInstalled, 1, 0) != 0)
        return STATUS_SUCCESS;   // ja' conectado nesta carga

    RtlZeroMemory(g_KrUnits, sizeof(g_KrUnits));
    g_KrHostDrv = NULL;
    g_KrDevObj  = NULL;
    g_KrWakeEvent = NULL;

    // Zera o mask ANTES de conectar: se Alt/Shift ja' estiverem fisicamente
    // pressionados, o primeiro callback apos o connect reconstroi o estado.
    InterlockedExchange(&KrMaskLive, 0);

    __try {
        // Host REAL (header de OM legítimo) — nunca um fake estatico.
        if (KrPegarHostReal() == NULL)
            st = STATUS_DEVICE_NOT_CONNECTED;
        else {
            st = IoCreateDevice(g_KrHostDrv, 0, NULL, FILE_DEVICE_KEYBOARD,
                                0, FALSE, &g_KrDevObj);
            // Sem fila manual: o WDK 28000 nao declara DO_DEVICE_OBJECT_QUEUE
            // e o device NAO recebe IRP mesmo (sem nome/link/abridor); quem
            // entrega entrada e' o callback do kbdclass, nao dispatch.
        }

        if (NT_SUCCESS(st)) {
            // Varre as unidades existentes (tipicamente KeyboardClass0; uma
            // maquina com teclado embutido + externo tem mais). Pelo menos
            // UMA precisa dar certo, senao o keyread nao serve para nada.
            for (i = 0; i < KMP_KR_MAX_UNITS; i++)
                if (KrConectarUnidade(i))
                    conectadas++;

            if (conectadas == 0) {
                IoDeleteDevice(g_KrDevObj);
                g_KrDevObj = NULL;
                st = STATUS_DEVICE_NOT_CONNECTED;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Excecao no caminho (ex.: pagina do host tocada em voo): converte
        // em falha e sai pelo mesmo cleanup. O estado parcial e' limpo por
        // baixo; no pior caso sobra ref que some no reboot — nunca um bug.
        st = (NTSTATUS)GetExceptionCode();
    }

    if (!NT_SUCCESS(st))
        goto falha;

    // So' publica o evento APOS tudo pronto: enquanto ele e' NULL o callback
    // acorda ninguem (guarda dentro dele), o que cobre a janela em que os
    // connects ja' podiam disparar pacotes. A partir daqui, qualquer tecla
    // acorda o work item para publicar.
    g_KrWakeEvent = wakeEvent;
    return STATUS_SUCCESS;

falha:
    // Limpa o que foi feito ate' aqui antes de sair. Ordem: tira os callbacks
    // da fila do kbdclass, depois o device, so' apos isso a ref do host.
    for (i = 0; i < KMP_KR_MAX_UNITS; i++)
        KrDesconectarUnidade(&g_KrUnits[i]);
    if (g_KrDevObj != NULL) {
        IoDeleteDevice(g_KrDevObj);
        g_KrDevObj = NULL;
    }
    if (g_KrHostDrv != NULL) {
        ObDereferenceObject(g_KrHostDrv);
        g_KrHostDrv = NULL;
    }
    g_KrWakeEvent = NULL;
    InterlockedExchange(&KrInstalled, 0);
    return st;
}

// ---------------------------------------------------------------------------
// UNINSTALL — espelho do install. Ordem importa (regra 3): desconecta todas
// as unidades (retira os callbacks da lista do kbdclass), so' depois libera
// o device cartorio (o kbdclass ainda podia referencia-lo ate' o ultimo
// disconnect completar).
// ---------------------------------------------------------------------------
static VOID KrUninstall(void)
{
    ULONG i;

    if (InterlockedCompareExchange(&KrInstalled, 0, 1) == 0)
        return;   // nunca foi conectado (install falhou antes)

    for (i = 0; i < KMP_KR_MAX_UNITS; i++)
        KrDesconectarUnidade(&g_KrUnits[i]);

    if (g_KrDevObj != NULL) {
        IoDeleteDevice(g_KrDevObj);
        g_KrDevObj = NULL;
    }
    if (g_KrHostDrv != NULL) {
        ObDereferenceObject(g_KrHostDrv);
        g_KrHostDrv = NULL;
    }
    g_KrWakeEvent = NULL;
}

// ---------------------------------------------------------------------------
// PUBLICACAO — chamada pelo work item do servidor (PASSIVE) em cada ciclo.
// ---------------------------------------------------------------------------
static kmp_u32 KrPublish(void)
{
    kmp_u32 m = (kmp_u32)(ULONG)KrMaskLive & KMP_KM_MASK;
    if (KrInstalled) m |= KMP_KM_KROK;
    return m;
}

#endif // KMP_WITH_KEYREAD
