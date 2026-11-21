#pragma once
// ===========================================================================
// ImportHash.h — resolve import POR HASH, andando na tabela de exports.
//
// ---------------------------------------------------------------------------
// O PROBLEMA
//
// `SetWindowDisplayAffinity` estava na IAT do mtmacr0.exe:
//
//     USER32.dll
//       ...
//       394 SetWindowDisplayAffinity
//
// Nao e' so' "uma funcao a mais". E' a assinatura de OVERLAY: a API existe
// para dizer ao DWM "nao me capture", e e' usada por OBS, por gravadores de
// tela — e por overlay de cheat. `dumpbin /imports` respondia isso sem
// heuristica nenhuma, e a string aparecia no .rdata.
//
// ---------------------------------------------------------------------------
// A SOLUCAO — E A QUE **NAO** FOI ESCOLHIDA
//
// O caminho obvio seria:
//
//     auto p = GetProcAddress(LoadLibraryA("user32.dll"), "SetWindowDisplayAffinity");
//
// Isso esta' ERRADO por dois motivos, e os dois importam:
//
//   1. `LoadLibrary` + `GetProcAddress` na IAT sao, eles proprios, o padrao
//      classico de malwares e cheats. Trocar uma string suspeita por duas
//      funcoes suspeitas nao resolve — piora;
//   2. resolver por NOME coloca o nome de volta no binario (nao resolve o
//      problema original).
//
// O que este header faz e' outra coisa:
//
//   - pega o modulo pelo PEB (`InMemoryOrderModuleList` — o `loader` ja' tem a
//     lista; nao precisamos pedir nada a ninguem);
//   - anda na TABELA DE EXPORTS da DLL;
//   - calcula o hash FNV-1a de cada nome e compara com um hash CONSTANTE;
//   - devolve o endereco da funcao.
//
// Nenhum nome de funcao aparece no binario. Nenhuma API de carregamento
// dinamico entra na IAT.
//
// ---------------------------------------------------------------------------
// O QUE ISSO **NAO** RESOLVE
//
//   - o TEXTO do hash nao esconde o EFEITO: com o processo rodando, a janela
//     continua com streamproof aplicado, e quem inspecionar o estado da janela
//     ve'. Ofuscacao de import nao muda comportamento;
//   - `LoadLibrary`/`GetProcAddress` CONTINUAM na IAT do executavel — eles sao
//     usados por outras partes do codigo (o mapper precisa carregar o driver).
//     Este header nao remove aquelas entradas; ele so' nao ACRESCENTA uso de
//     `SetWindowDisplayAffinity`;
//   - o resto da IAT (d3d11, dxgi, dcomp, dwmapi) continua. Um processo que
//     carrega D3D11 nao-jogo ja' e' um perfil suspeito por si so — e isso e'
//     fase seguinte.
//
// ---------------------------------------------------------------------------
// COMO USAR
//
//     #include "../Security/ImportHash.h"
//
//     using SetWDA_t = BOOL (WINAPI*)(HWND, DWORD);
//     static SetWDA_t pSetWDA = (SetWDA_t)ImportHash::ResolveExport(
//         ImportHash::FNV1a_64_W(L"USER32.dll"),
//         ImportHash::FNV1a_64("SetWindowDisplayAffinity"));
//
//     if (pSetWDA) pSetWDA(hwnd, aff);
//
// O ponteiro e' resolvido UMA vez (static local) e a chamada vira um `call`
// indireto. O custo em runtime e' uma indirecao — o mesmo que a IAT pagaria.
// ===========================================================================

#include <windows.h>
#include <winternl.h>   // PTEB, PEB, LDR_DATA_TABLE_ENTRY, UNICODE_STRING
#include <cstdint>
#include <cstddef>
#include <cstring>      // memcpy — usado por LeNoOffset

namespace ImportHash {

// ---------------------------------------------------------------------------
// FNV-1a — 64 bits.
//
// Escolhido por ser trivial de reimplementar nos dois lados (o constexpr que
// gera a constante e o runtime que compara) e por ter avalanche suficiente
// para nomes curtos de API. Nao e' criptografico e nao precisa ser: o alvo e'
// nao ter o NOME no binario, nao resistir a um ataque de forca bruta — quem
// pode rodar codigo no processo ja' pode ler o que quiser.
//
// Mesmas constantes dos dois lados, senao a resolucao nao acha nada e o
// sintoma e' "a funcao nao existe" — que e' o lugar errado pra procurar.
// ---------------------------------------------------------------------------
constexpr uint64_t kFNVOffset = 0xCBF29CE484222325ULL;
constexpr uint64_t kFNVPrime  = 0x00000100000001B3ULL;

constexpr uint64_t FNV1a_64(const char* s, uint64_t h = kFNVOffset)
{
    return (*s == '\0') ? h : FNV1a_64(s + 1, (h ^ (unsigned char)*s) * kFNVPrime);
}

// Versao para literal wide — usada nos nomes de DLL.
constexpr uint64_t FNV1a_64_W(const wchar_t* s, uint64_t h = kFNVOffset)
{
    return (*s == L'\0')
        ? h
        : FNV1a_64_W(s + 1, (h ^ (uint64_t)(unsigned char)(*s & 0xFF)) * kFNVPrime);
}

// ---------------------------------------------------------------------------
// Hash de um nome em ASCII JA' CARREGADO (dentro da tabela de exports).
// Precisa ser identico ao FNV1a_64 acima — se divergir, nada resolve.
// ---------------------------------------------------------------------------
inline uint64_t HashAnsi(const char* s)
{
    uint64_t h = kFNVOffset;
    while (*s) { h ^= (unsigned char)*s++; h *= kFNVPrime; }
    return h;
}

// ---------------------------------------------------------------------------
// ==================== A ARMADILHA DO CASE (MEDIDA) ====================
//
// O loader do Windows NAO normaliza o case do `BaseDllName`. Medido nesta
// maquina, na lista real:
//
//     "ntdll.dll"      <- minusculo
//     "KERNEL32.DLL"   <- MAIUSCULO
//     "KERNELBASE.dll" <- misto
//
// Ou seja: hashear `L"user32.dll"` e comparar com o nome do loader funciona
// por SORTE — depende de como a DLL foi registrada nesta execucao. Se o nome
// vier "USER32.DLL", o hash nao bate e a resolucao falha.
//
// E o sintoma seria o pior tipo: "a funcao nao existe" — que faz procurar bug
// no export walk, nao no hash do nome do modulo.
//
// Solucao: NORMALIZAR PARA MINUSCULO DOS DOIS LADOS. A constante e' calculada
// sobre "user32.dll" ja' minusculo, e o runtime baixa o case antes de hashear.
// Nomes de modulo sao ASCII, entao `| 0x20` resolve sem tabela de locale.
// ---------------------------------------------------------------------------
inline uint64_t HashWideCaseInsensitive(const wchar_t* s)
{
    uint64_t h = kFNVOffset;
    while (*s) {
        unsigned char c = (unsigned char)(*s++ & 0xFF);
        if (c >= 'A' && c <= 'Z') c |= 0x20;
        h ^= (uint64_t)c;
        h *= kFNVPrime;
    }
    return h;
}

// Versao para UNICODE_STRING, que NAO e' terminada em NUL: o limite e' Length.
// Usar a versao acima aqui leria alem do fim do buffer.
inline uint64_t HashUnicodeStringCaseInsensitive(const UNICODE_STRING* u)
{
    uint64_t h = kFNVOffset;
    const USHORT n = (USHORT)(u->Length / sizeof(wchar_t));
    for (USHORT i = 0; i < n; i++) {
        unsigned char c = (unsigned char)(u->Buffer[i] & 0xFF);
        if (c >= 'A' && c <= 'Z') c |= 0x20;
        h ^= (uint64_t)c;
        h *= kFNVPrime;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Le' um campo de um endereco, num offset, sem struct.
//
// POR QUE NAO USAR A STRUCT: eu escrevi uma `LdrEntryView` com paddings
// calculados e o walk parou de funcionar. Um offset errado no MEIO da struct
// desloca todos os campos do fim, e o sintoma e' identico ao de "o modulo nao
// existe" — o lugar errado pra procurar.
//
// Com os offsets medidos como constantes separadas, cada campo e' lido no
// lugar que a MEDICAO confirmou, e nao ha' layout implicito pra errar.
// ---------------------------------------------------------------------------
template <typename T>
inline T LeNoOffset(const BYTE* base, size_t off)
{
    T v;
    memcpy(&v, base + off, sizeof(T));
    return v;
}

// ---------------------------------------------------------------------------
// Acha a BASE de um modulo carregado, pelo PEB.
//
// `InMemoryOrderModuleList` e' uma lista duplamente ligada que o loader do
// Windows mantem. Cada entrada e' um `LDR_DATA_TABLE_ENTRY`, e o nome do
// arquivo fica em `BaseDllName` (UNICODE_STRING).
//
// O `DllBase` e' a base do mapeamento — o `MZ`/`PE` que a resolucao precisa.
// Pegar por aqui evita `GetModuleHandle`, que seria mais uma entrada na IAT e
// mais uma string.
//
// ---------------------------------------------------------------------------
// OS OFFSETS FORAM **MEDIDOS**, NAO COPIADOS.
//
// O `winternl.h` declara `_LDR_DATA_TABLE_ENTRY` e OMITE o `BaseDllName` — e
// os offsets variam entre arquiteturas e versoes do Windows. Chutar seria
// trocar um bug de deteccao por um bug de resolucao, e o sintoma dos dois e' o
// mesmo ("nao achou"), no lugar errado pra procurar.
//
// tools\ldr_offsets.cpp percorre a lista real e imprime, para cada offset
// testado, o que ele encontra como UNICODE_STRING. O offset certo e' o unico
// que produz nome de .dll plausivel em TODAS as entradas. Resultado nesta
// maquina (x64, Win10 22H2):
//
//     offsetof(InLoadOrderLinks)        = 0x00
//     offsetof(InMemoryOrderLinks)      = 0x10
//     offsetof(DllBase)                 = 0x30
//     offsetof(FullDllName)             = 0x48
//     offsetof(BaseDllName)             = 0x58
//
//     [0] ldr_offsets.exe   DllBase=0x...  SizeOfImage=0x28000
//     [1] ntdll.dll         DllBase=0x...
//     [2] KERNEL32.DLL      DllBase=0x...
//     [3] KERNELBASE.dll    DllBase=0x...
// ---------------------------------------------------------------------------
constexpr size_t kOffInMemoryOrderLinks = 0x10;
constexpr size_t kOffDllBase            = 0x30;
constexpr size_t kOffBaseDllName        = 0x58;

inline PVOID ModuleBase(uint64_t nomeHash)
{
    // O TEB em x64: gs:[0x30] = PEB. Nao ha' intrinsic publico pra isso.
    PTEB teb = (PTEB)__readgsqword(0x30);
    if (!teb || !teb->ProcessEnvironmentBlock) return nullptr;

    PEB* peb = teb->ProcessEnvironmentBlock;
    if (!peb->Ldr) return nullptr;

    LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
    if (!head->Flink) return nullptr;

    for (LIST_ENTRY* e = head->Flink; e != head; e = e->Flink) {
        auto* base = (const BYTE*)e - kOffInMemoryOrderLinks;

        const UNICODE_STRING u = LeNoOffset<UNICODE_STRING>(base, kOffBaseDllName);
        if (!u.Buffer || u.Length == 0) continue;
        if (u.Length > 260) continue;                 // guarda contra lixo
        if (u.MaximumLength < u.Length) continue;     // UNICODE_STRING valida

        if (HashUnicodeStringCaseInsensitive(&u) == nomeHash)
            return LeNoOffset<PVOID>(base, kOffDllBase);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Resolve um export por hash.
//
// `base` = base do modulo;  `alvo` = hash do nome da funcao.
//
// Anda a IMAGE_EXPORT_DIRECTORY: tres arrays paralelos — nomes, ordinais e
// enderecos (RVA). Para `i` de 0 a NumberOfNames-1, compara
// HashAnsi(AddressOfNames[i]) com `alvo` e, se casar, devolve
// base + AddressOfFunctions[AddressOfNameOrdinals[i]].
//
// O limite `NumberOfNames` e' o guarda contra a lista corromper: sem ele, um
// indice fora do array viraria leitura de memoria arbitraria.
// ---------------------------------------------------------------------------
inline PVOID ResolveExport(PVOID base, uint64_t alvo)
{
    if (!base) return nullptr;

    auto* dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

    auto* nt = (PIMAGE_NT_HEADERS)((BYTE*)base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return nullptr;

    auto* exp = (PIMAGE_EXPORT_DIRECTORY)((BYTE*)base + dir.VirtualAddress);

    auto* nomes    = (DWORD*)((BYTE*)base + exp->AddressOfNames);
    auto* ordinais = (WORD*) ((BYTE*)base + exp->AddressOfNameOrdinals);
    auto* funcoes  = (DWORD*)((BYTE*)base + exp->AddressOfFunctions);

    // Forwarded exports apontam para DENTRO do diretorio de export. Nao
    // seguimos forwarder: `SetWindowDisplayAffinity` nao e' um, e seguir
    // adicionaria um caminho de erro que nao teria como ser testado.
    const BYTE* dirIni = (BYTE*)exp;
    const BYTE* dirFim = dirIni + dir.Size;

    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        const char* nome = (const char*)base + nomes[i];
        if (HashAnsi(nome) != alvo) continue;

        const DWORD rva = funcoes[ordinais[i]];
        BYTE* end = (BYTE*)base + rva;

        if (end >= dirIni && end < dirFim) return nullptr;   // forwarder
        return end;
    }
    return nullptr;
}

// Conveniencia: resolve um export de um modulo pelo par de hashes.
inline PVOID ResolveExport(uint64_t moduloHash, uint64_t funcaoHash)
{
    return ResolveExport(ModuleBase(moduloHash), funcaoHash);
}

} // namespace ImportHash

// ===========================================================================
// OS HASHES QUE O PROJETO USA
//
// Calculados em tempo de COMPILACAO: se alguem trocar o nome da funcao aqui,
// o valor muda junto e nao ha' como divergir do runtime. E' por isso que a
// constante e' `constexpr` e nao um numero escrito a mao — numero a mao e'
// uma mentira esperando acontecer.
// ===========================================================================
namespace ImportHash {
    // user32.dll / SetWindowDisplayAffinity
    //
    // O nome do modulo e' hasheado JA' MINUSCULO (ver a armadilha do case
    // acima). O nome da funcao NAO e' normalizado: a tabela de exports usa o
    // case exato do simbolo, e "SetWindowDisplayAffinity" e' o case real.
    constexpr uint64_t kUser32_dll = FNV1a_64_W(L"user32.dll");

    constexpr uint64_t kSetWindowDisplayAffinity =
        FNV1a_64("SetWindowDisplayAffinity");
}
