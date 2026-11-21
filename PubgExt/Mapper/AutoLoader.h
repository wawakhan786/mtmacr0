#pragma once
#include <vector>
#include <cstdint>
#include <windows.h>
#include "kdmapper.hpp"
#include "intel_driver.hpp"
#include "driver_kevent_bytes.h"

// ===========================================================================
// AutoLoader — o mapeamento do driver embutido.
//
// ---------------------------------------------------------------------------
// POR QUE A ALOCACAO VIROU AllocateIndependentPages (1.2)
//
// A 1.1 usava AllocatePool. Isso significa que o driver inteiro morava dentro
// de uma alocacao de POOL — e alocacao de pool tem TAG. A tag era a do driver
// doador Intel (o kdmapper empresta a API de alocacao do iqvw64e), e tag de
// pool e' uma das primeiras coisas que um anti-cheat de kernel varre: um
// bloco grande, nao-imagem, executavel, com a tag de um driver conhecido.
//
// AllocateIndependentPages nao passa pelo pool manager: sao paginas proprias,
// vem com permissao por secao (RX no .text, RW no .data) e NAO TEM TAG. O
// mapper ja' tinha esse caminho implementado (kdmapper.cpp:181-230) — so' nao
// estava ligado.
//
// ---------------------------------------------------------------------------
// POR QUE passAllocationPtr = true
//
// Com este flag, o kdmapper chama o DriverEntry com:
//
//     DriverObject = REAL BASE da alocacao   (nao um DRIVER_OBJECT)
//     RegistryPath = param2                  (o seed; nao um UNICODE_STRING)
//
// O km_safe_source.c precisa dos dois:
//   - a REAL BASE, porque MmFreeIndependentPages exige o endereco exato que
//     foi alocado — nao da' pra "soltar o ponteiro" como no ExFreePool;
//   - o SEED, pra derivar os nomes de \Device, \DosDevices e \Driver sem
//     deixar nenhum literal dentro do .sys.
//
// Sem este flag o driver nao consegue se descarregar sozinho, e o unmap fica
// indisponivel (o DEACTIVATE continua funcionando).
// ===========================================================================
// ===========================================================================
// `seed` tem default 0 DE PROPOSITO, e nao e' conveniencia: existem outros
// projetos na solucao (o OffsetDumper) que chamam esta funcao e nao tem nada a
// ver com o seed. Com 0, o driver devolve STATUS_INVALID_PARAMETER e recusa a
// carga — falha ALTA, nome nenhum derivado. Um default "esperto" que inventasse
// um seed aqui reintroduziria um nome previsivel.
// ===========================================================================

// ===========================================================================
// LoadDriverKeventInternal — mapeia o driver de transporte KEVENT.
//
// Mesmo caminho do driver de producao (kdmapper + AllocateIndependentPages +
// passAllocationPtr), com os BYTES do outro .sys. Existe separado porque os
// dois drivers podem estar embutidos no mesmo .exe, e o chamador escolhe qual
// mapear.
//
// IMPORTANTE: o user-mode tem que criar a secao, os dois eventos e a chave do
// rendezvous ANTES desta chamada. Se o driver subir primeiro, o DriverEntry
// morre no ZwOpenKey e isto devolve false — o sintoma e' "erro 2", que parece
// "driver nao carregou".
// ===========================================================================
// DriverKeventImageSize - SizeOfImage do driver KEVENT embutido.
//
// POR QUE O USER MODE CALCULA: o driver mapeado a mao nao tem headers
// (destroyHeader=true no MapDriver), entao ele nao consegue ler o proprio
// SizeOfImage para publicar. O cliente TEM os bytes originais aqui, entao
// tira o numero do PE antes de mapear e usa no MmFreeIndependentPages.
//
// Devolve 0 se o PE nao puder ser lido (aí a liberacao e' pulada e o log diz).
// ===========================================================================
inline uint32_t DriverKeventImageSize()
{
    const size_t n = sizeof(driver_kevent_bytes);
    if (n < 0x100) return 0;
    const uint8_t* b = driver_kevent_bytes;
    if (b[0] != 'M' || b[1] != 'Z') return 0;

    uint32_t lfanew = 0;
    memcpy(&lfanew, b + 0x3C, sizeof(lfanew));
    if (lfanew == 0 || (size_t)lfanew + 0x60 >= n) return 0;

    const uint8_t* nt = b + lfanew;
    if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0) return 0;

    // IMAGE_OPTIONAL_HEADER64: SizeOfImage esta em +0x38, NAO em +0x3C.
    //
    // MEDIDO no smoke de 20/10/2026: o unload imprimiu "size=0x400".
    // 0x400 e o SizeOfHeaders — o campo SEGUINTE ao SizeOfImage. Ou seja, o
    // unload liberava 1 KB de uma imagem de ~20 KB, e o resto do driver ficava
    // residente. Um erro de 4 bytes no deslocamento, invisivel no build.
    uint32_t sizeOfImage = 0;
    memcpy(&sizeOfImage, nt + 24 + 0x38, sizeof(sizeOfImage));
    return sizeOfImage;
}

// ===========================================================================
// ---------------------------------------------------------------------------
// O QUE FOI ALOCADO (para o unload liberar exatamente isso).
//
// `DriverKeventImageSize()` devolve SizeOfImage do PE — que e' MAIOR que a
// alocacao: o mapper reduz o tamanho em SizeOfHeaders (nao mapeia headers).
// Liberar SizeOfImage e' liberar uma pagina a mais que o alocado, e isso
// corrompe o banco de PFNs: PFN_LIST_CORRUPT (0x4E), medido em 20/10/2026.
// ---------------------------------------------------------------------------
inline uint64_t DriverKeventAllocBase() { return kdmapper::GetUltimaBaseAlocada(); }
inline uint32_t DriverKeventAllocSize() { return kdmapper::GetUltimoTamanhoAlocado(); }

inline bool LoadDriverKeventInternal(uint64_t seed = 0)
{
    if (!NT_SUCCESS(intel_driver::Load())) {
        kdmLog(L"[-] kevent: intel_driver::Load() FALHOU\n");
        return false;
    }

    std::vector<uint8_t> data(driver_kevent_bytes,
                              driver_kevent_bytes + sizeof(driver_kevent_bytes));
    NTSTATUS exitCode = 0;

    // -----------------------------------------------------------------------
    // GRAVA O CANAL DENTRO DA IMAGEM (sem registro).
    //
    // A `g_BootSlot` do driver nasce com o magic KMP_BOOT_MAGIC e zeros. Aqui a
    // gente LOCALIZA esse magic nos bytes e escreve Pid/Signature/handles logo
    // depois — respeitando o layout de 40 bytes do KMP_BOOT_SLOT.
    //
    // Por que no arquivo e nao no kernel: escrever na imagem JA' mapeada exigiria
    // contornar a protecao das paginas (o mapper deixa .data READWRITE, mas a
    // escrita por handle do doador e' outro caminho). Antes de mapear, e' so'
    // memcpy local.
    //
    // Se o magic nao for encontrado, NAO seguimos: mapear um driver que vai
    // recusar o boot (Magic != KMP_BOOT_MAGIC) so' gasta tempo e bagunca o
    // estado.
    // -----------------------------------------------------------------------
    {
        const KmpBootPatch& p = BootPatch();
        if (p.ativo) {
            const uint32_t magic = (uint32_t)KMP_BOOT_MAGIC;
            uint8_t* alvo = nullptr;
            for (size_t i = 0; i + sizeof(KMP_BOOT_SLOT) <= data.size(); ++i) {
                uint32_t v = 0;
                memcpy(&v, data.data() + i, sizeof(v));
                if (v != magic) continue;
                // o campo Pid logo depois do magic tem que estar zerado:
                // assim a varredura nao casa com um 0x534C4F54 que apareceu por
                // acaso dentro de instrucao.
                uint32_t pidZerado = 0;
                memcpy(&pidZerado, data.data() + i + 4, sizeof(pidZerado));
                if (pidZerado != 0) continue;
                alvo = data.data() + i;
                break;
            }
            if (alvo == nullptr) {
                kdmLog(L"[-] kevent: boot slot not found in image\n");
                return false;
            }
            KMP_BOOT_SLOT slot = {};
            slot.Magic     = magic;
            slot.Pid       = p.pid;
            slot.Signature = p.sig;
            slot.Section   = p.hSec;
            slot.ReqEvent  = p.hReq;
            slot.RspEvent  = p.hRsp;
            memcpy(alvo, &slot, sizeof(slot));
            kdmLog(L"[+] kevent: canal entregue no slot de boot (sem registro)\n");
        }
    }
    const bool ok = kdmapper::MapDriver(data.data(), 0, seed,
                                        false,   // free
                                        true,    // destroyHeader
                                        kdmapper::AllocationMode::AllocateIndependentPages,
                                        true,    // passAllocationPtr
                                        nullptr, &exitCode);
    intel_driver::Unload();

    if (!ok) {
        kdmLog(L"[-] kevent: MapDriver devolveu FALSE\n");
        return false;
    }
    if (!NT_SUCCESS(exitCode)) {
        kdmLog(L"[-] kevent: DriverEntry devolveu 0x"
               << std::hex << (unsigned)exitCode << std::dec
               << L"  (0xC0000034 = rendezvous nao achado: o cliente tem que criar ANTES)\n");
        return false;
    }
    kdmLog(L"[+] kevent: driver mapped\n");
    return true;
}
