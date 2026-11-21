#include "pch.h"
#include "Aimbot.h"
#include "../Config/ConfigUtilities.h"
#include "Camera.h"
#include "../SharedState.h"
#include "../OS-ImGui/OS-ImGui.h"
#include "../Globals.h"
#include "../Misc/HotkeyHook.h"   // leitura de tecla por hook de baixo nivel
#include "RcsNoise.h"              // uniforme/normal + OUStep (testado em tools\rcs_hand_check.cpp)
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>

// Helper for C++14 compatibility
template <typename T> static T clamp_value(T v, T lo, T hi) {
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

uint64_t Aimbot::currentTargetAddress = 0;
int Aimbot::lastMoveX = 0;
int Aimbot::lastMoveY = 0;
float Aimbot::remainderX = 0.0f;
float Aimbot::remainderY = 0.0f;
std::chrono::steady_clock::time_point Aimbot::lastTargetSwitchTime;
std::chrono::steady_clock::time_point Aimbot::lastTargetSeenTime;
bool Aimbot::targetInGrace = false;
int Aimbot::currentBoneId = 5;
std::mt19937 Aimbot::rng(std::random_device{}());

// RCS — so' o que o feed-forward do acumulador drenado usa de fato.
// (Ver a nota em Aimbot.h sobre os onze estaticos mortos que sairam daqui.)
bool  Aimbot::s_rcsValid = false;
float Aimbot::s_ffRvvY = 0.0f;
float Aimbot::s_ffVr   = 0.0f;
float Aimbot::s_ffRvvX = 0.0f;

// ===========================================================================
// CENSO DO FUNIL DO AIMBOT — UM CONTADOR POR `return` DO `Aimbot::Tick`.
//
// POR QUE ISTO EXISTE (medido em campo): com o hotkey PRESSIONADO e
// `Configs.Aimbot.Enabled` ligado, o `Tick` passava do gate do hotkey e
// voltava em SILENCIO. No log: 11 amostras de `RBUTTON=down`, e ZERO linhas
// `DESISTIU`, ZERO `FUNIL(1s)`, ZERO `alvo=`. Isso prova que existe(m)
// `return`(s) ANTES do bloco de diagnostico (linha ~613) que nao deixam
// rastro nenhum — porque aquele bloco so' e' alcancado DEPOIS de um alvo ter
// sido escolhido.
//
// COMO FUNCIONA: cada `return` do `Tick` incrementa UM contador uint64_t.
// Uma vez por segundo (relogio steady — nao e' por frame) o censo inteiro sai
// numa linha so'. O contador que crescer diz ONDE o aimbot morre. A diferenca
// entre `ticks` e a soma de todos os motivos e' o numero de ticks que
// CHEGARAM AO FIM do `Tick`.
//
// ONDE O CENSO E' IMPRESSO: no INICIO do `Tick`, antes de qualquer `return`.
// Se ficasse no fim (como o `FUNIL(1s)` antigo, dentro do bloco de debug), o
// caminho que morre cedo nunca imprimiria nada — que e' exatamente o caso
// investigado.
//
// CUSTO COM `DebugLogs` DESLIGADO: um incremento de uint64_t por tick (e por
// candidato descartado), mais a leitura do relogio no gate do timer. Nenhuma
// string, nenhum `LOG`, nenhum IO no caminho normal: o `LOG` so' sai 1x/s e
// so' com `Configs.Survivor.DebugLogs` ligado.
//
// ISTO E' MEDICAO: nenhum `return` foi consertado, movido, removido ou
// alterado. Nenhum campo de config novo.
// ===========================================================================
namespace {

struct AimFunnelCensus {
    // -----------------------------------------------------------------
    // Um contador por `return` do `Aimbot::Tick`, na ORDEM em que os
    // `return`s aparecem dentro da funcao.
    // ATENCAO: os `L###` dos comentarios abaixo sao as linhas da versao
    // ANTES desta instrumentacao (o arquivo cresceu ~165 linhas com o
    // censo), nao as linhas atuais.
    // -----------------------------------------------------------------
    uint64_t ticks = 0;         // total de chamadas do Tick (o denominador)
    uint64_t disabled = 0;      // L358: !Configs.Aimbot.Enabled
    uint64_t hotkey = 0;        // L363: !IsHotkeyDown(Configs.Aimbot.Hotkey)
    uint64_t timeout = 0;       // L381: LastUpdateTime <= lastProcessedTime (SUSPEITO Nº 1)
    uint64_t noActors = 0;      // L388: data.Actors.empty() || !EngineInstance
    uint64_t badCamera = 0;     // POV.FOV do aimbot invalido (Engine::CameraEntry morta)
    uint64_t graceStart = 0;    // L463: sem alvo, acabou de entrar na janela de graca
    uint64_t graceWait = 0;     // L468: sem alvo, ainda dentro dos 500ms de graca
    uint64_t noTarget = 0;      // L474: varreu tudo e nao achou alvo
    uint64_t projFinalZero = 0; // L714: WorldToScreen do alvo FINAL devolveu (0,0)

    // -----------------------------------------------------------------
    // Motivos de DESCARTE de candidato. Sao `continue` (nao `return`), mas
    // sao a resposta para "por que nao achou alvo": o `noTarget` sozinho nao
    // distingue "nao tinha ninguem" de "todo mundo foi reprovado".
    // -----------------------------------------------------------------
    uint64_t actorFail = 0;     // L403/L418: !isCheck || isDie || Health <= 0
    uint64_t selfTeamFail = 0;  // L419: e' o proprio pawn ou o mesmo time
    uint64_t visFail = 0;       // L420: VisibilityCheck && !IsVisible
    uint64_t projFail = 0;      // L436: WorldToScreen do previsto devolveu (0,0)
    uint64_t fovFail = 0;       // L446: fora do raio de FOV em pixels

    // -----------------------------------------------------------------
    // EXTRAS (alem do pedido, mesma linha, mesmo custo): os 3 `return` de
    // `Aimbot::MoveMouseTowards`, que e' o UNICO consumidor do caminho de
    // sucesso do Tick. Sem eles, "o Tick chegou ao fim" e "o mouse se moveu"
    // continuam sendo a mesma coisa aos olhos do log — e o proximo passo as
    // cegas seria a ultima milha.
    //   mmOnTarget NAO e' falha: e' a mira ja' em cima do alvo (deadzone).
    // -----------------------------------------------------------------
    uint64_t mmThrottle = 0;    // L221: limitador de 2ms entre injecoes
    uint64_t mmOnTarget = 0;    // L232: distPixels <= 1.5 (mira ja' no alvo)
    uint64_t mmZeroMove = 0;    // L334: (moveX, moveY) arredondou para (0,0)

    // -----------------------------------------------------------------
    // Valores do gate de throttle, impressos LADO A LADO (`ut` x `lastProc`)
    // para o veredito sair lendo DUAS LINHAS CONSECUTIVAS do log: se `ut` nao
    // avancar entre dois censos, o Tick esta' morrendo no gate de timeout.
    // Sao capturados com o GDataMutex na mao, e ANTES do gate — inclusive no
    // tick que o gate REJEITA (que e' justamente o que precisa ser visto).
    // -----------------------------------------------------------------
    uint64_t ut = 0;            // GSharedData.LastUpdateTime (ms)
    uint64_t lastProc = 0;      // o `lastProcessedTime` do Tick
    uint64_t actors = 0;        // GSharedData.Actors.size() no gate

    // -----------------------------------------------------------------
    // A CAMERA QUE O AIMBOT USA. O ESP projeta com `Camera::GetCurrentView`
    // (Main.cpp:452, thread de render); o aimbot projeta com
    // `GSharedData.CameraCache`, que vem de `Engine::GetCameraCache()` ->
    // `CameraEntry`, alimentado por `Engine::RefreshViewMatrix()`
    // (Engine.cpp:2751) NA THREAD DE MEMORIA, e so' quando
    // `!FrameTranslateError` (Main.cpp:123). Sao DOIS caminhos diferentes para
    // a mesma struct de offsets. Se `povFov` vier 0/lixo, `WorldToScreen`
    // devolve (0,0) para TODO candidato (guarda da Camera.cpp:80) e o funil
    // morre em `projFail` -> `noTarget`, sem log nenhum. E' o par de dados que
    // confirma ou mata essa hipotese na MESMA rodada.
    // -----------------------------------------------------------------
    double povFov = 0.0;
    double povLocX = 0.0;
    double povLocY = 0.0;
    double povLocZ = 0.0;
    // -----------------------------------------------------------------
    // ROTACAO DA POV E VIEWPORT — os dois dados que FALTAVAM no censo.
    //
    // A `WorldToScreen` rejeita o alvo como "atras da camera" quando
    // `Dot(delta, forward) < 1.0f`. Se a ROTACAO estiver errada, TODO alvo cai
    // nesse caso e o funil morre em `projFail` — que foi exatamente o que o log
    // mediu (17 candidatos por tick, sem excecao). Sem imprimir a rotacao aqui
    // nao havia como distinguir "rotacao ruim" de "alvo atras mesmo".
    //
    // O `vp` (Gui.Window.Size) e' o mesmo global que a `WorldToScreen`, o
    // `viewCenter` e o `fovPixelRadius` usam. Se ele vier 0 na THREAD DE
    // MEMORIA, o raio do FOV vira o clamp de 10px e o centro vira (0,0):
    // TODO candidato reprova por `fovFail`. Este e' o unico dado que separa
    // essa hipotese das outras.
    // -----------------------------------------------------------------
    double povRotP = 0.0, povRotY = 0.0, povRotR = 0.0;
    double vpX = 0.0, vpY = 0.0;
    double fovPx = 0.0;         // raio de FOV calculado neste tick
    double fovCfg = 0.0;        // Configs.Aimbot.FOV deste tick

    std::chrono::steady_clock::time_point lastLog;
    bool logStarted = false;
};

AimFunnelCensus g_aimFunnel;

// Era `static uint64_t lastProcessedTime` DENTRO do `Tick`. Promovido para
// escopo de arquivo SEM mudanca de semantica — continua havendo UMA unica
// instancia, compartilhada por todas as chamadas —, apenas para o censo
// conseguir imprimir `lastProc` ao lado de `ut`. O `<=` e' o mesmo, com os
// mesmos tipos.
uint64_t g_aimLastProcessedTime = 0;

// Zera SO' os contadores. Preserva o timer do censo e os ultimos valores do
// gate (`ut`/`lastProc`/`actors`), que sao estado "do agora", nao contagem.
// Usa atribuicao da struct inteira: qualquer contador novo entra no reset
// automaticamente, sem risco de esquecer um.
void AimFunnelResetCounters() {
    const uint64_t keepUt       = g_aimFunnel.ut;
    const uint64_t keepLastProc = g_aimFunnel.lastProc;
    const uint64_t keepActors   = g_aimFunnel.actors;
    const double   keepPovFov   = g_aimFunnel.povFov;
    const double   keepPovLocX  = g_aimFunnel.povLocX;
    const double   keepPovLocY  = g_aimFunnel.povLocY;
    const double   keepPovLocZ  = g_aimFunnel.povLocZ;
    const double   keepPovRotP  = g_aimFunnel.povRotP;
    const double   keepPovRotY  = g_aimFunnel.povRotY;
    const double   keepPovRotR  = g_aimFunnel.povRotR;
    const double   keepVpX      = g_aimFunnel.vpX;
    const double   keepVpY      = g_aimFunnel.vpY;
    const double   keepFovPx    = g_aimFunnel.fovPx;
    const double   keepFovCfg   = g_aimFunnel.fovCfg;
    const auto     keepLastLog  = g_aimFunnel.lastLog;
    const bool     keepStarted  = g_aimFunnel.logStarted;
    g_aimFunnel = AimFunnelCensus{};
    g_aimFunnel.ut          = keepUt;
    g_aimFunnel.lastProc    = keepLastProc;
    g_aimFunnel.actors      = keepActors;
    g_aimFunnel.povFov      = keepPovFov;
    g_aimFunnel.povLocX     = keepPovLocX;
    g_aimFunnel.povLocY     = keepPovLocY;
    g_aimFunnel.povLocZ     = keepPovLocZ;
    g_aimFunnel.povRotP     = keepPovRotP;
    g_aimFunnel.povRotY     = keepPovRotY;
    g_aimFunnel.povRotR     = keepPovRotR;
    g_aimFunnel.vpX         = keepVpX;
    g_aimFunnel.vpY         = keepVpY;
    g_aimFunnel.fovPx       = keepFovPx;
    g_aimFunnel.fovCfg      = keepFovCfg;
    g_aimFunnel.lastLog     = keepLastLog;
    g_aimFunnel.logStarted  = keepStarted;
}

// Imprime o censo 1x/s (relogio steady, nao por frame) e zera os contadores.
// Chamada no INICIO do `Aimbot::Tick`, antes de qualquer `return`.
void AimFunnelCensusLog() {
    auto now = std::chrono::steady_clock::now();
    if (!g_aimFunnel.logStarted) {
        // primeira chamada: so' serve para marcar a referencia do timer
        g_aimFunnel.logStarted = true;
        g_aimFunnel.lastLog = now;
        return;
    }
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_aimFunnel.lastLog).count() < 1000) {
        return;
    }
    g_aimFunnel.lastLog = now;

    // Zero custo com DebugLogs desligado: nem string, nem IO.
    if (!Configs.Survivor.DebugLogs) {
        AimFunnelResetCounters();
        return;
    }

    // `lastProc` e' lido do global do Tick na hora de imprimir, para o par
    // (`ut`, `lastProc`) sair sempre coerente entre si.
    g_aimFunnel.lastProc = g_aimLastProcessedTime;

    LOG("[AIM] FUNIL: ticks=%llu | disabled=%llu hotkey=%llu timeout=%llu noActors=%llu "
        "badCamera=%llu noTarget=%llu graceStart=%llu graceWait=%llu projFail=%llu fovFail=%llu "
        "actorFail=%llu selfTeamFail=%llu visFail=%llu projFinalZero=%llu | "
        "mmThrottle=%llu mmOnTarget=%llu mmZeroMove=%llu | "
        "hotkeyAtual=%d ativo=%d | ut=%llu lastProc=%llu actors=%llu | "
        "povFov=%.2f povLoc=%.1f,%.1f,%.1f povRot=%.1f,%.1f,%.1f | "
        "vp=%.0fx%.0f FOVcfg=%.1f fovPx=%.1f\n",
        (unsigned long long)g_aimFunnel.ticks,
        (unsigned long long)g_aimFunnel.disabled,
        (unsigned long long)g_aimFunnel.hotkey,
        (unsigned long long)g_aimFunnel.timeout,
        (unsigned long long)g_aimFunnel.noActors,
        (unsigned long long)g_aimFunnel.badCamera,
        (unsigned long long)g_aimFunnel.noTarget,
        (unsigned long long)g_aimFunnel.graceStart,
        (unsigned long long)g_aimFunnel.graceWait,
        (unsigned long long)g_aimFunnel.projFail,
        (unsigned long long)g_aimFunnel.fovFail,
        (unsigned long long)g_aimFunnel.actorFail,
        (unsigned long long)g_aimFunnel.selfTeamFail,
        (unsigned long long)g_aimFunnel.visFail,
        (unsigned long long)g_aimFunnel.projFinalZero,
        (unsigned long long)g_aimFunnel.mmThrottle,
        (unsigned long long)g_aimFunnel.mmOnTarget,
        (unsigned long long)g_aimFunnel.mmZeroMove,
        (int)HotkeyHook::IsDown(Configs.Aimbot.Hotkey),
        (int)Configs.Aimbot.Enabled,
        (unsigned long long)g_aimFunnel.ut,
        (unsigned long long)g_aimFunnel.lastProc,
        (unsigned long long)g_aimFunnel.actors,
        g_aimFunnel.povFov,
        g_aimFunnel.povLocX,
        g_aimFunnel.povLocY,
        g_aimFunnel.povLocZ,
        g_aimFunnel.povRotP,
        g_aimFunnel.povRotY,
        g_aimFunnel.povRotR,
        g_aimFunnel.vpX,
        g_aimFunnel.vpY,
        g_aimFunnel.fovCfg,
        g_aimFunnel.fovPx);

    AimFunnelResetCounters();
}

// ===========================================================================
// AimPointFor — POSICAO DE MIRA DO ATOR, SEMPRE VALIDA.
//
// CAUSA RAIZ DO "AIMBOT NAO GRUDA" (medida, nao deduzida):
//
// O `Aimbot::Tick` monta a posicao alvo com os OSSOS (`Head3D` / `neck3D` /
// `pelvis3D`), e o `GetBestBone` devolve 1 (Head) sempre que `AutoBone` esta'
// off e `TargetBone == 0` (os dois defaults). So' que a leitura de ossos do
// `Engine::UpdatePlayers` estava gated em `Configs.Survivor.Skeleton` — o
// toggle do ESQUELETO DO ESP.
//
// Com o esqueleto desligado, `bHasBones` nunca virava true, os tres UEVector
// ficavam em (0,0,0), e o aimbot mirava a ORIGEM DO MUNDO. O log provou:
//
//     raw  (posicao do ator) = 16509,4053,1944 -> tela=(970,534)  d2c=12
//     osso (o que ele usava) = 0,0,0           -> tela=(-251,71833)
//
// O ator estava EM CIMA DA MIRA (12 px do centro) e o aimbot mirava outra
// coisa. A projecao da origem cai fora do raio de FOV -> `fovFail` em TODO
// candidato -> `noTarget` -> nada acontece, sem log.
//
// Aqui: `bHasBones` e' o sinal honesto de "os ossos deste ator foram lidos
// NESTE tick" (setado no fim do segundo passe do UpdatePlayers). Sem ele, cai
// para `GetPosition()` — a mesma posicao que o ESP usa para desenhar a caixa,
// que existe sempre.
// ===========================================================================
Vector3 AimPointFor(const std::shared_ptr<ActorEntity>& a, int boneIndex) {
    if (!a) return Vector3(0.0f, 0.0f, 0.0f);

    auto ok = [](const UEVector& b) {
        return std::isfinite(b.X) && std::isfinite(b.Y) && std::isfinite(b.Z) &&
               !(b.X == 0.0f && b.Y == 0.0f && b.Z == 0.0f);
    };

    if (a->bHasBones) {
        if (boneIndex == 1 && ok(a->Head3D))   return Vector3(a->Head3D.X,   a->Head3D.Y,   a->Head3D.Z);
        if (boneIndex == 2 && ok(a->neck3D))   return Vector3(a->neck3D.X,   a->neck3D.Y,   a->neck3D.Z);
        if (boneIndex == 5 && ok(a->pelvis3D)) return Vector3(a->pelvis3D.X, a->pelvis3D.Y, a->pelvis3D.Z + 30.0f);
        if (boneIndex == 0 && ok(a->pelvis3D)) return Vector3(a->pelvis3D.X, a->pelvis3D.Y, a->pelvis3D.Z);
    }
    return a->GetPosition();
}

// ===========================================================================
// AimNoTargetDump — POR QUE TODOS OS CANDIDATOS FORAM REPROVADOS.
//
// O BURACO QUE ISTO TAPA (medido): o bloco `[AIM] DESISTIU` (mais abaixo, no
// fim do Tick) tem todos os dados certos — `tela_osso_cru`, `prev_pre_recuo`,
// POV rot/fov, `vp`, `fovPx` — mas ele so' e' alcancado DEPOIS de um alvo ter
// sido escolhido. Quando o funil morre em `noTarget`, ele retorna ANTES, e o
// log fica com 2125 reprovacoes por segundo e ZERO explicacao de qual delas.
//
// Aqui a pergunta e' respondida NO `return` do `noTarget`: dumpa a camera
// inteira (inclusive a ROTACAO, que o censo nao imprimia) e reconstroi a
// projecao de ate' 3 candidatos dos TRES jeitos que importam:
//
//   tela (cru)  = WorldToScreen da posicao crua do ator   -> e' o que o ESP usa
//   tela (prev) = WorldToScreen com `pos + Velocity*flyTime` -> e' o que a
//                 SELECAO do aimbot usa (Aimbot.cpp, laco de candidatos)
//   tela (osso) = WorldToScreen do Head3D                 -> osso do esqueleto
//
// COMO LER (a linha que decide):
//   cru OK  + prev (0,0)      -> VELOCIDADE lixo contaminando a predicao
//   cru (0,0) + osso (0,0)    -> a CAMERA/POV esta' ruim (olhe `rot` e `vp`)
//   cru OK  + d2c >> fovPx    -> raio de FOV pequeno demais, ou `vp` zerado
//   vp=0x0                    -> `Gui.Window.Size` zerado NA THREAD DE MEMORIA:
//                                `viewCenter`=(0,0) e `fovPx` no clamp de 10px
//                                => TODO candidato reprova por `fovFail`
//
// 1x/s e atras de `Configs.Survivor.DebugLogs` — e' diagnostico, nao caminho
// quente. Nao muda nenhuma decisao: so' imprime.
// ===========================================================================
void AimNoTargetDump(const SharedData& data, Vector2 viewCenter, float fovPixelRadius) {
    if (!Configs.Survivor.DebugLogs) return;

    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() < 1000) return;
    last = now;

    const MinimalViewInfo& pov = data.CameraCache.POV;
    const Vector3 camLoc(pov.Location.X, pov.Location.Y, pov.Location.Z);

    // Quantos atores tem osso lido NESTE tick. `bHasBones` e' o unico sinal
    // honesto: se estiver 0 em todo mundo, o aimbot nao tem osso nenhum para
    // mirar e a leitura de ossos do UpdatePlayers nao esta' rodando.
    int comOsso = 0;
    for (auto& a : data.Actors) { if (a && a->bHasBones) comOsso++; }

    LOG("[AIM] SEM ALVO: vp=%.0fx%.0f viewCenter=%.0f,%.0f | FOVcfg=%.1f fovPx=%.1f | "
        "POV loc=%.1f,%.1f,%.1f rot=%.2f,%.2f,%.2f fov=%.2f | actors=%zu bones=%d\n",
        (double)Gui.Window.Size.x, (double)Gui.Window.Size.y,
        (double)viewCenter.x, (double)viewCenter.y,
        (double)Configs.Aimbot.FOV, (double)fovPixelRadius,
        (double)camLoc.x, (double)camLoc.y, (double)camLoc.z,
        (double)pov.Rotation.Pitch, (double)pov.Rotation.Yaw, (double)pov.Rotation.Roll,
        (double)pov.FOV,
        data.Actors.size(), comOsso);

    int shown = 0;
    for (auto& a : data.Actors) {
        if (shown >= 3) break;
        if (!a || !a->isCheck || a->isDie || a->Health <= 0) continue;
        if (a->Class == data.AcknowledgedPawn) continue;

        const Vector3 raw = a->GetPosition();
        const Vector2 sRaw = Camera::WorldToScreen(pov, raw);
        const float ddx = sRaw.x - viewCenter.x;
        const float ddy = sRaw.y - viewCenter.y;
        const float d2c = std::sqrt(ddx * ddx + ddy * ddy);

        // MESMA predicao que o laco de selecao do Tick usa.
        const float bspeed = EngineInstance ? EngineInstance->GetCurrentBulletSpeed() : 0.0f;
        const float dist   = Vector3::Distance(camLoc, raw);
        const float fly    = (bspeed > 1.0f) ? (dist / (bspeed * 100.0f)) : 0.0f;
        const Vector3 pred = raw + (a->Velocity * fly);
        const Vector2 sPred = Camera::WorldToScreen(pov, pred);

        const Vector3 head(a->Head3D.X, a->Head3D.Y, a->Head3D.Z);
        const Vector2 sHead = Camera::WorldToScreen(pov, head);

        // Sinal limpo de "a velocidade esta' absurda" (o sanitizador do
        // Engine::UpdatePlayers ja' zera o que passa de 2000 cm/s; isto aqui
        // mostra o valor NAO sanitizado, que e' o que interessa diagnosticar).
        LOG("[AIM]   cand=0x%llX d=%.1fm raw=%.0f,%.0f,%.0f -> tela=(%.0f,%.0f) d2c=%.0f | "
            "vel=%.2f,%.2f,%.2f fly=%.4fs pred=%.0f,%.0f,%.0f -> tela=(%.0f,%.0f) | "
            "osso=%.0f,%.0f,%.0f -> tela=(%.0f,%.0f)\n",
            (unsigned long long)a->Class, (double)(dist / 100.0f),
            (double)raw.x, (double)raw.y, (double)raw.z,
            (double)sRaw.x, (double)sRaw.y, (double)d2c,
            (double)a->Velocity.x, (double)a->Velocity.y, (double)a->Velocity.z,
            (double)fly,
            (double)pred.x, (double)pred.y, (double)pred.z,
            (double)sPred.x, (double)sPred.y,
            (double)head.x, (double)head.y, (double)head.z,
            (double)sHead.x, (double)sHead.y);
        shown++;
    }
    if (!shown) LOG("[AIM]   (nenhum candidato passou isCheck/isDie/Health)\n");
}

} // namespace

// ---------------------------------------------------------------------------
// IsHotkeyDown — LEITURA DE TECLA DO AIMBOT.
//
// ASSINATURA E MAPEAMENTO DE INDICES PRESERVADOS (0:Left Alt, 1:Right Click,
// 2:Shift, 3:Mouse5, 4:Mouse4, 5:Left Click): o indice vem de
// `Configs.Aimbot.Hotkey`, entao mudar qualquer um dos dois quebraria as
// configs ja' salvas.
//
// O QUE MUDOU: antes isto era `GetAsyncKeyState(vkey) & 0x8000`, polling que
// NESTE ambiente devolve 0 para qualquer tecla (cheat elevado + BattlEye/
// Xenuine ativo, mesmo com o PUBG em segundo plano — medido em campo). Agora
// quem manda e' o estado alimentado pelo hook de baixo nivel, que o Windows
// ENTREGA por mensagem — o mesmo caminho do RegisterHotKey do HOME/END, que
// funciona. O `GetAsyncKeyState` continua dentro do HotkeyHook::IsDown como
// rede de seguranca (nada foi removido).
//
// Quem chama: somente `Aimbot::Tick` (o gate do hotkey) e as duas leituras de
// botao esquerdo em `MoveMouseTowards` / `Tick`, que agora usam
// `HotkeyHook::IsDown(HotkeyHook::KEY_LBUTTON)` diretamente.
// ---------------------------------------------------------------------------
bool Aimbot::IsHotkeyDown(int index) {
    return HotkeyHook::IsDown(index);
}

// Auto bone selection - picks closest bone to crosshair or fallback to config
int Aimbot::GetBestBone(std::shared_ptr<ActorEntity> ent, Vector2 viewCenter) {
    if (!Configs.Aimbot.AutoBone) {
        // Use config TargetBone as fallback
        switch (Configs.Aimbot.TargetBone) {
            case 0: return 1; // Head
            case 1: return 2; // Neck
            case 2: return 5; // Spine / Chest
            case 3: return 0; // Pelvis
        }
        return 5;
    }

    int bestBone = 5; // Default Spine
    float bestDist = 999999.0f;
    int bonesToCheck[] = {1, 2, 5}; // Head, Neck, Chest
    
    for (int bone : bonesToCheck) {
        // AimPointFor ja' cai para a posicao do ator quando o osso nao foi
        // lido (bHasBones false) ou veio zerado. Ver a nota na funcao.
        Vector3 targetPos = AimPointFor(ent, bone);

        Vector3 camLoc(GSharedData.CameraCache.POV.Location.X, GSharedData.CameraCache.POV.Location.Y, GSharedData.CameraCache.POV.Location.Z);
        float dist = Vector3::Distance(camLoc, targetPos);
        float bulletSpeed = EngineInstance->GetCurrentBulletSpeed(); 
        
        float flyTime = (bulletSpeed > 1.0f) ? (dist / (bulletSpeed * 100.0f)) : 0.0f;
        
        Vector3 predictedTarget = targetPos + (ent->Velocity * flyTime);
        Vector2 screenPos = Camera::WorldToScreen(GSharedData.CameraCache.POV, predictedTarget);
        if (screenPos.x == 0.0f && screenPos.y == 0.0f) continue;

        float dx = screenPos.x - viewCenter.x;
        float dy = screenPos.y - viewCenter.y;
        float distScreen = std::sqrt(dx * dx + dy * dy);

        float bias = (bone == currentBoneId) ? 0.8f : 1.0f;
        if (distScreen * bias < bestDist) {
            bestDist = distScreen;
            bestBone = bone;
        }
    }
    return bestBone;
}

UERotator Aimbot::CalcAngle(Vector3 localPos, Vector3 predictedPos) {
    Vector3 delta = predictedPos - localPos;
    float hyp = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    
    UERotator angles;
    angles.Pitch = -std::atan2(delta.z, hyp) * (180.0f / (float)M_PI);
    angles.Yaw = std::atan2(delta.y, delta.x) * (180.0f / (float)M_PI);
    angles.Roll = 0.0f;
    return angles;
}

UERotator Aimbot::NormalizeAngle(UERotator angles) {
    if (angles.Pitch > 89.0f) angles.Pitch = 89.0f;
    if (angles.Pitch < -89.0f) angles.Pitch = -89.0f;

    while (angles.Yaw > 180.0f) angles.Yaw -= 360.0f;
    while (angles.Yaw < -180.0f) angles.Yaw += 360.0f;

    return angles;
}

float Aimbot::CalculateTargetScore(std::shared_ptr<ActorEntity> ent, Vector3 targetPos, float fov) {
    const float BASE = 100.0f;
    float score = 0.0f;

    float fovWeight = 3.0f;
    float distanceWeight = 1.0f;

    // FOV score (closer to center = higher)
    float fovRatio = fov / Configs.Aimbot.FOV;
    score += (1.0f - std::pow(fovRatio, 1.8f)) * BASE * fovWeight;

    // Distance score (closer = higher)
    Vector3 camLoc(GSharedData.CameraCache.POV.Location.X, GSharedData.CameraCache.POV.Location.Y, GSharedData.CameraCache.POV.Location.Z);
    float dx = targetPos.x - camLoc.x;
    float dy = targetPos.y - camLoc.y;
    float dz = targetPos.z - camLoc.z;
    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    float maxDist = 300.0f * 100.0f; // Approx 300m in unreal units (1 uu = 1 cm)
    float distRatio = (std::min)(dist / maxDist, 1.0f);
    float distScore = (1.0f - std::pow(distRatio, 1.2f)) * BASE * distanceWeight;
    score += distScore;

    // Visibility bonus
    //
    // Desde 2610.1.1.65 este `IsVisible` e' MEDIDO (LastTimeScreenRenderered),
    // nao mais um campo sempre-true: quem esta' atras de parede deixa de
    // receber os 35 pontos e, com `VisibilityCheck` ligado, nem entra na
    // selecao (ver o filtro no Tick). Os dois usos dependem do MESMO veredito,
    // calculado no Engine — nao existe segunda regra aqui.
    float visBonus = 0.0f;
    if (ent->IsVisible) {
        visBonus = 35.0f;
        score += visBonus; 
    }

    // Persistence bonus (stick to current target) — raised for aggressive lock
    float persistBonus = 0.0f;
    if (ent->Class == currentTargetAddress) {
        persistBonus = 80.0f; 
        score += persistBonus; 
    }

    // Verbose score //LOG for current target
    if (ent->Class == currentTargetAddress) {
        /*
        LOG("[Score] FOV_S: %.1f | Dist_S: %.1f | Vis: %.1f | Pers: %.1f | Total: %.1f\n", 
            (1.0f - std::pow(fovRatio, 1.8f)) * BASE * fovWeight, distScore, visBonus, persistBonus, score);
        */
    }

    return score;
}

// ===========================================================================
// ONDE ESTE CHEAT "ESCREVE A ROTACAO" — leia antes de mexer no eixo vertical.
//
// Existem DUAS rotacoes documentadas no Globals.h:
//     ControlRotation_CP    0x674   (FRotator: Pitch, Yaw, Roll)
//     RecoilADSRotation_CP  0x824   (fator de blend do recuo, NAO e' angulo)
//
// NENHUMA das duas e' ESCRITA por este cheat. Nao existe escrita de rotacao
// em lugar nenhum do projeto (nenhum TargetProcess.Write / Memory::Write):
//   - ControlRotation_CP   -> lido SO' pelo diagnostico `Engine::DiagChain`;
//   - RecoilADSRotation_CP -> lido pelo `Engine::UpdatePlayers` (Local.Recoil).
//
// Quem de fato move a vista sao os DOIS pontos que chamam
// `TargetProcess.driver.InjectMouseMove` (-> SendInput com MOUSEEVENTF_MOVE,
// ou seja movimento RELATIVO de mouse):
//
//   1. Aimbot::MoveMouseTowards  (chamado por Aimbot::Tick)   -> (moveX, moveY)
//   2. Aimbot::RecoilControl     (RCS, roda TODO frame)       -> (ix, iy)
//
// Nos dois, o componente Y e' o eixo VERTICAL (pitch) e o X e' o HORIZONTAL
// (yaw). Por isso a opcao "destravar o eixo vertical"
// (Configs.Aimbot.AimbotUnlockVertical) e' aplicada nos DOIS lugares: se ela
// valesse so' no aimbot, o RCS devolveria o pitch pela puxada do recuo e o
// usuario concluiria — com razao — que a opcao nao funciona.
//
// CONSEQUENCIA BOA DESSE DESENHO: "preservar o pitch atual do jogo" nao exige
// ler o valor vivo nem reescrever rotacao nenhuma; basta parar de mandar o
// delta vertical. Nao existe risco de escrever Pitch com valor velho/lixo.
// ===========================================================================
void Aimbot::MoveMouseTowards(Vector2 targetScreenPos, Vector2 viewCenter) {
    static std::chrono::steady_clock::time_point lastMoveTime;
    static int cadencia = 0;
    auto now = std::chrono::steady_clock::now();

    if (lastMoveTime.time_since_epoch().count() != 0) {
        auto deltaMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastMoveTime).count();
        // ------------------------------------------------------------------
        // CADÊNCIA: ritmo PERIODICO e uniforme (sempre ~2 ms) e' assinatura
        // de injecao — o olho humano move em MICRO-RACHADAS com pausas
        // irregulares. A cada 16 pacotes, um intervalo de 4 ms em vez de 2:
        // a media por segundo quase nao muda, mas a serie temporal deixa de
        // ser um cronometro. (O throttle classico de 2 ms continua valendo
        // em qualquer caso: ele e' o piso, nunca o teto do ritmo.)
        // ------------------------------------------------------------------
        const long necessarioMs = ((cadencia++) % 16 == 0) ? 4 : 2;
        if (deltaMs < necessarioMs) {
            g_aimFunnel.mmThrottle++;    // CENSO (ultima milha)
            return;
        }
    }

    float dx = targetScreenPos.x - viewCenter.x;
    float dy = targetScreenPos.y - viewCenter.y;

    float distPixels = std::sqrt(dx * dx + dy * dy);

    // Parada dura a menos de 1 px: "quase la'" virava um micro-ajuste eterno
    // (o 1.5 antigo deixava o mouse passeando ao redor do ponto — e passeio
    // ao redor da mira e' o que um analista de telemetria procura).
    if (distPixels < 1.0f) { 
        remainderX = 0; remainderY = 0;
        g_aimFunnel.mmOnTarget++;        // CENSO (mira ja' no alvo: NAO e' falha)
        return;
    }

    float clampedSmooth = clamp_value(Configs.Aimbot.AR_SmoothValue / 10.0f, 0.01f, 1.0f);
    
    float timeSinceSwitch = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTargetSwitchTime).count() / 1000.0f;
    if (timeSinceSwitch < 0.3f) {
        float progress = timeSinceSwitch / 0.3f;
        clampedSmooth += clampedSmooth * (2.0f - 1.0f) * (1.0f - progress);
    }

    // Botao esquerdo = "estou atirando". Antes era `GetAsyncKeyState(VK_LBUTTON)`
    // (sempre 0 aqui); agora vem do hook. Index 5 = Left Click, o MESMO do
    // mapeamento do hotkey.
    bool isFiring = HotkeyHook::IsDown(HotkeyHook::KEY_LBUTTON);
    if (isFiring) {
        clampedSmooth *= 0.8f;
    }

    // Smoothing: AR_SmoothValue and Aggression directly control speed at all ranges
    float aggression = clamp_value(Configs.Aimbot.Aggression, 0.5f, 3.0f);
    float smoothFactor = clampedSmooth / aggression;
    smoothFactor = clamp_value(smoothFactor, 0.15f, 1.0f);

    float curve = 1.0f - std::pow(smoothFactor, 2.0f);
    if (curve < 0.05f) curve = 0.05f; 
    if (curve > 0.45f) curve = 0.45f;
    // ADVANCED DIAGNOSTICS
    /*
    LOG("[Aimbot] Target: (%.1f, %.1f) | Center: (%.1f, %.1f) | Delta: (%.1f, %.1f)\n", 
        targetScreenPos.x, targetScreenPos.y, viewCenter.x, viewCenter.y, dx, dy);
    LOG("[MoveMath] DistP: %.1f | Smooth: %.2f | Curve: %.3f\n", distPixels, smoothFactor, curve);
    */

    dx *= curve;
    dy *= curve;
    
    //LOG("[MoveMath] DistP: %.1f | Smooth: %.2f | Curve: %.2f\n", distPixels, smoothFactor, curve);

    // Random multiplier for organic feel 
    std::uniform_real_distribution<float> randMult(1.0f, 1.5f);
    float maxStep = (distPixels < 80.0f) ? 60.0f : 30.0f;
    maxStep *= aggression * randMult(rng);
    // ---------------------------------------------------------------------
    // TETO ABSOLUTO DE PASSO: 48 px.
    //
    // A formula acima chegava a 60 * 3.0 * 1.5 = 270 px de salto por pacote.
    // Saltos assim sao FICKS: ninguem corre a mira 270 px num frame. O teto
    // absoluto nao depende da configuracao do usuario (Aggression no maximo
    // continua acelerando, so' nao passa dos 48).
    // ---------------------------------------------------------------------
    if (maxStep > 48.0f) maxStep = 48.0f;
    // ---------------------------------------------------------------------
    // APROXIMACAO PROPORCIONAL PERTO DO ALVO: a menos de 8 px, cada passo
    // cobre no maximo METADE da distancia restante (+0.5 para nao estagnar
    // com a redondeza para inteiro). Sem isto, o ultimo pedaco chega com um
    // passo cheio que ULTRAPASSA o alvo e volta — o vaivem de sub-pixel que
    // o teto do MaxDelta nao resolve por si (ele mede entre pacotes, nao a
    // distancia ate' o ponto).
    // ---------------------------------------------------------------------
    if (distPixels < 8.0f) {
        const float tetoProximo = distPixels * 0.5f + 0.5f;
        if (maxStep > tetoProximo) maxStep = tetoProximo;
    }

    float stepLen = std::sqrt(dx * dx + dy * dy);
    if (stepLen > maxStep && stepLen > 0.0f) {
        float scale = maxStep / stepLen;
        dx *= scale;
        dy *= scale;
    }

    // Subtle jitter layer - reduced for PUBG stability (was 0.6f)
    std::uniform_real_distribution<float> jitterDist(-0.15f, 0.15f);
    dx += jitterDist(rng);
    dy += jitterDist(rng);

    dx *= Configs.Aimbot.DriverScale;
    dy *= Configs.Aimbot.DriverScale;

    float targetX = dx + remainderX;
    float targetY = dy + remainderY;

    int moveX = static_cast<int>(std::round(targetX));
    int moveY = static_cast<int>(std::round(targetY));
    
    remainderX = targetX - static_cast<float>(moveX);
    remainderY = targetY - static_cast<float>(moveY);

    bool breakingX = std::abs(moveX) < std::abs(lastMoveX);
    if (!breakingX && std::abs(moveX - lastMoveX) > Configs.Aimbot.MaxDelta) {
        int oldMove = moveX;
        moveX = lastMoveX + (moveX > lastMoveX ? Configs.Aimbot.MaxDelta : -Configs.Aimbot.MaxDelta);
        //LOG("[BRAKE] X Cap: %d -> %d\n", oldMove, moveX);
    }

    bool breakingY = std::abs(moveY) < std::abs(lastMoveY);
    if (!breakingY && std::abs(moveY - lastMoveY) > Configs.Aimbot.MaxDelta) {
        moveY = lastMoveY + (moveY > lastMoveY ? Configs.Aimbot.MaxDelta : -Configs.Aimbot.MaxDelta);
    }

    // ---------------------------------------------------------------------
    // DESTRAVAR O EIXO VERTICAL (Configs.Aimbot.AimbotUnlockVertical)
    //
    // Zera o componente VERTICAL da injecao (= pitch). O horizontal (moveX,
    // = yaw) segue corrigido normalmente. Ver o bloco grande acima de
    // MoveMouseTowards sobre por que "eixo vertical" = dy da injecao.
    //
    // `remainderY` tambem zera: ele e' o acumulador fracionario do anti-snap.
    // Sem zerar, a correcao vertical calculada nos frames anteriores ficaria
    // guardada e seria despejada de uma vez quando o toggle fosse desligado —
    // exatamente o flick que a opcao existe para evitar.
    //
    // `lastMoveY` nao precisa de tratamento: ele recebe `moveY` logo abaixo.
    // ---------------------------------------------------------------------
    if (Configs.Aimbot.AimbotUnlockVertical) {
        moveY = 0;
        remainderY = 0.0f;
    }

    lastMoveX = moveX;
    lastMoveY = moveY;

    if (moveX == 0 && moveY == 0) {
        g_aimFunnel.mmZeroMove++;        // CENSO (ultima milha)
        return;
    }

    //LOG("[Aimbot] Inject: [%d, %d] (Rem: %.2f, %.2f)\n", moveX, moveY, remainderX, remainderY);
    static auto lastInjLog = std::chrono::steady_clock::now();
    auto nowInj = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(nowInj - lastInjLog).count() > 1000) {
        lastInjLog = nowInj;
        LOG("[AIM] inject mouse dx=%d dy=%d\n", moveX, moveY);
    }
    TargetProcess.driver.InjectMouseMove(moveX, moveY);
    lastMoveTime = now;
}

void Aimbot::Tick() {
    // -----------------------------------------------------------------------
    // CALIBRACAO EM ANDAMENTO -> O AIMBOT FICA FORA.
    //
    // PRIMEIRA coisa do Tick, antes ate' do censo. O aimbot injeta mouse pelo
    // MESMO caminho (InjectMouseMove) que o pulso de calibracao: se ele
    // corrigisse a mira durante a medicao, aquela correcao entraria na conta
    // como se fosse a sensibilidade do jogador, e o valor calibrado sairia
    // errado — do jeito mais silencioso possivel.
    // -----------------------------------------------------------------------
    if (IsCalibrating()) return;

    // CENSO DO FUNIL: publica o censo 1x/s ANTES de qualquer `return`. Tem
    // que ser aqui em cima — o `FUNIL(1s)` antigo ficava no fim do Tick (e
    // so' era alcancado com alvo escolhido), por isso nunca aparecia quando o
    // aimbot morria cedo, que e' exatamente o caso investigado.
    AimFunnelCensusLog();
    g_aimFunnel.ticks++;

    // NOTA: `lastProcessedTime` era um `static` local AQUI. Virou
    // `g_aimLastProcessedTime` (escopo de arquivo, declarado la' em cima) SO'
    // para o censo poder imprimir `lastProc`. Uma instancia so', mesma
    // semantica, mesmos tipos.

    // NOTA: o diagnostico [KEY] (estado do hook x GetAsyncKeyState cru) NAO
    // fica aqui. O `Tick` so' roda depois que o `TslGame.exe` foi encontrado
    // (o MemoryLoop depende do EngineInstance, criado so' no fim do
    // InitializeFeaturesThread), e o teste tem que dar para fazer SEM abrir o
    // jogo. Ele esta' no `RenderFrame` (Main.cpp), que roda todo frame desde o
    // primeiro segundo do overlay. O estado lido aqui e' o MESMO
    // (`HotkeyHook::IsDown`).

    if (!Configs.Aimbot.Enabled) {
        currentTargetAddress = 0;
        g_aimFunnel.disabled++;      // CENSO: return #1
        return;
    }
    
    if (!IsHotkeyDown(Configs.Aimbot.Hotkey)) {
        currentTargetAddress = 0;
        lastMoveX = 0;
        lastMoveY = 0;
        // DIAGNOSTICO: avisa (1x/1.5s) que o hotkey NAO esta pressionado.
        //
        // E diz POR QUE, no mesmo lugar. Antes so' saia o numero do hotkey, e
        // isso nao distingue "o hook nao recebe nada" de "recebo, mas a tecla
        // mapeada esta' errada" — sao causas opostas e a linha servia para as
        // duas.
        //
        //   hook=0            -> os hooks nao instalaram; e' outro problema
        //   evt parado em 0   -> o hook esta' instalado mas NAO RECEBE input
        //   evt subindo       -> recebe; se mask nao tem o bit, a TECLA esta'
        //                        errada (Hotkey=1 e' KEY_RBUTTON)
        //
        // O `evt` e' cumulativo e conta QUALQUER evento (mouse ou teclado),
        // inclusive movimento que o MouseProc ignora — entao ele sobe so' por
        // mexer o mouse. Zero significa hook morto, sem ambiguidade.
        static auto lastHKLog = std::chrono::steady_clock::now();
        auto nowHK = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(nowHK - lastHKLog).count() > 1500) {
            lastHKLog = nowHK;
            LOG("[AIM] waiting for hotkey (%d)... (ativo=%d) | hook=%d mask=0x%02X evt=%u\n",
                Configs.Aimbot.Hotkey, (int)Configs.Aimbot.Enabled,
                (int)HotkeyHook::Installed(),
                (unsigned)HotkeyHook::DownMask(),
                (unsigned)HotkeyHook::EventCount());
        }
        g_aimFunnel.hotkey++;        // CENSO: return #2
        return;
    }
    
    SharedData data;
    {
        std::lock_guard<std::mutex> lock(GDataMutex);

        // CENSO: captura `ut` (e o tamanho da lista de atores) AQUI, ANTES do
        // gate — e' o unico jeito de o censo mostrar o valor que o gate
        // REJEITOU. O lock ja' esta' na mao, entao a leitura e' de graca.
        // Fica atras de DebugLogs para o caminho normal nao pagar nada.
        if (Configs.Survivor.DebugLogs) {
            g_aimFunnel.ut = (uint64_t)GSharedData.LastUpdateTime;
            g_aimFunnel.actors = (uint64_t)GSharedData.Actors.size();
            g_aimFunnel.povFov = (double)GSharedData.CameraCache.POV.FOV;
            g_aimFunnel.povLocX = (double)GSharedData.CameraCache.POV.Location.X;
            g_aimFunnel.povLocY = (double)GSharedData.CameraCache.POV.Location.Y;
            g_aimFunnel.povLocZ = (double)GSharedData.CameraCache.POV.Location.Z;
            // Rotacao + viewport: os dois dados que faltavam para separar
            // "camera ruim" de "raio de FOV zerado" (ver o struct do censo).
            g_aimFunnel.povRotP = (double)GSharedData.CameraCache.POV.Rotation.Pitch;
            g_aimFunnel.povRotY = (double)GSharedData.CameraCache.POV.Rotation.Yaw;
            g_aimFunnel.povRotR = (double)GSharedData.CameraCache.POV.Rotation.Roll;
            g_aimFunnel.vpX = (double)Gui.Window.Size.x;
            g_aimFunnel.vpY = (double)Gui.Window.Size.y;
        }

        if (GSharedData.LastUpdateTime <= g_aimLastProcessedTime) {
            g_aimFunnel.timeout++;   // CENSO: return #3 (SUSPEITO Nº 1)
            return;
        }
        data = GSharedData;
        g_aimLastProcessedTime = data.LastUpdateTime;
    }

    if (data.Actors.empty() || !EngineInstance) {
        g_aimFunnel.noActors++;      // CENSO: return #4
        // NAO sai mudo: "sem atores" e' um modo de falha, e o usuario precisa
        // saber que o gate do hotkey passou e o problema e' DEPOIS dele.
        static auto lastNoAct = std::chrono::steady_clock::now();
        auto nowNoAct = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(nowNoAct - lastNoAct).count() > 1000) {
            lastNoAct = nowNoAct;
            LOG("[AIM] ATIVO (hotkey ok) mas SEM ATORES: lista=%zu engine=%d | "
                "ut=%llu ultimoProcessado=%llu\n",
                data.Actors.size(), (int)(EngineInstance != nullptr),
                (unsigned long long)data.LastUpdateTime,
                (unsigned long long)g_aimLastProcessedTime);
        }
        return;
    }

    // =======================================================================
    // UMA LINHA POR SEGUNDO, SEMPRE — o "esta' vivo e vendo o que" do aimbot.
    //
    // Todo o resto do diagnostico do aimbot vive dentro de
    // `if (Configs.Survivor.DebugLogs)`, que e' default OFF. Resultado: quando
    // ele falha, NAO SOBRA RASTRO NENHUM — o log do usuario tem so'
    // "[AIM] aguardando hotkey", e nao da' para distinguir "nunca achou alvo"
    // de "achou e reprovou" de "projetou e desistiu".
    //
    // Aqui e' o essencial: quantos atores chegaram, quantos passaram o
    // `isCheck`/`isDie`/`Health`, e a camera do momento. Custa uma linha por
    // segundo (o mesmo que a do hotkey ja' custa) e responde a primeira
    // pergunta de qualquer investigacao: o aimbot ESTA' VENDO alguem?
    // =======================================================================
    {
        static auto lastVivo = std::chrono::steady_clock::now();
        auto nowVivo = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(nowVivo - lastVivo).count() > 1000) {
            lastVivo = nowVivo;
            size_t total = 0, vivos = 0, checaveis = 0, comIsCheck = 0, comVida = 0;
            char exemplo[160] = {0};
            for (auto& ac : data.Actors) {
                if (!ac) continue;
                total++;
                if (!ac->isDie) vivos++;
                if (ac->isCheck) comIsCheck++;
                if (ac->Health > 0.0f) comVida++;
                if (ac->isCheck && !ac->isDie && ac->Health > 0.0f) checaveis++;
                // O PRIMEIRO ator com classe: o exemplo que diz qual dos dois
                // filtros esta' reprovando. Sem ele a contagem diz "0" e nao
                // diz se o culpado e' `isCheck` ou `Health`.
                if (exemplo[0] == 0 && ac->Class) {
                    snprintf(exemplo, sizeof(exemplo),
                             "ex: class=0x%llX isCheck=%d isDie=%d Health=%.3f obj='%s'",
                             (unsigned long long)ac->Class, (int)ac->isCheck,
                             (int)ac->isDie, ac->Health, ac->objName.c_str());
                }
            }
            LOG("[AIM] ATIVO: atores=%zu vivos=%zu | isCheck=%zu vida>0=%zu CHECAVEIS=%zu | "
                "POV fov=%.1f rot=(%.2f,%.2f) | cfg FOV=%.0f | hotkey=%d\n"
                "          %s\n",
                total, vivos, comIsCheck, comVida, checaveis,
                data.CameraCache.POV.FOV,
                data.CameraCache.POV.Rotation.Pitch,
                data.CameraCache.POV.Rotation.Yaw,
                Configs.Aimbot.FOV,
                (int)IsHotkeyDown(Configs.Aimbot.Hotkey),
                exemplo[0] ? exemplo : "(nenhum ator com Class)");
        }
    }

    // =======================================================================
    // CAMERA DO AIMBOT — FALHA ALTO (nao existe mais `return` mudo aqui).
    //
    // `Camera::WorldToScreen` rejeita FOV 0/nao-finito e devolve (0,0) para
    // TODO candidato (guarda em Camera.cpp:80). Sem esta checagem o Tick
    // seguia, reprovava todo mundo em `projFail`, caia no `noTarget` (mais
    // abaixo) e retornava em SILENCIO — porque aquele `return` e' ANTERIOR ao
    // bloco de diagnostico. Era isso que fazia "o aimbot nao faz nada" nao
    // deixar rastro nenhum no logs.txt.
    //
    // A camera do aimbot e' `Engine::CameraEntry` (via GSharedData.CameraCache),
    // NAO a mesma que o ESP usa. O ESP rele' a camera sozinho no RenderFrame
    // (`Camera::GetCurrentView`), entao ele continua desenhando normalmente
    // enquanto o aimbot esta' morto — que e' exatamente o sintoma relatado.
    //
    // O log le' TAMBEM a camera viva pela mesma funcao que o ESP usa. Isso
    // separa na hora as duas causas possiveis:
    //   viva OK  + POV ruim  -> `Engine::CameraEntry` parou de ser atualizada
    //                           (nunca chegou no RefreshViewMatrix).
    //   viva RUIM + POV ruim -> o `PlayerCameraManager`/offsets de camera.
    //
    // Sai 1x/s e NAO depende de `Configs.Survivor.DebugLogs`: falha silenciosa
    // e' o bug, entao ela nao pode ficar atras de um checkbox.
    // =======================================================================
    {
        const float povFovNow = data.CameraCache.POV.FOV;
        if (!std::isfinite(povFovNow) || povFovNow < 1.0f || povFovNow > 170.0f) {
            g_aimFunnel.badCamera++;     // CENSO: return #4b

            static auto lastCamLog = std::chrono::steady_clock::now();
            auto nowCam = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(nowCam - lastCamLog).count() > 1000) {
                lastCamLog = nowCam;

                const MinimalViewInfo viva = Camera::GetCurrentView(data.CameraManagerAddr);
                LOG("[AIM] CAMERA INVALIDA: POV.fov=%.3f POV.loc=%.1f,%.1f,%.1f | "
                    "camMgr=0x%llX | camera viva (mesma do ESP): fov=%.2f loc=%.1f,%.1f,%.1f | "
                    "leitura_esp=%s -> %s\n",
                    povFovNow,
                    data.CameraCache.POV.Location.X,
                    data.CameraCache.POV.Location.Y,
                    data.CameraCache.POV.Location.Z,
                    (unsigned long long)data.CameraManagerAddr,
                    viva.FOV, viva.Location.X, viva.Location.Y, viva.Location.Z,
                    (viva.FOV >= 1.0f && viva.FOV <= 170.0f) ? "OK" : "RUIM",
                    (viva.FOV >= 1.0f && viva.FOV <= 170.0f)
                        ? "Engine::CameraEntry nao esta' sendo atualizada (RefreshViewMatrix)"
                        : "PlayerCameraManager/offsets de camera");
            }
            return;
        }
    }

    // -----------------------------------------------------------------------
    // VIEWPORT DO AIMBOT — O MESMO QUE A PROJECAO USA.
    //
    // `Camera::WorldToScreen` cai em `GetSystemMetrics(SM_CXSCREEN/CYSCREEN)`
    // quando `Gui.Window.Size` vem menor que 10 (Camera.cpp:103 e :163). O
    // `viewCenter` e o `fovPixelRadius` daqui NAO tinham esse fallback: liam
    // `Gui.Window.Size` cru.
    //
    // Com `Gui.Window.Size` zerado os dois lados passam a medir coisas
    // diferentes: a projecao centra na METADE DA TELA DO SISTEMA, e o
    // `viewCenter` fica em (0,0) com o raio de FOV no clamp de 10px. Toda
    // distancia medida vira "distancia ate' o canto superior esquerdo", e TODO
    // candidato reprova por `fovFail`, todo tick, sem excecao.
    //
    // Este e' o numero medido no log: fovFail = 15 candidatos x 126 ticks =
    // 1890, com projFail = 0 e actorFail = 0.
    //
    // Agora os dois lados usam a MESMA resolucao: o cone de FOV nao pode
    // discordar da projecao que o alimenta.
    // -----------------------------------------------------------------------
    float vpW = Gui.Window.Size.x;
    float vpH = Gui.Window.Size.y;
    if (vpW < 10.0f || vpH < 10.0f) {
        vpW = (float)GetSystemMetrics(SM_CXSCREEN);
        vpH = (float)GetSystemMetrics(SM_CYSCREEN);
    }

    Vector2 viewCenter(vpW / 2.0f, vpH / 2.0f);
    Vector3 localPos = Vector3(data.CameraCache.POV.Location.X, data.CameraCache.POV.Location.Y, data.CameraCache.POV.Location.Z);
    
    float fovRad = Configs.Aimbot.FOV * (float)M_PI / 180.0f;
    float fovPixelRadius = std::tan(fovRad * 0.5f) * vpH;
    fovPixelRadius = clamp_value(fovPixelRadius, 10.0f, vpH * 0.5f);
    if (Configs.Survivor.DebugLogs) {
        g_aimFunnel.fovPx  = (double)fovPixelRadius;
        g_aimFunnel.fovCfg = (double)Configs.Aimbot.FOV;
    }

    std::shared_ptr<ActorEntity> bestTarget = nullptr;
    float bestScore = -999999.0f;
    Vector3 bestTargetPos(0, 0, 0);

    if (Configs.Aimbot.LockOnTarget && currentTargetAddress != 0) {
        for (auto& actor : data.Actors) {
            if (!actor->isCheck || actor->isDie || actor->Health <= 0) { g_aimFunnel.actorFail++; continue; }  // CENSO
            if (actor->Class != currentTargetAddress) continue;
            bestTarget = actor;
            int boneIndex = GetBestBone(actor, viewCenter);
            bestTargetPos = AimPointFor(actor, boneIndex);
            break;
        }
    }

    if (!bestTarget) {
        for (auto& actor : data.Actors) {
            if (!actor->isCheck || actor->isDie || actor->Health <= 0) { g_aimFunnel.actorFail++; continue; }  // CENSO
            if (actor->Class == data.AcknowledgedPawn) { g_aimFunnel.selfTeamFail++; continue; }  // CENSO
            // -----------------------------------------------------------------
            // TIME — `Local.Teamid != 0` E' OBRIGATORIO.
            //
            // `Local.Teamid` so' e' escrito quando o pawn local aparece na lista
            // de atores (Engine.cpp:1568). Enquanto ele for 0, significa "ainda
            // nao sei o meu time" — NAO "meu time e' o time 0".
            //
            // Sem a guarda, um ator com `LastTeamNum == 0` (leitura nao feita,
            // ou jogador sem time atribuido ainda) era descartado como colega.
            // O descarte e' um `continue` ANTES de projecao/FOV e ANTES do
            // bloco de diagnostico: o aimbot nao achava alvo nenhum e nao
            // deixava rastro nenhum no log.
            //
            // Engine.cpp:1935 e 2157 ja' usavam exatamente esta guarda. Aqui
            // faltava.
            // -----------------------------------------------------------------
            if (Local.Teamid != 0 && actor->LastTeamNum == Local.Teamid) { g_aimFunnel.selfTeamFail++; continue; }  // CENSO
            if (Configs.Aimbot.VisibilityCheck && !actor->IsVisible) { g_aimFunnel.visFail++; continue; }  // CENSO

            int boneIndex = GetBestBone(actor, viewCenter);
            Vector3 targetPos = AimPointFor(actor, boneIndex);

            float dist = Vector3::Distance(localPos, targetPos);
            float bulletSpeed = EngineInstance->GetCurrentBulletSpeed();
            float flyTime = (bulletSpeed > 1.0f) ? (dist / (bulletSpeed * 100.0f)) : 0.0f;
            
            Vector3 predictedTarget = targetPos + (actor->Velocity * flyTime);
            Vector2 screenPos = Camera::WorldToScreen(data.CameraCache.POV, predictedTarget);
            if (screenPos.x == 0.0f && screenPos.y == 0.0f) { g_aimFunnel.projFail++; continue; }  // CENSO
            
            float dx = screenPos.x - viewCenter.x;
            float dy = screenPos.y - viewCenter.y;
            float distPixels = std::sqrt(dx * dx + dy * dy);
            
            float effectiveFovRadius = fovPixelRadius;
            if (actor->Class == currentTargetAddress) {
                effectiveFovRadius *= 2.5f;
            }
            if (distPixels > effectiveFovRadius) { g_aimFunnel.fovFail++; continue; }  // CENSO

            float fovDegreesMatch = std::atan2(distPixels, Gui.Window.Size.y) * (180.0f / (float)M_PI) * 2.0f;
            float score = CalculateTargetScore(actor, targetPos, fovDegreesMatch);

            if (score > bestScore) {
                bestScore = score;
                bestTarget = actor;
                bestTargetPos = targetPos;
            }
        }
    }

    if (!bestTarget) {
        if (currentTargetAddress != 0 && !targetInGrace) {
            lastTargetSeenTime = std::chrono::steady_clock::now();
            targetInGrace = true;
            g_aimFunnel.graceStart++;    // CENSO: return #5 (mudo no log antigo)
            return;
        }
        if (targetInGrace) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - lastTargetSeenTime).count();
            if (elapsed < 500) {         // Extended grace for DMA latency
                g_aimFunnel.graceWait++; // CENSO: return #6 (mudo no log antigo)
                return;
            }
        }
        currentTargetAddress = 0;
        targetInGrace = false;
        lastMoveX = 0;
        lastMoveY = 0;
        g_aimFunnel.noTarget++;          // CENSO: return #7 (mudo no log antigo)

        // DIAGNOSTICO NO PONTO EXATO DA MORTE. Era aqui que o log ficava mudo:
        // o bloco `[AIM] DESISTIU` esta' DEPOIS deste `return`, entao ele nunca
        // explicava o caso `noTarget`. Este dump explica (1x/s, DebugLogs).
        AimNoTargetDump(data, viewCenter, fovPixelRadius);
        return;
    }

    targetInGrace = false;

    if (currentTargetAddress != bestTarget->Class) {
        //LOG("[Aimbot] New Target: 0x%llX\n", bestTarget->Class);
        lastTargetSwitchTime = std::chrono::steady_clock::now();
        lastMoveX = 0; 
        lastMoveY = 0;
    }
    currentTargetAddress = bestTarget->Class;
    int oldBone = currentBoneId;
    currentBoneId = GetBestBone(bestTarget, viewCenter);
    if (oldBone != currentBoneId) {
        //LOG("[Aimbot] Bone Switch: %d -> %d\n", oldBone, currentBoneId);
    }    
    float dropPitchOffset = 0.0f;
    // O checkbox "Prediction" agora realmente controla a previsao balistica
    // (antes ele existia no menu mas nao fazia nada: a previsao era sempre
    // aplicada, ignorando a config).
    Vector3 predictedPos = Configs.Survivor.Prediction
        ? EngineInstance->GetPrediction(bestTarget, bestTargetPos, dropPitchOffset)
        : bestTargetPos;
    
    float dx_3d = predictedPos.x - localPos.x;
    float dy_3d = predictedPos.y - localPos.y;
    float dist2D = std::sqrt(dx_3d * dx_3d + dy_3d * dy_3d);
    
    float dropPitchRad = dropPitchOffset * ((float)M_PI / 180.0f);
    
    float zShift = 0.0f;
    if (dist2D > 5000.0f) {
        zShift = dist2D * std::tan(dropPitchRad);
    }
    
    predictedPos.z += zShift;

    // [DIAG] guarda a posicao prevista ANTES do bloco de recuo abaixo. O bloco
    // de recuo REESCREVE predictedPos (linhas ~564-566), entao sem esta copia o
    // log de desistencia nao conseguiria separar "predicao balistica errada" de
    // "recuo reescreveu a posicao".
    const Vector3 predictedPosPreRecoil = predictedPos;

    // PAOD-style recoil compensation - reduces aim movement instead of modifying target position
    // Botao esquerdo pelo hook (era GetAsyncKeyState(VK_LBUTTON), sempre 0 aqui).
    bool isFiring = HotkeyHook::IsDown(HotkeyHook::KEY_LBUTTON);
    if (isFiring) {
        // Get current camera rotation
        float currentPitch = data.CameraCache.POV.Rotation.Pitch;
        float currentYaw = data.CameraCache.POV.Rotation.Yaw;

        // Calculate angle to target
        float dx = predictedPos.x - localPos.x;
        float dy = predictedPos.y - localPos.y;
        float dz = predictedPos.z - localPos.z;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

        float targetPitch = -std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * (180.0f / (float)M_PI);
        float targetYaw = std::atan2(dy, dx) * (180.0f / (float)M_PI);

        // Normalize angles
        while (targetYaw > 180.0f) targetYaw -= 360.0f;
        while (targetYaw < -180.0f) targetYaw += 360.0f;
        if (targetPitch > 89.0f) targetPitch = 89.0f;
        if (targetPitch < -89.0f) targetPitch = -89.0f;

        // Calculate angle delta
        float deltaPitch = targetPitch - currentPitch;
        float deltaYaw = targetYaw - currentYaw;

        // Normalize delta yaw
        while (deltaYaw > 180.0f) deltaYaw -= 360.0f;
        while (deltaYaw < -180.0f) deltaYaw += 360.0f;

        // Apply recoil compensation (PAOD style - reduces the angle delta)
        float recoilComp = Configs.Aimbot.AR_RecoilValue / 100.0f;
        float recoilCompYaw = Configs.Aimbot.AR_RecoilYaw / 100.0f;

        deltaPitch *= recoilComp;
        deltaYaw *= recoilCompYaw;

        // Calculate final compensated angle
        float finalPitch = currentPitch + deltaPitch;
        float finalYaw = currentYaw + deltaYaw;

        // Normalize final angles
        while (finalYaw > 180.0f) finalYaw -= 360.0f;
        while (finalYaw < -180.0f) finalYaw += 360.0f;
        if (finalPitch > 89.0f) finalPitch = 89.0f;
        if (finalPitch < -89.0f) finalPitch = -89.0f;

        // Convert back to world position using compensated angle
        float pitchRad = finalPitch * ((float)M_PI / 180.0f);
        float yawRad = finalYaw * ((float)M_PI / 180.0f);

        predictedPos.x = localPos.x + std::cos(yawRad) * std::cos(pitchRad) * dist;
        predictedPos.y = localPos.y + std::sin(yawRad) * std::cos(pitchRad) * dist;
        predictedPos.z = localPos.z - std::sin(pitchRad) * dist;
    }

    Vector2 moveMouse = Camera::WorldToScreen(data.CameraCache.POV, predictedPos);
    
    float dist = Vector3::Distance(localPos, predictedPos);
   // LOG("[AimbotDebug] Target: 0x%llX | Bone: %d | Dist: %.1f | DropPitchOffset: %.4f | ZShift: %.1f\n", 
     //   bestTarget->Class, currentBoneId, dist, dropPitchOffset, zShift);
   // LOG("[AimbotDebug] Recoil: {%.2f, %.2f} | FinalScreenPos: {X:%.1f, Y:%.1f} | ViewCenter: {X:%.1f, Y:%.1f}\n", 
     //   data.Recoil.x, data.Recoil.y, moveMouse.x, moveMouse.y, viewCenter.x, viewCenter.y);

    // =======================================================================
    // DIAGNOSTICO DE DESISTENCIA — TEM QUE FICAR ANTES DO `return` ABAIXO.
    //
    // O log `[AIM] alvo=` original estava DEPOIS deste `return`. Resultado
    // medido em campo: 50 linhas "[AIM] aguardando hotkey" e ZERO linhas
    // "[AIM] alvo=", porque toda vez que o botao estava pressionado o
    // `WorldToScreen` devolvia o sentinela (0,0) e a funcao saia AQUI, em
    // silencio. Ou seja: o diagnostico existia, mas nunca era alcancado
    // exatamente no caso que precisa ser diagnosticado.
    //
    // -----------------------------------------------------------------------
    // COMO LER ESTA LINHA (a regra que decide o veredito):
    //
    //   `tela_osso_cru` = WorldToScreen do OSSO CRU (bestTargetPos), SEM
    //                     predicao balistica, SEM recuo, SEM zShift.
    //   `prev_pre_recuo`= WorldToScreen da posicao com predicao balistica.
    //   `moveMouse`/`tela_final` = a que decidiu este `return`.
    //
    //   - tela_osso_cru VALIDO  -> a camera/POV esta BOA, e o problema esta'
    //                              NA PREDICAO BALISTICA (Velocity, flyTime,
    //                              dropPitchOffset/zShift, ou o reescrita do
    //                              bloco de recuo). ⭐ caso mais provavel.
    //   - tela_osso_cru TAMBEM (0,0) -> o problema e' a CAMERA/POV: FOV zerado,
    //                              POV lido de memoria reciclando, Location em
    //                              (0,0,0)/lixo, ou rotacao nao-finita. As
    //                              guardas da `Camera::WorldToScreen` estao
    //                              rejeitando a PROPRIA camera (nao o alvo).
    //
    // 1x/s, atras de Configs.Survivor.DebugLogs (sem campo de config novo).
    // =======================================================================
    if (Configs.Survivor.DebugLogs) {
        // ----------------------- CENSO DO FUNIL -----------------------------
        // Conta ONDE, no meio do Tick, os alvos foram descartados. Serve para
        // responder de graca a pergunta "o aimbot chegou a ver inimigo?" (o
        // log antigo nao distinguia "sem alvo" de "alvo descartado").
        static int s_diagTicks = 0;
        static int s_diagCand = 0;      // passaram isCheck/isDie/Health/team/vis
        static int s_diagProjFail = 0;  // WorldToScreen do previsto == (0,0)
        static int s_diagFovFail = 0;   // fora do raio de FOV em pixels
        s_diagTicks++;
        if (bestTarget) {
            s_diagCand++;
            // Reconstroi a tela do alvo escolhido como ela era na SELECAO
            // (mesma predicao da linha ~435), para saber se a reprovacao veio
            // da projecao ou do raio de FOV.
            float flySel = 0.0f;
            if (EngineInstance) {
                float bsSel = EngineInstance->GetCurrentBulletSpeed();
                flySel = (bsSel > 1.0f) ? (dist / (bsSel * 100.0f)) : 0.0f;
            }
            Vector2 selScreen = Camera::WorldToScreen(
                data.CameraCache.POV, bestTargetPos + (bestTarget->Velocity * flySel));
            if (selScreen.x == 0.0f && selScreen.y == 0.0f) s_diagProjFail++;
            else {
                float ddx = selScreen.x - viewCenter.x;
                float ddy = selScreen.y - viewCenter.y;
                if (std::sqrt(ddx * ddx + ddy * ddy) > fovPixelRadius) s_diagFovFail++;
            }
        }

        static auto lastDeadLog = std::chrono::steady_clock::now();
        static auto lastAliveLog = std::chrono::steady_clock::now();
        auto nowDead = std::chrono::steady_clock::now();

        // ---- (A) PROJECAO DO OSSO CRU (sem predicao / sem recuo) ----------
        Vector2 rawBone = Camera::WorldToScreen(data.CameraCache.POV, bestTargetPos);
        // ---- (B) PROJECAO DA PREDICAO, antes do recuo reescrever ----------
        Vector2 predPreRecoil = Camera::WorldToScreen(data.CameraCache.POV, predictedPosPreRecoil);

        if (moveMouse.x == 0.0f && moveMouse.y == 0.0f) {
            // DESISTIU: este e' o caminho que antes saia mudo.
            if (std::chrono::duration_cast<std::chrono::milliseconds>(nowDead - lastDeadLog).count() > 1000) {
                lastDeadLog = nowDead;

                // cru: distancia/class do alvo escolhido (bestTarget)
                LOG("[AIM] DESISTIU: moveMouse=(%.3f,%.3f) | tela_final=(%.3f,%.3f) | "
                    "alvo=0x%llX dist=%.1fm class=0x%llX | osso=%.0f,%.0f,%.0f | "
                    "tela_osso_cru=(%.3f,%.3f) | prev_pre_recuo=(%.1f,%.1f,%.1f) tela_prev=(%.3f,%.3f) | "
                    "prev_final=(%.1f,%.1f,%.1f) | "
                    "POV loc=%.1f,%.1f,%.1f rot=%.2f,%.2f,%.2f fov=%.1f | "
                    "vp=%.0fx%.0f fovPx=%.0f atirando=%d pred=%d bs=%.0f dropP=%.3f zShift=%.1f | "
                    "camMgr=0x%llX pc=0x%llX gs=0x%llX/%d hotkey=%d\n",
                    moveMouse.x, moveMouse.y, moveMouse.x, moveMouse.y,
                    (unsigned long long)(bestTarget ? bestTarget->Class : 0),
                    dist, (unsigned long long)(bestTarget ? bestTarget->Class : 0),
                    bestTargetPos.x, bestTargetPos.y, bestTargetPos.z,
                    rawBone.x, rawBone.y,
                    predictedPosPreRecoil.x, predictedPosPreRecoil.y, predictedPosPreRecoil.z,
                    predPreRecoil.x, predPreRecoil.y,
                    predictedPos.x, predictedPos.y, predictedPos.z,
                    data.CameraCache.POV.Location.X, data.CameraCache.POV.Location.Y,
                    data.CameraCache.POV.Location.Z,
                    data.CameraCache.POV.Rotation.Pitch, data.CameraCache.POV.Rotation.Yaw,
                    data.CameraCache.POV.Rotation.Roll, data.CameraCache.POV.FOV,
                    Gui.Window.Size.x, Gui.Window.Size.y, fovPixelRadius,
                    (int)isFiring, (int)Configs.Survivor.Prediction,
                    EngineInstance ? EngineInstance->GetCurrentBulletSpeed() : 0.0f,
                    dropPitchOffset, zShift,
                    (unsigned long long)data.CameraManagerAddr,
                    (unsigned long long)(EngineInstance ? EngineInstance->PlayerController : 0),
                    (unsigned long long)data.UWorld, (int)data.DataValid,
                    (int)IsHotkeyDown(Configs.Aimbot.Hotkey));

                // Linha-resumo do funil: uma por segundo, fechada antes de
                // zerar os contadores (nao existe "media", existe o numero).
                LOG("[AIM] FUNIL(1s): ticks=%d comAlvo=%d projFail=%d fovFail=%d "
                    "bone=%d | se osso_cru acima for valido => predicao; se tambem (0,0) => camera/POV\n",
                    s_diagTicks, s_diagCand, s_diagProjFail, s_diagFovFail, currentBoneId);
                s_diagTicks = 0; s_diagCand = 0; s_diagProjFail = 0; s_diagFovFail = 0;
            }
        } else {
            // NAO desistiu: mantem o antigo "[AIM] alvo=" (mesmo formato)
            // para nao perder a serie historica do log do usuario.
            if (std::chrono::duration_cast<std::chrono::milliseconds>(nowDead - lastAliveLog).count() > 1000) {
                lastAliveLog = nowDead;
                int visible = 0, total = 0;
                for (auto& ac : data.Actors) { total++; if (ac->IsVisible) visible++; }
                LOG("[AIM] alvo=0x%llX dist=%.0f vis=%d/%d hotkey=%d fov=%.0f | "
                    "tela_osso_cru=(%.3f,%.3f) tela_prev=(%.3f,%.3f) POV fov=%.1f\n",
                    (unsigned long long)(bestTarget ? bestTarget->Class : 0),
                    dist, visible, total,
                    (int)IsHotkeyDown(Configs.Aimbot.Hotkey), Configs.Aimbot.FOV,
                    rawBone.x, rawBone.y, predPreRecoil.x, predPreRecoil.y,
                    data.CameraCache.POV.FOV);
            }
        }
    }

    if (moveMouse.x == 0.0f && moveMouse.y == 0.0f) {
        lastMoveX = 0;
        lastMoveY = 0;
        g_aimFunnel.projFinalZero++;     // CENSO: return #8
        return;
    }

    // Ticks que chegam AQUI = `ticks` menos a soma de todos os contadores de
    // `return` acima. O que acontecer daqui pra frente aparece em
    // mmThrottle/mmOnTarget/mmZeroMove (os 3 `return` de MoveMouseTowards).
    MoveMouseTowards(moveMouse, viewCenter);
}

// ===========================================================================
// RCS — Recoil Control System
//
// O SENSOR (o que separa este RCS de um "spray pattern" gravado):
// a arma carrega `RecoilValueVector` — o recuo que o jogo AINDA VAI aplicar a
// camera. A cada frame o jogo DRENA uma fatia dele para a vista. A fatia que
// sumiu entre duas leituras e' exatamente o recuo que a camera acabou de levar.
//
// Nao precisa de tabela por arma nem por acessorio: o proprio jogo diz quanto
// empurrou, e isso acompanha qualquer build/anexo sozinho.
//
//     dropY = o que sumiu do RecoilValueVector.y  (o coice)
//     dropR = o que sumiu do VerticalRecovery     (a recuperacao que a camera
//                                                  ganha de volta)
//     graus = InputPitchScale * (dropY - dropR)
//
// ---------------------------------------------------------------------------
// O UNICO NUMERO QUE FALTAVA: counts-por-grau
//
// Os graus acima sao verdade. O que o cheat NAO sabe e' quanto movimento de
// mouse gira a vista 1 grau — isso depende da SENSIBILIDADE IN-GAME do
// jogador, que nao esta' legivel em lugar nenhum.
//
// Antes isso era o slider "Deg per count", calibrado no olho, e CONCORRIA com
// um segundo slider ("InputPitchScale") que era o MESMO ganho uma segunda vez:
// a conta final era `counts ∝ InputPitchScale / DegPerCount`, entao mexer num
// e compensar no outro — impossivel calibrar. Agora `InputPitchScale` e'
// constante (kInputPitchScale, o 2.5 dos posts do UC) e sobrou UM ganho.
//
// E ele nao precisa ser chutado: `RequestSensitivityCalibration()` MEDE.
//
// ---------------------------------------------------------------------------
// COMO A CALIBRACAO MEDE (e por que isso e' melhor que pedir a sensibilidade)
//
// Injeta um pulso de mouse CONHECIDO (kCalPulse counts em cada eixo), le o
// quanto a vista girou, e divide. Depois injeta o pulso inverso para devolver
// a mira ao lugar.
//
// E' exato por construcao: e' a sensibilidade real, desta maquina, com o
// perfil real deste jogador, medida no jogo. Nao depende de nenhuma tabela
// sensibilidade->graus que ninguem publica de forma confiavel.
// ===========================================================================

namespace {

// InputPitchScale do PUBG. Era `Configs.Aimbot.RCSInputPitchScale`; virou
// constante porque e' propriedade do JOGO, nao preferencia do usuario.
constexpr float kInputPitchScale = 2.5f;

// --- calibracao por pulso -------------------------------------------------
constexpr int kCalPulse = 200;   // counts em CADA eixo (movimento pequeno)
constexpr int kCalChunk = 50;    // counts por frame (espalha o pulso)
constexpr int kCalWait  = 6;     // frames de acomodacao antes de medir

enum CalPhase { CAL_IDLE = 0, CAL_BASE, CAL_PULSE, CAL_WAIT, CAL_READ, CAL_BACK, CAL_DONE };

// --- auto-calibracao ------------------------------------------------------
constexpr float kAutoStep     = 0.04f;    // 4% de ajuste por janela
constexpr float kAutoMinMul   = 0.20f;
constexpr float kAutoMaxMul   = 5.00f;
constexpr float kAutoDeadzone = 0.05f;    // deriva que ainda conta como zero
constexpr int64_t kAutoWindowMs = 500;

// ---------------------------------------------------------------------------
// LEITURA AO VIVO — atravessa thread (memoria escreve, render le).
// ---------------------------------------------------------------------------
std::atomic<float> g_rcsCountsPerDeg{0.0f};
std::atomic<float> g_rcsDropY{0.0f};
std::atomic<float> g_rcsDropR{0.0f};
std::atomic<float> g_rcsDegY{0.0f};
std::atomic<float> g_rcsResidual{0.0f};
std::atomic<int>   g_rcsCy{0};
std::atomic<int>   g_rcsCx{0};
std::atomic<float> g_rcsGainMul{1.0f};
std::atomic<bool>  g_rcsFiring{false};

// --- humanizacao v2: o que a thread de render precisa ver ----------------
// Existem pelo mesmo motivo que os de cima: calibrar OLHANDO o menu em vez
// de abrir o logs.txt no meio da partida.
std::atomic<float> g_rcsLagMs{0.0f};     // latencia sorteada desta rajada
std::atomic<float> g_rcsGainHz{1.0f};    // ganho humano efetivo
std::atomic<float> g_rcsOwedY{0.0f};     // divida do servo (counts)
std::atomic<float> g_rcsStepY{0.0f};     // liberado neste frame
std::atomic<float> g_rcsNoiseY{0.0f};    // desvio correlacionado atual

// --- estado da calibracao (so' a thread de memoria toca) ------------------
std::atomic<bool>  g_calibRequested{false};
std::atomic<bool>  g_calibActive{false};
std::atomic<float> g_calibProgress{0.0f};
std::atomic<int>   g_calibMsg{0};
int   s_calPhase = CAL_IDLE;
int   s_calFrame = 0;
int   s_calSent  = 0;
float s_calBaseP = 0.0f, s_calBaseY = 0.0f, s_calFov = 90.0f;
float s_calMeasP = 0.0f, s_calMeasY = 0.0f;

// Qual caminho de injecao a calibracao esta' usando.
//   false -> kernel (o caminho normal)
//   true  -> SendInput, SO' para o teste comparativo quando o kernel da' zero
// Ver o comentario no CAL_READ: e' o que separa "a injecao nao chega no jogo"
// de "o jogo nao estava' recebendo" de "o calculo esta' errado".
bool  s_calViaSendInput = false;

// --- estado da auto-calibracao (so' a thread de memoria toca) -------------
float s_autoMul = 1.0f;      // multiplicador adaptativo (sessao; NAO e' salvo)
float s_autoAccum = 0.0f;    // deriva acumulada na janela atual (graus)
bool  s_autoHadPitch = false;
float s_autoPrevPitch = 0.0f;
std::chrono::steady_clock::time_point s_autoWindowStart;

// diferenca de angulo com wrap (yaw cruza +-180)
float AngleDiff(float a, float b) {
    float d = a - b;
    while (d > 180.0f)  d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

// ===========================================================================
// HUMANIZACAO DO RCS — ESTADO
//
// Tudo file-scope e so' tocado pela thread de memoria (quem chama
// RecoilControl), igual ao resto do estado do RCS. Nada aqui vai pro config:
// e' estado de SESSAO (inicio de rajada), nao preferencia.
//
//   s_hzBurstIni       — quando a rajada atual comecou (base da rampa)
//   s_hzAtirandoAntes  — para detectar a BORDA (comeco de rajada)
//   s_hzRampa / s_hzJit / s_hzAdiou — o que foi aplicado no ultimo frame.
//                        Nao decidem nada: existem para o log de 1x/s poder
//                        mostrar POR QUE a compensacao saiu diferente do
//                        teorico. Sem isso, "o RCS esta' fraco" vira chute.
// ===========================================================================
std::chrono::steady_clock::time_point s_hzBurstIni{};
bool  s_hzAtirandoAntes = false;
float s_hzRampa = 1.0f;
float s_hzJit   = 1.0f;
bool  s_hzAdiou = false;

// Uniforme e simetrico. A implementacao mora em SDK/RcsNoise.h — o mesmo
// arquivo que `tools\rcs_hand_check.cpp` exercita. Reimplementar aqui seria
// testar uma copia, nao o codigo.
float RandUnit(std::mt19937& r) { return RcsNoise::Unit(r); }
float RandSym(std::mt19937& r)  { return RcsNoise::Sym(r); }   // [-1,1)

// ===========================================================================
// HUMANIZACAO DO RCS — MODELO DE MAO (v2)
//
// POR QUE A v1 NAO BASTAVA. A v1 tinha reacao, jitter e "adiar o envio", e as
// tres eram cosmeticas:
//
//   * a compensacao saia 100% no MESMO frame em que o recuo foi drenado. A
//     vista nunca chegava a subir — quem amostrasse o pitch durante a rajada
//     veria uma linha RETA, quando a de um jogador e' DENTE DE SERRA;
//   * o jitter era ruido BRANCO em cima do valor exato: media zero, novo a
//     cada frame. Qualquer analise que faca media numa janela ve o teorico
//     perfeito e o ruido some. Ruido branco e' o pior dos dois mundos — treme
//     para o jogador e nao esconde nada;
//   * "adiar o envio" so' mudava o instante do pacote, nunca a quantidade
//     devida. A razao `counts injetados / drop do acumulador` seguia
//     INVARIANTE no tempo, frame a frame. Isso e' uma assinatura de
//     CORRELACAO, e sobrevive a qualquer ruido simetrico.
//
// A v2 troca as tres pecas por versoes que sobrevivem a media:
//
//   1. LATENCIA sorteada POR RAJADA (30%..70% de `RCSReactionMs`) — duas
//      rajadas nunca tem a mesma reacao. Um 90 ms fixo e' um numero, e
//      numero fixo e' padrao;
//   2. SUB-GANHO sistematico no comeco da rajada (`s_hzUnderGain` < 1), que
//      fecha para 1.0 conforme a mao "pega" o padrao. Sobra residual de
//      verdade em todo spray — o RCS antigo nunca deixava sobra;
//   3. SERVO LAG: o devido e' ACUMULADO (`s_hzOwedY/X`) e liberado por um
//      filtro de 1a ordem de `RCSLagMs`. Nada se perde, mas a vista SOBE e e'
//      corrigida — e' o dente de serra que faltava;
//   4. ruido CORRELACIONADO (Ornstein-Uhlenbeck, tau ~200 ms) em vez de
//      branco: o desvio tem inercia, entao uma media em janela ainda o ve;
//   5. cadencia irregular por rajada (frame adiado), que ja' existia e agora
//      opera sobre um sinal que tambem varia em quantidade.
//
// COM `RCSHumanize` DESLIGADO o caminho e' o antigo: `cy`/`cx` vao direto
// para o acumulador fracionario, `owed` fica zerado e nenhum sorteio roda.
// ===========================================================================
float s_hzLagMs      = 0.0f;    // latencia sorteada desta rajada (ms)
float s_hzUnderGain  = 1.0f;    // ganho inicial desta rajada (< 1)
float s_hzOUy        = 0.0f;    // ruido correlacionado vertical (counts)
float s_hzOUx        = 0.0f;
float s_hzOwedY      = 0.0f;    // counts devidos, ainda nao liberados
float s_hzOwedX      = 0.0f;
float s_hzGainHz     = 1.0f;    // ganho humano efetivo deste frame (log)
float s_hzNoiseY     = 0.0f;    // ruido vertical aplicado (log)
float s_hzStepY      = 0.0f;    // o que o servo liberou no eixo Y (log)
float s_hzStepX      = 0.0f;    // o que o servo liberou no eixo X (log)
std::chrono::steady_clock::time_point s_hzPrevFrame{};

// ---------------------------------------------------------------------------
// OUUpdate — o passeio do ruido. Implementacao em SDK/RcsNoise.h (mesmo
// arquivo do teste). Wrapper para o call site ficar curto.
// ---------------------------------------------------------------------------
float OUUpdate(float& x, float tau, float amp, float dt, std::mt19937& r) {
    return RcsNoise::OUStep(x, tau, amp, dt, r);
}

const char* CalPhaseMsg(int fase) {
    switch (fase) {
        case CAL_BASE:  return "lendo a referencia da camera";
        case CAL_PULSE: return "injetando o pulso de mouse";
        case CAL_WAIT:  return "esperando a vista acomodar";
        case CAL_READ:  return "medindo o giro";
        case CAL_BACK:  return "devolvendo a mira ao lugar";
        case CAL_DONE:  return "gravando";
        default:        return "";
    }
}

// ---------------------------------------------------------------------------
// RunSensitivityCalibration — maquina de estados da calibracao.
//
// Devolve TRUE enquanto esta' ocupada: o `RecoilControl` tem que sair na hora,
// porque a calibracao esta' movendo a vista DE PROPOSITO e qualquer puxada do
// RCS em cima disso envenena a medida.
//
// Nunca aborta no meio de um pulso: o caminho de falha ainda passa por
// CAL_BACK para desfazer o movimento que ja' foi injetado.
// ---------------------------------------------------------------------------
bool RunSensitivityCalibration() {
    if (g_calibRequested.exchange(false, std::memory_order_relaxed)) {
        s_calPhase = CAL_BASE;
        s_calFrame = 0;
        s_calSent  = 0;
        s_calViaSendInput = false;   // sempre comeca pelo kernel
        g_calibActive.store(true, std::memory_order_relaxed);
        g_calibProgress.store(0.0f, std::memory_order_relaxed);
        LOG("[RCS] calibracao de sensibilidade: iniciando (pulso de %d counts/eixo)\n", kCalPulse);
    }

    if (!g_calibActive.load(std::memory_order_relaxed)) return false;

    // Sem camera nao da' para medir. SEGURA a calibracao ativa e tenta no
    // proximo frame, em vez de abortar no meio de um pulso ja' injetado.
    if (!EngineInstance) return true;

    const MinimalViewInfo pov = EngineInstance->GetCameraCache().POV;

    switch (s_calPhase) {

    case CAL_BASE: {
        s_calBaseP = pov.Rotation.Pitch;
        s_calBaseY = pov.Rotation.Yaw;
        s_calFov   = pov.FOV;
        // ABSOLUTO, nao so' o delta.
        //
        // O delta de BASE->READ pode ser zero por DOIS motivos muito diferentes:
        // (a) a injecao nao moveu a camera, ou (b) a leitura esta' congelada —
        //     `GetCameraCache()` devolve o `CameraEntry`, um CACHE, e existe um
        //     `return` na guarda de FOV que o deixa parado no valor anterior.
        //
        // O ESP nao sofre disso porque ele NAO usa o cache: ele rele' a camera
        // por conta propria (`Camera::GetCurrentView`). E' por isso que o
        // sintoma classico aqui e' "ESP desenha, aimbot morto".
        //
        // Com os dois absolutos no log, da' para separar: se BASE e READ
        // tiverem o MESMO pitch, o cache esta' parado e a injecao nunca foi
        // testada. Se mudarem, a medicao vale e o zero e' real.
        LOG("[RCS] calibracao BASE: pitch=%.4f yaw=%.4f fov=%.1f (via CameraEntry/cache)\n",
            s_calBaseP, s_calBaseY, s_calFov);
        s_calSent  = 0;
        s_calPhase = CAL_PULSE;
        g_calibMsg.store(CAL_PULSE, std::memory_order_relaxed);
        g_calibProgress.store(0.05f, std::memory_order_relaxed);
        break;
    }

    case CAL_PULSE: {
        const int restante = kCalPulse - s_calSent;
        const int env = restante > kCalChunk ? kCalChunk : restante;
        // (dx, dy) positivos = mouse para direita/baixo. O sinal nao importa:
        // CAL_READ usa modulo.
        TargetProcess.driver.InjectMouseMove(env, env, s_calViaSendInput);
        s_calSent += env;
        g_calibProgress.store(0.05f + 0.35f * (float)s_calSent / (float)kCalPulse,
                              std::memory_order_relaxed);
        if (s_calSent >= kCalPulse) {
            s_calPhase = CAL_WAIT;
            s_calFrame = 0;
            g_calibMsg.store(CAL_WAIT, std::memory_order_relaxed);
        }
        break;
    }

    case CAL_WAIT: {
        g_calibProgress.store(0.40f, std::memory_order_relaxed);
        if (++s_calFrame >= kCalWait) {
            s_calPhase = CAL_READ;
            g_calibMsg.store(CAL_READ, std::memory_order_relaxed);
        }
        break;
    }

    case CAL_READ: {
        s_calMeasP = AngleDiff(pov.Rotation.Pitch, s_calBaseP);
        s_calMeasY = AngleDiff(pov.Rotation.Yaw,   s_calBaseY);
        LOG("[RCS] calibracao: pulso=%d counts (%s) | dPitch=%.3f deg  dYaw=%.3f deg | fov=%.1f\n",
            kCalPulse, s_calViaSendInput ? "SendInput" : "kernel",
            s_calMeasP, s_calMeasY, s_calFov);
        // O absoluto do momento da LEITURA — par do "calibracao BASE" acima.
        LOG("[RCS] calibracao READ: pitch=%.4f yaw=%.4f | base era pitch=%.4f yaw=%.4f | "
            "diferenca=%.4f/%.4f\n",
            pov.Rotation.Pitch, pov.Rotation.Yaw, s_calBaseP, s_calBaseY,
            pov.Rotation.Pitch - s_calBaseP, pov.Rotation.Yaw - s_calBaseY);

        // -----------------------------------------------------------------
        // O TESTE COMPARATIVO — a razao de `s_calViaSendInput` existir.
        //
        // Se o pulso pelo KERNEL nao moveu a camera, ainda restam tres causas
        // diferentes e o log nao distingue nenhuma: injecao nao chega no jogo,
        // jogo sem foco, ou calculo errado. Repetir o MESMO pulso pelo
        // `SendInput` separa as tres de uma vez:
        //
        //   kernel 0.000 / SendInput > 0   -> chega no Windows, NAO no que o
        //                                     jogo le' (raw input). E' o nosso
        //                                     caminho de kernel que nao serve
        //                                     para mirar.
        //   kernel 0.000 / SendInput 0.000 -> nao e' injecao: e' estado do jogo
        //                                     (foco, menu aberto, tela de mapa).
        //   kernel > 0                     -> injecao OK, o bug esta' no calculo
        //                                     do RCS, nao na injeccao.
        //
        // Uma tentativa so': se o SendInput tambem der zero, repetir nao muda
        // nada e so' polui o log.
        // -----------------------------------------------------------------
        if (std::fabs(s_calMeasP) < 0.01f && std::fabs(s_calMeasY) < 0.01f && !s_calViaSendInput) {
            LOG("[RCS] kernel nao moveu a camera (0.000 deg). Repetindo o MESMO pulso "
                "pelo SendInput para separar 'injecao nao chega no jogo' de "
                "'jogo sem foco'...\n");
            s_calViaSendInput = true;
            s_calSent  = 0;
            s_calPhase = CAL_BASE;      // refaz o ciclo inteiro pelo outro caminho
            break;
        }

        s_calSent  = 0;
        s_calPhase = CAL_BACK;
        g_calibMsg.store(CAL_BACK, std::memory_order_relaxed);
        break;
    }

    case CAL_BACK: {
        const int restante = kCalPulse - s_calSent;
        const int env = restante > kCalChunk ? kCalChunk : restante;
        // Desfaz pelo MESMO caminho que fez — senao o pulso do kernel ficaria
        // sem contrapartida quando o SendInput e' que anda.
        TargetProcess.driver.InjectMouseMove(-env, -env, s_calViaSendInput);
        s_calSent += env;
        g_calibProgress.store(0.60f + 0.35f * (float)s_calSent / (float)kCalPulse,
                              std::memory_order_relaxed);
        if (s_calSent >= kCalPulse) {
            s_calPhase = CAL_DONE;
            g_calibMsg.store(CAL_DONE, std::memory_order_relaxed);
        }
        break;
    }

    case CAL_DONE: {
        // O teste comparativo terminou (ou nem foi preciso): volta ao caminho
        // normal. Sem isto o proximo pulso continuaria indo por SendInput.
        const bool veioDeSendInput = s_calViaSendInput;
        s_calViaSendInput = false;

        const float gPitch = std::fabs(s_calMeasP);
        const float gYaw   = std::fabs(s_calMeasY);

        float fovCal = s_calFov;
        if (fovCal < 1.0f || fovCal > 170.0f) fovCal = 90.0f;

        // graus-por-count medidos NESTA fov, normalizados para a fov 90 — que
        // e' a referencia que o `RecoilControl` assume.
        const float medido   = gPitch / (float)kCalPulse;
        const float naFov90  = medido * (90.0f / fovCal);

        if (gPitch >= 0.05f && naFov90 > 0.0005f && naFov90 < 0.5f) {
            Configs.Aimbot.RCSDegPerCount = naFov90;
            LOG("[+] RCS CALIBRADO: %.4f graus/count na FOV 90 "
                "(medido %.4f na FOV %.1f) = %.1f counts por grau\n"
                "    eixo yaw mediu %.4f graus/count — se a compensacao horizontal\n"
                "    ficar torta, ajuste em 'Horizontal (yaw)'.\n",
                naFov90, medido, fovCal,
                (naFov90 > 0.0f ? 1.0f / naFov90 : 0.0f),
                gYaw / (float)kCalPulse);
        } else {
            // -----------------------------------------------------------------
            // A CALIBRACAO FOI REJEITADA — e agora o log diz QUAL das causas.
            //
            // `veioDeSendInput` conta o que aconteceu no teste comparativo:
            // o kernel deu zero, entao o pulso foi repetido por SendInput, e
            // este e' o resultado DESSE segundo pulso.
            //   SendInput tambem 0 -> nao e' a injecao; o jogo nao estava'
            //                         recebendo mouse (foco, menu, mapa).
            //   SendInput > 0      -> o kernel nao entrega no que o jogo le'.
            // -----------------------------------------------------------------
            if (veioDeSendInput) {
                LOG("[-] RCS calibracao REJEITADA: medicao implausivel "
                    "(%.4f graus/count na FOV 90, dPitch=%.3f).\n"
                    "    O teste comparativo diz: o KERNEL deu 0.000 e o SendInput "
                    "TAMBEM deu ~0.000.\n"
                    "    Conclusao: NAO e' a injecao — o jogo nao estava' recebendo "
                    "mouse (sem foco, menu/mapa aberto, ou mouse capturado).\n"
                    "    O valor antigo (%.4f) foi mantido.\n",
                    naFov90, gPitch, Configs.Aimbot.RCSDegPerCount);
            } else {
                LOG("[-] RCS calibracao REJEITADA: medicao implausivel "
                    "(%.4f graus/count na FOV 90, dPitch=%.3f).\n"
                    "    causas provaveis: injecao de mouse bloqueada, jogo sem foco,\n"
                    "    munição/acumulador parado, ou sensibilidade in-game muito baixa.\n"
                    "    O valor antigo (%.4f) foi mantido.\n",
                    naFov90, s_calMeasP, Configs.Aimbot.RCSDegPerCount);
            }
        }

        g_calibProgress.store(1.0f, std::memory_order_relaxed);
        g_calibActive.store(false, std::memory_order_relaxed);
        g_calibMsg.store(0, std::memory_order_relaxed);
        s_calPhase = CAL_IDLE;
        s_calSent  = 0;
        Aimbot::ResetRcs();
        return false;
    }

    default:
        s_calPhase = CAL_IDLE;
        g_calibActive.store(false, std::memory_order_relaxed);
        return false;
    }

    return true;   // ocupada
}

} // namespace

void Aimbot::RequestSensitivityCalibration() {
    g_calibRequested.store(true, std::memory_order_relaxed);
}

bool Aimbot::IsCalibrating() {
    return g_calibActive.load(std::memory_order_relaxed);
}

Aimbot::RcsReadout Aimbot::GetRcsReadout() {
    RcsReadout r;
    r.countsPerDeg  = g_rcsCountsPerDeg.load(std::memory_order_relaxed);
    r.dropY         = g_rcsDropY.load(std::memory_order_relaxed);
    r.dropR         = g_rcsDropR.load(std::memory_order_relaxed);
    r.degY          = g_rcsDegY.load(std::memory_order_relaxed);
    r.residual      = g_rcsResidual.load(std::memory_order_relaxed);
    r.cy            = g_rcsCy.load(std::memory_order_relaxed);
    r.cx            = g_rcsCx.load(std::memory_order_relaxed);
    r.gainMul       = g_rcsGainMul.load(std::memory_order_relaxed);
    r.lagMs         = g_rcsLagMs.load(std::memory_order_relaxed);
    r.gainHz        = g_rcsGainHz.load(std::memory_order_relaxed);
    r.owedY         = g_rcsOwedY.load(std::memory_order_relaxed);
    r.stepY         = g_rcsStepY.load(std::memory_order_relaxed);
    r.noiseY        = g_rcsNoiseY.load(std::memory_order_relaxed);
    r.firing        = g_rcsFiring.load(std::memory_order_relaxed);
    r.calibrating   = g_calibActive.load(std::memory_order_relaxed);
    r.calibProgress = g_calibProgress.load(std::memory_order_relaxed);
    r.calibMsg      = CalPhaseMsg(g_calibMsg.load(std::memory_order_relaxed));
    return r;
}

void Aimbot::RecoilControl() {
    // A calibracao tem PRIORIDADE: enquanto ela roda, o RCS fica parado. Ela
    // esta' movendo a vista de proposito, e qualquer puxada em cima disso
    // envenena a medida.
    if (RunSensitivityCalibration()) return;

    if (!Configs.Aimbot.RCS) return;
    if (!EngineInstance) return;

    // O botao esquerdo e' lido AQUI (subiu de lugar): o caminho de "soltar o
    // gatilho" la' embaixo precisa saber se a mao ainda esta' apertando, e o
    // estado da borda (`s_hzAtirandoAntes`) tem que ser mantido nos DOIS
    // caminhos. Se so' o caminho principal atualizasse, sair por ali deixaria
    // `s_hzAtirandoAntes = true` preso e a rajada SEGUINTE nao detectaria a
    // borda de subida — ela comecaria sem latencia e sem sub-ganho, que e'
    // exatamente o comportamento que a humanizacao existe para remover.
    const bool atirando = HotkeyHook::IsDown(HotkeyHook::KEY_LBUTTON);
    g_rcsFiring.store(atirando, std::memory_order_relaxed);

    const uint64_t pawn = EngineInstance->AcknowledgedPawn;
    if (pawn < 0x1000000) return;

    // -----------------------------------------------------------------
    // ARMA EQUIPADA: WeaponProcessor -> EquippedWeapons[idx]
    // -----------------------------------------------------------------
    uint64_t weapon = 0;
    {
        uint64_t wp = 0;
        if (!TargetProcess.Read(pawn + SDK.WeaponProcessor, &wp, sizeof(wp), "WP") ||
            !IsCanonicalPtr(wp)) {
            s_rcsValid = false; return;
        }

        uint8_t idx = 0;
        uint64_t ew = 0;
        TargetProcess.Read(wp + SDK.CurrentWeaponIndex, &idx, sizeof(idx), "WIdx");
        TargetProcess.Read(wp + SDK.EquippedWeapons, &ew, sizeof(ew), "EW");
        if (!IsCanonicalPtr(ew) || idx > 4) { s_rcsValid = false; return; }

        TargetProcess.Read(ew + (uint64_t)idx * 8, &weapon, sizeof(weapon), "Weapon");
        if (!IsCanonicalPtr(weapon)) { s_rcsValid = false; return; }
    }

    // -----------------------------------------------------------------
    // OS DOIS ACUMULADORES  (o sensor direto do recuo)
    // -----------------------------------------------------------------
    Vector2 rvv = Vector2(0.f, 0.f);
    float   vr  = 0.0f;

    if (!TargetProcess.Read(weapon + SDK.RecoilValueVector, &rvv, sizeof(rvv), "RVV")) {
        s_rcsValid = false; return;
    }
    TargetProcess.Read(weapon + SDK.VerticalRecovery, &vr, sizeof(vr), "VR");

    // sanidade: o post avisa que read ruim tem que ser descartado
    if (std::abs(rvv.x) > 100.0f || std::abs(rvv.y) > 100.0f || std::abs(vr) > 100.0f) {
        s_rcsValid = false; return;
    }

    // nao atirando / totalmente recuperado -> zera tudo
    if (rvv.y == 0.0f && vr == 0.0f) {
        s_rcsValid = false;
        s_ffRvvY = 0.0f; s_ffVr = 0.0f; s_ffRvvX = 0.0f;

        // -----------------------------------------------------------------
        // SOLTAR O GATILHO — drenar o que o servo ainda devia.
        //
        // O servo lag guarda counts ENTRE frames de proposito. Se a rajada
        // acabar com saldo pendente, sair daqui zerado deixaria uma puxada
        // orfa esperando a proxima rajada — a vista ficaria torta e o proximo
        // spray comecaria ja' compensado. Drena com o MESMO filtro e desiste
        // quando o resto for sub-count.
        //
        // E' o comportamento certo do ponto de vista de assinatura tambem: um
        // jogador que solta o gatilho AINDA tem a mao na mira por um instante.
        // -----------------------------------------------------------------
        if (Configs.Aimbot.RCSHumanize && Configs.Aimbot.RCSLagMs > 0) {
            const float lagS  = (float)Configs.Aimbot.RCSLagMs * 1e-3f;
            const float dtNom = 1.0f / 144.0f;               // cadencia nominal
            const float mc    = (float)Configs.Aimbot.RCSMaxCounts;

            // O MESMO servo, sem duplicar a formula.
            const float dy = RcsServo::Step(s_hzOwedY, dtNom, lagS, mc);
            const float dx = RcsServo::Step(s_hzOwedX, dtNom, lagS, mc);

            // Mao soltando o gatilho nao injeta tremor.
            s_hzOUy = 0.0f; s_hzOUx = 0.0f;

            int sy = (int)dy;
            int sx = (int)dx;
            // O eixo vertical respeita o mesmo toggle do caminho normal.
            if (Configs.Aimbot.AimbotUnlockVertical) { sy = 0; s_hzOwedY = 0.0f; }
            if (sy != 0 || sx != 0) TargetProcess.driver.InjectMouseMove(sx, sy);

            // FECHA A RAJADA. O estado da borda vira `false` aqui, e NAO
            // `atirando`: e' o SENSOR que define rajada, nao o botao. Se
            // atribuisse `atirando` e o jogador ja' estivesse com o gatilho
            // apertado, a rajada seguinte nunca veria a borda de subida e
            // comecaria sem latencia e sem sub-ganho — exatamente o
            // comportamento que a humanizacao existe para remover.
            s_hzAtirandoAntes = false;
            if (!atirando && std::fabs(s_hzOwedY) < 0.5f && std::fabs(s_hzOwedX) < 0.5f) {
                s_hzOwedY = 0.0f; s_hzOwedX = 0.0f;
            }
            return;
        }

        s_hzOwedY = 0.0f; s_hzOwedX = 0.0f;
        s_hzOUy   = 0.0f; s_hzOUx   = 0.0f;
        s_hzAtirandoAntes = false;
        return;
    }

    if (!s_rcsValid) {
        s_ffRvvY = rvv.y; s_ffVr = vr; s_ffRvvX = rvv.x;
        s_rcsValid = true;
        return;                       // ancora e pula 1 frame
    }

    // -----------------------------------------------------------------
    // O DRENO: o que sumiu do acumulador = o que a camera acabou de levar
    //
    // SO a diminuicao conta. Um AUMENTO significa tiro novo que ainda nao
    // chegou na camera — nao e' movimento dela.
    // -----------------------------------------------------------------
    float dropY = (s_ffRvvY - rvv.y > 0.0f) ? (s_ffRvvY - rvv.y) : 0.0f;
    float dropR = (s_ffVr   - vr   > 0.0f) ? (s_ffVr   - vr  ) : 0.0f;
    float dropX = (std::abs(rvv.x) < std::abs(s_ffRvvX)) ? (s_ffRvvX - rvv.x) : 0.0f;

    s_ffRvvY = rvv.y; s_ffVr = vr; s_ffRvvX = rvv.x;

    // -----------------------------------------------------------------
    // GRAUS -> COUNTS  (o unico ganho que sobrou)
    //
    //     graus  = kInputPitchScale * (coice - recuperacao)
    //     counts = graus / degPerCount
    //
    // `degPerCount` varia com a FOV de proposito: mirar reduz a FOV, e com a
    // FOV menor o mesmo movimento de mouse gira MAIS graus. Sem essa correcao a
    // compensacao fica fraca justamente mirando — que e' quando ela importa.
    // -----------------------------------------------------------------
    const float degY = kInputPitchScale * (dropY - dropR);
    const float degX = kInputPitchScale * dropX;

    float fov = EngineInstance->GetCameraCache().POV.FOV;

    // -----------------------------------------------------------------
    // FAIXA DE FOV  (este clamp era o bug do 4x)
    //
    // Antes era ov < 20 -> 90. No 4x a FOV do PUBG cai pra ~10-15 graus,
    // entao o clamp JOGAVA FORA o valor certo e forcava 90 — o que deixava
    // degPerCount 4x maior e a compensacao 4x menor. Resultado: no 4x a mira
    // subia inteira, enquanto red dot / 2x / 3x (acima de 20) funcionavam.
    //
    // Agora so' rejeita o que e' claramente lixo de leitura.
    // -----------------------------------------------------------------
    if (fov < 1.0f || fov > 170.0f) fov = 90.0f;

    float degPerCount = Configs.Aimbot.RCSDegPerCount * (fov / 90.0f);
    if (degPerCount < 0.001f) degPerCount = 0.022f;   // config zerada ou lixo

    // =====================================================================
    // AUTO-CALIBRACAO — mede a sobra e corrige o ganho sozinho.
    //
    // O sensor ja' entrega o que o jogo empurrou ("graus"). Se a compensacao
    // estiver CERTA, a camera nao sobe. Curta -> ela sobe. Longa -> ela desce.
    // Entao o sinal de controle e' a PROPRIA DERIVA DA CAMERA, e o alvo e' 0.
    //
    // O que isto NAO consegue separar: o mouse do proprio jogador. Durante a
    // rajada, se ele puxar a mira na mao, o controlador le como
    // sobre-compensacao e reduz o ganho. Por isso:
    //
    //   - so' acumula com o BOTAO ESQUERDO pressionado E o acumulador ainda
    //     cheio (`rvv.y > 0`). Isso exclui a fase de RECUPERACAO, em que a
    //     camera desce sozinha — contar aquilo derrubaria o ganho sem motivo;
    //   - passo pequeno (4% por janela de 500 ms) e deriva baixada em deadzone;
    //   - limitado a [0.20x, 5.00x];
    //   - a opcao vem DESLIGADA por padrao.
    //
    // `s_autoMul` e' da SESSAO, nao vai para o config: e' correcao de
    // arma/acessorio daquele momento, e deve reaprender a cada partida.
    // =====================================================================
    // =====================================================================
    // `atirando` e `g_rcsFiring` foram lidos no TOPO do RecoilControl (o
    // caminho de soltar o gatilho precisa deles antes daqui). Nao redeclarar.
    // =====================================================================

    float mul = 1.0f;
    if (Configs.Aimbot.RCSAutoCalibrate) {
        const float camPitch = EngineInstance->GetCameraCache().POV.Rotation.Pitch;
        if (s_autoHadPitch) {
            const float d = AngleDiff(camPitch, s_autoPrevPitch);
            // Deriva absurda = teleporte de camera (troca de mapa, espectador,
            // read ruim), nao recuo. Descarta em vez de somar.
            if (atirando && rvv.y > 0.05f && std::fabs(d) < 5.0f) {
                s_autoAccum += d;
            }
        }
        s_autoPrevPitch = camPitch;
        s_autoHadPitch  = true;

        const auto agora = std::chrono::steady_clock::now();
        if (s_autoWindowStart.time_since_epoch().count() == 0) s_autoWindowStart = agora;

        const int64_t decorrido = std::chrono::duration_cast<std::chrono::milliseconds>(
            agora - s_autoWindowStart).count();

        if (decorrido >= kAutoWindowMs) {
            s_autoWindowStart = agora;
            if (atirando) {
                if (s_autoAccum > kAutoDeadzone) {
                    s_autoMul *= (1.0f + kAutoStep);   // mira SOBE -> puxa mais
                } else if (s_autoAccum < -kAutoDeadzone) {
                    s_autoMul *= (1.0f - kAutoStep);   // mira DESCE -> puxa menos
                }
                if (s_autoMul < kAutoMinMul) s_autoMul = kAutoMinMul;
                if (s_autoMul > kAutoMaxMul) s_autoMul = kAutoMaxMul;
            }
            g_rcsResidual.store(s_autoAccum, std::memory_order_relaxed);
            s_autoAccum = 0.0f;
        }
        mul = s_autoMul;
    } else {
        // Desligada: zera a adaptacao inteira para nao guardar estado velho de
        // uma sessao em que ela esteve ligada.
        s_autoMul = 1.0f;
        s_autoAccum = 0.0f;
        s_autoHadPitch = false;
        g_rcsResidual.store(0.0f, std::memory_order_relaxed);
    }

    const float sgn = Configs.Aimbot.RCSInvert ? -1.0f : 1.0f;

    float cy =  sgn * (degY / degPerCount) * mul * (Configs.Aimbot.AR_RecoilValue / 100.0f);
    float cx = -sgn * (degX / degPerCount) * mul * (Configs.Aimbot.AR_RecoilYaw   / 100.0f);

    // =====================================================================
    // HUMANIZACAO v2 — MODELO DE MAO
    //
    // A explicacao longa esta' no topo do arquivo, junto das variaveis
    // (`s_hzOwedY`, `OUUpdate`). Aqui fica so' a conta.
    //
    // ORDEM: o teorico (cy/cx) vira "o que uma mao deveria puxar agora" e
    // passa por quatro estagios antes de virar count inteiro:
    //
    //   teorico -> x ganho humano -> + divida (servo) -> libera fracao
    //           -> + ruido correlacionado -> x cadencia -> acumulador
    //
    // A garantia que NAO pode quebrar: `owed` preserva o total. O servo so'
    // muda QUANDO o count sai, nunca QUANTO sai ao longo da rajada — fora o
    // sub-ganho, que e' sub-compensacao DELIBERADA e fecha sozinha.
    // =====================================================================
    const float cap = (float)Configs.Aimbot.RCSMaxCounts;

    float hzRampa  = 1.0f;
    float hzJit    = 1.0f;
    bool  hzAdiar  = false;
    float hzForca  = 0.0f;

    if (Configs.Aimbot.RCSHumanize) {
        hzForca = (float)Configs.Aimbot.RCSHumanizeStrength / 100.0f;
        if (hzForca < 0.0f) hzForca = 0.0f;
        if (hzForca > 1.0f) hzForca = 1.0f;

        const auto hzAgora = std::chrono::steady_clock::now();

        // ---- dt REAL deste frame (medido, nao suposto) ----
        // Sem isto o servo roda com um dt fantasia: a 60 Hz ele liberaria a
        // mesma fracao por frame que a 144 Hz, ou seja 2.4x mais devagar em
        // tempo de parede. O lag tem que ser em MILISSEGUNDOS, nao em frames.
        float dt = 1.0f / 144.0f;
        if (s_hzPrevFrame.time_since_epoch().count() != 0) {
            dt = (float)std::chrono::duration_cast<std::chrono::microseconds>(
                     hzAgora - s_hzPrevFrame).count() * 1e-6f;
        }
        s_hzPrevFrame = hzAgora;
        if (!(dt > 0.001f)) dt = 0.001f;
        if (dt > 0.050f)    dt = 0.050f;   // hitch nao vira flick

        // ---- BORDA DE SUBIDA: cada rajada e' uma mao NOVA ----
        if (atirando && !s_hzAtirandoAntes) {
            s_hzBurstIni = hzAgora;

            // divida, ruido e servo comecam do zero: nada atravessa rajada
            s_hzOwedY = 0.0f; s_hzOwedX = 0.0f;
            s_hzOUy   = 0.0f; s_hzOUx   = 0.0f;

            // LATENCIA sorteada: 30%..70% da janela. Duas rajadas nunca tem a
            // mesma reacao — 90 ms fixo e' um numero, e numero fixo e' padrao.
            const float jan = (float)Configs.Aimbot.RCSReactionMs;
            s_hzLagMs = jan * (0.30f + 0.40f * RandUnit(rng));

            // SUB-GANHO inicial: a mao nao acha o ponto de cara. Teto de 12%
            // de recuo NAO compensado no comeco, escalado pela forca.
            const float subMax = 0.12f * hzForca;
            s_hzUnderGain = 1.0f - subMax * (0.55f + 0.45f * RandUnit(rng));
        }
        s_hzAtirandoAntes = atirando;

        // ---- 1) LATENCIA + RECUPERACAO ----
        //
        // `hzRampa` = quanto da compensacao ja' esta' disponivel. Zero durante
        // a latencia (a mao aceita o coice), depois sobe por `exp` ate' 1.
        // A janela e' o que SOBRA de `RCSReactionMs` depois do sorteio, entao
        // o tempo total ate' 100% continua sendo o slider — o que muda entre
        // rajadas e' a divisao entre "parado" e "subindo".
        float hzGain = 1.0f;
        float tMsBurst = 0.0f;
        if (atirando) {
            tMsBurst = (float)std::chrono::duration_cast<std::chrono::microseconds>(
                           hzAgora - s_hzBurstIni).count() * 1e-3f;

            if (s_hzLagMs > 0.5f) {
                const float passou = tMsBurst - s_hzLagMs;
                if (passou <= 0.0f) {
                    hzRampa = 0.0f;      // ainda nao reagiu: aceita o coice
                } else {
                    float resto = (float)Configs.Aimbot.RCSReactionMs - s_hzLagMs;
                    if (resto < 15.0f) resto = 15.0f;
                    hzRampa = 1.0f - std::exp(-passou / (resto / 3.0f));
                }
            }

            // ---- 2) SUB-GANHO que fecha conforme a mao aprende o padrao ----
            // tau ~420 ms: no fim de uma rajada longa o ganho ja' e' ~1. O que
            // fica de fora e' o COMECO — e o comeco e' o que a analise olha.
            const float aprendeu = 1.0f - std::exp(-tMsBurst / 420.0f);
            hzGain = s_hzUnderGain + (1.0f - s_hzUnderGain) * aprendeu;
            hzGain *= hzRampa;
        }

        // ---- 3) SERVO LAG ----
        //
        // O teorico vira DIVIDA; o filtro de 1a ordem libera uma fracao por
        // frame: `alpha = dt / (lag + dt)`. Com `RCSLagMs = 0`, `alpha = 1` e
        // sai tudo no frame — comportamento antigo.
        s_hzOwedY += cy * hzGain;
        s_hzOwedX += cx * hzGain;

        // Trava da divida. Sem isto, um hitch de 50 ms com `cy` no cap vira um
        // pacote de varios caps de uma vez. `4 * cap` e' generoso e ainda
        // impede o flick. O que a trava corta e' a UNICA coisa que o servo
        // pode perder — e e' por seguranca, nao por estetica.
        {
            const float owedMax = cap * 4.0f;
            if (s_hzOwedY >  owedMax) s_hzOwedY =  owedMax;
            if (s_hzOwedY < -owedMax) s_hzOwedY = -owedMax;
            if (s_hzOwedX >  owedMax) s_hzOwedX =  owedMax;
            if (s_hzOwedX < -owedMax) s_hzOwedX = -owedMax;
        }

        const float lagS = (float)Configs.Aimbot.RCSLagMs * 1e-3f;
        // O cap e' aplicado DENTRO do servo, antes de debitar a divida: o que
        // a trava corta continua devendo e sai no frame seguinte. Cortar
        // depois de debitar perderia counts em silencio.
        s_hzStepY = RcsServo::Step(s_hzOwedY, dt, lagS, cap);
        s_hzStepX = RcsServo::Step(s_hzOwedX, dt, lagS, cap);

        // ---- 4) RUIDO CORRELACIONADO (passeio, nao chiado) ----
        //
        // tau 200 ms no pitch, 170 no yaw: o desvio tem inercia, entao uma
        // media em janela ainda o ve. Amplitude em counts, escalada pela
        // forca — forca 0 desliga mesmo com `RCSNoiseCounts` alto.
        float nY = 0.0f, nX = 0.0f;
        if (atirando && hzForca > 0.0f && Configs.Aimbot.RCSNoiseCounts > 0.0f) {
            const float ampY = Configs.Aimbot.RCSNoiseCounts * hzForca;
            const float ampX = Configs.Aimbot.RCSNoiseCounts * 0.65f * hzForca;
            nY = OUUpdate(s_hzOUy, 0.200f, ampY, dt, rng);
            nX = OUUpdate(s_hzOUx, 0.170f, ampX, dt, rng);
        } else {
            s_hzOUy = 0.0f; s_hzOUx = 0.0f;
        }
        s_hzNoiseY = nY;
        s_hzGainHz = hzGain;

        // ---- 5) CADENCIA IRREGULAR ----
        // O jitter virou FINO (±6%, era ±18%): o trabalho de esconder o padrao
        // agora esta' no servo e no passeio, que sobrevivem a media. Um ±18%
        // aqui so' estragava a compensacao sem esconder nada.
        if (atirando && hzForca > 0.0f) {
            hzJit   = 1.0f + RandSym(rng) * (0.06f * hzForca);
            hzAdiar = (RandUnit(rng) < 0.10f * hzForca);
        }

        // O que entra no acumulador fracionario.
        cy = (s_hzStepY + nY) * hzJit;
        cx = (s_hzStepX + nX) * hzJit;
    } else {
        // Desligado: caminho antigo e limpeza de TODO o estado, para religar
        // sem herdar divida pendente nem um `s_hzBurstIni` de minutos atras.
        s_hzAtirandoAntes = atirando;
        s_hzOwedY = 0.0f; s_hzOwedX = 0.0f;
        s_hzOUy   = 0.0f; s_hzOUx   = 0.0f;
        s_hzStepY = 0.0f;
        s_hzStepX = 0.0f;
        s_hzGainHz = 1.0f;
        s_hzNoiseY = 0.0f;
        hzRampa = 1.0f;
        hzJit   = 1.0f;
    }

    s_hzRampa = hzRampa;
    s_hzJit   = hzJit;
    s_hzAdiou = hzAdiar;

    if (cy >  cap) cy =  cap;
    if (cy < -cap) cy = -cap;
    if (cx >  cap) cx =  cap;
    if (cx < -cap) cx = -cap;

    g_rcsGainMul.store(mul, std::memory_order_relaxed);
    g_rcsCountsPerDeg.store(degPerCount > 0.0f ? (1.0f / degPerCount) * mul : 0.0f,
                            std::memory_order_relaxed);
    g_rcsDropY.store(dropY, std::memory_order_relaxed);
    g_rcsDropR.store(dropR, std::memory_order_relaxed);
    g_rcsDegY.store(degY, std::memory_order_relaxed);

    // -----------------------------------------------------------------
    // ACUMULADOR FRACIONARIO  (o post: sem isto perde 20-40% em sens baixa)
    // -----------------------------------------------------------------
    static float accX = 0.0f, accY = 0.0f;

    // ---------------------------------------------------------------------
    // DESTRAVAR O EIXO VERTICAL — este e' o SEGUNDO ponto que move a vista.
    //
    // O RCS e' quem mais puxa o pitch (ele existe justamente para compensar o
    // recuo que sobe a mira). Se a opcao valesse so' no MoveMouseTowards, o
    // recuo voltaria a subir a mira por aqui e o usuario acharia que o toggle
    // nao funciona — por isso o gate e' aplicado AQUI TAMBEM.
    //
    // `cy` e' a puxada vertical (ja' em contagens de mouse -> dy da injecao) e
    // `accY` e' o acumulador fracionario dela. Os dois zeram juntos: zerar so'
    // `cy` deixaria a fracao guardada para o proximo frame, que da' no mesmo
    // que nao ter zerado.
    //
    // O sensor continua LENDO e atualizando `s_ffRvvY` normalmente (nao
    // congela nada), entao desligar o toggle retoma a compensacao sem pico.
    // O eixo horizontal (`cx` / `accX`) segue intocado.
    // ---------------------------------------------------------------------
    if (Configs.Aimbot.AimbotUnlockVertical) {
        cy   = 0.0f;
        accY = 0.0f;
        // A DIVIDA do servo tambem zera. Sem isto a divida vertical ficaria
        // crescendo a cada frame (o teorico continua entrando em `owedY`) e a
        // trava de `4 * cap` comecaria a cortar. Zerar aqui deixa o eixo
        // vertical literalmente com a mao do jogador, que e' o que o toggle
        // promete.
        s_hzOwedY = 0.0f;
    }

    accY += cy;
    accX += cx;

    // -----------------------------------------------------------------
    // TREMOR BRANCO — REMOVIDO na v2.
    //
    // Aqui somava `+-0.35 count` de ruido BRANCO no acumulador. Ele fazia
    // companhia ao jitter branco e tinha o mesmo defeito: media zero, novo a
    // cada frame, some quando alguem analisa em janela. O papel dele —
    // "impedir que a sequencia de inteiros injetados seja periodica" — agora
    // e' feito pelo ruido CORRELACIONADO do estagio 4, que sobrevive a media
    // e ao mesmo tempo nao precisa ser cortado para nao virar pacote.
    // ---------------------------------------------------------------------

    int iy = 0, ix = 0;

    // -----------------------------------------------------------------
    // DITHERING — o envio adiado.
    //
    // `hzAdiar` foi decidido acima (junto do jitter). Adiar = NAO extrair a
    // parte inteira neste frame: o acumulador guarda tudo e o pacote sai no
    // proximo, maior. E' o oposto de "perder movimento" — o total compensado
    // continua exato, o que muda e' o instante.
    //
    // O teto e' necessario: dois frames adiados seguidos com `cy` no cap
    // somariam 2x o cap num pacote unico. `2 * cap` e' o maximo plausivel
    // (um frame de atraso), e o driver ainda subdivide em pacotes de 3-9 px.
    // -----------------------------------------------------------------
    if (hzAdiar) {
        const float teto = (float)Configs.Aimbot.RCSMaxCounts * 2.0f;
        if (accY >  teto) accY =  teto;
        if (accY < -teto) accY = -teto;
        if (accX >  teto) accX =  teto;
        if (accX < -teto) accX = -teto;
    } else {
        iy = (int)accY; accY -= (float)iy;
        ix = (int)accX; accX -= (float)ix;
    }

    // Publica o que SAIU de fato (os inteiros, depois do acumulador
    // fracionario) — e' o numero que o menu mostra, nao o `cy`/`cx` teoricos.
    g_rcsCy.store(iy, std::memory_order_relaxed);
    g_rcsCx.store(ix, std::memory_order_relaxed);

    // ---- humanizacao v2: o que o painel "RCS ao vivo" mostra ----
    // `owedY` e' o numero que denuncia o servo funcionando: durante a rajada
    // ele fica ALTO e serpenteia; se ficar cravado em 0 com `RCSLagMs` > 0, o
    // servo nao esta' sendo alimentado (sensor morto ou toggle errado).
    g_rcsLagMs .store(Configs.Aimbot.RCSHumanize ? s_hzLagMs  : 0.0f, std::memory_order_relaxed);
    g_rcsGainHz.store(Configs.Aimbot.RCSHumanize ? s_hzGainHz : 1.0f, std::memory_order_relaxed);
    g_rcsOwedY .store(Configs.Aimbot.RCSHumanize ? s_hzOwedY  : 0.0f, std::memory_order_relaxed);
    g_rcsStepY .store(Configs.Aimbot.RCSHumanize ? s_hzStepY  : 0.0f, std::memory_order_relaxed);
    g_rcsNoiseY.store(Configs.Aimbot.RCSHumanize ? s_hzNoiseY : 0.0f, std::memory_order_relaxed);

    {
        static auto lastLog = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        // 1x/s so' com `Debug logs` ligado (aba DEBUG). Antes rodava SEMPRE:
        // eram 2 linhas por segundo ([RCS] + [CAM]) em toda sessao, ~7 mil
        // linhas por hora de jogo. Erro/aviso continua saindo sem a flag.
        if (Configs.Survivor.DebugLogs &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLog).count() > 1000) {
            lastLog = now;
            LOG("[RCS] weapon=0x%llX rvv=(%.3f,%.3f) vr=%.3f | dropY=%.4f dropR=%.4f dropX=%.4f | "
                "degY=%.3f degX=%.3f | fov=%.1f dpc=%.4f (%.1f counts/grau) mul=%.2f -> (%d,%d) | "
                "hz: rampa=%.2f jit=%.2f adiou=%d | "
                "mao: lat=%.0fms ganho=%.3f passo=%.2f devendo=%.2f ruido=%.2f | "
                "inj: k=%llu f=%llu err=%u\n",
                (unsigned long long)weapon, rvv.x, rvv.y, vr,
                dropY, dropR, dropX, degY, degX, fov, degPerCount,
                (degPerCount > 0.0f ? 1.0f / degPerCount : 0.0f), mul, ix, iy,
                s_hzRampa, s_hzJit, (int)s_hzAdiou,
                s_hzLagMs, s_hzGainHz, s_hzStepY, s_hzOwedY, s_hzNoiseY,
                (unsigned long long)g_mouseInjOk.load(std::memory_order_relaxed),
                (unsigned long long)g_mouseInjFail.load(std::memory_order_relaxed),
                (unsigned)g_mouseInjLastErr.load(std::memory_order_relaxed));

            // -------------------------------------------------------------
            // CAMERA: CACHE x FRESCA, NA MESMA LINHA.
            //
            // O aimbot mira com `Engine::CameraEntry` (via GSharedData) — um
            // cache escrito por `RefreshViewMatrix` na thread de memoria.
            // O ESP NAO usa ele: rele' a camera por conta propria com
            // `Camera::GetCurrentView`, na thread de render.
            //
            // Um cache parado explica sozinho "ESP desenha, aimbot morto": o
            // aimbot projeta contra uma camera que nao anda e acha que esta'
            // tudo certo, porque o FOV dela continua plausivel (os diagnosticos
            // existentes so' olham o FOV — e 80.0 passa em qualquer guarda).
            //
            // As duas leem OS MESMOS offsets, entao:
            //   diferentes -> e' cache velho (RefreshViewMatrix/frequencia)
            //   iguais E parados com voce jogando -> nao e' cache: e' o
            //                PlayerCameraManager/offset, ou o jogo nao publica
            //   mudando     -> esta' tudo vivo, e o problema do aimbot e' outro
            //
            // 1x/s, junto do log do RCS que ja' existe — sem spam novo.
            // -------------------------------------------------------------
            if (EngineInstance) {
                const MinimalViewInfo fresca = Camera::GetCurrentView(EngineInstance->PlayerCameraManager);
                LOG("[CAM] cache=(%.4f,%.4f) fov=%.1f | fresca=(%.4f,%.4f) fov=%.1f | %s\n",
                    GSharedData.CameraCache.POV.Rotation.Pitch,
                    GSharedData.CameraCache.POV.Rotation.Yaw,
                    GSharedData.CameraCache.POV.FOV,
                    fresca.Rotation.Pitch, fresca.Rotation.Yaw, fresca.FOV,
                    (std::fabs(fresca.Rotation.Pitch - GSharedData.CameraCache.POV.Rotation.Pitch) > 0.01f ||
                     std::fabs(fresca.Rotation.Yaw   - GSharedData.CameraCache.POV.Rotation.Yaw)   > 0.01f)
                        ? "DIFEREM -> cache atrasado em relacao a leitura viva"
                        : "iguais");
            }
        }
    }

    if (ix == 0 && iy == 0) return;
    TargetProcess.driver.InjectMouseMove(ix, iy);
}

// forca o RCS a reancorar no proximo frame (troca de arma, read ruim, etc)
void Aimbot::ResetRcs() {
    s_rcsValid = false;
    s_ffRvvY = 0.0f;
    s_ffVr   = 0.0f;
    s_ffRvvX = 0.0f;

    // A auto-calibracao tambem reancora: o multiplicador aprendido era daquela
    // arma/acessorio. Manter depois da troca aplicaria a correcao errada.
    s_autoMul = 1.0f;
    s_autoAccum = 0.0f;
    s_autoHadPitch = false;
    g_rcsGainMul.store(1.0f, std::memory_order_relaxed);
    g_rcsResidual.store(0.0f, std::memory_order_relaxed);

    // A humanizacao tambem reancora: manter `s_hzBurstIni` de uma rajada
    // anterior faria a proxima comecar com rampa = 1 (o oposto do que a
    // reacao existe para fazer).
    s_hzAtirandoAntes = false;
    s_hzRampa = 1.0f;
    s_hzJit   = 1.0f;
    s_hzAdiou = false;

    // ---- v2: servo e passeio ----
    // A DIVIDA tem que zerar aqui. `ResetRcs` e' chamado na troca de arma e na
    // calibracao de sensibilidade; carregar counts devidos de uma arma para a
    // outra injetaria uma puxada fantasma na primeira rajada da arma nova.
    s_hzOwedY   = 0.0f;
    s_hzOwedX   = 0.0f;
    s_hzOUy     = 0.0f;
    s_hzOUx     = 0.0f;
    s_hzStepY   = 0.0f;
    s_hzStepX   = 0.0f;
    s_hzGainHz  = 1.0f;
    s_hzNoiseY  = 0.0f;
    s_hzLagMs   = 0.0f;
    s_hzPrevFrame = std::chrono::steady_clock::time_point{};
    g_rcsOwedY.store(0.0f, std::memory_order_relaxed);
    g_rcsStepY.store(0.0f, std::memory_order_relaxed);
    g_rcsNoiseY.store(0.0f, std::memory_order_relaxed);
}
