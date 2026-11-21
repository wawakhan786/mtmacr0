#pragma once
#include "pch.h"
#include "Lang.h"      // L(...) nos rotulos de elemento/ancora logo abaixo

// ===========================================================================
// LAYOUT DO ESP — ONDE CADA INFORMACAO APARECE NA CAIXA DO JOGADOR
//
// ANTES: nome, arma, time, bot, kills e distancia eram concatenados numa
// STRING UNICA e desenhados num ponto FIXO — abaixo do pe', centralizado
// (PlayerEsp.cpp, `EspStrokeText(wdistance, Vec2(centerX, FootScreen.y + 5))`).
// Nao existia controle nenhum de posicao.
//
// AGORA cada elemento tem ANCORA + ajuste fino:
//
//     anchor  = qual borda/quina da caixa o elemento encosta
//     dx, dy  = ajuste fino em pixels de tela, aplicado DEPOIS da ancora
//
// O `dx/dy` existe porque 8 ancoras nao bastam para posicionar com precisao
// ("2 px mais pra cima"). Ele e' o RESIDUO do arrasto no painel do menu.
//
// QUEM EMPILHA: quando dois elementos caem na MESMA ancora, eles nao se
// sobrepoem — empilham na ordem do enum, do mais perto da caixa pro mais
// longe. E' por isso que a ordem do enum tem a BARRA DE VIDA primeiro: com
// ela no topo, a barra fica colada na caixa e o texto sobe a partir dela.
// ===========================================================================

enum EspElement : int
{
	ESP_EL_HEALTH = 0,   // barra de vida   (na frente: fica colada na caixa)
	ESP_EL_NAME,
	ESP_EL_WEAPON,
	ESP_EL_KILLS,
	ESP_EL_DIST,
	ESP_EL_BOT,          // [BOT] / [P]
	ESP_EL_TEAM,         // [S<n>]
	ESP_EL_COUNT
};

enum EspAnchor : int
{
	ESP_ANC_TOP = 0,        // acima da caixa, centralizado
	ESP_ANC_BOTTOM,         // abaixo da caixa, centralizado
	ESP_ANC_LEFT,           // a esquerda da caixa, texto alinhado a' direita
	ESP_ANC_RIGHT,          // a direita da caixa, texto alinhado a' esquerda
	ESP_ANC_TOP_LEFT,       // acima, encostado na borda esquerda
	ESP_ANC_TOP_RIGHT,      // acima, encostado na borda direita
	ESP_ANC_BOTTOM_LEFT,    // abaixo, encostado na borda esquerda
	ESP_ANC_BOTTOM_RIGHT,   // abaixo, encostado na borda direita
	ESP_ANC_COUNT
};

inline const char* EspAnchorNome(int a)
{
	switch (a) {
		case ESP_ANC_TOP:          return L("topo");
		case ESP_ANC_BOTTOM:       return L("base");
		case ESP_ANC_LEFT:         return L("esquerda");
		case ESP_ANC_RIGHT:        return L("direita");
		case ESP_ANC_TOP_LEFT:     return L("topo/esq");
		case ESP_ANC_TOP_RIGHT:    return L("topo/dir");
		case ESP_ANC_BOTTOM_LEFT:  return L("base/esq");
		case ESP_ANC_BOTTOM_RIGHT: return L("base/dir");
		default:                   return "?";
	}
}

// Nome de EXIBICAO (menu) e nome da CHAVE no JSON. Separados de proposito: o
// do menu tem acento e pode mudar sem quebrar config salva.
inline const char* EspElementRotulo(int e)
{
	switch (e) {
		case ESP_EL_HEALTH: return L("Barra de vida");
		case ESP_EL_NAME:   return L("Nome");
		case ESP_EL_WEAPON: return L("Arma");
		case ESP_EL_KILLS:  return L("Kills");
		case ESP_EL_DIST:   return L("Distancia");
		case ESP_EL_BOT:    return L("Bot/Player");
		case ESP_EL_TEAM:   return L("Time");
		default:            return "?";
	}
}

inline const char* EspElementChave(int e)
{
	switch (e) {
		case ESP_EL_HEALTH: return "Health";
		case ESP_EL_NAME:   return "Name";
		case ESP_EL_WEAPON: return "Weapon";
		case ESP_EL_KILLS:  return "Kills";
		case ESP_EL_DIST:   return "Dist";
		case ESP_EL_BOT:    return "Bot";
		case ESP_EL_TEAM:   return "Team";
		default:            return "?";
	}
}

struct EspElementLayout
{
	int   anchor = ESP_ANC_TOP;
	float dx = 0.0f;
	float dy = 0.0f;
};

// ===========================================================================
// FILTRO DE LOOT POR SUBSTRING (aba WORLD) — UM MECANISMO, TODAS AS CATEGORIAS
//
// O filtro individual (LootItemFilter) exige marcar item por item. Com 200+
// itens no catalogo isso nao escala: pra esconder TODA mira, ou todo
// acessorio de cano, seriam 30 cliques — e o item novo que aparecesse depois
// viria visivel de novo.
//
// Aqui a regra e' por PEDACO DE NOME, aplicada no nome BRUTO da classe:
//
//     "Scope"    pega  Item_Attach_Weapon_Scope_RedDot_C   (mira)
//     "Weapon"   pega  Item_Weapon_M16A4_C                 (arma)
//     "Muzzle"   pega  Item_Attach_Weapon_Muzzle_Compensator_C
//
// E' o MESMO codigo para arma, mira e acessorio de proposito: a diferenca
// entre eles e' so' o texto, e um `if` por categoria seria a mesma regra
// escrita tres vezes — exatamente o padrao que ja' produziu bugs neste
// projeto (ClassifyLootRaw e o filtro do ESP divergindo).
//
// MULTI-SELECT: varias substrings podem estar ativas ao mesmo tempo. A regra
// e' OR — item que casa com QUALQUER uma entra no filtro. Cada uma tem o seu
// `on`, entao da' pra deixar a lista montada e so' ligar/desligar a que
// interessa sem redigitar.
//
// COMBINACAO COM O FILTRO INDIVIDUAL: os dois valem juntos. O individual
// decide item a item; o substring decide por padrao. Se o substring esconde
// algo, o individual nao tem como reabilitar — a ordem e' individual primeiro,
// substring depois (ver DrawLootEsp).
// ===========================================================================
struct LootSubFilter
{
	std::string sub;          // substring, comparada CASE-INSENSITIVE
	bool        on = true;    // participa do filtro agora?
};

// Comparacao CASE-INSENSITIVE de `alvo` dentro de `texto`.
//
// Existe aqui (e nao em LootCatalog.h) porque esta e' a UNICA regra de
// comparacao do filtro: o painel conta os itens que casam e o ESP decide o
// que desenhar com ESTA mesma funcao. Duas implementacoes = duas respostas
// possiveis pra "quantos itens casam" — o numero na tela contra o que aparece
// no mapa.
inline bool CasaSubstringCI(const std::string& texto, const std::string& alvo)
{
	const size_t n = alvo.size();
	if (n == 0 || texto.size() < n) return false;
	for (size_t i = 0; i + n <= texto.size(); i++) {
		size_t j = 0;
		for (; j < n; j++) {
			const char a = (char)tolower((unsigned char)texto[i + j]);
			const char b = (char)tolower((unsigned char)alvo[j]);
			if (a != b) break;
		}
		if (j == n) return true;
	}
	return false;
}

class PlayerConfig
{
	std::string ConfigName;

public:
	PlayerConfig(const std::string& name)
	{
		ConfigName = name;
	}
	// -----------------------------------------------------------------------
	// IDIOMA — 0 = Portugues (BR), 1 = English.
	//
	// Ver Config/Lang.h. Os rotulos de TODO o cheat passam por `L(...)`; a
	// config guarda so' o numero, e o `Main.cpp` chama `Lang::Set` quando o
	// config carrega e quando o combo muda.
	// -----------------------------------------------------------------------
	int Language = 0;

    bool Name = true;
    bool Distance = true;
	bool Box = true;
	bool Health = true;

	// -----------------------------------------------------------------------
	// LAYOUT DO ESP (ver EspElementLayout no topo deste arquivo)
	//
	// ARRAY indexado por `EspElement` — e nao 7 campos soltos — porque tanto o
	// desenho (PlayerEsp) quanto o painel do menu PERCORREM os elementos num
	// laco. Com campos soltos seriam tres listas paralelas pra manter em sinc.
	//
	// O default reproduz um ESP sensato de primeira: barra de vida colada no
	// topo da caixa, nome/arma/kills subindo acima dela, e distancia/bot/time
	// descendo abaixo. Tudo arrastavel no painel da aba VISUALS.
	// -----------------------------------------------------------------------
	EspElementLayout EspLayout[ESP_EL_COUNT] = {
		/* ESP_EL_HEALTH */ { ESP_ANC_TOP,    0.0f, 0.0f },
		/* ESP_EL_NAME   */ { ESP_ANC_TOP,    0.0f, 0.0f },
		/* ESP_EL_WEAPON */ { ESP_ANC_TOP,    0.0f, 0.0f },
		/* ESP_EL_KILLS  */ { ESP_ANC_TOP,    0.0f, 0.0f },
		/* ESP_EL_DIST   */ { ESP_ANC_BOTTOM, 0.0f, 0.0f },
		/* ESP_EL_BOT    */ { ESP_ANC_BOTTOM, 0.0f, 0.0f },
		/* ESP_EL_TEAM   */ { ESP_ANC_BOTTOM, 0.0f, 0.0f },
	};
	// ---- GameState (zona azul / roster / stats) ----------------------
	// OFF por padrao: esta cadeia faz MUITAS leituras especulativas e foi a
	// causa provavel de BSOD (PAGE_FAULT) numa partida real. Ligue so' quando
	// quiser testar, ciente do risco.
	bool ZoneRadar = false;
	bool Vehicle = true;
	bool Loot = false;

	// ---- filtros do Loot ESP (por categoria) ----
	// Os quatro primeiros SAO os checkboxes gerais das categorias equivalentes
	// da aba WORLD (ver LootCategoryBool em ESP/LootCatalog.h).
	bool LootWeapons = true;      // armas            -> Weapons
	bool LootAmmo = true;         // municao          -> Ammo
	bool LootHealth = true;       // cura             -> Health
	// SEM USO desde a aba WORLD: "mira" caiu em Attachments (Item_Attach_...).
	// O campo fica declarado e serializado de proposito — apagar mudaria o
	// JSON salvo do usuario sem ganho nenhum.
	bool LootScopes = true;       // miras (sem uso)
	bool LootAttachments = true;  // acessorios       -> Attachments
	// SEM USO desde a aba WORLD: "equipamento" foi dividido em Helmet /
	// Backpack / Vest. Mesmo motivo do LootScopes pra nao apagar.
	bool LootEquipment = true;    // equipamento (sem uso)

	// ---- aba WORLD: checkbox GERAL das categorias NOVAS ----
	// (Weapons/Ammo/Attachments/Health reaproveitam os campos acima)
	bool LootCatBoost = true;       // Item_Boost_
	bool LootCatHelmet = true;      // Item_Head_
	bool LootCatBackpack = true;    // Item_Back_
	bool LootCatVest = true;        // Item_Armor_
	bool LootCatThrowables = true;  // Grenade / Throwable
	bool LootCatDeathBox = true;    // DeathDropItemPackage — caixa de morte
	bool LootCatOther = true;       // o resto

	// ---- aba WORLD: filtro INDIVIDUAL ----
	// nome BRUTO do item ("Item_Weapon_M16A4_C") -> visivel.
	//
	// E' o espelho PERSISTIDO de G_LootCatalog (ESP/LootCatalog.h): o catalogo
	// e' a lista do que o cheat ja' viu nesta sessao, este mapa e' o que vai
	// pro JSON. Os dois sao reconciliados 1x por frame em
	// SyncLootCatalogIntoConfig() (Main.cpp) e ficam sempre iguais.
	//
	// Vai pro JSON como objeto: { "Item_Weapon_M16A4_C": false, ... }
	std::map<std::string, bool> LootItemFilter;

	// ---- aba WORLD: filtro por SUBSTRING (ver LootSubFilter acima) ----
	// `LootSubFilterMode`:
	//   0 = ESCONDE o que casa (lista de exclusao — o uso mais comum: "nao
	//       quero ver mira nem coronha");
	//   1 = MOSTRA SO' o que casa (lista de inclusao — "so' arma e municao").
	//
	// Os dois modos usam a MESMA lista de substrings; trocar o modo troca a
	// pergunta, nao o dado. E' por isso que a lista sobrevive a troca.
	std::vector<LootSubFilter> LootSubFilters;
	int LootSubFilterMode = 0;

	// -----------------------------------------------------------------------
	// QUEM DECIDE SE UM ITEM PASSA PELO FILTRO DE SUBSTRING.
	//
	// Uma funcao so', usada pelo desenho (DrawLootEsp) E pelo painel (contagem
	// de itens que casam na aba WORLD). Se cada um tivesse a sua copia, o
	// painel poderia dizer "3 itens" enquanto o ESP esconde 4.
	//
	// SEM FILTRO ATIVO -> PASSA (fail-open). Lista vazia ou toda desmarcada
	// nao pode esvaziar o mapa: e' a mesma regra do filtro individual ausente.
	// -----------------------------------------------------------------------
	bool LootRawPassaSubFiltro(const std::string& rawBruto) const
	{
		bool temAtivo = false;
		bool casa     = false;
		for (const auto& f : LootSubFilters) {
			if (!f.on || f.sub.empty()) continue;
			temAtivo = true;
			if (CasaSubstringCI(rawBruto, f.sub)) { casa = true; break; }
		}
		if (!temAtivo) return true;
		return (LootSubFilterMode == 0) ? !casa : casa;
	}

	// true se o item casa com ALGUMA substring ATIVA — independente do modo.
	// E' o que os botoes "ligar/desligar os que casam" aplicam em massa.
	bool LootRawCasaAlgumFiltro(const std::string& rawBruto) const
	{
		for (const auto& f : LootSubFilters) {
			if (!f.on || f.sub.empty()) continue;
			if (CasaSubstringCI(rawBruto, f.sub)) return true;
		}
		return false;
	}

	bool Weapon = true;
	bool Skeleton = false;
	bool Radar = false;
	bool Prediction = false;
	float RadarSize = 200.0f;
	float RadarRange = 150.0f;
	float RadarX = 20.0f;          // posicao do radar na tela
	float RadarY = 100.0f;         // (encaixe em cima do minimapa do jogo)
	float RadarArrowOffset = 0.0f;  // corrige o sentido da seta do radar (-180..180)
	                                // a matematica esta' verificada; se a seta
	                                // apontar torto e' convencao de eixo do mapa
	bool  ShowSnaplines = false;   // linha da base da tela ate' o inimigo
	bool  SnaplineFromCenter = false;  // parte do centro em vez da base
	bool  StreamProof = false;     // overlay invisivel em OBS/Discord
	// -----------------------------------------------------------------
	// MODO CAPTURA (EpicPen / print de tela / OBS).
	//
	// O cliente usa o EpicPen para capturar a sobreposicao (ESP + HUD) e
	// mostrar ao dono do account. Com a streamproof ligada a janela some de
	// TODA captura — e o ESP nao aparece no print. Este toggle forca a
	// janela a ficar visivel (SetWindowDisplayAffinity = WDA_NONE, por cima
	// da StreamProof) e garante o backend LEGADO na recriacao, que e' o
	// caminho de composicao que todas as capturas (GDI/DXGI) enxergam.
	//
	// NAO confunde: desliga a streamproof so' para quem pede captura; quem
	// nao liga nunca muda de comportamento.
	// -----------------------------------------------------------------
	bool  CaptureMode = false;
	// -----------------------------------------------------------------
	// QUALIDADE DO TEXTO DO ESP (performance).
	//
	// StrokeText desenha a string uma vez por direcao do contorno. 9 = o
	 // contorno cheio (4 eixos + 4 diagonais + corpo): maxima legibilidade,
	// maximo custo — a maioria dos AddText/frame do overlay mora aqui.
	// 5 = 4 eixos + corpo (sem diagonais): contorno levemente mais fino nos
	// cantos, ~45% menos trabalho de texto.
	// -----------------------------------------------------------------
	int   EspTextQuality = 9;
	int   CrosshairStyle = 0;      // 0=off 1=ponto 2=cruz 3+circulo
	float CrosshairSize = 6.0f;
	float CrosshairGap = 4.0f;
	bool  AirdropEsp = true;       // caixas de suprimento aereo
	// -----------------------------------------------------------------
	// DIAGNOSTICO
	//
	// Liga os logs pesados (CENSO de classes, LOOTDIAG, LOOTDIAG2) e o dump
	// por IOCTL do driver. DESLIGADO por padrao: em uso normal eles so'
	// engordam o log e custam tempo de frame.
	//
	// Servem pra quando o PUBG atualizar e os offsets mudarem — foi com eles
	// que a gente achou o nome do loot (2 medicoes em vez de 10 chutes).
	// -----------------------------------------------------------------
	bool  DebugLogs = false;

	// ---- DIAGNOSTICO DE VISIBILIDADE (REMOVIDO) --------------------------
	// VisDiag / VisToleranceMs existiam para MEDIR, em partida real, qual
	// campo do mesh separava "na tela" de "atras de parede" (log "[VIS]",
	// 1x/s, em Engine::UpdatePlayers).
	//
	// A medicao FOI FEITA e o veredito esta' documentado em Globals.h
	// ("O QUE FOI MEDIDO NO JOGO"): nenhum campo de UPROPERTY discrimina
	// oclusao. Com o diagnostico cumprido, os DOIS campos e o log foram
	// removidos: nao existe mais leitura de visibilidade, e o ESP fica
	// fail-open (uma cor so'). Ver ActorEntity::IsVisible.
	// ---------------------------------------------------------------------

	// ---- HUD de tela (espectadores / inimigo mais proximo) --------------
	// Posicao em PIXELS de tela do canto superior-esquerdo do HUD. Antes o
	// HUD era desenhado em posicao FIXA (centro-x - 30, y=100 e
	// altura-100): nunca dava pra tirar de cima do minimapa/contador do
	// jogo. Agora a posicao vem daqui e o HUD e' arrastavel com o mouse.
	float HudX = 0.0f;
	float HudY = 0.0f;
	// Permite arrastar o HUD com o botao esquerdo. Ver DrawPlayerEsp: o
	// arrasto SO' funciona com o MENU ABERTO, porque com o menu fechado a
	// janela do overlay tem WS_EX_TRANSPARENT e o Windows entrega o clique
	// pro jogo em vez de pro ImGui (DirectOverlay::ToggleMenu).
	bool  HudDraggable = true;

	// ---- fundo atras do texto do ESP -------------------------------------
	// Retangulo escuro semi-transparente desenhado ATRAS de todo texto do
	// ESP (nome/distancia/arma, loot, veiculo, granada, HUD). Existia um
	// problema real: texto branco em cima de neve/ceu claro sumia.
	// A opacidade e' separada porque o usuario pode querer o fundo quase
	// invisivel (25) ou quase opaco (220).
	// Legacy mirror retained for older configuration files.
    bool PlayerEspEnabled = true;
    int ToggleEspKey = 0x75; // F6
    int ToggleAimKey = 0x76; // F7
    int ToggleLootKey = 0x77; // F8
    int ToggleRcsKey = 0x78; // F9
    int   EspTextStyle = 0;       // 0=outline, 1=shadow, 2=compact background
    int   EspFontFamily = 1;      // 0=Verdana, 1=Inter Medium
    int   PlayerTextSize = 15;
    int   ItemTextSize = 14;
    int   SecondaryTextSize = 13;
    bool  TextBackground = false;
	int   TextBgOpacity = 90;   // mais transparente (era 150: escuro demais)     // 0..255 (alpha do retangulo)

	bool  ShowSpectators = true;
	bool  ShowTeamInfo = true;     // mostra [S<time>] no ESP
	bool  ShowBotFlag = true;      // mostra [BOT] / [P] no ESP   // mostra o contador mesmo em 0
	bool  RadarNorthUp = true;     // true = norte fixo (estilo minimapa do PUBG)
	                               // false = gira com a camera
	// -----------------------------------------------------------------------
	// DISTANCIA MAXIMA — UMA POR CATEGORIA.
	//
	// Antes havia UM `MaxDistance` e as tres categorias dividiam o mesmo teto.
	// Isso obrigava a escolher entre duas coisas ruins: teto alto pra ver
	// jogador longe (e o mapa inteiro de loot entrando junto, enchendo a tela
	// e custando projecao por item) ou teto baixo pra limpar o loot (e perder
	// o jogador a 300 m).
	//
	// As categorias tem escalas diferentes de propósito:
	//   - jogador: 1000 m — e' a informacao que decide a partida;
	//   - veiculo: 500 m — interessa pra rotacao, some cedo;
	//   - item:    150 m — loot so' importa no alcance do pulo/da mao.
	//
	// O CORTE ACONTECE ANTES DE MONTAR A STRING (ver DrawLootEsp /
	// DrawVehicleEsp): item fora do teto nao passa por `snprintf`, nem por
	// `CleanEntityName`, nem por `ClassifyLootRaw`. E' o caminho quente do ESP
	// (dezenas de itens por frame), entao a ordem das checagens e' o que
	// importa — nao so' o resultado.
	//
	// `MaxDistance` continua sendo o do JOGADOR de proposito: e' a chave que
	// ja' esta' no JSON salvo de todo mundo. Renomear quebraria o save pra
	// ganhar nada.
	// -----------------------------------------------------------------------
	int MaxDistance = 1000;        // jogador (chave historica do JSON)
	int MaxDistVehicle = 500;      // veiculo
	int MaxDistItem = 150;         // loot / death box / airdrop

	int MemoryUpdateRate = 144;
	// -----------------------------------------------------------------------
	// `TextColour` = a cor de "OCLUIDO" (atras de parede). O nome e' historico
	// (era a cor do texto de quem estava oculto); a CHAVE no JSON tambem fica,
	// senao todo config salvo perde a escolha.
	//
	// DEFAULT MUDOU DE BRANCO PARA VERMELHO: com a caixa verde (BoxColour), a
	// diferenca verde->branco e' sutil o bastante para o usuario concluir que o
	// vischeck "nao funciona". Vermelho e' a leitura imediata de "nao da' pra
	// atirar nesse".
	//
	// ATENCAO: config JA' SALVO tem o branco antigo gravado e continua branco —
	// o default so' vale para config novo. Por isso a secao Visibilidade avisa
	// quando a cor esta' branca (ver Main.cpp).
	// -----------------------------------------------------------------------
	Colour TextColour = Colour(255, 64, 64);        // ocluido (atras de parede)

	// ---- cores do ESP (configuraveis) -----------------------------------
	// Antes eram fixas no PlayerEsp.cpp: verde pra visivel, laranja pra DBNO.
	// Agora saem daqui, com o mesmo helper Colour das outras.
	Colour BoxColour      = Colour(0, 255, 0);      // caixa do inimigo
	Colour SkeletonColour = Colour(0, 255, 0);      // esqueleto
	Colour DbnoColour     = Colour(255, 165, 0);    // deitado/knocked

	// -----------------------------------------------------------------------
	// ESPESSURA DA CAIXA E DO ESQUELETO (px) — e por que o default AFINOU.
	//
	// Onde cada uma e' usada (medido, nao suposto):
	//   PlayerEsp.cpp  DrawPlayerEsp    -> `Gui.Rectangle(..., BoxThickness)`
	//                                      (3 chamadas: osso, fallback osso,
	//                                       sem osso)
	//   PlayerEsp.cpp  DrawPlayerEsp    -> `Gui.Line(..., SkeletonThickness)`
	//                                      (14 segmentos do esqueleto)
	//
	// Os dois valores eram literais (1.5f e 1.0f). O 1.5f da caixa tinha
	// origem no fallback ANTIGO `width = height / 2`: uma caixa larga e
	// imprecisa, que pedia traco grosso pra nao sumir. A caixa de hoje e'
	// derivada da PROJECAO DOS OSSOS (+15% de folga lateral), entao ela ficou
	// justa — e o traco de 1.5 px, em 1080p, passa a pesar: o AddRect com
	// espessura > 1 gera uma faixa de 2 px de antialias em vez de 1 px de
	// linha, e a caixa lê como "suja" ao lado do esqueleto de 1.0.
	//
	// Default 1.0 nas duas: e' o traco que o ImGui desenha em pixel inteiro
	// (nitido), e o usuario sobe se jogar em 4K ou se preferir mais contraste.
	// O clamp de sanidade no desenho (PlayerEsp, `EspTraco`) impede que um
	// JSON editado a mao (NaN, 0, 9999) suma ou exploda o retangulo.
	// -----------------------------------------------------------------------
	float BoxThickness      = 1.0f;   // 0.5 .. 4.0
	float SkeletonThickness = 1.0f;   // 0.5 .. 4.0

	// -----------------------------------------------------------------------
	// BOX SMOOTH (28/10) — lerp da caixa entre frames, POR ATOR.
	//
	// A caixa bruta (projeção dos ossos) muda de pixels de um tick pro
	// outro: o esqueleto do jogo anima (respirar, crouch, andar) e a
	// bounding box dos 17 ossos projetados treme junto — o "treco" da
	// caixa que aparece em video de ESP. `DrawPlayerEsp` mantem o
	 // retangulo anterior por endereco de ator e interpola (k=0.6): o
	// tremor morre, o atraso e' de ~1 frame (6-9 ms), inperceptivel
	// comparado com o jitter de origem.
	//
	// FALHA ABERTA DE SEMANTICA: ator sem estado anterior (spawn, respawn,
	 // ou reuso de endereco apos morte) desenha a caixa BRUTA no frame e
	 // so' entao entra no lerp — nenhum voo estranho no primeiro frame.
	// Desligado = caixa bruta como sempre foi (o default era este).
	// -----------------------------------------------------------------------
	bool  BoxSmooth          = true;   // true = lerp por ator; false = bruta

	// -----------------------------------------------------------------------
	// VISIBILIDADE — `LastTimeScreenRenderered` (Globals.h, 0x750).
	//
	// LIGA A LEITURA. Desligado (default) o ESP fica como sempre esteve:
	// `IsVisible` true para todo mundo, uma cor so' (fail-open).
	//
	// Ligado, o Engine compara o "ultimo frame em que o pawn foi desenhado"
	// com o tempo de mundo do GameState e marca `IsVisible`:
	//   - ESP: quem esta' atras de parede cai na cor `TextColour` (aba VISUAL,
	//     "Oculto (atras de parede)") — o campo ja' existia e nunca era
	//     alcancado;
	//   - AIMBOT: `VisibilityCheck` passa a valer de verdade (ele ja' lia
	//     `IsVisible`; antes era sempre true e o filtro nao filtrava nada).
	//
	// POR QUE O DEFAULT E' OFF: o sinal e' NOVO nesta build e a regra da casa
	// e' nao trocar o padrao de desenho por um offset que ainda nao foi medido
	// em partida. Ligar e' uma decisao de teste, e o `VisToleranceMs` e' o
	// botao de calibragem da medicao.
	//
	// `VisToleranceMs`: quantos ms de atraso ainda contam como "na tela".
	// 100 ms cobre folga de alguns frames entre o render e a leitura da
	// memoria; abaixo de ~50 ms o ESP comeca a piscar em inimigo visivel a
	// 144 Hz (a leitura e a renderizacao nao sao sincronas).
	// -----------------------------------------------------------------------
	bool VisCheck       = false;
	int  VisToleranceMs = 100;      // 20 .. 400

	// -----------------------------------------------------------------------
	// TRANSPORTE KERNEL <-> USER.
	//
	// false = IOCTL (device + symbolic link; tem injecao de mouse, entao
	//         aimbot e RCS funcionam).
	// true  = shared memory + KEVENT (sem device, sem IRP; ESCOPO ESP —
	//         sem KMP_OP_MOUSE, aimbot e RCS ficam inertes).
	//
	// A variavel de ambiente MTMACR0_BACKEND_KEVENT=1 continua valendo e tem
	// precedencia, para testar A/B sem mexer no JSON.
	// -----------------------------------------------------------------------
	bool BackendKevent = false;

	// -----------------------------------------------------------------------
	// POSICAO DO LOADER NA TELA (px, canto superior esquerdo da janela).
	//
	// -1 = nunca foi movido -> abre centralizado. Guardar no config e' o que
	// faz o loader abrir onde o usuario deixou, em vez de voltar pro centro a
	// cada boot. Fica no Survivor porque o loader e' desenhado pelo mesmo
	// caminho do menu e nao justifica uma secao propria no JSON.
	// -----------------------------------------------------------------------
	int LoaderX = -1;
	int LoaderY = -1;

	int FontSize = 11;
    void ToJsonColour(json* j, const std::string& name, Colour* colour)
    {
        (*j)[ConfigName][name][LIT("r")] = colour->r;
        (*j)[ConfigName][name][LIT("g")] = colour->g;
        (*j)[ConfigName][name][LIT("b")] = colour->b;
        (*j)[ConfigName][name][LIT("a")] = colour->a;

    }
    void FromJsonColour(json j, const std::string& name, Colour* colour)
    {
        if (j[ConfigName].contains(name))
        {
            colour->r = j[ConfigName][name][LIT("r")];
            colour->g = j[ConfigName][name][LIT("g")];
            colour->b = j[ConfigName][name][LIT("b")];
            colour->a = j[ConfigName][name][LIT("a")];
        }
    }

    json ToJson()
    {
        json j;
        j[ConfigName][LIT("Name")] = Name;
        j[ConfigName][LIT("Distance")] = Distance;
        j[ConfigName][LIT("Box")] = Box;
        j[ConfigName][LIT("Skeleton")] = Skeleton;
        j[ConfigName][LIT("Health")] = Health;
        j[ConfigName][LIT("Vehicle")] = Vehicle;
        j[ConfigName][LIT("Loot")] = Loot;
		j[ConfigName][LIT("LootWeapons")] = LootWeapons;
		j[ConfigName][LIT("LootAmmo")] = LootAmmo;
		j[ConfigName][LIT("LootHealth")] = LootHealth;
		j[ConfigName][LIT("LootScopes")] = LootScopes;
		j[ConfigName][LIT("LootAttachments")] = LootAttachments;
		j[ConfigName][LIT("LootEquipment")] = LootEquipment;
		// ---- aba WORLD: checkbox geral das categorias novas + filtro por item
		j[ConfigName][LIT("LootCatBoost")] = LootCatBoost;
		j[ConfigName][LIT("LootCatHelmet")] = LootCatHelmet;
		j[ConfigName][LIT("LootCatBackpack")] = LootCatBackpack;
		j[ConfigName][LIT("LootCatVest")] = LootCatVest;
		j[ConfigName][LIT("LootCatThrowables")] = LootCatThrowables;
		j[ConfigName][LIT("LootCatDeathBox")] = LootCatDeathBox;
		j[ConfigName][LIT("LootCatOther")] = LootCatOther;
		// std::map<std::string,bool> vira objeto JSON direto (nlohmann)
		j[ConfigName][LIT("LootItemFilter")] = LootItemFilter;
		// Filtro por substring: ARRAY de objetos (e nao um map nome->bool)
		// porque a ORDEM importa pra leitura humana da lista e porque duas
		// substrings iguais em case diferente ("Scope" / "scope") sao
		// entradas legitimas — num map elas colidiriam.
		//
		//   "LootSubFilters": [ { "s": "Scope", "on": true }, ... ]
		{
			json arr = json::array();
			for (const auto& f : LootSubFilters) {
				json one;
				one[LIT("s")]  = f.sub;
				one[LIT("on")] = f.on;
				arr.push_back(one);
			}
			j[ConfigName][LIT("LootSubFilters")] = arr;
		}
		j[ConfigName][LIT("LootSubFilterMode")] = LootSubFilterMode;
		j[ConfigName][LIT("ZoneRadar")] = ZoneRadar;
        j[ConfigName][LIT("Weapon")] = Weapon;
        j[ConfigName][LIT("Radar")] = Radar;
        j[ConfigName][LIT("Prediction")] = Prediction;
        j[ConfigName][LIT("RadarSize")] = RadarSize;
        j[ConfigName][LIT("RadarRange")] = RadarRange;
        j[ConfigName][LIT("FontSize")] = FontSize;
        j[ConfigName][LIT("MaxDistance")] = MaxDistance;
        j[ConfigName][LIT("MaxDistVehicle")] = MaxDistVehicle;
        j[ConfigName][LIT("MaxDistItem")] = MaxDistItem;
        // Espessura do traco (px) — ver o bloco na declaracao dos campos.
        j[ConfigName][LIT("BoxThickness")] = BoxThickness;
        j[ConfigName][LIT("SkeletonThickness")] = SkeletonThickness;
        j[ConfigName][LIT("BoxSmooth")] = BoxSmooth;
        // Visibilidade (LastTimeScreenRenderered) — ver o bloco na declaracao.
        j[ConfigName][LIT("VisCheck")] = VisCheck;
        j[ConfigName][LIT("VisToleranceMs")] = VisToleranceMs;
        j[ConfigName][LIT("BackendKevent")] = BackendKevent;
        j[ConfigName][LIT("LoaderX")] = LoaderX;
        j[ConfigName][LIT("LoaderY")] = LoaderY;
        j[ConfigName][LIT("MemoryUpdateRate")] = MemoryUpdateRate;
        ToJsonColour(&j, LIT("TextColour"), &TextColour);
	ToJsonColour(&j, LIT("BoxColour"),      &BoxColour);
	ToJsonColour(&j, LIT("SkeletonColour"), &SkeletonColour);
	ToJsonColour(&j, LIT("DbnoColour"),     &DbnoColour);

        // -----------------------------------------------------------------
        // CAMPOS DO ESP QUE NUNCA ERAM SALVOS
        //
        // Estavam declarados na struct e tinham checkbox na GUI, mas nao
        // apareciam aqui nem no FromJson: a cada boot voltavam pro default.
        // Era o bug das "TRES PARTES" (declarar / ToJson / FromJson) que o
        // projeto ja' tinha tomado antes com o filtro do loot.
        //
        // O caso mais visivel era o ShowSnaplines: o usuario ligava a linha e
        // ela sumia no proximo boot. Como snapline/radar/crosshair/stream proof
        // sao todos do ESP, entram aqui de uma vez.
        // -----------------------------------------------------------------
        j[ConfigName][LIT("ShowSnaplines")] = ShowSnaplines;
        j[ConfigName][LIT("SnaplineFromCenter")] = SnaplineFromCenter;
        j[ConfigName][LIT("ShowTeamInfo")] = ShowTeamInfo;
        j[ConfigName][LIT("ShowBotFlag")] = ShowBotFlag;
        j[ConfigName][LIT("ShowSpectators")] = ShowSpectators;
        j[ConfigName][LIT("AirdropEsp")] = AirdropEsp;
        j[ConfigName][LIT("DebugLogs")] = DebugLogs;
        // VisDiag / VisToleranceMs sairam daqui junto com o diagnostico
        // [VIS] (ver o comentario na declaracao dos campos).
        // ---- HUD de tela ----
        j[ConfigName][LIT("HudX")] = HudX;
        j[ConfigName][LIT("HudY")] = HudY;
        j[ConfigName][LIT("HudDraggable")] = HudDraggable;
        // ---- fundo do texto do ESP ----
        TextBackground = EspTextStyle == 2;
        j[ConfigName][LIT("EspTextStyle")] = EspTextStyle;
        j[ConfigName][LIT("EspFontFamily")] = EspFontFamily;
        j[ConfigName][LIT("PlayerTextSize")] = PlayerTextSize;
        j[ConfigName][LIT("ItemTextSize")] = ItemTextSize;
        j[ConfigName][LIT("SecondaryTextSize")] = SecondaryTextSize;
        j[ConfigName][LIT("TextBackground")] = TextBackground;
        j[ConfigName][LIT("TextBgOpacity")] = TextBgOpacity;
        j[ConfigName][LIT("StreamProof")] = StreamProof;
        j[ConfigName][LIT("CaptureMode")] = CaptureMode;
        j[ConfigName][LIT("EspTextQuality")] = EspTextQuality;
        j[ConfigName][LIT("CrosshairStyle")] = CrosshairStyle;
        j[ConfigName][LIT("CrosshairSize")] = CrosshairSize;
        j[ConfigName][LIT("CrosshairGap")] = CrosshairGap;
        j[ConfigName][LIT("RadarNorthUp")] = RadarNorthUp;
        j[ConfigName][LIT("RadarX")] = RadarX;
        j[ConfigName][LIT("RadarY")] = RadarY;
        j[ConfigName][LIT("RadarArrowOffset")] = RadarArrowOffset;
        j[ConfigName][LIT("PlayerEspEnabled")] = PlayerEspEnabled;
        j[ConfigName][LIT("ToggleEspKey")] = ToggleEspKey;
        j[ConfigName][LIT("ToggleAimKey")] = ToggleAimKey;
        j[ConfigName][LIT("ToggleLootKey")] = ToggleLootKey;
        j[ConfigName][LIT("ToggleRcsKey")] = ToggleRcsKey;
        j[ConfigName][LIT("Language")] = Language;

        // ---- layout do ESP: um OBJETO por elemento ----
        //   "EspLayout": { "Name": { "a": 0, "x": 0.0, "y": 0.0 }, ... }
        // A chave e' `EspElementChave` (estavel); o rotulo do menu pode mudar
        // sem quebrar config salva.
        {
            json lay = json::object();
            for (int e = 0; e < ESP_EL_COUNT; ++e) {
                json one;
                one[LIT("a")] = EspLayout[e].anchor;
                one[LIT("x")] = EspLayout[e].dx;
                one[LIT("y")] = EspLayout[e].dy;
                lay[EspElementChave(e)] = one;
            }
            j[ConfigName][LIT("EspLayout")] = lay;
        }

        return j;
    }
    void FromJson(const json& j)
    {
        if (!j.contains(ConfigName))
            return;
        if (j[ConfigName].contains(LIT("Name")))
            Name = j[ConfigName][LIT("Name")];
        if (j[ConfigName].contains(LIT("Distance")))
            Distance = j[ConfigName][LIT("Distance")];
        if (j[ConfigName].contains(LIT("Box")))
            Box = j[ConfigName][LIT("Box")];
        if (j[ConfigName].contains(LIT("Skeleton")))
            Skeleton = j[ConfigName][LIT("Skeleton")];
        if (j[ConfigName].contains(LIT("Health")))
            Health = j[ConfigName][LIT("Health")];
        if (j[ConfigName].contains(LIT("Vehicle")))
            Vehicle = j[ConfigName][LIT("Vehicle")];
        if (j[ConfigName].contains(LIT("Loot")))
            Loot = j[ConfigName][LIT("Loot")];
        // -----------------------------------------------------------------
        // Estes seis SEMPRE foram gravados no ToJson mas NUNCA lidos de volta
        // (o FromJson nao tinha nenhum deles): o filtro do loot voltava no
        // default a cada boot e parecia que o save nao funcionava. E' o bug
        // classico do projeto — campo de config sem a TERCEIRA parte.
        // -----------------------------------------------------------------
        if (j[ConfigName].contains(LIT("LootWeapons")))
            LootWeapons = j[ConfigName][LIT("LootWeapons")];
        if (j[ConfigName].contains(LIT("LootAmmo")))
            LootAmmo = j[ConfigName][LIT("LootAmmo")];
        if (j[ConfigName].contains(LIT("LootHealth")))
            LootHealth = j[ConfigName][LIT("LootHealth")];
        if (j[ConfigName].contains(LIT("LootScopes")))
            LootScopes = j[ConfigName][LIT("LootScopes")];
        if (j[ConfigName].contains(LIT("LootAttachments")))
            LootAttachments = j[ConfigName][LIT("LootAttachments")];
        if (j[ConfigName].contains(LIT("LootEquipment")))
            LootEquipment = j[ConfigName][LIT("LootEquipment")];
        // ---- aba WORLD ----
        if (j[ConfigName].contains(LIT("LootCatBoost")))
            LootCatBoost = j[ConfigName][LIT("LootCatBoost")];
        if (j[ConfigName].contains(LIT("LootCatHelmet")))
            LootCatHelmet = j[ConfigName][LIT("LootCatHelmet")];
        if (j[ConfigName].contains(LIT("LootCatBackpack")))
            LootCatBackpack = j[ConfigName][LIT("LootCatBackpack")];
        if (j[ConfigName].contains(LIT("LootCatVest")))
            LootCatVest = j[ConfigName][LIT("LootCatVest")];
        if (j[ConfigName].contains(LIT("LootCatThrowables")))
            LootCatThrowables = j[ConfigName][LIT("LootCatThrowables")];
        if (j[ConfigName].contains(LIT("LootCatOther")))
            LootCatOther = j[ConfigName][LIT("LootCatOther")];
        if (j[ConfigName].contains(LIT("LootCatDeathBox")))
            LootCatDeathBox = j[ConfigName][LIT("LootCatDeathBox")];
        // Filtro individual: objeto JSON -> std::map<std::string,bool>
        if (j[ConfigName].contains(LIT("LootItemFilter")))
            LootItemFilter = j[ConfigName][LIT("LootItemFilter")].get<std::map<std::string, bool>>();
        // Filtro por substring — array de { "s", "on" } (ver ToJson).
        // Entrada sem "s" (ou vazia) e' descartada aqui: substring vazia
        // casaria com TODOS os itens, e num filtro de exclusao isso esconderia
        // o mapa inteiro por causa de um campo malformado.
        if (j[ConfigName].contains(LIT("LootSubFilters"))) {
            const json& arr = j[ConfigName][LIT("LootSubFilters")];
            LootSubFilters.clear();
            if (arr.is_array()) {
                LootSubFilters.reserve(arr.size());
                for (const auto& one : arr) {
                    if (!one.is_object() || !one.contains(LIT("s"))) continue;
                    if (!one[LIT("s")].is_string()) continue;
                    LootSubFilter f;
                    f.sub = one[LIT("s")].get<std::string>();
                    if (f.sub.empty()) continue;
                    f.on = one.contains(LIT("on")) ? one[LIT("on")].get<bool>() : true;
                    LootSubFilters.push_back(f);
                }
            }
        }
        if (j[ConfigName].contains(LIT("LootSubFilterMode"))) {
            const int m = j[ConfigName][LIT("LootSubFilterMode")];
            LootSubFilterMode = (m == 1) ? 1 : 0;
        }
        if (j[ConfigName].contains(LIT("Weapon")))
            Weapon = j[ConfigName][LIT("Weapon")];
        if (j[ConfigName].contains(LIT("Radar")))
            Radar = j[ConfigName][LIT("Radar")];
        if (j[ConfigName].contains(LIT("Prediction")))
            Prediction = j[ConfigName][LIT("Prediction")];
        if (j[ConfigName].contains(LIT("RadarSize")))
            RadarSize = j[ConfigName][LIT("RadarSize")];
        if (j[ConfigName].contains(LIT("RadarRange")))
            RadarRange = j[ConfigName][LIT("RadarRange")];
        if (j[ConfigName].contains(LIT("FontSize")))
            FontSize = j[ConfigName][LIT("FontSize")];
        if (j[ConfigName].contains(LIT("MaxDistance")))
            MaxDistance = j[ConfigName][LIT("MaxDistance")];
        // Config antiga nao tem estes dois: o default (500 / 150) fica valendo.
        // Nao e' fallback pra "MaxDistance" de proposito — o valor antigo era
        // um teto UNICO, e herda-lo por categoria traria de volta exatamente o
        // problema que a separacao resolve.
        if (j[ConfigName].contains(LIT("MaxDistVehicle")))
            MaxDistVehicle = j[ConfigName][LIT("MaxDistVehicle")];
        if (j[ConfigName].contains(LIT("MaxDistItem")))
            MaxDistItem = j[ConfigName][LIT("MaxDistItem")];
        // Clamp: JSON editado a mao (ou o antigo slider de loot) pode trazer
        // 0/negativo, e distancia maxima 0 nao desenha NADA.
        if (MaxDistance    < 10)  MaxDistance    = 10;
        if (MaxDistVehicle < 10)  MaxDistVehicle = 10;
        if (MaxDistItem    < 10)  MaxDistItem    = 10;

        if (j[ConfigName].contains(LIT("BoxThickness")))
            BoxThickness = j[ConfigName][LIT("BoxThickness")];
        if (j[ConfigName].contains(LIT("SkeletonThickness")))
            SkeletonThickness = j[ConfigName][LIT("SkeletonThickness")];
        if (j[ConfigName].contains(LIT("BoxSmooth")))
            BoxSmooth = j[ConfigName][LIT("BoxSmooth")];
        // Mesma razao do clamp das distancias: NaN, 0 e 9999 chegam aqui via
        // JSON editado a mao e o desenho nao pode confiar neles. O range e' o
        // do slider; o desenho tem uma segunda barreira (EspTraco).
        if (!(BoxThickness      >= 0.5f && BoxThickness      <= 6.0f)) BoxThickness      = 1.0f;
        if (!(SkeletonThickness >= 0.5f && SkeletonThickness <= 6.0f)) SkeletonThickness = 1.0f;

        if (j[ConfigName].contains(LIT("VisCheck")))
            VisCheck = j[ConfigName][LIT("VisCheck")];

        if (j[ConfigName].contains(LIT("VisToleranceMs")))
            VisToleranceMs = j[ConfigName][LIT("VisToleranceMs")];

        if (j[ConfigName].contains(LIT("BackendKevent")))
            BackendKevent = j[ConfigName][LIT("BackendKevent")];

        if (j[ConfigName].contains(LIT("LoaderX")))
            LoaderX = j[ConfigName][LIT("LoaderX")];
        if (j[ConfigName].contains(LIT("LoaderY")))
            LoaderY = j[ConfigName][LIT("LoaderY")];
        // Tolerancia 0 esconderia TODO MUNDO (delta > 0 em qualquer leitura) —
        // e um valor absurdo marcaria todo mundo como visivel. Nos dois casos
        // o defeito pareceria "o vischeck nao funciona", que e' o pior
        // sintoma possivel: ele funciona e mente.
        if (VisToleranceMs < 20)  VisToleranceMs = 20;
        if (VisToleranceMs > 400) VisToleranceMs = 400;
        if (j[ConfigName].contains(LIT("MemoryUpdateRate")))
            MemoryUpdateRate = j[ConfigName][LIT("MemoryUpdateRate")];
        FromJsonColour(j, LIT("TextColour"), &TextColour);
	FromJsonColour(j, LIT("BoxColour"),      &BoxColour);
	FromJsonColour(j, LIT("SkeletonColour"), &SkeletonColour);
	FromJsonColour(j, LIT("DbnoColour"),     &DbnoColour);

        // ---- os campos do ESP que faltavam (ver comentario no ToJson) ----
        if (j[ConfigName].contains(LIT("ShowSnaplines")))
            ShowSnaplines = j[ConfigName][LIT("ShowSnaplines")];
        if (j[ConfigName].contains(LIT("SnaplineFromCenter")))
            SnaplineFromCenter = j[ConfigName][LIT("SnaplineFromCenter")];
        if (j[ConfigName].contains(LIT("ShowTeamInfo")))
            ShowTeamInfo = j[ConfigName][LIT("ShowTeamInfo")];
        if (j[ConfigName].contains(LIT("ShowBotFlag")))
            ShowBotFlag = j[ConfigName][LIT("ShowBotFlag")];
        if (j[ConfigName].contains(LIT("ShowSpectators")))
            ShowSpectators = j[ConfigName][LIT("ShowSpectators")];
        if (j[ConfigName].contains(LIT("AirdropEsp")))
            AirdropEsp = j[ConfigName][LIT("AirdropEsp")];
        if (j[ConfigName].contains(LIT("DebugLogs")))
            DebugLogs = j[ConfigName][LIT("DebugLogs")];
        // VisDiag / VisToleranceMs: campos removidos (ver comentario no ToJson).
        // ---- HUD de tela ----
        if (j[ConfigName].contains(LIT("HudX")))
            HudX = j[ConfigName][LIT("HudX")];
        if (j[ConfigName].contains(LIT("HudY")))
            HudY = j[ConfigName][LIT("HudY")];
        if (j[ConfigName].contains(LIT("HudDraggable")))
            HudDraggable = j[ConfigName][LIT("HudDraggable")];
        // ---- fundo do texto do ESP ----
        // Explicit modern settings take precedence; legacy background-only files migrate once.
        auto read_text_int = [&](const char* key, int fallback, int low, int high) {
            const auto& object = j[ConfigName];
            if (!object.contains(key) || !object[key].is_number_integer()) return fallback;
            if (object[key].is_number_unsigned()) return static_cast<int>((std::min)(object[key].get<uint64_t>(), static_cast<uint64_t>(high)));
            const auto number = object[key].get<int64_t>();
            return static_cast<int>((std::clamp)(number, static_cast<int64_t>(low), static_cast<int64_t>(high)));
        };
        const bool legacy_background = j[ConfigName].contains(LIT("TextBackground")) &&
            j[ConfigName][LIT("TextBackground")].is_boolean() && j[ConfigName][LIT("TextBackground")].get<bool>();
        EspTextStyle = read_text_int(LIT("EspTextStyle"), legacy_background ? 2 : 0, 0, 2);
        EspFontFamily = read_text_int(LIT("EspFontFamily"), 1, 0, 1);
        PlayerTextSize = read_text_int(LIT("PlayerTextSize"), 15, 12, 15);
        ItemTextSize = read_text_int(LIT("ItemTextSize"), 14, 12, 15);
        SecondaryTextSize = read_text_int(LIT("SecondaryTextSize"), 13, 12, 15);
        TextBackground = EspTextStyle == 2;
        TextBgOpacity = read_text_int(LIT("TextBgOpacity"), 90, 0, 255);

        if (j[ConfigName].contains(LIT("StreamProof")))
            StreamProof = j[ConfigName][LIT("StreamProof")];
        if (j[ConfigName].contains(LIT("CaptureMode")))
            CaptureMode = j[ConfigName][LIT("CaptureMode")];
        if (j[ConfigName].contains(LIT("EspTextQuality"))) {
            EspTextQuality = j[ConfigName][LIT("EspTextQuality")];
            // So' existem dois valores validos: qualquer outra coisa vira o
            // padrao (Ultra) em vez de um numero de passadas impossivel.
            if (EspTextQuality != 4 && EspTextQuality != 9) EspTextQuality = 9;
        };
        if (j[ConfigName].contains(LIT("CrosshairStyle")))
            CrosshairStyle = j[ConfigName][LIT("CrosshairStyle")];
        if (j[ConfigName].contains(LIT("CrosshairSize")))
            CrosshairSize = j[ConfigName][LIT("CrosshairSize")];
        if (j[ConfigName].contains(LIT("CrosshairGap")))
            CrosshairGap = j[ConfigName][LIT("CrosshairGap")];
        if (j[ConfigName].contains(LIT("RadarNorthUp")))
            RadarNorthUp = j[ConfigName][LIT("RadarNorthUp")];
        if (j[ConfigName].contains(LIT("RadarX")))
            RadarX = j[ConfigName][LIT("RadarX")];
        if (j[ConfigName].contains(LIT("RadarY")))
            RadarY = j[ConfigName][LIT("RadarY")];
        if (j[ConfigName].contains(LIT("RadarArrowOffset")))
            RadarArrowOffset = j[ConfigName][LIT("RadarArrowOffset")];
        if (j[ConfigName].contains(LIT("Language"))) {
            int l = j[ConfigName][LIT("Language")];
            Language = (l == LANG_EN_US) ? LANG_EN_US : LANG_PT_BR;
        }

        if (j[ConfigName].contains("PlayerEspEnabled") && j[ConfigName]["PlayerEspEnabled"].is_boolean())
            PlayerEspEnabled = j[ConfigName]["PlayerEspEnabled"].get<bool>();
        const auto read_toggle_key = [&](const char* name, int fallback) {
            const auto& section = j[ConfigName];
            if (!section.contains(name) || !section[name].is_number_integer()) return fallback;
            const auto value = section[name].get<long long>();
            return value == 0 || (value >= 0x70 && value <= 0x7b) ? static_cast<int>(value) : fallback;
        };
        ToggleEspKey = read_toggle_key("ToggleEspKey", 0x75);
        ToggleAimKey = read_toggle_key("ToggleAimKey", 0x76);
        ToggleLootKey = read_toggle_key("ToggleLootKey", 0x77);
        ToggleRcsKey = read_toggle_key("ToggleRcsKey", 0x78);

        // ---- layout do ESP ----
        // Config antiga nao tem "EspLayout": o default do array fica valendo
        // (e' o comportamento certo — quem nunca arrastou nao deve perder o
        // layout padrao por causa de um campo novo).
        if (j[ConfigName].contains(LIT("EspLayout"))) {
            const json& lay = j[ConfigName][LIT("EspLayout")];
            for (int e = 0; e < ESP_EL_COUNT; ++e) {
                const char* chave = EspElementChave(e);
                if (!lay.contains(chave)) continue;
                const json& one = lay[chave];
                if (one.contains(LIT("a"))) {
                    int a = one[LIT("a")];
                    // Sanidade: config editada a mao pode trazer ancora fora
                    // da faixa, e `anchor` indexa um switch de POSICIONAMENTO.
                    if (a >= 0 && a < ESP_ANC_COUNT) EspLayout[e].anchor = a;
                }
                if (one.contains(LIT("x"))) EspLayout[e].dx = one[LIT("x")];
                if (one.contains(LIT("y"))) EspLayout[e].dy = one[LIT("y")];
            }
        }
    }
};

