#pragma once
#include "Engine.h"
#include <atomic>
#include <cmath>
#include <random>
#include <memory>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class Aimbot {
private:
    static uint64_t currentTargetAddress;
    static int currentBoneId;
    static int lastMoveX;
    static int lastMoveY;

    static float remainderX;
    static float remainderY;

    static std::chrono::steady_clock::time_point lastTargetSwitchTime;
    static std::chrono::steady_clock::time_point lastTargetSeenTime;
    static bool targetInGrace;
    static std::mt19937 rng;

    // -----------------------------------------------------------------------
    // RCS — ESTADO QUE O CODIGO REALMENTE LE.
    //
    // Aqui moravam ONZE estaticos que eram declarados e definidos e NUNCA
    // lidos: `lastDx`, `lastDy` (sobra do move antigo), `s_basePitch`,
    // `s_baseYaw`, `lastRecP`, `lastRecY` (sobra dos RCS v6 "recoil delta") e
    // `integralFF`, `lastAimP`, `pinnedSrc`, `adaptStart`, `riseFilt` (sobra
    // dos RCS v7/v8 "controlador integral"). Todos removidos.
    //
    // O que sobrou e' o que o feed-forward do acumulador drenado usa de fato.
    // -----------------------------------------------------------------------
    static bool  s_rcsValid;     // ancora do acumulador (1o frame da rajada)
    static float s_ffRvvY;       // ultimo RecoilValueVector.y lido
    static float s_ffVr;         // ultimo VerticalRecovery lido
    static float s_ffRvvX;       // ultimo RecoilValueVector.x lido

    static bool IsHotkeyDown(int index);
    
    static UERotator CalcAngle(Vector3 localPos, Vector3 predictedPos);
    static UERotator NormalizeAngle(UERotator angles);

    static void MoveMouseTowards(Vector2 targetScreenPos, Vector2 viewCenter);
    static float CalculateTargetScore(std::shared_ptr<ActorEntity> ent, Vector3 targetPos, float fov);
    static int GetBestBone(std::shared_ptr<ActorEntity> ent, Vector2 viewCenter);

public:
    static void Tick();
    static void RecoilControl();   // RCS: compensa o recuo real com o mouse
    static void ResetRcs();        // forca reancorar o RCS

    // -----------------------------------------------------------------------
    // CALIBRACAO DE SENSIBILIDADE (counts-por-grau)
    //
    // `Configs.Aimbot.RCSDegPerCount` e' o unico numero que o RCS NAO consegue
    // deduzir sozinho: ele depende da sensibilidade in-game do jogador, que o
    // cheat nao le (e que nenhuma tabela publica mapeia direito).
    //
    // Entao em vez de pedir o numero, MEDIMOS. O botao dispara um pulso de
    // mouse CONHECIDO (200 counts em cada eixo), le o quanto a vista girou, e
    // divide. Depois injeta o pulso inverso para devolver a mira ao lugar.
    //
    // E' exato por construcao: e' a sensibilidade real desta maquina, com o
    // perfil real deste jogador, medida no jogo — nao um valor de tabela.
    // -----------------------------------------------------------------------
    static void RequestSensitivityCalibration();

    // True enquanto a calibracao esta' rodando. O `Tick` consulta isto para
    // ficar FORA: o aimbot injeta mouse pelo MESMO caminho do pulso de
    // calibracao, entao qualquer correcao dele entraria na medida como se
    // fosse a sensibilidade do jogador.
    static bool IsCalibrating();

    // -----------------------------------------------------------------------
    // LEITURA AO VIVO (thread de render)
    //
    // Tudo atomico: quem ESCREVE e' a thread de memoria (RecoilControl), quem
    // LE e' o RenderFrame. Existe para dar para calibrar OLHANDO o menu em vez
    // de abrir o logs.txt.
    // -----------------------------------------------------------------------
    struct RcsReadout {
        float countsPerDeg = 0.0f;   // ganho EFETIVO em uso (1 / degPerCount)
        float dropY        = 0.0f;   // dreno vertical do acumulador neste frame
        float dropR        = 0.0f;   // dreno de recuperacao
        float degY         = 0.0f;   // graus de recuo estimados neste frame
        float residual     = 0.0f;   // deriva medida da mira (auto-calibracao)
        int   cy           = 0;      // counts verticais injetados
        int   cx           = 0;      // counts horizontais injetados
        float gainMul      = 1.0f;   // multiplicador da auto-calibracao
        // ---- humanizacao v2 (servo lag) ----
        float lagMs        = 0.0f;   // latencia SORTEADA desta rajada
        float gainHz       = 1.0f;   // ganho humano efetivo (sub-ganho x reacao)
        float owedY        = 0.0f;   // counts devidos ainda nao liberados
        float stepY        = 0.0f;   // o que o servo liberou neste frame
        float noiseY       = 0.0f;   // desvio correlacionado atual (counts)
        bool  firing       = false;
        bool  calibrating  = false;
        float calibProgress = 0.0f;
        const char* calibMsg = "";
    };
    static RcsReadout GetRcsReadout();
};
