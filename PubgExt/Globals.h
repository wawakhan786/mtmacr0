#pragma once
#include "Engine.h"
// Catalogo de loot da aba WORLD (G_LootCatalog + categorias). Fica aqui pra'
// Engine.cpp, PlayerEsp.cpp e Main.cpp enxergarem o mesmo catalogo sem
// include extra em cada um.
#include "ESP/LootCatalog.h"

extern std::shared_ptr<Engine> EngineInstance;
extern std::string ProcessName;

// ===========================================================================
// IsAddrValid — validacao de ponteiro usada em TODO o projeto.
//
// O bugcheck 0x50 (PAGE_FAULT_IN_NONPAGED_AREA) de 02/10 teve:
//     P1 = 0x0031002000730079     <- endereco acessado
//     P2 = 0                      <- LEITURA
//
// P1 em UTF-16LE e'  "ys 1"  (79 00 73 00 20 00 31 00) — ou seja, o alvo
// continha TEXTO, nao um endereco. Isso acontece quando lemos um campo de
// string com o offset errado, ou quando o objeto de origem foi reciclado.
//
// A versao antiga era:
//     (ptr >= 0x100000 && ptr < CachedBaseAddress)
// e 0x0031002000730079 PASSA nela (esta' abaixo da base da imagem). O driver
// desreferenciava isso em ring0 -> 0x50, que NAO e' excecao: e' bugcheck
// imediato. Nenhum __try/__except segura (esta' documentado no proprio
// km_driver_source.c).
//
// Agora exigimos tambem:
//   - alinhamento a 8  (objetos UE4 sao sempre 8-alinhados;
//                       0x...79 & 7 == 1 -> rejeitado)
//   - canonicidade x64 (bits 48..63 tem que assinar o bit 47; endereco
//                       nao-canonico e' lixo de leitura, nao ponteiro)
//
// Escrito inline (sem chamar helper) porque e' macro expandida em muitos
// pontos e nao queremos depender da ordem de include.
// ===========================================================================
#define IsAddrValid(ptr) ( \
    EngineInstance && \
    EngineInstance->CachedBaseAddress && \
    (uintptr_t)(ptr) >= 0x100000 && \
    (uintptr_t)(ptr) < EngineInstance->CachedBaseAddress && \
    (((uintptr_t)(ptr) & 7) == 0) && \
    ((((uintptr_t)(ptr) >> 47) == 0) || (((uintptr_t)(ptr) >> 47) == 0x1FFFF)) \
)

struct _Local
{
	int Teamid;
	int SpectatedCount;
	bool reset;
	Vector3 Recoil;
	std::map<int, std::string> KV;
}inline Local;

// ===========================================================================
// OFFSETS
//
// Build de referencia: TslGame 2610.1.2.7
//
// AINDA NAO VALIDADA EM JOGO: a lista veio do Rei em 28/10/2026 e o usuario
// avisou que os offsets ainda nao foram validados. Os valores antigos eram
// da 2610.1.1.71 e ficam registrados nos comentarios como "era ...".
//
// FONTE DA VERDADE desta build: _offsets\build_2610.1.2.7.txt (lista do
// Rei). Todos os valores daqui vieram dela. O que NAO veio esta' marcado
// com `// NAO ATUALIZADO nesta build (nao veio na lista 2609.1.3.5)` e ficou
// com o valor antigo de proposito: offset velho marcado e' melhor que offset
// velho silencioso.
//
// ATENCAO: a DECIFRAGEM DE NOME e os OFFSETS tem que ser do MESMO build.
// Trocar metade quebra mais do que conserta: sem os keys certos o GetNames
// nao resolve nome nenhum e o ESP fica VAZIO (nome -> IsPlayer -> atores).
// Ver Engine::DecryptCIndex e SDK.XenuineDecrypt.
//
// ATENCAO: o `PubgExt\offsetdump.txt` deste repo e de um build ANTERIOR e
// diverge em 33 de 39 offsets. Nao use ele como referencia; use este arquivo.
// Rode `python tools\dump_driver\offset_audit.py --dump _offsets\build_2610.1.2.7.txt`
// para conferir o codigo contra a lista da build.
// (A unica divergencia esperada: RecoilValueVector — o codigo usa 0x11F8,
// medido no jogo; a lista traz 0x11B0 de novo. Ver o campo na secao weapon.)
//
// Os tres simbolos abaixo sao RVA dentro do modulo do jogo. Ha' mais alguns
// RVA espalhados pelos blocos (GObjects, GObjectsCount, PhysxSDK) marcados
// com "(RVA no modulo)". O resto e deslocamento de campo.
// ===========================================================================
struct OFFSET {
	// ---- base do modulo (RVA) ------------------------------------------
	// build 2611.1.1.37 (AINDA NAO VALIDADA EM JOGO)
	uint64_t UWorld = 0x140DB078;            // 2611.1.1.37
	uint64_t GNames = 0x14394070;            // 2611.1.1.37
	uint64_t XenuineDecrypt = 0x122EF828;    // 2611.1.1.37

	// =====================================================================
	// PhysX: investigado e REMOVIDO do cheat (build 2609.1.3.1).
	//
	// A cadeia foi PROVADA no jogo — base + 0x123BE088 -> NpPhyscis* ->
	// mSceneArray (+0x8) -> NpScene -> mSQManager (+0x2D0) -> os dois pruners
	// de heap — mas o PruningPool (a lista de {Shape, Actor}, sem a qual nao
	// ha' geometria nem raycast) NAO existe nos offsets conhecidos deste
	// build: todos os candidatos reprovaram (pares 0/16) ou eram falsos
	// positivos fisicamente impossiveis.
	//
	// Sem pool, um visible check sairia de dado nao confiavel, e feature que
	// mente e' pior que feature ausente. O usuario tambem pediu para o cheat
	// NAO ler memoria do PhysX. Os offsets sairam daqui de proposito e NAO ha'
	// mais nenhuma leitura de PhysX no codigo.
	//
	// A cadeia completa, o custo medido, o que falhou e o caminho de retomada
	// estao em ARQUITETURA.md, secao
	// "VISIBLE CHECK VIA PHYSX — INVESTIGADO E REMOVIDO (build 2609.1.3.1)".
	// =====================================================================

	uint64_t GNames_offset = 0x0;            // GNamesPtr
	uint32_t ElementsPerChunk = 0x4184;      // ChunkSize 2610.1.2.9 (era 0x4010 na .71; listas 2610.1.2.7 trazem 0x4040)

	// ---- decrypt de indice de nome (FName) -----------------------------
	// Ver Engine::DecryptCIndex
	//
	// FORMULA DESTA BUILD (2610.1.2.7 — AINDA NAO VALIDADA EM JOGO) — veio do
	// DUMP do Rei, nao de deducao. A lista traz DOIS nomes para a MESMA
	// funcao (equivalencia bit-a-bit demonstrada em
	// _offsets\_check_2610_1_2_7.py):
	//
	//     #define XE_INDEX(v) (((((v)>>10)|((v)<<22)) ^ ((((v))<<6)&0xFFFF0000U)) ^ 0x821E5EADU)
	//     uint32_t XeDecryptCIndex2(uint32_t value) {
	//         uint32_t xored = value ^ 0x8EF87ACEU;
	//         return ((xored << 6) & 0xFFFF0000U) ^ Ror32(xored, 10) ^ 0x8FA3E0B3U;
	//     }
	//
	// Sem XOR inicial visivel => K1 = 0; o termo direito da forma sumiu
	// (Shr = MaskRight = 0). ((v>>10)|(v<<22)) e' o rotr10 completo (termos
	// de bits disjuntos — nesse ponto OR == XOR). Rank GF(2) = 32/32:
	// BIJETIVA, entao o autoteste volta a passar sem falso alarme.
	//
	// Cada campo abaixo diz o que FAZ (nao o nome que a lista usa). Mapa
	// lista(dump) -> campo daqui, em _offsets\build_2610.1.1.57.txt:
	//
	//     DecryptNameIndexXorKey1 = 0x3F346DAB -> NameIndexXor1      (XOR inicial)
	//     DecryptNameIndexDval    = 0x2        -> NameIndexShl       (SHIFT ESQUERDA)
	//     DecryptNameIndexShlMask = 0xFFFF0000 -> NameIndexShlMask   (MASCARA do termo da ESQUERDA)
	//     DecryptNameIndexSval    = 0x0        -> NameIndexShr       (SHIFT DIREITA)
	//     DecryptNameIndexXorKey3 = 0x0        -> NameIndexMaskRight (MASCARA do termo da direita)
	//     DecryptNameIndexRval    = 0xE = 14   -> NameIndexRotr      (ROTACAO a direita)
	//     DecryptNameIndexXorKey2 = 0x9531CE38 -> NameIndexXor2      (XOR final)
	//     DecryptNameIndexRor     = 0x1        -> NameIndexRor       (NAO usado; ver o campo)
	//
	// ===========================================================================
	// A FORMA E' ESTAVEL ENTRE BUILDS. SO' AS CONSTANTES MUDAM.
	// ===========================================================================
	//
	//     xored  = value ^ XorKey1
	//     left   = (xored << Dval) & ShlMask
	//     right  = (xored >> Sval) & XorKey3
	//     return (left | right) ^ _rotr(xored, Rval) ^ XorKey2
	//
	// As builds que a gente tem caem na MESMA forma:
	//
	//   2609:        K1=0xB753B1E8  Dval=19  ShlMask=inofensivo  Sval=13  Key3=0x70000   Rval=29  K2=0x0CAFC916
	//   2610.1.1:    K1=0x3F346DAB  Dval=2   ShlMask=0xFFFF0000  Sval=0   Key3=0x0       Rval=14  K2=0x9531CE38
	//   2610.1.1.71: K1=0x46BAE3B9  Dval=11  ShlMask=0xFFFF0000  Sval=0   Key3=0x0       Rval=5   K2=0x54D51E55
	//   2610.1.2.7:  K1=0x00000000  Dval=6   ShlMask=0xFFFF0000  Sval=0   Key3=0x0       Rval=10  K2=0x821E5EAD
	//
	// HISTORICO DO ERRO — DUAS VEZES, E A LICAO E' A MESMA:
	//
	//   1) Na 2609 a leitura usava `NameIndexRotr` para o `Ror`=1, tratava o
	//      `Rval`=0x1D como "nao usado" e DEDUZIA `shl = (48 - rotr) & 31 = 15`
	//      de um invariante suposto. O resultado era uma funcao BIJETIVA (rank
	//      32/32 — o autoteste dizia "ok") e ERRADA: os indices de FName
	//      resolviam para memoria aleatoria, os nomes de classe saiam como
	//      bytecode x86 (`cls='WH...H‹ÚH...'`) e o `IsPlayer` rejeitava TODOS
	//      os atores -> `[STATE] Actors=0` sempre.
	//
	//   2) AINDA na 2609, o `ShlMask` foi REMOVIDO com a justificativa "nao
	//      existe nesta formula" — porque com `<< 19` ele era, de fato,
	//      inofensivo. Isso valia para AQUELES VALORES, nao para a FORMA.
	//      Na 2610 o shift virou `<< 2`, e a mascara passou a ser a diferenca
	//      entre o termo certo (bits 16..31) e um com 14 bits a mais. Sem ela,
	//      o sintoma seria IDENTICO ao de (1): ESP vazio.
	//
	// LICAO DAS DUAS: campo que faz parte da FORMA fica, mesmo parecendo
	// morto para os valores de hoje. "Inutil" quase sempre significa "inutil
	// para esta build" — e a proxima build cobra a conta.
	// ===========================================================================
	uint64_t NameIndexXor1 = 0x4A8AC78B;     // ? NEW
	uint64_t NameIndexXor2 = 0x8210BFD7;     // ? NEW
	uint32_t NameIndexShl = 1;               // ? NEW (Dval = 1)
	uint32_t NameIndexShlMask = 0xFFFF0000;
	uint32_t NameIndexShr = 0;
	uint32_t NameIndexMaskRight = 0;
	uint32_t NameIndexRotr = 15;             // ? NEW (Rval = 15)
	uint32_t NameIndexRor = 0x1;

	// =====================================================================
	// Demais decodificadores da build 2610.1.2.7 — APENAS REGISTRO.
	// NENHUM e' lido pelo cheat hoje (o unico em uso e' o DecryptCIndex acima;
	// os offsets UObject_InternalIndex/FNameNumber/ClassPrivate tambem
	// nao tem leitor). Ficam aqui para a proxima build: comparar e trocar.
	// =====================================================================
	//   XeDecryptInternalIndex(v):  rotr8(v)  ^ ((v<<8)&0xFFFF0000)    ^ 0xB95718C9
	//   XeDecryptFNameNumber(v):    rotr14(v) ^ ((v&0xFFFFC000)<<2)    ^ 0xB8CADE10
	//   XeDecryptClassPrivate(enc): v ^= 0x9E8291460D982D5C
	//                               rotr64(v,22) ^ ((v&0xFFFFFFFFFFC00000)<<10) ^ 0x3B392DDDAEF654F7

	// ---- objeto / mundo -------------------------------------------------
	uint32_t ObjectID = 0xC;                 // 2611
	uint32_t CurrentLevel = 0x170;           // 2611
	uint32_t GameInstance = 0x168;           // 2611: UGameEngine::GameInstance (NOT UWorld)
	uint32_t LocalPlayers = 0xF8;             // 2611`r`n`tuint32_t LocalPlayers = 0xF8;            // alias of LocalPlayer (array data ptr — same offset)
	uint32_t Actors = 0x138;   // 2611.1.1.37
	uint32_t TimeSeconds = 0x948;            // 2611
	uint32_t WorldOrigin = 0x38;             // NAO ATUALIZADO nesta build (nao veio na lista 2609.1.3.5)

	// ---- nivel / gamestate ----------------------------------------------
	uint32_t GameState = 0x950;              // 2611
	// LevelScriptActor 0x150 era FANTASMA: veio na transcricao de 27/10 mas NAO
	// esta na lista CRUA do Rei (colada 28/10) — campo removido, sem leitor.
	uint32_t NumAliveTeams = 0x540;
	uint32_t PlayerArray = 0x420;

	// ---- controller / camera --------------------------------------------
	uint32_t PlayerController = 0x38;        // 2611
	uint32_t AcknowledgedPawn = 0x4B0;       // 2611
	uint32_t PlayerCameraManager = 0x4D8;    // 2611

	// Os tres campos abaixo sao lidos a partir de PlayerCameraManager
	// (Engine::RefreshViewMatrix le PlayerCameraManager + CameraFov/Pos/Rot).
	// Build 2609.1.3.5: a lista traz CameraCacheFOV=0xA78,
	// CameraCacheLocation=0x101C e CameraCacheRotation=0x1010.
	// ATENCAO: o RCS usa o FOV lido daqui — 0xA78 no lugar do 0x10C0 antigo.
	uint32_t CameraFov = 0xAEC;               // = CameraCacheFOV (2611)
	uint32_t CameraPos = 0xAC4;              // = CameraCacheLocation (2611)
	uint32_t CameraRot = 0x1074;              // = CameraCacheRotation (2611)

	uint32_t ViewTarget = 0xAB0;
	uint32_t CameraCacheFOV = 0xAEC;         // 2611
	uint32_t CameraCacheRotation = 0x1074;   // 2611
	uint32_t CameraCacheLocation = 0xAC4;    // 2611

	// ---- componente / transform -----------------------------------------
	uint32_t ComponentToWorld = 0x210;       // 2611
	uint32_t ComponentLocation = 0x220;      // 2611
	uint32_t ComponentVelocity = 0x2AC;      // 2611 lista 2609.1.3.5: 0x2E0 (era 0x354, de build antiga)
	uint32_t RootComponent = 0x150;          // 2611
	// -----------------------------------------------------------------------
	// Mesh — O COMPONENTE DE MESH DO PERSONAGEM.
	//
	// ERA 0x300, que e' o MESMO valor de `CapsuleComponent` na lista da build
	// (os dois nao podem estar em 0x300). Resultado medido no logs.txt:
	//
	//     [HP]  Health=100.000 BoneArray=0x0 bHasBones=0 mesh=0x0
	//     [VIS] LOCAL mesh=0x0 INVALIDO - leitura pulada (hipotese 1)
	//
	// Com Mesh=0, o meshBulk nunca era lido, o BoneArray ficava 0,
	// `bHasBones` ficava false e o ESQUELETO NAO DESENHAVA. Tambem deixava o
	// AIMBOT sem osso — ele caia no fallback e mirava no PE' do inimigo.
	//
	// A lista oficial da build 2609.1.3.5 traz Mesh = 0x778. Corrigido.
	// -----------------------------------------------------------------------
	uint32_t Mesh = 0x500;                   // 2611
	uint32_t StaticMesh = 0xAF8;               // 2610.1.2.7 confirma (mesmo valor)
	uint32_t SkeletalMesh = 0xAE8;             // 2610.1.2.7 (novo — USkeletalMeshComponent do ACharacter)
	uint32_t Eyes = 0x76C;                     // 2610.1.2.7 (era 0x750 — MUDOU 4 bytes; 0x750 voltou pro LastRenderTimeOnScreen)
	// -----------------------------------------------------------------------
	// LastTimeScreenRenderered — O SINAL DE VISIBILIDADE DA BUILD ATUAL.
	//
	// MEDIDO por quem joga: e' o MESMO slot do `Eyes` (0x750) logo acima, lido
	// a partir da CLASS do pawn. `Eyes` e' o offset de TslCharacter que ja'
	// existia; o campo que mora ali na build 2610.1.1.65 e' o
	// `LastTimeScreenRenderered` — um float em SEGUNDOS de tempo de mundo,
	// escrito em TODO frame em que o pawn foi desenhado.
	//
	// POR QUE ISSO E' DIFERENTE DO 0x75C QUE FALHOU (ver o bloco VISIBILIDADE
	// logo abaixo): o 0x75C era lido do PONTEIRO DO MESH e nunca avancava para
	// inimigo. Este e' lido da CLASSE do ator — o mesmo endereco de onde o
	// `Mesh` sai — e o valor e' comparado com `TimeSeconds` do GameState, na
	// MESMA base de tempo.
	//
	// A CONTA (Engine::UpdatePlayers):
	//     atraso_ms = (worldTime - lastRender) * 1000
	//     visivel   = atraso_ms <= Configs.Survivor.VisToleranceMs
	//
	// O sentinela medido no 0x75C tambem vale aqui: -1000.0 = "nunca
	// desenhado" (delta gigante -> invisivel, que e' a resposta certa).
	// -----------------------------------------------------------------------
	uint32_t LastTimeScreenRenderered = 0x750;
	uint32_t CharacterMovement = 0x598;  // SDK 2611 line 515
	uint32_t LastUpdateVelocity = 0x3D0;
	uint32_t ReplicatedMovement = 0x60;
	uint32_t bAlwaysCreatePhysicsState = 0x488;

	// =====================================================================
	// VISIBILIDADE / RENDER
	//
	// FONTE (medida, nao chute): _lixo\_offsets\offsets_gobjects_objects.txt
	//   GObjects[38999] FName->Engine.PrimitiveComponent.*fa48f94705 extra->0x758
	//   GObjects[38998] FName->Engine.PrimitiveComponent.*8d5122ee25 extra->0x75C
	//
	// Os dois ficam imediatamente depois de
	//   GObjects[35430-area] Engine.PrimitiveComponent.BoundsScale extra->0x754
	// e antes de CanBeCharacterBase (0x764) — que e' exatamente onde o
	// UPrimitiveComponent do UE4 guarda os dois floats de render:
	//
	//   LastSubmitTime         (0x758) avanca em TODO frame em que o primitivo
	//                                  e' SUBMETIDO a cena. Basta estar no
	//                                  frustum: parede na frente nao importa.
	//   LastRenderTimeOnScreen (0x75C) avanca so' quando ele foi DESENHADO.
	//
	// O codigo antigo lia 0x758 nos DOIS lados da comparacao (LastRenderTime-
	// OnScreen e LastSubmitTime apontavam os dois pra 0x758). Comparar
	// LastSubmitTime com LastSubmitTime da' sempre "quase igual" -> todo mundo
	// visivel -> a cor nunca mudava. Era esse o bug.
	//
	// Os nomes das propriedades vem HASHED no dump (`*fa48f94705`), o que e'
	// coerente com o PUBG esconder justamente os campos uteis pra cheat. A
	// posicao (logo apos BoundsScale) e a ordem batem com o UE4.
	// =====================================================================
	// VISIBILIDADE — O QUE FOI MEDIDO NO JOGO (build 2609.1.3.1)
	//
	// O diagnostico [VIS] logou os dois campos para 8 atores, 1x/s, numa
	// partida real. Resultado MEDIDO:
	//
	//     ator 171m   ren(0x75C) =  7.619    <- CONGELADO em todas as leituras
	//     ator  33m   ren(0x75C) = 10.521    <- CONGELADO
	//     ator  15m   ren(0x75C) = 28.168    <- SO' o player local avanca
	//     maximo     28.168 -> 29.169 -> 30.172     (~1/s)
	//
	// 0x75C NAO avanca para inimigo -> atraso ~20000ms -> vis=0 pra TODOS ->
	// a cor trava num valor so'. O sinal NAO discrimina visibilidade.
	//
	// A ordem dos dois campos era INFERIDA da posicao no layout (o autor
	// avisou: "os nomes vem hashed no dump, e' inferencia, nao certeza").
	// A inferencia estava ERRADA.
	//
	// O que a medicao deu de REAL:
	//   - existe um SENTINELA: ren = -1000.000 = "nunca desenhado"
	//   - scn(0x22D) difere (CF perto / 97 longe), mas correlaciona com
	//     DISTANCIA, nao com parede -> e' LOD/cull-distance, nao oclusao
	//
	// CONCLUSAO: nao ha sinal de oclusao legivel na memoria do objeto.
	// Oclusao no UE4 e' transitoria, vive no renderer (HZB / occlusion
	// query) e NAO e' UPROPERTY — nao tem onde estar.
	//
	// O que daria precisao real (projeto a parte, NAO e' leitura de memoria):
	// ler o DEPTH BUFFER via DXGI e comparar a profundidade na posicao de
	// tela do inimigo. Ou o raycast do jogo, que exigiria injecao.
	//
	// Ate' existir um sinal medido, o ESP deve ficar com UMA cor (fail-open).
	// =====================================================================
	// (Os quatro offsets desta secao — 0x758, 0x75C, 0xBE8 e 0x22D — foram
	//  REMOVIDOS junto com o diagnostico [VIS] da MEDICAO #1: ver o bloco
	//  logo abaixo.
	//
	//  NA VERSAO ATUAL os offsets do mesh VOLTARAM (0x754, 0x758, 0x75C,
	//  0x760, 0x764 e 0x22D) por causa da MEDICAO #2 — o usuario quer
	//  retestar a visibilidade. Eles alimentam SO' o log de diagnostico
	//  atras de DebugLogs; NADA no ESP depende deles (IsVisible continua
	//  true, fail-open). O 0xBE8 continua fora: a medicao #1 provou que e'
	//  padding. Ver "MEDICAO #2" no fim desta secao.)
	// =====================================================================

	// ---------------------------------------------------------------------
	// 0xBE8 NAO ERA bRecentlyRendered (e por isso nao virou campo).
	//
	// O dump mapeia a vizinhanca inteira do USkinnedMeshComponent:
	//   GObjects[39269] Engine.SkinnedMeshComponent.CapsuleIndirectShadowMinVisibility extra->0xBE4
	//   GObjects[39177] Engine.SkinnedMeshComponent.bEnableUpdateRateOptimizations   extra->0xBF0
	// ou seja: 0xBE4 e' um float (ocupa 0xBE4..0xBE7) e o proximo campo so'
	// aparece em 0xBF0. 0xBE8..0xBEF sao 8 bytes de PADDING.
	//
	// Procurando no dump inteiro: `bRecentlyRendered` = 0 ocorrencias e
	// nenhuma propriedade de PrimitiveComponent/SkeletalMeshComponent cai em
	// 0xBE8. Ler ali devolvia lixo (padding), nao uma flag — e o unico
	// consumidor era o log [VIS] que existia pra provar isso.
	// ---------------------------------------------------------------------

	// Byte de bitfields do USceneComponent: bVisible e bHiddenInGame dividem
	// o MESMO byte.
	//   GObjects[38705] Engine.SceneComponent.bHiddenInGame extra->0x22D
	//   GObjects[38706] Engine.SceneComponent.bVisible      extra->0x22D
	// Nao e' oclusao (e' "o jogo escondeu o componente"), e nao serve pra
	// decidir a cor do ESP — o byte correlaciona com DISTANCIA (LOD/cull), nao
	// com parede (foi o que a medicao mostrou).
	// ---------------------------------------------------------------------

	// =====================================================================
	// O QUE ACONTECEU COM ESTES OFFSETS (leia antes de reintroduzir qualquer
	// leitura de visibilidade):
	//
	//   Ate' esta versao existiam aqui TRES offsets de render —
	//   LastSubmitTime (0x758), LastRenderTimeOnScreen (0x75C),
	//   RecentlyRendered (0xBE8) — mais o SceneVisibleBits (0x22D), todos
	//   consumidos pelo diagnostico "[VIS]" em Engine::UpdatePlayers.
	//
	//   Eles foram REMOVIDOS porque a medicao provou que nenhum deles
	//   discrimina oclusao. O comentario de MEDICAO acima e' o registro
	//   historico e FICA (foi ele que evitou, mais de uma vez, a gente
	//   "consertar" de novo um offset que nunca ia funcionar).
	//
	//   Se um dia alguem quiser reprovar: era o log [VIS] comparando
	//   ren(0x75C) de um inimigo atras de parede com o de um na frente. O
	//   resultado esta' acima. O que daria precisao real e' o DEPTH BUFFER via
	//   DXGI (projeto a parte), nao leitura de UPROPERTY.
	//
	//   >>> MEDICAO #2 (versao atual): o usuario pediu para retestar (ele
	//       acredita ter visto diferenca de cor antes). Os offsets do mesh
	//       voltaram a existir AQUI EMBAIXO, exclusivamente para o log
	//       `[VIS]` (que esta' de novo em Engine::UpdatePlayers, atras de
	//       DebugLogs). `IsVisible` continua true — ou seja, o ESP nao mudou
	//       de comportamento: o bloco novo so' MEDE e LOGA.
	// =====================================================================

	// =====================================================================
	// MEDICAO #2 — offsets do mesh (UPrimitiveComponent / USceneComponent)
	//
	// FONTE (o MESMO dump que originou a medicao #1, arquivo versionado no
	// repo): _lixo\_offsets\_vis_probe\components.txt
	//   GObjects[39000] Engine.PrimitiveComponent.BoundsScale          extra->0x754
	//   GObjects[38999] Engine.PrimitiveComponent.*fa48f94705          extra->0x758
	//   GObjects[38998] Engine.PrimitiveComponent.*8d5122ee25          extra->0x75C
	//   GObjects[38997] Engine.PrimitiveComponent.*aaf2196170          extra->0x760
	//   GObjects[38996] Engine.PrimitiveComponent.CanBeCharacterBase   extra->0x764
	//   GObjects[38995] Engine.PrimitiveComponent.CanCharacterStepUpOn extra->0x766
	//   GObjects[38993] Engine.PrimitiveComponent.*d78ca461c8          extra->0x768
	//   (SceneComponent, o mesmo byte para os dois:)
	//   GObjects[38705] Engine.SceneComponent.bHiddenInGame            extra->0x22D
	//   GObjects[38706] Engine.SceneComponent.bVisible                 extra->0x22D
	//
	// POR QUE ESTES: 0x754 (BoundsScale) e' um float CONHECIDO e estavel
	// (~1.0) — serve de ANCORA: se a ancora vier coerente e os vizinhos vierem
	// sem sentido, o problema e' o campo; se a ancora vier lixo, o problema e'
	// a leitura/o ponteiro. 0x758 e 0x75C sao os dois candidatos a
	// "ultimo submit" / "ultimo render on screen" (nomes hashed no dump, ou
	// seja: a ORDEM entre eles e' INFERENCIA — era uma das hipoteses abertas).
	// 0x760 e 0x764 entram como vizinhos medidos no mesmo batch.
	//
	// ATENCAO: `Eyes` (mais acima, tambem 0x760) e' offset de OUTRA struct
	// (TslCharacter). Coincidencia numerica: os campos abaixo sao lidos a
	// partir do PONTEIRO DO MESH. Nao misture os dois sentidos.
	// =====================================================================
	// -----------------------------------------------------------------------
	// OS TRES TEMPOS DO COMPONENTE — MEDIDOS COM O [VISCAN] EM PARTIDA
	// (19/10/2026, jogador correndo):
	//
	//   [TIME?] MESH +0x748  12.4056 -> 12.5524 -> 12.6946
	//   [TIME?] MESH +0x74C  12.4056 -> 12.5524 -> 12.6946
	//   [TIME?] MESH +0x750  12.4056 -> 12.5524 -> 12.6946
	//
	// TRES floats consecutivos, todos avancando ~125 ms por amostra — e' o
	// bloco de tempos do UPrimitiveComponent. Os valores anteriores (0x748 como
	// "submit" e 0x75C como "on screen") estavam DESLOCADOS: no 0x75C a leitura
	// e' ZERO nesta build, e a ancora BoundsScale em 0x754 tambem (o que provou
	// que a faixa inteira mudou de lugar, e nao que o campo estava morto).
	//
	// A ordem do bloco do UE4 e':
	//   0x748 LastSubmitTime          — submetido a cena (frustum; parede nao
	//                                   importa)
	//   0x74C LastRenderTime          — desenhado em QUALQUER passe (inclui
	//                                   shadow map: parede nao importa)
	//   0x750 LastRenderTimeOnScreen  — desenhado NA TELA (parede importa)
	//
	// O veredito usa o ULTIMO da lista que estiver VIVO (mudando a cada frame),
	// com fallback para os anteriores — ver Engine::UpdatePlayers. O [VIS] loga
	// os tres, e o menu mostra os tres: e' assim que se descobre qual deles
	// congela atras da parede, sem chute.
	// -----------------------------------------------------------------------
	uint32_t PrimBoundsScale = 0x754;          // NAO USAR: zero nesta build (fica como ancora historica)
	uint32_t LastSubmitTime = 0x748;           // MEDIDO: avanca todo frame
	uint32_t LastRenderTime = 0x74C;           // MEDIDO: avanca todo frame
	uint32_t LastRenderTimeOnScreen = 0x750;   // MEDIDO: avanca todo frame (candidato a oclusao)
	uint32_t PrimRenderCand760 = 0x760;        // candidato vizinho (nome hashed)
	uint32_t PrimRenderCand764 = 0x764;        // CanBeCharacterBase (bitfields)
	uint32_t SceneVisibleBits = 0x22D;         // bVisible / bHiddenInGame (mesmo byte)

	// ---- postura (RCS) ---------------------------------------------------
	// Achado no dump de offsets 2605.1.1.97:
	//     FName->Engine.Character.bIsCrouched   extra->0x508
	//
	// ATENCAO: o dump e' do 2605 e o nosso build e' 2609, e os offsets andam
	// (~8 bytes: bIsScoping_CP era 0x85D e virou 0x865). Por isso o RCS LOGA o
	// valor lido — antes de confiar, a gente confere no log que ele vira 1
	// quando agacha.
	// ---------------------------------------------------------------------
	uint32_t bIsCrouched = 0x508;            // NAO ATUALIZADO nesta build (nao veio na lista 2609.1.3.5)

	// ---- estado do personagem -------------------------------------------
	uint32_t CharacterName = 0x16B0;         // 2611
	uint32_t LastTeamNum = 0x1258;           // 2611
	uint32_t SpectatedCount = 0xFF4;        // 2611
	uint32_t PlayerState = 0x440;            // 2611 PlayerState (a lista traz 0x440 TAMBEM para PlayerStatusType)
	uint32_t PlayerStatistics = 0x498;
	uint32_t CharacterState = 0x11BC;
	uint32_t AnimScriptInstance = 0xE40;  // SDK 2611 SkeletalMeshComponent
	uint32_t SubInstances = 0xE28;
	uint32_t PreEvalPawnState = 0x650;
	uint32_t GroggyHealth = 0x1850;   // 2610.1.2.7 confirma (mesmo valor desde a 2609)
	uint32_t Gender = 0xB38;

	// ---- health (ver Engine::DecryptHealth e HealthDecipher) -------------
	// HeaFlag e a base: os campos sao lidos com offset relativo a ele.
	uint32_t HeaFlag = 0x258;               // ? 2611
	uint32_t Health1 = 0xA48;
	uint32_t Health2 = 0xA60;               
	uint32_t Health3 = 0x99C;
	uint32_t Health4 = 0x988;
	uint32_t Health5 = 0x99D;
	uint32_t Health6 = 0x998;

	uint64_t HealthXorKey0 = 0xCEC7A5AC;    // 
	uint64_t HealthXorKey1 = 0x9B63B29B;    // ? NEW
	uint64_t HealthXorKey2 = 0xCA886E0F;    // ? NEW
	uint64_t HealthXorKey3 = 0x9B384866;    // ? NEW
	uint64_t HealthXorKey4 = 0x88911D0A;    // ? NEW
	uint64_t HealthXorKey5 = 0x23DD4258;    // ? NEW
	uint64_t HealthXorKey6 = 0x094566C8;    // ? NEW
	uint64_t HealthXorKey7 = 0x0F372921;    // ? NEW
	uint64_t HealthXorKey8 = 0x0B427A58;    // ? NEW
	uint64_t HealthXorKey9 = 0x7BEF9B87;    // ? NEW
	uint64_t HealthXorKey10 = 0xE275588D;   // ? NEW
	uint64_t HealthXorKey11 = 0x728ADB51;   // ? NEW
	uint64_t HealthXorKey12 = 0xBD9390D5;   // ? NEW
	uint64_t HealthXorKey13 = 0x9B936607;   // ? NEW
	uint64_t HealthXorKey14 = 0x51099E38;   // ? NEW
	uint64_t HealthXorKey15 = 0xB4FA2AE5;   // ? NEW

	// ---- HUD / input -----------------------------------------------------
	uint32_t MyHUD = 0x4C8;
	uint32_t BlockInputWidgetList = 0x5C0;
	uint32_t WidgetStateMap = 0x548;
	uint32_t SelectMinimapSizeIndex = 0x558;
	uint32_t bShowMouseCursor = 0x658;
	uint32_t PlayerInput = 0x548;            // ATENCAO: mesmo 0x560 de WidgetStateMap na lista
	uint32_t InputAxisProperties = 0x130;
	uint32_t InputYawScale = 0x64C;

	// ---- weapon ----------------------------------------------------------
	uint32_t WeaponProcessor = 0xA40;
	uint32_t EquippedWeapons = 0x210;
	uint32_t CurrentWeaponIndex = 0x319;
	uint32_t WeaponTrajectoryData = 0x1230;
	uint32_t TrajectoryGravityZ = 0x1148;    // corrigido (dump completo)
	uint32_t TrajectoryConfig = 0x100;
	uint32_t BallisticCurve = 0x28;
	uint32_t FloatCurves = 0x30;
	uint32_t Keys = 0x60;
	uint32_t CurrentAmmoData = 0xB44;      // 2611
	// -----------------------------------------------------------------------
	// RecoilValueVector — o SENSOR do RCS.
	//
	// ===========================================================================
	// 0x11B0 -> 0x11F8. O VALOR DA LISTA ESTAVA ERRADO, E FOI MEDIDO.
	// ===========================================================================
	//
	// A lista da build 2610.1.1.57 trazia 0x11B0, e a gente aplicou. O sintoma
	// que apareceu no log foi este, em TODA amostra, durante toda a sessao:
	//
	//     [RCS] weapon=0x... rvv=(0.000,0.000) vr=0.577 | dropY=0.0000 ...
	//
	// Um ACUMULADOR de recuo que nunca sai de zero nao e' recuo: e' o offset
	// errado. E o efeito no RCS era silencioso e especifico — o `dropY` sai do
	// delta do `rvv`, entao ele ficava sempre 0, e a compensacao HORIZONTAL nao
	// tinha fonte nenhuma (dropX=0, degX=0 sempre). O RCS so' puxava pra baixo,
	// pelo `vr`, nunca pros lados.
	//
	// 0x11F8 veio de fonte externa (forum), e a distancia de 0x48 explica por
	// que os dois parecem plausiveis: sao campos VIZINHOS. Nao e' "quase o
	// mesmo offset" — e' outro campo.
	//
	// Se o `rvv` voltar a ler (0,0) SEM parar de atirar, o offset caiu de novo.
	// E' o primeiro lugar a olhar, e o teste e' uma linha de log.
	// -----------------------------------------------------------------------
	uint32_t RecoilValueVector = 0x11B0;     // ? SDK kehta hai (ya 0x11A0 test karo)   // MEDIDO no jogo (a lista da .57 trazia 0x11B0, errado).
	                                       // A lista 2610.1.2.7 traz 0x11B0 DE NOVO — manter 0x11F8 ate'
	                                       // o log provar o contrario ([RCS] rvv=(0,0) durante o tiro).
	                                       // A regiao ao redor (0x1140/0x1148/0x1230) nao mudou na lista,
	                                       // o que favorece o 0x11F8 continua certo.
	uint32_t VerticalRecovery = 0x11A8;
	uint32_t WeaponConfig_WeaponClass = 0x521;
	uint32_t Mesh3P = 0x810;
	uint32_t FiringAttachPoint = 0x8C0;
	uint32_t ScopingAttachPoint = 0x1018;    // 2611
	uint32_t ScopingSpringArm = 0xFF0;
	uint32_t AttachedStaticComponentMap = 0x14A0;  // 2610.1.2.7 (era 0x1508 — MUDOU)
	uint32_t AttachedItems = 0x858;          // = AttachedItems (a lista traz o MESMO 0x878 para DroppedItemGroupUItem)
	uint32_t WeaponAttachmentData = 0x290;
	uint32_t ElapsedCookingTime = 0xB40;        // 2610.1.2.7 (era 0xB80 — MUDOU)

	// ---- viewmodel / anim (CP = character pawn) --------------------------
	uint32_t AimOffsets = 0x18A8;
	uint32_t ControlRotation_CP = 0x674;     // ? NEW
	uint32_t RecoilADSRotation_CP = 0x824;   // ? NEW (era 0x814 — MUDOU; o RCS le' daqui, Engine.cpp:2050/4263)
	uint32_t RecoilRollValue_CP = 0x810;     // 2610.1.2.7 (novo — roll do recuo; NAO lido por enquanto)
	uint32_t LeanLeftAlpha_CP = 0x6B4;
	uint32_t LeanRightAlpha_CP = 0x6B8;
	uint32_t bIsScoping_CP = 0x865;          // ? NEW
	uint32_t bIsReloading_CP = 0x75D;        // ? NEW
	uint32_t bIsAiming_CP = 0x854;
	uint32_t bIsDBNO_CP = 0x8E2;             // era 0x8F2 (antigo) — 2610.1.2.7 confirma 0x8E2. O filtro de morte depende dele
	uint32_t DBNOHealthMax = 0x1E08;          // 2610.1.2.7 (novo — vida maxima do DBNO; NAO lido por enquanto)
	uint32_t bIsInVehicle_CP = 0x814;        // MUDOU (era 0x954) — mesma posicao de RecoilADSRotation_CP
	uint32_t bIsFlying_CP = 0x300;           // novo nesta build
	uint32_t bIsParachuting_CP = 0x8DF;      // MUDOU (era 0x92E)
	uint32_t bIsFreefalling_CP = 0x8DE;      // MUDOU (era 0x92D)
	uint32_t bEmergencyPickup_Flying_CP = 0x8E0;  // MUDOU (era 0x92F)
	uint32_t bIsVaulting_CP = 0x8E1;         // novo nesta build
	uint32_t bIsReviving_CP = 0x8E3;         // MUDOU (era 0x932)
	uint32_t bIsSwimming_CP = 0x8E6;         // MUDOU (era 0x935)
	uint32_t bIsSelfHemostasis_CP = 0x2FF;   // novo nesta build
	uint32_t bIsActiveRagdoll_CP = 0x8E5;    // 2610.1.2.7 (era 0x300 — colidia com bIsFlying_CP; agora separado)
	uint32_t VTable = 0xA20;                 // novo nesta build
	uint32_t bIsDBNO0 = 0x357A;              // novo nesta build
	uint32_t bIsDBNO1 = 0x3578;              // novo nesta build
	uint32_t bIsDBNO2 = 0x3579;              // novo nesta build
	uint32_t bIsTppAiming = 0x85C;           // novo nesta build
	uint32_t bIsPreparingThrow_CP = 0x558;   // novo nesta build
	uint32_t bIsThrowing_CP = 0x8E9;         // novo nesta build
	uint32_t bIsBurning_CP = 0x656;          // novo nesta build
	uint32_t bIsFlashed_CP = 0x657;          // novo nesta build
	uint32_t bIsStunned_CP = 0x658;          // novo nesta build
	uint32_t bIsTasered_CP = 0x659;          // novo nesta build

	// ---- inventario ------------------------------------------------------
	uint32_t InventoryFacade = 0x1218;       // ? NEW
	uint32_t Inventory = 0x438;              // ? NEW
	uint32_t InventoryItems = 0x600;         // ? NEW
	uint32_t InventoryItemCount = 0x608;
	uint32_t InventoryItemTagItemCount = 0x38;
	uint32_t Equipment = 0x420;
	uint32_t EquipmentItems = 0x568;         // = ItemsArray na lista nova
	uint32_t Durability = 0x1D4;
	uint32_t StackCount = 0x38;              // NAO ATUALIZADO nesta build (nao veio na lista 2609.1.3.5)

	// ---- items no chao ---------------------------------------------------
	uint32_t ItemID = 0x270;
	uint32_t DroppedItemGroupUItem = 0x898;   // dump 2609.1.3.5 (mesmo valor de AttachedItems na lista)
	uint32_t ItemTable = 0xB0;
	uint32_t ItemPackageItems = 0x578;
	uint32_t DroppedItemGroup = 0x178;
	uint32_t DroppedItem = 0x480;
	uint32_t DroppedItemGroup_UItem = 0x898;  // = DroppedItemGroupUItem (duplicata historica)

	// ---- projeto/is ------------------------------------------------------
	uint32_t TimeTillExplosion = 0x824;
	uint32_t ExplodeState = 0x638;
	uint32_t MortarElevation = 0x518;
	uint32_t MortarYaw = 0x51C;              // NAO ATUALIZADO nesta build (nao veio na lista 2609.1.3.5)
	uint32_t MortarShellMovement = 0x450;    // NAO ATUALIZADO nesta build (nao veio na lista 2609.1.3.5)
	uint32_t ProjectileInitialSpeed = 0x240; // NAO ATUALIZADO nesta build (nao veio na lista 2609.1.3.5)
	uint32_t ProjectileGravityScale = 0x25C; // NAO ATUALIZADO nesta build (nao veio na lista 2609.1.3.5)
	uint32_t ProjectileVelocity = 0x20C;     // NAO ATUALIZADO nesta build (nao veio na lista 2609.1.3.5)

	// ---- zona azul -------------------------------------------------------
	uint32_t SafetyZoneRadius = 0xAC;
	uint32_t SafetyZonePosition = 0xA0;
	uint32_t BlueZoneRadius = 0xBC;
	uint32_t BlueZonePosition = 0xB0;

	// ---- veiculo ---------------------------------------------------------
	uint32_t VehicleRiderComponent = 0x2060;
	uint32_t LastVehiclePawn = 0x270;        // ? NEW
	uint32_t VehicleMovement = 0x480;        // ? NEW
	uint32_t VehicleCommonComponent = 0xB50; // ? NEW
	uint32_t VehicleFuel = 0x2F8;            // ? NEW
	uint32_t VehicleHealth = 0x2F0;          // ? NEW
	uint32_t VehicleFuelMax = 0x2EC;
	uint32_t VehicleHealthMax = 0x2E4;
	uint32_t FloatingComponent = 0x500;      // = FloatingVehicleCommonComponent na lista
	uint32_t SeatIndex = 0x228;
	uint32_t Wheels = 0x320;
	uint32_t WheelLocation = 0xF8;
	uint32_t DampingRate = 0x4C;
	uint32_t ShapeRadius = 0x40;

	// ---- placar / identidade ---------------------------------------------
	uint32_t PlayerName = 0x448;             // 2611
	uint32_t PlayerStatusType = 0x418;       // ? NEW
	uint32_t SquadMemberIndex = 0x538;       // ? NEW
	uint32_t AccountId = 0x548;              // ? NEW
	uint32_t PlayerStateKilled = 0x53C;  // lista 2609.1.3.5 (era 0x6F8, de build antiga)
	uint32_t DamageDealtOnEnemy = 0x740;
	uint32_t PartnerLevel = 0x704;
	uint32_t TeamNumber = 0x6D8;
	uint32_t SurvivalTier = 0xE70;
	uint32_t SurvivalLevel = 0xE74;
	uint32_t PubgIdData = 0xE50;
	uint32_t CharacterClanInfo = 0x950;
	uint32_t LastTakeHitInfo = 0x19D0;       // NAO ATUALIZADO nesta build (nao veio na lista 2609.1.3.5)

	// ---- BOT -------------------------------------------------------------
	// Campo NATIVO do jogo (nao e' heuristica de nome!):
	//     Engine.PlayerState.bIsABot   extra->0x468
	// Achado no dump de offsets. 1 = bot, 0 = jogador humano.
	// Fica no PlayerState.
	// ---------------------------------------------------------------------
	uint32_t bIsABot = 0x468;                // NAO ATUALIZADO nesta build (nao veio na lista 2609.1.3.5)

	// ---- static mesh sockets --------------------------------------------
	uint32_t StaticSockets = 0xC0;
	uint32_t StaticSocketName = 0x28;
	uint32_t StaticRelativeLocation = 0x30;
	uint32_t StaticRelativeRotation = 0x3C;
	uint32_t StaticRelativeScale = 0x48;

	// ---- mapa ------------------------------------------------------------
	uint32_t TrainingMapGrid = 0x608;
	uint32_t MapGrid_Map = 0x4B8;

	// ---- widget / layout -------------------------------------------------
	uint32_t LayoutData = 0x38;
	uint32_t Visibility = 0xA1;
	uint32_t Slot = 0x30;
	uint32_t Offset = 0x0;                   // era "Offsets" no dump
	uint32_t Alignment = 0x20;

	// ---- arma: modo de tiro ----------------------------------------------
	uint32_t FiringModeIndex = 0xC74;        // NAO ATUALIZADO nesta build (nao veio na lista 2609.1.3.5)
	uint32_t FireModes = 0xB60;              // NAO ATUALIZADO nesta build (nao veio na lista 2609.1.3.5)

	// ---- timing / render -------------------------------------------------
	uint32_t RecentlyRendered = 0xBC8;       // novo nesta build
	uint32_t MortarRotation = 0x540;         // 2610.1.2.7 (era 0x538 — colidia com MortarElevation)
	uint32_t MortarEntity = 0x98;            // 2610.1.2.7 (era 0xB8; a lista traz tambem MortarLocation 0x98, alias)

	// ---- misc (novos nesta build) ----------------------------------------
	uint32_t AntiCheatCharacterSyncManager = 0xDC8;
	uint32_t CustomTimeDilation = 0x1F8;
	uint32_t CapsuleComponent = 0x568;       // = Mesh (mesmo valor na lista)
	uint32_t bIsInVehicle = 0x814;           // = bIsInVehicle_CP (alias do mesmo campo)
	uint32_t SphereRadius = 0x9E0;              // 2610.1.2.7 (novo — USkeletalMeshComponent::SphereRadius; NAO lido; nome exato da lista crua)

	// ---- anti-cheat / funcoes (RVA no modulo — NAO sao deslocamento) -----
	// Vem na lista da build mas NAO entraram na struct antes. Ficam aqui,
	// como uint64_t, junto dos outros RVA. Nenhum deles e' lido pelo cheat
	// hoje; se algum dia forem, o valor ja' esta' da build certa.
	// (SPOOFCALL/LineTraceSingle/HOOK/HOOK_TWO sao funcoes internas do jogo.
	//  Unreal_Engine e' RVA de simbolo. PhysxSDK VOLTOU na lista 2610.1.2.7
	//  (0x1358F908) e fica registrado no bloco de PhysX abaixo — NAO USADO.)
	uint64_t SPOOFCALL = 0x2CABCA5;
	uint64_t LineTraceSingle = 0xF95F530;
	uint64_t HOOK = 0x12B99410;
	uint64_t HOOK_TWO = 0xD5A21D0;
	uint64_t Unreal_Engine = 0x124CEA70;  // 2611: GEngine static RVA

	// ---- PhysX (NAO USAR — ver o bloco do PhysX no inicio da struct) -----
	// A lista traz PhysicsScene/mPhysXScene/rigid_dynamics, mas o cheat NAO
	// le' mais nada de PhysX (foi removido de proposito, ARQUITETURA.md §14).
	// Os valores ficam registrados para o dia em que a cadeia for retomada.
	uint32_t PhysicsScene = 0x1A0;           // NAO USADO (PhysX removido)
	uint32_t mPhysXScene = 0xD0;             // NAO USADO (PhysX removido)
	uint32_t rigid_dynamics = 0x3F98;        // NAO USADO (PhysX removido)
	uint64_t PhysxSDK = 0x139F3108;  // 2610.1.2.7 (novamente na lista — era 0x12C460C8; NAO USADO, registrado p/ raycast)

	// ---- minimap / anim (globais) ----------------------------------------
	uint32_t CurrentMinimapViewScale1D = 0x464;  // novo nesta build
	uint32_t LastMinimapPos = 0x478;             // novo nesta build
	uint32_t Minimap = 0x480;                    // novo nesta build
	uint32_t GlobalAnimRateScale = 0xEC8;        // novo nesta build
	uint32_t LastDamage = 0x1A40;                // novo nesta build
	uint32_t FirstPersonCamera = 0xFB8;          // novo nesta build
	uint32_t SubAnim_CachedParentInstance = 0x3A0;  // novo nesta build
	uint32_t SubAnim_CharacterReference = 0x3A8;    // novo nesta build
	uint32_t IK_RecoilADSRotation = 0x3F0;          // novo nesta build

	// ---- BlueBlocker (Gauge) ---------------------------------------------
	uint32_t BlueBlockerGaugeTotalMax = 0x10;    // novo nesta build

	// ---- cursor (dentro do bloco de input) -------------------------------
	uint32_t MouseX = 0x5369;                    // = MouseX (mesmo valor da lista)
	uint32_t MouseY = 0x536A;                    // = MouseY (mesmo valor da lista)

	// =====================================================================
	// NAO VERIFICADOS
	//
	// Este bloco era o "sobraram do build anterior". Na lista da 2609.1.3.5
	// estes campos VIERAM — os que sobraram de verdade estao marcados um a um
	// com `// NAO ATUALIZADO nesta build (nao veio na lista 2609.1.3.5)`.
	//
	// Os que vieram foram atualizados mas continuam AQUI (nao subiram para as
	// secoes tematicas) e o valor antigo fica no comentario: nenhum deles e'
	// lido por nenhum .cpp do projeto (checado com grep antes de mexer), entao
	// o custo de mudar e' zero e o de deixar velho e' um campo mentiroso.
	//
	// `python tools\dump_driver\offset_audit.py --dump _offsets\build_2610.1.2.7.txt`
	// aponta qualquer um que ainda divirja.
	// =====================================================================

	uint32_t WorldToMap = 0x240;                     // lista 2610.1.2.7 (era 0x6D0 — MUDOU)
	uint32_t ActorsForGC = 0x2B0;                    // veio na lista 2609.1.3.5 (era 0x1F8) (lista da comunidade)
	uint32_t FeatureRepObject = 0xD10;               // veio na lista 2609.1.3.5 (era 0xCF0)
	uint32_t Durabilitymax = 0x1D0;                  // veio na lista 2609.1.3.5 (era 0x1E0)
	uint32_t MatchId = 0x508;                        // veio na lista 2609.1.3.5 (era 0x510)
	uint32_t Ping = 0x480;                           // veio na lista 2609.1.3.5 (era 0x3F8) (era "ping" no dump)
	uint32_t GObjects = 0x1409C1F0;  // lista 2610.1.2.9 (era 0x1306FF60) (RVA no modulo)
	uint64_t GObjectsCount = 0x1409C200;  // lista 2610.1.2.9 (novo — contador global de objetos, RVA)
	uint32_t GObjectItemStride = 0x18;               // lista 2610.1.2.7 (novo — tamanho do FUObjectItem; confirma ObjectID 0x10 + FName + class)
}inline SDK;

