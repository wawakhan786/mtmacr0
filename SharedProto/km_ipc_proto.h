#pragma once
// ===========================================================================
// km_ipc_proto.h — PROTOCOLO do protótipo de comunicação kernel <-> user-mode
//
// Seção SEM NOME + dois eventos SEM NOME. O único ponto de encontro é uma chave
// de registro com nome derivado do seed, apagada assim que o boot termina.
//
// Este header é a fronteira: entra nos DOIS lados, e nada aqui depende do
// cheat, do jogo ou do driver de produção.
//
// ---------------------------------------------------------------------------
// REGRAS DE OURO (cada uma existe por um erro concreto)
//
// 1. POD, SEM C++. É incluído por um driver em C puro. Sem `namespace`, sem
//    `bool`, sem método. Tipos com largura explícita e checagem de tamanho em
//    tempo de compilação.
//
// 2. `volatile` NOS CAMPOS DE SINAL. `volatile` sozinho NÃO ordena memória
//    entre núcleos: cada lado chama barreira ANTES de publicar e DEPOIS de
//    observar. Ver RELATORIO_ARQUITETURA.md §1.
//
// 3. O PAYLOAD TEM UM DONO POR VEZ. O user-mode escreve `Req` e a metade de
//    entrada de `Data`; o driver escreve `Resp` e a metade de saída. Ninguém
//    escreve na metade do outro: elimina a corrida de DADO e deixa só a de
//    VISIBILIDADE, que as barreiras resolvem.
//
// 4. VERSÃO NO CABEÇALHO, checada ANTES de qualquer offset. Um user-mode de
//    build diferente não pode "quase funcionar".
//
// 5. NENHUM OFFSET IMPLÍCITO. Todo acesso a `Data` passa por `DataOff` /
//    `PayloadOff`, e o validador confere `off + size <= limite`. Foi um
//    overflow de pool com tamanho controlado pelo chamador que derrubou a
//    máquina 6 vezes no projeto principal.
// ===========================================================================

typedef unsigned char       kmp_u8;
typedef unsigned short      kmp_u16;
typedef unsigned int        kmp_u32;
typedef unsigned long long  kmp_u64;
typedef long long           kmp_i64;

#define KMP_MAGIC      0x4B4D4950u   // 'K''M''I''P'
#define KMP_VERSION    5u            // 5 = + Sig por lote (autenticacao) + watchdog
                                    //    4 = + KeyMask no Hdr (keyread via kbdclass)
                                    //    3 = objetos SEM NOME + rendezvous por registro

// ---------------------------------------------------------------------------
// KEYMASK — estado das teclas publicado pelo DRIVER no cabecalho.
//
// Bit 0 (KMP_KM_ALT) e bit 2 (KMP_KM_SHIFT) espelham os INDICES do HotkeyHook
// user-mode (KEY_LEFT_ALT = 0, KEY_SHIFT = 2): o bit `i` vale 1 quando a tecla
// do indice `i` esta' PRESSIONADA. Os bits sobrantes (1,3,4..) ficam a
// disposicao se o keyread crescer; o bit 16 e' a FLAG DE ESTADO, nao uma tecla.
//
// Bit 16 (KMP_KM_KROK): o keyread de kernel ESTA INSTALADO nesta carga. O
// user-mode confere este bit antes de confiar no mask; driver antigo (v3) nem
// tem o campo — por isso o bump de versao vai junto.
//
// O callback do kbdclass pode rodar em DISPATCH_LEVEL: ele so' toca um LONG
// interlocked dentro da imagem (nonpaged) e faz KeSetEvent. A PUBLICACAO na
// secao acontece no work item do servidor (PASSIVE_LEVEL), que reescreve o
// campo em todo wake — inclusive nos timeouts de 100 ms. Ou seja: a latencia
// maxima da tecla ate' aparecer aqui e' ~100 ms, e o valor NUNCA esta' "entre"
// duas escritas (u32 alinhado, escrita atomic por construtor).
// ---------------------------------------------------------------------------
#define KMP_KM_ALT      0x1u
#define KMP_KM_SHIFT    0x4u
#define KMP_KM_MASK     0x0000FFFFu
#define KMP_KM_KROK     0x00010000u

// ---------------------------------------------------------------------------
// RENDEZVOUS — como os dois lados se encontram SEM nome de objeto nenhum
//
// (A v2 deste header assumia seção e eventos NOMEADOS, que aparecem em
//  \BaseNamedObjects e \KernelObjects e são listáveis por qualquer processo.
//  A v3 usa o desenho do RegCommState: objetos SEM NOME, e o único ponto de
//  encontro é uma CHAVE DE REGISTRO com nome derivado do seed.)
//
// ORDEM (não há polling e não há nome de objeto):
//
//   1. user-mode: cria a seção e os DOIS eventos SEM NOME
//                 (CreateFileMapping(INVALID_HANDLE_VALUE) + CreateEvent(NULL,..))
//   2. user-mode: cria a chave derivada e grava 5 valores REG_QWORD:
//                   V0 = Signature (mix do seed)     V3 = handle do ReqEvt
//                   V1 = Pid do processo             V4 = handle do RspEvt
//                   V2 = handle da seção
//   3. user-mode: mapeia o driver (kdmapper, com o seed em param2)
//   4. driver (DriverEntry, já em contexto de SISTEMA): deriva o MESMO caminho,
//      abre a chave, lê os 5 valores, abre o processo por Pid e DUPLICA os 3
//      handles para o kernel. Depois APAGA a chave.
//   5. daqui pra frente existem 3 handles no kernel e 3 no processo. Nenhum
//      nome, nenhum device, nenhum IOCTL, nenhum IRP.
//
// Os NOMES DOS VALORES também são derivados do seed, então o .sys não tem
// nenhum literal de nome de valor para uma varredura de strings achar.
// ---------------------------------------------------------------------------
#define KMP_REG_ROOT        L"\\Registry\\Machine\\SOFTWARE\\Classes\\CLSID\\"
#define KMP_REG_GUID_CHARS  32     // 32 hex = GUID sem as chaves
#define KMP_VALUE_NAME_LEN  16     // 16 hex por nome de valor
#define KMP_VALUE_COUNT     5

#define KMP_V_SIGNATURE  0
#define KMP_V_PID        1
#define KMP_V_SECTION    2
#define KMP_V_REQEVT     3
#define KMP_V_RSPEVT     4

// Mistura usada por AMBOS os lados. Mudar aqui muda o nome derivado nos dois
// lugares ao mesmo tempo — que é o que se quer.
#define KMP_MIX 0x9E3779B97F4A7C15ULL
#define KMP_MIX_SIG   0x300u   // kind da assinatura
#define KMP_MIX_KEY   0x100u   // kind do caminho da chave
#define KMP_MIX_VAL   0x200u   // kind base dos nomes de valor

// ---------------------------------------------------------------------------
// MÁQUINA DE ESTADOS DO CICLO DE VIDA (do RegCommState)
//
//   INIT -> WAITING -> BUSY -> WAITING -> ... -> STOPPING -> DONE
//
// Regras: só a thread do servidor escreve BUSY/WAITING; só o caminho de
// shutdown escreve STOPPING; DONE é terminal — depois dele o servidor já saiu e
// nenhum handle é usado, que é a pré-condição para desmapear.
// ---------------------------------------------------------------------------
#define KMP_LC_INIT      0u
#define KMP_LC_WAITING   1u
#define KMP_LC_BUSY      2u
#define KMP_LC_STOPPING  3u
#define KMP_LC_DONE      4u

// ---------------------------------------------------------------------------
// PEDIDO DE PARADA — COMO O USER MODE DERRUBA O SERVIDOR DO KERNEL.
//
// O servidor vive num `KeWaitForSingleObject(ReqEvent)`. Sem um sinal
// ESPECIFICO, a unica saida era o driver descarregar — e num mapeamento
// manual nao existe unload: a thread ficaria viva para sempre, esperando num
// evento que ninguem mais sinaliza. Uma thread de kernel por sessao.
//
// O sinal e' o proprio `Req.Magic`: o cliente escreve KMP_MAGIC_STOP e
// sinaliza; o laco acorda, ve' que nao e' um lote valido e sai. Nao precisa de
// op nova, nao precisa de campo novo, e um lote legitimo nunca tem outro
// Magic (o cliente sempre o repoe em LimparReq).
// ---------------------------------------------------------------------------
#define KMP_MAGIC_STOP   0x504F5453u   // "STOP"

// ---------------------------------------------------------------------------
// TAMANHOS
// ---------------------------------------------------------------------------
#define KMP_MAX_OPS            64
#define KMP_DATA_BYTES         (256 * 1024)
#define KMP_TIMEOUT_MS_DEFAULT  2000

// ---------------------------------------------------------------------------
// TETO DE TAXA E WATCHDOG — as duas protecoes que faltavam (KEVENT §13).
//
// TETO: contagem por segundo, TELEMETRIA (nada e' descartado). Um lote acima do
// esperado incrementa Hdr.ThrottledBatches — quem olha o numero ve um cliente
// que virou spin, em vez de descobrir depois que o kernel estava a 100%.
//
// WATCHDOG: antes a morte do cliente era invisivel — o servidor ficava vivo ate
// o fim da sessao sem que ninguem o derrubasse ("shutdown depende do cliente").
// Agora: sem nenhum lote por KMP_WDOG_MS apos a primeira atividade, o servidor
// para e o driver executa a propria limpeza (mesma ordem do shutdown classico).
// 60 s e' folga larga: o loop de memoria manda lote a cada tick, inclusive
// no lobby, entao o watchdog so' acorda quando o cliente morreu de verdade.
// ---------------------------------------------------------------------------
#define KMP_MAX_OPS_PER_SEC    20000u
#define KMP_WDOG_MS            60000u

// ---------------------------------------------------------------------------
// OPERAÇÕES — só o que o protótipo precisa para medir transporte
//
// NÃO existe leitura nem escrita em memória de outro processo. O protótipo é de
// TRANSPORTE; a operação privilegiada entra depois, se o transporte provar que
// vale. É o que permite carregá-lo numa VM sem que ele tenha poder nenhum.
// ---------------------------------------------------------------------------
#define KMP_OP_NOP        0u   // não faz nada: mede o custo puro do round-trip
#define KMP_OP_ECHO       1u   // copia Size bytes de Data[PayloadOff] para a saída
#define KMP_OP_FILL       2u   // preenche Size bytes com o byte em Out0
#define KMP_OP_SUM        3u   // soma Size bytes de Data[PayloadOff] -> Out0

// Operacoes que o CHEAT precisa (o prototipo so' somava bytes ate' agora).
#define KMP_OP_READ       4u   // le Size bytes de Address no processo Pid -> Data
#define KMP_OP_GET_BASE   5u   // base do modulo principal de Pid -> Out0
#define KMP_OP_MOUSE      6u   // injecao de mouse: dx em Out0, dy em Out1
#define KMP_OP_EXPOSE     7u   // diagnostico: preenche Data com o que o AC veria

// ---------------------------------------------------------------------------
// STATUS
//
// NTSTATUS não existe no lado user-mode sem arrastar header de kernel, então o
// campo é um u32 com os valores que o protocolo realmente devolve.
// ---------------------------------------------------------------------------
#define KMP_OK                    0x00000000u
#define KMP_ERR_BAD_VERSION       0xC0000059u   // STATUS_REVISION_MISMATCH
#define KMP_ERR_BAD_LENGTH        0xC0000023u   // STATUS_BUFFER_TOO_SMALL
#define KMP_ERR_BAD_PARAMETER     0xC000000Du   // STATUS_INVALID_PARAMETER
#define KMP_ERR_NOT_IMPLEMENTED   0xC0000002u   // STATUS_NOT_IMPLEMENTED
#define KMP_ERR_TIMEOUT           0xC0000120u   // STATUS_IO_TIMEOUT
#define KMP_ERR_SHUTDOWN          0xC0000141u   // STATUS_DELETE_PENDING
// Assinatura do lote nao confere (alguem escreveu na secao sem ter a chave).
#define KMP_ERR_BAD_SIGNATURE     0xC000BADBu

// ---------------------------------------------------------------------------
// ASSINATURA POR LOTE — a autenticacao que o protocolo nao tinha.
//
// Antes qualquer processo com uma view da secao podia mandar lotes: o unice
// ponto de encontro (o slot de boot) era o nome do jogo inteiro, e o transporte
// em si dizia "falei com o kernel" sem provar quem falou. A assinatura fecha
// isto sem trocar nenhum campo de lugar:
//
//   chave  = KmpSessKey(Hdr.Signature)   (a Signature ja e' unica por carga)
//   Sig    = FNV-1a( Seq, Count, PayloadUsed, ops... ) ^ chave
//
// O user-mode calcula DEPOIS de escrever os campos do Req e ANTES de publicar
// `Seq`; o driver recalcula ao acordar e qualquer byte trocado rejeita o lote
// (GlobalStatus = KMP_ERR_BAD_SIGNATURE). E FNV por proposito: barato o bastante
// para rodar por lote a 144 Hz, e NAO precisa ser criptografica — quem quer
// falsificar a assinatura tambem teria que adivinhar a Signature da carga.
//
// `static __inline` (nao `static inline`): compila igual em C89 do WDK e no C++
// do app; cada TU pega sua copia, sem custo de link. A funcao e' a MESMA nos
// dois lados por construicao: so' existe aqui, num header unico.
// ---------------------------------------------------------------------------
#define KMP_FNV_OFF    0xcbf29ce484222325ULL
#define KMP_FNV_PRIME  0x100000001b3ULL

static __inline kmp_u64 KmpSessKey(kmp_u64 signature)
{
    kmp_u64 k = signature ^ KMP_MIX;
    k ^= k >> 33; k *= 0xFF51AFD7ED558CCDULL;
    k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ULL;
    return k ^ (k >> 29);
}

static __inline void KmpSigFold(kmp_u64* h, kmp_u64 v)
{
    *h ^= v;
    *h = (*h) * KMP_FNV_PRIME;
}

// PREENCHER depois que todos os campos do lote estao escritos. O driver usa a
// MESMA funcao na checagem — divergencia nos dois lados seria bug invisivel.
// Os campos Out0/Out1 ENTRAM no hash: o MOUSE usa eles como entrada (dx/dy).
// (definida logo depois do typedef de KMP_REQ — precisa da struct)

#pragma pack(push, 8)

// ---------------------------------------------------------------------------
// UMA OPERAÇÃO — 64 bytes
// ---------------------------------------------------------------------------
typedef struct _KMP_OP {
    kmp_u32 Kind;         // in  KMP_OP_*
    kmp_u32 Status;       // out KMP_OK / KMP_ERR_*
    kmp_u32 Pid;          // in  reservado (0 = próprio processo de teste)
    kmp_u32 Flags;        // in  reservado (0)
    kmp_u64 Address;      // in  reservado (0 por enquanto)
    kmp_u64 Size;         // in  bytes
    kmp_u64 PayloadOff;   // in  offset em Data (entrada)
    kmp_u64 DataOff;      // out offset em Data (saída)
    kmp_u64 Out0;         // out resultado escalar (SUM / FILL)
    kmp_u64 Out1;         // out reservado
} KMP_OP, *PKMP_OP;

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// SLOT DE BOOT — o rendezvous SEM REGISTRO.
//
// ==================== POR QUE ISTO EXISTE (21/10/2026) ====================
//
// Ate aqui o cliente criava uma chave em
// `HKLM\SOFTWARE\Classes\CLSID\<32 hex>` com cinco valores REG_QWORD, o driver
// lia e apagava. Funcionava — e era uma IMPRESSAO DIGITAL:
//
//     qualquer driver pode registrar CmRegisterCallback e ver
//     "Classes\CLSID\<aleatorio>: 5 REG_QWORD, apagada em ~1 ms"
//
// Esse padrao e' identico para TODO usuario do build. Nao identifica a pessoa:
// identifica a FAMILIA — que e' exatamente o que da' ban em massa.
//
// O slot resolve sem rendezvous: o cliente tem os bytes do driver ANTES de
// mapear, entao escreve os handles DENTRO da imagem, no campo estatico
// `g_BootSlot`. O driver le' a si mesmo.
//
// Some: HKLM, derivacao de nome de chave, ZwOpenKey/ZwDeleteKey, e a exigencia
// de rodar elevado para gravar a chave.
// Fica: a secao sem nome (que ja' era o unico objeto de verdade).
// ===========================================================================
#define KMP_BOOT_MAGIC 0x534C4F54u   /* 'SLOT' little-endian: 54 4F 4C 53 */

typedef struct _KMP_BOOT_SLOT {
    kmp_u32 Magic;       // out KMP_BOOT_MAGIC (o cliente procura ESTE valor)
    kmp_u32 Pid;         // in  processo dono dos handles
    kmp_u64 Signature;   // in  prova de que a sessao e' esta
    kmp_u64 Section;     // in  handle da secao
    kmp_u64 ReqEvent;    // in  handle de requisicao
    kmp_u64 RspEvent;    // in  handle de resposta
} KMP_BOOT_SLOT;          // 40 bytes, sem padding

// LOTE — escrito pelo USER MODE
//
// `Seq` é o campo de sinal:
//   user-mode: escreve tudo, barreira, DEPOIS publica `Seq` (volatile);
//   driver   : barreira ao acordar, lê `Seq`, compara com o último processado.
//              Igual = evento espúrio, ignora.
// ---------------------------------------------------------------------------
typedef struct _KMP_REQ {
    kmp_u32 Magic;         // in  KMP_MAGIC
    kmp_u32 Version;       // in  KMP_VERSION
    volatile kmp_u32 Seq;  // in  publicado por ÚLTIMO
    kmp_u32 Count;         // in  ops válidas (<= KMP_MAX_OPS)
    kmp_u64 Sig;           // in  KmpReqSig() — escrito ANTES do Seq
    kmp_u64 PayloadUsed;   // in  bytes usados em Data pelo user-mode
    KMP_OP  Ops[KMP_MAX_OPS];
} KMP_REQ, *PKMP_REQ;

// Assinatura FNV do lote (ver comentario anterior ao struct):
// PREENCHER depois que todos os campos do lote estao escritos. O driver usa a
// MESMA funcao na checagem — divergencia nos dois lados seria bug invisivel.
static __inline kmp_u64 KmpReqSig(const PKMP_REQ r, kmp_u64 key)
{
    kmp_u64 h = KMP_FNV_OFF ^ key;
    kmp_u32 i;
    KmpSigFold(&h, (kmp_u64)r->Seq);
    KmpSigFold(&h, (kmp_u64)r->Count);
    KmpSigFold(&h, r->PayloadUsed);
    for (i = 0; i < r->Count; ++i) {
        const KMP_OP* op = &r->Ops[i];
        KmpSigFold(&h, (kmp_u64)op->Kind);
        KmpSigFold(&h, (kmp_u64)op->Pid);
        KmpSigFold(&h, (kmp_u64)op->Flags);
        KmpSigFold(&h, op->Address);
        KmpSigFold(&h, op->Size);
        KmpSigFold(&h, op->PayloadOff);
        KmpSigFold(&h, op->Out0);
        KmpSigFold(&h, op->Out1);
    }
    return h;
}

// ---------------------------------------------------------------------------
// RESPOSTA — escrita pelo DRIVER
//
// O resultado de cada op vem em `Ops[i]`, espelhando o índice do pedido. O
// driver NÃO escreve no Req: o Req é do user-mode e fica intacto, o que deixa o
// benchmark reenviar o mesmo pedido sem reescrever nada.
// ---------------------------------------------------------------------------
typedef struct _KMP_RESP {
    kmp_u32 Magic;         // out KMP_MAGIC
    kmp_u32 Version;       // out KMP_VERSION
    volatile kmp_u32 AckSeq;   // out = Req.Seq, escrito por ÚLTIMO
    kmp_u32 Count;         // out ops processadas
    kmp_u32 GlobalStatus;  // out status do LOTE
    kmp_u32 _pad;
    kmp_u64 DataUsed;      // out bytes válidos em Data (saída)
    kmp_u64 OpsProcessed;  // out acumulado desde o boot do driver
    kmp_u64 OpsFailed;     // out acumulado
    kmp_i64 CyclesLast;    // out ciclos de CPU do último lote (telemetria)
    KMP_OP  Ops[KMP_MAX_OPS];
} KMP_RESP, *PKMP_RESP;

// ---------------------------------------------------------------------------
// CABEÇALHO DA SEÇÃO
//
// `Ready` é o handshake de boot: o user-mode espera 1 antes de mandar o primeiro
// lote. Sem isso, o primeiro `submit` pode acontecer antes de o driver mapear a
// view — e o sintoma seria "o driver não carregou", que é o lugar errado de
// procurar.
// ---------------------------------------------------------------------------
typedef struct _KMP_HDR {
    kmp_u32 Magic;             // out KMP_MAGIC
    kmp_u32 Version;           // out KMP_VERSION
    volatile kmp_u32 Ready;    // out 1 quando o boot terminou
    volatile kmp_u32 Ativo;    // out 0 quando o driver está desligando
    kmp_u64 Signature;         // out valor por carga: prova que é ESTA sessão
    kmp_u64 TotalBytes;        // out sizeof(KMP_SHARED)
    kmp_u64 ReqOffset;         // out offset de KMP_REQ
    kmp_u64 RespOffset;        // out offset de KMP_RESP
    kmp_u64 DataOffset;        // out offset de Data
    kmp_u64 DataBytes;         // out KMP_DATA_BYTES
    // Base/tamanho da imagem mapeada. Publicados pelo driver para o MAPPER
    // poder chamar MmFreeIndependentPages no fim da sessao — sem isso,
    // as paginas do driver ficam residentes para sempre numa sessao KEVENT.
    kmp_u64 ImageBase;         // out base do mapeamento (0 = nao informado)
    kmp_u64 ImageSize;         // out tamanho da imagem (SizeOfImage)
    // Keyread (v4): estado ALT/SHIFT capturado pelo kbdclass no kernel,
    // publicado pelo work item do servidor. Ver KMP_KM_* acima.
    volatile kmp_u32 KeyMask;  // out bits de tecla + flag KMP_KM_KROK
    kmp_u32       _krPad;
    // Telemetria de protecao (v5): quantas vezes a assinatura do lote NAO
    // conferiu e quantos lotes passaram do teto de taxa. So' leitura para o
    // cliente (aba DEBUG): numero > 0 em sessao normal e' um aviso.
    volatile kmp_u32 BadSigBatches;
    volatile kmp_u32 ThrottledBatches;
    volatile kmp_u32 WatchdogFires;
    kmp_u32       _vPad;
} KMP_HDR, *PKMP_HDR;

// ---------------------------------------------------------------------------
// A SEÇÃO
//
// Os offsets ficam no cabeçalho em vez de `offsetof` calculado nos dois lados:
// um `pack` diferente produziria offsets diferentes e o bug seria invisível.
// O user-mode usa os offsets que o KERNEL publicou, e confere contra os seus.
// ---------------------------------------------------------------------------
typedef struct _KMP_SHARED {
    KMP_HDR  Hdr;
    KMP_REQ  Req;
    KMP_RESP Resp;
    // Data começa em Hdr.DataOffset e tem Hdr.DataBytes bytes.
} KMP_SHARED, *PKMP_SHARED;

#pragma pack(pop)

#define KMP_SHARED_BYTES   (sizeof(KMP_SHARED) + KMP_DATA_BYTES)

// ---------------------------------------------------------------------------
// CHECAGEM DE LAYOUT EM TEMPO DE COMPILAÇÃO
//
// `typedef char [cond ? 1 : -1]` e não `static_assert`: `_Static_assert` é C11
// e o `cl /TC` deste build não usa /std:c11, e `static_assert` como macro vive
// em <assert.h>, que header de driver não inclui. A array de tamanho negativo é
// verificada em C89, C11, C++ e no cl do WDK, sem dependência nenhuma.
//
// (O primeiro rascunho usou `_Static_assert` e NÃO compilava em C. Só apareceu
//  porque o header foi compilado de verdade nos dois modos.)
// ---------------------------------------------------------------------------
#define KMP_ASSERT_SIZE(tipo, esperado) \
    typedef char kmp_size_check_##tipo[ (sizeof(tipo) == (esperado)) ? 1 : -1 ]

// 4 x u32 (16) + 8 x u64 (64) = 80  (ImageBase/ImageSize entraram em 20/10)
KMP_ASSERT_SIZE(KMP_OP,   64);
// 4 x u32 (16) + Sig (8) + PayloadUsed (8) + ops = 32 + 4096 = 4128  (Sig na v5)
KMP_ASSERT_SIZE(KMP_REQ,  32 + KMP_MAX_OPS * 64);
// 6 x u32 (24) + 4 x u64 (32) + ops = 56 + 4096 = 4152
KMP_ASSERT_SIZE(KMP_RESP, 56 + KMP_MAX_OPS * 64);
// 4 x u32 (16) + 8 x u64 (64) + KeyMask/_krPad (8) + telemetria v5 (16) = 104
KMP_ASSERT_SIZE(KMP_HDR,  104);
// 104 + 4128 + 4152 = 8384
KMP_ASSERT_SIZE(KMP_SHARED, 104 + 32 + KMP_MAX_OPS * 64 + 56 + KMP_MAX_OPS * 64);
