#include "pch.h"
#include "ConfigUtilities.h"
#include "ConfigInstance.h"

json Config;
std::string Path;
ConfigInstances Configs;

// ===========================================================================
// PASTA DE ARTEFATOS — em UM lugar so'.
//
// ANTES:  %USERPROFILE%\Documents\Hunt\
// AGORA:  %LOCALAPPDATA%\RenderCache\
//
// DOIS PROBLEMAS, e o nome e' o menor deles:
//
//   1. O LUGAR. `Documents` e' indexado pelo Windows Search, sincronizado pelo
//      OneDrive e visivel para o usuario. NENHUM programa poe cache la' — entao
//      um arquivo de configuracao em Documents e', por si so', um sinal de
//      "este programa nao se comporta como um programa normal".
//      `%LOCALAPPDATA%` e' onde as coisas de verdade moram; um arquivo a mais
//      la' e' ruido entre centenas, e nao aparece para o usuario.
//
//   2. O NOME. `Hunt` era o nome do cheat. (Ele JA' era montado via `LIT`,
//      entao nao aparecia em texto claro no binario — mas a PASTA NO DISCO
//      aparecia, e e' isso que uma varredura de filesystem procura.)
//
// ISTO NAO E' UMA MEDIDA DE SEGURANCA. E' reducao de superficie: tira dois
// marcadores de graca, sem custo nenhum de funcionalidade. Quem procura o
// processo `mtmacr0.exe` continua achando.
// ===========================================================================
std::filesystem::path PastaDeArtefatos()
{
    const char* lad = getenv(LIT("LOCALAPPDATA"));
    if (lad == nullptr) return {};
    // "RenderCache" cifrado: e' o nome que aparece em %LOCALAPPDATA% e e' o
    // marcador de artefato mais previsivel do conjunto (a pasta, o log e o
    // ui.bin). Cifrar tira o literal do .rdata; o diretorio em disco continua
    // com esse nome — esconder isso exigiria mudar o nome, nao cifra-lo.
    return std::filesystem::path(lad) / LIT("RenderCache");
}

// ---------------------------------------------------------------------------
// Migracao — UMA vez, e sem apagar nada que nao deu para mover.
//
// Sem isto, todo mundo que ja' usou o cheat acorda com as configuracoes
// zeradas. Um "reset silencioso" e' pior que o nome antigo: o usuario perde
// tudo e nao entende por que.
//
// Copia arquivo por arquivo em vez de `rename()` de proposito: `rename` entre
// volumes diferentes falha, `Documents` e `%LOCALAPPDATA%` podem estar em
// drives distintos, e um `rename` que falha no meio pode deixar a pasta pela
// metade. Aqui cada arquivo e' copiado; so' apagamos o original DEPOIS que a
// copia deu certo. Se algo falhar, o arquivo antigo continua la'.
// ---------------------------------------------------------------------------
static void MigrarArtefatos(const std::filesystem::path& nova)
{
    const char* up = getenv(LIT("USERPROFILE"));
    if (up == nullptr || nova.empty()) return;

    std::error_code ec;
    const std::filesystem::path antiga =
        std::filesystem::path(up) / LIT("Documents") / LIT("Hunt");

    if (!std::filesystem::exists(antiga, ec)) return;
    if (std::filesystem::exists(nova, ec))    return;   // ja' migrou antes

    std::filesystem::create_directories(nova, ec);
    if (ec) {
        LOG("[cfg] migracao: nao consegui criar %s (%s) — seguindo na pasta antiga\n",
            nova.string().c_str(), ec.message().c_str());
        return;
    }

    int movidos = 0, falhas = 0;
    for (const auto& e : std::filesystem::directory_iterator(antiga, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;

        const std::filesystem::path destino = nova / e.path().filename();
        std::error_code ec2;
        std::filesystem::copy_file(e.path(), destino,
                                   std::filesystem::copy_options::skip_existing, ec2);
        if (ec2) { falhas++; continue; }

        std::filesystem::remove(e.path(), ec2);   // so' remove com a copia feita
        if (!ec2) movidos++;
    }

    LOG("[cfg] migracao Documents\\Hunt -> %s : %d movidos, %d falhas\n",
        nova.string().c_str(), movidos, falhas);

    // Remove a pasta antiga SE ela ficou vazia. Se sobrou arquivo (falha de
    // copia), ela fica — nao apagamos dado do usuario.
    std::error_code ec3;
    std::filesystem::remove(antiga, ec3);
}

void SetUpConfig()
{
    // O original era uma linha so:
    //
    //     std::filesystem::path filePath = std::filesystem::path(
    //         getenv(LIT("USERPROFILE"))) / LIT("Documents") / LIT("Hunt")
    //         / LIT("Default.json");
    //     std::filesystem::create_directories(filePath.parent_path());
    //     std::filesystem::exists(filePath) ? LoadConfig(...) : SaveConfig(...);
    //
    // std::filesystem LANCA em caso de erro. Qualquer coisa fora do lugar —
    // USERPROFILE ausente, `Documents` sem permissao, caminho muito longo —
    // virava uma excecao que subia pelo Run() e MATAVA o processo antes do
    // overlay. O sintoma no log era so:
    //
    //     [*] Setup configuration loader...
    //     [FATAL] Unhandled Exception: 0xE06D7363
    //
    // 0xE06D7363 e o codigo que o MSVC usa para "excecao C++", nao um
    // access violation. Um erro de configuracao nao pode derrubar o cheat:
    // o overlay e o ESP nao dependem do JSON.
    //
    // Agora falha em voz alta e segue com os defaults.

    std::filesystem::path pasta = PastaDeArtefatos();
    if (pasta.empty()) {
        LOG("[cfg] LOCALAPPDATA ausente no ambiente — usando defaults\n");
        return;
    }

    MigrarArtefatos(pasta);

    std::filesystem::path filePath;
    try {
        filePath = pasta / LIT("Default.json");
        std::filesystem::create_directories(filePath.parent_path());
    } catch (const std::exception& e) {
        LOG("[cfg] nao consegui preparar %s: %s\n",
            pasta.string().c_str(), e.what());
        LOG("[cfg] seguindo com a configuracao padrao\n");
        return;
    }

    try {
        if (std::filesystem::exists(filePath)) LoadConfig(LIT(L"Default"));
        else                                     SaveConfig(LIT(L"Default"));
    } catch (const std::exception& e) {
        LOG("[cfg] Load/Save falhou: %s — seguindo com os defaults\n", e.what());
    }
}
void SaveConfig(const std::wstring& config)
{
    std::string cfg(config.begin(), config.end());
    Config = json::array();
    Config = Configs.ToJson();
    std::string jsonstring = Config.dump() + "\n";

    std::filesystem::path filePath = PastaDeArtefatos() / (cfg + LIT(".json"));

    std::filesystem::create_directories(filePath.parent_path());

    std::ofstream file(filePath);
    if (file.is_open()) {
        file << jsonstring;
        file.close();
    }

}

void LoadConfig(const std::wstring& config)
{
    std::string cfg(config.begin(), config.end());

    std::filesystem::path filePath = PastaDeArtefatos() / (cfg + LIT(".json"));

    if (!std::filesystem::exists(filePath)) {
        return;
    }

    std::ifstream file(filePath);
    if (file.is_open()) {
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string jsonstring = buffer.str();

        if (json::accept(jsonstring)) {
            Config = json::parse(jsonstring);
            Configs.FromJson(Config);

        }
        else {

        }
        file.close();
    }
    else {

    }
}

std::vector<std::wstring> GetAllConfigs()
{
    std::vector<std::wstring> confignames;

    std::filesystem::path directory = PastaDeArtefatos();

    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file()) {
            std::wstring configname = entry.path().filename().stem();
            confignames.push_back(configname);
        }
    }

    return confignames;
}

// ---------------------------------------------------------------------------
// AUTO-SAVE
//
// Chamado quando o menu fecha. Salva no maximo 1x a cada 2 s (o menu pode
// fechar/abrir rapido e nao faz sentido escrever o JSON toda vez).
//
// Engole excecao de proposito: config nao pode derrubar o cheat (foi assim que
// a gente ganhou o [FATAL] 0xE06D7363 no boot).
// ---------------------------------------------------------------------------
void AutoSaveConfig()
{
    static auto ultimo = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto agora = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(agora - ultimo).count() < 2)
        return;
    ultimo = agora;

    try {
        SaveConfig(L"Default");
        LOG("[cfg] auto-save ok (menu fechado)\n");
    } catch (const std::exception& e) {
        LOG("[cfg] auto-save falhou: %s\n", e.what());
    } catch (...) {
        LOG("[cfg] auto-save falhou (excecao desconhecida)\n");
    }
}
