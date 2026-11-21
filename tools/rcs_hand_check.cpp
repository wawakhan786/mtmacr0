// ===========================================================================
// rcs_hand_check.cpp — verificacao SEM JOGO da matematica da humanizacao
//
// Exercita `PubgExt\SDK\RcsNoise.h` DIRETO (nao uma copia dele): o OUStep, o
// gerador normal e o servo de 1a ordem. Se este arquivo compilar e passar, a
// numeracao do ruido e do servo esta' certa — o que NAO prova que a
// compensacao em jogo esta' certa, e' bom deixar dito.
//
// Build:
//     powershell -ExecutionPolicy Bypass -File tools\build_rcs_hand_check.ps1
//     %TEMP%\rcs_hand_check.exe
//
// O QUE ELE PEGA (e por que cada um importa):
//
//   OU: desvio estacionario == amp    um erro na discretizacao muda a
//                                     AMPLITUDE do ruido, nao a forma. O
//                                     ruido continua "parecendo" ruido, so'
//                                     que 3x maior — e vira um flick.
//   OU: independencia de dt           se so' valesse a 144 Hz, cair para 60 Hz
//                                     mudaria o passeio em tempo de parede.
//   OU: autocorrelacao == e^(-dt/tau) e' o que separa PASSEIO de CHIADO. Sem
//                                     isto o "ruido correlacionado" nao e'
//                                     correlacionado e a humanizacao volta a
//                                     ser cosmetica.
//   OU: trava de 4 sigma              a cauda de uma Gaussiana nao vale a
//                                     estatistica num acumulador que vira
//                                     pacote de mouse.
//   servo: conserva o total           a garantia central. Se quebrar, o RCS
//                                     fica fraco e nao da' para diagnosticar
//                                     olhando a mira.
//   servo: lag 0 == caminho antigo    o toggle tem que devolver o
//                                     comportamento anterior byte a byte.
//   servo: o cap nao rouba            o que a trava corta continua devendo.
//   servo: constante de tempo ~3*lag  o slider tem que significar ms, nao
//                                     frames.
//   traco: o DENTE DE SERRA           a evidencia visual de que a mudanca faz
//                                     o que promete: sem servo a vista nunca
//                                     sobe; com servo ela sobe e e' puxada.
// ===========================================================================

#include <cstdio>
#include <cmath>
#include <random>
#include <vector>

#include "../PubgExt/SDK/RcsNoise.h"

// ---------------------------------------------------------------------------
// Placar
// ---------------------------------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;

static void Ok(const char* nome, bool cond, const char* detalhe) {
    std::printf("  %s %-44s %s\n", cond ? "[+]" : "[-]", nome, detalhe);
    if (cond) ++g_pass; else ++g_fail;
}

// ---------------------------------------------------------------------------
// Estatistica simples sobre um vetor
// ---------------------------------------------------------------------------
static double Media(const std::vector<float>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (size_t i = 0; i < v.size(); ++i) s += (double)v[i];
    return s / (double)v.size();
}

static double Desvio(const std::vector<float>& v) {
    if (v.size() < 2) return 0.0;
    const double m = Media(v);
    double s = 0.0;
    for (size_t i = 0; i < v.size(); ++i) {
        const double d = (double)v[i] - m;
        s += d * d;
    }
    return std::sqrt(s / (double)(v.size() - 1));
}

// ---------------------------------------------------------------------------
// Barra ASCII para o traco. `escala` = valor que ocupa a largura toda.
// ---------------------------------------------------------------------------
static void Barra(char* out, int largura, float valor, float escala, bool negativo) {
    if (!(escala > 0.0f)) escala = 1.0f;
    float u = valor / escala;
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    int n = (int)(u * (float)largura + 0.5f);
    if (n < 0) n = 0;
    if (n > largura) n = largura;
    for (int i = 0; i < n; ++i) out[i] = negativo ? '=' : '#';
    out[n] = '\0';
}

int main() {
    std::printf("\n=== rcs_hand_check — humanizacao do RCS (v2: servo + passeio) ===\n\n");

    // =====================================================================
    // 1) OU — desvio estacionario == amp
    //
    // 200 mil passos a 144 Hz sao ~1388 s simulados; com tau = 0,2 s isso e'
    // ~6900 tempos de correlacao, entao a media amostral ja' convergiu.
    // =====================================================================
    std::printf("[OU — passeio]\n");
    {
        std::mt19937 r(0xC0FFEEu);
        float x = 0.0f;
        const float amp = 1.0f, tau = 0.2f, dt = 1.0f / 144.0f;
        std::vector<float> v;
        v.reserve(200000);
        for (int i = 0; i < 200000; ++i) {
            RcsNoise::OUStep(x, tau, amp, dt, r);
            if (i > 2000) v.push_back(x);      // descarta o transiente
        }
        char b[160];
        const double sd = Desvio(v);
        const double me = Media(v);
        std::snprintf(b, sizeof(b), "sd=%.3f (alvo %.1f)  media=%+.3f", sd, (double)amp, me);
        Ok("OU: desvio estacionario == amp", std::fabs(sd - (double)amp) < 0.10 * (double)amp, b);

        std::snprintf(b, sizeof(b), "media=%+.4f", me);
        Ok("OU: media ~ 0 (sem vies)", std::fabs(me) < 0.02, b);
    }

    // =====================================================================
    // 2) OU — independencia de dt
    //
    // Mesmo tempo de parede (300 s), duas cadencias. Se a discretizacao
    // estivesse errada, 60 Hz e 144 Hz dariam passes diferentes — e o cheat
    // rodaria diferente do que foi medido.
    // =====================================================================
    {
        const float amp = 1.0f, tau = 0.2f;
        const double totalS = 300.0;

        std::vector<float> a144, a60;
        {
            std::mt19937 r(111u);
            float x = 0.0f;
            const float dt = 1.0f / 144.0f;
            const int n = (int)(totalS * 144.0);
            for (int i = 0; i < n; ++i) {
                RcsNoise::OUStep(x, tau, amp, dt, r);
                if (i > 500) a144.push_back(x);
            }
        }
        {
            std::mt19937 r(222u);
            float x = 0.0f;
            const float dt = 1.0f / 60.0f;
            const int n = (int)(totalS * 60.0);
            for (int i = 0; i < n; ++i) {
                RcsNoise::OUStep(x, tau, amp, dt, r);
                if (i > 200) a60.push_back(x);
            }
        }
        char b[160];
        const double s144 = Desvio(a144);
        const double s60  = Desvio(a60);
        const double rel = std::fabs(s144 - s60) / (0.5 * (s144 + s60));
        std::snprintf(b, sizeof(b), "144Hz=%.3f  60Hz=%.3f  diferenca=%.1f%%", s144, s60, rel * 100.0);
        Ok("OU: mesmo passeio a 60 e 144 Hz", rel < 0.10, b);
    }

    // =====================================================================
    // 3) OU — autocorrelacao
    //
    // E' o teste que separa PASSEIO de CHIADO. Com dt = tau/10, a correlacao
    // de um passo pro seguinte tem que ser e^(-0,1) = 0,9048. Ruido branco
    // daria ~0 — e ruido branco e' exatamente o que a v1 tinha.
    // =====================================================================
    {
        std::mt19937 r(4242u);
        float x = 0.0f;
        const float amp = 1.0f, tau = 0.2f, dt = tau / 10.0f;
        std::vector<float> v;
        v.reserve(120000);
        for (int i = 0; i < 120000; ++i) {
            RcsNoise::OUStep(x, tau, amp, dt, r);
            if (i > 2000) v.push_back(x);
        }
        const double m = Media(v);
        double num = 0.0, den = 0.0;
        for (size_t i = 0; i + 1 < v.size(); ++i) {
            num += ((double)v[i] - m) * ((double)v[i + 1] - m);
        }
        for (size_t i = 0; i < v.size(); ++i) {
            const double d = (double)v[i] - m;
            den += d * d;
        }
        const double corr  = (den > 0.0) ? (num / den) : 0.0;
        const double alvo  = std::exp(-(double)dt / (double)tau);   // 0.9048
        char b[160];
        std::snprintf(b, sizeof(b), "corr=%.4f (alvo %.4f, branco=~0)", corr, alvo);
        Ok("OU: autocorrelacao de 1 passo", std::fabs(corr - alvo) < 0.03, b);
    }

    // =====================================================================
    // 4) OU — a trava de 4 sigma segura
    //
    // tau longo + amp alto = a pior combinacao (passeio quase livre). Nenhuma
    // amostra pode passar de 4*amp, senao um "ruido" vira flick.
    // =====================================================================
    {
        std::mt19937 r(7u);
        float x = 0.0f;
        const float amp = 2.0f, tau = 5.0f, dt = 1.0f / 144.0f;
        bool estourou = false;
        float pior = 0.0f;
        for (int i = 0; i < 300000; ++i) {
            RcsNoise::OUStep(x, tau, amp, dt, r);
            const float a = std::fabs(x);
            if (a > pior) pior = a;
            if (a > 4.0f * amp) estourou = true;
        }
        char b[160];
        std::snprintf(b, sizeof(b), "maior |x| = %.3f (limite %.1f)", pior, 4.0 * (double)amp);
        Ok("OU: trava de 4 sigma segura", !estourou, b);
    }

    // =====================================================================
    // 5) OU — amp 0 desliga E limpa o estado
    //
    // Se so' devolvesse 0 sem zerar `x`, religar o ruido traria de volta um
    // valor antigo — a mira daria um pulo no instante do toggle.
    // =====================================================================
    {
        std::mt19937 r(99u);
        float x = 5.0f;                        // estado sujo de proposito
        const float ret = RcsNoise::OUStep(x, 0.2f, 0.0f, 1.0f / 144.0f, r);
        char b[160];
        std::snprintf(b, sizeof(b), "ret=%.3f  x=%.3f", (double)ret, (double)x);
        Ok("OU: amp 0 zera o estado", ret == 0.0f && x == 0.0f, b);
    }

    // =====================================================================
    // 6) SERVO — conserva o total   (a garantia central)
    // =====================================================================
    std::printf("\n[servo — filtro de 1a ordem]\n");
    {
        float owed = 0.0f;
        const float lagS = 0.045f, dt = 1.0f / 144.0f, cap = 500.0f;
        double entrou = 0.0, saiu = 0.0;
        for (int i = 0; i < 1440; ++i) {       // 10 s a 144 Hz
            const float inflow = 3.0f;
            owed += inflow;
            entrou += (double)inflow;
            saiu   += (double)RcsServo::Step(owed, dt, lagS, cap);
        }
        const double erro = std::fabs((entrou - saiu) - (double)owed);
        char b[160];
        std::snprintf(b, sizeof(b), "entrou=%.2f saiu=%.2f devendo=%.4f erro=%.2e",
                      entrou, saiu, (double)owed, erro);
        Ok("servo: conserva o total", erro < 1e-2, b);
    }

    // =====================================================================
    // 7) SERVO — lag 0 e' o caminho antigo
    // =====================================================================
    {
        float owed = 7.5f;
        const float passo = RcsServo::Step(owed, 1.0f / 144.0f, 0.0f, 60.0f);
        char b[160];
        std::snprintf(b, sizeof(b), "passo=%.2f devendo=%.2f", (double)passo, (double)owed);
        Ok("servo: lag 0 == aplica tudo no frame", passo == 7.5f && owed == 0.0f, b);
    }

    // =====================================================================
    // 8) SERVO — o cap nao rouba
    //
    // Devendo 1000 counts com cap 60: nenhum passo pode passar de 60, e a
    // soma tem que chegar aos 1000 inteiros (o cap ATRASA, nao corta).
    // =====================================================================
    {
        float owed = 1000.0f;
        const float lagS = 0.045f, dt = 1.0f / 144.0f, cap = 60.0f;
        double saiu = 0.0;
        float maior = 0.0f;
        int frames = 0;
        while (owed > 0.01f && frames < 100000) {
            const float s = RcsServo::Step(owed, dt, lagS, cap);
            if (std::fabs(s) > maior) maior = std::fabs(s);
            saiu += (double)s;
            ++frames;
        }
        char b[160];
        std::snprintf(b, sizeof(b), "maior passo=%.2f (cap %.0f)  total=%.2f  %d frames",
                      (double)maior, (double)cap, saiu, frames);
        Ok("servo: cap atrasa, nao corta", maior <= cap + 0.001f && std::fabs(saiu - 1000.0) < 0.5, b);
    }

    // =====================================================================
    // 9) SERVO — a constante de tempo e' o slider, em ms
    //
    // Resposta ao degrau: o tempo ate' 95% tem que ser ~3*lag em TEMPO DE
    // PAREDE, nas duas cadencias. Se o codigo medisse em frames, 60 Hz levaria
    // 2,4x mais tempo.
    // =====================================================================
    {
        char b[200];
        bool ok = true;
        double med[2] = { 0.0, 0.0 };
        const float lags[2] = { 0.045f, 0.120f };
        const float dts[2]  = { 1.0f / 144.0f, 1.0f / 60.0f };
        for (int k = 0; k < 2; ++k) {
            float owed = 1.0f;                 // degrau unitario
            double acumulado = 0.0;
            double tempoAte95 = -1.0;
            int frames = 0;
            while (frames < 100000 && acumulado < 0.95) {
                acumulado += (double)RcsServo::Step(owed, dts[k], lags[k], 1000.0f);
                ++frames;
            }
            tempoAte95 = (double)frames * (double)dts[k];
            med[k] = tempoAte95;
            const double esperado = 3.0 * (double)lags[k];
            const double rel = std::fabs(tempoAte95 - esperado) / esperado;
            std::snprintf(b, sizeof(b), "lag=%.0fms dt=%.1fms -> 95%% em %.1fms (alvo ~%.1fms)",
                          (double)(lags[k] * 1000.0f), (double)(dts[k] * 1000.0f),
                          tempoAte95 * 1000.0, esperado * 1000.0);
            Ok("servo: constante de tempo = slider", rel < 0.20, b);
            if (rel >= 0.20) ok = false;
        }
        (void)ok;
    }

    // =====================================================================
    // 10) NORMAL — sem NaN, sem vies, desvio 1
    //
    // Um NaN aqui contamina o acumulador do servo por toda a rajada: a mira
    // simplesmente para, sem erro e sem log. O `+1e-7f` do u1 existe por isto.
    // =====================================================================
    std::printf("\n[gerador normal]\n");
    {
        std::mt19937 r(2026u);
        std::vector<float> v;
        v.reserve(200000);
        bool temNaN = false;
        for (int i = 0; i < 200000; ++i) {
            const float n = RcsNoise::Normal(r);
            if (!(n == n)) temNaN = true;      // NaN nao e' igual a si mesmo
            v.push_back(n);
        }
        char b[160];
        std::snprintf(b, sizeof(b), "media=%+.4f  sd=%.4f  NaN=%d",
                      Media(v), Desvio(v), (int)temNaN);
        Ok("normal: sem NaN, media 0, sd 1",
           !temNaN && std::fabs(Media(v)) < 0.02 && std::fabs(Desvio(v) - 1.0) < 0.03, b);
    }

    // =====================================================================
    // 11) O TRACO — dente de serra
    //
    // Isto e' o que a mudanca existe para fazer, e da' para VER aqui.
    //
    // Um tiro a cada 10 frames injeta `drop` de 6 counts no acumulador. Sem
    // servo, o passo emitido acompanha o tiro: 6, 0, 0, 0, 0, 0, 0, 0, 0, 0.
    // A VISTA (que e' `drop` menos o passo) nunca sobe — fica reta.
    //
    // Com servo, o passo sai espalhado: a vista SOBE no tiro e e' puxada de
    // volta. E' o dente de serra.
    // =====================================================================
    std::printf("\n[traco — vista durante uma rajada de 40 frames]\n");
    {
        const float drop = 6.0f;      // counts que o recuo empurra por tiro
        const int   periodo = 10;     // 1 tiro a cada 10 frames
        const int   frames = 40;
        const float dt = 1.0f / 144.0f;
        const float lagS = 0.045f;
        const float cap = 60.0f;

        // --- sem servo (v1) ---
        std::vector<float> vistaSem, vistaCom, passoSem, passoCom;
        {
            float owed = 0.0f, vista = 0.0f;
            for (int i = 0; i < frames; ++i) {
                const float in = (i % periodo == 0) ? drop : 0.0f;
                owed += in;
                const float s = RcsServo::Step(owed, dt, 0.0f, cap);   // lag 0
                vista += in - s;
                passoSem.push_back(s);
                vistaSem.push_back(vista);
            }
        }
        {
            float owed = 0.0f, vista = 0.0f;
            for (int i = 0; i < frames; ++i) {
                const float in = (i % periodo == 0) ? drop : 0.0f;
                owed += in;
                const float s = RcsServo::Step(owed, dt, lagS, cap);
                vista += in - s;
                passoCom.push_back(s);
                vistaCom.push_back(vista);
            }
        }

        std::printf("   f | passo V1 | passo V2 | vista V1 | vista V2\n");
        std::printf("  ---+----------+----------+----------+---------\n");
        char barra[64], b[256];
        for (int i = 0; i < frames; ++i) {
            Barra(barra, 10, passoCom[(size_t)i], drop, false);
            std::snprintf(b, sizeof(b), "  %2d |   %5.2f  | %5.2f %-10s |  %+6.2f  |  %+6.2f\n",
                          i, (double)passoSem[(size_t)i], (double)passoCom[(size_t)i], barra,
                          (double)vistaSem[(size_t)i], (double)vistaCom[(size_t)i]);
            std::printf("%s", b);
        }

        const double sdSem = Desvio(vistaSem);
        const double sdCom = Desvio(vistaCom);
        // O "sem servo" da' desvio ZERO — e' literalmente esse o defeito: a
        // vista nao se mexe. Entao a razao vira "infinito" e nao um numero.
        if (sdSem > 1e-9) {
            std::snprintf(b, sizeof(b), "desvio da vista: sem servo=%.3f  com servo=%.3f  (x%.1f)",
                          sdSem, sdCom, sdCom / sdSem);
        } else {
            std::snprintf(b, sizeof(b), "desvio da vista: sem servo=0.000 (RETA)  com servo=%.3f  (infinito)",
                          sdCom);
        }
        Ok("traco: o servo produz dente de serra", sdCom > sdSem * 3.0, b);

        // O passo do primeiro frame tem que ser FRACAO do teorico com servo, e
        // o teorico inteiro sem servo. E' a assinatura que sumia.
        char b2[200];
        std::snprintf(b2, sizeof(b2), "V1=%.2f (teorico %.2f)  V2=%.2f (%.0f%% do teorico)",
                      (double)passoSem[0], (double)drop, (double)passoCom[0],
                      (double)(100.0f * passoCom[0] / drop));
        Ok("traco: o primeiro frame nao compensa tudo", passoCom[0] < drop * 0.35f, b2);
    }

    // =====================================================================
    std::printf("\n=== RESULTADO: %d passaram, %d falharam ===\n\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
