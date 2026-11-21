// ===========================================================================
// importhash_check.cpp — prova que o resolvedor por hash acha a funcao certa.
//
// POR QUE ISTO EXISTE
//
// O ImportHash.h tem DUAS implementacoes do mesmo FNV-1a: uma `constexpr`, que
// gera a constante em tempo de compilacao, e uma de runtime, que hasheia os
// nomes dentro da tabela de exports da DLL. Se as duas divergirem em um bit, a
// resolucao nao acha nada — e o sintoma e' "a funcao nao existe", que e' o
// lugar ERRADO pra procurar um bug de hash.
//
// Este programa compara as tres coisas que precisam concordar:
//
//   1. o hash `constexpr` do nome que a gente quer
//   2. o hash de runtime do MESMO nome (deve ser igual)
//   3. o que a resolucao de fato encontra (deve ser um endereco valido)
//
// E, para nao virar um teste que concorda consigo mesmo, ele IMPRIME o nome da
// funcao que achou no endereco resolvido — lendo a tabela de exports ao
// contrario. Se o hash tivesse colidido, o nome impresso seria outro.
//
// Uso:
//     importhash_check.exe
// ===========================================================================

#include <windows.h>
#include <cstdio>
#include <cstdint>

#include "../PubgExt/Security/ImportHash.h"

// Le' de volta o nome que esta' na tabela de exports, no endereco resolvido.
// Serve de contraprova: confirma que o hash nao colidiu com outro export.
static const char* NomeNoEndereco(PVOID base, PVOID funcao)
{
    auto* dos = (PIMAGE_DOS_HEADER)base;
    auto* nt  = (PIMAGE_NT_HEADERS)((BYTE*)base + dos->e_lfanew);
    auto* exp = (PIMAGE_EXPORT_DIRECTORY)
        ((BYTE*)base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);

    auto* nomes    = (DWORD*)((BYTE*)base + exp->AddressOfNames);
    auto* ordinais = (WORD*) ((BYTE*)base + exp->AddressOfNameOrdinals);
    auto* funcoes  = (DWORD*)((BYTE*)base + exp->AddressOfFunctions);

    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        if ((BYTE*)base + funcoes[ordinais[i]] == (BYTE*)funcao)
            return (const char*)base + nomes[i];
    }
    return "(endereco nao corresponde a nenhum export)";
}

int main(void)
{
    int falhas = 0;

    printf("=========================================================\n");
    printf(" ImportHash — verificacao do resolvedor por hash\n");
    printf("=========================================================\n\n");

    // ---- 1) as duas implementacoes do FNV-1a concordam? ----
    {
        const char*    nomeAnsi = "SetWindowDisplayAffinity";
        const wchar_t* nomeWide = L"user32.dll";

        const uint64_t constAnsi = ImportHash::FNV1a_64(nomeAnsi);
        const uint64_t runAnsi   = ImportHash::HashAnsi(nomeAnsi);
        const uint64_t constWide = ImportHash::FNV1a_64_W(nomeWide);
        const uint64_t runWide   = ImportHash::HashWideCaseInsensitive(nomeWide);

        printf("--- 1. constexpr vs runtime (mesmo algoritmo?) ---\n");
        printf("  ANSI  constexpr = 0x%016llX\n", (unsigned long long)constAnsi);
        printf("  ANSI  runtime   = 0x%016llX\n", (unsigned long long)runAnsi);
        printf("  WIDE  constexpr = 0x%016llX\n", (unsigned long long)constWide);
        printf("  WIDE  runtime   = 0x%016llX\n", (unsigned long long)runWide);

        if (constAnsi != runAnsi) { printf("  [X] ANSI DIVERGE\n"); falhas++; }
        else                      { printf("  [+] ANSI concorda\n"); }

        if (constWide != runWide) { printf("  [X] WIDE DIVERGE\n"); falhas++; }
        else                      { printf("  [+] WIDE concorda\n"); }

        // A armadilha do case: a MESMA coisa em caixa diferente tem que dar o
        // MESMO hash. Sem isso, o nome de modulo vindo do loader ("USER32.DLL"
        // ou "user32.dll") decide se a resolucao funciona — por sorte.
        const uint64_t baixo = ImportHash::HashWideCaseInsensitive(L"user32.dll");
        const uint64_t alto  = ImportHash::HashWideCaseInsensitive(L"USER32.DLL");
        printf("  'user32.dll' = 0x%016llX\n", (unsigned long long)baixo);
        printf("  'USER32.DLL' = 0x%016llX\n", (unsigned long long)alto);
        if (baixo != alto) { printf("  [X] CASE-INSENSITIVE FALHOU\n"); falhas++; }
        else               { printf("  [+] case-insensitive OK (loader pode devolver qualquer caixa)\n"); }
        printf("\n");
    }

    // ---- 2) a constante do header bate com o nome? ----
    {
        const uint64_t esperado = ImportHash::FNV1a_64("SetWindowDisplayAffinity");
        printf("--- 2. a constante do header aponta pro nome certo? ---\n");
        printf("  kSetWindowDisplayAffinity = 0x%016llX\n",
               (unsigned long long)ImportHash::kSetWindowDisplayAffinity);
        printf("  FNV1a do nome             = 0x%016llX\n", (unsigned long long)esperado);
        if (ImportHash::kSetWindowDisplayAffinity != esperado) {
            printf("  [X] A CONSTANTE NAO CORRESPONDE AO NOME\n"); falhas++;
        } else {
            printf("  [+] confere\n");
        }
        printf("\n");
    }

    // ---- 3) a resolucao acha um endereco de verdade? ----
    {
        printf("--- 3. resolucao pelo PEB + tabela de exports ---\n");

        // -------------------------------------------------------------------
        // TESTE CONTRA MODULOS QUE ESTAO CARREGADOS DE CERTEZA.
        //
        // A primeira versao deste bloco so' testava user32.dll e falhou. Havia
        // DUAS causas possiveis, com o mesmo sintoma:
        //
        //   (a) o walk do PEB esta' errado (offset, TEB, lista);
        //   (b) user32.dll SIMPLESMENTE NAO ESTA' CARREGADO neste processo.
        //
        // (b) e' perfeitamente possivel: user32 so' entra na lista quando algo
        // cria uma janela, usa um hook ou chama uma API de UI. Este checker
        // roda num console que nao fez nada disso.
        //
        // Testar primeiro contra ntdll/kernel32 separa as duas: se ELES
        // resolvem, o walk esta' certo e o problema era a ausencia do modulo.
        // Sem esta distincao, eu ia procurar bug no walk sem motivo.
        // -------------------------------------------------------------------
        struct { const wchar_t* nome; uint64_t hash; } certos[] = {
            { L"ntdll.dll",    ImportHash::FNV1a_64_W(L"ntdll.dll")    },
            { L"kernel32.dll", ImportHash::FNV1a_64_W(L"kernel32.dll") },
            { L"kernelbase.dll", ImportHash::FNV1a_64_W(L"kernelbase.dll") },
        };

        printf("  3a. modulos que ESTAO carregados com certeza:\n");
        for (auto& c : certos) {
            PVOID b = ImportHash::ModuleBase(c.hash);
            printf("      %-16ls base = %p  %s\n", c.nome, b, b ? "OK" : "NAO ACHOU");
            if (!b) falhas++;
        }
        printf("\n");

        // 3b. user32.dll — carrega explicitamente antes de tentar.
        //
        // `LoadLibraryA` AQUI E' SO' DO CHECKER, e e' legitimo: o objetivo deste
        // programa e' testar o resolvedor, e para testar user32 ele precisa
        // existir. O mtmacr0.exe nao faz isso — la' o modulo ja' esta' na
        // lista porque a janela do overlay foi criada antes.
        printf("  3b. user32.dll (carregado sob demanda pelo checker):\n");
        HMODULE hU32 = LoadLibraryA("user32.dll");
        printf("      LoadLibraryA = %p\n", (void*)hU32);

        PVOID base = ImportHash::ModuleBase(ImportHash::kUser32_dll);
        printf("      base pelo PEB walk  = %p\n", base);
        printf("      base pelo LoadLibrary = %p  %s\n", (void*)hU32,
               (base == (PVOID)hU32) ? "IGUAIS" : "DIFERENTES");
        if (!base) {
            printf("      [X] NAO ACHEI O MODULO mesmo carregado.\n");
            falhas++;
        } else if (base != (PVOID)hU32) {
            printf("      [X] o walk achou OUTRO modulo (hash de nome colidiu?)\n");
            falhas++;
        } else {
            printf("      [+] walk e LoadLibrary concordam\n");
        }
        printf("\n");

        if (base) {
            PVOID p = ImportHash::ResolveExport(base, ImportHash::kSetWindowDisplayAffinity);
            printf("  3c. SetWindowDisplayAffinity : %p\n", p);

            if (!p) {
                printf("      [X] export NAO RESOLVIDO\n"); falhas++;
            } else {
                // A contraprova: o nome que esta' NAQUELE endereco.
                const char* nome = NomeNoEndereco(base, p);
                printf("      nome no endereco : %s\n", nome);

                if (strcmp(nome, "SetWindowDisplayAffinity") != 0) {
                    printf("      [X] COLISAO DE HASH — o endereco e' de outra funcao\n");
                    falhas++;
                } else {
                    printf("      [+] resolveu a funcao CERTA\n");
                }

                // A prova final: a funcao responde?
                using SetWDA_t = BOOL (WINAPI*)(HWND, DWORD);
                auto pSetWDA = (SetWDA_t)p;
                SetLastError(0);
                const BOOL r = pSetWDA(GetConsoleWindow(), 0 /* WDA_NONE */);
                const DWORD e = GetLastError();
                printf("      chamada de teste : ret=%d err=%lu\n", (int)r, (unsigned long)e);
                printf("      (err invalido de HWND e' esperado — o console pode\n");
                printf("       recusar afinidade. O que importa e' que a chamada NAO\n");
                printf("       crashou: o ponteiro e' executavel.)\n");
            }
        }
        printf("\n");
    }

    printf("=========================================================\n");
    if (falhas == 0) {
        printf(" RESULTADO: as tres verificacoes passaram.\n");
        printf(" O resolvedor por hash funciona e o nome nao esta' no codigo.\n");
    } else {
        printf(" RESULTADO: %d falha(s). NAO use o resolvedor antes de corrigir.\n", falhas);
    }
    printf("=========================================================\n");

    return falhas == 0 ? 0 : 1;
}
