#pragma once
#include "ActorEntity.h"
#include "EngineStructs.h"
#include <unordered_map>
#include <unordered_set>
// ===========================================================================
// Validacao ESTRITA de ponteiro (user-mode x64).
//
// O IsAddrValid do projeto so' checa faixa — deixa passar ponteiro
// RECICLADO (pool liberado), e ler la' em ring0 = BSOD (PAGE_FAULT).
// ===========================================================================
inline bool IsCanonicalPtr(uint64_t p)
{
	if (p == 0) return false;
	if (p < 0x10000ULL) return false;
	if (p & 7ULL) return false;
	uint64_t top = p >> 47;                       // x64: bits 48..63 assinam o 47
	if (top != 0 && top != 0x1FFFFULL) return false;

	// -----------------------------------------------------------------------
	// SO' ACEITA ENDERECO DE USER-MODE.
	//
	// A versao anterior aceitava tambem endereco de KERNEL (0xFFFF...) porque
	// so' olhava canonicidade. Isso era um buraco real: se um campo lido do
	// jogo viesse com um valor 0xFFFF..., a gente pedia pro driver ler um
	// endereco de kernel NO CONTEXTO DO JOGO — endereco que nao existe la' —
	// e o resultado e' bugcheck, nao erro.
	//
	// Todo ponteiro que a gente le' do jogo e' user-mode (o jogo roda em
	// ring3). Qualquer coisa acima de 0x0000800000000000 e' lixo.
	// -----------------------------------------------------------------------
	if (p >= 0x0000800000000000ULL) return false;

	return true;
}

inline bool IsCanonicalRange(uint64_t base, uint64_t size)
{
	if (!IsCanonicalPtr(base)) return false;
	if (size == 0 || size > 0x100000ULL) return false;
	if (base + size < base) return false;         // overflow
	return true;
}

class Engine
{

private:
	uint64_t OwningActor;
	int MaxPacket;
	int localTempId;
	std::string oldMapName;
	CameraCacheEntry CameraEntry; // ScriptStruct Engine.CameraCacheEntry
	MinimalViewInfo CameraViewInfo; // ScriptStruct Engine.MinimalViewInfo
	
	std::chrono::steady_clock::time_point LastActorRefreshTimestamp;
	std::chrono::steady_clock::time_point LastWeaponUpdateTimestamp;
	bool bIsMapInitialized = false;
	bool bIsDriving = false;

	float CurrentBulletSpeed = 0.0f;
	float CurrentGravity = -9.8f;
	float CurrentSimulationSubstepTime = 0.016f;
	float CurrentVDragCoefficient = 1.0f;

public:
	uintptr_t CachedBaseAddress = 0;
	uint64_t LocalCharacterPawn = 0;
	uint64_t CachedDecryptKey = 0;
	bool FrameTranslateError = false;

	// -----------------------------------------------------------------------
	// bInMatch — "neste tick o Cache() conseguiu ler um estado de PARTIDA".
	//
	// Recalculado em TODO tick (ver Cache): comeca falso e so' vira true
	// depois que UWorld/mapa/camera/array de atores passaram na validacao.
	// O MemoryLoop publica ele como SharedData::DataValid, que e' o gate de
	// desenho — e' o que impede o ESP de desenhar a ultima partida no lobby.
	//
	// Nao e' flag grudenta de proposito: se ficar presa em false, o ESP nunca
	// mais desenha; por isso ela e' reescrita a cada tick, e nao "setada uma
	// vez e limpa depois".
	// -----------------------------------------------------------------------
	bool bInMatch = false;

	// Limpa o que a thread de render consome (as quatro listas + o que o HUD
	// le' fora do snapshot) e marca bInMatch = false. Chamado pelo Cache() em
	// todo caminho que significa "nao existe partida/dado agora".
	//
	// `motivo` so' serve para o log: ele aparece UMA vez por transicao
	// valido -> invalido, o que deixa rastreavel no logs.txt porque o ESP
	// parou de desenhar (e que nao e' um "return" silencioso qualquer).
	void InvalidateSnapshot(const char* motivo = "sem partida");

	template<typename T>
	T SafeRead(uintptr_t addr, const char* name) {
		if (FrameTranslateError) return T{};
		T val{};
		if (!TargetProcess.Read(addr, &val, sizeof(T), name)) {
			FrameTranslateError = true;
			return T{};
		}
		return val;
	}

	float GetCurrentBulletSpeed() { return CurrentBulletSpeed; }
	float GetCurrentGravity() { return CurrentGravity; }

	uint64_t UWorld = 0, CurrentLevel = 0, GameInstance = 0, LocalPlayers = 0, PlayerController = 0, AcknowledgedPawn = 0, PlayerCameraManager = 0, GNames = 0;
	// Roster do GameState: PlayerState* -> stats
	struct PlayerStats { int Kills = -1; int Team = 0; float Damage = 0.0f; bool Valid = false; };
	std::unordered_map<uint64_t, PlayerStats> PlayerStatsMap;

	// GameState (zona azul / minimapa)
	uint64_t GameState = 0;
	float BlueZoneRadius = 0.0f, SafetyZoneRadius = 0.0f;
	UEVector BlueZonePos = {}, SafetyZonePos = {};

	// =====================================================================
	// MEDICAO DA VISIBILIDADE (`LastTimeScreenRenderered`) — o que a UI mostra.
	//
	// Existe porque "a cor nao muda" tem QUATRO causas possiveis e o olho nao
	// separa nenhuma:
	//   1. o toggle esta' desligado          -> `VisAtivo` = false
	//   2. o tempo de mundo nao veio          -> `VisBase` = "local"/"nenhuma"
	//   3. o campo nao avanca para inimigo    -> `VisInimDly` enorme SEMPRE
	//   4. a tolerancia esta' errada          -> dly plausivel mas vis=1
	// Publicar os numeros no menu resolve isso sem abrir o log.
	// =====================================================================
	// Cada um dos TRES campos de tempo tem o seu "parado ha" em ms.
	// Ordem: [0]=0x748 submit, [1]=0x74C render, [2]=0x750 on-screen.
	float VisLocalParadoMs[3] = { 0.0f, 0.0f, 0.0f };
	float VisInimParadoMs[3]  = { 0.0f, 0.0f, 0.0f };
	bool  VisLocalVis = true;
	bool  VisInimVis  = true;
	int   VisAtivos   = 0;
	int   VisOcultos  = 0;
	bool  VisAtivo    = false;
	// Qual indice o veredito esta'' usando: 0=0x748, 1=0x74C, 2=0x750.
	// -1 = nenhum vivo (fail-open).
	int   VisCampoEscolhido = -1;
	bool  VisCampoVivo = false;
	std::chrono::steady_clock::time_point VisUltimaMudanca[3]{};
	// ---- compatibilidade com a UI (Main.cpp/SharedState) ----
	float VisWorldTime  = 0.0f;   // nao ha' mais base externa; mantido p/ UI
	float VisLocalLr    = 0.0f;
	float VisLocalDlyMs = 0.0f;   // == VisLocalParadoMs[escolhido]
	float VisInimLr     = 0.0f;
	float VisInimDlyMs  = 0.0f;   // == VisInimParadoMs[escolhido]
	int   VisBaseFonte  = 3;      // 3 = por mudanca do campo
	std::vector<std::shared_ptr<ActorEntity>> Actors;
	std::vector<std::shared_ptr<ActorEntity>> GrenadeActors;  // Grenade/Molotov ESP
	std::vector<std::shared_ptr<ActorEntity>> LootActors;     // Loot ESP
	std::vector<std::shared_ptr<ActorEntity>> VehicleActors;  // Vehicle ESP
	std::unordered_map<uint64_t, std::shared_ptr<ActorEntity>> ActorCache;
	Engine();
	~Engine();
	inline bool InitDecrypt(uint64_t offset);
	void Cache();
	void UpdatePlayers();
	void UpdateGrenades();  // Read projectile-specific offsets for grenade ESP
	void UpdateWeapons();   // Arma equipada (nome) de cada ator
	void UpdateVehicles();  // Vehicle ESP (vida/combustivel/posicao)
	void UpdateLoot();      // Loot ESP (ItemID/posicao)
	void DiagChain();       // DIAGNOSTICO: candidatos da cadeia do RCS
	// PhysX: a sonda DiagPhysx() foi removida (ver Globals.h / ARQUITETURA.md).
	void ReadGameState();   // Zone azul / SafetyZone / WorldToMap
	void UpdatePlayerList(); // GameState -> PlayerArray: kills/team/dano
	void UpdateWeaponPhysics();
	Vector3 GetPrediction(const std::shared_ptr<ActorEntity>& target, const Vector3& targetPos, float& dropPitchOffset);
	std::vector<std::shared_ptr<ActorEntity>> GetActors();
	CameraCacheEntry GetCameraCache();
	void RefreshViewMatrix();
	uint64_t GetActorSize();
	uintptr_t xe_decrypt(const uintptr_t encrypted);
	void HealthDecipher(uint8_t* a1, uint32_t a3);
	// DecryptHealth — devolve a vida decifrada E informa se a LEITURA foi valida.
	//
	// `readValid` e' parametro de saida por REFERENCIA, de proposito:
	//   - `0.0f` significava DUAS coisas diferentes ("o valor e' zero" e "a
	//     leitura falhou"). Quem so' olha o float nao consegue separar morte de
	//     falha de leitura — e marcar um vivo como morto e' pior que nao filtrar.
	//   - NAO tem valor default: qualquer call site que esquecer o `bool` nao
	//     compila. Com `std::pair` (ou um struct) o call site poderia escrever
	//     `.first` e descartar a validade em silencio — que e' exatamente o bug
	//     que este parametro existe para impedir.
	float DecryptHealth(uint8_t* buffer, bool& readValid);
	DWORD DecryptCIndex(DWORD Encrypted);
	bool SelfTestDecryptCIndex();
	void GetGNames();
	int IsPlayer(std::string entity_name);
	int IsProjectile(const std::string& entity_name);  // Returns explosion radius (UU) or 0
	bool IsVehicle(const std::string& entity_name);
	bool IsLoot(const std::string& entity_name);
	bool IsAirdrop(const std::string& entity_name);
	std::string GetNamesFromList(DWORD ID);
	std::string GetNames(DWORD ID);
	// =====================================================================
	// CLSPROBE — identificacao por CLASSE (build 2610.1.2.7).
	//
	// Nesta build o nome do proprio ator (ObjectID @ +0x10) e HASH para os
	// personagens ('*xxxxxxxxxx' — 28 mil nomes com esse padrao na tabela
	// estatica do dump), entao o IsPlayer por nome do objeto nunca casa.
	// O ponteiro de Class (UObject+ClassPrivate) NAO esta no dump de
	// reflexao (UObject nao tem UPROPERTY), entao sondamos os candidatos
	// {0x08, 0x18, 0x20}: vence o primeiro que decifra para endereco
	// canonico E resolve um nome de classe legivel na tabela GNames.
	//
	// Custo: probe unica por ator unico (memo pos/negativo); o nome
	// resolvido e cacheado por PONTEIRO DE CLASSE (compartilhado entre
	// todas as instancias da mesma classe). As linhas [CLSPROBE] dizem
	// qual offset e' o ClassPrivate verdadeiro desta build — apos a
	// confirmacao em jogo o candidato certo fica documentado aqui.
	// =====================================================================
	std::string ResolveActorClassName(uint64_t actorAddr);
	std::unordered_map<uint64_t, std::string> ClassNameByClassPtr; // class ptr -> nome
	std::unordered_map<uint64_t, uint64_t>   ClassPtrByActor;     // ator      -> class ptr
	std::unordered_set<uint64_t>              ClassProbeFailed;   // memo negativo
};
