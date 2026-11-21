#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include "SDK/ActorEntity.h"
#include "SDK/EngineStructs.h"

struct SharedData {
    // -----------------------------------------------------------------------
    // VALIDADE DO SNAPSHOT — o gate de TODO desenho de jogo.
    //
    // Antes o render decidia so' por `UWorld != 0`. Isso NAO basta e era a
    // causa do ESP fantasma no lobby: o LOBBY tambem tem um UWorld valido, e
    // quando a partida acabava o `UWorld` continuava preenchido — entao o
    // gate passava e o ESP seguia desenhando a ULTIMA lista de atores que
    // existiu na partida (esqueleto/caixa/nome congelados por cima do lobby).
    //
    // Agora quem decide e' este flag, publicado JUNTO com as listas (ver
    // MemoryLoop em Main.cpp): true so' quando o Engine conseguiu ler um
    // estado de partida consistente neste tick.
    //
    // IMPORTANTE: ele e' RECALCULADO em todo tick do MemoryLoop a partir de
    // Engine::bInMatch — nao existe flag grudenta, entao ele volta a true
    // sozinho no primeiro tick de uma partida nova (nada de "ESP morto").
    // -----------------------------------------------------------------------
    bool DataValid = false;
    std::vector<std::shared_ptr<ActorEntity>> Actors;
    std::vector<std::shared_ptr<ActorEntity>> GrenadeActors;  // Grenade/Molotov ESP
    std::vector<std::shared_ptr<ActorEntity>> VehicleActors;  // Vehicle ESP
    std::vector<std::shared_ptr<ActorEntity>> LootActors;     // Loot ESP

    // ---- GameState (zona azul / zona segura) ----
    bool  HasGameState = false;
    float BlueZoneRadius = 0.0f;
    float SafetyZoneRadius = 0.0f;
    Vector3 BlueZonePos{};
    Vector3 SafetyZonePos{};
    CameraCacheEntry CameraCache;
    uint64_t UWorld = 0;
    uint64_t GNames = 0;
    uint64_t GameInstance = 0;
    uint64_t AcknowledgedPawn = 0;
    uint64_t LocalCharacterPawn = 0; // Character pawn addr (persists even when driving vehicle)
    uint64_t CameraManagerAddr = 0;
    Vector3 Recoil = {0.f, 0.f, 0.f};
    uint32_t MemoryThreadId = 0;
    long long LastUpdateTime = 0;

    // -----------------------------------------------------------------------
    // VISIBILIDADE — os numeros que o menu mostra (ver Engine.h).
    //
    // Publicado no MESMO lock das listas (Main.cpp, MemoryLoop), entao o
    // render le' um conjunto consistente. Sem isto, "o vischeck esta'
    // funcionando?" so' se responde abrindo o logs.txt — e a resposta chega
    // tarde, com a partida perdida.
    // -----------------------------------------------------------------------
    float VisWorldTime  = 0.0f;   // TimeSeconds lido do GameState
    float VisLocalLr    = 0.0f;   // LastTimeScreenRenderered do pawn local
    float VisLocalDlyMs = 0.0f;   // atraso do local em ms
    bool  VisLocalVis   = true;
    float VisInimLr     = 0.0f;   // do inimigo mais proximo
    float VisInimDlyMs  = 0.0f;
    bool  VisInimVis    = true;
    int   VisAtivos     = 0;      // atores com veredito neste tick
    int   VisOcultos    = 0;      // quantos deram "atras de parede"
    bool  VisAtivo      = false;  // a conta rodou?
    int   VisBaseFonte  = 0;      // 3 = por mudanca do campo
    bool  VisCampoVivo  = false;  // algum dos tres campos esta' mudando?
    int   VisCampo      = -1;     // 0=0x748 1=0x74C 2=0x750 (-1=nenhum)
    float VisInimParado[3] = { 0.0f, 0.0f, 0.0f };
    float VisLocalParado[3] = { 0.0f, 0.0f, 0.0f };
};

extern SharedData GSharedData;
extern std::mutex GDataMutex;
extern bool GIsRunning;
