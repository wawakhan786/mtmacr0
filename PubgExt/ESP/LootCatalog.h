#pragma once
#include "pch.h"
#include <mutex>

// ===========================================================================
// CATALOGO DE LOOT — base da aba WORLD e do filtro individual do loot ESP.
//
// Por que existe: nao da' pra ter lista fixa de itens no codigo. O PUBG tem
// centenas e o nome SO' aparece em runtime (via GNames, ver
// Engine::UpdateLoot). Entao o catalogo se AUTO-POPULA conforme o cheat ve'
// itens no chao e o usuario liga/desliga item por item na aba WORLD.
//
// A chave e' SEMPRE o nome BRUTO da classe ("Item_Weapon_M16A4_C"), nunca o
// nome limpo: o limpo (CleanEntityName) e' so' apresentacao — ele troca,
// traduz e corta prefixo, entao nao serve como chave estavel.
//
// THREAD-SAFETY
// -------------
// Este map e' escrito pela THREAD DE MEMORIA (Engine::UpdateLoot, chamado do
// MemoryLoop) e lido/iterado pela THREAD DE RENDER (aba WORLD em Main.cpp).
// std::map NAO aceita insercao concorrente com leitura: o rebalanceamento da
// arvore invalida o iterador que a renderizacao esta' usando -> crash. Por
// isso:
//   - TODA escrita (Engine) e TODA leitura (Main) passa por G_LootCatalogMutex;
//   - G_LootCatalogDirty so' e' lido/escrito com o mutex na mao (por isso ele
//     NAO e' std::atomic: um bool atomico aqui daria a falsa impressao de que
//     da' pra consultar a flag sem lock);
//   - a renderizacao tira um SNAPSHOT e desenha o snapshot, sem segurar o
//     lock durante chamada de ImGui;
//   - o ESP (DrawLootEsp) NAO le este map: ele filtra pelo mapa do config, e
//     o config so' a thread de render toca (ver SyncLootCatalogIntoConfig).
// ===========================================================================

// As 10 categorias, na ordem em que aparecem na aba WORLD.
enum class LootCategory {
	Weapons = 0,
	Ammo,
	Attachments,
	Health,
	Boost,
	Helmet,
	Backpack,
	Vest,
	Throwables,
	Other,
	// -----------------------------------------------------------------------
	// AIRDROP — categoria PROPRIA, e nao "Other".
	//
	// Antes, uma caixa de suprimento caia em `Other` (nenhum dos padroes de
	// `ClassifyLootRaw` casava com "Carapackage_RedBox_C"), o que dava dois
	// problemas de uma vez:
	//   - `Engine::IsAirdrop` existia e NUNCA era chamado;
	//   - o checkbox "Airdrop / caixa de suprimento" gravava no config e
	//     NINGUEM lia — quem quisesse ver airdrop dependia de deixar "Other"
	//     ligado, e quem desligasse "Other" perdia o airdrop sem entender.
	//
	// Com categoria propria, `LootCategoryBool` devolve o `AirdropEsp` e o
	// toggle passa a fazer exatamente o que o nome diz.
	// -----------------------------------------------------------------------
	Airdrop,
	// -----------------------------------------------------------------------
	// DEATH BOX — a caixa que aparece quando um inimigo morre.
	//
	// A classe e' "DeathDropItemPackage_C". Antes ela caia em "Other" junto
	// com todo o resto, e aparecia no ESP com o nome cru da classe
	// ("DeathDropItemPackage"), que nao diz nada a quem esta' jogando.
	//
	// Separado de loot de chao de proposito: a caixa de morte e' CONSEQUENCIA
	// de uma kill (tem o inventario do cara), o loot de chao e' geracao do
	// mapa. Sao coisas diferentes e o jogador quer poder ligar uma sem a
	// outra.
	// -----------------------------------------------------------------------
	DeathBox,
	COUNT
};

inline const char* LootCategoryName(LootCategory c)
{
	switch (c) {
	case LootCategory::Weapons:     return "Weapons";
	case LootCategory::Ammo:        return "Ammo";
	case LootCategory::Attachments: return "Attachments";
	case LootCategory::Health:      return "Health";
	case LootCategory::Boost:       return "Boost";
	case LootCategory::Helmet:      return "Helmet";
	case LootCategory::Backpack:    return "Backpack";
	case LootCategory::Vest:        return "Vest";
	case LootCategory::Throwables:  return "Throwables";
	case LootCategory::Airdrop:     return "Airdrop";
	case LootCategory::DeathBox:    return "Death Box";
	default:                        return "Other";
	}
}

// nome BRUTO do item (ex: "Item_Weapon_M16A4_C") -> visivel no ESP?
// So' entram os nomes que o cheat JA' VIU na partida.
inline std::map<std::string, bool> G_LootCatalog;
inline std::mutex G_LootCatalogMutex;    // protege G_LootCatalog E G_LootCatalogDirty
inline bool G_LootCatalogDirty = false;  // so' com o mutex na mao

// ---------------------------------------------------------------------------
// Classificacao por NOME BRUTO — padroes MEDIDOS em log real (LOOTNAME).
//
// A ORDEM IMPORTA: "Item_Attach_Weapon_Lower_ThumbGrip_C" e' ACESSORIO mas
// contem "Weapon". Se o teste de arma viesse primeiro, acessorio viraria arma
// e sumiria junto quando o usuario desligasse "Weapons" — esse bug ja'
// aconteceu neste projeto. Por isso Item_Attach_ vem ANTES.
// ---------------------------------------------------------------------------
inline LootCategory ClassifyLootRaw(const std::string& raw)
{
	auto has = [&](const char* p) { return raw.find(p) != std::string::npos; };

	// -----------------------------------------------------------------------
	// hasI — busca CASE-INSENSITIVE.
	//
	// POR QUE EXISTE: as chaves de airdrop estavam escritas "AirDrop" e
	// "CarePackage" (maiusculas no meio), mas a classe real do jogo e'
	// "Carepackage" — C maiusculo e o RESTO MINUSCULO. Nao e' nenhuma das
	// duas grafias, e `std::string::find` e' case-sensitive.
	//
	// Resultado: a caixa de suprimento caia em `Other`, entao nunca recebia a
	// categoria Airdrop — e por isso o rotulo "Airdrop" que o ESP aplica por
	// categoria NUNCA aparecia. Ela continuava exibindo o nome cru da classe
	// ("Carepackage Light"). Um bug de uma letra, em dois lugares diferentes.
	//
	// O `IsLoot`/`IsAirdrop` do Engine ja' foram corrigidos; este e' o mesmo
	// conserto na ponta que decide a CATEGORIA. Sem os dois, nao funciona.
	// -----------------------------------------------------------------------
	auto hasI = [&](const char* p) {
		const size_t n = strlen(p);
		if (n == 0 || raw.size() < n) return false;
		for (size_t i = 0; i + n <= raw.size(); i++) {
			size_t j = 0;
			for (; j < n; j++) {
				const char a = (char)tolower((unsigned char)raw[i + j]);
				const char b = (char)tolower((unsigned char)p[j]);
				if (a != b) break;
			}
			if (j == n) return true;
		}
		return false;
	};

	// 0) AIRDROP PRIMEIRO: nenhuma outra regra casa com a classe da caixa, e
	//    se caisse no `default` viraria "Other" — foi o que acontecia.
	//    Tem que vir ANTES de tudo, e case-insensitive.
	if (hasI("Carapackage") || hasI("CarePackage") ||
	    hasI("AirDrop") || hasI("AircraftCarePackage") ||
	    hasI("TslBalloonCarePackage") || hasI("TslCustomAirDrop"))
		                                                        return LootCategory::Airdrop;

	// 0b) DEATH BOX — tambem antes das outras: "DeathDropItemPackage_C" contem
	//     "ItemPackage", e se a checagem generica viesse primeiro ela pegaria.
	if (hasI("DeathDrop"))                                      return LootCategory::DeathBox;

	// 1) ACESSORIO antes de ARMA (ver comentario acima)
	if (has("Item_Attach_"))                                return LootCategory::Attachments;
	// 2) armas: classe 'WeapAK47_C' OU o prefixo completo 'Item_Weapon_'
	if (raw.rfind("Weap", 0) == 0 || has("Item_Weapon_"))   return LootCategory::Weapons;
	if (has("Item_Ammo_"))                                  return LootCategory::Ammo;
	if (has("Item_Heal_"))                                  return LootCategory::Health;
	if (has("Item_Boost_"))                                 return LootCategory::Boost;
	if (has("Item_Head_"))                                  return LootCategory::Helmet;
	if (has("Item_Back_"))                                  return LootCategory::Backpack;
	if (has("Item_Armor_"))                                 return LootCategory::Vest;
	if (has("Grenade") || has("Throwable"))                 return LootCategory::Throwables;
	return LootCategory::Other;
}

// Checkbox GERAL de cada categoria mora no PlayerConfig (um bool por grupo).
//
// E' template de proposito: assim este header nao precisa incluir o
// PlayerConfig (e o PlayerConfig nao precisa conhecer LootCategory). Quem
// instancia (Main.cpp / PlayerEsp.cpp) tem os dois tipos visiveis.
template <typename TConfig>
inline bool* LootCategoryBool(TConfig& cfg, LootCategory c)
{
	switch (c) {
	case LootCategory::Weapons:     return &cfg.LootWeapons;
	case LootCategory::Ammo:        return &cfg.LootAmmo;
	case LootCategory::Attachments: return &cfg.LootAttachments;
	case LootCategory::Health:      return &cfg.LootHealth;
	case LootCategory::Boost:       return &cfg.LootCatBoost;
	case LootCategory::Helmet:      return &cfg.LootCatHelmet;
	case LootCategory::Backpack:    return &cfg.LootCatBackpack;
	case LootCategory::Vest:        return &cfg.LootCatVest;
	case LootCategory::Throwables:  return &cfg.LootCatThrowables;
	case LootCategory::Airdrop:     return &cfg.AirdropEsp;
	case LootCategory::DeathBox:    return &cfg.LootCatDeathBox;
	default:                        return &cfg.LootCatOther;
	}
}
