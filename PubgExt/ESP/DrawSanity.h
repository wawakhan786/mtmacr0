#pragma once
// ===========================================================================
// DRAW SANITY — A BARREIRA QUE FALTAVA ANTES DE TODO Add* DO ESP
//
// POR QUE ESTE HEADER EXISTE (nao e' chute — e' o dump do crash)
// ---------------------------------------------------------------------------
// O 0xC0000005 de 11/10/2026 23:08 (logs.txt:427) foi analisado no dump
// RuntimeBroker.exe.15084.dmp e a cadeia inteira e' esta:
//
//   PlayerEsp.cpp  DrawPlayerEsp  Gui.Rectangle(Vec2(centerX - width/2, HeadScreen.y),
//                                                Vec2(width, height), ...)
//     -> ImDrawList::AddRect
//     -> ImDrawList::PathRect            (imgui_draw.cpp:1484)
//     -> ImDrawList::PathArcToFast       (imgui_draw.cpp:1294)
//     -> ImDrawList::_PathArcToFastEx    (imgui_draw.cpp:1183)
//     -> ImDrawList::_CalcCircleAutoSegmentCount (imgui_draw.cpp:694)
//     -> CircleSegmentCounts[INT_MIN]    <-- LEITURA ~2 GB FORA DA TABELA
//
// O primeiro elo e' uma coordenada NAO-FINITA. No dump, a instrucao que
// estourou foi `movzx eax, byte ptr [rdx+rax+1C4h]` com rax = 0xFFFFFFFF80000000
// (INT_MIN, o "integer indefinite" que o `cvttss2si` devolve para NaN/+-inf).
// O ImGui converte o raio do canto arredondado com `(int)(radius + 0.999999f)`
// e checava SO' o limite SUPERIOR — indice negativo passava.
//
// O detalhe que fazia isso chegar la': `Vector2::IsZero()` (Vector.cpp:43) e'
// `!x && !y`, ou seja rejeita APENAS o zero exato. NaN e +-inf passam por todos
// os `continue` de "projecao invalida" que existiam no ESP. Depois, dentro do
// ImGui, `NaN < 0.5f` e' FALSO — entao um retangulo de tamanho NaN entra no
// caminho do canto arredondado (o "seguro" seria ele cair no caminho reto).
//
// REGRA DA CASA: rejeitar NAO e' erro, e' o caminho normal. Leitura de memoria
// de jogo devolve lixo de verdade (o log DESSA partida tem
// `[LOOT] 'WeapJuju_C' itemId=0x0 pos=-796602,-20178,0`). O ESP tem que
// descartar isso em silencio e continuar vivo.
//
// USO:
//     if (!DrawSanity::MundoOk(pos, "loot")) continue;        // mundo
//     if (!DrawSanity::TelaOk(sx, sy, "loot", pos)) continue; // tela
// ===========================================================================

#include "pch.h"                          // LOG() (BaseLog) + std::chrono
#include <cmath>                          // std::isfinite
#include "../Config/ConfigUtilities.h"    // Configs.Survivor.DebugLogs

namespace DrawSanity
{
	// -----------------------------------------------------------------------
	// FAIXAS PLAUSIVEIS
	//
	// MUNDO: o maior mapa do PUBG tem ~816.000 UU de lado (8x8 km). A margem
	// de 1.000.000 UU aceita qualquer coordenada real de qualquer mapa e ainda
	// barra o lixo de leitura — quando o ponteiro esta' errado o valor vem na
	// casa de 1e30/1e38, nao na casa de 1e5.
	//
	// ALTURA: o PUBG tem terreno de ~0 a 20.000 UU de altitude. 200.000 e' uma
	// folga enorme — de novo, o que se quer barrar esta' muitas ordens de
	// grandeza acima disso.
	//
	// TELA: 30.000 px e' ~15x a largura de um monitor 4K. Passar disso nao e'
	// "fora da tela": e' conta quebrada (projecao com z quase zero ou FOV
	// degenerado).
	// -----------------------------------------------------------------------
	constexpr float kMundoMaxXY = 1000000.0f;   // UU
	constexpr float kMundoMaxZ  =  200000.0f;   // UU
	constexpr float kTelaMax    =   30000.0f;   // px

	// -----------------------------------------------------------------------
	// LIMITE DO LOG DE DIAGNOSTICO: ~5 LINHAS POR SEGUNDO.
	//
	// O limite NAO pode ser "por frame": a 144 fps isso daria 144 linhas/s e o
	// logs.txt (que e' aberto/escrito/sincronizado a cada LOG) viraria o
	// gargalo do frame. A janela e' de 1 s, entao o custo maximo em uso real e'
	// 5 escritas por segundo — e SO' quando o checkbox "Debug logs" esta' ligado.
	//
	// Nao precisa de lock: quem chama e' sempre a thread de render (o desenho do
	// ESP nunca sai dela).
	// -----------------------------------------------------------------------
	inline void LogRejeitado(const char* tipo,
	                         float px, float py, float pz,
	                         float tx, float ty,
	                         const char* motivo)
	{
		if (!Configs.Survivor.DebugLogs) return;   // default OFF: custo zero

		// -------------------------------------------------------------------
		// SO' "nao-finito" ENTRA NO LOG.
		//
		// "fora-de-faixa" e' NORMAL: jogador atras da camera projeta numa
		// coordenada absurda (ex: tela=(-227256, 4900)). Isso acontece dezenas
		// de vezes por segundo numa partida e NAO e' problema — a rejeicao
		// continua acontecendo, so' nao polui o log.
		//
		// "nao-finito" (NaN/inf) e' o que causava o 0xC0000005 (NaN -> INT_MIN
		// -> leitura fora da tabela do ImGui). Esse SIM tem que aparecer.
		//
		// Antes os dois logavam e o debug virava uma metralhadora de linhas
		// inuteis — escondendo o que importa no meio do ruido.
		// -------------------------------------------------------------------
		if (strcmp(motivo, "nao-finito") != 0) return;

		static std::chrono::steady_clock::time_point janela = std::chrono::steady_clock::now();
		static int contador = 0;

		const auto agora = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::milliseconds>(agora - janela).count() >= 1000) {
			janela = agora;
			contador = 0;
		}
		if (contador >= 5) return;
		contador++;

		// NaN/inf saem como "nan"/"-nan"/"inf"/"-inf" no printf — o que e'
		// exatamente a informacao que interessa.
		LOG("[DRAW] NaN rejeitado: tipo=%s pos=(%.1f,%.1f,%.1f) tela=(%.1f,%.1f)\n",
		    tipo, px, py, pz, tx, ty);
	}

	// -----------------------------------------------------------------------
	// POSICAO DE MUNDO (Vector3): finita e dentro da faixa plausivel.
	// -----------------------------------------------------------------------
	inline bool MundoOk(const Vector3& p, const char* tipo)
	{
		if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
			LogRejeitado(tipo, p.x, p.y, p.z, 0.0f, 0.0f, "nao-finito");
			return false;
		}
		if (fabsf(p.x) > kMundoMaxXY || fabsf(p.y) > kMundoMaxXY || fabsf(p.z) > kMundoMaxZ) {
			LogRejeitado(tipo, p.x, p.y, p.z, 0.0f, 0.0f, "fora-de-faixa");
			return false;
		}
		return true;
	}

	// -----------------------------------------------------------------------
	// COORDENADA DE TELA: finita e dentro do que faz sentido desenhar.
	//
	// `pos` entra so' para o log (quem chama ja' tem a posicao de mundo).
	// -----------------------------------------------------------------------
	inline bool TelaOk(float x, float y, const char* tipo, const Vector3& pos)
	{
		if (!std::isfinite(x) || !std::isfinite(y)) {
			LogRejeitado(tipo, pos.x, pos.y, pos.z, x, y, "nao-finito");
			return false;
		}
		if (fabsf(x) > kTelaMax || fabsf(y) > kTelaMax) {
			LogRejeitado(tipo, pos.x, pos.y, pos.z, x, y, "fora-de-faixa");
			return false;
		}
		return true;
	}

	// Variante para quem nao tem posicao de mundo (HUD, mira de tela).
	inline bool TelaOk(float x, float y, const char* tipo)
	{
		const Vector3 semMundo(0.0f, 0.0f, 0.0f);
		return TelaOk(x, y, tipo, semMundo);
	}
}
