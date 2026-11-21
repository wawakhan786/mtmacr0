#pragma once
#include "Pch.h"
#include "PlayerConfig.h"
#include "Overlay.h"
#include "AimbotConfig.h"
class ConfigInstances
{
public:
	PlayerConfig Survivor = PlayerConfig(LIT("Survivor"));
	PlayerConfig Killer = PlayerConfig(LIT("Killer"));
	OverlayConfig Overlay = OverlayConfig(LIT("Overlay"));
	// Seção renomeada "Aimbot" -> "Motion" (hardening-p3): a palavra "aimbot"
	// no JSON do usuario era rotulo autoevidente; "Motion" so' diz o que e'
	// se o usuario já souber o que este app faz. Leitura de config antiga:
	// o AimbotConfig::FromJson aceita a chave legado (ver la').
	AimbotConfig Aimbot = AimbotConfig(LIT("Motion"));
	json ToJson()
	{
		json jsoned;
		jsoned.merge_patch(Survivor.ToJson());
		jsoned.merge_patch(Overlay.ToJson());
		jsoned.merge_patch(Killer.ToJson());
		jsoned.merge_patch(Aimbot.ToJson());
		return jsoned;
	}

	void FromJson(json jsoned)
	{
		Survivor.FromJson(jsoned);
		Overlay.FromJson(jsoned);
		Killer.FromJson(jsoned);
		Aimbot.FromJson(jsoned);
	}
};