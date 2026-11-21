#pragma once

// ===========================================================================
//
//                      ################################
//                      #   C O D I G O   M O R T O    #
//                      ################################
//
// ESTE HEADER NAO FAZ PARTE DO DRIVER QUE RODA HOJE.
//
// Quem o inclui e' `km_driver_source.c` — o driver APOSENTADO (`Ndu`), que
// derrubou a maquina 6 vezes com PAGE_FAULT_IN_NONPAGED_AREA (0x50) e nao e'
// compilado por nenhum script deste projeto. O driver atual,
// `km_safe_source.c`, NAO usa canal de secao nomeada: ele fala por device +
// IOCTL (`\Device\PUBG_KM_<seed>`).
//
// A auditoria externa de 2026 apontou, com razao, que este protocolo "nao tem
// sincronizacao atomica adequada" e que "nao esta conectado ao driver enviado".
// As duas coisas sao verdade, e o motivo e' que ele nao esta' conectado a nada:
// e' o desenho do canal que perdeu para o IOCTL.
//
// O QUE ELE TEM DE ERRADO, registrado para ninguem copiar:
//
//   * ReqSeq/AckSeq/Busy sao `km_u32` normais, sem volatile e sem barreira de
//     memoria. Entre dois nucleos isto nao ordena nada: o compilador pode
//     manter o valor em registrador e a CPU pode reordenar. O protocolo diz
//     "escreve o corpo, DEPOIS publica o ReqSeq" — e nada no codigo garante
//     essa ordem. Precisa de volatile + KeMemoryBarrier (kernel) e
//     InterlockedExchange (user), ou de um evento/KSynchronizationEvent.
//   * `Busy` e' escrito pelo driver e lido pelo user-mode em laco de espera:
//     um spin em memoria compartilhada sem Yield e' queima de CPU com
//     prioridade normal.
//   * O `Name[64]` em ANSI no pedido e' inconsistente com o resto do contrato,
//     que e' UNICODE em todo lugar.
//
// POR QUE NAO FOI DELETADO: `km_driver_source.c` continua no repositorio como
// referencia historica (e ele inclui este arquivo), entao apagar aqui quebraria
// o que sobrou do arquivo. A regra do projeto de "codigo perigoso morto se
// deleta" vale quando o arquivo nao tem leitor nenhum; aqui tem um, mesmo que
// aposentado.
//
// NAO RELIGUE ESTE CANAL. Se um dia precisar de memoria compartilhada, o
// caminho e' reescrever do zero com evento de sincronizacao — nao consertar
// este.
//
// ===========================================================================
// KmShared.h — canal de memoria compartilhada driver <-> user-mode
//
// (LEMBRETE: codigo MORTO — ver o banner no topo. O texto abaixo e' o desenho
//  original do Ndu, mantido como registro historico.)
// POR QUE ISSO EXISTE
//
// O driver (Ndu.sys) CARREGA e o READ_MEMORY funciona â€” provado com 20
// leituras identicas de 4 KiB e 1 MiB. Mas o user-mode nao consegue falar com
// ele, e as tres tentativas de dar um caminho falharam:
//
//   IoCreateDevice   -> BSOD 0x50  (exige _DRIVER_EXTENSION valida)
//   IoCreateDriver   -> 0xC0000035 (sempre: nome unico, boot limpo, nada
//                                   criado antes â€” nao e colisao)
//   device manual    -> cria, o symlink diz 0x0, mas o user-mode nao abre:
//                       objeto do pool nao tem _OBJECT_HEADER, e inventar um
//                       da BSOD 0x18 REFERENCE_BY_POINTER
//
// Este canal NAO USA NADA DISSO. Uma secao nomeada (ZwCreateSection +
// ZwMapViewOfSection) e mapeada nos dois lados. Nao ha device object, nao ha
// symbolic link, nao ha IRP, nao ha MajorFunction. O Object Manager so
// aparece para o NOME da secao.
//
// ===========================================================================
// PROTOCOLO
//
// Tres campos de 32 bits coordenam tudo, sem operacao atomica:
//
//   ReqSeq   user-mode INCREMENTA para publicar um pedido
//   AckSeq   driver INCREMENTA depois de processar
//   Busy     driver seta 1 no inicio e 0 no fim
//
// user-mode: escreve corpo -> ReqSeq = n -> espera AckSeq >= n e Busy == 0
// driver   : se AckSeq < ReqSeq -> Busy=1, processa, AckSeq=ReqSeq, Busy=0
//
// O driver NUNCA escreve no corpo do pedido, e o user-mode NUNCA escreve na
// resposta. Cada lado so escreve no seu lado do buffer, entao nao ha corrida
// de dados â€” so de visibilidade, que os campos de sequencia ordenam.
//
// ===========================================================================
// BATCH
//
// Uma ida e volta por leitura seria lento demais para ESP (o driver tem que
// pollar, e cada poll custa ~1 ms de Sleep). Entao o pedido carrega ate
// KM_SHARED_MAX_OPS operacoes e a resposta os dados de todas: o user-mode
// preenche varias leituras de uma vez e paga UMA volta.
// ===========================================================================

// Tipos: NAO incluir nada aqui.
//
//   `_KERNEL_MODE` definido  -> ntifs.h ja deu ULONG/ULONG64/CHAR/NTSTATUS
//   user-mode                -> windows.h ja deu os mesmos
//
// <stdint.h> NAO existe no ambiente km do WDK, e incluir quebra o driver.
typedef unsigned char       km_u8;
typedef unsigned int        km_u32;
typedef unsigned __int64    km_u64;

#define KM_SHARED_MAGIC      0x4E647532u   // 'Ndu2'
#define KM_SHARED_VERSION    1u

// Nome da secao. O driver cria em \BaseNamedObjects\Ndu2Shared; o user-mode
// abre por "Global\\Ndu2Shared" (o prefixo Global aponta exatamente para
// \BaseNamedObjects).
#define KM_SHARED_NAME_W     L"\\BaseNamedObjects\\Ndu2Shared"
#define KM_SHARED_USER_NAME  "Global\\Ndu2Shared"

#define KM_SHARED_MAX_OPS        64              // operacoes por volta
#define KM_SHARED_MAX_PAYLOAD    (256 * 1024)    // 256 KiB de dados por volta

// ---------------------------------------------------------------------------
// Operacoes
// ---------------------------------------------------------------------------
#define KM_OP_NOP        0u
#define KM_OP_READ       1u   // le Size bytes de Address -> Data
#define KM_OP_WRITE      2u   // escreve payload em Address
#define KM_OP_GET_PID    3u   // processo por Name -> Out0
#define KM_OP_GET_MODULE 4u   // base do modulo Name no Pid -> Out0/Out1
#define KM_OP_GET_CR3    5u   // DirectoryTableBase do Pid -> Out0

#pragma pack(push, 8)

typedef struct _KM_SHARED_OP {
    km_u32  Kind;        // in  KM_OP_*
    km_u32  Pid;         // in  0 = usa o processo alvo corrente
    km_u64  Address;     // in  origem (read) / destino (write)
    km_u64  Size;        // in  tamanho
    km_u64  PayloadOff;  // in  offset em Data do payload (escrita)
    char    Name[64];    // in  nome (GET_PID / GET_MODULE)
    km_u32  _pad;
    km_u32  Status;      // out NTSTATUS da operacao
    km_u64  DataOff;     // out offset em Data onde o resultado comeca
    km_u64  Out0;        // out Base (GET_MODULE) / Pid (GET_PID) / Cr3
    km_u64  Out1;        // out Size (GET_MODULE)
} KM_SHARED_OP, *PKM_SHARED_OP;

// Pedido â€” corpo escrito pelo USER-MODE.
typedef struct _KM_SHARED_REQ {
    km_u32        Count;                 // in  quantas ops validas
    km_u32        _pad;
    KM_SHARED_OP  Ops[KM_SHARED_MAX_OPS];
} KM_SHARED_REQ, *PKM_SHARED_REQ;

// Resposta â€” escrita pelo DRIVER.
typedef struct _KM_SHARED_RESP {
    km_u32  Count;          // out ops processadas
    km_u32  _pad;
    km_u64  DataUsed;       // out bytes usados em Data
    km_u64  TargetPid;      // out processo alvo atual (0 = nenhum)
    km_u64  TargetCr3;      // out CR3 do alvo
    km_u64  TargetBase;     // out base do modulo principal do alvo
    km_u64  TargetSize;     // out SizeOfImage do alvo
} KM_SHARED_RESP, *PKM_SHARED_RESP;

typedef struct _KM_SHARED {
    km_u32  Magic;          // out 'Ndu2'
    km_u32  Version;        // out KM_SHARED_VERSION
    km_u32  ReqSeq;         // in  user-mode incrementa para publicar
    km_u32  AckSeq;         // out driver incrementa ao terminar
    km_u32  Busy;           // out 1 enquanto o driver processa
    km_u32  LastError;      // out NTSTATUS do ultimo ciclo
    km_u64  Requests;       // out pedidos atendidos
    km_u64  _reserved[6];

    KM_SHARED_REQ   Req;
    KM_SHARED_RESP  Resp;
    km_u8           Data[KM_SHARED_MAX_PAYLOAD];
} KM_SHARED, *PKM_SHARED;

#pragma pack(pop)

#define KM_SHARED_SIZE  (sizeof(KM_SHARED))
