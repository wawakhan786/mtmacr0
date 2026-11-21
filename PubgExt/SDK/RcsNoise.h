#pragma once
// ===========================================================================
// RUIDO DO RCS — geradores e o processo de Ornstein-Uhlenbeck
//
// POR QUE ISTO E' UM HEADER SEPARADO. O resto da humanizacao do RCS e' um
// punhado de somas e multiplicacoes que da' para conferir lendo; a parte
// NUMERICA — a discretizacao do OU — e' onde um erro fica invisivel: o codigo
// roda, nao estoura, a mira treme, e ninguem descobre que o desvio-padrao
// estacionario nao e' o que devia. Entao ela mora aqui, isolada, e
// `tools\rcs_hand_check.cpp` exercita ESTE arquivo (nao uma copia dele):
//
//     powershell -File tools\build_rcs_hand_check.ps1
//     %TEMP%\rcs_hand_check.exe
//
// A REGRA DO PROJETO QUE ISSO RESPEITA: a conta mora num lugar so'. Um teste
// que reimplementa a formula testa a copia, nao o codigo. Referencia:
// `PlayerConfig::LootRawPassaSubFiltro` (§2f do ARQUITETURA.md), que existe
// pelo mesmo motivo.
//
// TUDO AQUI E' `inline` E SEM ESTADO GLOBAL: o estado do ruido (o valor
// corrente do passeio) e' do chamador, passado por referencia. Isso permite
// rodar duas instancias — uma por eixo — sem interferencia, e permite o teste
// rodar 100 mil passos sem tocar em nada do cheat.
// ===========================================================================

#include <cmath>
#include <random>

namespace RcsNoise {

// ---------------------------------------------------------------------------
// Uniforme em [0,1) e simetrico em [-1,1).
//
// Usa os 24 bits ALTOS do mt19937: os baixos sao os de pior qualidade na
// distribuicao uniforme do gerador. O custo e' um shift literal.
// ---------------------------------------------------------------------------
inline float Unit(std::mt19937& r) { return (float)(r() >> 8) / (float)(1u << 24); }
inline float Sym(std::mt19937& r)  { return Unit(r) * 2.0f - 1.0f; }

// ---------------------------------------------------------------------------
// Normal(0,1) por Box-Muller.
//
// O `+ 1e-7f` no u1 nao e' decoracao: `Unit` pode devolver exatamente 0, e
// `log(0)` e' -infinito — que vira NaN depois do sqrt e CONTAMINA o acumulador
// do servo por toda a rajada. Um frame em 16 milhoes daria esse zero; e' raro
// o bastante para passar num teste curto e acontecer numa partida longa.
// ---------------------------------------------------------------------------
inline float Normal(std::mt19937& r) {
    const float u1 = Unit(r) * 0.9999999f + 1e-7f;
    const float u2 = Unit(r);
    return std::sqrt(-2.0f * std::log(u1)) * std::cos(6.28318530718f * u2);
}

// ---------------------------------------------------------------------------
// OUStep — um passo de Ornstein-Uhlenbeck, discretizado de forma EXATA.
//
//     dx = -x/tau * dt + sigma * dW
//
// A solucao exata desse SDE num intervalo `dt` e':
//
//     x' = x * e^(-dt/tau) + sqrt( (sigma^2 * tau / 2) * (1 - e^(-2dt/tau)) ) * N(0,1)
//
// e com a troca `sigma = amp * sqrt(2/tau)` (ou seja `amp` = desvio-padrao
// ESTACIONARIO) sobra:
//
//     x' = x * e^(-dt/tau) + amp * sqrt(1 - e^(-2dt/tau)) * N(0,1)
//
// POR QUE NAO A FORMA "DE EULER" (`x += -x*dt/tau + sigma*sqrt(dt)*N`): ela e'
// uma aproximacao de primeira ordem. Quando `dt` se aproxima de `tau` — que
// acontece sempre que o processo trava ou roda a 30 Hz com tau de 200 ms — a
// variancia estacionaria que ela produz NAO e' `amp`, e o passeio vira
// quase-aleatorio. A forma exponencial preserva a variancia estacionaria para
// QUALQUER `dt`, entao 144 Hz e 30 Hz geram o mesmo passeio em tempo de
// parede. Ha um teste para exatamente isso.
//
// `amp`  = desvio-padrao estacionario, em COUNTS.
// `tau`  = tempo de correlacao, em SEGUNDOS.
// `dt`   = intervalo, em SEGUNDOS.
// Devolve o novo valor de `x` (que tambem e' escrito por referencia).
// ---------------------------------------------------------------------------
inline float OUStep(float& x, float tau, float amp, float dt, std::mt19937& r) {
    // amp 0 = sem ruido. Zera o estado em vez de so' devolver 0: se `amp` for
    // desligado no meio de uma rajada, um `x` antigo ficaria guardado e
    // reapareceria ao religar.
    if (!(amp > 0.0f)) { x = 0.0f; return 0.0f; }
    if (!(tau > 0.0005f)) tau = 0.0005f;
    // dt <= 0 nao tem passo valido — devolve o estado sem mexer.
    if (!(dt > 0.0f)) return x;

    const float e  = std::exp(-dt / tau);
    const float sd = std::sqrt(1.0f - e * e);

    x = x * e + amp * sd * Normal(r);

    // Trava em 4 sigma. Uma Gaussiana pura nao precisa de trava, mas esta
    // sequencia alimenta um acumulador que vira PACOTE DE MOUSE: um valor de
    // 9 sigma (probabilidade ~0, mas nao zero num gerador finito) seria um
    // flick de 3 graus. Num ruido nao vale a estatistica bonita, vale o pior
    // caso.
    const float lim = 4.0f * amp;
    if (x >  lim) x =  lim;
    if (x < -lim) x = -lim;
    return x;
}

} // namespace RcsNoise

// ===========================================================================
// SERVO — o filtro de 1a ordem que transforma "dever" em "puxar agora"
//
// Mora aqui pelo mesmo motivo do OUStep: e' a outra peca numerica onde um erro
// passa silencioso. A garantia que ele TEM que cumprir e' a conservacao: ao
// longo da rajada, a soma dos passos emitidos tem que ser igual a soma do que
// foi devido (menos o que a trava de `cap` cortar de proposito). Se essa
// garantia quebrar, o RCS fica fraco de um jeito que ninguem consegue
// diagnosticar olhando a mira — so' olhando o numero.
//
//     step  = owed * dt/(lag+dt)      , com step limitado a +-cap
//     owed -= step
//
// `lag` = 0 significa "sem suavidade": emite tudo no frame e zera a divida,
// byte a byte o comportamento antigo.
// ===========================================================================
namespace RcsServo {

inline float Step(float& owed, float dt, float lagS, float cap) {
    if (!(lagS > 0.0005f)) {
        // Caminho antigo: aplica o teorico inteiro neste frame.
        float step = owed;
        if (step >  cap) step =  cap;
        if (step < -cap) step = -cap;
        owed = 0.0f;
        return step;
    }
    if (!(dt > 0.0f)) return 0.0f;

    const float alpha = dt / (lagS + dt);
    float step = owed * alpha;

    // O cap e' aplicado ANTES de debitar: o que a trava corta continua
    // devendo e sai no frame seguinte. Cortar depois de debitar perderia
    // counts em silencio — e' a diferenca entre "o servo esta' limitado" e
    // "o servo esta' roubando".
    if (step >  cap) step =  cap;
    if (step < -cap) step = -cap;
    owed -= step;
    return step;
}

} // namespace RcsServo
