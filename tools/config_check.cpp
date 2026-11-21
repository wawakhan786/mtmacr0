// ===========================================================================
// config_check — prova o comportamento dos campos de config, contra os
// HEADERS DE VERDADE (Config\PlayerConfig.h e Config\AimbotConfig.h).
//
// POR QUE ESTE TESTE EXISTE
// O projeto ja' pagou caro tres vezes pelo mesmo padrao: campo declarado sem
// ToJson (nao salva) ou salvo sem FromJson (volta ao default a cada boot).
// O nome disso aqui e' "as TRES PARTES". Um teste de tela nunca pega isso —
// ele pega o sintoma semanas depois, como "meu filtro sumiu".
//
// Este checker exercita o round-trip JSON completo de:
//   - filtro de loot por substring (LootSubFilters / LootSubFilterMode)
//   - distancia maxima por categoria (MaxDistItem / MaxDistVehicle)
//   - espessura do traco (BoxThickness / SkeletonThickness)
//   - visibilidade por LastTimeScreenRenderered (VisCheck / VisToleranceMs)
//   - humanizacao do RCS (RCSHumanize / RCSHumanizeStrength / RCSReactionMs)
//
// e as REGRAS que nao podem ser verificadas na tela:
//   - modo 0 (esconde) x modo 1 (mostra so') sao inversos de verdade;
//   - comparacao de substring ignora caixa;
//   - lista vazia e' fail-open (nao esvazia o mapa);
//   - JSON torto (editado a mao) nao derruba e cai em default seguro.
//
// NAO e' teste de tela nem de driver: nada de ImGui, nada de IOCTL.
//
// Build:  powershell -ExecutionPolicy Bypass -File tools\build_config_check.ps1
// Run  :  %TEMP%\config_check.exe
// ===========================================================================

#include "pch.h"
// Os cabecalhos REAIS — nao copias das regras. Se o config mudar, este teste
// muda junto por construcao.
#include "PlayerConfig.h"
#include "AimbotConfig.h"
#include "VisCheck.h"

int g_falhas = 0;
int g_total  = 0;

static void Checar(bool ok, const char* oque)
{
	g_total++;
	if (!ok) { g_falhas++; std::printf("  [FAIL] %s\n", oque); }
	else     { std::printf("  [ ok ] %s\n", oque); }
}

// O `LOG` do Pch aponta pra um BaseLog definido no Main.cpp. Este teste nao
// linka o Main: um stub mantem a macro utilizavel.
void BaseLog(const char* fmt, ...) { (void)fmt; }

// O `Pch.h` declara `inline Memory TargetProcess;` — todo .cpp do projeto
// carrega um objeto global cujo ctor/dtor moram no Memory.cpp, que puxa o
// driver. Um teste de config nao pode falar com o driver nem por acidente,
// entao estes quatro simbolos ficam vazios aqui.
Memory::Memory() {}
Memory::~Memory() {}
DriverInterfaceV3::DriverInterfaceV3() {}
DriverInterfaceV3::~DriverInterfaceV3() {}

int main()
{
	std::printf("=== config_check ===\n\n");
	// Sem buffer: se um ASSERT do json derrubar o processo no meio de um
	// teste, o que ja' rodou tem que estar na tela. Sem isto o checker
	// "morre sem dizer nada" e o numero da linha do assert vira a unica pista.
	setvbuf(stdout, nullptr, _IONBF, 0);

	// -----------------------------------------------------------------
	// 1. ROUND-TRIP: LOOT + ESPESSURA + DISTANCIAS
	// -----------------------------------------------------------------
	{
		PlayerConfig salvo("Survivor");
		salvo.LootSubFilters.push_back({ "Scope",  true  });
		salvo.LootSubFilters.push_back({ "Muzzle", false });
		salvo.LootSubFilters.push_back({ "Weapon", true  });
		salvo.LootSubFilterMode = 1;
		salvo.BoxThickness      = 2.5f;
		salvo.SkeletonThickness = 1.25f;
		salvo.MaxDistance       = 800;
		salvo.MaxDistVehicle    = 420;
		salvo.MaxDistItem       = 180;

		const json j = salvo.ToJson();

		PlayerConfig lido("Survivor");
		lido.FromJson(j);

		Checar(lido.LootSubFilters.size() == 3, "loot: 3 substrings voltaram");
		if (lido.LootSubFilters.size() == 3) {
			Checar(lido.LootSubFilters[0].sub == "Scope"  && lido.LootSubFilters[0].on,
			       "loot: [0] Scope/on");
			Checar(lido.LootSubFilters[1].sub == "Muzzle" && !lido.LootSubFilters[1].on,
			       "loot: [1] Muzzle/off");
			Checar(lido.LootSubFilters[2].sub == "Weapon" && lido.LootSubFilters[2].on,
			       "loot: [2] Weapon/on");
		}
		Checar(lido.LootSubFilterMode == 1, "loot: modo = 1");
		Checar(lido.BoxThickness      == 2.5f,  "loot: BoxThickness = 2.5");
		Checar(lido.SkeletonThickness == 1.25f, "loot: SkeletonThickness = 1.25");
		Checar(lido.MaxDistance    == 800, "loot: MaxDistance (jogador) = 800");
		Checar(lido.MaxDistVehicle == 420, "loot: MaxDistVehicle = 420");
		Checar(lido.MaxDistItem    == 180, "loot: MaxDistItem = 180");
	}

	// -----------------------------------------------------------------
	// 2. MODO 0 — ESCONDE O QUE CASA
	// -----------------------------------------------------------------
	{
		PlayerConfig c("Survivor");
		c.LootSubFilterMode = 0;
		c.LootSubFilters.push_back({ "Scope", true });

		Checar(!c.LootRawPassaSubFiltro("Item_Attach_Weapon_Scope_RedDot_C"),
		       "modo 0: item que casa e' BLOQUEADO");
		Checar(c.LootRawPassaSubFiltro("Item_Weapon_M16A4_C"),
		       "modo 0: item que nao casa PASSA");
		Checar(!c.LootRawPassaSubFiltro("item_attach_weapon_sCOPE_holographic_c"),
		       "modo 0: comparacao ignora caixa");

		c.LootSubFilters[0].on = false;
		Checar(c.LootRawPassaSubFiltro("Item_Attach_Weapon_Scope_RedDot_C"),
		       "modo 0: substring desligada nao filtra nada");
	}

	// -----------------------------------------------------------------
	// 3. MODO 1 — SO' O QUE CASA (+ OR entre varias)
	// -----------------------------------------------------------------
	{
		PlayerConfig c("Survivor");
		c.LootSubFilterMode = 1;
		c.LootSubFilters.push_back({ "Weapon", true });
		c.LootSubFilters.push_back({ "Ammo",   true });

		Checar(c.LootRawPassaSubFiltro("Item_Weapon_Beryl_C"),
		       "modo 1: casa Weapon -> PASSA");
		Checar(c.LootRawPassaSubFiltro("Item_Ammo_762mm_C"),
		       "modo 1: casa Ammo -> PASSA (OR entre substrings)");
		Checar(!c.LootRawPassaSubFiltro("Item_Heal_FirstAid_C"),
		       "modo 1: nao casa nada -> BLOQUEADO");
	}

	// -----------------------------------------------------------------
	// 4. FAIL-OPEN: lista vazia / substring vazia
	// -----------------------------------------------------------------
	{
		PlayerConfig c("Survivor");
		c.LootSubFilterMode = 0;
		Checar(c.LootRawPassaSubFiltro("Item_Weapon_AK47_C"),
		       "lista vazia: passa tudo (fail-open)");

		c.LootSubFilters.push_back({ "", true });
		Checar(c.LootRawPassaSubFiltro("Item_Weapon_AK47_C"),
		       "substring vazia: ignorada, nao esconde o mapa");
	}

	// -----------------------------------------------------------------
	// 5. JSON MALFORMADO / TORTO NAO DERRUBA (config editado a mao)
	// -----------------------------------------------------------------
	{
		json j;
		j["Survivor"]["LootSubFilters"] = json::array({
			json::object({ {"s", "Scope"}, {"on", true} }),   // valida
			json::object({ {"on", false} }),                  // sem "s"
			json::object({ {"s", ""} }),                      // vazia
			"lixo",                                            // nao-object
			json::object({ {"s", 42} })                        // tipo errado
		});
		j["Survivor"]["LootSubFilterMode"] = 99;              // fora da faixa
		j["Survivor"]["BoxThickness"]      = 999.0;           // absurdo
		j["Survivor"]["MaxDistItem"]       = 0;               // zero

		std::printf("  [..] teste 5: FromJson com JSON torto\n");
		PlayerConfig c("Survivor");
		c.FromJson(j);

		Checar(c.LootSubFilters.size() == 1, "JSON torto: so' a entrada valida sobreviveu");
		Checar(c.LootSubFilterMode == 0, "JSON torto: modo 99 virou 0");
		Checar(c.BoxThickness == 1.0f, "JSON torto: espessura 999 virou default 1.0");
		Checar(c.MaxDistItem == 10, "JSON torto: distancia 0 virou 10 (nao esvazia o ESP)");
	}

	// -----------------------------------------------------------------
	// 6. JSON ANTIGO (sem os campos novos) -> DEFAULTS
	// -----------------------------------------------------------------
	{
		json j;
		j["Survivor"]["Loot"] = true;      // config de antes do filtro
		j["Survivor"]["MaxDistance"] = 700;

		std::printf("  [..] teste 5: FromJson com JSON torto\n");
		PlayerConfig c("Survivor");
		c.FromJson(j);

		Checar(c.LootSubFilters.empty(), "config antiga: nenhuma substring inventada");
		Checar(c.MaxDistItem == 150, "config antiga: MaxDistItem = default 150");
		Checar(c.MaxDistVehicle == 500, "config antiga: MaxDistVehicle = default 500");
		Checar(c.MaxDistance == 700, "config antiga: MaxDistance (jogador) preservado");
		Checar(c.VisCheck == false, "config antiga: VisCheck = default OFF");
		Checar(c.VisToleranceMs == 100, "config antiga: VisToleranceMs = default 100");
	}

	// -----------------------------------------------------------------
	// 7. VISIBILIDADE (LastTimeScreenRenderered) — round-trip e clamps
	// -----------------------------------------------------------------
	{
		PlayerConfig salvo("Survivor");
		salvo.VisCheck       = true;
		salvo.VisToleranceMs = 250;

		PlayerConfig lido("Survivor");
		lido.FromJson(salvo.ToJson());

		Checar(lido.VisCheck == true,      "vischeck: VisCheck voltou true");
		Checar(lido.VisToleranceMs == 250, "vischeck: tolerancia voltou 250");

		// Tolerancia 0 esconderia todo mundo; 10000 marcaria todo mundo visivel.
		// Os dois defeitos parecem "o vischeck nao funciona", entao o clamp tem
		// que existir — e tem que ser verificavel.
		PlayerConfig baixo("Survivor");
		json jb;
		jb["Survivor"]["VisToleranceMs"] = 0;
		baixo.FromJson(jb);
		Checar(baixo.VisToleranceMs == 20, "vischeck: tolerancia 0 virou 20 (nao esconde o mapa)");

		PlayerConfig alto("Survivor");
		json ja;
		ja["Survivor"]["VisToleranceMs"] = 10000;
		alto.FromJson(ja);
		Checar(alto.VisToleranceMs == 400, "vischeck: tolerancia 10000 virou 400");
	}

	// -----------------------------------------------------------------
	// 8. HUMANIZACAO DO RCS — round-trip e clamps
	// -----------------------------------------------------------------
	{
		AimbotConfig salvo("Aimbot");
		salvo.RCSHumanize         = false;   // diferente do default, para o teste valer
		salvo.RCSHumanizeStrength = 70;
		salvo.RCSReactionMs       = 150;

		AimbotConfig lido("Aimbot");
		lido.FromJson(salvo.ToJson());

		Checar(lido.RCSHumanize == false,      "rcs: RCSHumanize voltou false");
		Checar(lido.RCSHumanizeStrength == 70, "rcs: Strength voltou 70");
		Checar(lido.RCSReactionMs == 150,      "rcs: ReactionMs voltou 150");

		// Forca 0 e' VALIDA (desliga os efeitos sem desmarcar o checkbox) —
		// nao pode ser "corrigida" para 1 pelo clamp.
		AimbotConfig zero("Aimbot");
		json jz;
		jz["Aimbot"]["RCSHumanizeStrength"] = 0;
		jz["Aimbot"]["RCSReactionMs"] = 0;
		zero.FromJson(jz);
		Checar(zero.RCSHumanizeStrength == 0, "rcs: forca 0 e' aceita (nao vira 1)");
		Checar(zero.RCSReactionMs == 0,       "rcs: reacao 0 e' aceita (sem rampa)");

		AimbotConfig absurdo("Aimbot");
		json ja;
		ja["Aimbot"]["RCSHumanizeStrength"] = 900;
		ja["Aimbot"]["RCSReactionMs"] = -50;
		ja["Aimbot"]["RCSMaxCounts"] = 0;
		absurdo.FromJson(ja);
		Checar(absurdo.RCSHumanizeStrength == 100, "rcs: forca 900 virou 100");
		Checar(absurdo.RCSReactionMs == 0,         "rcs: reacao -50 virou 0");
		Checar(absurdo.RCSMaxCounts == 1,          "rcs: max counts 0 virou 1 (nunca injecao nula)");
	}

	// -----------------------------------------------------------------
	// 9. VISAO — a REGRA do mesh+0x75C (ESP/VisCheck.h)
	//
	// A regra mudou de "atraso contra uma base de tempo" para "o campo mudou
	// nos ultimos N ms?". As duas versoes anteriores falharam por depender de
	// base (GameState ausente; pawn local em -1000 em FPP), e essa e'' a parte
	// que este teste trava: a decisao nao pode voltar a olhar um "agora".
	// -----------------------------------------------------------------
	{
		const float tol = 100.0f;

		Checar(VisCheck::VereditoRecente(0, tol) == true,   "vis: mudou agora (0 ms) -> VISIVEL");
		Checar(VisCheck::VereditoRecente(8, tol) == true,   "vis: parado 8 ms -> VISIVEL");
		Checar(VisCheck::VereditoRecente(100, tol) == true, "vis: parado == tolerancia -> VISIVEL (<=)");
		Checar(VisCheck::VereditoRecente(101, tol) == false,"vis: parado 101 ms -> OCULTO");
		Checar(VisCheck::VereditoRecente(13870, tol) == false, "vis: parado 13,8 s (atras de parede) -> OCULTO");
		Checar(VisCheck::VereditoRecente(100000, tol) == false, "vis: parado 100 s (nunca desenhado) -> OCULTO");

		// Tolerancia invalida nao pode esconder o mapa: fail-open.
		Checar(VisCheck::VereditoRecente(5000, 0.0f) == true,  "vis: tolerancia 0 -> VISIVEL (fail-open)");
		Checar(VisCheck::VereditoRecente(5000, NAN) == true,   "vis: tolerancia NaN -> VISIVEL (fail-open)");
	}

	std::printf("\nRESULTADO: %d/%d verificacoes passaram\n",
	            g_total - g_falhas, g_total);
	return g_falhas == 0 ? 0 : 1;
}
