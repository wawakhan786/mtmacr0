#pragma once
// ===========================================================================
// Perf.h — INSTRUMENTACAO DE DESEMPENHO
// ===========================================================================
//
// POR QUE ISTO EXISTE
//
// A auditoria de desempenho so' pode afirmar o que MEDIU. "Ficou mais rapido"
// sem numero e' chute, e este projeto ja' pagou caro por conclusao sem medicao
// (o `MaskRight`, o `srand`, o `NameIndexShlMask` — tres "consertos" em cima de
// sintoma).
//
// Sem instrumentacao, a unica forma de responder "o ESP custa quanto?" era
// opiniao. Com ela, cada otimizacao tem antes/depois no mesmo cenario.
//
// ===========================================================================
// REGRAS DE PROJETO (para a instrumentacao nao virar o problema)
// ===========================================================================
//
// 1. CUSTO PROXIMO DE ZERO quando desligada. Os contadores sao `std::atomic`
//    com `memory_order_relaxed` — em x86 um `lock xadd` que ninguem disputa e'
//    barato, mas ainda e' uma instrucao atomica. Por isso o registro e' um
//    PONTEIRO NULO quando desligado: o `if` preve' bem e o custo vira um teste
//    de ponteiro.
//
// 2. NAO ESCREVE ARQUIVO. O usuario foi explicito: nada de log.txt. A saida e'
//    um painel do ImGui, ligado pela aba DEBUG, so' quando se esta' medindo.
//
// 3. NAO ALTERA O COMPORTAMENTO MEDIDO. Nao muda ordem, nao muda frequencia,
//    nao muda o que e' chamado. So' le' o relogio em volta do que ja' roda.
//
// 4. TEMPO DE PAREDE, NAO CPU. `steady_clock` mede tempo decorrido — numa
//    thread que dorme, isso inclui o sono. Por isso o que se mede aqui sao as
//    FASES (o trabalho em si), nunca o loop inteiro.
//
// ===========================================================================
// O QUE SE MEDE, E POR QUE CADA UM
// ===========================================================================
//
//   renderFrame / buildNs    -> quanto custa MONTAR o frame (ImGui + ESP)
//   presentNs                -> quanto custa APRESENTAR (inclui o vsync!)
//   memTick / memNs          -> quanto custa um tick da thread de memoria
//   batchCalls / batchEntries-> quantas leituras de memoria por segundo
//   ioctlNs                  -> quanto tempo se gasta DENTRO do driver
//   strCalls / strCopies     -> construcoes de std::string no caminho de desenho
//   snapshotNs / snapshotN   -> copia da SharedData (o snapshot por frame)
//
// `presentNs` merece aviso: com `Present(1,0)` ele INCLUI a espera do vblank,
// entao ele mede ~1/refresh por definicao. Nao serve para comparar backends —
// para isso o que vale e' o custo do DWM medido FORA do processo.
// ===========================================================================

#include <atomic>
#include <chrono>
#include <cstdint>

namespace Perf {

// ---------------------------------------------------------------------------
// LIGA/DESLIGA. Desligado, `Ativo()` e' falso e os macros viram quase nada.
// ---------------------------------------------------------------------------
inline std::atomic<bool> g_ativo{ false };

inline bool Ativo() { return g_ativo.load(std::memory_order_relaxed); }
inline void Ligar(bool v) { g_ativo.store(v, std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// CONTADORES ACUMULADOS (monotonicos — quem calcula taxa e' a UI)
// ---------------------------------------------------------------------------
struct Contadores {
    std::atomic<uint64_t> renderFrames{ 0 };
    std::atomic<uint64_t> buildNs{ 0 };
    std::atomic<uint64_t> presentNs{ 0 };
    std::atomic<uint64_t> memTicks{ 0 };
    std::atomic<uint64_t> memNs{ 0 };
    std::atomic<uint64_t> batchCalls{ 0 };
    std::atomic<uint64_t> batchEntries{ 0 };
    std::atomic<uint64_t> singleReads{ 0 };
    std::atomic<uint64_t> ioctlNs{ 0 };
    std::atomic<uint64_t> strCalls{ 0 };
    std::atomic<uint64_t> strCopies{ 0 };
    std::atomic<uint64_t> snapshotNs{ 0 };
    std::atomic<uint64_t> snapshotN{ 0 };
    std::atomic<uint64_t> espActores{ 0 };
    std::atomic<uint64_t> oclusoes{ 0 };   // vezes que o Present devolveu DXGI_STATUS_OCCLUDED

    // -----------------------------------------------------------------------
    // TEXTO — o maior custo de render, e a auditoria so' tinha ARITMETICA.
    //
    // `StrokeText` chama `Text` 9x por string (8 deslocamentos de contorno +
    // o principal), e `Text` chama `CalcTextSizeA` quando precisa centralizar.
    // O auditor estimou "11 medicoes por string, 10 redundantes" e "10.800
    // quads por frame" — numeros CALCULADOS, nao medidos.
    //
    // Estes dois contadores transformam a estimativa em fato: `addText` por
    // frame diz quantos quads de texto saem (x glifos), e `calcText` diz
    // quantas vezes a string foi varrida so' para medir.
    // -----------------------------------------------------------------------
    std::atomic<uint64_t> addText{ 0 };
    std::atomic<uint64_t> calcText{ 0 };
    std::atomic<uint64_t> memoHit{ 0 };
    std::atomic<uint64_t> memoMiss{ 0 };

    // -----------------------------------------------------------------------
    // DIVISAO DO TEMPO POR SUBSISTEMA (dentro do tick de memoria)
    //
    // O total (`memNs`) diz SE a aquisicao satura. So' a divisao diz ONDE.
    // Sao coisas diferentes, e sem a segunda a lista de suspeitos continua
    // sendo hipotese: `UpdatePlayers` pode ser 90% do tick ou 10%.
    //
    // Precisa ser por-subsistema porque a ordem de ataque MUDA: se 80% esta'
    // em `UpdatePlayers`, o alvo e' o `ActorReadInfo`/healthBulk. Se esta' em
    // `UpdateWeapons`, e' o cache de nome de arma. Se esta' no IOCTL puro e
    // espalhado, o alvo e' o lote no driver.
    // -----------------------------------------------------------------------
    std::atomic<uint64_t> nsCache{ 0 };
    std::atomic<uint64_t> nsPlayers{ 0 };
    std::atomic<uint64_t> nsWeapons{ 0 };
    std::atomic<uint64_t> nsGrenades{ 0 };
    std::atomic<uint64_t> nsVehicles{ 0 };
    std::atomic<uint64_t> nsLoot{ 0 };
    std::atomic<uint64_t> nsViewMatrix{ 0 };
    std::atomic<uint64_t> nsAimbot{ 0 };
    std::atomic<uint64_t> nsPublish{ 0 };      // copia das 4 listas p/ GSharedData
    std::atomic<uint64_t> nsAimTickCopy{ 0 };  // copia que o Aimbot::Tick faz
};
inline Contadores C;

// ---------------------------------------------------------------------------
// P95 / P99 DO FRAME
//
// Media esconde o que o usuario SENTE. Microtravamento e' cauda: um frame de
// 40ms a cada 200 nao move a media e trava na tela. Por isso guardamos um
// histograma simples (buckets de 0,25 ms ate' 64 ms) e calculamos percentil.
//
// Custo: um incremento de indice. Sem alocacao, sem ordenacao.
// ---------------------------------------------------------------------------
inline constexpr int kBuckets = 256;          // 256 x 0.25ms = 64ms
inline std::atomic<uint32_t> histBuild[kBuckets];
inline std::atomic<uint32_t> histPresent[kBuckets];

inline void ContarFrame(uint64_t ns) {
    if (!Ativo()) return;
    // Soma o total E o bucket. O `Perf::Escopo` nao pode ser usado para
    // isto: o destrutor dele roda DEPOIS do bloco, entao ler a duracao
    // dentro do proprio bloco pegaria zero.
    C.buildNs.fetch_add(ns, std::memory_order_relaxed);
    C.renderFrames.fetch_add(1, std::memory_order_relaxed);
    int b = (int)(ns / 250000ull);            // 0,25 ms por bucket
    if (b >= kBuckets) b = kBuckets - 1;
    histBuild[b].fetch_add(1, std::memory_order_relaxed);
}

inline void RegistrarPresent(uint64_t ns) {
    if (!Ativo()) return;
    C.presentNs.fetch_add(ns, std::memory_order_relaxed);
    int b = (int)(ns / 250000ull);
    if (b >= kBuckets) b = kBuckets - 1;
    histPresent[b].fetch_add(1, std::memory_order_relaxed);
}

// Percentil em ms, a partir do histograma. `p` em 0..1.
inline double Percentil(const std::atomic<uint32_t>* h, double p) {
    uint64_t total = 0;
    for (int i = 0; i < kBuckets; ++i) total += h[i].load(std::memory_order_relaxed);
    if (total == 0) return 0.0;
    const uint64_t alvo = (uint64_t)(p * (double)total);
    uint64_t acc = 0;
    for (int i = 0; i < kBuckets; ++i) {
        acc += h[i].load(std::memory_order_relaxed);
        if (acc >= alvo) return (double)i * 0.25;
    }
    return (kBuckets - 1) * 0.25;
}

inline void ZerarHistogramas() {
    for (int i = 0; i < kBuckets; ++i) {
        histBuild[i].store(0, std::memory_order_relaxed);
        histPresent[i].store(0, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// CRONOMETRO DE ESCOPO
//
// Uso:
//     { Perf::Escopo e(Perf::C.buildNs); ... trabalho ... }   // soma
//     PERF_TEMPO(C.ioctlNs) { ... }                           // idem, via macro
//
// O destrutor so' escreve se os contadores estiverem ligados — desligado, o
// objeto existe e faz um teste de ponteiro no fim.
// ---------------------------------------------------------------------------
class Escopo {
public:
    explicit Escopo(std::atomic<uint64_t>& destino)
        : d_(Ativo() ? &destino : nullptr),
          t_(d_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}

    ~Escopo() {
        if (!d_) return;
        const auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - t_).count();
        d_->fetch_add((uint64_t)dt, std::memory_order_relaxed);
    }

    Escopo(const Escopo&) = delete;
    Escopo& operator=(const Escopo&) = delete;

private:
    std::atomic<uint64_t>* d_;
    std::chrono::steady_clock::time_point t_;
};

// Incremento simples, com o mesmo teste barato.
inline void Contar(std::atomic<uint64_t>& c, uint64_t n = 1) {
    if (Ativo()) c.fetch_add(n, std::memory_order_relaxed);
}

} // namespace Perf

#define PERF_TEMPO(contador) for (Perf::Escopo _perf_##__LINE__(contador); false; )
