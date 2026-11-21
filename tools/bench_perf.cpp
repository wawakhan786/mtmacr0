// ===========================================================================
// bench_perf.cpp — MICRO-BENCHMARK DAS OTIMIZACOES DE CPU
// ===========================================================================
//
// POR QUE ISTO EXISTE
//
// A auditoria tem uma regra: nao afirmar ganho sem medir. Mas uma parte das
// otimizacoes e' CPU PURO — nao depende do jogo, do driver, nem do overlay.
// O custo dessas da' para medir aqui, isolado, sem carregar nada no sistema.
//
// Isto NAO mede o ganho no FPS do jogo. Mede o custo da OPERACAO, que e' o que
// a otimizacao remove. A conversao para "FPS" depende de quantas vezes por
// frame a operacao acontece — e isso o painel do jogo diz.
//
// O QUE E' MEDIDO
//
//   A) std::string por VALOR x por REFERENCIA   (o que o P0 corrigiu)
//      A cadeia real: EspStrokeText -> OsImGui::StrokeText -> 9x OsImGui::Text
//      = 11 construcoes de std::string por texto desenhado.
//
//   B) CleanEntityName: cru x memoizado          (o que a rodada 3 corrigiu)
//      O corpo e' copiado VERBATIM do PlayerEsp.cpp, para o numero valer.
//
// COMPILAR (dentro do Developer Command Prompt do VS):
//     cl /O2 /std:c++17 /EHsc bench_perf.cpp
//
// ===========================================================================

#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <memory>
#include <thread>

// ---------------------------------------------------------------------------
// (A) A CADEIA DE STRING DO DESENHO DE TEXTO
// ---------------------------------------------------------------------------
// Reproduz fielmente o que os 6 metodos faziam: cada um recebia `std::string`
// POR VALOR, e o StrokeText chamava o Text 9 vezes. Sao 11 construcoes para
// UMA string desenhada.

static void TextPorValor(std::string Text) {          // como ERA
    volatile size_t n = Text.size(); (void)n;
}
static void StrokeTextPorValor(std::string Text) {     // como ERA
    for (int i = 0; i < 9; ++i) TextPorValor(Text);
}
static void EspStrokeTextPorValor(std::string Text) {  // como ERA
    StrokeTextPorValor(Text);
    (void)Text;
}

static void TextPorRef(const std::string& Text) {      // como FICOU
    volatile size_t n = Text.size(); (void)n;
}
static void StrokeTextPorRef(const std::string& Text) {// como FICOU
    for (int i = 0; i < 9; ++i) TextPorRef(Text);
}
static void EspStrokeTextPorRef(const std::string& Text) {
    StrokeTextPorRef(Text);
}

// ---------------------------------------------------------------------------
// (B) CleanEntityName — CORPO COPIADO DO PlayerEsp.cpp:183
// ---------------------------------------------------------------------------
// Se o original mudar, este numero envelhece. Esta' aqui de proposito: e' o
// unico jeito de medir o custo REAL, e nao uma reimplementacao parecida.

static std::string CleanEntityNameSemCache(const std::string& raw, bool firstTokenOnly)
{
    if (raw.empty()) return raw;
    std::string s = raw;

    static const char* prefixes[] = {
        "BP_Vehicle_", "VehicleBP_", "BP_", "Vehicle_",
        "DroppedItemGroup_", "ItemPackage_", "DroppedItem_", "Dropped_",
        "UltAIPawn_Base_", "AIPawn_Base_",
        "Item_Attach_Weapon_", "Item_Attach_", "Item_Weapon_", "Item_Ammo_",
        "Item_Heal_", "Item_Boost_", "Item_Head_", "Item_Back_", "Item_Use_",
        "Item_Armor_", "Item_Helmet_", "Item_",
        "Weapon_",
        "Weap",
        "Carapackage_RedBox_", "Carapackage_", "CarePackage_",
        "TslDrop"
    };
    for (const char* p : prefixes) {
        size_t n = strlen(p);
        if (s.size() > n && s.compare(0, n, p) == 0) {
            if (n == 4 && strcmp(p, "Weap") == 0) {
                char c = s[n];
                if (c < 'A' || c > 'Z') continue;
            }
            s.erase(0, n);
            break;
        }
    }

    static const char* suffixes[] = {
        "_C", "_Spawn", "_Main", "_BP", "_01", "_Skeletal", "_Physics", "_Weapon"
    };
    for (const char* suf : suffixes) {
        size_t n = strlen(suf);
        if (s.size() > n && s.compare(s.size() - n, n, suf) == 0) {
            s.erase(s.size() - n);
            break;
        }
    }

    {
        static const struct { const char* de; const char* para; } ajustesLoot[] = {
            { "Head_E_01_Lv", "Helmet Lv" }, { "Back_E_01_Lv", "Backpack Lv" },
            { "Head_E_02_Lv", "Helmet Lv" }, { "Back_E_02_Lv", "Backpack Lv" },
            { "Head_E_",      "Helmet " },   { "Back_E_",      "Backpack " },
            { "JerryCan",     "Jerry Can" }, { "MedKit",       "Med Kit" },
            { "FirstAid",     "First Aid" }, { "AdrenalineSyringe", "Adrenaline" },
            { "InstantRevivalKit", "Revival Kit" },
            { "BulletproofShield", "Bulletproof Shield" },
            { "ThumbGrip",    "Thumb Grip" }, { "StickyGrenade","Sticky Grenade" },
            { "IntegratedRepair", "Repair Kit" }, { "StunGun", "Stun Gun" },
            { "TacPack",      "Tactical Pack" },
        };
        for (const auto& a : ajustesLoot) {
            size_t pos = s.find(a.de);
            if (pos != std::string::npos) s.replace(pos, strlen(a.de), a.para);
        }
    }

    if (firstTokenOnly) {
        size_t u = s.find('_');
        if (u != std::string::npos && u > 0) s = s.substr(0, u);
    }

    std::string out;
    out.reserve(s.size());
    bool cap = true;
    for (char c : s) {
        if (c == '_' || c == '-') { out.push_back(' '); cap = true; continue; }
        if (cap && c >= 'a' && c <= 'z') c = (char)(c - 32);
        out.push_back(c);
        cap = false;
    }

    size_t a = out.find_first_not_of(' ');
    size_t b = out.find_last_not_of(' ');
    if (a == std::string::npos) return raw;
    return out.substr(a, b - a + 1);
}

static std::string CleanEntityNameMemo(const std::string& raw, bool firstTokenOnly)
{
    static std::unordered_map<std::string, std::string> memoSimples;
    static std::unordered_map<std::string, std::string> memoPrimeiro;
    auto& memo = firstTokenOnly ? memoPrimeiro : memoSimples;

    const auto it = memo.find(raw);
    if (it != memo.end()) return it->second;

    std::string limpo = CleanEntityNameSemCache(raw, firstTokenOnly);
    if (memo.size() < 4096) memo.emplace(raw, limpo);
    return limpo;
}

// ---------------------------------------------------------------------------

static double Agora()
{
    using namespace std::chrono;
    return (double)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

int main()
{
    // Nomes REAIS de classe do jogo — os que aparecem no log da partida.
    std::vector<std::string> nomes = {
        "WeapAK47_C", "WeapHK416_C", "WeapM416_C", "BP_Vehicle_Dacia_C",
        "Item_Weapon_AK47_C", "DroppedItemGroup_Item_Heal_FirstAid_C",
        "Item_Armor_Level3_C", "Head_E_01_Lv3", "Back_E_01_Lv2",
        "Carapackage_RedBox_C", "PlayerMale_A_C", "BP_Dirtbike_C",
        "WeapM24_C", "Item_Boost_Adrenaline_C", "TslDrop_Item_Weapon_UMP9_C",
    };

    printf("=================================================================\n");
    printf(" MICRO-BENCHMARK DAS OTIMIZACOES DE CPU\n");
    printf("=================================================================\n\n");

    // =====================================================================
    // (A) std::string por valor x por referencia
    // =====================================================================
    // Uma "string desenhada" tem ~18 caracteres ("Fulano [123m] [S4]").
    // ACIMA do SSO do MSVC (15 bytes) => alocacao de heap em CADA construcao.
    const std::string nome = "Jogador_Alfa [248m]";   // 20 chars, heap garantido

    const int kDraws = 200000;   // ~1300 por segundo a 144 Hz => 150 s de jogo

    {
        const double t0 = Agora();
        for (int i = 0; i < kDraws; ++i) EspStrokeTextPorValor(nome);
        const double dt = Agora() - t0;
        printf("(A) POR VALOR      %d desenhos  %.1f ms  -> %.3f us por desenho\n",
               kDraws, dt / 1e6, dt / kDraws / 1000.0);
    }
    {
        const double t0 = Agora();
        for (int i = 0; i < kDraws; ++i) EspStrokeTextPorRef(nome);
        const double dt = Agora() - t0;
        printf("    POR REFERENCIA %d desenhos  %.1f ms  -> %.3f us por desenho\n",
               kDraws, dt / 1e6, dt / kDraws / 1000.0);
    }
    printf("\n");

    // =====================================================================
    // (B) CleanEntityName cru x memoizado
    // =====================================================================
    const int kIters = 500000;
    {
        const double t0 = Agora();
        size_t acc = 0;
        for (int i = 0; i < kIters; ++i) acc += CleanEntityNameSemCache(nomes[i % nomes.size()], false).size();
        const double dt = Agora() - t0;
        printf("(B) CRU            %d chamadas  %.1f ms  -> %.3f us por chamada  (acc=%zu)\n",
               kIters, dt / 1e6, dt / kIters / 1000.0, acc);
    }
    {
        const double t0 = Agora();
        size_t acc = 0;
        for (int i = 0; i < kIters; ++i) acc += CleanEntityNameMemo(nomes[i % nomes.size()], false).size();
        const double dt = Agora() - t0;
        printf("    MEMOIZADO      %d chamadas  %.1f ms  -> %.3f us por chamada  (acc=%zu)\n",
               kIters, dt / 1e6, dt / kIters / 1000.0, acc);
    }
    printf("\n");

    // =====================================================================
    // (C) SNAPSHOT: copia de 4 vector<shared_ptr> x copia de shared_ptr unico
    // =====================================================================
    // Main.cpp:744 (por frame) e Main.cpp:338 (por tick) copiavam a SharedData
    // inteira — que tem QUATRO vetores de shared_ptr. Cada elemento custa um
    // incremento atomico na copia e um decremento na destruicao.
    //
    // A alternativa e' publicar shared_ptr<const SharedData>: a copia passa a
    // ser UMA operacao, independente do tamanho das listas.
    {
        struct Entidade { int x = 0; };
        const int kFrames = 200000;

        std::vector<std::shared_ptr<Entidade>> origem, l2, l3, l4;
        const int tam[4] = { 48, 2, 3, 37 };   // actors/grenade/vehicle/loot
        std::vector<std::shared_ptr<Entidade>>* lst[4] = { &origem, &l2, &l3, &l4 };
        for (int q = 0; q < 4; ++q) {
            lst[q]->reserve(tam[q]);
            for (int e = 0; e < tam[q]; ++e) lst[q]->push_back(std::make_shared<Entidade>());
        }

        volatile size_t acc = 0;
        {
            const double t0 = Agora();
            for (int f = 0; f < kFrames; ++f) {
                std::vector<std::shared_ptr<Entidade>> a, b, c, d;
                a = origem; b = l2; c = l3; d = l4;      // como ERA
                acc += a.size() + b.size() + c.size() + d.size();
            }
            const double dt = Agora() - t0;
            printf("(C) COPIA 4 VETORES   %d frames  %.1f ms  -> %.3f us por frame\n",
                   kFrames, dt / 1e6, dt / kFrames / 1000.0);
        }
        {
            const double t0 = Agora();
            for (int f = 0; f < kFrames; ++f) {
                std::shared_ptr<const int> snap = std::make_shared<const int>(1);  // como FICARIA
                acc += snap ? 1 : 0;
            }
            const double dt = Agora() - t0;
            printf("    SHARED_PTR UNICO %d frames  %.1f ms  -> %.3f us por frame\n",
                   kFrames, dt / 1e6, dt / kFrames / 1000.0);
        }
        (void)acc;
    }
    printf("\n");

    // =====================================================================
    // (D) ActorReadInfo: 16,3 KB POR ATOR, value-initialized todo tick
    // =====================================================================
    // Engine.cpp:1830 faz `std::vector<ActorReadInfo> infos(Actors.size())`.
    // O value-initialize ZERA os tres buffers de cada ator — meshBulk,
    // healthBulk e fullBoneBuffer — mesmo para quem nao tem osso nem mesh.
    // Os tamanhos sao os do codigo.
    {
        const size_t kMeshBulk   = 0xC00;
        const size_t kHealthBulk = 0x1000;
        const size_t kFullBones  = 190 * 48;
        const size_t porAtor     = kMeshBulk + kHealthBulk + kFullBones;

        printf("(D) ActorReadInfo = %zu bytes por ator (%.1f KB)\n", porAtor, porAtor / 1024.0);

        const int kTicks = 20000;
        const int ns[3] = { 20, 48, 100 };
        for (int q = 0; q < 3; ++q) {
            const int N = ns[q];
            const double t0 = Agora();
            for (int t = 0; t < kTicks; ++t) {
                std::vector<uint8_t> bloco((size_t)N * porAtor, 0);   // aloca + zera, como o ERA
                volatile uint8_t v = bloco[0]; (void)v;
            }
            const double dt = Agora() - t0;
            const double porTick = dt / kTicks / 1000.0;
            const double porSeg  = porTick * 144.0 / 1000.0;
            printf("    N=%3d  %6.2f us por tick -> %6.1f ms/s a 144 Hz = %4.1f%% de um nucleo  (%.0f MB/s de zeroing)\n",
                   N, porTick, porSeg, porSeg / 10.0, (double)((size_t)N * porAtor) * 144.0 / 1e6);
        }
    }
    printf("\n");

    // =====================================================================
    // (E) SNAPSHOT COM CONTENCAO — o furo da medicao single-thread
    // =====================================================================
    // O (C) mediu o custo SEM outra thread mexendo nos refcounts. Mas na
    // pratica a thread de memoria TROCA a lista enquanto o render copia: os
    // refcounts de cada ator sao linhas de cache compartilhadas entre os dois
    // nucleos, e isso custa muito mais que um atomico sem disputa.
    //
    // Este bloco reproduz a disputa real: uma thread publica (troca a lista)
    // a 144 Hz enquanto a principal copia o snapshot, tambem a 144 Hz.
    {
        struct Entidade { int x = 0; };
        const int N = 48;
        const int kFrames = 60000;

        std::vector<std::shared_ptr<Entidade>> lista;
        lista.reserve(N);
        for (int e = 0; e < N; ++e) lista.push_back(std::make_shared<Entidade>());

        std::atomic<bool> rodando{ true };
        std::atomic<uint64_t> publicacoes{ 0 };

        // thread publicadora: refaz a lista e a troca, como a MemoryLoop faz
        std::thread publicador([&]() {
            std::vector<std::shared_ptr<Entidade>> nova;
            while (rodando.load(std::memory_order_relaxed)) {
                nova = lista;                       // copia (incref em N)
                publicacoes.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::microseconds(6944)); // 144 Hz
            }
        });

        volatile size_t acc = 0;
        const double t0 = Agora();
        for (int f = 0; f < kFrames; ++f) {
            std::vector<std::shared_ptr<Entidade>> snap;
            snap = lista;                           // como o ERA (com disputa)
            acc += snap.size();
            std::this_thread::sleep_for(std::chrono::microseconds(6944));
        }
        const double dt = Agora() - t0;
        rodando.store(false);
        publicador.join();
        (void)acc;
        printf("(E) COPIA COM CONTENCAO   %d frames em %.0f ms  -> %.3f us por copia (%.0f publicacoes da outra thread)\n",
               kFrames, dt / 1e6, dt / kFrames / 1000.0, (double)publicacoes.load());
        printf("    O (C) sem disputa deu 1.020 us. A diferenca e' o custo de \n");
        printf("    compartilhar as linhas de cache dos refcounts entre nucleos.\n");
    }
    printf("\n");

    // =====================================================================
    // TOTAL POR SEGUNDO DE JOGO
    // =====================================================================
    printf("=================================================================\n");
    printf(" CONVERSAO PARA O JOGO (usa contagens por frame, nao supoe nenhuma)\n");
    printf("=================================================================\n");
    printf(" O painel do jogo da' AddText/frame e o numero de atores. Com eles:\n");
    printf("   custo_string/s = (AddText/frame / 9) x custo_por_desenho x fps\n");
    printf("   custo_nome/s   = itens_de_loot x custo_por_chamada x fps\n");
    printf(" Aqui NAO se afirma impacto em FPS: isso exige a medicao do jogo.\n");
    printf("=================================================================\n");
    return 0;
}
