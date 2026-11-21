#pragma once
#include "EngineStructs.h"
#include <cmath>      // NAN (sentinela de "sem leitura" em LastRenderTime)
enum class EPlayerRole: uint8_t
{
	EPlayerRole__VE_None = 0,
	EPlayerRole__VE_Slasher = 1,
	EPlayerRole__VE_Camper = 2,
	EPlayerRole__VE_Observer = 3,
	EPlayerRole__Max = 4,
	EPlayerRole__EPlayerRole_MAX = 5
};

struct Index
{
	int Head,
		neck,
		pelvis,
		Lshoulder,
		Lelbow,
		Lhand,
		Rshoulder,
		Relbow,
		Rhand;
	int	Lbuttock,
		Lknee,
		Lfoot,
		Lball,
		Rbuttock,
		Rknee,
		Rfoot,
		Rball;
};

class FMatrix
{
public:
	float _11, _12, _13, _14;
	float _21, _22, _23, _24;
	float _31, _32, _33, _34;
	float _41, _42, _43, _44;
	float m[4][4];
	FMatrix MatrixMultiplication(const FMatrix& other);
};
struct FQuat
{
	float X;
	float Y;
	float Z;
	float W;
};
struct FTransform
{
	FQuat Rotation;
	FQuat Translation;
	FQuat Scale3D;

	FMatrix ToMatrixWithScale();
};

void GetIndex(Index& index, const std::string& objName);

class ActorEntity
{
public:

	FTransform Head, neck, pelvis, Lshoulder, Lelbow, Lhand, Rshoulder, Relbow, Rhand,
		Lbuttock, Lknee, Lfoot, Lball, Rbuttock, Rknee, Rfoot, Rball;
	UEVector Head3D, neck3D, pelvis3D, Lshoulder3D, Lelbow3D, Lhand3D, Rshoulder3D, Relbow3D, Rhand3D,
		Lbuttock3D, Lknee3D, Lfoot3D, Lball3D, Rbuttock3D, Rknee3D, Rfoot3D, Rball3D;


	uint64_t Class = 0;
	int PlayerRole;
	bool isDie = true;
	int TempId;
	int id;
	int PlayerType;
	float Health;
	std::string objName;
	Index index{};
	uint64_t PlayerState, RootComponent, MeshLastTeamNum, Mesh, Mesh3P, BoneArray, AcknowledgedPawn, RelativeLocation;
	int LastTeamNum;
	FTransform ToWorld;
	bool isCheck = FALSE;
	// ---------------------------------------------------------------------
	// IsVisible — SEMPRE true (fail-open).
	//
	// Era a decisao "esta' na tela ou atras de parede", calculada a partir do
	// LastRenderTimeOnScreen do mesh (0x75C). O diagnostico [VIS] mediu, em
	// partida real, que esse campo NAO avanca para inimigo (so' o player
	// local) — o sinal nao discrimina. Ver Globals.h, "O QUE FOI MEDIDO NO
	// JOGO", e o bloco VISIBILIDADE em Engine::UpdatePlayers.
	//
	// O CAMPO CONTINUA EXISTINDO (e continua true) de proposito, porque tem
	// DOIS leitores vivos:
	//   - ESP (PlayerEsp, escolha entre BoxColour e TextColour)
	//   - AIMBOT (Aimbot.cpp, VisibilityCheck e o bonus de score)
	// Deixa-lo false pintaria todo mundo com a cor "oculto" e, com o
	// VisibilityCheck ligado (default), o aimbot ficaria SEM ALVO.
	//
	// Os campos CRUS do diagnostico (VisSubmit / VisRender / VisDeltaMs /
	// VisPadByte / VisSceneByte / LastRenderTime) foram REMOVIDOS junto com o
	// log [VIS]: so' existiam para a medicao, que ja' foi feita.
	//
	// MEDICAO #2 (versao atual): o log `[VIS]` VOLTOU (Engine::UpdatePlayers,
	// atras de Configs.Survivor.DebugLogs), mas NENHUM campo voltou pra ca' —
	// ele le os floats do mesh na hora e imprime. `IsVisible` continua true:
	// o ESP nao mudou de comportamento.
	// ---------------------------------------------------------------------
	bool IsVisible = true;
	bool bHasBones = false;

	// ---------------------------------------------------------------------
	// VISIBILIDADE MEDIDA (build 2610.1.1.65) — `LastTimeScreenRenderered`.
	//
	// `LastRenderTime` e' o float CRU lido de `Class + SDK.LastTimeScreenRenderered`
	// (0x750, o mesmo slot do `Eyes`). `IsVisible` acima e' o VEREDITO dele.
	//
	// Guardar o cru tem dois usos concretos:
	//   - o log [VIS] imprime o valor e o atraso em ms, entao "esta' sempre
	//     invisivel" vira uma leitura com numero em vez de uma impressao;
	//   - se o offset mudar de build, o valor passa a vir absurdo e da' pra
	//     ver isso ANTES de mexer em qualquer outra coisa.
	//
	// NAN = sem leitura neste tick (o batch nao escreveu). Ver o fail-open em
	// Engine::UpdatePlayers: sem dado, o ator continua visivel.
	// ---------------------------------------------------------------------
	// ---------------------------------------------------------------------
	// VISIBILIDADE POR "QUANDO O CAMPO MUDOU" — e nao por base de tempo.
	//
	// `mesh + 0x75C` (LastRenderTimeOnScreen) AVANCA todo frame em que o pawn
	// e'' desenhado e CONGELA quando ele esta'' atras de algo. Comparar esse
	// valor com um "agora" exigia uma base de tempo confiavel — e as duas
	// tentativas anteriores falharam por isso (TimeSeconds ausente no lobby;
	// pawn local em -1000 quando o corpo dele nao e'' renderizado em FPP).
	//
	// A pergunta que nao precisa de base nenhuma: **o campo mudou nos ultimos
	// N ms?** Se mudou, o motor desenhou agora -> visivel. Se ficou parado mais
	// que N ms, ele nao e'' desenhado -> ocluido. O proprio campo carrega a
	// informacao; nada externo entra na conta.
	//
	//   LastRenderPrev      ultimo valor cru lido de 0x75C
	//   LastRenderMudanca   QUANDO ele mudou pela ultima vez (steady_clock)
	// ---------------------------------------------------------------------
	// TRES campos de tempo, medidos em partida pelo [VISCAN]:
	//   [0] mesh+0x748  LastSubmitTime
	//   [1] mesh+0x74C  LastRenderTime
	//   [2] mesh+0x750  LastRenderTimeOnScreen  (o que deve congelar na parede)
	// Para cada um guardamos o valor do tick anterior e QUANDO ele mudou — a
	// regra do veredito e'' "mudou nos ultimos N ms?" (nenhuma base de tempo).
	float LastRenderTime[3]  = { NAN, NAN, NAN };
	float LastRenderPrev[3]  = { NAN, NAN, NAN };
	float VisParadoMs[3]     = { 0.0f, 0.0f, 0.0f };
	std::chrono::steady_clock::time_point LastRenderMudanca[3]{};
	// Qual indice o veredito usou neste tick (0=submit,1=render,2=onScreen).
	int   VisCampoUsado = 2;
	float VisDelayMs    = 0.0f;   // atraso medido (só vale quando VisCheck liga)

	// ---------------------------------------------------------------------
	// VALIDADE DA POSICAO — O ANTIDOTO DO "ESP FANTASMA".
	//
	// `LastGoodPosition` existe para absorver falhas TRANSITORIAS de leitura do
	// RootComponent (elas acontecem). O problema e' que ele nao tinha TETO:
	// um ator que saiu da partida (objeto reciclado, leitura sempre falhando)
	// continuava desenhado na ULTIMA posicao boa, com `isDie` falso — porque
	// `isDie` depende de uma leitura de vida que tambem falha.
	//
	// `posFalhas` conta falhas SEGUIDAS e `isStale` liga depois de um teto
	// (~10 ticks a 144 Hz = ~70 ms; falha transitoria nao chega perto disso).
	// O ESP pula quem estiver `isStale`.
	// ---------------------------------------------------------------------
	int  posFalhas = 0;
	bool isStale = false;

	// ---------------------------------------------------------------------
	// IDENTIDADE — QUANDO FOI A ULTIMA TENTATIVA DE (RE)LER.
	//
	// `PlayerState` e `Name` sao lidos no CONSTRUTOR, uma vez so'. Se aquela
	// leitura perder a corrida (o pawn nasce antes do PlayerState estar
	// ligado), o ator fica "Player" para o RESTO DA SESSAO — o objeto e'
	// reaproveitado pelo ActorCache e nada mais re-le.
	//
	// O `Engine::UpdatePlayers` re-tenta enquanto a identidade estiver
	// faltando, com throttle por este timestamp.
	// ---------------------------------------------------------------------
	std::chrono::steady_clock::time_point identidadeTentativa;

	// Re-le PlayerState + nome + stats do roster. Devolve true se conseguiu o
	// nome. Ver a nota acima e o construtor.
	bool RefreshIdentity();

	// ---------------------------------------------------------------------
	// SO' OS STATS — sem NENHUMA leitura de memoria.
	//
	// O RefreshIdentity faz duas coisas de custo MUITO diferente:
	//
	//   caro   : dois reads de kernel (Class+PlayerState, PlayerState+Nome)
	//   barato : um find() no PlayerStatsMap, que ja' esta' em memoria
	//
	// E o chamador tratava as duas como uma so': `if (!PlayerState ||
	// Name.empty()) RefreshIdentity();`. Depois que o ator tem nome, o
	// RefreshIdentity NUNCA MAIS rodava — e Kills, TeamNumber e DamageDealt
	// ficavam CONGELADOS no primeiro valor lido.
	//
	// Pior: se aquela primeira leitura aconteceu antes do roster popular,
	// TeamNumber ficava 0 PARA SEMPRE, e o filtro de time nao funcionava
	// naquele jogador.
	//
	// Separando, o barato roda TODO TICK (e' um lookup em mapa) e o caro
	// continua rodando so' quando falta identidade.
	// ---------------------------------------------------------------------
	void RefreshStats();

	std::wstring Name = LIT(L"Entity");
	UEVector UEPosition;
	UEVector LastGoodPosition;  // Cache for surviving transient zero-reads
	Vector3 Position;
	Vector3 Velocity;

	// Grenade ESP fields (only valid when PlayerType == 3)
	float TimeTillExplosion = 0.0f;
	uint32_t ExplodeState = 0;
	float ExplosionRadius = 0.0f;  // Hardcoded per grenade type (in Unreal Units)
	std::chrono::steady_clock::time_point SpawnTime;
	bool SpawnTimeSet = false;

	// ---- DBNO / knocked ------------------------------------------------
	bool bIsDBNO = false;         // deitado/rastejando (knocked)
	float GroggyHealth = 0.0f;    // vida enquanto deitado (0-100)

	// ---- Arma equipada --------------------------------------------------
	std::string WeaponName;       // nome da arma atual
	int Ammo = -1;                // municao no pente (-1 = desconhecido)
	uint64_t WeaponAddr = 0;      // ponteiro do ator da arma equipada
	// -----------------------------------------------------------------
	// WeaponProcessor FRESCO — lido por UpdatePlayers (ja' sai dentro da
	// janela grande de Class, nao custa op a parte). UpdateWeapons roda
	// logo depois, no MESMO tick e mesma thread, e reusa o valor em vez de
	// reler `Class + SDK.WeaponProcessor` para cada ator — era a duplicata
	// R1 do inventario de leituras: o mesmo qword lido duas vezes por
	// personagem por tick. WpFresh zera quando o valor e' consumido; se o
	// ator nao passou no gate do UpdatePlayers deste tick, fica false e o
	// UpdateWeapons le direto (fallback exato do comportamento antigo).
	// -----------------------------------------------------------------
	uint64_t WeaponProcessorFresh = 0;
	bool     WeaponProcessorFresca = false;

	// -----------------------------------------------------------------
	// WeaponNameCurto (28/10) — o nome DA ARMA com o prefixo BP_Weapon_/
	// Weapon_/BP_ e o sufixo _C JA' cortados, mantido pelo UpdateWeapons
	// SOMENTE quando o nome muda. Antes esse corte era refeito no ESP, a
	// cada frame, em um std::string LOCAL novo por ator (nome de arma tem
	// >16 chars -> alocacao de heap por ator por frame). O ESP agora le o
	// campo direto: zero copia, zero heap. Mesma exposicao sem lock da
	// WeaponName (mesmo padrao ja' aceito no projeto).
	// -----------------------------------------------------------------
	std::string WeaponNameCurto;

	// ---- Veiculo --------------------------------------------------------
	float VehHealth = 0.0f;
	float VehHealthMax = 0.0f;
	float VehFuel = 0.0f;
	float VehFuelMax = 0.0f;

	// ---- Loot / item no chao -------------------------------------------
	// ---- stats (GameState -> PlayerArray) ----
	int   Kills = -1;
	int   TeamNumber = 0;
	bool  bIsBot = false;      // PlayerState.bIsABot (flag nativa do jogo)
	float DamageDealt = 0.0f;

	std::string ItemName;
	uint32_t ItemIdValue = 0;

	ActorEntity(uint64_t address);
	void SetUp1();
	void SetUp2();
	void SetUp3();
	uint64_t GetClass();
	int GetPlayerRole();
	// Por REFERENCIA: devolver `Name` por valor custava uma alocacao de heap do
// wstring por ator por frame, so' para o chamador copiar de novo num
// `std::string` (PlayerEsp.cpp:507). O unico chamador usa o retorno na linha
// seguinte, entao a referencia nao fica pendurada.
std::wstring& GetName();
	Vector3 GetPosition();
	void UpdatePosition();
	void UpdateVelocity();
	UEVector GetBoneMatrix(FTransform bone);
	void UpdateBone();
};