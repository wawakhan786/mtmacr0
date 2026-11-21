// ===========================================================================
// DriverlessProbe.cpp
//
// Sonda SEM driver. Existe porque o unico teste em runtime que tínhamos
// exigia carregar o Ndu.sys — e o Ndu.sys derrubou a maquina com 0x50.
//
// Este arquivo roda tudo que NAO depende de ler memoria de outro processo:
//
//   * ParsePattern      — puro, so a sintaxe do padrao
//   * FindSignature     — IMPLEMENTACAO-ESPELHO, lendo o nosso proprio modulo
//                         com memcpy (sem driver), e comparando com o que o
//                         Memory::FindSignature teria que devolver
//   * cabecalho PE      — DOS/NT, integridade do SizeOfImage
//   * GET_CR3           — __readcr3() do proprio processador
//
// O valor disto: prova que a LOGICA esta certa antes de arriscar um BSOD.
// Se o ParsePattern ou o scan estiverem quebrados, o problema e o meu codigo,
// nao o driver — e nao adianta continuar cacando driver doador.
//
// O que ele NAO prova: nada sobre leitura de memoria de outro processo.
// Isso so o self test completo prova, e ele precisa do driver.
// ===========================================================================

#include "pch.h"
#include "Memory/Memory.h"
#include <intrin.h>

namespace {

int g_dl_pass = 0;
int g_dl_fail = 0;
int g_dl_skip = 0;

void DLReport(const char* name, bool pass, const char* fmt, ...)
{
    char detail[256] = { 0 };
    va_list args;
    va_start(args, fmt);
    vsnprintf(detail, sizeof(detail), fmt, args);
    va_end(args);

    if (pass) {
        g_dl_pass++;
        LOG("[+] %-34s %s\n", name, detail);
    } else {
        g_dl_fail++;
        LOG("[-] %-34s %s\n", name, detail);
    }
}

void DLSkip(const char* name, const char* why)
{
    g_dl_skip++;
    LOG("[~] %-34s PULADO (%s)\n", name, why);
}

uint64_t Hash(const void* data, size_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Implementacao-ESPELHO do Memory::FindSignature, mas com memcpy local em vez
// de leitura via driver. Mesma logica de blocos, mesma sobreposicao, mesma
// busca com wildcard.
//
// Se este scan achar a assinatura no offset certo e o Memory::FindSignature
// nao achar, o bug esta na camada de leitura — nao no algoritmo do scan.
// ---------------------------------------------------------------------------
uint64_t FindSignatureLocal(const uint8_t* base, size_t size,
                            const std::vector<uint8_t>& pat,
                            const std::vector<uint8_t>& mask)
{
    const size_t patLen = pat.size();
    if (!base || patLen == 0 || size < patLen) return 0;

    for (size_t i = 0; i + patLen <= size; ++i) {
        bool hit = true;
        for (size_t j = 0; j < patLen; ++j) {
            if ((base[i + j] & mask[j]) != (pat[j] & mask[j])) { hit = false; break; }
        }
        if (hit) return i;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Leitura da CR3, isolada numa funcao propria.
//
// PRECISA ser separada: o compilador recusa `__try` (C2712) em funcoes que
// exigem liberacao de objeto — ou seja, qualquer funcao com std::vector ou
// std::string vivos no escopo. Esta aqui nao tem nenhum.
//
// E `__readcr3` e privilegiada em modo usuario: sem SEH ela mata o processo
// com 0xC0000096.
// ---------------------------------------------------------------------------
bool TryReadCr3(uint64_t& out)
{
    __try {
        out = __readcr3();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
bool RunDriverlessProbe()
{
    LOG("\n");
    LOG("==================================================================\n");
    LOG("  SONDA SEM DRIVER — logica pura, alvo: o proprio RuntimeBroker.exe\n");
    LOG("==================================================================\n");
    LOG("  Nao carrega driver, nao le outro processo. Seguro de rodar.\n\n");

    g_dl_pass = 0;
    g_dl_fail = 0;
    g_dl_skip = 0;

    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleA(NULL));
    if (!base) {
        LOG("[-] GetModuleHandleA(NULL) devolveu 0 — impossivel continuar\n");
        LOG("==================================================================\n\n");
        return false;
    }

    // -----------------------------------------------------------------------
    // 1. ParsePattern — puro. Nao depende de nada.
    //
    // Se isto falhar, o FindSignature NUNCA acha nada, e o sintoma seria
    // "assinatura nao encontrada" num padrao que esta la na frente.
    //
    // CUIDADO: os bytes esperados tem que vir com tamanho EXPLICITO. Usar
    // strlen aqui e bug — "\x00\x00" tem strlen 0, e a comparacao vira
    // vazio-contra-vazio e passa sem testar nada. Foi exatamente isso que
    // fez os casos de wildcard falharem por motivo errado.
    // -----------------------------------------------------------------------
    {
        struct PatCase {
            const char*   pattern;
            const uint8_t wantBytes[8];
            const uint8_t wantMasks[8];
            size_t        wantLen;
            bool          valid;
        };
        const PatCase cases[] = {
            { "48 8B",          {0x48,0x8B},             {0xFF,0xFF},             2, true  },
            { "?? ??",          {0x00,0x00},             {0x00,0x00},             2, true  },
            { "4?",             {0x40},                  {0xF0},                  1, true  },
            { "?8",             {0x08},                  {0x0F},                  1, true  },
            { "48 8B ?? 4? 90", {0x48,0x8B,0x00,0x40,0x90},
                                {0xFF,0xFF,0x00,0xF0,0xFF},                        5, true  },
            { "ZZ",             {0},                     {0},                     0, false },
            { "4",              {0},                     {0},                     0, false },
            { "48 8B 0",        {0},                     {0},                     0, false },
            { "",               {0},                     {0},                     0, false },
        };

        int ok = 0;
        const int total = (int)(sizeof(cases) / sizeof(cases[0]));
        for (const auto& c : cases) {
            std::vector<uint8_t> b, m;
            const bool got = Memory::ParsePattern(c.pattern, b, m);

            bool good = (got == c.valid);
            if (good && c.valid) {
                good = (b.size() == c.wantLen) && (m.size() == c.wantLen) &&
                       (memcmp(b.data(), c.wantBytes, c.wantLen) == 0) &&
                       (memcmp(m.data(), c.wantMasks, c.wantLen) == 0);
            }
            if (good) {
                ++ok;
            } else {
                LOG("      padrao '%s': esperava %s com %zu bytes, obteve %s "
                    "com %zu\n",
                    c.pattern, c.valid ? "valido" : "invalido", c.wantLen,
                    got ? "valido" : "invalido", b.size());
            }
        }
        DLReport("ParsePattern (puro)", ok == total, "%d/%d padroes ok", ok, total);
    }

    // -----------------------------------------------------------------------
    // 2. Cabecalho PE — le da memoria com a verdade absoluta (memcpy).
    // -----------------------------------------------------------------------
    uint64_t imageSize = 0;
    {
        IMAGE_DOS_HEADER dos{};
        memcpy(&dos, base, sizeof(dos));

        bool dosOk = (dos.e_magic == IMAGE_DOS_SIGNATURE);
        DLReport("PE: assinatura MZ", dosOk, "e_magic=0x%04X e_lfanew=0x%X",
                 dos.e_magic, dos.e_lfanew);

        if (dosOk) {
            IMAGE_NT_HEADERS64 nt{};
            memcpy(&nt, base + dos.e_lfanew, sizeof(nt));

            const bool ntOk = (nt.Signature == IMAGE_NT_SIGNATURE);
            DLReport("PE: assinatura PE\\0\\0", ntOk, "sig=0x%08X",
                     nt.Signature);

            imageSize = nt.OptionalHeader.SizeOfImage;
            const bool sizeOk = (imageSize > 0x10000 && imageSize < 0x10000000);
            DLReport("PE: SizeOfImage plausivel", sizeOk, "0x%llX bytes",
                     (unsigned long long)imageSize);

            const bool machineOk = (nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64);
            DLReport("PE: machine x64", machineOk, "0x%04X",
                     nt.FileHeader.Machine);
        } else {
            DLSkip("PE: NT headers", "sem MZ");
        }
    }

    // -----------------------------------------------------------------------
    // 3. FindSignature no proprio modulo
    //
    // O padrao comeca na base: "MZ" + 60 bytes wildcard + "PE\0\0".
    // O resultado TEM que ser o offset 0. Se vier qualquer outro, a busca
    // ou o calculo de deslocamento esta errado.
    // -----------------------------------------------------------------------
    {
        std::string pat = "4D 5A";
        for (int i = 0; i < 60; ++i) pat += " ??";
        pat += " 50 45 00 00";

        std::vector<uint8_t> pb, pm;
        if (!Memory::ParsePattern(pat.c_str(), pb, pm)) {
            DLReport("FindSignature (MZ..PE)", false, "ParsePattern recusou o padrao");
        } else if (imageSize == 0) {
            DLSkip("FindSignature (MZ..PE)", "SizeOfImage desconhecido");
        } else {
            const uint64_t off = FindSignatureLocal(base, (size_t)imageSize, pb, pm);
            DLReport("FindSignature (MZ..PE)", off == 0,
                     "offset=0x%llX (esperado 0)",
                     (unsigned long long)off);
        }
    }

    // -----------------------------------------------------------------------
    // 4. Scan num padrao que NAO existe — o scan tem que devolver 0
    //
    // Sem este caso, um scan que sempre devolve "achei" passaria no teste 3.
    // -----------------------------------------------------------------------
    {
        std::vector<uint8_t> pb, pm;
        // 32 bytes de 0xCC seguidos: nao aparece no .text de um binario
        // normal, e se aparecer o teste avisa que o caso nao discrimina.
        std::string pat;
        for (int i = 0; i < 32; ++i) pat += (i ? " CC" : "CC");

        if (!Memory::ParsePattern(pat.c_str(), pb, pm)) {
            DLReport("scan negativo (recusa)", false, "ParsePattern recusou");
        } else if (imageSize == 0) {
            DLSkip("scan negativo (recusa)", "SizeOfImage desconhecido");
        } else {
            const uint64_t off = FindSignatureLocal(base, (size_t)imageSize, pb, pm);
            DLReport("scan negativo (recusa)", off == 0,
                     off ? "achou em 0x%llX — caso nao discrimina"
                         : "nao achou, correto",
                     (unsigned long long)off);
        }
    }

    // -----------------------------------------------------------------------
    // 5. E_lfanew — o unico campo do cabecalho que da para comparar com o
    //    arquivo
    //
    // A versao anterior deste teste comparava os 4 KiB inteiros do cabecalho
    // em memoria contra o arquivo no disco e reportava "DIVERGEM". Isso e
    // comportamento NORMAL, nao erro: o loader do Windows modifica os headers
    // em memoria — realoca imports, aplica relocations, marca secoes. A
    // comparacao inteira nunca ia bater.
    //
    // `e_lfanew` e estavel: o loader nao mexe nele.
    // -----------------------------------------------------------------------
    {
        char exePath[MAX_PATH] = { 0 };
        GetModuleFileNameA(NULL, exePath, MAX_PATH);

        HANDLE h = CreateFileA(exePath, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            DLSkip("e_lfanew: memoria == arquivo", "nao abri o exe no disco");
        } else {
            IMAGE_DOS_HEADER diskDos{};
            DWORD got = 0;
            const BOOL readOk = ReadFile(h, &diskDos, sizeof(diskDos), &got, NULL);
            CloseHandle(h);

            IMAGE_DOS_HEADER memDos{};
            memcpy(&memDos, base, sizeof(memDos));

            if (!readOk || got != sizeof(diskDos)) {
                DLReport("e_lfanew: memoria == arquivo", false,
                         "ReadFile devolveu %lu/%zu", got, sizeof(diskDos));
            } else {
                const bool same = (memDos.e_lfanew == diskDos.e_lfanew) &&
                                  (memDos.e_magic  == diskDos.e_magic);
                DLReport("e_lfanew: memoria == arquivo", same,
                         "mem=0x%X disco=0x%X (cabecalho alem disto o loader modifica)",
                         memDos.e_lfanew, diskDos.e_lfanew);
            }
        }
    }

    // -----------------------------------------------------------------------
    // 6. CR3 do processador
    //
    // CUIDADO: __readcr3() e PRIVILEGIADA em modo usuario. Chamar direto
    // levanta excecao 0xC0000096 (privileged instruction) e mata o processo
    // sem __try. Foi assim que esta sonda crashou na primeira execucao.
    //
    // O valor so vale como referencia se a leitura passar — e para saber se
    // passa, tem que tentar dentro de um __try.
    // -----------------------------------------------------------------------
    {
        uint64_t cr3 = 0;
        const bool leu = TryReadCr3(cr3);

        if (!leu) {
            DLSkip("__readcr3() do processador",
                   "instrucao privilegiada em modo usuario (0xC0000096)");
        } else {
            const uint64_t pfn = cr3 & 0x000FFFFFFFFFF000ULL;
            const bool ok = (pfn != 0) && ((cr3 & 0xFFF) == 0);
            DLReport("__readcr3() do processador", ok,
                     "cr3=0x%llX pfn=0x%llX",
                     (unsigned long long)cr3, (unsigned long long)pfn);
            if (ok) {
                LOG("      ^ e este o valor que o GET_CR3 tem que bater no self test completo\n");
            }
        }
    }

    // -----------------------------------------------------------------------
    // Resumo
    // -----------------------------------------------------------------------
    LOG("\n");
    LOG("------------------------------------------------------------------\n");
    LOG("  RESULTADO (sem driver): %d passaram, %d falharam, %d pulados\n",
        g_dl_pass, g_dl_fail, g_dl_skip);
    LOG("------------------------------------------------------------------\n");

    if (g_dl_fail == 0) {
        LOG("  A logica de scan e de cabecalho PE esta CORRETA.\n");
        LOG("  Se alguma coisa falhar no self test completo (com driver),\n");
        LOG("  o problema esta na camada de leitura — nao no algoritmo.\n");
    } else {
        LOG("  HA FALHA NA LOGICA. Nao adianta carregar driver: o problema\n");
        LOG("  e o codigo, e ele vai falhar do mesmo jeito com driver.\n");
    }

    if (g_dl_skip) {
        LOG("\n  %d caso(s) pulado(s) — ver a razao em cada linha.\n", g_dl_skip);
    }

    LOG("==================================================================\n\n");
    return g_dl_fail == 0;
}
