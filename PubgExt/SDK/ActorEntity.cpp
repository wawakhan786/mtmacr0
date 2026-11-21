#include "pch.h"
#include "ActorEntity.h"
#include "Camera.h"
#include "Globals.h"
FMatrix FTransform::ToMatrixWithScale()
{
	FMatrix m;

	m._41 = Translation.X;
	m._42 = Translation.Y;
	m._43 = Translation.Z;

	float x2 = Rotation.X + Rotation.X;
	float y2 = Rotation.Y + Rotation.Y;
	float z2 = Rotation.Z + Rotation.Z;

	float xx2 = Rotation.X * x2;
	float yy2 = Rotation.Y * y2;
	float zz2 = Rotation.Z * z2;
	m._11 = (1.0f - (yy2 + zz2)) * Scale3D.X;
	m._22 = (1.0f - (xx2 + zz2)) * Scale3D.Y;
	m._33 = (1.0f - (xx2 + yy2)) * Scale3D.Z;


	float yz2 = Rotation.Y * z2;
	float wx2 = Rotation.W * x2;
	m._32 = (yz2 - wx2) * Scale3D.Z;
	m._23 = (yz2 + wx2) * Scale3D.Y;


	float xy2 = Rotation.X * y2;
	float wz2 = Rotation.W * z2;
	m._21 = (xy2 - wz2) * Scale3D.Y;
	m._12 = (xy2 + wz2) * Scale3D.X;


	float xz2 = Rotation.X * z2;
	float wy2 = Rotation.W * y2;
	m._31 = (xz2 + wy2) * Scale3D.Z;
	m._13 = (xz2 - wy2) * Scale3D.X;

	m._14 = 0.0f;
	m._24 = 0.0f;
	m._34 = 0.0f;
	m._44 = 1.0f;

	return m;
}


FMatrix FMatrix::MatrixMultiplication(const FMatrix& other)
{
	FMatrix NewMatrix;

	NewMatrix._11 = this->_11 * other._11 + this->_12 * other._21 + this->_13 * other._31 + this->_14 * other._41;
	NewMatrix._12 = this->_11 * other._12 + this->_12 * other._22 + this->_13 * other._32 + this->_14 * other._42;
	NewMatrix._13 = this->_11 * other._13 + this->_12 * other._23 + this->_13 * other._33 + this->_14 * other._43;
	NewMatrix._14 = this->_11 * other._14 + this->_12 * other._24 + this->_13 * other._34 + this->_14 * other._44;
	NewMatrix._21 = this->_21 * other._11 + this->_22 * other._21 + this->_23 * other._31 + this->_24 * other._41;
	NewMatrix._22 = this->_21 * other._12 + this->_22 * other._22 + this->_23 * other._32 + this->_24 * other._42;
	NewMatrix._23 = this->_21 * other._13 + this->_22 * other._23 + this->_23 * other._33 + this->_24 * other._43;
	NewMatrix._24 = this->_21 * other._14 + this->_22 * other._24 + this->_23 * other._34 + this->_24 * other._44;
	NewMatrix._31 = this->_31 * other._11 + this->_32 * other._21 + this->_33 * other._31 + this->_34 * other._41;
	NewMatrix._32 = this->_31 * other._12 + this->_32 * other._22 + this->_33 * other._32 + this->_34 * other._42;
	NewMatrix._33 = this->_31 * other._13 + this->_32 * other._23 + this->_33 * other._33 + this->_34 * other._43;
	NewMatrix._34 = this->_31 * other._14 + this->_32 * other._24 + this->_33 * other._34 + this->_34 * other._44;
	NewMatrix._41 = this->_41 * other._11 + this->_42 * other._21 + this->_43 * other._31 + this->_44 * other._41;
	NewMatrix._42 = this->_41 * other._12 + this->_42 * other._22 + this->_43 * other._32 + this->_44 * other._42;
	NewMatrix._43 = this->_41 * other._13 + this->_42 * other._23 + this->_43 * other._33 + this->_44 * other._43;
	NewMatrix._44 = this->_41 * other._14 + this->_42 * other._24 + this->_43 * other._34 + this->_44 * other._44;

	return NewMatrix;
}
void GetIndex(Index& index, const std::string& objName)
{
	index.Head = 10;
	index.neck = 5;
	index.pelvis = 1;

	if (objName.find("Female") != std::string::npos) {
		// PlayerFemale bone indices
		index.Lshoulder = 95;
		index.Lelbow = 96;
		index.Lhand = 97;
		index.Rshoulder = 122;
		index.Relbow = 123;
		index.Rhand = 124;
		index.Lbuttock = 180;
		index.Lknee = 183;
		index.Lfoot = 181;
		index.Lball = 182;
		index.Rbuttock = 186;
		index.Rknee = 189;
		index.Rfoot = 187;
		index.Rball = 188;
	} else {
		// PlayerMale bone indices 
		index.Lshoulder = 88;
		index.Lelbow = 89;
		index.Lhand = 90;
		index.Rshoulder = 115;
		index.Relbow = 116;
		index.Rhand = 117;
		index.Lbuttock = 172;
		index.Lknee = 173;
		index.Lfoot = 174;
		index.Lball = 175;
		index.Rbuttock = 178;
		index.Rknee = 179;
		index.Rfoot = 180;
		index.Rball = 181;
	}
}
ActorEntity::ActorEntity(uint64_t address)
{
	Class = address;
	PlayerRole = 0;
	TempId = 0;
	id = 0;
	PlayerType = 0;
	Health = 100.0f;
	PlayerState = 0;
	RootComponent = 0;
	MeshLastTeamNum = 0;
	Mesh = 0;
	Mesh3P = 0;
	BoneArray = 0;
	AcknowledgedPawn = 0;
	RelativeLocation = 0;
	LastTeamNum = 0;

	// -----------------------------------------------------------------
	// OSSOS ZERADOS POR CONSTRUCAO.
	//
	// Estes tres UEVector ficavam SEM INICIALIZACAO neste construtor: o valor
	// so' aparecia depois de `SetUp3()`, que roda no fim do segundo passe do
	// `Engine::UpdatePlayers` e SO' quando o osso foi lido de verdade.
	//
	// O AIMBOT le' os tres direto. Sem osso lido, o valor era INDETERMINADO —
	// na pratica vinha (0,0,0) porque a memoria do alocador era nova, mas isso
	// e' coincidencia do alocador, nao garantia. Zerar aqui faz "nao lido" ser
	// um valor DETERMINISTICO, que e' exatamente o que `AimPointFor` (Aimbot.cpp)
	// testa antes de cair para a posicao do ator.
	// -----------------------------------------------------------------
	Head3D   = UEVector{};
	neck3D   = UEVector{};
	pelvis3D = UEVector{};

	isCheck = false;
	isDie = false; 
	
	if(!address || !IsAddrValid(address))
		return;
		
	// -----------------------------------------------------------------------
	// 0x800, NAO 0x500.
	//
	// `SDK.Mesh` virou 0x778 (era 0x300, colidia com CapsuleComponent — ver a
	// nota no Globals.h). Este buffer era de 0x500, e `buffer + 0x778` seria
	// leitura ~0x280 bytes FORA DO BUFFER: lixo de stack. Na maioria das vezes
	// nao estoura nada (so' le' memoria adjacente), mas e' bug — e o tipo de
	// bug que aparece como "as vezes o mesh e' um ponteiro aleatorio".
	//
	// Os outros campos lidos daqui (PlayerState 0x440, RootComponent 0x2B0,
	// ObjectID 0xC) cabiam em 0x500. Agora todos cabem com folga.
	// -----------------------------------------------------------------------
	uint8_t buffer[0x800];
	memset(buffer, 0, sizeof(buffer));
	TargetProcess.Read(Class, buffer, sizeof(buffer), "ActorBase");

	// -----------------------------------------------------------------------
	// IDENTIDADE — PlayerState + nome + stats do roster.
	//
	// SAIU DAQUI (era inline neste construtor) e virou `RefreshIdentity()`.
	//
	// MOTIVO: estes campos eram lidos UMA VEZ, na criacao do ator. Se aquela
	// leitura perdesse a corrida — o pawn nasce antes de o PlayerState estar
	// ligado, ou o read falha transiente — o ator ficava "Player" para o RESTO
	// DA SESSAO: o objeto e' reaproveitado pelo ActorCache e nada mais re-lia.
	// Sintoma relatado: alguns inimigos com nome generico e outros certos.
	//
	// Agora o `Engine::UpdatePlayers` re-tenta enquanto a identidade estiver
	// faltando (com throttle por ator); o construtor so' faz a 1a tentativa.
	// -----------------------------------------------------------------------
	RefreshIdentity();
	RootComponent = *(uint64_t*)(buffer + SDK.RootComponent);
	if (RootComponent) RootComponent = EngineInstance->xe_decrypt(RootComponent);
	if (!IsAddrValid(RootComponent)) RootComponent = 0;
	
	Mesh = *(uint64_t*)(buffer + SDK.Mesh);
	if (!IsAddrValid(Mesh)) Mesh = 0;

	// -----------------------------------------------------------------------
	// DIAGNOSTICO: loga o RootComponent cru vs decifrado, e a posicao lida do
	// decifrado. Serve para distinguir: offset errado (pos=0 mas decRoot valido)
	// OU decrypt errado (decRoot parece valido mas e lixo).
	// -----------------------------------------------------------------------
	static bool loggedFirstRC = false;
	if (!loggedFirstRC) {
		loggedFirstRC = true;
		uint64_t rawRoot = *(uint64_t*)(buffer + SDK.RootComponent);
		UEVector posDec = {};
		if (RootComponent) TargetProcess.Read(RootComponent + SDK.ComponentLocation, &posDec, sizeof(posDec));
		LOG("[RC] Class=0x%llX rawRoot=0x%llX decRoot=0x%llX Mesh=0x%llX pos@dec=%.1f,%.1f,%.1f\n",
		    (unsigned long long)Class, (unsigned long long)rawRoot,
		    (unsigned long long)RootComponent, (unsigned long long)Mesh,
		    posDec.X, posDec.Y, posDec.Z);
	}

	id = *(int*)(buffer + SDK.ObjectID);
	
	RelativeLocation = SDK.ComponentLocation;
}
// ===========================================================================
// RefreshIdentity — PlayerState + nome + stats do roster.
//
// POR QUE ISTO E' UMA FUNCAO, E NAO CODIGO NO CONSTRUTOR (como era):
//
//   `PlayerState` e `Name` eram lidos UMA VEZ, na criacao do ator. Quando
//   aquela leitura perdia a corrida — o pawn nasce antes de o PlayerState
//   estar ligado, ou o read falha transiente — o ator ficava com o rotulo
//   generico ("Player") para o RESTO DA SESSAO: o objeto e' reaproveitado pelo
//   ActorCache (Engine::Cache) e nada mais relia aqueles campos.
//
//   O sintoma relatado e' a assinatura exata de uma CORRIDA, e nao de um
//   offset errado: ALGUNS inimigos com nome generico e OUTROS certos. Offset
//   errado erraria todos.
//
// O `Engine::UpdatePlayers` chama isto enquanto `!PlayerState || Name.empty()`,
// com throttle por ator. Custo zero quando a identidade ja' esta' completa.
//
// NUNCA APAGA: uma leitura ruim nao sobrescreve um PlayerState/nome que ja'
// estavam bons — so' o contrario disso e' que seria regressao.
// ===========================================================================
bool ActorEntity::RefreshIdentity()
{
	if (!Class || !EngineInstance) return false;

	// ---- PlayerState (pawn + SDK.PlayerState, decifrado) ----
	uint64_t ps = 0;
	if (!TargetProcess.Read(Class + SDK.PlayerState, &ps, sizeof(ps), "IdentityPS")) return false;
	if (!ps) return false;

	ps = EngineInstance->xe_decrypt(ps);
	if (!IsAddrValid(ps)) return false;

	PlayerState = ps;

	// ---- stats do GameState (kills / squad / dano) ----
	// O UpdatePlayerList le' GameState -> PlayerArray e guarda num mapa
	// PlayerState* -> {kills, team, dano}. Aqui so' casamos pelo ponteiro.
	//
	// SAIU DAQUI para o RefreshStats(): este bloco e' um find() em mapa, nao
	// uma leitura de memoria, e por isso NAO pode depender do guard de "falta
	// identidade" que gate o RefreshIdentity. Ver o comentario no .h.
	RefreshStats();

	// ---- nome real do jogador (FString dentro do PlayerState) ----
	//   FString = { wchar_t* Data; int Count; int Max; }
	//
	// VALIDACAO ESTRITA — este e' o fix do BSOD de 02/10.
	//
	// O bugcheck foi:
	//   0x50 PAGE_FAULT_IN_NONPAGED_AREA
	//   P1 = 0x0031002000730079   <- endereco acessado
	//   P2 = 0                        <- LEITURA
	//
	// P1 em UTF-16LE e'  "ys 1" :  79 00='y'  73 00='s'  20 00=' '  31 00='1'
	//
	// Ou seja: o campo `Data` da FString apontava para os PROPIOS CARACTERES da
	// string, nao para um buffer. Isso acontece quando o offset esta' errado ou
	// o PlayerState foi reciclado.
	//
	// O check antigo era so' `fs.Data > 0x10000` — e 0x...79 passa nele.
	// Desreferenciar isso em ring0 da' 0x50, que NAO e' excecao: e' bugcheck
	// imediato, nenhum __try/__except segura.
	//
	// Agora exigimos canonico x64 + alinhado a 8 + FString coerente.
	// 0x0031002000730079 & 7 == 1  ->  REJEITADO.
	struct FStringRaw { uint64_t Data; int32_t Count; int32_t Max; };
	FStringRaw fs = { 0, 0, 0 };
	if (!TargetProcess.Read(PlayerState + SDK.PlayerName, &fs, sizeof(fs), "PlayerNameFS"))
		return false;

	const bool ptrOk = IsCanonicalPtr(fs.Data) &&
	                   IsCanonicalRange(fs.Data, (uint64_t)fs.Count * sizeof(wchar_t) + 2);
	const bool lenOk = (fs.Count > 0 && fs.Count < 64 &&
	                    fs.Max >= fs.Count && fs.Max < 4096);

	if (!ptrOk || !lenOk) {
		// Loga 1x para sabermos que a validacao esta' pegando lixo.
		static bool loggedBadName = false;
		if (!loggedBadName) {
			loggedBadName = true;
			LOG("[SAFE] PlayerName rejeitado: Data=0x%llX Count=%d Max=%d (ptrOk=%d lenOk=%d)\n",
			    (unsigned long long)fs.Data, fs.Count, fs.Max, (int)ptrOk, (int)lenOk);
		}
		return false;
	}

	wchar_t wbuf[65] = { 0 };
	if (!TargetProcess.Read(fs.Data, wbuf, (size_t)fs.Count * sizeof(wchar_t), "PlayerNameData"))
		return false;

	wbuf[fs.Count] = 0;
	Name = wbuf;
	return !Name.empty();
}

// ---------------------------------------------------------------------------
// RefreshStats — o "barato": sem NENHUMA leitura de memoria, so' um find().
//
// Existe separado porque o custo dele e' ordens de grandeza menor que o do
// RefreshIdentity (dois reads de kernel), e os dois estavam presos ao MESMO
// guard. Consequencia do acoplamento: Kills, TeamNumber e DamageDealt
// congelavam no primeiro valor lido — e TeamNumber em 0 quebra o filtro de
// time.
//
// Chamado a cada tick de UpdatePlayers. Com ~100 atores e um mapa de ~100
// entradas, e' um find() logaritmico por ator por tick: custo irrelevante.
// ---------------------------------------------------------------------------
void ActorEntity::RefreshStats()
{
	if (!PlayerState || !EngineInstance) return;

	Kills = -1;
	TeamNumber = 0;
	DamageDealt = 0.0f;

	auto sit = EngineInstance->PlayerStatsMap.find(PlayerState);
	if (sit != EngineInstance->PlayerStatsMap.end() && sit->second.Valid) {
		Kills       = sit->second.Kills;
		TeamNumber  = sit->second.Team;
		DamageDealt = sit->second.Damage;
	}
}

void ActorEntity::SetUp1()
{
	// Deprecated
}

UEVector ActorEntity::GetBoneMatrix(FTransform bone)
{
		float bx = bone.Translation.X;
	float by = bone.Translation.Y;
	float bz = bone.Translation.Z;

	float x2 = ToWorld.Rotation.X + ToWorld.Rotation.X;
	float y2 = ToWorld.Rotation.Y + ToWorld.Rotation.Y;
	float z2 = ToWorld.Rotation.Z + ToWorld.Rotation.Z;

	float xx2 = ToWorld.Rotation.X * x2;
	float yy2 = ToWorld.Rotation.Y * y2;
	float zz2 = ToWorld.Rotation.Z * z2;

	float _11 = (1.0f - (yy2 + zz2)) * ToWorld.Scale3D.X;
	float _22 = (1.0f - (xx2 + zz2)) * ToWorld.Scale3D.Y;
	float _33 = (1.0f - (xx2 + yy2)) * ToWorld.Scale3D.Z;

	float yz2 = ToWorld.Rotation.Y * z2;
	float wx2 = ToWorld.Rotation.W * x2;
	float _32 = (yz2 - wx2) * ToWorld.Scale3D.Z;
	float _23 = (yz2 + wx2) * ToWorld.Scale3D.Y;

	float xy2 = ToWorld.Rotation.X * y2;
	float wz2 = ToWorld.Rotation.W * z2;
	float _21 = (xy2 - wz2) * ToWorld.Scale3D.Y;
	float _12 = (xy2 + wz2) * ToWorld.Scale3D.X;

	float xz2 = ToWorld.Rotation.X * z2;
	float wy2 = ToWorld.Rotation.W * y2;
	float _31 = (xz2 + wy2) * ToWorld.Scale3D.Z;
	float _13 = (xz2 - wy2) * ToWorld.Scale3D.X;

	float wx = bx * _11 + by * _21 + bz * _31 + ToWorld.Translation.X;
	float wy = bx * _12 + by * _22 + bz * _32 + ToWorld.Translation.Y;
	float wz = bx * _13 + by * _23 + bz * _33 + ToWorld.Translation.Z;

	return UEVector(wx, wy, wz);
}

void ActorEntity::SetUp2()
{
	// Deprecated
}
void ActorEntity::SetUp3()
{
	Head3D = GetBoneMatrix(Head);
	neck3D = GetBoneMatrix(neck);
	pelvis3D = GetBoneMatrix(pelvis);
	Lshoulder3D = GetBoneMatrix(Lshoulder);
	Lelbow3D = GetBoneMatrix(Lelbow);
	Lhand3D = GetBoneMatrix(Lhand);
	Rshoulder3D = GetBoneMatrix(Rshoulder);
	Relbow3D = GetBoneMatrix(Relbow);
	Rhand3D = GetBoneMatrix(Rhand);
	Lbuttock3D = GetBoneMatrix(Lbuttock);
	Lknee3D = GetBoneMatrix(Lknee);
	Lfoot3D = GetBoneMatrix(Lfoot);
	Lball3D = GetBoneMatrix(Lball);
	Rbuttock3D = GetBoneMatrix(Rbuttock);
	Rknee3D = GetBoneMatrix(Rknee);
	Rfoot3D = GetBoneMatrix(Rfoot);
	Rball3D = GetBoneMatrix(Rball);
}
int ActorEntity::GetPlayerRole()
{
	return PlayerRole;
}

uint64_t ActorEntity::GetClass()
{
	return Class;
}

std::wstring& ActorEntity::GetName()
{
	return Name;
}

Vector3 ActorEntity::GetPosition()
{
	Position = Vector3(UEPosition.X, UEPosition.Y, UEPosition.Z);
	return Position;
}

// TODO
void ActorEntity::UpdatePosition()
{
	if (!Class || !RootComponent || !isCheck || !IsAddrValid(RootComponent))
		return;

	if (!TargetProcess.Read(RootComponent + RelativeLocation, &UEPosition, sizeof(UEVector), "RelativeLocation")) {
	}
}
void ActorEntity::UpdateVelocity()
{
	if (!Class || !RootComponent || !isCheck || !IsAddrValid(RootComponent))
		return;
	
	if (!TargetProcess.Read(RootComponent + SDK.ComponentVelocity, &Velocity, sizeof(Vector3), "ComponentVelocity")) {
	}
}
void ActorEntity::UpdateBone()
{
	if (!Class)
		return;
	if (!RootComponent)
		return;
	if (!PlayerState)
		return;
	if (!isCheck) // players aren't pawns
		return;
	if (Mesh < 65535) {
		return;
	}
	

	
}
