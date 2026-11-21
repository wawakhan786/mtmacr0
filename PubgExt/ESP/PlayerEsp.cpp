#include "pch.h"
#include "../Perf.h"
#include <cstring>   // std::strlen (prefixos de arma viraram const char*)
#include <unordered_map>   // memo de CleanEntityName
#include "PlayerEsp.h"
#include "DrawSanity.h"   // [DRAW SANITY] barreira de coordenada (ver o header)
#include "EspLayout.h"    // geometria das ancoras (compartilhada com o menu)
#include "Engine.h"
#include "Globals.h"
#include "Camera.h"
#include "../Config/ConfigUtilities.h"
#include "../OS-ImGui/OS-ImGui.h"
// DirectOverlay entra so' pelo `ShowMenu`: o HUD arrastavel precisa saber se o
// menu esta' aberto (ver a nota em DrawPlayerEsp). O header e' self-contained
// (nao puxa o resto do projeto).
#include "../OS-ImGui/DirectOverlay.h"
#include "../SharedState.h"
#include "../UI/EspTypography.h"   // tipografia nativa do ESP (freetype) — transplante
extern bool RunDrawingTest;

// All world-label paths share native font metrics and the selected contrast style.
static inline void EspText(const std::string& value, Vec2 position, ImColor color, float size = 15, bool centered = false)
{
    if (DrawSanity::TelaOk(position.x, position.y, "texto")) Gui.StrokeText(value, position, color, size, centered);
}
static inline void EspStrokeText(const std::string& value, Vec2 position, ImColor color, float size = 15, bool centered = false, int passes = 0)
{
    if (DrawSanity::TelaOk(position.x, position.y, "texto")) Gui.StrokeText(value, position, color, size, centered, false, passes);
}
static inline void EspBlockText(const std::string& value, Vec2 position, ImColor color, float size, bool centered = false)
{
    if (!DrawSanity::TelaOk(position.x, position.y, "texto")) return;
    EspTypography::draw(ImGui::GetBackgroundDrawList(), value.c_str(), position.ToImVec2(), color, size, centered,
        EspTypography::active_style, 0); // One shared panel is already drawn for this block.
}

// ===========================================================================
// LAYOUT DO ESP — A CAIXA DO JOGADOR COMO REFERENCIA
//
// Ver `EspElementLayout` em Config/PlayerConfig.h para o modelo e o porque.
// Aqui mora a GEOMETRIA: dado o retangulo da caixa na tela, onde cai o canto
// superior-esquerdo de um elemento ancorado.
//
// POR QUE O EMPILHAMENTO E' UM PARAMETRO (`EspStack`) E NAO UM `static`:
// ele e' POR ATOR. Dois jogadores na tela tem caixas diferentes e precisam de
// pilhas diferentes; um `static` faria o segundo jogador herdar a altura que o
// primeiro ja' empilhou, e o texto ia descendo a tela.
// ===========================================================================
namespace {

// A GEOMETRIA vive em EspLayout.h, compartilhada com o preview do menu. Se o
// preview calculasse a posicao por conta propria, ele passaria a MENTIR na
// primeira vez que as duas regras divergissem (voce arrasta num lugar, o texto
// sai em outro). Uma geometria, dois consumidores.
using EspBox   = EspLayout::Caixa;
using EspStack = EspLayout::Pilha;

// Devolve o canto SUPERIOR-ESQUERDO do retangulo (w x h) do elemento.
// `keepCenter` sai dizendo se o desenho deve CENTRAR em X no ponto devolvido
// (ancoras de topo/base) ou usar o ponto como borda esquerda (laterais e
// quinas) — e' a mesma convencao do `KeepCenter` do OSImGui::Text.
//
// O corpo esta' em EspLayout.h; aqui e' so' o apelido do namespace pra nao
// reescrever as chamadas no meio do desenho.
Vec2 EspResolver(const EspBox& b, int anchor, float dx, float dy,
                 float w, float h, EspStack& st, bool& keepCenter)
{
	return EspLayout::Resolver(b, anchor, dx, dy, w, h, st, keepCenter);
}

} // namespace

// ===========================================================================
// PAINEL DE FUNDO PARA UM BLOCO DE TEXTO (2+ linhas empilhadas).
//
// Blocos como o HUD e a etiqueta da granada tem DUAS/TRES linhas na mesma
// posicao X. Se cada linha desenhasse o proprio retangulo, a emenda entre elas
// ficaria visivel (alpha empilha e a faixa do meio fica mais escura). Entao o
// bloco mede UMA vez e desenha UM retangulo; as linhas entram com fundo
// desligado (Opacity = 0 no OSImGui::TextWithBg).
//
// Mesma cor/alpha do resto do ESP. NAO e' preto puro de proposito: o overlay
// usa LWA_COLORKEY com preto = transparente (ver PlayerConfig.h/OS-ImGui.cpp).
// ===========================================================================
static inline void EspPanelBg(Vec2 Pos, float Largura, float Altura)
{
	if (!Configs.Survivor.TextBackground) return;
	if (Configs.Survivor.TextBgOpacity <= 0) return;
	// [DRAW SANITY] Painel de 2+ linhas (HUD, veiculo): Largura/Altura vem da
	// MEDIDA do texto e Pos da projecao. Qualquer um nao-finito = nao desenha
	// (o OSImGui::RectangleFilled tem a mesma guarda; esta aqui e' para o log
	// de diagnostico dizer QUAL elemento foi rejeitado).
	if (!DrawSanity::TelaOk(Pos.x, Pos.y, "painel"))
		return;
	if (!std::isfinite(Largura) || !std::isfinite(Altura))
		return;
	Gui.RectangleFilled(Pos, Vec2(Largura, Altura),
	                    IM_COL32(12, 12, 14, Configs.Survivor.TextBgOpacity), 3.0f);
}

// ===========================================================================
// LARGURA DO TEXTO DO ESP — NA MESMA FONTE E NO MESMO TAMANHO DO DESENHO.
//
// `ImGui::CalcTextSize` mede na fonte PADRAO (15px, a do menu). O ESP desenha
// com GEspFont a um FontSize proprio (13/14/15...), entao medir por
// CalcTextSize daria um painel sistematicamente errado (a fonte e o tamanho
// sao outros).
//
// Este helper repete exatamente a medicao que o OSImGui::Text ja' faz para
// centralizar — mesma fonte, mesmo FontSize. E' a MESMA expressao de
// OS-ImGui.cpp: se um dia o desenho mudar de fonte, os dois mudam aqui.
// ===========================================================================
static inline ImVec2 EspTextSize(const std::string& Text, float FontSize)
{
	ImFont* fonte = EspTypography::font_for(FontSize);
	// [DRAW SANITY] Sem fonte (atlas ainda nao construido) ou com FontSize
	// absurdo a medida nao existe: devolve 0x0 e o painel sai do tamanho do
	// texto (o desenho continua valido) em vez de propagar NaN.
	if (!fonte || !std::isfinite(FontSize) || FontSize <= 0.0f)
		return ImVec2(0.0f, 0.0f);
	return EspTypography::measure(Text.c_str(), FontSize);
}

// ===========================================================================
// ESPESSURA DO TRACO (caixa / esqueleto) — SEGUNDA BARREIRA.
//
// O config ja' clampa no FromJson, mas este e' o ponto em que o valor vira
// argumento de AddRect/AddLine. Tres casos que o clamp do JSON NAO cobre:
//
//   1. NaN            — `NaN >= 0.5f` e' FALSO, entao o clamp do FromJson
//                       ate' pega. Mas o ESP tambem le' o valor de um config
//                       que pode ter sido trocado em runtime (preset), e a
//                       guarda do OSImGui::Rectangle descarta o desenho
//                       INTEIRO (nao-finites = nao desenha). Resultado seria
//                       "a caixa sumiu", sem log e sem pista.
//   2. 0.0            — AddRect com espessura 0 desenha um contorno de 1 px
//                       FANTASMA que depende do backend; AddLine com 0 vira
//                       um fio que o AA apaga pela metade.
//   3. > 6            — a caixa vira um bloco; nao e' "mais visivel", e' lixo.
//
// Devolver um default bom e' melhor que devolver 0: o ESP continua legivel e
// o defeito fica obvio (o valor no menu nao e' o que esta' desenhando).
// ===========================================================================
static inline float EspTraco(float v)
{
	if (!std::isfinite(v) || v < 0.5f) return 1.0f;
	if (v > 6.0f) return 6.0f;
	return v;
}

// ===========================================================================
// BOX SMOOTH (28/10) — lerp da caixa entre frames, POR ATOR.
//
// A caixa bruta (projeção dos ossos + padding) TREME de frame pra frame —
// e' a animacao do proprio esqueleto do jogo (respirar, andar, crouch),
// nao erro de leitura. Interpolacao exponencial em direcao ao alvo
// (k=0.6 por frame) mata o jitter: a 144 Hz o atraso residual e' de ~1
// frame, inperceptivel; o "treco" que aparece em video de ESP some.
//
// O estado e' STATIC, chaveado pelo endereco do ator (`entity->Class` —
// o ActorEntity nao guarda identificador alem do proprio endereco, e a
// Engine garante uma entidade por endereco no ActorCache). Endereco novo
// (spawn, respawn, reuso apos morte) RECEBE O RETANGULO BRUTO como seed:
// sem voo estranho no primeiro frame. A purga tira enderecos mortos UMA
// vez por frame — n e' pequeno (<= dezenas), o O(n^2) aqui custa menos
// que um AddText.
//
// Desligado (`BoxSmooth=false`) ou retangulo nao-finito: desenha o bruto,
// nao toca no estado (NaN nunca contamina o lerp).
// ===========================================================================
struct EspBoxSmoothEnt { uint64_t addr; float x, y, w, h; };
static std::vector<EspBoxSmoothEnt> g_boxSmooth;

static void EspBoxSmoothPurge(const std::vector<std::shared_ptr<ActorEntity>>& ativos)
{
	if (g_boxSmooth.empty()) return;
	size_t m = 0;
	for (auto& s : g_boxSmooth) {
		bool vivo = false;
		for (auto& e : ativos) if (e && e->Class == s.addr) { vivo = true; break; }
		if (vivo) g_boxSmooth[m++] = s;
	}
	g_boxSmooth.resize(m);
}

static void EspBoxRectSuave(uint64_t actorAddr, float& x, float& y, float& w, float& h)
{
	if (!Configs.Survivor.BoxSmooth) return;
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h)) return;
	const float k = 0.6f; // ~1 frame de atraso residual a 144 Hz
	for (auto& s : g_boxSmooth) {
		if (s.addr != actorAddr) continue;
		s.x += (x - s.x) * k;
		s.y += (y - s.y) * k;
		s.w += (w - s.w) * k;
		s.h += (h - s.h) * k;
		x = s.x; y = s.y; w = s.w; h = s.h;
		return;
	}
	g_boxSmooth.push_back({ actorAddr, x, y, w, h }); // seed com o bruto
}


// ---------------------------------------------------------------------------
// Limpa o nome de classe do ator para mostrar algo legivel.
//   "BP_Vehicle_Dacia_2_C"        -> "Dacia"
//   "DroppedItemGroup_C"          -> "DroppedItemGroup"
//   "BP_Weapon_AK47_C"            -> "AK47"
// ---------------------------------------------------------------------------
// ===========================================================================
// CleanEntityName — nome legivel a partir do nome de classe UE4.
//
//   "BP_Vehicle_Truck_C"          -> "Truck"           (firstTokenOnly)
//   "Item_Weapon_AK47_C"          -> "AK47"
//   "Item_Attach_Weapon_Muzzle_Compensator_C"
//                                 -> "Muzzle Compensator"
//
// `firstTokenOnly` existe porque veiculo tem nome longo e redundante
// (Dacia_2_Wheel) e o que importa e' o modelo; ja' item precisa do nome
// inteiro (senao "Item_Weapon_AK47" vira so "Item").
// ===========================================================================
static std::string CleanEntityNameSemCache(const std::string& raw, bool firstTokenOnly)
{
	if (raw.empty()) return raw;
	std::string s = raw;

	static const char* prefixes[] = {
		"BP_Vehicle_", "VehicleBP_", "BP_", "Vehicle_",
		"DroppedItemGroup_", "ItemPackage_", "DroppedItem_", "Dropped_",
		"UltAIPawn_Base_", "AIPawn_Base_",
		// itens: tira o ruido mas MANTEM o que identifica
		// ---- prefixos MEDIDOS no LOOTDIAG2 ------------------------------
		// ordem importa: o mais especifico primeiro
		"Item_Attach_Weapon_", "Item_Attach_", "Item_Weapon_", "Item_Ammo_",
		"Item_Heal_", "Item_Boost_", "Item_Head_", "Item_Back_", "Item_Use_",
		"Item_Armor_", "Item_Helmet_", "Item_",
		// deixa 'Head'/'Back' legiveis em vez de sumir com a informacao
		"Weapon_",
		// ---- prefixos confirmados no CENSO de classes -------------------
		// o jogo expoe as armas como 'WeapAK47_C', 'WeapHK416_C' etc — o nome
		// da ARMA esta' na propria classe, entao so' falta tirar o 'Weap'
		"Weap",
		// airdrop: classe real e' 'Carapackage_RedBox_C' (com A, nao E)
		"Carapackage_RedBox_", "Carapackage_", "CarePackage_",
		// drop solto
		"TslDrop"
	};
	for (const char* p : prefixes) {
		size_t n = strlen(p);
		if (s.size() > n && s.compare(0, n, p) == 0) {
			// -------------------------------------------------------------
			// "Weap" so' sai quando o proximo caractere for MAIUSCULO.
			//
			// Sem isso, "WeaponItem" virava "onItem" (e depois "OnItem"):
			// o prefixo "Weap" casava com "Weapo" tambem. As armas do jogo
			// sao "WeapAK47_C" / "WeapHK416_C" (sempre maiusculo depois), e
			// "WeaponItem" e' a classe generica de item de inventario —
			// justamente a que NAO se pode mutilar.
			// -------------------------------------------------------------
			if (n == 4 && strcmp(p, "Weap") == 0) {
				char c = s[n];
				if (c < 'A' || c > 'Z') continue;   // nao e' arma, nao mexe
			}
			s.erase(0, n);
			break;
		}
	}

	static const char* suffixes[] = {
		"_C", "_Spawn", "_Main", "_BP", "_01", "_Skeletal", "_Physics", "_Weapon"
	};
	for (const char* suf : suffixes) {
		size_t n = strlen(suf);
		if (s.size() > n && s.compare(s.size() - n, n, suf) == 0) {
			s.erase(s.size() - n);
			break;
		}
	}

	// -----------------------------------------------------------------
	// AJUSTES DE NOME (padroes MEDIDOS no LOOTDIAG2)
	//
	// O jogo codifica variante de modelo no nome ('E_01', 'E_02') e usa
	// 'Head'/'Back' pra capacete/mochila. Traduzindo:
	//     Head_E_01_Lv1  -> Helmet Lv1
	//     Back_E_01_Lv1  -> Backpack Lv1
	// -----------------------------------------------------------------
	{
		static const struct { const char* de; const char* para; } ajustesLoot[] = {
			{ "Head_E_01_Lv", "Helmet Lv" },
			{ "Back_E_01_Lv", "Backpack Lv" },
			{ "Head_E_02_Lv", "Helmet Lv" },
			{ "Back_E_02_Lv", "Backpack Lv" },
			{ "Head_E_",      "Helmet " },
			{ "Back_E_",      "Backpack " },
			{ "JerryCan",     "Jerry Can" },
			{ "MedKit",       "Med Kit" },
			{ "FirstAid",     "First Aid" },
			{ "AdrenalineSyringe", "Adrenaline" },
			{ "InstantRevivalKit", "Revival Kit" },
			{ "BulletproofShield", "Bulletproof Shield" },
			{ "ThumbGrip",    "Thumb Grip" },
			{ "StickyGrenade","Sticky Grenade" },
			{ "IntegratedRepair", "Repair Kit" },
			{ "StunGun",      "Stun Gun" },
			{ "TacPack",      "Tactical Pack" },
		};
		for (const auto& a : ajustesLoot) {
			size_t pos = s.find(a.de);
			if (pos != std::string::npos) {
				s.replace(pos, strlen(a.de), a.para);
			}
		}
	}

	// -----------------------------------------------------------------
	// APELIDOS (dados do CENSO)
	//
	// Alguns nomes do jogo sao abreviados/juntos e ficam ilegiveis crus.
	// Estes sao os que o censo mostrou que precisam de traducao.
	// -----------------------------------------------------------------
	{
		static const struct { const char* de; const char* para; } apelidosCenso[] = {
			{ "HK416",      "M416" },          // no PUBG e' licenciado como M416
			{ "BerylM762",  "Beryl M762" },
			{ "Mini14",     "Mini 14" },
			{ "Mk12",       "Mk12" },
			{ "Winchester", "Winchester" },
			{ "RedBox",     "Airdrop" },
			{ "PicoBus",    "PicoBus" },
		};
		for (const auto& a : apelidosCenso) {
			if (s == a.de) { s = a.para; break; }
		}
	}

	if (firstTokenOnly) {
		size_t u = s.find('_');
		if (u != std::string::npos && u > 0) s = s.substr(0, u);
	}

	// '_' -> ' ' e capitaliza o inicio de cada palavra (AK47 fica AK47)
	std::string out;
	out.reserve(s.size());
	bool cap = true;
	for (char c : s) {
		if (c == '_' || c == '-') { out.push_back(' '); cap = true; continue; }
		if (cap && c >= 'a' && c <= 'z') c = (char)(c - 32);
		out.push_back(c);
		cap = false;
	}

	// tira espacos duplicados nas pontas
	size_t a = out.find_first_not_of(' ');
	size_t b = out.find_last_not_of(' ');
	if (a == std::string::npos) return raw;
	return out.substr(a, b - a + 1);
}

// ===========================================================================
// CleanEntityName — MEMOIZADO
//
// A funcao acima e' PURA: o resultado depende so' de `(raw, firstTokenOnly)`.
// Mesmo assim ela era chamada por item de loot e por veiculo A CADA FRAME,
// refazendo sempre a mesma conta para os mesmos nomes:
//
//   30 comparacoes de prefixo + 8 de sufixo + 17 `find` que varrem a string
//   inteira + 7 `==` + um laco char-a-char + 2-3 alocacoes de heap.
//
// Os nomes vem de um conjunto FINITO (as classes do jogo), entao o cache
// acerta praticamente sempre e o custo por chamada vira um hash + um lookup.
//
// POR QUE E' SEGURO:
//   - e' funcao pura, entao o valor guardado nunca fica "velho";
//   - todos os chamadores estao na THREAD DE RENDER (DrawVehicleEsp,
//     DrawLootEsp e a aba WORLD), entao os dois `static` nao precisam de
//     lock. SE ALGUM DIA ALGUEM CHAMAR ISTO DA THREAD DE MEMORIA, PRECISA
//     DE MUTEX — por isso o aviso esta' aqui em cima e nao num commit.
//   - o tamanho tem teto: se aparecer nome dinamico, o cache para de
//     crescer em vez de virar vazamento.
//
// O RESULTADO E' IDENTICO ao de antes, byte a byte.
// ===========================================================================
// DEVOLVE REFERENCIA (28/10): antes voltava copia por valor — no HIT do
// memo isso custava uma alocacao por item de loot/veiculo por frame. Os
// nodes do unordered_map tem endereco estavel (garantia da STL), entao o
// referencia para `it->second` vive ate' o proximo clear/rehash... e rehash
// NAO invalida referencia, so' muda de bucket. O caso raro de cache CHEIO +
// miss usa um scratch file-static seguro (thread de render unica — ver
// acima). Os chamadores PODEM guardar como `const std::string&`.
const std::string& CleanEntityName(const std::string& raw, bool firstTokenOnly)
{
	static std::unordered_map<std::string, std::string> memoSimples;
	static std::unordered_map<std::string, std::string> memoPrimeiro;
	static std::string scratchForaDoTeto; // miss com mapa cheio (conjunto finito: quase nunca)
	auto& memo = firstTokenOnly ? memoPrimeiro : memoSimples;

	const auto it = memo.find(raw);
	if (it != memo.end()) { Perf::Contar(Perf::C.memoHit); return it->second; }
	Perf::Contar(Perf::C.memoMiss);

	std::string limpo = CleanEntityNameSemCache(raw, firstTokenOnly);

	// Teto: nome de classe e' conjunto finito, mas se um dia entrar algo
	// dinamico o cache para de crescer em vez de vazar memoria.
	if (memo.size() < 4096)
		return memo.emplace(std::move(raw), std::move(limpo)).first->second;
	scratchForaDoTeto = std::move(limpo);
	return scratchForaDoTeto;
}

// ===========================================================================
// Categorias do Loot ESP.
//
// A classificacao e o catalogo (nome BRUTO -> categoria) moram em
// ESP/LootCatalog.h, porque a aba WORLD em Main.cpp precisa da MESMA tabela
// pra montar os grupos. Aqui so' fica a COR de cada grupo.
//
// Historico: a versao antiga classificava por palavra-chave no nome JA'
// limpo, com categorias diferentes (LC_SCOPE/LC_EQUIP). Os padroes de agora
// sao por prefixo do nome BRUTO, medidos em log real — "Item_Attach_" ANTES
// de "Item_Weapon_" (senao acessorio vira arma).
// ===========================================================================

// Cor por categoria (facilita achar o que interessa no chao)
static ImU32 LootCatColor(LootCategory c)
{
	switch (c) {
	case LootCategory::Weapons:     return IM_COL32(255, 120, 120, 230);   // vermelho claro
	case LootCategory::Ammo:        return IM_COL32(255, 220, 120, 230);   // amarelo
	case LootCategory::Attachments: return IM_COL32(120, 200, 255, 230);   // azul
	case LootCategory::Health:      return IM_COL32(120, 255, 160, 230);   // verde
	case LootCategory::Boost:       return IM_COL32(90, 255, 220, 230);    // turquesa
	case LootCategory::Helmet:      return IM_COL32(255, 180, 90, 230);    // laranja
	case LootCategory::Backpack:    return IM_COL32(200, 160, 255, 230);   // lilas
	case LootCategory::Vest:        return IM_COL32(255, 140, 220, 230);   // rosa
	case LootCategory::Throwables:  return IM_COL32(255, 80, 80, 240);     // vermelho forte
	case LootCategory::Airdrop:     return IM_COL32(255, 205, 60, 245);    // dourado
	case LootCategory::DeathBox:    return IM_COL32(255, 120, 220, 245);   // magenta
	default:                        return IM_COL32(190, 190, 190, 200);   // cinza
	}
}

void DrawPlayerEsp(const SharedData& data)
{
	float minDis = 9999.0f;
	auto drawList = ImGui::GetBackgroundDrawList();

	EspBoxSmoothPurge(data.Actors); // estado de smooth de ator que saiu (28/10)
	for (auto& entity : data.Actors)   // por referencia: era copia de shared_ptr por ator por frame
	{
		if (!Configs.Survivor.PlayerEspEnabled) break;
		if (!entity) continue;
		if (entity->isDie) continue;
		// POSICAO MORTA: o RootComponent parou de ser lido ha' varios ticks
		// (ator saiu da partida / objeto reciclado). Sem este filtro o ESP
		// desenharia a caixa na ULTIMA posicao boa — o "ESP fantasma". Ver a
		// nota de `posFalhas`/`isStale` em ActorEntity.h.
		if (entity->isStale) continue;
		if (entity->PlayerType == 0) continue; // Unidentified/Local
		if (entity->Class == data.AcknowledgedPawn || entity->Class == data.LocalCharacterPawn) continue; // Skip local player

		// -----------------------------------------------------------------
		// ALIADO NAO ENTRA NO ESP.
		//
		// Este e' um filtro de DESENHO, e e' coisa DIFERENTE do skip de aliado
		// que existe no `Engine::UpdatePlayers`. Aquele e' de COMPORTAMENTO
		// (nao mirar nem compensar recuo em quem e' do time) — e foi
		// justamente por ele estar ANTES do bloco de posicao que o aliado
		// ficava com posicao congelada e virava o "esp fantasma".
		//
		// Agora o dado do aliado e' lido e atualizado como o de qualquer um
		// (posicao, vida, nome) e simplesmente NAO E' DESENHADO. Ler e
		// desenhar sao decisoes separadas de proposito: separar as duas e' o
		// que impede o proximo bug dessa familia.
		//
		// `Teamid != 0` importa: enquanto o time do local nao foi lido, ele e'
		// 0 — e todo mundo tambem esta' com `LastTeamNum` 0. Sem esta guarda,
		// nos primeiros ticks TODO MUNDO seria tratado como aliado e o ESP
		// ficaria vazio por um instante.
		// -----------------------------------------------------------------
		if (Local.Teamid != 0 && entity->LastTeamNum == Local.Teamid) continue;

		Vector3 actorPos = entity->GetPosition();
		if (actorPos.IsZero()) continue;
		// [DRAW SANITY] `IsZero()` e' `!x && !y` (Vector.cpp:43): NAO barra NaN
		// nem +-inf. Todo o resto deste laco (projecao, caixa, esqueleto, barra
		// de vida, snapline, texto) depende desta posicao — entao ela e' a
		// primeira a ser validada.
		if (!DrawSanity::MundoOk(actorPos, "player")) continue;

		auto& config = Configs.Survivor;

		// Raw Debug Drawing (Always draw a small dot/text for any discovered actor if in debug)
		Vector2 rootScreen = Camera::WorldToScreen(data.CameraCache.POV, actorPos);
		if (!rootScreen.IsZero() && RunDrawingTest &&
		    DrawSanity::TelaOk(rootScreen.x, rootScreen.y, "player", actorPos)) { // Use RunDrawingTest as a proxy for "Draw Everything"
			char debugInfo[128];
			sprintf(debugInfo, "[%d] %s", entity->id, entity->objName.c_str());
			EspText(debugInfo, Vec2(rootScreen.x, rootScreen.y), IM_COL32(255, 255, 0, 200), 12.0f);
		}

		if (entity->PlayerType == 0) continue; // Skip non-players for main ESP

		// Calculate Head and Foot positions for the box
		Vector3 headPos;
		Vector3 footPos;

		if (entity->bHasBones) {
			headPos = Vector3(entity->Head3D.X, entity->Head3D.Y, entity->Head3D.Z + 10.0f); // Top of head + helmet margin
			
			// Use the lowest ball-of-foot bone as the actual base of the player (ground level)
			Vector3 ballL = Vector3(entity->Lball3D.X, entity->Lball3D.Y, entity->Lball3D.Z);
			Vector3 ballR = Vector3(entity->Rball3D.X, entity->Rball3D.Y, entity->Rball3D.Z);
			footPos = (ballL.z < ballR.z) ? ballL : ballR; // Z is up in Unreal Engine

			// Fallback to foot bones if ball reads fail
			if (footPos.IsZero()) {
				Vector3 footL = Vector3(entity->Lfoot3D.X, entity->Lfoot3D.Y, entity->Lfoot3D.Z);
				Vector3 footR = Vector3(entity->Rfoot3D.X, entity->Rfoot3D.Y, entity->Rfoot3D.Z);
				footPos = (footL.z < footR.z) ? footL : footR;
			}

			// Fallback to actorPos if all bone reads fail
			if (footPos.IsZero()) {
				footPos = actorPos;
			}
		} else {
			// Estimate based on actorPos (since actorPos is elevated at the pelvis, adjust up and down)
			headPos = Vector3(actorPos.x, actorPos.y, actorPos.z + 90.0f);
			footPos = Vector3(actorPos.x, actorPos.y, actorPos.z - 90.0f);
		}

		// [DRAW SANITY] Teto/pes vem dos OSSOS (leitura de jogo). Um osso lixo
		// pode ser NaN ou ~1e38; o segundo caso e' o pior porque ele passa por
		// qualquer `IsZero()` e projeta para uma coordenada de tela PLAUSIVEL
		// (inf/inf). Rejeitar aqui e' o que impede que a caixa/skeleton sejam
		// desenhados a partir de lixo.
		if (!DrawSanity::MundoOk(headPos, "player") || !DrawSanity::MundoOk(footPos, "player"))
			continue;

		Vector2 HeadScreen = Camera::WorldToScreen(data.CameraCache.POV, headPos);
		Vector2 FootScreen = Camera::WorldToScreen(data.CameraCache.POV, footPos);

		if (HeadScreen.IsZero() || FootScreen.IsZero()) continue;

		// [DRAW SANITY] O crash do dump RuntimeBroker.exe.15084 nasceu de uma
		// projecao NAO-FINITA que passou por este ponto: `IsZero()` so' olha
		// zero exato. `height = abs(HeadScreen.y - FootScreen.y)` com as duas
		// pontas em +inf da' NaN, `width = height/2` tambem, e a caixa ia para
		// o ImGui com tamanho NaN (PlayerEsp.cpp:504).
		if (!DrawSanity::TelaOk(HeadScreen.x, HeadScreen.y, "player", actorPos) ||
		    !DrawSanity::TelaOk(FootScreen.x, FootScreen.y, "player", actorPos))
			continue;

		Vector3 campos = Vector3(data.CameraCache.POV.Location.X, data.CameraCache.POV.Location.Y, data.CameraCache.POV.Location.Z);
		float distance = (Vector3::Distance(campos, actorPos) / 100.0f);

		// [DRAW SANITY] Comparacao com NaN e' FALSA: uma distancia NaN passaria
		// direto pelo `> MaxDistance` abaixo e iria para o texto/distancia.
		if (!std::isfinite(distance)) continue;

		// Skip rendering for players outside the configured ESP Render Distance
		if (distance > config.MaxDistance) continue;

		// -----------------------------------------------------------------
		// TEXTO (nome / distancia / arma).
		//
		// BUG CORRIGIDO: antes o bloco terminava com
		//     else continue;
		// quando Nome e Distancia estavam os DOIS desligados. Esse `continue`
		// nao matava so' o texto: ele pulava o ator inteiro e, com ele, Box,
		// Skeleton, Health Bar e Snaplines. Ou seja, tudo dependia de
		// "Nome || Distancia" estar ligado.
		//
		// Agora o bloco so' e' montado se alguma das tres opcoes de texto
		// estiver ligada e, se nada tiver o que mostrar, nao escreve nada —
		// sem `continue`.
		// -----------------------------------------------------------------
		// -----------------------------------------------------------------
		// CADA INFORMACAO VIRA UM ELEMENTO SEPARADO.
		//
		// Antes tudo isto era concatenado numa UNICA string (`wdistance`) e
		// desenhada num ponto FIXO abaixo do pe'. Agora cada pedaco tem a sua
		// string e o LAYOUT decide onde ele cai (ver `EspResolver` acima).
		//
		// Um buffer por elemento — e nao um so' com separadores — e' o que
		// torna possivel "nome em cima, distancia embaixo".
		// -----------------------------------------------------------------
		char elName[160]  = {0};
		char elWeapon[64] = {0};
		char elKills[24]  = {0};
		char elDist[24]   = {0};
		char elBot[16]    = {0};
		char elTeam[16]   = {0};

		{
			// Nome REAL do jogador (lido do PlayerState). Se vazio, cai no
			// rotulo generico por tipo.
			std::string pname;
			std::wstring wn = entity->GetName();
			if (!wn.empty()) pname.assign(wn.begin(), wn.end());
			if (pname.empty()) pname = (entity->PlayerType == 1) ? "AI" : "Player";
			if (entity->bIsDBNO) pname += " [DBNO]";

			// NOME DA ARMA JA' ENCURTADO (28/10) — o corte de prefixo/sufixo
			// (BP_Weapon_AK47_C -> AK47) migrou para o UpdateWeapons, que
			// mantem `WeaponNameCurto` so' quando o nome MUDA. Antes o ESP
			// refazia esse corte em um std::string LOCAL por ator por frame
			// (alocacao de heap, nome de arma tem >16 chars). Agora e' um
			// referencia: zero copia. O gate antigo (vazio / UNKOWNNAME)
			// continua valendo pelo campo cheio.
			static const std::string vazioArma;
			const std::string& weapon = (!entity->WeaponName.empty() && entity->WeaponName != "UNKOWNNAME")
			                            ? entity->WeaponNameCurto : vazioArma;

			// Cada elemento atras da SUA opcao (nomes de config preservados).
			//
			// [S<n>] e [BOT]/[P] continuam INDEPENDENTES: o time precisa do
			// TeamNumber (GameState/roster), o tipo sai do PlayerType do ator.
			if (config.Name)
				snprintf(elName, sizeof(elName), "%s", pname.c_str());
			if (config.Weapon && !weapon.empty())
				snprintf(elWeapon, sizeof(elWeapon), "%s", weapon.c_str());
			if (entity->Kills > 0)
				snprintf(elKills, sizeof(elKills), "%dK", entity->Kills);
			if (config.Distance)
				snprintf(elDist, sizeof(elDist), "[%dm]", (int)distance);
			if (config.ShowBotFlag)
				snprintf(elBot, sizeof(elBot), "%s", entity->bIsBot ? "[BOT]" : "[P]");
			if (config.ShowTeamInfo && entity->TeamNumber > 0)
				snprintf(elTeam, sizeof(elTeam), "[S%d]", entity->TeamNumber);
		}

		if (!HeadScreen.IsZero() && !FootScreen.IsZero())
		{
			if (entity->LastTeamNum != Local.Teamid) {
				if (distance < minDis) {
					minDis = distance;
				}
			}

            auto getCol32 = [](Colour c) -> ImU32 { return IM_COL32(c.r, c.g, c.b, c.a); };
			// -----------------------------------------------------------------
			// COR: `IsVisible` SO' PEGA AQUI QUANDO O VISCHECK DO ESP ESTA' ON.
			//
			// `IsVisible` tambem e' calculado quando o AIMBOT esta' ligado com
			// `VisibilityCheck` (o Engine liga a leitura se qualquer um dos
			// dois consumidores pedir). Sem este `config.VisCheck` no meio, o
			// ESP mudaria de cor como EFEITO COLATERAL de ligar o aimbot — e o
			// usuario iria caçar bug no ESP.
			//
			// Cada consumidor tem o seu toggle; o dado medido e' um so'.
			// -----------------------------------------------------------------
			const bool oculto = config.VisCheck && !entity->IsVisible;
			ImU32 boxColor      = !oculto ? getCol32(config.BoxColour)
			                              : getCol32(config.TextColour);
			ImU32 skeletonColor = getCol32(config.SkeletonColour);
			if (oculto) skeletonColor = getCol32(config.TextColour);

			// DBNO (deitado/knocked): cor propria, pra nao confundir com alvo vivo
			if (entity->bIsDBNO) {
				boxColor      = getCol32(config.DbnoColour);
				skeletonColor = getCol32(config.DbnoColour);
			}

			// Draw Box — use bone spread when available for accurate width
			float height = abs(HeadScreen.y - FootScreen.y);
			float centerX = (HeadScreen.x + FootScreen.x) / 2.0f;
			float width = height / 2.0f; // Default ratio fallback

			if (config.Box) {
				// Espessura do traco vinda do config (PlayerConfig.h,
				// `BoxThickness`). Resolvida UMA vez por ator, fora dos tres
				// caminhos de desenho, para os tres usarem o mesmo valor.
				const float tracoCaixa = EspTraco(config.BoxThickness);

				// Retangulo FINAL (left, top) — o fallback e' o mesmo do
				// antes (centro na projecao head/foot, topo no head). O
				// caminho de ossos so' refina os quatro valores quando a
				// projecao da; o DESENHO saiu de dentro dos branches para o
				// ponto unico abaixo, ja' suavizado (28/10 — BoxSmooth).
				float left = centerX - width / 2.0f;
				float top  = HeadScreen.y;

				if (entity->bHasBones) {
					// Project all key bones to find actual screen-space bounding box
					float minX = 99999.0f, maxX = -99999.0f;
					float minY = 99999.0f, maxY = -99999.0f;

					UEVector bonesToProject[] = {
						entity->Head3D, entity->neck3D, entity->pelvis3D,
						entity->Lshoulder3D, entity->Lelbow3D, entity->Lhand3D,
						entity->Rshoulder3D, entity->Relbow3D, entity->Rhand3D,
						entity->Lbuttock3D, entity->Lknee3D, entity->Lfoot3D, entity->Lball3D,
						entity->Rbuttock3D, entity->Rknee3D, entity->Rfoot3D, entity->Rball3D
					};

					for (auto& bone : bonesToProject) {
						Vector2 bp = Camera::WorldToScreen(data.CameraCache.POV, Vector3(bone.X, bone.Y, bone.Z));
						if (bp.IsZero()) continue;
						// [DRAW SANITY] Um osso NAO-FINITO envenenava minX/maxX:
						// `maxX > minX` com NaN e' FALSO, entao a caixa caia' no
						// fallback (linha 562) E as bordas ficavam NaN — foi esse
						// o combo do crash. Osso rejeitado nao entra no min/max.
						if (!DrawSanity::TelaOk(bp.x, bp.y, "player", actorPos)) continue;
						if (bp.x < minX) minX = bp.x;
						if (bp.x > maxX) maxX = bp.x;
						if (bp.y < minY) minY = bp.y;
						if (bp.y > maxY) maxY = bp.y;
					}

					if (maxX > minX && maxY > minY) {
						float boneWidth = (maxX - minX);
						float boneHeight = (maxY - minY);
						// Add 15% padding for gear/backpack/weapon
						float padX = boneWidth * 0.15f;
						float padY = boneHeight * 0.08f;
						centerX = (minX + maxX) / 2.0f;
						width = boneWidth + padX * 2.0f;
						height = boneHeight + padY * 2.0f;
						left = centerX - width / 2.0f;
						top = minY - padY;
					}
					// projecao falhou -> left/top/w/h seguem o fallback acima
				}

				EspBoxRectSuave(entity->Class, left, top, width, height);
				Gui.Rectangle(Vec2(left, top), Vec2(width, height), boxColor, tracoCaixa);
			}

			// -----------------------------------------------------------------
			// SNAPLINES: linha da base (ou do centro) da tela ate' o inimigo.
			// Ajuda a localizar rapido quem esta' fora do campo de visao direto.
			//
			// BUG CORRIGIDO: este bloco estava ANINHADO dentro de
			//     if (config.Box) { if (bHasBones) { if (projecao ok) { ... } } }
			// entao a snapline so' aparecia com o Box ligado (e, pior, so'
			// quando a projecao dos ossos dava certo). Agora ele e' irmao do
			// Box, do Skeleton e da Health Bar: ShowSnaplines decide sozinho.
			//
			// `centerX`/`width` continuam sendo calculados acima (e o Box pode
			// refina-los a partir dos ossos); para o ponto de chegada da linha
			// o que importa e' centerX + FootScreen.y, que existem sempre.
			// -----------------------------------------------------------------
			if (config.ShowSnaplines) {
				const float telaL = Gui.Window.Size.x;
				const float telaA = Gui.Window.Size.y;
				Vec2 origem = config.SnaplineFromCenter
				            ? Vec2(telaL * 0.5f, telaA * 0.5f)
				            : Vec2(telaL * 0.5f, telaA);
				Gui.Line(origem, Vec2(centerX, FootScreen.y), boxColor, 1.0f);
			}

			// =================================================================
			// LAYOUT — UM PASSE DE POSICIONAMENTO, DOIS DE DESENHO.
			//
			// O empilhamento (`EspStack`) e' POR ATOR e COMPARTILHADO entre a
			// barra de vida e os textos: se cada um tivesse a sua pilha, os
			// dois brigariam pela mesma faixa ao cair na mesma ancora.
			//
			// Por isso TODAS as posicoes sao resolvidas aqui, num passe so', e
			// o desenho acontece depois — preservando a ordem de sempre:
			// barra de vida -> esqueleto -> texto.
			//
			// A resolucao vem DEPOIS do Box de proposito: o Box refina
			// `width`/`height` a partir dos ossos, e as ancoras laterais
			// dependem de `halfW`.
			//
			// Elemento SEM conteudo nao entra na pilha: um rotulo vazio nao
			// pode deixar buraco no empilhamento dos outros.
			// =================================================================
			EspBox caixa;
			caixa.cx     = centerX;
			caixa.top    = HeadScreen.y;
			caixa.bottom = FootScreen.y;
			caixa.halfW  = width * 0.5f;

			EspStack pilha;
			Vec2     posEl[ESP_EL_COUNT];
			bool     centroEl[ESP_EL_COUNT] = { false };

			const char* textoEl[ESP_EL_COUNT] = {
				nullptr,    // ESP_EL_HEALTH — e' barra, nao texto
				elName,     // ESP_EL_NAME
				elWeapon,   // ESP_EL_WEAPON
				elKills,    // ESP_EL_KILLS
				elDist,     // ESP_EL_DIST
				elBot,      // ESP_EL_BOT
				elTeam,     // ESP_EL_TEAM
			};

			float textSizes[ESP_EL_COUNT] = {};

			// -----------------------------------------------------------------
			// DIMENSOES DA BARRA — E ELA VIRA SE A ANCORA FOR LATERAL.
			//
			// Barra HORIZONTAL saindo pro lado da caixa fica errada: atravessa a
			// tela em vez de acompanhar o corpo. Nas ancoras ESQUERDA/DIREITA
			// ela fica VERTICAL, do tamanho da caixa — e' a convencao de
			// qualquer ESP e a unica leitura que faz sentido naquela posicao.
			//
			// `barEsp` e' a ESPESSURA (o lado curto), nos DOIS casos. E' ela
			// que define o arredondamento e o tamanho minimo — se fosse a
			// altura, a versao vertical arredondaria com raio ~75 px e viraria
			// uma bolha.
			//
			// Calculadas AQUI (e nao no bloco de desenho) porque o
			// posicionamento precisa delas.
			// -----------------------------------------------------------------
			float barEsp = height * 0.04f;
			if (barEsp < 3.0f) barEsp = 3.0f;
			if (barEsp > 6.0f) barEsp = 6.0f;

			const int  ancoraSaude = config.EspLayout[ESP_EL_HEALTH].anchor;
			const bool barVertical = (ancoraSaude == ESP_ANC_LEFT ||
			                          ancoraSaude == ESP_ANC_RIGHT);

			const float barW = barVertical ? barEsp : width;
			const float barH = barVertical ? height : barEsp;

			if (config.Health) {
				const EspElementLayout& L = config.EspLayout[ESP_EL_HEALTH];
				bool kc = false;
				posEl[ESP_EL_HEALTH]    = EspResolver(caixa, L.anchor, L.dx, L.dy, barW, barH, pilha, kc);
				centroEl[ESP_EL_HEALTH] = kc;
			}

			for (int e = 1; e < ESP_EL_COUNT; ++e) {
				if (!textoEl[e] || textoEl[e][0] == '\0') continue;
				const EspElementLayout& L = config.EspLayout[e];
				textSizes[e] = static_cast<float>(e == ESP_EL_NAME ? config.PlayerTextSize : config.SecondaryTextSize);
                const ImVec2 sz = EspTypography::measure(textoEl[e], textSizes[e]);
				bool kc = false;
				posEl[e]    = EspResolver(caixa, L.anchor, L.dx, L.dy, sz.x, sz.y, pilha, kc);
				centroEl[e] = kc;
			}

			// Draw Horizontal Health Bar
			if (config.Health) {
				// -----------------------------------------------------------------
				// A VIDA E' 0..100. SEM HEURISTICA.
				//
				// Antes:
				//     healthPct = (Health > 1.5f) ? Health : (Health * 100.0f);
				//
				// A intencao era cobrir builds que devolvessem FRACAO (0..1) em
				// vez de percentual. So' que a faixa real e' 0..100 — os logs
				// mostram `hp= 1.00`, `hp= 82.21`, `hp= 89.21`, `hp= 100.00` —
				// entao TUDO abaixo de 1.5 era multiplicado por 100.
				//
				// Resultado medido pelo testador: inimigo com 1 de vida exibia
				// barra CHEIA. E o inverso tambem acontecia: 0.5 virava 50%.
				//
				// A heuristica nao protegia contra nada — `DecryptHealth`
				// devolve o float cru do jogo, e esse float sempre foi 0..100.
				// -----------------------------------------------------------------
				float healthPct = entity->Health;
				// [DRAW SANITY] `entity->Health` NaN passa por TODOS os clamps
				// abaixo (qualquer comparacao com NaN e' falsa), `healthFrac`
				// vira NaN, `fillW` vira NaN — e o AddRectFilled desta barra
				// usa canto arredondado (rounding = barHeight/2): mesmo caminho
				// do crash da caixa. Vira 0 = "sem vida" e a barra sai vazia.
				if (!std::isfinite(healthPct)) healthPct = 0.0f;
				if (healthPct > 100.0f) healthPct = 100.0f;
				if (healthPct < 0.0f) healthPct = 0.0f;
				float healthFrac = healthPct / 100.0f;

				// Dimensoes: as MESMAS usadas no passe de layout (barW/barH).
				// Se fossem recalculadas aqui, o desenho e o posicionamento
				// poderiam divergir e a barra sairia fora da ancora.
				float barWidth  = barW;
				float barHeight = barH;

				// Posicao: vem da ancora. `centroEl` diz se o X resolvido e' o
				// CENTRO (ancoras de topo/base) ou a borda ESQUERDA (laterais e
				// quinas) — a mesma convencao do KeepCenter do texto.
				float barX = centroEl[ESP_EL_HEALTH]
				           ? (posEl[ESP_EL_HEALTH].x - barWidth * 0.5f)
				           :  posEl[ESP_EL_HEALTH].x;
				float barY = posEl[ESP_EL_HEALTH].y;

				// Health color
				ImU32 healthColor;
				if (healthPct < 30.0f)
					healthColor = IM_COL32(255, 40, 40, 255);
				else if (healthPct < 60.0f)
					healthColor = IM_COL32(255, 220, 50, 255);
				else
					healthColor = IM_COL32(0, 255, 80, 255);

				// Canto = METADE DA ESPESSURA (o lado CURTO), nos dois sentidos.
				// Era `barHeight * 0.5f`: na barra vertical isso daria metade da
				// ALTURA da caixa (~75 px) e o retangulo viraria uma bolha.
				const float espBarra = barVertical ? barWidth : barHeight;
				const float rounding = espBarra * 0.5f;

				// Fundo (escuro)
				drawList->AddRectFilled(ImVec2(barX, barY),
				                        ImVec2(barX + barWidth, barY + barHeight),
				                        IM_COL32(0, 0, 0, 180), rounding);

				// Parte preenchida — e o SENTIDO acompanha a orientacao:
				//   horizontal -> enche da ESQUERDA pra direita;
				//   vertical   -> enche de BAIXO pra CIMA (e' assim que se le'
				//                 vida ao lado de um boneco; encher de cima pra
				//                 baixo parece "dano" em vez de "vida").
				if (healthFrac > 0.01f) {
					// Minimo para o canto arredondado ter o que arredondar.
					float preenchido = (barVertical ? barHeight : barWidth) * healthFrac;
					if (preenchido < rounding * 2.0f) preenchido = rounding * 2.0f;

					if (barVertical) {
						drawList->AddRectFilled(ImVec2(barX, barY + barHeight - preenchido),
						                        ImVec2(barX + barWidth, barY + barHeight),
						                        healthColor, rounding);
					} else {
						drawList->AddRectFilled(ImVec2(barX, barY),
						                        ImVec2(barX + preenchido, barY + barHeight),
						                        healthColor, rounding);
					}
				}

				// Contorno
				drawList->AddRect(ImVec2(barX, barY),
				                  ImVec2(barX + barWidth, barY + barHeight),
				                  IM_COL32(0, 0, 0, 220), rounding, 0, 1.0f);
			}

			// Draw Skeleton
			if (config.Skeleton && entity->bHasBones) {
				// Espessura do esqueleto vinda do config (PlayerConfig.h,
				// `SkeletonThickness`). Resolvida fora do bloco de linhas para
				// os 14 segmentos usarem exatamente o mesmo traco — antes era
				// o literal 1.0f repetido catorze vezes, que e' como um deles
				// fica diferente numa edicao futura.
				const float tracoEsq = EspTraco(config.SkeletonThickness);
				// [DRAW SANITY] O esqueleto e' 17 projecoes de osso ligadas por
				// 14 linhas. Uma unica ponta NaN nao estoura o AddLine (ele nao
				// tem canto arredondado), mas contamina os vertices do frame e
				// desenha linha atravessando a tela. `bonesOk` faz o bloco
				// inteiro depender de TODAS as pontas estarem validas.
				bool bonesOk = true;
				auto W2S = [&](UEVector world) -> Vec2 {
					Vector2 screen = Camera::WorldToScreen(data.CameraCache.POV, Vector3(world.X, world.Y, world.Z));
					if (!DrawSanity::TelaOk(screen.x, screen.y, "player", actorPos))
						bonesOk = false;
					return Vec2(screen.x, screen.y);
				};

				Vec2 head = W2S(entity->Head3D);
				Vec2 neck = W2S(entity->neck3D);
				Vec2 pelvis = W2S(entity->pelvis3D);
				Vec2 lShoulder = W2S(entity->Lshoulder3D), lElbow = W2S(entity->Lelbow3D), lHand = W2S(entity->Lhand3D);
				Vec2 rShoulder = W2S(entity->Rshoulder3D), rElbow = W2S(entity->Relbow3D), rHand = W2S(entity->Rhand3D);
				Vec2 lButtock = W2S(entity->Lbuttock3D), lKnee = W2S(entity->Lknee3D), lFoot = W2S(entity->Lfoot3D), lBall = W2S(entity->Lball3D);
				Vec2 rButtock = W2S(entity->Rbuttock3D), rKnee = W2S(entity->Rknee3D), rFoot = W2S(entity->Rfoot3D), rBall = W2S(entity->Rball3D);

				if (bonesOk) {
					// Spine
					Gui.Line(head, neck, skeletonColor, tracoEsq);
					Gui.Line(neck, pelvis, skeletonColor, tracoEsq);

					// Left Arm
					Gui.Line(neck, lShoulder, skeletonColor, tracoEsq);
					Gui.Line(lShoulder, lElbow, skeletonColor, tracoEsq);
					Gui.Line(lElbow, lHand, skeletonColor, tracoEsq);

					// Right Arm
					Gui.Line(neck, rShoulder, skeletonColor, tracoEsq);
					Gui.Line(rShoulder, rElbow, skeletonColor, tracoEsq);
					Gui.Line(rElbow, rHand, skeletonColor, tracoEsq);

					// Left Leg (4-segment: hip→knee→foot→ball)
					Gui.Line(pelvis, lButtock, skeletonColor, tracoEsq);
					Gui.Line(lButtock, lKnee, skeletonColor, tracoEsq);
					Gui.Line(lKnee, lFoot, skeletonColor, tracoEsq);
					Gui.Line(lFoot, lBall, skeletonColor, tracoEsq);

					// Right Leg (4-segment: hip→knee→foot→ball)
					Gui.Line(pelvis, rButtock, skeletonColor, tracoEsq);
					Gui.Line(rButtock, rKnee, skeletonColor, tracoEsq);
					Gui.Line(rKnee, rFoot, skeletonColor, tracoEsq);
					Gui.Line(rFoot, rBall, skeletonColor, tracoEsq);
				}
			}

	

            // -----------------------------------------------------------------
			// TEXTO DO ESP — UM DESENHO POR ELEMENTO, NA POSICAO DA ANCORA.
			//
			// Antes: UMA string concatenada, num ponto FIXO abaixo do pe'.
			// As posicoes ja' vieram resolvidas no passe de layout (junto com a
			// barra de vida, para as duas dividirem a mesma pilha) — aqui so'
			// desenha.
			//
			// O `centroEl` e' repassado como `KeepCenter` do OSImGui: e' o que
			// faz "topo/base" centrar na caixa e "esquerda/direita/quina" usar
			// o X resolvido como borda.
			//
			// O gate de desenho e' o CONTEUDO de cada elemento, e nao mais um
			// "Name || Distance": com elementos separados, "so' arma" e
			// "so' kills" passaram a ser combinacoes legitimas.
			// -----------------------------------------------------------------
				// -----------------------------------------------------------------
			// Keep the chosen contrast consistent at every distance.
			for (int e = 1; e < ESP_EL_COUNT; ++e) {
				if (!textoEl[e] || textoEl[e][0] == '\0') continue;
				EspStrokeText(textoEl[e], posEl[e], boxColor, textSizes[e], centroEl[e]);
			}
		}
	}

	// =====================================================================
	// HUD DE TELA — contador de espectadores + inimigo mais proximo.
	//
	// ANTES: duas linhas em posicao FIXA (centro-x - 30, y = altura-100 e
	// y = 100). Nunca dava pra tirar de cima do minimapa/contador do jogo.
	//
	// AGORA: uma posicao so' (HudX/HudY, salva no config), as duas linhas
	// logo abaixo dela, um FUNDO unico atras das duas e ARRASTO com o botao
	// esquerdo.
	//
	// ⚠️ O ARRASTO SO' FUNCIONA COM O MENU ABERTO — e isso NAO e' limitacao
	// deste codigo:
	//   com o menu FECHADO a janela do overlay tem WS_EX_TRANSPARENT
	//   (DirectOverlay::ToggleMenu), ou seja o Windows entrega o clique pro
	//   JOGO e o overlay nunca ve' o mouse. Sem receber o clique nao existe
	//   "segurar e arrastar". Com o menu ABERTO o ex-style perde o
	//   WS_EX_TRANSPARENT e o overlay recebe o input normalmente.
	// Por isso o gate e' `DirectOverlay::ShowMenu`: fora dele o HUD so' e'
	// desenhado (menu fechado = jogando = nada de input).
	//
	// A POSICAO NAO SE PERDE: HudX/HudY vao pro JSON (ToJson/FromJson em
	// PlayerConfig.h) e o auto-save roda quando o menu fecha
	// (DirectOverlay::ToggleMenu -> AutoSaveConfig). Arrastar e apertar HOME
	// pra fechar o menu ja' grava o novo lugar.
	//
	// O fundo e' desenhado AQUI, uma vez para as duas linhas, e nao por
	// linha: um retangulo por texto deixaria uma emenda visivel no meio.
	// =====================================================================
	{
		char hudLinha1[64] = {0};   // espectadores
		char hudLinha2[64] = {0};   // inimigo mais proximo

		const bool temEspectadores = Configs.Survivor.ShowSpectators &&
		                             Local.SpectatedCount >= 0 && Local.SpectatedCount < 100;
		if (temEspectadores)
			sprintf(hudLinha1, L("Spectators: %d"), Local.SpectatedCount);
		if (minDis < 9999.0f)
			sprintf(hudLinha2, L("Closest Enemy: %dm"), (int)minDis);

		// Se nenhuma das duas linhas tem conteudo nao ha' HUD nenhum: nao
		// avalia mouse nem desenha fundo (antes ficava um retangulo vazio
		// boiando — e, pior, ele ainda podia ser "arrastado" sem nada dentro).
		if (hudLinha1[0] != '\0' || hudLinha2[0] != '\0')
		{
			// As duas linhas sao medidas com o MESMO tamanho de fonte que o
			// desenho usa (15px do ESP — ver EspTextSize). Medir com
			// ImGui::CalcTextSize (fonte do MENU) daria um painel errado,
			// porque GEspFont e a do menu sao fontes diferentes.
			const float hudFonte = 15.0f;
			const ImVec2 medida1 = hudLinha1[0] ? EspTextSize(hudLinha1, hudFonte) : ImVec2(0.0f, 0.0f);
			const ImVec2 medida2 = hudLinha2[0] ? EspTextSize(hudLinha2, hudFonte) : ImVec2(0.0f, 0.0f);

			// Passo entre as linhas. Usa o tamanho da fonte (e nao a altura de
			// linha da fonte do menu, que e' outra): `size.y` da fonte do ESP a
			// 15px E' a altura de linha dela. Com o passo menor a segunda linha
			// sairia por baixo do retangulo.
			const float alturaLinha = medida1.y > 0.0f ? medida1.y
			                        : (medida2.y > 0.0f ? medida2.y : hudFonte + 2.0f);
			const bool  duasLinhas  = (hudLinha1[0] != '\0' && hudLinha2[0] != '\0');

			const float hudLargura = ((medida1.x > medida2.x) ? medida1.x : medida2.x) + 12.0f;  // padding 6px por lado
			const float hudAltura  = alturaLinha * (duasLinhas ? 2.0f : 1.0f) + 8.0f;           // 4px em cima/baixo

			// Posicao salva (HudX/HudY). Default 0,0 = canto superior esquerdo.
			float hudX = Configs.Survivor.HudX;
			float hudY = Configs.Survivor.HudY;
			// [DRAW SANITY] HudX/HudY vem do JSON (o usuario arrasta o HUD e o
			// auto-save grava). Um JSON editado na mao / corrompido pode trazer
			// `NaN` — e NaN de posicao e' o gatilho do crash do ImGui. Volta pro
			// canto (0,0) e segue desenhando, em vez de morrer.
			if (!DrawSanity::TelaOk(hudX, hudY, "hud")) {
				hudX = 0.0f;
				hudY = 0.0f;
			}
			ImGuiIO& io = ImGui::GetIO();

			// -------------------------------------------------------------
			// ARRASTO. So' com o menu aberto (ver a nota grande acima).
			//
			// A deteccao de hover e' por COORDENADA e nao por
			// InvisibleButton/Hovered de item de ImGui: o HUD e' desenhado no
			// BackgroundDrawList, fora de qualquer janela, e um item exigiria
			// uma janela hospedeira — foi de proposito que o HUD nunca teve
			// uma (ele tem que aparecer com o menu fechado tambem).
			//
			// `io.MousePos` e' coordenada de TELA e o overlay cobre a tela
			// inteira, entao o hit-test e' direto. As condicoes de
			// `MouseDelta/MousePos` validos vem do proprio ImGui (quando o
			// mouse esta' fora da janela eles ficam zerados): com delta 0 o
			// arrasto simplesmente nao move, sem precisar de tratamento extra.
			// -------------------------------------------------------------
			if (Configs.Survivor.HudDraggable && DirectOverlay::ShowMenu)
			{
				const bool sobreHud = io.MousePos.x >= hudX && io.MousePos.x <= hudX + hudLargura &&
				                      io.MousePos.y >= hudY && io.MousePos.y <= hudY + hudAltura;

				// Estatico de proposito: o "segurando" tem que sobreviver ao
				// frame. Se dependesse so' do hover, arrastar rapido (o mouse
				// sai do retangulo enquanto o botao esta' apertado) soltaria o
				// HUD no meio do movimento.
				static bool arrastandoHud = false;

				if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
					arrastandoHud = false;                       // soltou o botao
				else if (!arrastandoHud && sobreHud)
					arrastandoHud = true;                        // comecou em cima do HUD

				if (arrastandoHud)
				{
					const ImVec2 delta = io.MouseDelta;
					// Clamp: o HUD nao pode sair da tela (senao o usuario perde
					// o unico jeito de arrastar ele de volta). So' clampa com a
					// janela ja' dimensionada — antes do primeiro frame util o
					// Size e' 0 e o clamp esmagaria a posicao salva. O `max`
					// evita a faixa invertida quando o HUD for mais largo que a
					// tela (o ImClamp com max < min devolve o max).
					const float telaL = Gui.Window.Size.x;
					const float telaA = Gui.Window.Size.y;
					if (telaL > 1.0f) {
						const float xMax = (telaL - hudLargura > 0.0f) ? (telaL - hudLargura) : 0.0f;
						hudX = ImClamp(hudX + delta.x, 0.0f, xMax);
					}
					if (telaA > 1.0f) {
						const float yMax = (telaA - hudAltura > 0.0f) ? (telaA - hudAltura) : 0.0f;
						hudY = ImClamp(hudY + delta.y, 0.0f, yMax);
					}

					Configs.Survivor.HudX = hudX;   // ja' grava pro auto-save
					Configs.Survivor.HudY = hudY;

					// Realce enquanto arrasta: sem isso o usuario nao sabe que
					// pegou o HUD (nao existe cursor de software neste overlay).
					drawList->AddRect(ImVec2(hudX - 1.0f, hudY - 1.0f),
					                  ImVec2(hudX + hudLargura + 1.0f, hudY + hudAltura + 1.0f),
					                  IM_COL32(139, 92, 246, 200), 4.0f, 0, 1.5f);
				}
			}

			// FUNDO unico do HUD, desenhado ANTES das duas linhas.
			EspPanelBg(Vec2(hudX, hudY), hudLargura, hudAltura);

			// As linhas entram SEM fundo proprio (Opacity = 0): o painel acima
			// ja' cobre as duas. Um retangulo por linha deixaria uma faixa mais
			// escura no meio do HUD.
			if (hudLinha1[0] != '\0')
				EspBlockText(hudLinha1, Vec2(hudX + 6.0f, hudY + 4.0f), IM_COL32(255, 255, 255, 255), hudFonte);
			if (hudLinha2[0] != '\0')
				EspBlockText(hudLinha2, Vec2(hudX + 6.0f, hudY + 4.0f + alturaLinha), IM_COL32(255, 255, 255, 255), hudFonte);
		}
	}

	// --- Grenade / Molotov ESP ---
	DrawGrenadeEsp(data);
	// --- Vehicle / Loot ESP ---
	DrawVehicleEsp(data);
	DrawLootEsp(data);
}

// ===========================================================================
// VEHICLE ESP — caixa + nome + distancia + vida/combustivel
// ===========================================================================
void DrawVehicleEsp(const SharedData& data)
{
	if (!Configs.Survivor.Vehicle) return;

	Vector3 campos(data.CameraCache.POV.Location.X,
	               data.CameraCache.POV.Location.Y,
	               data.CameraCache.POV.Location.Z);

	// Dedup: o mesmo veiculo pode ter mais de um ator (corpo/partes). Se dois
	// atores de veiculo estiverem a menos de 400 UU, desenha so o primeiro.
	std::vector<Vector3> drawn;
	drawn.reserve(data.VehicleActors.size());   // sem reserve, realocava a cada push_back

	for (auto& v : data.VehicleActors) {
		if (!v) continue;
		Vector3 pos = v->GetPosition();
		if (pos.IsZero()) continue;
		// [DRAW SANITY] Ver a nota do DrawPlayerEsp: `IsZero()` NAO barra
		// NaN/+-inf. A posicao do veiculo e' usada na distancia, no dedup e na
		// projecao — e o dedup compara com `< 400.0f` (falso para NaN, entao
		// todos os atores de veiculo passariam).
		if (!DrawSanity::MundoOk(pos, "veiculo")) continue;

		float distance = Vector3::Distance(campos, pos) / 100.0f;
		if (!std::isfinite(distance)) continue;
		// Teto PROPRIO do veiculo (PlayerConfig::MaxDistVehicle). Antes era o
		// mesmo teto do jogador, o que obrigava a ver carro a 900 m quando o
		// que importa e' rotacao a curta distancia.
		if (distance > (float)Configs.Survivor.MaxDistVehicle) continue;

		bool dup = false;
		for (auto& d : drawn) {
			if (Vector3::Distance(d, pos) < 400.0f) { dup = true; break; }
		}
		if (dup) continue;
		drawn.push_back(pos);

		Vector2 screen = Camera::WorldToScreen(data.CameraCache.POV, pos);
		if (screen.IsZero()) continue;
		if (!DrawSanity::TelaOk(screen.x, screen.y, "veiculo", pos)) continue;

		const std::string& vn = CleanEntityName(v->objName, true); // ref p/ o memo (28/10)

		char text[160];
		float y = screen.y;

		// -----------------------------------------------------------------
		// O veiculo tem ate' TRES linhas na mesma posicao X (nome, HP, Fuel).
		// Cada linha com fundo proprio deixaria duas emendas visiveis no meio
		// do bloco, entao aqui tambem e' UM painel + linhas sem fundo (mesma
		// solucao do HUD — ver EspPanelBg).
		//
		// A largura/altura saem da MEDIDA real das linhas que vao ser
		// desenhadas (e nao de um chute): e' o que faz o painel nao sobrar
		// nem faltar em nenhum dos tres casos (so' nome / nome+HP /
		// nome+HP+Fuel).
		// -----------------------------------------------------------------
		char linhaHp[64]   = {0};
		char linhaFuel[64] = {0};
		if (v->VehHealthMax > 0.0f)
			snprintf(linhaHp, sizeof(linhaHp), "HP %.0f%%", (v->VehHealth / v->VehHealthMax) * 100.0f);
		if (v->VehFuelMax > 0.0f)
			snprintf(linhaFuel, sizeof(linhaFuel), "Fuel %.0f%%", (v->VehFuel / v->VehFuelMax) * 100.0f);

		snprintf(text, sizeof(text), "%s [%dm]", vn.c_str(), (int)distance);

		// Largura: a MAIOR das linhas presentes. Altura: linha do nome (medida
		// na fonte/tamanho do desenho, 14px) + o passo de 14px de cada linha
		// extra ("HP"/"Fuel" sao desenhadas a 12px, o passo de antes).
		float painelLarg = EspTextSize(text, 14.0f).x;
		// Medido UMA vez cada: a expressao chamava `EspTextSize` nos dois lados
		// do ternario, entao o texto era varrido duas vezes para o mesmo numero.
		if (linhaHp[0])   { const float wHp   = EspTextSize(linhaHp,   12.0f).x; if (wHp   > painelLarg) painelLarg = wHp;   }
		if (linhaFuel[0]) { const float wFuel = EspTextSize(linhaFuel, 12.0f).x; if (wFuel > painelLarg) painelLarg = wFuel; }

		const int   linhasExtras = (linhaHp[0] ? 1 : 0) + (linhaFuel[0] ? 1 : 0);
		const float painelAlt    = 15.0f + linhasExtras * 14.0f;

		EspPanelBg(Vec2(screen.x - painelLarg * 0.5f - 3.0f, y - 2.0f),
		           painelLarg + 6.0f, painelAlt + 4.0f);

		EspBlockText(text, Vec2(screen.x, y), IM_COL32(255, 200, 0, 255), 14.0f, true);
		y += 15.0f;

		if (linhaHp[0]) {
			EspBlockText(linhaHp, Vec2(screen.x, y), IM_COL32(80, 255, 120, 255), 12.0f, true);
			y += 14.0f;
		}
		if (linhaFuel[0]) {
			EspBlockText(linhaFuel, Vec2(screen.x, y), IM_COL32(120, 200, 255, 255), 12.0f, true);
		}
	}
}

// ===========================================================================
// LOOT ESP — nome do item + distancia
// ===========================================================================
void DrawLootEsp(const SharedData& data)
{
	if (!Configs.Survivor.Loot) return;

	Vector3 campos(data.CameraCache.POV.Location.X,
	               data.CameraCache.POV.Location.Y,
	               data.CameraCache.POV.Location.Z);

	for (auto& it : data.LootActors) {
		if (!it) continue;
		Vector3 pos = it->GetPosition();
		if (pos.IsZero()) continue;
		// [DRAW SANITY] Foi AQUI que apareceu o primeiro sintoma no log da
		// partida que crashou: `[LOOT] 'WeapJuju_C' itemId=0x0 pos=-796602,
		// -20178,0`. Posicao lixo de leitura. Este item era descartado so' pela
		// distancia (8 km); se a leitura lixo fosse NaN ele passaria (NaN nunca
		// e' maior que MaxDistance) e iria direto pro desenho.
		if (!DrawSanity::MundoOk(pos, "loot")) continue;

		float distance = Vector3::Distance(campos, pos) / 100.0f;
		if (!std::isfinite(distance)) continue;
		// -----------------------------------------------------------------
		// CORTE POR CATEGORIA, E ANTES DE MONTAR A STRING.
		//
		// O teto do item (`MaxDistItem`, default 150 m) e' o primeiro filtro
		// barato do caminho — e o que mais corta: loot e' dezenas de atores
		// por frame contra meia duzia de jogadores. Tudo o que vem DEPOIS
		// custa, e por isso a ordem e':
		//     distancia (aqui)     -> sqrt ja' pago, comparacao de 1 float
		//     nome bruto + classe  -> string e ClassifyLootRaw (varredura)
		//     filtro individual    -> lookup em map
		//     filtro substring     -> varredura de string
		//     projecao             -> WorldToScreen
		//     CleanEntityName      -> monta o rotulo
		//     snprintf             -> monta a STRING final
		// Item a 400 m nao passa do primeiro `if`.
		// -----------------------------------------------------------------
		if (distance > (float)Configs.Survivor.MaxDistItem) continue;

		// O ItemName vem do UItem real (DroppedItem -> ObjectID -> GNames) e e'
		// o nome BRUTO da classe ("Item_Weapon_M16A4_C") — o Engine grava ele
		// sem limpar (ver Engine::UpdateLoot). Se falhou, cai no nome de classe
		// do ator do chao, que tambem e' BRUTO ('WeapAK47_C').
		//
		// Ou seja: o `raw` daqui e' exatamente a chave que o catalogo da aba
		// WORLD usa. Nao precisou de campo novo no ActorEntity.
		std::string raw = it->ItemName.empty() ? it->objName : it->ItemName;
		if (raw.empty()) continue;

		LootCategory cat = ClassifyLootRaw(raw);

		// 1) checkbox GERAL do grupo desligado -> nao desenha nada do grupo
		if (!*LootCategoryBool(Configs.Survivor, cat)) continue;

		// 2) filtro INDIVIDUAL do item (aba WORLD). Ausente no mapa = aparece:
		//    o default e' "visivel", senao item novo sumiria do ESP sozinho.
		{
			auto& filtro = Configs.Survivor.LootItemFilter;
			auto f = filtro.find(raw);
			if (f != filtro.end() && !f->second) continue;
		}

		// 3) filtro por SUBSTRING (aba WORLD) — o mesmo mecanismo para arma,
		//    mira e acessorio. A regra mora em PlayerConfig::LootRawPassaSubFiltro
		//    (usada tambem pelo painel pra contar quantos itens casam); aqui so'
		//    e' chamada. Vem DEPOIS do individual de proposito: o individual e'
		//    uma decisao explicita do usuario por aquele item, e o substring e'
		//    um padrao — mas quem tem a ultima palavra e' o padrao, porque
		//    "esconder tudo que tem 'Scope'" precisa valer tambem pro item que
		//    foi marcado a mao numa sessao anterior.
		if (!Configs.Survivor.LootRawPassaSubFiltro(raw)) continue;

		Vector2 screen = Camera::WorldToScreen(data.CameraCache.POV, pos);
		if (screen.IsZero()) continue;
		if (!DrawSanity::TelaOk(screen.x, screen.y, "loot", pos)) continue;

		const std::string& nmLimpo = CleanEntityName(raw); // ref p/ o memo (28/10)
		if (nmLimpo.empty()) continue;

		// -----------------------------------------------------------------
		// NOME GENERICO NAO VIRA ROTULO.
		//
		// Quando a cadeia do nome do item falha, o que sobra e' a classe do
		// ator do chao — e ela nao identifica nada. O jogador via "DROPPEDITEM"
		// na tela: era a queixa do testador. Um rotulo que nao diz qual e' o
		// item e' PIOR que nenhum rotulo: ocupa a tela e parece bug.
		//
		// O "C" solto esta' na lista de proposito — `CleanEntityName` corta o
		// prefixo "DroppedItemGroup_" e o sufixo "_C" ja' nao cabe (a string
		// ficou com 1 caractere), entao "DroppedItemGroup_C" saia daqui como
		// um rotulo "C".
		// -----------------------------------------------------------------
		{
			static const char* genericos[] = {
				"DroppedItem", "DroppedItemGroup", "Dropped", "C",
				"ItemPackage", "Item", "PickupItem", "WeaponItem",
				"ThrowableItem", "Equipment", "Inventory", "InventoryFacade",
				"Magazine", "TslDropMagazine"
			};
			bool generico = false;
			for (const char* g : genericos) {
				if (nmLimpo == g) { generico = true; break; }
			}
			if (generico) continue;
		}

		// -----------------------------------------------------------------
		// ROTULO FIXO PARA AS CATEGORIAS DE NOME FEIO.
		//
		// Airdrop e death box vem com nome de CLASSE cru e inconsistente:
		// "CarePackage_Light_C" limpa pra algo como "P Carepackage light", e
		// "DeathDropItemPackage_C" limpa pra "DeathDropItemPackage". Nenhum
		// dos dois diz ao jogador o que aquilo E'.
		//
		// O que ele quer saber e' "isso e' um airdrop" ou "isso e' a caixa de
		// um morto". Entao o rotulo dessas duas categorias nao vem do nome do
		// item — vem do `L(...)`, e sai traduzido junto com o resto do cheat.
		// -----------------------------------------------------------------
		// Rotulo final: o fixo (airdrop/deathbox) tem prioridade sobre o nome
		// da classe; `nmLimpo.c_str()` so' no else — nenhuma copia de string.
		const char* nm = nullptr;
		if (cat == LootCategory::Airdrop)      nm = L("Airdrop");
		else if (cat == LootCategory::DeathBox) nm = L("Deathbox");
		else                                    nm = nmLimpo.c_str();

		char text[160];
		snprintf(text, sizeof(text), "%s [%dm]", nm, (int)distance);

		// Loot e' o texto MAIS numeroso do ESP (dezenas por frame). Aqui o
		// fundo e' um painel de UMA linha: 1 CalcTextSizeA + 1 AddRectFilled,
		// igual as outras variantes — sem alocacao e sem loop.
		EspStrokeText(text, Vec2(screen.x, screen.y), LootCatColor(cat), static_cast<float>(Configs.Survivor.ItemTextSize), true);
	}
}

void DrawGrenadeEsp(const SharedData& data)
{
	auto drawList = ImGui::GetBackgroundDrawList();
	Vector3 camPos(data.CameraCache.POV.Location.X,
	               data.CameraCache.POV.Location.Y,
	               data.CameraCache.POV.Location.Z);

	for (auto& entity : data.GrenadeActors) {
		if (!entity || entity->PlayerType != 3) continue;

		bool isMolotov = (entity->objName == "ProjMolotov_C");
		bool isGrenade = (entity->objName == "ProjGrenade_C");

		// --- Timer logic: grenades use game offset, molotovs use client-side countdown ---
		float timer = 0.0f;
		if (isGrenade) {
			if (entity->ExplodeState != 0) continue;        // Already detonated
			if (entity->TimeTillExplosion <= 0.0f) continue; // Expired
			timer = entity->TimeTillExplosion;
		} else if (isMolotov) {
			// Molotovs don't share the grenade's TimeTillExplosion offset.
			// Use client-side spawn time with 5 second burn duration.
			if (!entity->SpawnTimeSet) {
				entity->SpawnTime = std::chrono::steady_clock::now();
				entity->SpawnTimeSet = true;
			}
			float elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - entity->SpawnTime).count() / 1000.0f;
			timer = 5.0f - elapsed;
			if (timer <= 0.0f) continue; // Burn finished, stop rendering
		} else {
			continue; // Unknown projectile type
		}

		Vector3 grenadePos = entity->GetPosition();
		if (grenadePos.IsZero()) continue;
		if (!DrawSanity::MundoOk(grenadePos, "granada")) continue;

		float distance = Vector3::Distance(camPos, grenadePos) / 100.0f;
		if (!std::isfinite(distance)) continue;
		if (distance > 150.0f) continue;  // Max render distance for grenades

		Vector2 screenPos = Camera::WorldToScreen(data.CameraCache.POV, grenadePos);
		if (screenPos.IsZero()) continue;
		if (!DrawSanity::TelaOk(screenPos.x, screenPos.y, "granada", grenadePos)) continue;

		float radius = entity->ExplosionRadius;  // In Unreal Units
		// [DRAW SANITY] O raio da explosao vem de leitura de jogo e e' somado a
		// posicao do mundo 40 vezes (o circulo). NaN aqui contaminaria os 40
		// pontos e o AddPolyline.
		if (!std::isfinite(radius)) radius = 0.0f;

		// --- Color by urgency ---
		ImU32 textColor;
		ImU32 circleColor;
		if (timer > 3.0f) {
			textColor = IM_COL32(0, 255, 0, 255);       // Green = safe
			circleColor = IM_COL32(0, 255, 0, 120);
		} else if (timer > 1.5f) {
			textColor = IM_COL32(255, 255, 0, 255);      // Yellow = warning
			circleColor = IM_COL32(255, 255, 0, 150);
		} else {
			textColor = IM_COL32(255, 0, 0, 255);        // Red = danger
			circleColor = IM_COL32(255, 0, 0, 200);
		}

		// --- Draw world-space explosion radius circle ---
		// Projects 40 points on a horizontal circle around the grenade through WorldToScreen.
		// This gives a proper perspective-distorted ellipse, not a flat screen circle.
		if (radius > 0.0f && distance < 80.0f) {
			const int segments = 40;
			const float step = 6.2831853f / segments;  // 2*PI / segments
			std::vector<ImVec2> points;
			points.reserve(segments);
			bool circleBroken = false;

			for (int i = 0; i < segments; i++) {
				float theta = step * i;
				Vector3 worldPt(
					grenadePos.x + radius * cosf(theta),
					grenadePos.y + radius * sinf(theta),
					grenadePos.z  // Flat circle at grenade height
				);
				Vector2 screenPt = Camera::WorldToScreen(data.CameraCache.POV, worldPt);
				if (screenPt.IsZero() ||
				    !DrawSanity::TelaOk(screenPt.x, screenPt.y, "granada", worldPt)) {
					// -----------------------------------------------------------------
					// BUG DA LINHA RETA NO TOPO DA TELA
					//
					// Antes a gente PULAVA o ponto invalido e seguia. Ficava um
					// buraco no meio do vetor, e como o AddPolyline usa
					// ImDrawFlags_Closed, ele ligava o ultimo ponto valido de
					// volta no primeiro — atravessando a tela inteira. Era
					// aquele risco que piscava e sumia.
					//
					// Agora: se QUALQUER ponto do circulo nao projeta (granada
					// atras da camera, na borda da tela), nao desenha o circulo.
					// Ele volta a aparecer assim que entrar todo no campo de visao.
					// -----------------------------------------------------------------
					circleBroken = true;
					break;
				}
				points.push_back(ImVec2(screenPt.x, screenPt.y));
			}

			if (!circleBroken && points.size() >= 3) {
				drawList->AddPolyline(points.data(), (int)points.size(),
					circleColor, ImDrawFlags_Closed, 2.0f);
			}
		}

		// --- Draw label: "FRAG (3.2s) [45m]" ---
		//
		// Este era o UNICO texto do ESP que nao passava pelo OSImGui (usava
		// drawList->AddText direto, com a fonte padrao). Agora usa o mesmo
		// caminho dos outros: fonte do ESP (GEspFont), contorno e o painel de
		// fundo configuravel. A posicao e' a mesma de antes (centro-x do
		// texto, 20 px acima da granada) — o desenho e' que ficou consistente.
		const char* label = isGrenade ? "FRAG" : "MOLOTOV";
		char text[64];
		sprintf(text, "%s (%.1fs) [%dm]", label, timer, (int)distance);

		// O 15.0f e' o MESMO tamanho que o resto do ESP usa (nome, loot,
		// veiculo); o `KeepCenter` e' o que faz o meio do texto cair em
		// screenPos.x. O `+ altura/2` converte a posicao antiga (TOPO do
		// texto, baseline em screenPos.y - 20) para a nova (MEIO do texto):
		// sem ele o rotulo subiria ~7 px. A altura vem da medida na fonte do
		// ESP (nao do GetTextLineHeight, que e' da fonte do MENU).
		const ImVec2 labelMedida = EspTextSize(text, 15.0f);
		const float  labelY      = screenPos.y - 20.0f + labelMedida.y * 0.5f;

		// Painel proprio (mesma cor/alpha do resto) e depois o texto COM
		// contorno e SEM fundo: e' uma linha so', entao nao existe emenda a
		// esconder — o painel aqui serve pra tirar o rotulo do fundo do jogo
		// quando a granada esta' sobre neve/grama clara.
		Gui.StrokeText(text, Vec2(screenPos.x, labelY), textColor, 15.0f, true);

		// --- Warning marker dot at grenade position ---
		drawList->AddCircleFilled(ImVec2(screenPos.x, screenPos.y), 4.0f, textColor);
		drawList->AddCircle(ImVec2(screenPos.x, screenPos.y), 6.0f, IM_COL32(0, 0, 0, 200), 0, 1.5f);
	}
}

// ---------------------------------------------------------------------------
// MIRA CENTRAL CUSTOMIZADA
//
// Utilitario de tela (nao depende do jogo): desenha uma mira no centro para
// armas sem reticula nativa sem mirar. Tres estilos.
// ---------------------------------------------------------------------------
void DrawCrosshair()
{
    if (Configs.Survivor.CrosshairStyle <= 0) return;

    const float cx = Gui.Window.Size.x * 0.5f;
    const float cy = Gui.Window.Size.y * 0.5f;
    const float t  = 1.5f;
    const float s  = Configs.Survivor.CrosshairSize;
    const float g  = Configs.Survivor.CrosshairGap;
    const ImColor cor(0, 255, 0, 220);

    // [DRAW SANITY] CrosshairSize/CrosshairGap vem do config (JSON). Eles viram
    // RAIO de circulo e deslocamento de linha: um NaN aqui cairia no
    // AddCircle/AddCircleFilled — o mesmo caminho de tabela do crash da caixa.
    if (!std::isfinite(s) || !std::isfinite(g) || !DrawSanity::TelaOk(cx, cy, "mira"))
        return;

    if (Configs.Survivor.CrosshairStyle == 1) {
        // ponto central
        Gui.CircleFilled(Vec2(cx, cy), s * 0.25f, cor);
    }
    else if (Configs.Survivor.CrosshairStyle == 2) {
        // cruz com vao central
        Gui.Line(Vec2(cx - g - s, cy), Vec2(cx - g, cy), cor, t);
        Gui.Line(Vec2(cx + g, cy), Vec2(cx + g + s, cy), cor, t);
        Gui.Line(Vec2(cx, cy - g - s), Vec2(cx, cy - g), cor, t);
        Gui.Line(Vec2(cx, cy + g), Vec2(cx, cy + g + s), cor, t);
    }
    else {
        // circulo + ponto
        Gui.Circle(Vec2(cx, cy), g + s, cor, t);
        Gui.CircleFilled(Vec2(cx, cy), 1.5f, cor);
    }
}
