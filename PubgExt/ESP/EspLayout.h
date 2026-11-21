#pragma once
// ===========================================================================
// GEOMETRIA DO LAYOUT DO ESP — COMPARTILHADA ENTRE O ESP E O MENU
//
// POR QUE ISTO E' UM HEADER PROPRIO, E NAO UM BLOCO DENTRO DO PlayerEsp.cpp:
//
//   O preview arrastavel do menu mostra onde cada elemento VAI CAIR. Se o
//   preview calculasse a posicao por conta propria, ele e o ESP teriam duas
//   implementacoes da mesma regra — e a primeira vez que uma mudasse, o
//   preview passaria a MENTIR (voce arrasta num lugar, o texto sai em outro).
//
//   Uma geometria, dois consumidores.
//
// O MODELO (ancora + ajuste fino) e os nomes estao em Config/PlayerConfig.h.
// ===========================================================================

#include "pch.h"
#include "../OS-ImGui/OS-ImGui_Struct.h"    // Vec2
#include "../Config/PlayerConfig.h"         // EspAnchor / ESP_ANC_* / EspElement

namespace EspLayout {

	// A caixa do jogador na tela — a referencia de TODAS as ancoras.
	struct Caixa {
		float cx     = 0.0f;   // centro X
		float top    = 0.0f;   // Y do topo (cabeca)
		float bottom = 0.0f;   // Y da base (pe')
		float halfW  = 0.0f;   // metade da largura
	};

	// -----------------------------------------------------------------------
	// PILHA DE EMPILHAMENTO — POR ATOR (no ESP) OU POR PREVIEW (no menu).
	//
	// Ela NAO pode ser `static`: dois jogadores na tela tem caixas diferentes e
	// precisam de pilhas diferentes. Um `static` faria o segundo herdar a
	// altura que o primeiro ja' empilhou, e o texto desceria a tela.
	// -----------------------------------------------------------------------
	struct Pilha {
		float usado[ESP_ANC_COUNT] = { 0.0f };
	};

	constexpr float kGap   = 2.0f;   // entre dois elementos na MESMA ancora
	constexpr float kFolga = 5.0f;   // da caixa ate' o primeiro elemento

	// -----------------------------------------------------------------------
	// Resolve o canto SUPERIOR-ESQUERDO do retangulo (w x h) de um elemento.
	//
	// `keepCenter` sai dizendo se o desenho deve CENTRAR em X no ponto
	// devolvido (ancoras de topo/base) ou usar o ponto como BORDA ESQUERDA
	// (ancoras laterais e de quina). E' a mesma convencao do `KeepCenter` do
	// OSImGui::Text — de proposito: assim quem desenha repassa o valor direto,
	// sem traduzir convencao no meio do caminho.
	// -----------------------------------------------------------------------
	inline Vec2 Resolver(const Caixa& b, int anchor, float dx, float dy,
	                     float w, float h, Pilha& st, bool& keepCenter)
	{
		keepCenter = false;

		const float midY = (b.top + b.bottom) * 0.5f;
		float x = b.cx;
		float y = b.top;

		switch (anchor) {
		case ESP_ANC_TOP:
			keepCenter = true;
			y = b.top - kFolga - h - st.usado[anchor];
			break;
		case ESP_ANC_BOTTOM:
			keepCenter = true;
			y = b.bottom + kFolga + st.usado[anchor];
			break;
		case ESP_ANC_LEFT:
			x = b.cx - b.halfW - kFolga - w;   // borda DIREITA encosta na caixa
			y = midY - h * 0.5f + st.usado[anchor];
			break;
		case ESP_ANC_RIGHT:
			x = b.cx + b.halfW + kFolga;       // borda ESQUERDA encosta na caixa
			y = midY - h * 0.5f + st.usado[anchor];
			break;
		case ESP_ANC_TOP_LEFT:
			x = b.cx - b.halfW;
			y = b.top - kFolga - h - st.usado[anchor];
			break;
		case ESP_ANC_TOP_RIGHT:
			x = b.cx + b.halfW - w;
			y = b.top - kFolga - h - st.usado[anchor];
			break;
		case ESP_ANC_BOTTOM_LEFT:
			x = b.cx - b.halfW;
			y = b.bottom + kFolga + st.usado[anchor];
			break;
		case ESP_ANC_BOTTOM_RIGHT:
			x = b.cx + b.halfW - w;
			y = b.bottom + kFolga + st.usado[anchor];
			break;
		default:
			break;
		}

		// Empilha SEMPRE, inclusive no `default`: se uma ancora invalida entrar
		// por config editada a mao, os elementos se afastam em vez de cair
		// todos exatamente um em cima do outro.
		if (anchor >= 0 && anchor < ESP_ANC_COUNT) {
			st.usado[anchor] += h + kGap;
		}

		return Vec2(x + dx, y + dy);
	}

	// -----------------------------------------------------------------------
	// Ancora mais proxima de um ponto do PREVIEW.
	//
	// E' o "encaixe" do arrasto: enquanto o usuario arrasta, o chip segue o
	// mouse; ao soltar, ele volta para a ancora mais proxima e o `dx/dy` guarda
	// o que sobrou.
	//
	// A tabela de pontos e' a posicao NOMINAL de cada ancora (sem empilhamento
	// e sem ajuste), na mesma convencao do `Resolver`: topo/base devolvem o
	// CENTRO horizontal (e o chip e' centrado ao redor dele), laterais
	// devolvem a borda.
	// -----------------------------------------------------------------------
	inline int AncoraMaisProxima(const Caixa& b, float px, float py)
	{
		const float midY = (b.top + b.bottom) * 0.5f;

		struct Ponto { int ancora; float x; float y; };
		const Ponto pontos[ESP_ANC_COUNT] = {
			{ ESP_ANC_TOP,          b.cx,                 b.top    - kFolga },
			{ ESP_ANC_BOTTOM,       b.cx,                 b.bottom + kFolga },
			{ ESP_ANC_LEFT,         b.cx - b.halfW,       midY },
			{ ESP_ANC_RIGHT,        b.cx + b.halfW,       midY },
			{ ESP_ANC_TOP_LEFT,     b.cx - b.halfW,       b.top    - kFolga },
			{ ESP_ANC_TOP_RIGHT,    b.cx + b.halfW,       b.top    - kFolga },
			{ ESP_ANC_BOTTOM_LEFT,  b.cx - b.halfW,       b.bottom + kFolga },
			{ ESP_ANC_BOTTOM_RIGHT, b.cx + b.halfW,       b.bottom + kFolga },
		};

		int    melhor    = ESP_ANC_TOP;
		float  melhorD2  = 3.4e38f;

		for (int i = 0; i < ESP_ANC_COUNT; ++i) {
			const float dx = px - pontos[i].x;
			const float dy = py - pontos[i].y;
			const float d2 = dx * dx + dy * dy;
			if (d2 < melhorD2) { melhorD2 = d2; melhor = pontos[i].ancora; }
		}
		return melhor;
	}

} // namespace EspLayout
