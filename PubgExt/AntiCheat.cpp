// ===========================================================================
// AntiCheat.cpp — implementacao. Ver o cabecalho para o desenho e o porque.
// ===========================================================================

// ---------------------------------------------------------------------------
// ORDEM DOS INCLUDES — nao e' estilo, e' o que compila.
//
// `AntiCheat.h` inclui <windows.h>. Se ele viesse DEPOIS de <winternl.h>, as
// declaracoes dos tipos do Win32 se repetiriam e o compilador acusaria
// redefinicao. Por isso o header do projeto vem primeiro.
//
// `XorStr.h` vem em seguida porque este arquivo usa LIT() nos identificadores
// de anti-cheat. Ele NAO esta' no pch.h deste projeto (o AntiCheat.cpp compila
// com PrecompiledHeader=NotUsing), entao o include e' explicito.
// ---------------------------------------------------------------------------
#include "AntiCheat.h"
#include "../Security/XorStr.h"       // LIT(): cifra os nomes de anti-cheat
#include "../Security/ImportHash.h"   // FNV1a_64 constexpr (hash dos nomes de processo)

#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwchar>

// ---------------------------------------------------------------------------
// LOG — DECLARADO A MAO, E NAO PELO `Pch.h`.
//
// A tentativa obvia era `#include "Pch.h"`. Ela ARRASTA O PROJETO INTEIRO
// (WinSock, ImGui, json) e o resultado foi um muro de redefinicao de tipo:
//
//     ws2def.h(246): error C2011: 'sockaddr': redefinicao do tipo 'struct'
//     WinSock2.h(136): error C2011: 'fd_set': redefinicao
//     ...
//
// O Pch.h inclui <windows.h> com WIN32_LEAN_AND_MEAN e mais um monte de coisa;
// combinar isso com o <windows.h>/<winternl.h> que este arquivo ja' precisa,
// nessa ordem, quebra.
//
// O que este arquivo quer do Pch.h e' UMA funcao. Entao declara-se UMA funcao.
// `BaseLog` esta' definida no Main.cpp com vinculo C++ — o `extern "C"` nao
// muda o simbolo (a declaracao tem que ser IDENTICA nos dois lados).
// ---------------------------------------------------------------------------
extern void BaseLog(const char* fmt, ...);
#define LOG(fmt, ...) BaseLog(fmt, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// NtQuerySystemInformation — NAO REDECLARAR.
//
// O <winternl.h> JA' declara:
//
//     NTSTATUS NTAPI NtQuerySystemInformation(
//         SYSTEM_INFORMATION_CLASS SystemInformationClass, ...);
//
// A tentativa anterior de declarar de novo "a mao" deu C2733: nao da' pra
// sobrecarregar funcao com vinculo externo "C". O que o header publico NAO
// declara e' o valor de `SystemModuleInformation` (11, anterior ao Win32) e as
// structs que ele preenche — e essas continuam declaradas aqui.
//
// O cast para SYSTEM_INFORMATION_CLASS e' obrigatorio por causa da assinatura
// tipada: `NtQuerySystemInformation(11, ...)` nao compila.
// ---------------------------------------------------------------------------
#define ACH_SYSTEM_MODULE_INFORMATION 11

typedef struct _ACH_RTL_PROCESS_MODULE_INFORMATION {
    HANDLE Section;
    PVOID  MappedBase;
    PVOID  ImageBase;
    ULONG  ImageSize;
    ULONG  Flags;
    USHORT LoadOrderIndex;
    USHORT InitOrderIndex;
    USHORT LoadCount;
    USHORT OffsetToFileName;
    UCHAR  FullPathName[256];
} ACH_RTL_PROCESS_MODULE_INFORMATION;

typedef struct _ACH_RTL_PROCESS_MODULES {
    ULONG NumberOfModules;
    ACH_RTL_PROCESS_MODULE_INFORMATION Modules[1];   // flexivel
} ACH_RTL_PROCESS_MODULES;

namespace AntiCheat {

// ---------------------------------------------------------------------------
// Os nomes que indicam anti-cheat. SUBSTRING, case-insensitive.
//
// Por que substring e nao igualdade: o modulo pode chegar com sufixo de versao,
// caminho curto, ou nome de arquivo temporario. O prefixo e' o que se mantem.
//
//   BEDaisy   -> BattlEye (PUBG, Rainbow Six, DayZ, Arma)
//   BEKernel  -> variante antiga de BattlEye
//   Zak       -> Zakynthos (Krafton / PUBG internal anti-cheat)
//   xenuine   -> Xenuine (PUBG, parte user-mode mas tem modulo)
//   EasyAnti  -> EAC (usado em outros titulos; custa nada incluir)
//
// ---------------------------------------------------------------------------
// CIFRADOS COM LIT() NA 1.3 — e o motivo nao e' estetico.
//
// Estes nomes estavam em texto claro no .exe. Isso significa que um `strings`
// no arquivo respondia, de uma vez:
//
//     "isto e' um cheat" + "ele procura BattlEye e Zakynthos"
//
// Um anti-cheat nao precisa de heuristica nenhuma pra isso: `strings mtmacr0.exe
// | findstr -i bedaisy` e' a assinatura inteira. O LIT() resolve a string em
// tempo de execucao e a chave vem do __TIME__ do build (ver XorStr.h), entao
// cada compilacao gera uma chave diferente e nenhum literal sobra no .rdata.
//
// O QUE ISSO **NAO** PROTEGE — e vale dizer, porque ofuscacao que promete mais
// do que entrega e' pior que nenhuma:
//
//   - com o processo RODANDO, a string existe na heap (o LIT() materializa no
//     stack e o std::string abaixo copia). Quem pode ler a memoria do nosso
//     processo le' assim mesmo;
//   - o NOME DO MODULO configurado (BattlEye.cli em alguns titulos) continua
//     vindo da config em disco.
//
// O ganho e' concreto e e' o que estava faltando: o binario EM REPOUSO nao
// entrega o alvo.
// ---------------------------------------------------------------------------
const std::vector<std::string>& PadroesAntiCheat()
{
    // ==================== "zak" FOI REMOVIDO (2.0) ====================
    //
    // A lista tinha `zakynthos` E `zak`. O `zak` pareceu util ("pega variante
    // nova que eu nao conheco"), mas e' um SUBSTRING de 3 letras num matcher
    // que casa por substring — e o alvo nao e' so' um nome de arquivo, e' o
    // CAMINHO inteiro do modulo (FullPathName).
    //
    // Tres letras casam em qualquer coisa. `zak` aparece em:
    //
    //     C:\Windows\System32\drivers\**zak**...
    //     ...\NVIDIA\**Zak**...
    //
    // E o padrao mais curto e' o que MAIS falso positivo gera, porque a chance
    // de uma sequencia curta aparecer em texto aleatorio cresce com o quadrado
    // do inverso do comprimento.
    //
    // PRECEDENTE NESTE PROJETO: `"Weap"` casando com `"WeaponItem"` (ver o
    // item 4 da licao transversal no ARQUITETURA.md). Padrao curto em matcher
    // por substring e' o mesmo bug, com outro nome.
    //
    // O custo de tirar: uma build futura de Zakynthos com nome que nao contenha
    // "zakynthos" nao seria pega por este padrao. Mas ela provavelmente nao
    // conteria "zak" tambem — e se contiver, o nome real e' que tem que entrar
    // na lista, nao um truncamento dele.
    static const std::vector<std::string> padroes = {
        LIT("bedaisy"),
        LIT("bekernel"),
        LIT("zakynthos"),
        LIT("xenuine"),
        LIT("easyanti"),
    };
    return padroes;
}

// ---------------------------------------------------------------------------
// ModulosDeKernel — a fonte que decide.
//
// SizeofImage e' grande o bastante pra cobrir SystemModuleInformation. Se o
// driver de kernel do Windows crescer alem do teto, a chamada devolve
// STATUS_INFO_LENGTH_MISMATCH e a lista sai vazia — degradacao SEGURA: sem
// lista, `Ativo()` nao bloqueia por modulo, e o gate segue pelas outras fontes.
// Melhor nao bloquear do que bloquear errado.
// ---------------------------------------------------------------------------
std::vector<std::string> ModulosDeKernel()
{
    std::vector<std::string> nomes;

    ULONG tam = 2 * 1024 * 1024;             // 2 MB de teto
    std::vector<UCHAR> buffer(tam);

    NTSTATUS st = NtQuerySystemInformation(
        (SYSTEM_INFORMATION_CLASS)ACH_SYSTEM_MODULE_INFORMATION,
        buffer.data(), tam, &tam);

    // -----------------------------------------------------------------------
    // A FALHA SAI NO LOG — e este e' o ponto.
    //
    // `if (st < 0) return nomes;` degrada com seguranca (lista vazia = nenhum
    // modulo casa = nao bloqueia). Mas degradava EM SILENCIO. Se o
    // NtQuerySystemInformation falhar no contexto do processo — e ele FALHA sem
    // privilegio (medido: STATUS_ACCESS_DENIED 0xC0000022 neste ambiente) — a
    // fonte que DECIDE a deteccao simplesmente nao roda, e ninguem fica sabendo.
    //
    // O sintoma seria o pior tipo: o gate "funcionando", bloqueando ou liberando
    // por PROCESSO e SERVICO, sem a fonte forte nunca ter opinado. E quem le' o
    // log acharia que a deteccao por modulo estivesse funcionando.
    // -----------------------------------------------------------------------
    if (st < 0) {
        LOG("[ac] NtQuerySystemInformation(SystemModuleInformation) FALHOU: 0x%08lX\n",
            (unsigned long)st);
        LOG("[ac] -> MODULE-based detection will not run in this session.\n");
        LOG("[ac] -> The gate will rely solely on process and service checks (the weaker sources).\n");
        LOG("[ac] -> 0xC0000022 = access denied: please run the cheat as an Administrator.\n");
        return nomes;
    }

    const ACH_RTL_PROCESS_MODULES* mods = (const ACH_RTL_PROCESS_MODULES*)buffer.data();
    const ULONG n = mods->NumberOfModules;

    // Quantos modulos foram lidos. Se vier 0 ou um numero absurdo, o problema
    // esta' na leitura e nao no matcher.
    LOG("[ac] loaded kernel modules: %lu\n", (unsigned long)n);

    for (ULONG i = 0; i < n; i++) {
        const ACH_RTL_PROCESS_MODULE_INFORMATION& m = mods->Modules[i];
        const char* full = (const char*)m.FullPathName;

        // OffsetToFileName aponta pro nome do arquivo dentro do caminho.
        const char* nome = full + m.OffsetToFileName;
        if (m.OffsetToFileName >= sizeof(m.FullPathName)) continue;

        nomes.push_back(nome);
    }

    return nomes;
}

// ---------------------------------------------------------------------------
// Processos anti-cheat conhecidos. Comparados sem case.
//
// ==================== AQUI EU COMETI O MESMO BUG, DUAS VEZES ====================
//
// A primeira versao usava `static const char*` com os nomes em texto claro —
// iguais a varredura de string. Troquei por LIT():
//
//     static const char* kProcessos[] = {
//         LIT("BEService.exe"), ...              // <- ERRADO
//     };
//
// E isso tem o MESMO defeito do `MakeClassName` do overlay: `LIT()` devolve
// `crypt_get()`, que aponta para o `_storage` de um objeto TEMPORARIO. O
// temporario morre no fim da expressao do inicializador, e o array fica com
// ponteiros para stack ja' sobrescrita.
//
// A CONSEQUENCIA AQUI E' PIOR QUE NO OVERLAY. La' o sintoma foi cosmetico (o
// nome da classe virou `_9797`). Aqui e' o GATE DE ANTI-CHEAT comparando
// `_stricmp(nome, alvo)` contra lixo — ou seja, a deteccao podia simplesmente
// NAO DETECTAR, em silencio, e o cheat mapear com o BattlEye ativo.
//
// Nao deu crash por sorte: o `static` mantem o array vivo e o espaco de stack
// continuou legivel. "Nao crashou" nao e' "esta' certo" — e essa e' a licao
// que ja' custou tela azul neste projeto.
//
// A CORRECAO NAO E' TROCAR POR std::string. E' TIRAR O PONTEIRO DA EQUACAO:
// compara-se HASH contra HASH. O nome-alvo existe apenas como numero de 64
// bits, calculado em tempo de compilacao — nao ha' ponteiro, nao ha'
// temporario, e nao ha' a string alvo no binario em momento nenhum.
//
// O hash e' o mesmo FNV-1a do ImportHash.h, com normalizacao de caixa: os
// nomes de processo chegam em caixa variavel e a comparacao original era
// `_stricmp` (case-insensitive).
// ---------------------------------------------------------------------------
static const uint64_t kProcessosHash[] = {
    ImportHash::FNV1a_64("BEService.exe"),
    ImportHash::FNV1a_64("BEService_x64.exe"),
    ImportHash::FNV1a_64("BEService_pubg.exe"),
    ImportHash::FNV1a_64("BEDaisy.exe"),
    ImportHash::FNV1a_64("Zakynthos.exe"),
    ImportHash::FNV1a_64("xenuine.exe"),
};

// Hash de um nome de processo com a caixa baixada — equivalente ao `_stricmp`
// que a comparacao usava antes.
static uint64_t HashProcesso(const char* nome)
{
    uint64_t h = ImportHash::kFNVOffset;
    for (; *nome; nome++) {
        unsigned char c = (unsigned char)*nome;
        if (c >= 'A' && c <= 'Z') c |= 0x20;
        h ^= (uint64_t)c;
        h *= ImportHash::kFNVPrime;
    }
    return h;
}

static std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });
    return s;
}

static bool ContemAlgumPadrao(const std::string& alvo, std::string* qual)
{
    const std::string baixo = ToLower(alvo);
    for (const auto& p : PadroesAntiCheat()) {
        if (baixo.find(p) != std::string::npos) {
            if (qual) *qual = alvo;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Checar — a varredura completa, na ordem de forca.
// ---------------------------------------------------------------------------
Estado Checar()
{
    Estado e;

    // ---- 1) MODULO DE KERNEL (a que decide) ----
    {
        std::vector<std::string> mods = ModulosDeKernel();
        for (const auto& m : mods) {
            std::string qual;
            if (ContemAlgumPadrao(m, &qual)) {
                e.moduloKernel = true;
                e.modulosBateram.push_back(qual);
            }
        }
    }

    // ---- 2) PROCESSO ----
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    // nome UTF-16 -> ASCII simples (os alvos sao ASCII)
                    char nome[64] = {0};
                    for (int i = 0; i < 63 && pe.szExeFile[i]; i++)
                        nome[i] = (pe.szExeFile[i] < 128) ? (char)pe.szExeFile[i] : '?';

                    // HASH contra HASH — o nome-alvo nao existe como string em
                    // lugar nenhum. Ver o bloco acima sobre o ponteiro dangling.
                    const uint64_t alvoHash = HashProcesso(nome);
                    for (uint64_t h : kProcessosHash) {
                        if (alvoHash == h) {
                            e.processo = true;
                            e.processosBateram.push_back(nome);
                            break;
                        }
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
    }

    // ---- 3) SERVICO INSTALADO ----
    // So' sinal de INSTALACAO. Nao entra em Ativo() — ver o cabecalho.
    //
    // As chaves sao montadas com LIT(L"...") — o caminho do registry carrega o
    // nome do anti-cheat no ULTIMO componente, entao o literal inteiro era
    // achavel. O prefixo e' comum aos tres, e so' o sufixo e' cifrado
    // separadamente, o que tambem deixa claro no codigo O QUE muda entre eles.
    {
        // -------------------------------------------------------------------
        // `std::wstring` e NAO `const wchar_t*`.
        //
        // Este array era `static const wchar_t* const kServicos[]` com LIT(L"...")
        // — o MESMO ponteiro dangling dos kProcessos. Passou batido porque o
        // sintoma aqui seria "o servico nao foi encontrado", que e' indistinguivel
        // de "o servico nao existe".
        //
        // Com `std::wstring`, o construtor COPIA dentro da expressao, enquanto o
        // temporario do LIT() ainda esta' vivo. E' o mesmo raciocinio do
        // MakeClassName.
        // -------------------------------------------------------------------
        static const std::wstring kServicos[] = {
            LIT(L"BEService"),
            LIT(L"BEDaisy"),
            LIT(L"Zakynthos"),
        };

        for (const std::wstring& servico : kServicos) {
            std::wstring caminho = LIT(L"SYSTEM\\CurrentControlSet\\Services\\");
            caminho += servico;

            HKEY h = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, caminho.c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS) {
                e.servico = true;
                e.servicosBateram.push_back(std::string(servico.begin(), servico.end()));
                RegCloseKey(h);
            }
        }
    }

    // ---- 4) DEVICE ----
    // O mais fraco: o BEDaisy fecha o device pra user-mode. Um erro de acesso
    // nao distingue "nao existe" de "existe e me barrou", entao isto e'
    // confirmacao, nunca decisao.
    //
    // O `\\.\` vai separado do nome: cifrar o caminho inteiro de uma vez
    // gastaria mais chave e nao esconderia nada a mais — o que identifica o
    // alvo e' o nome.
    {
        std::wstring alvo = LIT(L"\\\\.\\");
        alvo += LIT(L"BEDaisy");

        HANDLE h = CreateFileW(alvo.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            e.device = true;
            CloseHandle(h);
        }
    }

    return e;
}

// ===========================================================================
// ChecarComRelatorio — o veredito COM o de-para, para o botao MAP NOW.
//
// O `Checar()` devolve o resultado e nao conta como chegou nele. Este conta:
// quantos modulos, qual casou, com qual padrao, e o que cada fonte decidiu.
//
// E' a diferenca entre "ANTI-CHEAT ATIVO" (que nao diz o que fazer) e
// "ANTI-CHEAT ATIVO porque o modulo X casou com o padrao Y" (que diz
// exatamente onde olhar).
// ===========================================================================
Estado ChecarComRelatorio()
{
    LOG("[ac] ================ varredura de anti-cheat ================\n");

    // ---- 1) MODULOS DE KERNEL (a fonte que decide) ----
    // O `ModulosDeKernel` ja' loga a contagem e a falha da chamada.
    {
        std::vector<std::string> mods = ModulosDeKernel();

        LOG("[ac] padroes ativos: ");
        for (const auto& p : PadroesAntiCheat()) LOG("%s ", p.c_str());
        LOG("\n");

        int casaram = 0;
        for (const auto& m : mods) {
            const std::string baixo = ToLower(m);
            for (const auto& p : PadroesAntiCheat()) {
                if (baixo.find(p) != std::string::npos) {
                    // ESTA linha e' a que faltava: o modulo E o padrao.
                    LOG("[ac]   CASA modulo='%s'  padrao='%s'\n", m.c_str(), p.c_str());
                    casaram++;
                    break;
                }
            }
        }
        if (casaram == 0)
            LOG("[ac]   nenhum dos %zu modulos casou com nenhum padrao\n", mods.size());
    }

    // ---- 2, 3, 4) as fontes fracas ----
    Estado e = Checar();

    LOG("[ac] verdict:\n");
    LOG("[ac]   kernel module : %s\n", e.moduloKernel ? "Active" : "Clean");
    LOG("[ac]   process         : %s\n", e.processo     ? "Active" : "Clean");
    LOG("[ac]   service          : %s\n", e.servico      ? "Installed (doesn't lock automatically)" : "Absent");
    LOG("[ac]   device           : %s\n", e.device       ? "Active" : "doesn't respond");
    LOG("[ac]   -> Active() = %s\n", e.Ativo() ? "YES (blocks)" : "NO (release)");
    LOG("[ac]   resume: %s\n", e.Resumo().c_str());
    LOG("[ac] ========================================================\n");

    return e;
}

std::string Estado::Resumo() const
{
    std::string s;

    auto add = [&s](const char* tag, const std::vector<std::string>& v) {
        if (v.empty()) return;
        if (!s.empty()) s += ", ";
        s += tag;
        s += ": ";
        for (size_t i = 0; i < v.size(); i++) {
            if (i) s += "|";
            s += v[i];
        }
    };

    add(LIT("modulo"),   modulosBateram);
    add(LIT("processo"), processosBateram);
    add(LIT("servico"),  servicosBateram);
    if (device) {
        if (!s.empty()) s += ", ";
        s += LIT("device: ");
        s += LIT("\\Device\\BEDaisy");
    }

    if (s.empty()) s = LIT("nenhum sinal");
    return s;
}

} // namespace AntiCheat
