#pragma once
// ===========================================================================
// common.h — CONTRATO DE IOCTL, LADO USER MODE. FONTE UNICA.
//
// Este arquivo e' incluido por `driver_interface_v3.h` e, por ele, pelo
// `driver_interface_v3.cpp`. Nenhum outro .cpp do projeto inclui — o
// `TestHarness.exe` (tools\TestHarness.cpp) mantem a copia dele de proposito,
// porque e' compilado FORA da solution por cl direto e nao pode arrastar os
// includes do PubgExt. O `tools\dump_driver\contract_check.py` CONFERE que a
// copia do harness continua igual a esta.
//
// ---------------------------------------------------------------------------
// O QUE ESTAVA ERRADO AQUI (corrigido)
//
// Este header tinha ficado no padrao do driver ANTIGO (`Ndu`, `\Device\Ndu`,
// `0x00222004`), enquanto o driver que roda hoje (`km_safe_source.c`) usa
// `0x801 GET_PID`, `0x802 MODULE_BASE`, `0x803 READ`. Os NOMES tambem
// estavam deslocados: aqui `IOCTL_GET_PID` valia 0x803, e no kernel
// `IOCTL_GET_PID` vale 0x801.
//
// Isso nunca explodiu porque o `driver_interface_v3.cpp` redefinia as quatro
// macros DEPOIS do include, e a redefinicao vencia (com `warning C4005`).
// Ou seja: o header estava errado e mascarado pelo arquivo que deveria
// consumir ele. Se alguem apagasse aquelas linhas do .cpp "por limpeza", o
// cheat passaria a falar 0x801=READ com um driver que trata 0x801 como
// GET_PID — leitura de memoria lendo PID. Sem erro claro.
//
// A regra que fica: A COPIA DO KERNEL E A VERDADE. Toda macro daqui tem que
// casar com `km_safe_source.c`, e quem prova isso e' o contract_check.py.
// Se os dois lados precisarem mudar, mudam JUNTOS.
// ===========================================================================

#include <windows.h>
#include <cstdint>

// ---------------------------------------------------------------------------
// CODIGOS DE IOCTL
//
// FILE_DEVICE_UNKNOWN e' o `KM_DEVICE_TYPE` do kernel (o mesmo 0x22). O
// contract_check.py decodifica o CTL_CODE inteiro, entao o token do device
// pode ser diferente dos dois lados — o que conta e' o numero final.
// ---------------------------------------------------------------------------
#define IOCTL_GET_PID           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GET_MODULE_BASE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_READ_MEMORY       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

// DECLARADO E NAO IMPLEMENTADO no kernel, de proposito: nao existe caminho de
// escrita em memoria de outro processo neste projeto (`Memory::Write` devolve
// false fixo). O macro fica para o dia em que existir, e para o
// contract_check.py poder dizer "isto nao tem par do outro lado" em vez de
// alguem descobrir com um IOCTL que devolve STATUS_INVALID_DEVICE_REQUEST.
#define IOCTL_WRITE_MEMORY      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Injecao de mouse pelo KERNEL — existe para o pacote NAO levar
// LLMHF_INJECTED, que qualquer hook WH_MOUSE_LL ve' no SendInput.
//   req.Pid   = dx (LONG, sinal estendido)
//   req.Size  = dy
#define IOCTL_MOUSE_MOVE        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Versao do driver: o antidoto do "driver fantasma". O kdmapper nao
// descarrega, entao uma sessao nova pode achar o device de uma build ANTIGA.
// O PING (0x8FF) prova que ha' um km_safe; este IOCTL prova QUAL.
#define IOCTL_KM_GET_VERSION    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KM_DEACTIVATE     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Fora de uso no user mode (o driver nao se libera mais), mas continua
// definido dos dois lados: um user-mode de build antiga tem que receber
// STATUS_NOT_IMPLEMENTED, nao conversar com um caminho que nao existe.
#define IOCTL_KM_SELFDESTRUCT   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)

// out = { ULONG64 base; ULONG64 size; } — o mapper usa para liberar as
// paginas do driver. 16 bytes.
#define IOCTL_KM_GET_IMAGE_INFO CTL_CODE(FILE_DEVICE_UNKNOWN, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Impressao digital do driver. So' o km_safe responde: sem a assinatura, o
// TestHarness NAO roda os casos perigosos (foi assim que a maquina parou de
// cair com 0x50 contra o driver velho).
#define IOCTL_KM_PING           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8FF, METHOD_BUFFERED, FILE_ANY_ACCESS)

// ---------------------------------------------------------------------------
// CONSTANTES DO CONTRATO
//
// Ficam aqui porque o contract_check.py compara TODA constante numerica
// presente nos dois lados. Uma delas derivando e' tao ruim quanto um IOCTL
// divergente, so' que mais silencioso.
// ---------------------------------------------------------------------------
#define KM_PING_MAGIC           0x504B4D53UL   // 'S''M''K''P' -> "PKMS" em LE
#define KM_DESTRUCT_MAGIC       0x4B4D5344UL   // 'D''S''M''K' -> "KMSD" em LE
#define KM_DESTRUCT_UNMAP       2              // vai no campo Pid
#define KM_DRIVER_VERSION       0x00010002     // 1.2: + DEACTIVATE + nomes por seed

#define KM_MAX_COPY             (1024 * 1024)  // 1 MiB por IOCTL (teto do kernel)
#define KM_MAX_NAME             260

// ---------------------------------------------------------------------------
// NAO HA STRUCT AQUI, DE PROPOSITO.
//
// Este arquivo tinha `MemoryReadRequest` e `ModuleBaseRequest` — restos do
// contrato do Ndu, sem um unico leitor no projeto (conferido com grep). O
// request real vive no `driver_interface_v3.cpp` e e' `DRIVER_REQUEST`
// (40 bytes); o kernel chama o mesmo layout de `KM_DRIVER_REQUEST`.
//
// O contract_check.py compara o LAYOUT dos dois (tamanho, offset e largura de
// cada campo) e ignora os nomes: o kernel usa `Pid`, o user mode usa `pid`, e
// isso esta' certo. Duas structs a mais sem leitor so' davam a impressao de
// que existia mais de um contrato.
// ---------------------------------------------------------------------------
