#pragma once


// Defaults do contrato: quem inclui pode definir antes para logar de verdade.
#ifndef KM_MOUSE_LOG
#define KM_MOUSE_LOG(txt)            do { } while (0)
#endif
#ifndef KM_MOUSE_LOG_HEX
#define KM_MOUSE_LOG_HEX(pfx, v)     do { } while (0)
#endif
#ifndef KM_MOUSE_LOG_HEX64
#define KM_MOUSE_LOG_HEX64(pfx, v)   do { } while (0)
#endif
#ifndef KM_MOUSE_LOG_DUMP
#define KM_MOUSE_LOG_DUMP(pfx, p, n) do { } while (0)
#endif
// ---------------------------------------------------------------------------
// Declaracoes que o ntifs do WDK 28000 NAO traz e que este header usa.
// Ficam AQUI (e nao no driver que inclui) para o header ser autossuficiente:
// sem isso, cada .c que o incluir teria que repetir as mesmas tres linhas e
// esquecer uma delas vira erro de compilacao no meio do bloco de mouse.
// ---------------------------------------------------------------------------
// O ntifs do WDK 28000 nao declara esta; sem declaracao o compilador assume
// retorno `int` e perde a checagem de tipo (a chamada funciona por acidente no
// x64, que tem uma ABI so'' — mas compila com C4013 e isso e'' ruido que
// esconde erro de verdade).
NTKERNELAPI NTSTATUS NTAPI ObReferenceObjectByName(
    PUNICODE_STRING ObjectName, ULONG Attributes, PACCESS_STATE PassedAccessState,
    ACCESS_MASK DesiredAccess, POBJECT_TYPE ObjectType, KPROCESSOR_MODE AccessMode,
    PVOID ParseContext, PVOID* Object);

extern POBJECT_TYPE* IoDeviceObjectType;
extern POBJECT_TYPE* IoDriverObjectType;
extern PLIST_ENTRY    PsLoadedModuleList;

// ===========================================================================
// km_mouse.h - INJECAO DE MOUSE POR KERNEL (Mouclass), compartilhada.
//
// DE ONDE VEM: este bloco nasceu dentro de `km_safe_source.c` (driver IOCTL) e
// ficou provado la: resolve o `MouseClassServiceCallback` no DeviceExtension do
// Mouclass e chama com DISPATCH_LEVEL, sem `SendInput` e sem `LLMHF_INJECTED`.
//
// POR QUE VIROU HEADER: o caminho IOCTL vai sair (decisao do He, 20/10/2026 -
// "ficar full KEVENT"). Se o mouse ficasse dentro do driver que sera apagado,
// o aimbot e o RCS morreriam junto. Aqui ele sobrevive: os DOIS drivers incluem
// este header, e ele nao depende de device, de IOCTL nem de nome derivado.
//
// CONTRATO COM QUEM INCLUI:
//   * `KM_MOUSE_LOG(txt)`      - log opcional (pode ser no-op)
//   * `KM_MOUSE_LOG_HEX(pfx,v)`- idem, para valor hex
//   * ntifs.h + ntimage.h ja' incluidos antes deste header
//
// API:
//   BOOLEAN KmResolveMouseCallback(void);      // 1x, lazy
//   NTSTATUS KmInjectMouse(int dx, int dy);    // movimento relativo
//   void     KmMouseRelease(void);             // solta a referencia do device
// ===========================================================================


// `IoDeviceObjectType` 鈥?necessario para o ObReferenceObjectByPointer do
// device de classe do mouse (KmResolveMouseCallback). Mesmo motivo do de cima:
// o ntifs.h do WDK 28000 nao declara, e o simbolo existe no ntoskrnl.
// O ntifs do WDK 28000 nao declara esta; sem declaracao o compilador assume
// retorno `int` e perde a checagem de tipo (a chamada funciona por acidente no
// x64, que tem uma ABI so'' — mas compila com C4013 e isso e'' ruido que
// esconde erro de verdade).
NTKERNELAPI NTSTATUS NTAPI ObReferenceObjectByName(
    PUNICODE_STRING ObjectName, ULONG Attributes, PACCESS_STATE PassedAccessState,
    ACCESS_MASK DesiredAccess, POBJECT_TYPE ObjectType, KPROCESSOR_MODE AccessMode,
    PVOID ParseContext, PVOID* Object);

extern POBJECT_TYPE* IoDeviceObjectType;

// ---------------------------------------------------------------------------
// KLDR_DATA_TABLE_ENTRY 鈥?a entrada de PsLoadedModuleList.
//
// Layout x64, estavel desde o Vista. Os tres LIST_ENTRY primeiro (a lista e'
// ligada por InLoadOrderLinks, que e' o campo +0x00).
// ---------------------------------------------------------------------------
typedef struct _KM_LDR_ENTRY {
    LIST_ENTRY     InLoadOrderLinks;        // +0x00
    LIST_ENTRY     InMemoryOrderLinks;      // +0x10
    LIST_ENTRY     InInitializationOrderLinks; // +0x20
    PVOID          DllBase;                 // +0x30
    PVOID          EntryPoint;              // +0x38
    ULONG          SizeOfImage;             // +0x40
    UNICODE_STRING FullDllName;             // +0x48
    UNICODE_STRING BaseDllName;             // +0x58
} KM_LDR_ENTRY;

// ---------------------------------------------------------------------------
// MOUSE_INPUT_DATA 鈥?o pacote do mouse em kernel.
//
// Declarado aqui em vez de incluir `ntddmou.h` porque o WDK nao expoe esse
// header para drivers comuns. O layout e' o do x64: `Buttons` e' um union de
// ULONG com {USHORT ButtonFlags; USHORT ButtonData;} 鈥?mesmo tamanho, entao
// declarar como ULONG e' equivalente para MOVIMENTO (que nao mexe em botoes).
// ---------------------------------------------------------------------------
typedef struct _KM_MOUSE_INPUT_DATA {
    USHORT UnitId;
    USHORT Flags;
    ULONG  Buttons;
    ULONG  RawButtons;
    LONG   LastX;
    LONG   LastY;
    ULONG  ExtraInformation;
} KM_MOUSE_INPUT_DATA;

typedef VOID (*KM_MOUSE_SERVICE_CALLBACK)(
    PDEVICE_OBJECT        DeviceObject,
    KM_MOUSE_INPUT_DATA*  InputDataStart,
    KM_MOUSE_INPUT_DATA*  InputDataEnd,
    PULONG                InputDataConsumed);

// Ponteiro do callback, resolvido UMA vez (lazy) e guardado.
static KM_MOUSE_SERVICE_CALLBACK g_MouseCallback = NULL;
static PDEVICE_OBJECT           g_MouseDevice   = NULL;

// TRUE quando `g_MouseDevice` carrega uma REFERENCIA nossa (ObReferenceObjectByPointer).
// Existe porque referencia sem par escrito e' a forma mais silenciosa de vazar
// um objeto do kernel: nada falha, nada loga, e o objeto fica vivo ate' o
// reboot. Ver o bloco "REFERENCIA DO DEVICE DE CLASSE" no fim do
// KmResolveMouseCallback e o KmShutdownDevice.
static BOOLEAN                  g_MouseDeviceRefd = FALSE;

static LONG                     g_MouseResolved = 0;   // 0=nao tentou 1=tentou

// Prototipo: `KmAchaModulo` usa `KmWcsStrI`, que e' definida logo abaixo.
static BOOLEAN KmWcsStrI(PCWSTR hay, PCWSTR needle);

// ---------------------------------------------------------------------------
// Acha [base, base+size) de um modulo pelo nome.
//
// Compara so' o nome base ("mouclass.sys"), case-insensitive, e aceita tanto
// "mouclass.sys" quanto "mouclass" 鈥?o campo BaseDllName tem o nome com
// extensao, mas isso ja' variou.
// ---------------------------------------------------------------------------
static BOOLEAN KmAchaModulo(PCWSTR nome, PULONG_PTR base, PULONG tamanho)
{
    PLIST_ENTRY cursor;

    if (!PsLoadedModuleList || !nome || !base || !tamanho) return FALSE;

    for (cursor = PsLoadedModuleList->Flink;
         cursor && cursor != PsLoadedModuleList;
         cursor = cursor->Flink)
    {
        KM_LDR_ENTRY* e = CONTAINING_RECORD(cursor, KM_LDR_ENTRY, InLoadOrderLinks);
        if (!e->DllBase || e->BaseDllName.Buffer == NULL || e->BaseDllName.Length == 0)
            continue;

        if (KmWcsStrI(e->BaseDllName.Buffer, nome)) {
            *base   = (ULONG_PTR)e->DllBase;
            *tamanho = e->SizeOfImage;
            return TRUE;
        }
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// KmEhExecutavel 鈥?o ponteiro cai numa SECAO EXECUTAVEL do modulo?
//
// ===========================================================================
// ESTA FUNCAO EXISTE POR CAUSA DE UM BUGCHECK. LEIA ANTES DE MEXER.
// ===========================================================================
//
// A primeira versao da varredura aceitava qualquer ponteiro que caisse dentro
// de [base, base+SizeOfImage) do mouclass.sys. Isso estava ERRADO e derrubou a
// maquina:
//
//     0x000000FC  ATTEMPTED_EXECUTE_OF_NOEXECUTE_MEMORY
//     P1 = 0xfffff80153c57000      <- o endereco que tentamos executar
//     P2 = 0x89000008051db121      <- a PTE, com o bit NX SETADO
//     Modulo que falhou: mouclass.sys
//
// O bit NX no P2 e' a prova: aquele endereco estava numa pagina NAO-EXECUTAVEL
// do mouclass.sys. Ou seja, era um ponteiro de DADOS.
//
// O DeviceExtension esta' cheio de dados alem de funcoes: listas, flags,
// buffers, campos de configuracao. "Aponta para dentro do modulo" NAO distingue
// uma funcao de um blob de dados 鈥?e o primeiro ponteiro que casa pode ser o
// blob.
//
// A correcao e' a distincao que faltava: FUNCAO MORA EM SECAO EXECUTAVEL.
// Andando os headers de secao do PE e aceitando so' ponteiros que caiam numa
// secao com IMAGE_SCN_MEM_EXECUTE, um ponteiro de dados e' rejeitado POR
// CONSTRUCAO 鈥?nao por heuristica, e nao por "quase sempre".
//
// E' verificavel a priori: o kernel nao mapeia .data como executavel, e o
// proprio bugcheck provou isso (o NX estava setado na PTE).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// KmPtrOk - o resolver abaixo caminha por estruturas do PnP (device objects,
// DriverObject, DeviceExtension) que NAO sao nossas. Ler um ponteiro dessas
// estruturas sem checar e' como andar no escuro: um campo vazio ou reciclado
// vira KMODE_EXCEPTION_NOT_HANDLED (0x1E) com P1 = 0xC0000005, e a maquina cai
// sem nenhuma pista de qual leitura foi.
//
// `MmIsAddressValid` nao e' garantia absoluta (a pagina pode sumir entre a
// checagem e o acesso), mas elimina o caso comum: campo NULL, ponteiro
// reciclado, endereco de modo usuario. E' a diferenca entre "nao achou o mouse"
// e "tela azul".
// ---------------------------------------------------------------------------
static __inline BOOLEAN KmPtrOk(const void* p)
{
    return (p != NULL && MmIsAddressValid((PVOID)p)) ? TRUE : FALSE;
}


static BOOLEAN KmEhExecutavel(ULONG_PTR base, ULONG tam, ULONG_PTR alvo, char* nomeSecao)
{
    PIMAGE_DOS_HEADER     dos;
    PIMAGE_NT_HEADERS     nt;
    PIMAGE_SECTION_HEADER sec;
    USHORT                i;

    if (nomeSecao) nomeSecao[0] = 0;
    if (alvo < base || alvo >= base + tam) return FALSE;

    // O `base` vem de DriverStart de um DriverObject que NAO e' nosso: se a
    // estrutura foi reciclada, este endereco pode nao ser imagem nenhuma.
    // Ler o e_magic direto aqui era mais um caminho para 0x1E.
    if (!KmPtrOk((const void*)base)) return FALSE;
    dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;

    // Sanidade do e_lfanew: e' um deslocamento. Valor absurdo significa que nao
    // estamos lendo um PE de verdade 鈥?barra antes de seguir o ponteiro.
    if (dos->e_lfanew < 0x40 || dos->e_lfanew > 0x1000) return FALSE;

    if (!KmPtrOk((const void*)(base + dos->e_lfanew))) return FALSE;
    nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return FALSE;

    if (!KmPtrOk((const void*)nt)) return FALSE;
    if (nt->FileHeader.NumberOfSections > 96) return FALSE;   // absurdo = nao e' PE
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {

        ULONG_PTR ini, fim;

        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;

        ini = base + sec->VirtualAddress;
        fim = ini + sec->Misc.VirtualSize;
        if (alvo < ini || alvo >= fim) continue;

        if (nomeSecao) {
            int k;
            for (k = 0; k < 8 && sec->Name[k]; k++) nomeSecao[k] = (char)sec->Name[k];
            nomeSecao[k] = 0;
        }

        // =================================================================
        // AQUI ESTA' A CORRECAO DO 0xD1. LEIA ANTES DE MEXER.
        // =================================================================
        //
        // Rejeita CODIGO PAGINAVEL.
        //
        // O segundo bugcheck foi:
        //     0xD1 DRIVER_IRQL_NOT_LESS_OR_EQUAL em mouclass.sys
        //
        // "Acessou memoria PAGINAVEL num IRQL alto demais". E o candidato que a
        // varredura achou estava em:
        //
        //     PAGE   0x00C000 0x0028C7   Exec=SIM   chars=0x60000020
        //
        // Ou seja: secao EXECUTAVEL e PAGINAVEL ao mesmo tempo. O filtro de
        // "secao executavel" (feito pra consertar o 0xFC) aceitava ela numa
        // boa 鈥?e nos subimos pra DISPATCH_LEVEL e chamamos codigo que pode
        // estar paginado. Bugcheck imediato.
        //
        // ISTO NAO E' HEURISTICA, E' CONSEQUENCIA DO CONTRATO:
        //
        //     O MouseClassServiceCallback e' chamado pelo driver de PORTA em
        //     DISPATCH_LEVEL. Codigo paginavel NAO EXECUTA nesse IRQL. Logo
        //     ele NAO PODE estar na secao PAGE 鈥?tem que estar em .text.
        //
        // Entao rejeitar PAGE/INIT nao e' "melhorar o chute": e' exigir a
        // unica coisa que o callback pode ser.
        //
        //   PAGE = codigo paginavel (convencao da Microsoft)
        //   INIT = codigo discardavel, valido so' durante DriverEntry
        //          (esse daria 0xD1 igual, e ainda por cima por um motivo pior:
        //           a pagina nem existe mais depois do boot)
        // =================================================================
        {
            char n0 = (char)sec->Name[0], n1 = (char)sec->Name[1],
                 n2 = (char)sec->Name[2], n3 = (char)sec->Name[3];
            if (n0 >= 'a' && n0 <= 'z') n0 -= 32;
            if (n1 >= 'a' && n1 <= 'z') n1 -= 32;
            if (n2 >= 'a' && n2 <= 'z') n2 -= 32;
            if (n3 >= 'a' && n3 <= 'z') n3 -= 32;

            if (n0 == 'P' && n1 == 'A' && n2 == 'G' && n3 == 'E') return FALSE;
            if (n0 == 'I' && n1 == 'N' && n2 == 'I' && n3 == 'T') return FALSE;
        }

        return TRUE;
    }
    return FALSE;
}

// Comparacao case-insensitive de substring, sem depender de RtlUnicodeString
// (o BaseDllName nao e' necessariamente NULL-terminado).
static BOOLEAN KmWcsStrI(PCWSTR hay, PCWSTR needle)
{
    SIZE_T hn, nn, i, j;
    if (!hay || !needle) return FALSE;
    nn = wcslen(needle);
    if (nn == 0) return FALSE;

    // Limite de varredura: o nome e' curto. Evita ler pagina nao mapeada se o
    // UNICODE_STRING vier sem terminador.
    for (hn = 0; hn < 128 && hay[hn]; hn++) { }

    if (hn < nn) return FALSE;
    for (i = 0; i + nn <= hn; i++) {
        for (j = 0; j < nn; j++) {
            WCHAR a = hay[i + j], b = needle[j];
            if (a >= L'A' && a <= L'Z') a += 32;
            if (b >= L'A' && b <= L'Z') b += 32;
            if (a != b) break;
        }
        if (j == nn) return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// KM_S() 鈥?string cifrada em TEMPO DE COMPILACAO, decifrada no stack.
//
// ---------------------------------------------------------------------------
// ============ TRES TENTATIVAS. AS DUAS PRIMEIRAS ESTAVAM ERRADAS. ============
//
// TENTATIVA 1 鈥?cifra em RUNTIME.
//
//     static const WCHAR s[] = L"\\Driver\\Mouhid";
//     // ... uma funcao que "aplica XOR" numa copia
//
// Nao esconde NADA. Quem coloca o literal no .rdata e' o COMPILADOR, antes de
// qualquer codigo nosso rodar. A funcao de runtime so' produz uma copia
// cifrada 鈥?o original ja' esta' no binario. Eu tinha escrito exatamente isso,
// e o comentario dizia que estava resolvido.
//
// TENTATIVA 2 鈥?cifra em compilacao com `std::index_sequence` + /TP.
//
// Funcionava, mas exigia compilar o DRIVER INTEIRO como C++ (/TP). Consequencia
// imediata: C++ MANGLING em todo simbolo. O `DriverEntry` virou
// `?DriverEntry@@YAJPEAU_DRIVER_OBJECT@@...`, e oito funcoes de kernel
// declaradas a mao viraram LNK2019 (`?IoCreateDriver@@...`,
// `?MmCopyVirtualMemory@@...`). O ntoskrnl exporta esses nomes SEM mangling.
//
// Passar o driver todo para C++ por causa de uma string e' trocar um problema
// grande por um problema menor. Eu reconsiderei.
//
// TENTATIVA 3 鈥?o que esta' abaixo: cifra em compilacao, SEM template, em C.
// ---------------------------------------------------------------------------
//
// COMO FUNCIONA
//
// O texto nunca e' materializado: cada caractere e' cifrado INDIVIDUALMENTE no
// literal, com a chave dependendo da POSICAO. O que o compilador ve' e'
// `(wchar_t)(0x00 ^ 0x5A), (wchar_t)(0x5C ^ ...), ...` 鈥?um array de valores
// ja' transformados. Nao existe `L"\\Driver\\Mouhid"` em lugar nenhum do
// fonte, entao nao ha' como ele ir parar no .rdata.
//
// Custo declarado: uma chave derivada de posicao nao e' criptografia, e nao
// pretende ser. O objetivo e' nao entregar o texto a uma varredura de string 鈥?
// que e' o que roda automaticamente. Quem tem o binario e conhece o esquema,
// decifra; isso sempre foi verdade para ofuscacao de string.
//
// USO
//
//     static const WCHAR cifrado[] = KM_S(L"\\Driver\\Mouhid");
//     WCHAR nome[64];
//     KmUnmask(nome, cifrado, 64);
//     RtlInitUnicodeString(&drvName, nome);
//
// O `static const` no array cifrado faz ele ir para .rdata UMA vez (e nao ser
// reconstruido a cada chamada). O buffer decifrado e' do STACK.
// ---------------------------------------------------------------------------
#define KM_SAL 0x5Au
#define KM_K1  0x5Au
#define KM_K2  0x1Fu

// Um caractere cifrado, chave derivada da POSICAO.
#define KM_CC(c, i)  ((WCHAR)((WCHAR)(c) ^ (WCHAR)(KM_K1 + (int)(i) * KM_K2)))

// 16 caracteres explicitos. Nao da' para fazer laco aqui: o indice precisa ser
// CONSTANTE para o compilador calcular a chave na compilacao. Um laco em
// runtime reintroduziria a tentativa 1.
#define KM_S16(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p) \
    { KM_CC(a,0), KM_CC(b,1), KM_CC(c,2),  KM_CC(d,3),  \
      KM_CC(e,4), KM_CC(f,5), KM_CC(g,6),  KM_CC(h,7),  \
      KM_CC(i,8), KM_CC(j,9), KM_CC(k,10), KM_CC(l,11), \
      KM_CC(m,12),KM_CC(n,13),KM_CC(o,14), KM_CC(p,15), 0 }

// `\Driver\Mouhid` = 15 caracteres + NUL.
//
// A ORDEM DOS CARACTERES ESTA' CERTA: '\' 'D' 'r' 'i' 'v' 'e' 'r' '\'
// 'M' 'o' 'u' 'h' 'i' 'd' 鈥?e o ultimo slot e' o terminador.
#define KM_CIFRA_MOUHID  KM_S16('\\','D','r','i','v','e','r','\\', \
                                'M','o','u','h','i','d',0,0)

// ---------------------------------------------------------------------------
// KmUnmask 鈥?decifra de volta. Mesma chave, mesma derivacao por posicao.
//
// Para no primeiro cifrado que decifra para NUL. O limite `maxChars` existe
// porque este codigo roda no kernel: um buffer sem terminador viraria leitura
// de memoria arbitraria.
// ---------------------------------------------------------------------------
static void KmUnmask(WCHAR* destino, const WCHAR* cifrado, ULONG maxChars)
{
    ULONG i;
    for (i = 0; i + 1 < maxChars; i++) {
        WCHAR c = (WCHAR)(cifrado[i] ^ (WCHAR)(KM_K1 + (int)i * KM_K2));
        destino[i] = c;
        if (c == 0) return;
    }
    destino[i] = 0;
}

// Resolve o callback do Mouclass. Devolve TRUE so' com o par (device, callback)
// preenchido e VALIDADO.
//
// `g_MouseResolved` existe para nao repetir a varredura a cada movimento: o
// resultado (achar ou nao achar) e' definitivo para a vida do driver. Se nao
// achou uma vez, nao vai achar depois 鈥?o Mouclass nao carrega e descarrega.
// ---------------------------------------------------------------------------
// CONNECT_DATA 鈥?a estrutura onde o callback REALMENTE mora.
//
// Definida em <kbdmou.h>; redeclarada aqui para nao depender do header.
// 16 bytes no x64: dois ponteiros.
// ---------------------------------------------------------------------------
typedef struct _KM_CONNECT_DATA {
    PDEVICE_OBJECT ClassDeviceObject;   // +0x00  o device do Mouclass
    PVOID          ClassService;        // +0x08  MouseClassServiceCallback
} KM_CONNECT_DATA;

// ---------------------------------------------------------------------------
// Resolve o class service callback. TRUE so' com o par VALIDADO.
//
// ===========================================================================
// SEGUNDA REESCRITA. LEIA: AS DUAS PRIMEIRAS PROCURAVAM NO LUGAR ERRADO.
// ===========================================================================
//
// v1: varria o DeviceExtension do MOUCLASS procurando "um ponteiro para o
//     modulo". Achou DADO. -> 0xFC ATTEMPTED_EXECUTE_OF_NOEXECUTE_MEMORY.
//
// v2: exigiu secao EXECUTAVEL e NAO-PAGINAVEL. Achou uma dispatch routine.
//     -> 0xD1 DRIVER_IRQL_NOT_LESS_OR_EQUAL.
//
// Nenhum filtro conserta alvo errado. O callback NAO esta no Mouclass.
//
// O QUE A DOCUMENTACAO DIZ (e o que o projeto MouHidInputHook usa em producao):
//
//   O Mouclass manda IOCTL_INTERNAL_MOUSE_CONNECT com um CONNECT_DATA para
//   baixo na pilha. O driver de BAIXO (mouhid) guarda esse CONNECT_DATA no
//   PROPRIO DeviceExtension 鈥?e e' por ali que ele chama o callback:
//
//       PCONNECT_DATA cd = &MouHidDevice->DeviceExtension->ConnectData;
//       ((MOUSE_SERVICE_CALLBACK_ROUTINE)cd->ClassService)(
//           cd->ClassDeviceObject,       // 1o arg NAO e' o device do MouHid
//           DeviceExtension->InputDataStart,
//           DeviceExtension->InputDataEnd,
//           &InputDataConsumed);
//
// A VALIDACAO E' DE DOIS CAMPOS, e e' isso que faltava nas duas tentativas:
//
//   1. ClassDeviceObject == dev->AttachedDevice       (o device de cima)
//   2. ClassService aponta para secao executavel NAO-PAGINAVEL do driver DESSE
//      mesmo device
//
// Os dois JUNTOS. Nao e' "parece codigo": e' um par coerente, e a chance de
// falso positivo e' desprezivel.
//
// Fontes: documentacao da Microsoft (connect a class service callback) e o
// projeto MouHidInputHook (changeofpace), que resolve o CONNECT_DATA assim.
// ---------------------------------------------------------------------------
static BOOLEAN KmResolveMouseCallback(void)
{
    UNICODE_STRING   drvName;
    PDRIVER_OBJECT   hidDrv = NULL;
    PDEVICE_OBJECT   dev;
    NTSTATUS         st;
    ULONG            encontrados = 0;

    KM_CONNECT_DATA  melhor;
    ULONG_PTR        melhorBase = 0;
    ULONG            melhorTam  = 0;
    int              melhorNivel = -1;
    char             melhorSec[16] = {0};

    if (InterlockedCompareExchange(&g_MouseResolved, 1, 0) != 0)
        return (g_MouseCallback != NULL);

    melhor.ClassDeviceObject = NULL;
    melhor.ClassService      = NULL;

    // -----------------------------------------------------------------------
    // O NOME DO DRIVER DE BAIXO, cifrado.
    //
    // Era `L"\\Driver\\Mouhid"` em texto claro 鈥?a unica string do NOSSO
    // mecanismo de mouse que sobrava no .sys. (O resto dos textos de log sai no
    // link: com KM_LOG_HABILITADO=0 o KmLog e' no-op, e o linker descarta os
    // literais nao referenciados. Medido, nao suposto.)
    //
    // Quem faz `strings` no driver embutido e le' "Mouhid" sabe exatamente o
    // que o driver vai fazer com o mouse. E' identificador do MECANISMO, nao
    // de anti-cheat 鈥?e por isso e' o que mais importa tirar.
    //
    // O array cifrado e' `static const` (vai pro .rdata UMA vez, ja'
    // transformado). O texto decifrado vive no STACK e sai de escopo.
    // -----------------------------------------------------------------------
    {
        static const WCHAR cifrado[] = KM_CIFRA_MOUHID;
        WCHAR nomeMouhid[64];

        KmUnmask(nomeMouhid, cifrado, 64);
        RtlInitUnicodeString(&drvName, nomeMouhid);
    }
    st = ObReferenceObjectByName(&drvName, OBJ_CASE_INSENSITIVE, NULL, 0,
                                 *IoDriverObjectType, KernelMode, NULL, (PVOID*)&hidDrv);
    if (!NT_SUCCESS(st) || hidDrv == NULL) {
        KM_MOUSE_LOG("-- mouse: ObReferenceObjectByName(Mouhid) falhou\r\n");
        KM_MOUSE_LOG_HEX("   status = ", (ULONG)st);
        return FALSE;
    }

    KM_MOUSE_LOG("-- mouse: varrendo a PILHA do Mouhid (nao so' um nivel)\r\n");

    for (dev = hidDrv->DeviceObject; dev != NULL; dev = dev->NextDevice) {

        PDEVICE_OBJECT cadeia[8];
        int            n = 0;
        int            i, k;
        PDEVICE_OBJECT d;

        // ---------------------------------------------------------------------
        // SOBE A PILHA INTEIRA.
        //
        // Aqui esta' a correcao. Antes eu olhava so' `dev->AttachedDevice` 鈥?um
        // nivel. Numa VM com Guest Additions existe um FILTRO (VBoxMouse) nesse
        // meio:
        //
        //     mouhid  ->  VBoxMouse (filtro)  ->  mouclass
        //
        // E o filtro SUBSTITUI o CONNECT_DATA antes de passar pra baixo,
        // guardando a copia do ORIGINAL (da classe) no proprio extension. Ver a
        // documentacao da Microsoft sobre conectar class service e filter
        // service callback.
        //
        // Entao um nivel so' acha o callback do FILTRO. Foi por isso que tudo
        // deu [ok] e o cursor nunca andou: o pacote entrava numa fila que o
        // filtro nao repassa.
        //
        // Subindo a cadeia e aceitando qualquer nivel como ClassDeviceObject,
        // a gente acha os DOIS 鈥?e escolhe o de CIMA, que e' o da classe.
        // ---------------------------------------------------------------------
        d = dev;
        while (KmPtrOk(d) && n < 8) { cadeia[n++] = d; d = d->AttachedDevice; }

        if (n < 1) continue;

        // Loga a pilha: com estes numeros da' pra identificar quem e' quem pelo
        // tamanho (mouclass.sys ~ 0x10800; o filtro do VirtualBox e' bem maior).
        KM_MOUSE_LOG("-- mouse: pilha do device\r\n");
        for (i = 0; i < n; i++) {
            ULONG_PTR b = 0; ULONG tSz = 0;
            if (KmPtrOk(cadeia[i]) && KmPtrOk(cadeia[i]->DriverObject)) {
                b    = (ULONG_PTR)cadeia[i]->DriverObject->DriverStart;
                tSz  = cadeia[i]->DriverObject->DriverSize;
            }
            KM_MOUSE_LOG_HEX("   nivel ", (ULONG)i);
            KM_MOUSE_LOG_HEX64("      device      = ", (ULONG_PTR)cadeia[i]);
            KM_MOUSE_LOG_HEX64("      DriverStart = ", b);
            KM_MOUSE_LOG_HEX("      DriverSize  = ", tSz);
        }

        // ---------------------------------------------------------------------
        // Procura CONNECT_DATA no extension de CADA nivel da pilha.
        //
        // ClassDeviceObject pode ser QUALQUER device da cadeia: no caso sem
        // filtro e' o de cima; com filtro, o do meio guarda o par da classe.
        // Quem decide e' o indice 鈥?quanto MAIOR, mais perto da classe.
        // ---------------------------------------------------------------------
        for (i = 0; i < n; i++) {

            PUCHAR ext = KmPtrOk(cadeia[i]) ? (PUCHAR)cadeia[i]->DeviceExtension : NULL;
            ULONG  off;

            if (ext == NULL) continue;

            for (off = 0; off + sizeof(KM_CONNECT_DATA) <= 0x400; off += sizeof(PVOID)) {

                KM_CONNECT_DATA cd;
                PDEVICE_OBJECT  alvo = NULL;
                int             alvoNivel = -1;
                ULONG_PTR       b2 = 0;
                ULONG           t2 = 0;
                char            nomeSec[16] = {0};

                // LER O EXTENSION E' LER MEMORIA DE OUTRO DRIVER: a faixa
                // pode atravessar uma pagina nao mapeada. Sem esta checagem,
                // ext+0x400 num extension de 0x100 = KMODE_EXCEPTION_NOT_HANDLED.
                if (!KmPtrOk(ext + off) || !KmPtrOk(ext + off + sizeof(PVOID))) continue;
                cd.ClassDeviceObject = *(PDEVICE_OBJECT*)(ext + off);
                cd.ClassService      = *(PVOID*)(ext + off + sizeof(PVOID));
                for (k = 0; k < n; k++) {
                    if (cadeia[k] == cd.ClassDeviceObject) { alvo = cadeia[k]; alvoNivel = k; break; }
                }
                if (alvo == NULL) continue;

                if (!KmPtrOk(alvo) || !KmPtrOk(alvo->DriverObject)) continue;
                b2 = (ULONG_PTR)alvo->DriverObject->DriverStart;
                t2 = alvo->DriverObject->DriverSize;
                if (b2 == 0 || t2 == 0) continue;

                // CAMPO 2: codigo executavel e NAO-PAGINAVEL do driver DELE.
                if (!KmEhExecutavel(b2, t2, (ULONG_PTR)cd.ClassService, nomeSec)) {
                    KM_MOUSE_LOG("-- mouse: NEAR-MISS (device bateu, ClassService reprovado)\r\n");
                    KM_MOUSE_LOG_HEX("   ext do nivel ", (ULONG)i);
                    KM_MOUSE_LOG_HEX("   devExt + ", off);
                    KM_MOUSE_LOG_HEX64("   ClassService = ", (ULONG_PTR)cd.ClassService);
                    KM_MOUSE_LOG_HEX64("   base         = ", b2);
                    KM_MOUSE_LOG_HEX("   tam          = ", t2);
                    KM_MOUSE_LOG("   secao        = ");
                    if (nomeSec[0]) { KM_MOUSE_LOG(nomeSec); } else { KM_MOUSE_LOG("(fora do modulo)"); }
                    KM_MOUSE_LOG("\r\n");
                    continue;
                }

                encontrados++;
                KM_MOUSE_LOG("-- mouse: CONNECT_DATA valido\r\n");
                KM_MOUSE_LOG_HEX("   ext do nivel  = ", (ULONG)i);
                KM_MOUSE_LOG_HEX("   devExt + ", off);
                KM_MOUSE_LOG_HEX("   aponta p/ nivel = ", (ULONG)alvoNivel);
                KM_MOUSE_LOG_HEX64("   ClassDeviceObject   = ", (ULONG_PTR)cd.ClassDeviceObject);
                KM_MOUSE_LOG_HEX64("   ClassService        = ", (ULONG_PTR)cd.ClassService);
                KM_MOUSE_LOG_HEX("   RVA do ClassService = ", (ULONG)((ULONG_PTR)cd.ClassService - b2));
                KM_MOUSE_LOG("   secao               = ");
                KM_MOUSE_LOG(nomeSec);
                KM_MOUSE_LOG("\r\n");

                // FICA COM O DE NIVEL MAIS ALTO (mais perto da classe).
                if (alvoNivel > melhorNivel) {
                    melhorNivel     = alvoNivel;
                    melhor          = cd;
                    melhorBase      = b2;
                    melhorTam       = t2;
                    for (k = 0; k < 16; k++) melhorSec[k] = nomeSec[k];
                }
            }
        }
    }

    if (melhorNivel < 0) {
        KM_MOUSE_LOG("-- mouse: NENHUM CONNECT_DATA valido na pilha do Mouhid.\r\n");
        // -------------------------------------------------------------------
        // DEREF DA REFERENCIA DO DRIVER OBJECT.
        //
        // O `ObReferenceObjectByName` la' em cima PEGOU uma referencia no driver
        // object do Mouhid. Este return (e o de baixo) precisa solta-la, senao a
        // referencia fica presa para sempre: o Mouhid nunca mais pode ser
        // descarregado e o contador do object manager vaza uma unidade por
        // carga do nosso driver. Nada falha, nada loga 鈥?so' vaza.
        //
        // A auditoria externa pegou este caminho. Era o unico `ObReference*`
        // sem `ObDereference*` no arquivo.
        // -------------------------------------------------------------------
        ObDereferenceObject(hidDrv);
        return FALSE;
    }

    // -----------------------------------------------------------------------
    // REFERENCIA DO DEVICE DE CLASSE 鈥?o par que faltava.
    //
    // `g_MouseDevice` era um ponteiro CRU para o device object do mouclass,
    // guardado sem referencia nenhuma. Se alguma coisa removesse aquele device
    // (mouse desinstalado, dispositivo removido em runtime, filtro descarregando
    // em cima dele), o valor continuaria sendo um numero plausivel e a chamada
    // do callback iria para memoria liberada 鈥?bugcheck imediato.
    //
    // Com a referencia, o device object nao pode ser liberado enquanto a gente
    // nao soltar, e o par (referencia aqui, deref no KmShutdownDevice) e' o que
    // torna a chamada segura DEPOIS que esta funcao sai.
    //
    // O que a gente chama e' o service callback do MOUCLASS, via o device de
    // classe; o mouclass e' driver de boot e nao descarrega. O driver object do
    // Mouhid (Mouhid) nao precisa continuar referenciado depois da varredura 鈥?
    // e e' ele que a gente solta logo abaixo.
    // -----------------------------------------------------------------------
    {
        const NTSTATUS stRef = ObReferenceObjectByPointer(
            melhor.ClassDeviceObject, 0, *IoDeviceObjectType, KernelMode);

        if (!NT_SUCCESS(stRef)) {
            KM_MOUSE_LOG("-- mouse: ObReferenceObjectByPointer FALHOU - callback NAO armado\r\n");
            KM_MOUSE_LOG_HEX("   status = ", (ULONG)stRef);
            ObDereferenceObject(hidDrv);
            return FALSE;
        }
        g_MouseDeviceRefd = TRUE;
    }

    // Solta a referencia do driver object: a varredura acabou, e o que interessa
    // (o device de classe) agora tem a referencia DELE.
    ObDereferenceObject(hidDrv);

    g_MouseCallback = (KM_MOUSE_SERVICE_CALLBACK)melhor.ClassService;
    g_MouseDevice   = melhor.ClassDeviceObject;

    KM_MOUSE_LOG("-- mouse: escolhido o de NIVEL MAIS ALTO\r\n");
    KM_MOUSE_LOG_HEX("   nivel        = ", (ULONG)melhorNivel);
    KM_MOUSE_LOG_HEX64("   ClassDeviceObject = ", (ULONG_PTR)melhor.ClassDeviceObject);
    KM_MOUSE_LOG_HEX64("   ClassService      = ", (ULONG_PTR)melhor.ClassService);
    KM_MOUSE_LOG_HEX64("   base              = ", melhorBase);
    KM_MOUSE_LOG_HEX("   tam               = ", melhorTam);
    KM_MOUSE_LOG("   secao             = ");
    KM_MOUSE_LOG(melhorSec);
    KM_MOUSE_LOG("\r\n");
    KM_MOUSE_LOG_HEX("   CONNECT_DATA validos no total = ", encontrados);
    return TRUE;
}

// ===========================================================================
// KmInjectMouse - movimento RELATIVO pela pilha do Mouclass.
//
// `KeRaiseIrql(DISPATCH_LEVEL)` NAO e' opcional: o Mouclass espera ser chamado
// nesse nivel. Sem subir, o bugcheck e' imediato (IRQL_NOT_LESS_OR_EQUAL) —
// ja' aconteceu neste projeto.
// ===========================================================================
static NTSTATUS KmInjectMouse(int dx, int dy)
{
    KM_MOUSE_INPUT_DATA data;
    KIRQL               irqlAntigo;
    ULONG               consumidos = 0;

    if (g_MouseCallback == NULL || g_MouseDevice == NULL) {
        if (!KmResolveMouseCallback()) return STATUS_PROCEDURE_NOT_FOUND;
        if (g_MouseCallback == NULL || g_MouseDevice == NULL) return STATUS_PROCEDURE_NOT_FOUND;
    }

    RtlZeroMemory(&data, sizeof(data));
    data.UnitId  = 0;
    data.Flags   = 0;
    data.Buttons = 0;          // so' movimento; botao nao e' nosso
    data.LastX   = dx;
    data.LastY   = dy;

    // -----------------------------------------------------------------
    // ULTIMA CHECAGEM ANTES DE SALTAR PARA O CALLBACK.
    //
    // O `g_MouseCallback` sai de uma VARREDURA por assinatura em memoria de
    // outro driver. A varredura ja' exige secao executavel e nao-paginavel,
    // mas aqui e' o ultimo ponto onde da' para recusar sem consequencia:
    // chamar um endereco invalido em DISPATCH_LEVEL e' bugcheck imediato,
    // sem excecao tratavel.
    // -----------------------------------------------------------------
    if (g_MouseCallback == NULL || !KmPtrOk((const void*)g_MouseCallback)) {
        g_MouseCallback = NULL;
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    KeRaiseIrql(DISPATCH_LEVEL, &irqlAntigo);
    g_MouseCallback(g_MouseDevice, &data, &data + 1, &consumidos);
    KeLowerIrql(irqlAntigo);

    return STATUS_SUCCESS;
}

// Solta a referencia do device de classe (se houver). Chamado no shutdown.
static void KmMouseRelease(void)
{
    if (g_MouseDevice != NULL && g_MouseDeviceRefd) {
        ObDereferenceObject(g_MouseDevice);
        g_MouseDeviceRefd = FALSE;
    }
    g_MouseDevice   = NULL;
    g_MouseCallback = NULL;
}
