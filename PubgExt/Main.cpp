#include "pch.h"
#include <process.h>
#include <stdlib.h>
#include "Memory.h"
#include "Globals.h"
#include "Engine.h"
#include "ESP/PlayerEsp.h"
#include "ESP/EspLayout.h"   // geometria das ancoras (a MESMA do ESP)
#include "SDK/Aimbot.h"
#include "OS-ImGui/OS-ImGui.h"
#include "OS-ImGui/DirectOverlay.h"
#include "UI/EspTypography.h"   // tipografia nativa do ESP (transplante)
#include "Perf.h"    // instrumentacao de desempenho (painel de teste)
#include <cstdlib>   // _dupenv_s / free (marcador de perf)
#include "Misc/HotkeyHook.h"
#include "Mapper/AutoLoader.h"
#include "AntiCheat.h"     // deteccao de BE/Zakynthos antes de mapear

#include "Config/ConfigUtilities.h"
#include "Config/Lang.h"
#include "Config/Version.h"     // MTM_VERSAO_TAG (fonte unica da versao)
#include "Config/MenuVariant.h"
#include "SDK/Camera.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <mutex>
#include <chrono>
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include "SharedState.h"
#include "ESP/Radar.h"

SharedData GSharedData;
std::mutex GDataMutex;
bool GIsRunning = true;

std::shared_ptr<Engine> EngineInstance;
Radar GRadar;
std::string ProcessName = "TslGame.exe";

// ===========================================================================
// FASES DO APP  LOADER x SESSAO
//
// POR QUE EXISTE: o cheat tinha UM caminho de boot (espera o jogo, mapeia,
// abre o overlay) e o END matava o processo. Consequencia pratica: para
// carregar ou descarregar o driver era preciso abrir a aba DEBUG  que no
// build publico nem existe  e o "descarregar" so' acontecia saindo.
//
// AGORA:
//   FASE_LOADER      janela pequena com Carregar/Descarregar driver e
//                    "Iniciar PUBG" (que passa a aguardar o TslGame.exe)
//   FASE_AGUARDANDO  esperando o processo aparecer (cancelavel)
//   FASE_RODANDO     sessao ativa: menu + ESP
//
// O END encerra a SESSAO e volta pro LOADER (o driver continua carregado, para
// ser descarregado na mao). No LOADER, o END sai do processo.
// ===========================================================================
enum AppFase : int { FASE_LOADER = 0, FASE_AGUARDANDO = 1, FASE_RODANDO = 2 };
static std::atomic<int>  GFase{ FASE_LOADER };
static std::atomic<bool> GSessaoOcupada{ false };   // thread de inicio viva
static std::atomic<bool> GMemThreadViva{ false };   // MemoryLoop rodando
static std::mutex        GStatusMutex;
static std::string       GStatusTexto;

static void LoaderStatus(const char* fmt, ...)
{
	char buf[256] = { 0 };
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	std::lock_guard<std::mutex> lk(GStatusMutex);
	GStatusTexto = buf;
}

// Declaracoes: o loader (no RenderFrame) e' definido/ usa estas rotinas que
// ficam mais abaixo no arquivo (perto do DescarregarDriver).
static void PararSessao();
static void CarregarDriverThread();
static void DescarregarDriverThread();
static void IniciarSessaoThread(bool carregarDriver);
static void DesenharLoader();
// hardening-p4: o registro de paginas e' gravado na hora do mapa (em
// SubirDriverKevent, acima das definicoes) e apagado apos a liberacao ok.
static void GravarPaginasResiduas(uint64_t base, uint64_t size);
static bool ProcessoExiste(const char* nome);   // forward declaration — Steam helper use karta hai

static std::string LoaderStatusTexto()
{
	std::lock_guard<std::mutex> lk(GStatusMutex);
	return GStatusTexto;
}

// Estado da inicializacao. Declarado aqui em cima porque RenderFrame usa.
static std::atomic_bool GInitRunning{ false };
static std::atomic_bool GInitDone{ false };
static std::atomic_bool GInitOk{ false };
// ---------------------------------------------------------------------------
// GMapeadoManual  o driver foi mapeado pelo botao MAP NOW.
//
// ==================== O BUG QUE ISTO CORRIGE ====================
//
// Medido no log do teste: o He clicou MAP NOW (o driver mapeou, versao
// 0x00010002 confirmada) e DEPOIS abriu o jogo. O log mostrou:
//
//     [+] MAP NOW: driver mapeado e confirmado (0x00010002)
//     ...
//     [!] ANTI-CHEAT ATIVO  mapeamento BLOQUEADO
//
// Ou seja: o driver JA' ESTAVA MAPEADO, e o gate ficou esperando o BattlEye
// sair PARA MAPEAR DE NOVO. Ele nunca ia sair (o jogo estava aberto), entao o
// gate girou o teto inteiro e o `TargetProcess.Init`  que e' quem monta a
// leitura de memoria e o ESP  nunca foi chamado.
//
// SINTOMA REPORTADO: "nao funcionou nada nem esp".
// CAUSA: o gate bloqueava um mapeamento que ja' tinha acontecido.
//
// O `InitializeFeaturesThread` avaliou `!DriverVersionOk()` ANTES do clique.
// Aquela decisao ficou congelada; o clique mudou a realidade e ninguem avisou
// o laco. A flag e' o aviso: o MAP NOW levanta, o gate consulta a cada volta.
//
// DECLARADA AQUI NO TOPO (e nao junto do gate) porque o menu do ImGui usa-la
// antes  o `RenderFrame` e' definido cedo no arquivo.
// ---------------------------------------------------------------------------
static std::atomic_bool GMapeadoManual{ false };

void InitializeFeaturesThread();

// Banco de teste do driver. Definido em DriverSelfTest.cpp.
// Retorna false se o driver nao respondeu.
bool RunDriverSelfTest();

// Sonda SEM driver: logica pura de scan e cabecalho PE contra nos mesmos.
// Ver DriverlessProbe.cpp. Segura de rodar — nao carrega nada.
bool RunDriverlessProbe();

// Sonda do driver doador AsIO3 (rota A). Definida em DriverSelfTest.cpp.
void RunAsIO3Probe();

bool ShowMenu = false; 
int ActiveTab = 0;
bool IsFrozen = false;
std::vector<std::shared_ptr<ActorEntity>> FrozenActors;
bool RunDrawingTest = false;

// Desenho do circulo de FOV do aimbot. NAO e' campo de config (nao vai pro
// JSON): e' um toggle de tela, consumido em RenderFrame.
bool GDrawFovCircle = true;

// ColorEdit4 para o struct Colour (r,g,b,a em 0..255). Existe pra dar
// controle ao unico campo de cor que o config tem hoje (Survivor.TextColour,
// usado pelo ESP pra quem esta' oculto) sem inventar campo novo.
// ===========================================================================
// ColourEdit4  AMOSTRA + POPUP COM SLIDERS RGB/A.
//
// POR QUE NAO `ImGui::ColorEdit4` ("nao consigo alterar a cor"): o
// `ColorEdit4` do fork desenha os campos dentro de `CalcItemWidth()`, que aqui
// vem pequeno  sobram poucos pixels por slider  e o clique na amostra abre o
// popup de opcoes, que nesta UI cai fora do painel. Resultado: sem controle.
//
// Este widget nao depende de nada disso:
//   - a AMOSTRA e' desenhada a mao (AddRectFilled + InvisibleButton), entao o
//     clique e' capturado pela linha inteira;
//   - o POPUP e' ancorado no cursor e usa `SliderInt` 0..255  o MESMO
//     controle que funciona no resto do menu;
//   - os sliders escrevem DIRETO no `Colour` do config (sem copia
//     intermediaria para dessincronizar).
// ===========================================================================
// COLOUR PICKER  QUADRADO SV + BARRA DE MATIZ + ALFA, LARGURA FIXA.
//
// O QUE QUEBROU ANTES (print do He, 19/10/2026): ao clicar na amostra, o popup
// se esticava na HORIZONTAL ate' a borda da tela. Causa: os `SliderInt` dentro
// dele medem a largura da JANELA do popup, e o popup e' auto-dimensionado 
// `SetNextItemWidth` era ignorado nesse caminho, entao cada slider pedia "tudo
// que ha'" e a janela ia crescendo.
//
// A correcao tem duas partes, e as duas sao necessarias:
//   1. `SetNextWindowSizeConstraints` fixa a largura do popup (268 px). Mesmo
//      que um controle peca a largura toda, a janela NAO cresce;
//   2. o seletor em si nao usa nenhum widget do tema: o quadrado SV e a barra
//      de matiz sao desenhados com `AddRectFilledMultiColor` e lidos com
//      `InvisibleButton` + `IsItemActive`. Nao existe `CalcItemWidth` no
//      caminho, entao nao existe como esticar.
//
// Layout (igual ao do Astral, que o He pediu como referencia):
//   [ quadrado SV 240x120 ]  [ barra de matiz 14x120 ]
//   [ barra de alfa 240x10 ]
//   hex   +  valor RGBA
// ===========================================================================
static void RgbParaHsv(float r, float g, float b, float& h, float& s, float& v)
{
	const float mx = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
	const float mn = (r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b);
	const float d  = mx - mn;
	v = mx;
	s = (mx <= 0.0f) ? 0.0f : (d / mx);
	if (d <= 0.0f) { h = 0.0f; return; }
	if (mx == r)      h = (g - b) / d + (g < b ? 6.0f : 0.0f);
	else if (mx == g) h = (b - r) / d + 2.0f;
	else              h = (r - g) / d + 4.0f;
	h /= 6.0f;
}

static void HsvParaRgb(float h, float s, float v, float& r, float& g, float& b)
{
	const float i = floorf(h * 6.0f);
	const float f = h * 6.0f - i;
	const float p = v * (1.0f - s);
	const float q = v * (1.0f - f * s);
	const float t = v * (1.0f - (1.0f - f) * s);
	switch (((int)i) % 6) {
		case 0: r = v; g = t; b = p; break;
		case 1: r = q; g = v; b = p; break;
		case 2: r = p; g = v; b = t; break;
		case 3: r = p; g = q; b = v; break;
		case 4: r = t; g = p; b = v; break;
		default: r = v; g = p; b = q; break;
	}
}

static void ColourEdit4(const char* label, Colour& col)
{
	ImGui::PushID(label);

	const ImVec2 p   = ImGui::GetCursorScreenPos();
	const ImVec2 tam(46.0f, 20.0f);

	ImGui::InvisibleButton("##amostra", tam);
	const bool abrir = ImGui::IsItemClicked();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(L("clique para escolher a cor"));

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + tam.x, p.y + tam.y), IM_COL32(col.r, col.g, col.b, 255), 4.0f);
	// borda clara: a amostra tem que aparecer mesmo quando a cor e'' preta
	dl->AddRect(p, ImVec2(p.x + tam.x, p.y + tam.y), IM_COL32(255, 255, 255, 70), 4.0f);

	ImGui::SameLine();
	ImGui::TextUnformatted(label);

	if (abrir) ImGui::OpenPopup("##cor");

	// largura FIXA (ver o comentario do topo): sem isto o popup cresce ate' a
	// borda da tela quando um controle pede a largura disponivel
	ImGui::SetNextWindowSizeConstraints(ImVec2(280.0f, 0.0f), ImVec2(280.0f, 600.0f));

	if (ImGui::BeginPopup("##cor")) {
		// -----------------------------------------------------------------
		// O DRAW LIST TEM QUE SER O DO POPUP  ESTE ERA O BUG DO "TUDO ESCURO".
		//
		// O `dl` la' em cima foi obtido ANTES do `BeginPopup`, entao apontava
		// para o draw list do MENU. Todo o quadrado SV, a barra de matiz e a de
		// alfa eram desenhados NELE  ou seja, POR BAIXO da janela do popup, que
		// tem fundo proprio. Resultado no print: popup preto com o hex aparecendo
		// (esse e' texto do ImGui, desenhado no list certo).
		//
		// `GetWindowDrawList()` dentro do popup devolve o list da janela ATUAL,
		// que e' o popup. A amostra (fora do popup) continua usando o list do
		// menu, e por isso os dois `dl` existem.
		// -----------------------------------------------------------------
		ImDrawList* pdl = ImGui::GetWindowDrawList();
		// (o `dl` da amostra, acima, continua valendo fora do popup)

		float h = 0.0f, s = 0.0f, v = 0.0f;
		RgbParaHsv(col.r / 255.0f, col.g / 255.0f, col.b / 255.0f, h, s, v);

		const float  sqW = 238.0f, sqH = 120.0f, barW = 16.0f, alfaH = 12.0f;
		const ImVec2 base = ImGui::GetCursorScreenPos();

		// ---------------- quadrado S/V ----------------
		ImGui::InvisibleButton("##sv", ImVec2(sqW, sqH));
		if (ImGui::IsItemActive() || ImGui::IsItemClicked()) {
			const ImVec2 m = ImGui::GetIO().MousePos;
			s = ImClamp((m.x - base.x) / sqW, 0.0f, 1.0f);
			v = ImClamp(1.0f - (m.y - base.y) / sqH, 0.0f, 1.0f);
		}

		float hr, hg, hb;
		HsvParaRgb(h, 1.0f, 1.0f, hr, hg, hb);
		const ImU32 corHue = IM_COL32((int)(hr * 255), (int)(hg * 255), (int)(hb * 255), 255);
		const ImVec2 sq0 = base, sq1 = ImVec2(base.x + sqW, base.y + sqH);
		// branco -> matiz na horizontal, preto na vertical (mesma aproximacao
		// que todo picker usa: dois gradientes em vez de 1 pixel por cor)
		pdl->AddRectFilledMultiColor(sq0, sq1, IM_COL32(255, 255, 255, 255), corHue, corHue, IM_COL32(255, 255, 255, 255));
		pdl->AddRectFilledMultiColor(sq0, sq1, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 255), IM_COL32(0, 0, 0, 255));
		pdl->AddRect(sq0, sq1, IM_COL32(255, 255, 255, 40), 3.0f);

		const ImVec2 cur(base.x + s * sqW, base.y + (1.0f - v) * sqH);
		pdl->AddCircle(cur, 5.0f, IM_COL32(0, 0, 0, 200), 16, 2.5f);
		pdl->AddCircle(cur, 5.0f, IM_COL32(255, 255, 255, 235), 16, 1.2f);

		// ---------------- barra de matiz (vertical) ----------------
		ImGui::SetCursorScreenPos(ImVec2(base.x + sqW + 10.0f, base.y));
		ImGui::InvisibleButton("##matiz", ImVec2(barW, sqH));
		if (ImGui::IsItemActive() || ImGui::IsItemClicked()) {
			const float my = ImGui::GetIO().MousePos.y - base.y;
			h = ImClamp(my / sqH, 0.0f, 0.9999f);
		}
		{
			const ImVec2 b0(base.x + sqW + 10.0f, base.y);
			const ImVec2 b1(b0.x + barW, b0.y + sqH);
			const int    passos = 24;
			for (int k = 0; k < passos; k++) {
				float r0, g0, b0c, r1, g1, b1c;
				HsvParaRgb((float)k / passos, 1.0f, 1.0f, r0, g0, b0c);
				HsvParaRgb((float)(k + 1) / passos, 1.0f, 1.0f, r1, g1, b1c);
				const float y0 = b0.y + sqH * (float)k / passos;
				const float y1 = b0.y + sqH * (float)(k + 1) / passos;
				pdl->AddRectFilledMultiColor(ImVec2(b0.x, y0), ImVec2(b1.x, y1),
				                            IM_COL32((int)(r0 * 255), (int)(g0 * 255), (int)(b0c * 255), 255),
				                            IM_COL32((int)(r0 * 255), (int)(g0 * 255), (int)(b0c * 255), 255),
				                            IM_COL32((int)(r1 * 255), (int)(g1 * 255), (int)(b1c * 255), 255),
				                            IM_COL32((int)(r1 * 255), (int)(g1 * 255), (int)(b1c * 255), 255));
			}
			pdl->AddRect(b0, b1, IM_COL32(255, 255, 255, 40), 3.0f);
			const float hy = b0.y + h * sqH;
			pdl->AddRect(ImVec2(b0.x - 2.0f, hy - 2.0f), ImVec2(b1.x + 2.0f, hy + 2.0f),
			            IM_COL32(255, 255, 255, 235), 2.0f, 0, 1.5f);
		}

		// ---------------- barra de alfa ----------------
		ImGui::SetCursorScreenPos(ImVec2(base.x, base.y + sqH + 12.0f));
		ImGui::InvisibleButton("##alfa", ImVec2(sqW + 10.0f + barW, alfaH));
		if (ImGui::IsItemActive() || ImGui::IsItemClicked()) {
			const float mx = ImGui::GetIO().MousePos.x - base.x;
			col.a = (int)(ImClamp(mx / sqW, 0.0f, 1.0f) * 255.0f);
		}
		{
			const ImVec2 a0(base.x, base.y + sqH + 12.0f);
			const ImVec2 a1(a0.x + sqW, a0.y + alfaH);
			// xadrez de transparencia
			for (int y = 0; y < 2; y++)
				for (int x = 0; x < 24; x++)
					pdl->AddRectFilled(ImVec2(a0.x + x * (sqW / 24.0f), a0.y + y * (alfaH * 0.5f)),
					                  ImVec2(a0.x + (x + 1) * (sqW / 24.0f), a0.y + (y + 1) * (alfaH * 0.5f)),
					                  ((x + y) & 1) ? IM_COL32(90, 90, 90, 255) : IM_COL32(60, 60, 60, 255));
			pdl->AddRectFilledMultiColor(a0, a1,
			                            IM_COL32(col.r, col.g, col.b, 0), IM_COL32(col.r, col.g, col.b, 255),
			                            IM_COL32(col.r, col.g, col.b, 255), IM_COL32(col.r, col.g, col.b, 0));
			pdl->AddRect(a0, a1, IM_COL32(255, 255, 255, 40), 3.0f);
			const float ax = a0.x + (col.a / 255.0f) * sqW;
			pdl->AddRect(ImVec2(ax - 2.0f, a0.y - 2.0f), ImVec2(ax + 2.0f, a1.y + 2.0f),
			            IM_COL32(255, 255, 255, 235), 2.0f, 0, 1.5f);
		}

		// ---------------- aplica o resultado ----------------
		float fr, fg, fb;
		HsvParaRgb(h, s, v, fr, fg, fb);
		col.r = (int)(fr * 255.0f + 0.5f);
		col.g = (int)(fg * 255.0f + 0.5f);
		col.b = (int)(fb * 255.0f + 0.5f);

		// ---------------- rodape ----------------
		ImGui::SetCursorScreenPos(ImVec2(base.x, base.y + sqH + 12.0f + alfaH + 10.0f));
		ImGui::TextDisabled(L("#%02X%02X%02X  %d, %d, %d, %d"),
		                    col.r, col.g, col.b, col.r, col.g, col.b, col.a);

		ImGui::EndPopup();
	}

	ImGui::PopID();
}
bool TestMouseEnabled = false;
std::atomic_bool TestMouseRunning = false;
std::thread TestMouseThread;

std::string GLiveLogs;
std::mutex GLiveLogsMutex;

// ---------------------------------------------------------------------------
// Caminho do logs.txt  resolvido UMA vez, na pasta de artefatos.
//
// ANTES: `logs.txt` relativo, ou seja, AO LADO DO .EXE. Isso montava no disco
// o conjunto `mtmacr0.exe` + `logs.txt` + `imgui.ini` na mesma pasta  tres
// arquivos que, juntos, descrevem exatamente o que o programa e'.
//
// Agora vai para a mesma pasta neutra da configuracao. O arquivo continua
// existindo e continua sendo o que interessa para suporte (e' de onde saiu
// TODO diagnostico desta sessao); so' mudou de lugar.
//
// Fallback para "logs.txt" relativo se LOCALAPPDATA nao existir: um log no
// lugar errado e' melhor que log nenhum.
// ---------------------------------------------------------------------------
static const std::filesystem::path& CaminhoDoLog()
{
	static const std::filesystem::path p = [] {
		// -------------------------------------------------------------------
		// CADEIA DE CANDIDATOS, COM PROVA DE ESCRITA.
		//
		// A versao anterior escolhia `PastaDeArtefatos()` e pronto. Se o
		// `ofstream` nao abrisse, o `is_open()` era falso, TODA chamada de LOG
		// virava no-op e o programa seguia normalmente  janela, menu, botao.
		// O sintoma era o pior possivel: "falha no mapeamento - ver logs.txt"
		// e o logs.txt nao existia.
		//
		// Agora cada candidato e' TESTADO abrindo de verdade. O primeiro que
		// aceitar a escrita vence:
		//   1. %LOCALAPPDATA%\RenderCache   (o lugar certo, neutro)
		//   2. %TEMP%                       (sempre gravavel)
		//   3. %LOCALAPPDATA%               (se a subpasta falhar)
		//   4. a pasta do .exe              (ultimo recurso)
		//
		// Um log no lugar errado e' infinitamente melhor que log nenhum  e'
		// a unica janela para dentro de um app que nao tem console.
		// -------------------------------------------------------------------
		std::vector<std::filesystem::path> candidatos;

		{
			std::filesystem::path pasta = PastaDeArtefatos();
			if (!pasta.empty()) candidatos.push_back(pasta);
		}
		if (const char* t = getenv("TEMP"))         candidatos.emplace_back(t);
		if (const char* l = getenv("LOCALAPPDATA")) candidatos.emplace_back(l);
		{
			char buf[MAX_PATH] = { 0 };
			if (GetModuleFileNameA(nullptr, buf, MAX_PATH))
				candidatos.emplace_back(std::filesystem::path(buf).parent_path());
		}

		for (const auto& c : candidatos) {
			std::error_code ec;
			std::filesystem::create_directories(c, ec);
			const auto f = c / "logs.txt";
			std::ofstream teste(f, std::ios::app);
			if (teste.is_open()) {
				teste.close();
				return f;
			}
		}

		// Nenhum candidato abriu: devolve o relativo e o Run() percebe que
		// falhou (ele checa o is_open do arquivo truncado).
		return std::filesystem::path("logs.txt");
	}();
	return p;
}

// ---------------------------------------------------------------------------
// ESTADO DO ARQUIVO DE LOG
//
// ===========================================================================
// POR QUE ISTO E' `static` DE FUNCAO E NAO DE NAMESPACE
// ===========================================================================
//
// A primeira versao desta otimizacao declarou
//
//     static std::ofstream GLogFile;      // escopo de ARQUIVO
//
// e derrubou o processo na largada: 0xC0000005 (ACCESS_VIOLATION) dentro de
// MSVCP140.dll, em `mov rcx, qword ptr [rax+8]`, com `rax` invalido.
//
// A causa e' o static initialization order fiasco. Objetos de escopo de
// arquivo sao construidos na inicializacao estatica, e a ORDEM entre unidades
// de traducao nao e' definida. Se QUALQUER global de outro .cpp chamar LOG()
// no proprio construtor, o BaseLog roda antes do GLogFile existir  e `open()`
// escreve em memoria que ainda e' zeros.
//
// Um `static` DENTRO DE FUNCAO nao tem esse problema: o compilador gera uma
// guarda (magic static, C++11) e o objeto e' construido na PRIMEIRA chamada,
// seja ela quando for. Mesma economia de abrir o arquivo uma vez so', sem
// depender de nenhuma ordem.
//
// A licao vale para o resto deste arquivo: `GLiveLogs` e `GLiveLogsMutex` SAO
// de escopo de namespace, e por isso o BaseLog nao pode confiar neles para
// funcionar. Onde a inicializacao importa, o objeto e' local.
// ===========================================================================
static std::ofstream& ArquivoDeLog() {
	static std::ofstream f(CaminhoDoLog(), std::ios::app);
	return f;
}

// Mutex do arquivo: local de funcao pelo mesmo motivo do ofstream acima.
static std::mutex& MutexDoLog() {
	static std::mutex m;
	return m;
}

// Ligado so' quando PUBGEXT_CONSOLE=1 realmente alocou um console (ver Run).
// Sem isto o `printf` roda num processo que nao tem console nenhum.
//
// `bool` e' POD: sem construtor, sem ordem de inicializacao, sempre seguro.
bool GConsoleAtivo = false;

void BaseLog(const char* fmt, ...) {
	// -----------------------------------------------------------------------
	// BUFFER LOCAL  nem `static`, nem `thread_local`.
	//
	// `static char buf[8192]` era um buffer UNICO compartilhado por todas as
	// threads: duas chamadas simultaneas formatavam no MESMO buffer e a linha
	// que chegava no arquivo era um misto das duas. Esse era o bug de verdade.
	//
	// `thread_local` conserta a concorrencia  mas troca por outra dependencia
	// do mesmo tipo que me derrubou hoje: a variavel passa a viver no TLS do
	// thread, e o TLS pode nao estar pronto se o BaseLog rodar durante a
	// inicializacao estatica. E' a mesma armadilha do ofstream de escopo de
	// arquivo, s que mais dificil de enxergar.
	//
	// Um buffer LOCAL resolve os dois de uma vez: cada chamada tem o seu 
	// logo e' thread-safe POR CONSTRUCAO, sem lock nenhum na formatacao  e
	// ele vive na pilha, sem TLS, sem ordem de inicializacao, sem nada.
	//
	// Sao 8 KB de pilha liberados no retorno; o default de thread e' 1 MB.
	// -----------------------------------------------------------------------
	char buf[8192];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (GConsoleAtivo) printf("%s", buf);

	// -----------------------------------------------------------------------
	// O ARQUIVO FICA ABERTO  e o mutex e' LOCAL DE FUNCAO.
	//
	// Antes: `std::ofstream logFile(CaminhoDoLog(), app)` por LINHA. Sao
	// quatro operacoes de sistema por chamada  abrir, escrever, flush, fechar
	//  e o log roda na thread de memoria a 144 Hz.
	//
	// Agora abre uma vez e mantem. E o `flush()` por linha CONTINUA, de
	// proposito: e' ele que entrega a linha ao sistema operacional na hora, e
	// e' isso que faz o logs.txt sobreviver a um crash.
	//
	// O mutex passou a ser `MutexDoLog()` (local de funcao) e NAO o
	// `GLiveLogsMutex` de escopo de namespace: este ultimo so' existe depois da
	// inicializacao estatica, e o BaseLog pode rodar antes disso. Ver o bloco
	// grande acima do ArquivoDeLog.
	// -----------------------------------------------------------------------
	{
		std::lock_guard<std::mutex> lock(MutexDoLog());
		std::ofstream& f = ArquivoDeLog();
		if (f.is_open()) {
			f << buf;
			f.flush();
		}
	}

	// -----------------------------------------------------------------------
	// BUFFER DE LOG VIVO  SO' EXISTE PARA A ABA LOGS.
	//
	// Aqui, sim, o `GLiveLogs` de escopo de namespace e' usado  e ele TEM
	// construtor, entao depende de ordem. Fica dentro de um `if` defensivo:
	// se o buffer ainda nao foi construido, a leitura e' cara ou nada, mas
	// nao derruba o processo no caminho que importa (o arquivo acima ja'
	// recebeu a linha).
	//
	// Sem a aba (build publico) isto era trabalho puro: uma string que cresce
	// ate' 100 KB, com mutex, alimentada por CADA chamada de log  para nada.
	// -----------------------------------------------------------------------
#if MENU_DEBUG_TABS
	GLiveLogs += buf;
	if (GLiveLogs.size() > 100000) {
		GLiveLogs.erase(0, GLiveLogs.size() - 50000);
	}
#endif
}
void MemoryLoop() {
	GMemThreadViva.store(true, std::memory_order_relaxed);
	while (GIsRunning) {
		// ---------------------------------------------------------------------
		// INSTRUMENTADO: o tick INTEIRO da thread de memoria. TEM que ficar
		// DENTRO do while  a primeira versao foi inserida ANTES dele, entao
		// `memTicks` foi de 1 para 1 e o log saiu com `mem=0 t/s` enquanto o
		// `IOCTL=53000/s` provava que a thread estava rodando. O contador
		// mentia e nao avisava.
		// ---------------------------------------------------------------------
		Perf::Escopo eTickMem(Perf::C.memNs);
		Perf::Contar(Perf::C.memTicks);
		int rate = Configs.Survivor.MemoryUpdateRate > 0 ? Configs.Survivor.MemoryUpdateRate : 144;
		auto target = std::chrono::microseconds(1000000 / rate);
		auto start = std::chrono::high_resolution_clock::now();

		if (EngineInstance) {
			try {
				{ Perf::Escopo eSub(Perf::C.nsCache); EngineInstance->Cache(); }
				
				// -----------------------------------------------------------------
				// UpdatePlayers/UpdateGrenades/UpdateWeapons/UpdateVehicles/
				// UpdateLoot num try/catch PROPRIO: se qualquer um lancar
				// excecao, ela nao pula o GSharedData update logo abaixo (era
				// isso que fazia o debug ficar zerado mesmo o Cache achando 20
				// atores).
				//
				// O `RefreshViewMatrix` SAIU daqui de proposito: ele tem bloco
				// proprio logo abaixo. Ver o comentario la'.
				// -----------------------------------------------------------------
				try {
					if (!EngineInstance->FrameTranslateError && EngineInstance->PlayerCameraManager != 0) {
						{ Perf::Escopo eSub(Perf::C.nsPlayers); EngineInstance->UpdatePlayers(); }
						{ Perf::Escopo eSub(Perf::C.nsGrenades); EngineInstance->UpdateGrenades(); }
						{ Perf::Escopo eSub(Perf::C.nsWeapons); EngineInstance->UpdateWeapons(); }
						{ Perf::Escopo eSub(Perf::C.nsVehicles); EngineInstance->UpdateVehicles(); }

						// -------------------------------------------------
						// UpdateLoot em taxa REDUZIDA  30 Hz, nao 144.
						//
						// Ele faz a leitura em lote mais pesada do cheat: monta
						// um BatchRead com TODOS os atores de loot do mapa e
						// resolve nome/categoria de cada um, a cada tick da
						// thread de memoria.
						//
						// Mas item no chao NAO SE MEXE. A posicao so' muda
						// quando alguem pega (e sai da lista) ou dropa (e ja'
						// entra na proxima varredura). 30 Hz e' indistinguivel
						// de 144 Hz para o olho, e corta ~79% desse trabalho.
						//
						// Os outros updates ficam na taxa cheia: jogador,
						// granada e veiculo se movem de verdade.
						// -------------------------------------------------
						static auto ultimoLoot = std::chrono::steady_clock::now();
						const auto agoraLoot = std::chrono::steady_clock::now();
						if (std::chrono::duration_cast<std::chrono::milliseconds>(agoraLoot - ultimoLoot).count() >= 33) {
							ultimoLoot = agoraLoot;
							{ Perf::Escopo eSub(Perf::C.nsLoot); EngineInstance->UpdateLoot(); }
						}
					}
				} catch (...) {
					static bool loggedUpdExc = false;
					if (!loggedUpdExc) {
						loggedUpdExc = true;
						LOG("[EXC] UpdatePlayers/UpdateGrenades/UpdateWeapons/UpdateVehicles/UpdateLoot lancou excecao\n");
					}
				}

				// -----------------------------------------------------------------
				// CAMERA DO AIMBOT — FORA DO try ACIMA, DE PROPOSITO.
				//
				// `Engine::CameraEntry` e' a camera que o AIMBOT projeta
				// (GSharedData.CameraCache -> Engine::GetCameraCache), e este e'
				// o UNICO lugar que a atualiza.
				//
				// Antes ele era a ULTIMA linha de dentro do try dos Update*.
				// Qualquer excecao em qualquer um daqueles cinco (engolida pelo
				// catch, logada 1x) pulava a atualizacao da camera. O ESP nao
				// sentia — ele rele' a camera sozinho no RenderFrame
				// (Camera::GetCurrentView, mais abaixo) — mas o aimbot ficava
				// com FOV=0 e TODA projecao dele devolvia (0,0). Aimbot morto,
				// ESP vivo, nenhum log: exatamente o sintoma relatado.
				//
				// `FrameTranslateError` tambem saiu do gate: ele significa "uma
				// leitura QUALQUER falhou neste tick", nao "a camera esta' ruim".
				// O que protege contra camera ruim agora e' a validacao dentro
				// do RefreshViewMatrix (que mantem o valor anterior em vez de
				// gravar lixo).
				// -----------------------------------------------------------------
				if (EngineInstance->PlayerCameraManager != 0) {
					{ Perf::Escopo eSub(Perf::C.nsViewMatrix); try { EngineInstance->RefreshViewMatrix(); } catch (...) {} }
				}

				// -----------------------------------------------------------------
				// DIAGNOSTICO 1x/2s: candidatos da cadeia do RCS (PreEvalPawnState)
				// e leitura do GameState (zona azul / minimapa).
				//
				// Roda FORA do try acima para nao ser afetado por excecao dos
				// updates, e so' quando o jogo esta' legivel.
				// -----------------------------------------------------------------
				{
					static auto lastDiag = std::chrono::steady_clock::now();
					auto nowDiag = std::chrono::steady_clock::now();
					if (std::chrono::duration_cast<std::chrono::milliseconds>(nowDiag - lastDiag).count() > 2000) {
						lastDiag = nowDiag;
						if (EngineInstance->AcknowledgedPawn >= 0x1000000) {
							try { EngineInstance->DiagChain(); } catch (...) {}
						}
						if (EngineInstance->UWorld) {
							try { EngineInstance->ReadGameState(); } catch (...) {}
							try { EngineInstance->UpdatePlayerList(); } catch (...) {}
						}
					}
				}

				bool dataChanged = false;
				{
					Perf::Escopo ePub(Perf::C.nsPublish);
					std::lock_guard<std::mutex> lock(GDataMutex);
					{
						GSharedData.Actors = EngineInstance->Actors;
					GSharedData.GrenadeActors = EngineInstance->GrenadeActors;
					GSharedData.VehicleActors = EngineInstance->VehicleActors;
					GSharedData.LootActors = EngineInstance->LootActors;
						// ---------------------------------------------------------
						// GATE DE DESENHO — publicado JUNTO com as listas.
						//
						// `Engine::bInMatch` diz se o Cache() deste tick conseguiu
						// ler um estado de PARTIDA (e nao so' um UWorld qualquer:
						// o lobby tambem tem UWorld). O RenderFrame usa isto para
						// nao desenhar nada quando nao ha' partida — e' o que
						// impede o "ESP fantasma" congelado na tela do lobby.
						//
						// Vem do Engine, nao de uma heuristica de tempo aqui: e'
						// recalculado em todo tick, entao volta a true sozinho no
						// primeiro frame de uma partida nova.
						// ---------------------------------------------------------
						GSharedData.DataValid = EngineInstance->bInMatch;
						GSharedData.CameraCache = EngineInstance->GetCameraCache();
						GSharedData.UWorld = EngineInstance->UWorld;
						if (EngineInstance->UWorld != 0) {
							GInitOk.store(true, std::memory_order_relaxed);
						}
						GSharedData.GNames = EngineInstance->GNames;
						GSharedData.GameInstance = EngineInstance->GameInstance;
						GSharedData.AcknowledgedPawn = EngineInstance->AcknowledgedPawn;
						GSharedData.LocalCharacterPawn = EngineInstance->LocalCharacterPawn;
						GSharedData.CameraManagerAddr = EngineInstance->PlayerCameraManager;
						GSharedData.Recoil = Local.Recoil;
						GSharedData.MemoryThreadId = GetCurrentThreadId();

						// ---- GameState: zona azul / zona segura ----
						GSharedData.HasGameState = (EngineInstance->GameState != 0);
						GSharedData.BlueZoneRadius = EngineInstance->BlueZoneRadius;
						GSharedData.SafetyZoneRadius = EngineInstance->SafetyZoneRadius;
						GSharedData.BlueZonePos = { EngineInstance->BlueZonePos.X,
						                            EngineInstance->BlueZonePos.Y,
						                            EngineInstance->BlueZonePos.Z };
						GSharedData.SafetyZonePos = { EngineInstance->SafetyZonePos.X,
						                              EngineInstance->SafetyZonePos.Y,
						                              EngineInstance->SafetyZonePos.Z };

						GSharedData.LastUpdateTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

						// ---- visibilidade (LastTimeScreenRenderered) ----
						GSharedData.VisWorldTime  = EngineInstance->VisWorldTime;
						GSharedData.VisLocalLr    = EngineInstance->VisLocalLr;
						GSharedData.VisLocalDlyMs = EngineInstance->VisLocalDlyMs;
						GSharedData.VisLocalVis   = EngineInstance->VisLocalVis;
						GSharedData.VisInimLr     = EngineInstance->VisInimLr;
						GSharedData.VisInimDlyMs  = EngineInstance->VisInimDlyMs;
						GSharedData.VisInimVis    = EngineInstance->VisInimVis;
						GSharedData.VisAtivos     = EngineInstance->VisAtivos;
						GSharedData.VisOcultos    = EngineInstance->VisOcultos;
						GSharedData.VisAtivo      = EngineInstance->VisAtivo;
						GSharedData.VisBaseFonte  = EngineInstance->VisBaseFonte;
						GSharedData.VisCampoVivo  = EngineInstance->VisCampoVivo;
						GSharedData.VisCampo      = EngineInstance->VisCampoEscolhido;
						for (int k = 0; k < 3; k++) {
							GSharedData.VisInimParado[k]  = EngineInstance->VisInimParadoMs[k];
							GSharedData.VisLocalParado[k] = EngineInstance->VisLocalParadoMs[k];
						}

						dataChanged = true;
					}
				}

#if !MENU_SO_ESP
				if (dataChanged) {
					{ Perf::Escopo eSub(Perf::C.nsAimbot); Aimbot::Tick(); }
				}

				// RCS roda TODO frame (nao depende do aimbot estar ligado).
				Aimbot::RecoilControl();
#endif
				static int consecutiveFaults = 0;
				if (EngineInstance->FrameTranslateError) {
					consecutiveFaults++;
					if (consecutiveFaults > 10) { 
						// silencioso: esta chamada roda DENTRO do loop de
						// memoria. Quando a traducao de frame esta falhando
						// a 144 Hz, ela dispara ~14x por segundo e enchia o
						// log de "leitura ok" — escondendo o problema real,
						// que e o FrameTranslateError.
						TargetProcess.ProbeReadable(true);
						consecutiveFaults = 0; 
					}
				} else {
					consecutiveFaults = 0; 
				}

			} catch (...) {}
		}
		auto end = std::chrono::high_resolution_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
		
		if (elapsed < target) {
			std::this_thread::sleep_for(target - elapsed);
		}
	}
	GMemThreadViva.store(false, std::memory_order_relaxed);
}

// ===========================================================================
// ABA WORLD  catalogo de loot (auto-populado pelo Engine)
// G_LootCatalog e' escrito pela THREAD DE MEMORIA (Engine::UpdateLoot, via
// MemoryLoop) e lido aqui, na THREAD DE RENDER. Toda passada no catalogo e'
// feita com G_LootCatalogMutex na mao.
//
// Ja' Configs.Survivor.LootItemFilter e' tocado SO' nesta thread — e e' por
// isso que o ESP (DrawLootEsp) pode filtrar por ele sem lock nenhum.
// ===========================================================================

// Ponte catalogo <-> config. Roda 1x por frame e sai na hora se o Engine nao
// registrou item novo (G_LootCatalogDirty), entao nao custa nada no caso
// normal.
//
// Regras:
//   - item NOVO (o cheat acabou de ver) -> entra no config com o estado que o
//     catalogo deu (true);
//   - item que o config JA' conhece      -> o CONFIG manda. E' a escolha do
//     usuario, inclusive um "desligado" salvo numa sessao anterior: sem isso
//     o item voltaria LIGADO no primeiro frame em que fosse visto de novo.
static void SyncLootCatalogIntoConfig()
{
	// O early-return vem ANTES do lock: `G_LootCatalogDirty` e'' um bool
	// simples, e pegar um mutex compartilhado com a thread de memoria 144x/s
	// so'' para ler uma flag transfere a linha de cache entre os nucleos a toa.
	if (!G_LootCatalogDirty) return;
	std::lock_guard<std::mutex> lock(G_LootCatalogMutex);

	auto& filtro = Configs.Survivor.LootItemFilter;
	for (auto& kv : G_LootCatalog) {
		auto it = filtro.find(kv.first);
		if (it == filtro.end()) filtro[kv.first] = kv.second;
		else                    kv.second = it->second;
	}
	G_LootCatalogDirty = false;
}

// ===========================================================================
// SeedLootCatalogFromConfig  devolve ao catalogo o que JA' foi visto.
//
// O PROBLEMA QUE ISTO CONSERTA (relatado pelo usuario):
//
//   "na aba WORLD aparece os itens que vao aparecendo no mapa, caso nao tenha
//    por exemplo uma beryl no mapa simplesmente nao aparece na lista, mas essa
//    lista teria que ser fixa"
//
//   A aba WORLD desenha a partir de `G_LootCatalog`, que vive SO' NA MEMORIA e
//   NASCE VAZIA a cada abertura do cheat. O `SyncLootCatalogIntoConfig` copia
//   catalogo -> config (e o config E' salvo em disco), mas NADA fazia o
//   caminho de volta. Entao:
//
//     - o config guardava corretamente tudo que voce ja' viu;
//     - e o catalogo ignorava isso e comecava do zero toda sessao;
//     - resultado: item que nao esta' no mapa AGORA nao aparece na lista.
//
//   Semear do config resolve exatamente o que ele pediu  a lista fica FIXA e
//   so' cresce, porque o config persiste entre partidas. E sem eu escrever
//   lista de nome de classe: os nomes vem do proprio jogo, medidos.
//
// Por que UMA vez so': depois da primeira passada, quem manda e' o jogo (o
// `UpdateLoot` registra item novo) e o `SyncLootCatalogIntoConfig` leva de
// volta pro config. Semear em todo frame sobrescreveria o estado vivo com o
// estado antigo do disco.
// ===========================================================================
static void SeedLootCatalogFromConfig()
{
	static bool feito = false;
	if (feito) return;
	feito = true;

	std::lock_guard<std::mutex> lock(G_LootCatalogMutex);
	for (auto& kv : Configs.Survivor.LootItemFilter) {
		G_LootCatalog[kv.first] = kv.second;
	}
	// O catalogo acabou de ficar igual ao config: nao ha' o que sincronizar.
	G_LootCatalogDirty = false;
}

// Liga/desliga em massa. catIdx < 0 = todas as categorias (Enable/Disable All).
// Grava no catalogo (sob mutex) E no mapa do config na MESMA passada, senao os
// dois divergem e o save perde a escolha.
static void LootSetVisibleCategory(int catIdx, bool visivel)
{
	std::lock_guard<std::mutex> lock(G_LootCatalogMutex);
	auto& filtro = Configs.Survivor.LootItemFilter;
	for (auto& kv : G_LootCatalog) {
		if (catIdx >= 0 && (int)ClassifyLootRaw(kv.first) != catIdx) continue;
		kv.second = visivel;
		filtro[kv.first] = visivel;
	}
}

// Um item so' (checkbox individual). Grava no config mesmo se o nome ja' nao
// estiver no catalogo — o ESP le' o config, nao o catalogo.
static void LootSetVisibleItem(const std::string& raw, bool visivel)
{
	std::lock_guard<std::mutex> lock(G_LootCatalogMutex);
	auto it = G_LootCatalog.find(raw);
	if (it != G_LootCatalog.end()) it->second = visivel;
	Configs.Survivor.LootItemFilter[raw] = visivel;
}

// Em massa, pelo FILTRO DE SUBSTRING: todo item do catalogo que casa com
// alguma substring ativa recebe `visivel`  no catalogo E no config, na mesma
// passada (se divergirem, o save perde a escolha).
//
// E' a ponte entre os dois filtros: o substring decide o que o ESP mostra
// AGORA, e este botao grava essa decisao no filtro INDIVIDUAL, que persiste e
// continua valendo mesmo se a substring for apagada depois.
static void LootSetVisibleMatching(bool visivel)
{
	std::lock_guard<std::mutex> lock(G_LootCatalogMutex);
	auto& filtro = Configs.Survivor.LootItemFilter;
	for (auto& kv : G_LootCatalog) {
		if (!Configs.Survivor.LootRawCasaAlgumFiltro(kv.first)) continue;
		kv.second = visivel;
		filtro[kv.first] = visivel;
	}
}

// ===========================================================================
// Item da sidebar do menu (visual da referencia Neverlose).
//
// Desenhado a' mao com InvisibleButton + DrawList, e nao com
// ImGui::Selectable: o realce do Selectable e' um retangulo SEM
// arredondamento, e o padrao que o usuario pediu usa realce arredondado +
// barra de acento colada na esquerda.
//
// `ativo` guarda o indice da aba selecionada; o retorno diz se ESTE item foi
// clicado neste frame. Usa so' API publica do ImGui (imgui.h) de proposito:
// imgui_internal.h nao esta' na cadeia de includes do Main.cpp.
// ===========================================================================
static bool MenuSidebarItem(const char* rotulo, int indice, int* ativo)
{
	const float  altura  = 32.0f;
	const float  largura = ImGui::GetContentRegionAvail().x;
	const ImVec2 pos     = ImGui::GetCursorScreenPos();

	ImGui::InvisibleButton(rotulo, ImVec2(largura, altura));
	const bool clicado = ImGui::IsItemClicked();
	const bool hover   = ImGui::IsItemHovered();
	const bool sel     = (*ativo == indice);
	if (clicado) *ativo = indice;

	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 a = pos;
	const ImVec2 b = ImVec2(pos.x + largura, pos.y + altura);

	// Realce arredondado. O acento e' o ROXO do projeto (#8B5CF6), adotado em
	// 19/10 no lugar do ciano  ver a paleta do ApplyProfessionalStyle.
	if (sel)        dl->AddRectFilled(a, b, IM_COL32(139, 92, 246, 44), 7.0f);
	else if (hover) dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 14), 7.0f);

	// Barra de acento (3px) do item ativo.
	if (sel) dl->AddRectFilled(ImVec2(a.x, a.y + 7.0f), ImVec2(a.x + 3.0f, b.y - 7.0f),
	                           IM_COL32(139, 92, 246, 255), 2.0f);

	const ImU32 cor = sel   ? IM_COL32(167, 139, 250, 255)
	                : hover ? IM_COL32(232, 236, 245, 255)
	                        : IM_COL32(146, 152, 168, 255);
	const ImVec2 medida = ImGui::CalcTextSize(rotulo);
	dl->AddText(ImVec2(a.x + 16.0f, a.y + (altura - medida.y) * 0.5f), cor, rotulo);
	return clicado;
}

// ===========================================================================
// CheckboxPair  DUAS COLUNAS DE CHECKBOX, CADA UMA COM O SEU TOGGLE.
//
// `ImGui::Checkbox` deste fork desenha o trilho na borda direita da REGIAO DE
// CONTEUDO (nao do item). Numa linha de duas colunas com `SameLine(240)` os
// dois trilhos caem no MESMO x  ficam sobrepostos  e o rotulo da segunda
// coluna aparece sem toggle ao lado. Era exatamente o "checkboxes
// desalinhados" do print.
//
// Aqui cada coluna recebe METADE da largura via `CheckboxW`, entao o trilho
// fica na borda de cada metade:
//
//     [rotulo 1 ................. toggle] [rotulo 2 ................. toggle]
//
// `SameLine(0,0)` (e nao `SameLine(240)`) porque a primeira coluna JA' tem
// exatamente a largura da metade: somar ItemSpacing empurraria a segunda para
// fora do painel.
// ===========================================================================
// ===========================================================================
// Seccao  TITULO DE SECAO do menu.
//
// Substitui o `SeparatorText` do ImGui (que desenha duas linhas cinzas com o
// texto no meio: apagado, sem hierarquia). Aqui:
//   - barra de acento 3x14 a' esquerda;
//   - rotulo na fonte de TITULO (17 px semibold)  e' o que cria o degrau
//     visual entre "secao" e "controle";
//   - filete que nasce no acento e desbota para a direita.
//
// A assinatura aceita o texto ja' traduzido por `L(...)`, igual ao
// `SeparatorText`, entao a troca nos call sites e' mecanica e o
// `check_lang` continua vendo o mesmo literal.
// ===========================================================================
// ===========================================================================
// LOADER  a janela de entrada do cheat.
//
// TRES BOTOES, e cada um faz UMA coisa:
//   Carregar driver     gate de anti-cheat + mapeamento
//   Descarregar driver  UNMAP (SELFDESTRUCT)  so' com a sessao parada
//   Iniciar PUBG        passa a aguardar o TslGame.exe; ao achar, abre a sessao
//                       e o menu
//
// O END encerra a SESSAO e volta pra ca' (o driver fica carregado). No loader,
// o END sai do processo  e' o panic de sempre.
// ===========================================================================
// ===========================================================================
// KillAntiCheatProcessesThread — LOADER KO BLOCK NAHI KARTA
//
// Kyun thread:
//   TerminateProcess + OpenProcess blocking calls hain. Agar UI thread pe
//   chalao to loader freeze ho jayega (jo tumne explicitly mana kiya hai).
//   Background thread mein chalao, aur status mutex se update karo.
//
// Kyun "safe" method:
//   - OpenProcess PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION
//     (PROCESS_ALL_ACCESS nahi — 0x1F0FFF kernel-level handle AC ko
//     suspicious lagta hai; PROCESS_ALL_ACCESS ka use anti-cheat driver
//     handle-callback mein flag karta hai).
//   - TerminateProcess ke baad WaitForSingleObject(handle, 2000) — process
//     ko poori tarah exit hone ka wait karo. Bina wait ke race condition:
//     driver load hote waqt BE service phir se start ho jaye.
//   - SNAPSHOT ek baar lo, phir loop. Har iteration mein new snapshot nahi —
//     ye paging storm avoid karta hai.
//   - Sirf EXACT name match (_wcsicmp). Substring match NAHI — warna
//     "steam.exe" search "steamwebhelper.exe" ko bhi match kar lega.
// ===========================================================================
static void KillAntiCheatProcessesThread()
{
	LoaderStatus("Killing & Initializing...");

	// NAYA: UI ko 100ms do yellow status dikhane ke liye
	std::this_thread::sleep_for(std::chrono::milliseconds(4000));

	static const wchar_t* kAlvos[] = {
		// ---- PUBG / BattlEye ----
		L"TslGame.exe", L"TslGame_BE.exe", L"TslGame_BE_64.exe",
		L"BEService.exe", L"BEService64.exe", L"BEService_x64.exe", L"BEService_pubg.exe",
		L"BEDaisy.exe", L"BEDaisy64.exe", L"BEDaisy_64.exe",
		L"BEClient_x64.exe", L"BEClient.exe",
		// ---- Zakynthos / PUBG Internal ----
		L"Zakynthos.exe", L"zksvc.exe", L"ZakynthosService.exe",
		// ---- Steam (BE launcher dependency) ----
		L"steam.exe", L"steamwebhelper.exe", L"steamservice.exe", L"steamerrorreporter.exe",
		// ---- Riot Vanguard / Valorant ----
		L"vgc.exe", L"vgtray.exe", L"VALORANT.exe",
		L"RiotClientServices.exe", L"RiotClientUx.exe",
		// ---- Easy Anti-Cheat (EAC) ----
		L"EasyAntiCheat.exe", L"EasyAntiCheat_EOS.exe", L"EasyAntiCheat_Setup.exe",
		// ---- Other common AC ----
		L"FACEITClient.exe", L"FaceitAC.exe", L"ESEADriver2.exe",
		L"nProtect.exe", L"GameMon.des", L"xhunter1.exe",
	};

	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	int  mortos = 0;
	int  tentados = 0;

	if (snap != INVALID_HANDLE_VALUE) {
		PROCESSENTRY32W pe{};
		pe.dwSize = sizeof(pe);
		if (Process32FirstW(snap, &pe)) {
			do {
				if (pe.th32ProcessID == GetCurrentProcessId() || pe.th32ProcessID == 0 || pe.th32ProcessID == 4) continue;

				bool ehAlvo = false;
				for (const wchar_t* alvo : kAlvos) {
					if (_wcsicmp(pe.szExeFile, alvo) == 0) { ehAlvo = true; break; }
				}
				if (!ehAlvo) continue;

				tentados++;
				HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
				if (!h) {
					LOG("[-] kill: OpenProcess failed for %ls (pid %lu, err %lu)\n", pe.szExeFile, pe.th32ProcessID, GetLastError());
					continue;
				}

				if (TerminateProcess(h, 1)) {
					WaitForSingleObject(h, 2000);
					mortos++;
					LOG("[+] kill: %ls (pid %lu) terminated\n", pe.szExeFile, pe.th32ProcessID);
				}
				else {
					LOG("[-] kill: TerminateProcess failed for %ls (pid %lu, err %lu)\n", pe.szExeFile, pe.th32ProcessID, GetLastError());
				}
				CloseHandle(h);
			} while (Process32NextW(snap, &pe));
		}
		CloseHandle(snap);
	}

	// =======================================================================
	// 2. KERNEL DRIVERS: SIRF STOP KARO (NO DELETE)
	// =======================================================================
	// Delete nahi karenge, taake Steam integrity check ka masla na ho.
	// Stop karne se driver load nahi ho sakta, jo kaafi hai.
	// =======================================================================
	LOG("[*] Stopping kernel drivers (NO DELETE)...\n");
	LoaderStatus("Stopping kernel & Closing other process");

	static const wchar_t* kDrivers[] = {
		// ---- BattlEye ----
		L"BEDaisy",         // Main kernel driver
		L"BEService",       // Service jo driver load karta hai
		L"BEDaisy64",       // 64-bit variant
		// ---- Zakynthos / PUBG Internal ----
		L"zksvc",           // Zakynthos service name
		L"Zakynthos",       // Alternate name
		L"ZakynthosSvc",    // Alternate name
		// ---- Riot Vanguard ----
		L"vgk",             // Vanguard kernel driver
		// ---- Easy Anti-Cheat ----
		L"EasyAntiCheat",
		L"EasyAntiCheat_EOS",
		// ---- ESEA ----
		L"ESEADriver2",
	};

	int driversStopped = 0;

	for (const wchar_t* drv : kDrivers) {
		wchar_t cmd[256];

		// 1. Pehle check karo ke driver system me hai ya nahi (sc query)
		swprintf_s(cmd, L"sc query %s >nul 2>&1", drv);
		int queryResult = _wsystem(cmd);

		// Agar queryResult 0 hai, matlab driver system me hai
		if (queryResult == 0) {
			// 2. Agar hai, to stop karo
			swprintf_s(cmd, L"sc stop %s >nul 2>&1", drv);
			_wsystem(cmd);

			// 3. Thoda wait
			std::this_thread::sleep_for(std::chrono::milliseconds(300));

			// 4. Count badhao (sirf tab jab driver asal me tha)
			driversStopped++;
			LOG("[+] Driver stopped: %ls\n", drv);
		}
		else {
			// Agar driver system me nahi hai, to skip karo
			LOG("[*] Driver not found (skipped): %ls\n", drv);
		}
	}

	// Saare drivers ko stop hone ka time do
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	LOG("[+] All kernel drivers stopped.\n");

	// =======================================================================
	// 3. STATUS UPDATE (State Synchronization)
	// =======================================================================
	if (tentados == 0 && driversStopped == 0) {
		LoaderStatus("Kill Process Complete (no AC found)");
	}
	else if (mortos == 0 && driversStopped == 0) {
		LoaderStatus("Kill Process: %d found, 0 killed (permission?)", tentados);
	}
	else {
		LoaderStatus("Kill Process Complete (%d processes, %d drivers)", mortos, driversStopped);
	}
	
}
static void DesenharLoader()
{
	if (!DirectOverlay::ShowMenu) DirectOverlay::ToggleMenu();

	const ImGuiIO& io = ImGui::GetIO();

	// ---- Position ----
	const bool temPos = (Configs.Survivor.LoaderX >= 0 && Configs.Survivor.LoaderY >= 0);
	if (temPos)
		ImGui::SetNextWindowPos(ImVec2((float)Configs.Survivor.LoaderX, (float)Configs.Survivor.LoaderY), ImGuiCond_Always);
	else
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));

	ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Always);

	// ---- Colors ----
	const ImU32 colBg = IM_COL32(20, 26, 31, 255);
	const ImU32 colBorder = IM_COL32(90, 100, 115, 140);
	const ImU32 colCyan = IM_COL32(0, 191, 255, 255);
	const ImU32 colCyanGlow = IM_COL32(0, 191, 255, 60);
	const ImU32 colRed = IM_COL32(220, 60, 60, 255);
	const ImU32 colGrey = IM_COL32(38, 44, 52, 255);
	const ImU32 colGreyHover = IM_COL32(52, 60, 70, 255);
	const ImU32 colText = IM_COL32(230, 235, 240, 255);
	const ImU32 colTextDim = IM_COL32(150, 160, 175, 255);

	// Alpha 1.00f se 0.80f kiya (20% transparent)
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.078f, 0.102f, 0.122f, 0.80f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
	ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.35f, 0.39f, 0.45f, 0.55f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 16.0f));

	ImGui::Begin("##loader", nullptr,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);

	// ============================================================
// DRAG HANDLE (Sirf top 30px area jahan ATTOCK likha hai)
// ============================================================
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 wpos = ImGui::GetWindowPos();
	const ImVec2 wsize = ImGui::GetWindowSize();

	{
		const ImVec2 pCab = ImGui::GetCursorScreenPos();
		// Width thodi kam ki hai taake KILL PROCESS button ke paas
		// click capture ke liye jagah bache.
		const float  largDrag = 130.0f;

		ImGui::SetCursorScreenPos(pCab);
		ImGui::InvisibleButton("##arrasto_loader", ImVec2(largDrag, 30.0f));

		const bool hoverDrag = ImGui::IsItemHovered();

		// -----------------------------------------------------------------
		// CURSOR CHANGE: jab mouse drag area pe aaye to CROSSHAIR/DRAG
		// cursor set karo. ImGuiMouseCursor_ResizeAll ek 4-arrow icon
		// deta hai jo exactly "move / drag" ka signal deta hai.
		// (Tumhare screenshot jaisa.)
		// -----------------------------------------------------------------
		if (hoverDrag) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
		}

		// -----------------------------------------------------------------
		// ":::" (6-dot) DRAG HANDLE ICON — ATTOCK ke LEFT side pe
		//
		// 2 columns x 3 rows ke chhote dots. Ye visual hint hai ke
		// "yahan se pakad ke drag karo". Text-based `:::` se clean
		// dikhta hai kyunki dots draw karte hain, glyph nahi.
		// -----------------------------------------------------------------
		{
			const ImU32 dotCor = hoverDrag
				? IM_COL32(0, 191, 255, 255)          // cyan jab hover
				: IM_COL32(140, 150, 165, 200);       // halka grey normally

			const float dotR = 1.4f;                // dot radius
			const float gapX = 5.0f;                // columns ka gap
			const float gapY = 5.0f;                // rows ka gap
			const float startX = pCab.x + 6.0f;
			const float startY = pCab.y + 11.0f;

			for (int col = 0; col < 2; ++col) {
				for (int row = 0; row < 3; ++row) {
					const ImVec2 centro(startX + col * gapX, startY + row * gapY);
					dl->AddCircleFilled(centro, dotR, dotCor);
				}
			}
		}

		static bool arrastando = false;
		static bool pendente = false;
		static auto ultimoMov = std::chrono::steady_clock::now();

		if (ImGui::IsItemClicked()) arrastando = true;
		if (arrastando) {
			if (ImGui::IsMouseDown(0)) {
				const ImVec2 d = ImGui::GetIO().MouseDelta;
				if (d.x != 0.0f || d.y != 0.0f) {
					const ImVec2 atual = ImGui::GetWindowPos();
					const ImVec2 novo(atual.x + d.x, atual.y + d.y);
					ImGui::SetWindowPos(novo);
					Configs.Survivor.LoaderX = (int)(novo.x + 0.5f);
					Configs.Survivor.LoaderY = (int)(novo.y + 0.5f);
					pendente = true;
					ultimoMov = std::chrono::steady_clock::now();
				}
			}
			else { arrastando = false; }
		}
		if (pendente && std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - ultimoMov).count() > 700) {
			pendente = false;
			const ImVec2 tela = ImGui::GetIO().DisplaySize;
			if (Configs.Survivor.LoaderX > (int)tela.x - 120) Configs.Survivor.LoaderX = (int)tela.x - 120;
			if (Configs.Survivor.LoaderY > (int)tela.y - 80)  Configs.Survivor.LoaderY = (int)tela.y - 80;
			if (Configs.Survivor.LoaderX < -420) Configs.Survivor.LoaderX = -420;
			if (Configs.Survivor.LoaderY < -30)  Configs.Survivor.LoaderY = -30;
			SaveConfig(L"Default");
		}
		ImGui::SetCursorScreenPos(pCab); // cursor wapas upar le aao
	}

	// ==========================================
         // HEADER: | ATTOCK [ KILL PROCESS ]     
    // ==========================================
	{
		const float curY = ImGui::GetCursorScreenPos().y;

		// YELLOW bar
		dl->AddRectFilled(ImVec2(wpos.x + 38, curY + 4), ImVec2(wpos.x + 41, curY + 28), IM_COL32(255, 215, 0, 255), 1.0f);

		// ATTOCK text
		ImGui::PushFont(DirectOverlay::UiTitleFont);
		// ATTOCK text — 32 se 48 ki shift, kyunki ':::' icon left me hai (6 + 2 col * 5 = ~16px)
       // Aur cyan vertical bar 20→40 shift taake icon ke baad aaye
		ImGui::SetCursorScreenPos(ImVec2(wpos.x + 48, curY));
		ImGui::TextColored(ImVec4(0.90f, 0.92f, 0.95f, 1.0f), "ATTOCK");
		ImGui::PopFont();

		// --- KILL PROCESS BUTTON (Round + Red Edges, Thoda Chhota) ---
       // killW: 130 → 110 (15% reduction: 130 * 0.85 = 110.5)
		const float killW = 110.0f, killH = 26.0f;
		const float xBtn = wpos.x + wsize.x - 20;
		const ImVec2 killPos = ImVec2(xBtn - killW, curY + 2);

		ImGui::SetCursorScreenPos(killPos);

		// RED colors — 10% halka kiya:
		//   Button     : (0.22, 0.06, 0.06) → (0.27, 0.10, 0.10)   (background halka)
		//   Hovered    : (0.45, 0.12, 0.12) → (0.50, 0.16, 0.16)
		//   Active     : (0.60, 0.15, 0.15) → (0.65, 0.19, 0.19)
		//   Text       : (1.0, 0.85, 0.85)  → (1.0, 0.88, 0.88)    (text aur halka)
		//   Border     : (0.90, 0.20, 0.20) → (0.85, 0.25, 0.25)    (border thoda soft)
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.27f, 0.10f, 0.10f, 0.80f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.50f, 0.16f, 0.16f, 1.00f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.19f, 0.19f, 1.00f));
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.88f, 0.88f, 1.00f));
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.85f, 0.25f, 0.25f, 1.00f));

		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);

		ImGui::BeginDisabled(false); // KILL PROCESS hamesha active rahega, chahe gate loop me ho

		if (ImGui::Button("KILL PROCESS", ImVec2(killW, killH))) {
			GSessaoOcupada.store(true); // Loader lock
			std::thread(KillAntiCheatProcessesThread).detach();
		}

		ImGui::EndDisabled();
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(5);

		// Divider line
		dl->AddRectFilled(ImVec2(wpos.x + 16, curY + 38), ImVec2(wpos.x + wsize.x - 16, curY + 39), IM_COL32(60, 70, 85, 120));

		// Cursor ko header ke neeche le aao
		ImGui::SetCursorScreenPos(ImVec2(wpos.x + 20, curY + 46));
	}

	// ============================================================
	// MIDDLE: [ LOAD ]  [ UNLOAD ]  (side-by-side)
	// ============================================================
	{
		const bool driverOk = TargetProcess.driver.DriverVersionOk();
		const bool ocupado = GSessaoOcupada.load();
		const ImVec2 wpos = ImGui::GetWindowPos();
		const ImVec2 wsize = ImGui::GetWindowSize();
		const float btnW = (wsize.x - 40.0f - 12.0f) * 0.5f;
		const float btnH = 52.0f;

		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.2f);
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.42f, 0.46f, 0.52f, 0.9f));
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.94f, 0.97f, 1.0f));

		// ---- LOAD ----
		ImGui::BeginDisabled(ocupado || driverOk);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.149f, 0.176f, 0.208f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.205f, 0.235f, 0.275f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.28f, 0.33f, 1.0f));
		if (ImGui::Button("  v  LOAD", ImVec2(btnW, btnH))) {
		std::thread(CarregarDriverThread).detach();
		}
		ImGui::PopStyleColor(3);
		ImGui::EndDisabled();

		ImGui::SameLine(0.0f, 12.0f);

		// ---- UNLOAD ----
		ImGui::BeginDisabled(ocupado || !driverOk);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.149f, 0.176f, 0.208f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.205f, 0.235f, 0.275f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.28f, 0.33f, 1.0f));
		if (ImGui::Button("  ^  UNLOAD", ImVec2(btnW, btnH))) {
			std::thread(DescarregarDriverThread).detach();
		}
		ImGui::PopStyleColor(3);
		ImGui::EndDisabled();

		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(2);
	}

	ImGui::Dummy(ImVec2(0.0f, 10.0f));

	// ============================================================
// LAUNCH / CANCEL  (big cyan glow button)  FIXED
// ============================================================
	{
		const bool isWaiting = (GFase.load() == FASE_AGUARDANDO);
		const bool driverOk = TargetProcess.driver.DriverVersionOk();
		const ImVec2 wpos = ImGui::GetWindowPos();
		const ImVec2 wsize = ImGui::GetWindowSize();
		const bool launchDisabled = (!driverOk && !isWaiting);
		const float btnW = wsize.x - 40.0f;
		const float btnH = 72.0f;
		const ImVec2 p = ImGui::GetCursorScreenPos();

		// 1. Glow (sirf tab jab waiting na ho) - YELLOW GLOW
		if (!isWaiting && !launchDisabled) {
			for (int i = 6; i > 0; i--) {
				float expand = i * 1.5f;
				dl->AddRectFilled(
					ImVec2(p.x - expand, p.y - expand),
					ImVec2(p.x + btnW + expand, p.y + btnH + expand),
					IM_COL32(255, 215, 0, 8), 10.0f); // YELLOW GLOW
			}
		}

		// 2. Click detect karne ke liye InvisibleButton (SAB SE PEHLE)
		ImGui::SetCursorScreenPos(ImVec2(p.x, p.y));

		// --- NAYA: BEGIN DISABLED LAGAO ---
		ImGui::BeginDisabled(launchDisabled);
		ImGui::InvisibleButton("##bigBtn", ImVec2(btnW, btnH));
		ImGui::EndDisabled();

		const bool clicked = ImGui::IsItemClicked();
		const bool hovered = ImGui::IsItemHovered();

		// 3. Button ka background aur border draw karo
		ImU32 bgCol, borderCol, textCol;

		// --- NAYA: DISABLED CHECK AUR TEXT COLOR ---
		if (launchDisabled) {
			bgCol = IM_COL32(20, 20, 20, 200);       // Dark grey (blur)
			borderCol = IM_COL32(70, 70, 70, 255);       // Grey border
			textCol = IM_COL32(120, 120, 120, 255);    // Grey text
		}
		else {
			bgCol = isWaiting
				? (hovered ? IM_COL32(107, 26, 26, 255) : IM_COL32(77, 20, 20, 255)) // Waiting = Red
				: (hovered ? IM_COL32(38, 32, 10, 255) : IM_COL32(25, 22, 8, 255));   // Normal = Dark Yellowish

			borderCol = isWaiting ? IM_COL32(220, 60, 60, 255) : IM_COL32(255, 215, 0, 255); // YELLOW BORDER
			textCol = IM_COL32(255, 255, 255, 255);    // White text
		}
		// --------------------------------------------

		dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + btnW, p.y + btnH), bgCol, 10.0f);
		dl->AddRect(ImVec2(p.x, p.y), ImVec2(p.x + btnW, p.y + btnH), borderCol, 10.0f, 0, 2.0f);

		// 4. Text ko center karo (sirf EK BAAR)
		const char* label = isWaiting ? "||  CANCEL" : ">>  LAUNCH";  // ? symbol
		ImGui::PushFont(DirectOverlay::UiTitleFont);
		const ImVec2 tsBig = ImGui::CalcTextSize(label);
		const ImVec2 textPos(
			p.x + (btnW - tsBig.x) * 0.5f,
			p.y + (btnH - tsBig.y) * 0.5f
		);
		dl->AddText(DirectOverlay::UiTitleFont, DirectOverlay::UiTitleFont->FontSize,
		textPos, textCol, label);
		ImGui::PopFont();

		// 5. Click par action
		if (clicked) {
			if (isWaiting) {
				GFase = FASE_LOADER;
				GIsRunning = false;
				GSessaoOcupada.store(false); // <-- YEH ADD KARO (Lock free)
				LoaderStatus("Launching Cancelled");
				std::this_thread::sleep_for(std::chrono::milliseconds(300));
			}
			else {
				GFase = FASE_AGUARDANDO;
				GIsRunning = true; // <-- YEH ADD KARO (Loop chalne ke liye)
				GSessaoOcupada.store(false); // <-- YEH ADD KARO (Lock free)
				std::thread(IniciarSessaoThread, !driverOk).detach();
				LOG("[*] loader: Log in thread triggered (driverOk=%d)\n", (int)driverOk);
				LoaderStatus("waiting for TslGame.exe...");
			}
		}
	}

	ImGui::Dummy(ImVec2(0.0f, 12.0f));

	// ============================================================
	// STATUS BAR
	// ============================================================
	{
		const ImVec2 wpos = ImGui::GetWindowPos();
		const ImVec2 wsize = ImGui::GetWindowSize();
		const float curY = ImGui::GetCursorScreenPos().y;

		// Divider above status
		dl->AddRectFilled(ImVec2(wpos.x + 16, curY), ImVec2(wpos.x + wsize.x - 16, curY + 1), IM_COL32(60, 70, 85, 100));

		std::string st = LoaderStatusTexto();
		if (st.empty()) st = "READY";

		ImVec4 dotColor = ImVec4(0.30f, 0.90f, 0.30f, 1.0f); 
		if (st.find("erro") != std::string::npos ||
			st.find("falha") != std::string::npos ||
			st.find("ativo") != std::string::npos ||
			st.find("DETECTED") != std::string::npos ||
			st.find("Blocked") != std::string::npos ||    
			st.find("Watching") != std::string::npos)     
			dotColor = ImVec4(0.95f, 0.30f, 0.30f, 1.0f); 
		else if (st.find("...") != std::string::npos)
			dotColor = ImVec4(1.00f, 0.85f, 0.20f, 1.0f); 

		ImGui::SetCursorScreenPos(ImVec2(wpos.x + 24, curY + 14));
		dl->AddCircleFilled(ImVec2(wpos.x + 30, curY + 26), 6.0f, IM_COL32((int)(dotColor.x * 255), (int)(dotColor.y * 255), (int)(dotColor.z * 255), 255));

		ImGui::SetCursorScreenPos(ImVec2(wpos.x + 46, curY + 16));
		ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.68f, 1.0f), "STATUS:");
		ImGui::SameLine(0.0f, 6.0f);
		ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "%s", st.c_str());
	}

	ImGui::End();
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(3);
}

// ===========================================================================
// CARTES  o layout do Astral (pedido do He, 19/10/2026).
//
// COMO FUNCIONA (e por que desta forma):
//
// O menu de referencia desenha cada grupo dentro de um cartao cuja ALTURA
// depende do conteudo. O nosso nao pode medir antes de desenhar, e desenhar o
// fundo depois cobriria os controles. A saida e' usar a altura do FRAME
// ANTERIOR: o cartao e' pintado no inicio da secao com a altura que sobrou da
// ultima vez, e o conteudo e' desenhado por cima. Um frame de atraso e'
// invisivel a 60+ fps e nao custa nada.
//
// A altura e' medida ao ABRIR a secao seguinte (ou ao fechar a aba): a
// diferenca entre o Y atual e o Y onde o cartao comecou e', exatamente, o que
// o conteudo ocupou.
// ===========================================================================
static std::map<int, float> g_cartaoAltura;   // id -> altura medida
static int   g_cartaoId   = -1;               // cartao aberto (-1 = nenhum)
static float g_cartaoY0   = 0.0f;             // Y onde ele comecou
static int   g_cartaoSeq  = 0;                // contador dentro da aba

// hash FNV-1a do titulo  o id tem que ser estavel entre frames e nao depender
// da ordem em que as secoes aparecem (um "if" que esconde uma secao deslocava
// todo o contador sequencial e os cartoes perdiam a altura guardada)
static int CartaoId(const char* s)
{
	unsigned h = 2166136261u;
	for (; s && *s; ++s) { h ^= (unsigned char)*s; h *= 16777619u; }
	return (int)(h & 0x7fffffffu);
}
// fecha o cartao aberto, medindo a altura ate' o Y informado
static void CartaoFechar(float yAtual)
{
	if (g_cartaoId < 0) return;
	const float h = yAtual - g_cartaoY0;
	if (h > 8.0f) g_cartaoAltura[g_cartaoId] = h + 4.0f;
	g_cartaoId = -1;
}

static void Seccao(const char* titulo)
{
	const ImVec2 p   = ImGui::GetCursorScreenPos();
	const float  w   = ImGui::GetContentRegionAvail().x;
	ImDrawList*  dl  = ImGui::GetWindowDrawList();

	CartaoFechar(p.y);

	g_cartaoId = CartaoId(titulo);   // id pelo texto do titulo (estavel entre frames)
	g_cartaoY0 = p.y;

	// fundo do cartao com a altura do frame anterior (0 na primeira renderizacao)
	const auto it = g_cartaoAltura.find(g_cartaoId);
	const float h = (it != g_cartaoAltura.end()) ? it->second : 0.0f;
	if (h > 12.0f) {
		const ImVec2 a(p.x - 10.0f, p.y);
		const ImVec2 b(p.x + w + 10.0f, p.y + h);
		dl->AddRectFilled(a, b, IM_COL32(28, 33, 47, 255), 10.0f);
		dl->AddRect(a, b, IM_COL32(255, 255, 255, 14), 10.0f);
		// fio de acento no topo do cartao (o "relevo" do Astral)
		dl->AddRectFilledMultiColor(ImVec2(a.x + 10.0f, a.y), ImVec2(b.x - 10.0f, a.y + 1.0f),
		                            IM_COL32(139, 92, 246, 0), IM_COL32(139, 92, 246, 170),
		                            IM_COL32(139, 92, 246, 170), IM_COL32(139, 92, 246, 0));
	}

	// titulo centralizado, dentro do cartao
	ImGui::PushFont(DirectOverlay::UiTitleFont);
	const ImVec2 medida = ImGui::CalcTextSize(titulo);
	ImGui::SetCursorScreenPos(ImVec2(p.x + (w - medida.x) * 0.5f, p.y + 8.0f));
	ImGui::TextColored(ImVec4(0.90f, 0.93f, 0.98f, 1.00f), L("%s"), titulo);
	ImGui::PopFont();

	// conteudo com respiro lateral e abaixo do titulo
	ImGui::SetCursorScreenPos(ImVec2(p.x + 6.0f, p.y + 34.0f));
}

static void CheckboxPair(const char* rotulo1, bool* v1, const char* rotulo2, bool* v2)
{
	const float total  = ImGui::GetContentRegionMax().x - ImGui::GetStyle().WindowPadding.x * 2.0f;
	// O GAP entra na conta das colunas  senao a segunda comeca exatamente na
	// borda do trilho da primeira e o rotulo dela encosta no toggle vizinho
	// (medido no print: "ESP Box [toggle]Skeleton").
	const float gap    = 16.0f;
	const float coluna = (total - gap) * 0.5f;

	ImGui::CheckboxW(rotulo1, v1, coluna);
	ImGui::SameLine(0.0f, gap);
	ImGui::CheckboxW(rotulo2, v2, coluna);
}

// ===========================================================================
// SubirDriverKevent - UNICO caminho de carga do transporte (full KEVENT).
//
// POR QUE VIROU FUNCAO: a mesma sequencia (limpa sessao anterior -> mapeia ->
// confirma versao) estava repetida em TRES lugares do Main (MAP NOW, thread de
// init e a sessao do loader), cada um com o seu texto de log. Agora e' uma so',
// e o IOCTL saiu de vez: nao existe mais `LoadDriverInternal` nem device.
//
// O `Cleanup()` antes e' proposital: o kdmapper nao desmapeia, entao uma carga
// anterior pode estar viva. Limpar antes garante que os handles da sessao velha
// saem (o handshake de parada do KEVENT fecha a thread do servidor).
// ===========================================================================
// ---------------------------------------------------------------------------
// DIAGNOSTICO DE EXPOSICAO  "o que o AC ve?"
//
// Roda depois que o canal esta' aberto: o driver devolve numeros, e o log
// traduz. Cada linha responde uma pergunta que um AC de kernel faria:
//
//   imagem em PsLoadedModuleList?   se NAO, a imagem e' invisivel para quem
//                                   enumera modulos  e esse e' o primeiro
//                                   sinal de mapeamento manual
//   start da nossa thread em modulo? se NAO, a thread de sistema aponta para
//                                   memoria que nenhum driver reivindica
//   quantas threads ja' estao assim? mede ruido de sistema, para a nossa
//                                   assinatura ser comparada com o normal
// ---------------------------------------------------------------------------
static void LogarExposicao()
{
	uint64_t v[16] = { 0 };

	// O cliente informa o SizeOfImage porque o driver NAO consegue ler o proprio:
	// o mapper destruiu os headers (destroyHeader=true).
	//
	// 28/10/2026: a falha agora diz ONDE  antes qualquer causa virava
	// "driver sem KMP_OP_EXPOSE?", inclusive a que a manutencao produziu:
	// round trip morto, nao op ausente.
	uint32_t porque = 0;
	if (!TargetProcess.driver.DiagnosticoExposicao(v, (uint64_t)DriverKeventImageSize(), &porque)) {
		switch (porque) {
		case 1:	LOG("[expose] channel not ready\n"); break;
		case 2:	LOG("[expose] round trip failed; see [kmb] line above (kernel server stopped/lagging?)\n"); break;
		case 3:	LOG("[expose]Driver rejected the op (build without KMP_OP_EXPOSE?)\n"); break;
		default:	LOG("[expose] backend its not KEVENT\n"); break;
		}
		return;
	}

	LOG("\n===== EXPOSURE DIAGNOSIS =====\n");
	LOG("[expose] image : base=0x%llX size=0x%llX  |  em PsLoadedModuleList: %s\n",
	    (unsigned long long)v[0], (unsigned long long)v[1],
	    v[2] ? "SIM (aparece como modulo)" : "NAO (mapeada a mao)");

	// -----------------------------------------------------------------------
	// v[3] == 0 significa: NENHUMA thread do sistema aponta para a nossa imagem.
	//
	// Isso e' o resultado BOM desde a troca para work item (o servidor roda numa
	// thread de worker do sistema, e o StartAddress dela e' do ntoskrnl). A
	// leitura anterior tratava 0 como "falhou" e imprimia "FORA de qualquer
	// modulo <== assinatura de injecao"  ou seja, dava veredito de problema
	// exatamente quando nao havia problema nenhum.
	//
	//	 v[3] = endereco encontrado no ETHREAD (0 = nenhum)
	//	 v[5] = a troca do StartAddress foi aplicada (modo patch, desligado)
	//	 v[6] = o endereco de v[3] cai dentro de algum modulo?
	// -----------------------------------------------------------------------
	const bool temThreadNossa = (v[3] != 0);

	if (!temThreadNossa) {
		LOG("[expose] server thread: no thread points to our image (WORK ITEM)\n");
	} else {
		LOG("[expose] thread server : 0x%llX\n", (unsigned long long)v[3]);
		LOG("[expose] start points to : %s\n",
		    v[6] ? "dentro de um modulo (legitimo a olho nu)" : "FORA de qualquer modulo  <== assinatura de injecao");
	}

	if (v[2] == 0 && !temThreadNossa) {
		LOG("[expose] VERDICT: image invisible for module enumeration, and no threads of our own.\n");
	} else if (v[2] == 0 && temThreadNossa && v[6] == 0) {
		LOG("[expose] VERDICT: hidden image, but there is a thread pointing to it.\n");
	} else if (v[2] != 0) {
		LOG("[expose] VERDICT: the image appears as a nodule - there is no concealment.\n");
	} else {
		LOG("[expose] VERDICT: thread in a legitimate module; nothing to note.\n");
	}
	LOG("====================================\n\n");
}

// Keyread de kernel (driver v4): o provider entra no HotkeyHook como fonte
// primria para ALT/SHIFT (OR com o hook LL). Ele consulta o estado VIVO a
// cada chamada  driver v3, kbdclass que falhou ou canal fechado deixam
// `KeyreadKernelOk()` false e o hook volta a mandar sozinho.
static bool     KmpKeyreadOk()   { return TargetProcess.driver.KeyreadKernelOk(); }
static uint32_t KmpKeyreadMask() { return TargetProcess.driver.KeyMaskKernel(); }

static bool SubirDriverKevent(const char* origem)
{
	TargetProcess.driver.Cleanup();
	HotkeyHook::SetKernelProvider(nullptr, nullptr);   // canal antigo fechou

	if (!TargetProcess.driver.InitializeKevent(&LoadDriverKeventInternal)) {
		LOG("[-] %s: channel KEVENT nao open (see [kdm] and [kevent])\n", origem);
		return false;
	}
	if (!TargetProcess.driver.DriverVersionOk()) {
		LOG("[-] %s: transportation provided a response 0x%08X (esperado 0x%08X)\n",
		    origem, TargetProcess.driver.driverVersion, (unsigned)KMP_VERSION);
		return false;
	}
	LOG("[+] %s: KEVENT active (0x%08X) | keyread_kernel=%d\n", origem,
	    TargetProcess.driver.driverVersion, (int)KmpKeyreadOk());

	// -----------------------------------------------------------------
	// REGISTRO DE PAGINAS NA HORA DO MAPA (hardening-p4).
	//
	// Antes o registro ia para disco so' no END com jogo aberto. Toda saida
	// que pula o END  crash, taskkill /f, queda de luz, liberacao falha 
	// deixava ~19 KB de codigo executavel residentes sem que ninguem
	// registrasse a base, e MmFreeIndependentPages em base errada ou achada
	// apos os fatos nao e' limpeza: e' bugcheck.
	//
	// Gravando no mapa e apagando somente depois do free bem-sucedido,
	// TODA saida possivel deixa um registro valido e o proximo start com o
	// jogo fechado libera (TentarLiberarPaginasResiduas). O bootId dentro do
	// registro ja' protege contra reboot entre o mapa e a liberacao.
	// -----------------------------------------------------------------
	{
		const uint64_t b = TargetProcess.driver.KeventImageBase();
		uint64_t s = (uint64_t)DriverKeventAllocSize();
		if (s == 0) {
			const uint64_t si = (uint64_t)DriverKeventImageSize();
			s = (si > 0x1000) ? (si - 0x1000) : si;   // desconta SizeOfHeaders
		}
		if (b != 0 && s != 0) {
			GravarPaginasResiduas(b, s);
		} else {
			LOG("[!] %s: session with no recordable base/size (b=0x%llX s=0x%llX) – no automatic future release\n",
			    origem, (unsigned long long)b, (unsigned long long)s);
		}
	}

	HotkeyHook::SetKernelProvider(KmpKeyreadOk, KmpKeyreadMask);
	LogarExposicao();   // o retrato de exposicao sai a cada carga
	return true;
}

void RenderFrame() {
	// =====================================================================
	// APRESENTACAO FIXA DO ESP (transplante do modulo ESP da build 28/10).
	//
	// Familia inter, contraste outline, tamanhos nativos 12-15  independente
	// das preferencias salvas em Default.json (mesma regra da build de origem:
	// a melhora visual do ESP nao depende de configuracao antiga).
	// =====================================================================
	Configs.Survivor.EspFontFamily = 1;
	Configs.Survivor.EspTextStyle = 0;
	EspTypography::active_family = static_cast<int>(EspTypography::Family::inter);
	EspTypography::active_style = EspTypography::Contrast::outline;
	EspTypography::background_opacity = (std::clamp)(Configs.Survivor.TextBgOpacity, 0, 255);
	EspTypography::secondary_size = EspTypography::clamp_size(Configs.Survivor.SecondaryTextSize);
	Configs.Survivor.TextBackground = false;
	GEspFont = EspTypography::font_for(15);

	// =====================================================================
	// END  FIM DE SESSAO (nao fim de processo).
	//
	// O WndProc levanta `DirectOverlay::PedidoFimSessao` (ele nao pode chamar
	// PararSessao: o unmap tem sleeps e travaria a fila de mensagens). Aqui:
	//   - com sessao viva -> encerra a sessao e volta pro LOADER
	//   - no LOADER      -> sai do processo (panic de sempre)
	// =====================================================================
	if (DirectOverlay::PedidoFimSessao.exchange(false)) {
		if (GFase.load() != FASE_LOADER) {
			PararSessao();
		} else {
			GIsRunning = false;
			PostQuitMessage(0);
		}
	}

	// =====================================================================
	// LOADER PRIMEIRO  o menu/ESP so' existem com a sessao ativa.
	// =====================================================================
	if (GFase.load() != FASE_RODANDO) {
		DesenharLoader();
		return;
	}

	// =====================================================================
	// MODO DE MEDICAO POR VARIAVEL DE AMBIENTE  para nao precisar clicar.
	//
	// O painel da aba DEBUG exige abrir o menu, achar a aba e marcar um
	// checkbox. Isso e'' atrito suficiente para a medicao nunca acontecer  e
	// sem medicao a auditoria nao conclui.
	//
	//     set MTMACR0_PERF=1
	//     mtmacr0.exe
	//
	// Liga os contadores e joga o resumo no LOG que o projeto JA'' tem
	// (nao cria arquivo nenhum: o `LOG` grava no logs.txt existente, e a linha
	// sai so'' quando esta variavel esta'' definida).
	//
	// A SAIDA E'' A MESMA DO PAINEL, no mesmo formato  quem le'' o log ve'' o
	// mesmo que quem olha a tela.
	// =====================================================================
	{
		static const bool perfAuto = []() {
			// -----------------------------------------------------------------
			// DOIS CAMINHOS PARA LIGAR A MEDICAO, e o do ARQUIVO e' o bom.
			//
			// 1) MTMACR0_PERF=1  so' vale no terminal onde foi definido.
			//    `$env:` e' da SESSAO: abrir um PowerShell novo apaga.
			//    Foi exatamente o que aconteceu numa partida inteira: o usuario
			//    jogou, o log encheu de [STATE]/[KEY]/[AIM], e ZERO [PERF].
			//
			// 2) ARQUIVO MARCADOR  sobrevive a terminal novo, a reboot, a
			//    tudo. Para ligar:
			//        %LOCALAPPDATA%\RenderCache\perf.flag
			//    Para desligar: apaga o arquivo.
			//
			// O arquivo ganha do env var porque nao depende de como o processo
			// foi lancado  e a medicao ja' custou rodadas demais por causa
			// disso.
			// -----------------------------------------------------------------
			char v[8] = { 0 };
			const DWORD n = GetEnvironmentVariableA("MTMACR0_PERF", v, sizeof(v));
			if (n > 0 && v[0] == '1') { Perf::Ligar(true); return true; }

			char* base = nullptr;
			size_t tamBase = 0;
			if (_dupenv_s(&base, &tamBase, "LOCALAPPDATA") == 0 && base) {
				char caminho[MAX_PATH] = { 0 };
				// "%s" resolve o LOCALAPPDATA; o resto vem do LIT() para o nome
				// da pasta nao ficar no .rdata.
				snprintf(caminho, sizeof(caminho), "%s%s", base, LIT("\\RenderCache\\perf.flag"));
				free(base);
				if (GetFileAttributesA(caminho) != INVALID_FILE_ATTRIBUTES) {
					Perf::Ligar(true);
					return true;
				}
			}
			return false;
		}();

		if (perfAuto) {
			static uint64_t aFrames = 0, aBuild = 0, aPres = 0, aTicks = 0, aMemNs = 0;
			static uint64_t aReads = 0, aIoNames = 0, aAdd = 0, aCalc = 0, aSnapNs = 0, aSnapN = 0;
			static auto aT = std::chrono::steady_clock::now();

			const auto aAgora = std::chrono::steady_clock::now();
			const double aDt = std::chrono::duration<double>(aAgora - aT).count();

			if (aDt >= 2.0) {   // 2 s: mais estavel que 1 s, e menos linhas no log
				const uint64_t f  = Perf::C.renderFrames.load(std::memory_order_relaxed);
				const uint64_t b  = Perf::C.buildNs.load(std::memory_order_relaxed);
				const uint64_t pr = Perf::C.presentNs.load(std::memory_order_relaxed);
				const uint64_t t  = Perf::C.memTicks.load(std::memory_order_relaxed);
				const uint64_t mn = Perf::C.memNs.load(std::memory_order_relaxed);
				const uint64_t rd = Perf::C.singleReads.load(std::memory_order_relaxed);
				const uint64_t io = Perf::C.ioctlNs.load(std::memory_order_relaxed);
				const uint64_t ad = Perf::C.addText.load(std::memory_order_relaxed);
				const uint64_t ct = Perf::C.calcText.load(std::memory_order_relaxed);
				const uint64_t sn = Perf::C.snapshotNs.load(std::memory_order_relaxed);
				const uint64_t snn= Perf::C.snapshotN.load(std::memory_order_relaxed);

				const double dF = (double)(f - aFrames), dR = (double)(rd - aReads);
				const double dM = (double)(t - aTicks),  dI = (double)(io - aIoNames);
				const double dSn= (double)(snn - aSnapN);

				LOG("[PERF] render=%.0f fps build=%.2fms present=%.2fms P95=%.2f P99=%.2f | "
					"mem=%.0f t/s %.2fms/tick | IOCTL=%.0f/s %.1fus cada %.0f%% de nucleo | "
					"AddText=%.0f/frame CalcText=%.0f/frame | snapshot=%.3fms (%llu) | oclusoes=%llu\n",
					dF / aDt,
					dF > 0 ? (double)(b - aBuild) / dF / 1e6 : 0.0,
					dF > 0 ? (double)(pr - aPres) / dF / 1e6 : 0.0,
					Perf::Percentil(Perf::histBuild, 0.95), Perf::Percentil(Perf::histBuild, 0.99),
					dM / aDt, dM > 0 ? (double)(mn - aMemNs) / dM / 1e6 : 0.0,
					dR / aDt, dR > 0 ? (double)dI / dR / 1000.0 : 0.0,
					dR > 0 ? 100.0 * (double)dI / (aDt * 1e9) : 0.0,
					dF > 0 ? (double)(ad - aAdd) / dF : 0.0,
					dF > 0 ? (double)(ct - aCalc) / dF : 0.0,
					dSn > 0 ? (double)(sn - aSnapNs) / dSn / 1e6 : 0.0, (unsigned long long)dSn,
					(unsigned long long)Perf::C.oclusoes.load(std::memory_order_relaxed));

				// CROSS-CHECK: `snapshotN` conta um por RenderFrame, do mesmo jeito
				// que `renderFrames`. Se os dois discordarem, UM dos contadores esta'
				// quebrado  e isso ja' aconteceu uma vez (o `RegistrarBuild` que
				// ninguem chamava deixou `render` em 0 e todo o resto sem sentido).
				// Com os dois lado a lado, um erro desses se denuncia sozinho.
				LOG("[PERF]   cross-check: frames=%llu por render e %llu por snapshot (%s)\n",
					(unsigned long long)(f - aFrames), (unsigned long long)dSn,
					(dSn > 0 && (f - aFrames) > 0 && dSn > (f - aFrames) * 2) ? "DIVERGEM - contador quebrado" : "ok");

				// A divisao do tick e'' a informacao que ORDENA o ataque.
				{
					const uint64_t total = (mn - aMemNs);
					if (total > 0) {
						LOG("[PERF]   tick: Cache=%.0f%% Players=%.0f%% Weapons=%.0f%% "
							"Grenades=%.0f%% Vehicles=%.0f%% Loot=%.0f%% ViewMatrix=%.0f%% "
							"Aimbot=%.0f%% publish=%.0f%%\n",
							100.0 * (double)Perf::C.nsCache.load(std::memory_order_relaxed)      / (double)total,
							100.0 * (double)Perf::C.nsPlayers.load(std::memory_order_relaxed)    / (double)total,
							100.0 * (double)Perf::C.nsWeapons.load(std::memory_order_relaxed)    / (double)total,
							100.0 * (double)Perf::C.nsGrenades.load(std::memory_order_relaxed)   / (double)total,
							100.0 * (double)Perf::C.nsVehicles.load(std::memory_order_relaxed)   / (double)total,
							100.0 * (double)Perf::C.nsLoot.load(std::memory_order_relaxed)       / (double)total,
							100.0 * (double)Perf::C.nsViewMatrix.load(std::memory_order_relaxed) / (double)total,
							100.0 * (double)Perf::C.nsAimbot.load(std::memory_order_relaxed)     / (double)total,
							100.0 * (double)Perf::C.nsPublish.load(std::memory_order_relaxed)    / (double)total);
					}
				}
				{
					std::lock_guard<std::mutex> lk(GDataMutex);
					LOG("[PERF]   listas: actors=%zu grenade=%zu vehicle=%zu loot=%zu\n",
						GSharedData.Actors.size(), GSharedData.GrenadeActors.size(),
						GSharedData.VehicleActors.size(), GSharedData.LootActors.size());
				}

				aFrames = f; aBuild = b; aPres = pr; aTicks = t; aMemNs = mn;
				aReads = rd; aIoNames = io; aAdd = ad; aCalc = ct; aSnapNs = sn; aSnapN = snn;
				aT = aAgora;
			}
		}
	}

	// Aba WORLD: devolve ao catalogo o que ja' foi visto em sessoes passadas
	// (uma vez), e depois reconcilia catalogo -> config a cada frame.
	SeedLootCatalogFromConfig();
	SyncLootCatalogIntoConfig();

	static bool homeKeyWasPressed = false;
	bool homeKeyIsPressed = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
	if (homeKeyIsPressed && !homeKeyWasPressed) {
		ShowMenu = !ShowMenu;
		if (ShowMenu != DirectOverlay::ShowMenu) DirectOverlay::ToggleMenu();
	}
	homeKeyWasPressed = homeKeyIsPressed;

	// -----------------------------------------------------------------------
	// DIAGNOSTICO [KEY] — 1x/s, atras de `Configs.Survivor.DebugLogs`.
	//
	// PROVA a correcao da leitura de tecla SEM abrir o jogo: imprime o estado
	// do HOOK e o retorno CRU do `GetAsyncKeyState` lado a lado. Segurando o
	// botao direito, o esperado e' `RBUTTON=down` com
	// `GetAsyncKeyState(RBUTTON)=0x0000` — o hook recebe a tecla exatamente
	// onde o polling nao recebe.
	//
	// Fica AQUI (e nao no `Aimbot::Tick`) de proposito: o Tick so' roda depois
	// que o `TslGame.exe` foi encontrado, e este teste tem que funcionar com o
	// jogo FECHADO. O `RenderFrame` roda todo frame desde o primeiro segundo do
	// overlay. O estado lido e' o MESMO que o aimbot le
	// (`HotkeyHook::g_downMask`, atomico — quem escreve e' o hook, na thread
	// principal; o aimbot le da thread de memoria).
	//
	// `evt=` e' o contador de eventos do hook (mouse + teclado, cumulativo).
	// Se ele sobe, o hook esta' VIVO recebendo input — e' o que distingue
	// "o hook nao recebe nada" de "recebo, mas a tecla mapeada esta' errada".
	//
	// `DebugLogs` ja' existia (aba DEBUG, default OFF): nenhum campo novo de
	// config foi criado. 1x/s, nunca por frame.
	// -----------------------------------------------------------------------
	// -----------------------------------------------------------------------
	// MUDANCA DE TECLA  logado NA HORA, nao por amostragem.
	//
	// O diagnostico do aimbot (`[AIM] aguardando hotkey ... | mask=..`) roda a
	// cada 1,5 s e MOSTRA o mask do instante. Isso nao serve para provar que uma
	// tecla e' vista: um clique dura milissegundos e cai entre as amostras  e
	// "nao vi" fica indistinguivel de "nao apertou".
	//
	// Aqui o log sai quando o mask MUDA. Toda tecla que o hook registrar vira
	// uma linha, com o bit exato.
	//
	// Custo: so' muda quando o usuario aperta/solta botao ou tecla mapeada.
	// Movimento do mouse NAO mexe no mask (o MouseProc ignora WM_MOUSEMOVE de
	// proposito), entao isto nao vira spam  e por isso NAO fica atras de
	// DebugLogs.
	//
	// Nao logar DENTRO do MouseProc: o callback de hook de baixo nivel tem
	// deadline (LowLevelHooksTimeout, 300 ms por default) e BaseLog faz I/O de
	// arquivo. Estourar o deadline faz o Windows REMOVER o hook em silencio.
	// Aqui e' a thread de render, fora desse caminho.
	//
	// Bits: ALT=0x01 RBUTTON=0x02 SHIFT=0x04 MOUSE5=0x08 MOUSE4=0x10 LBUTTON=0x20
	// -----------------------------------------------------------------------
	{
		static uint32_t ultimoMask = 0;
		static bool primeiroMask = true;
		const uint32_t maskAgora = HotkeyHook::DownMask();
if (Configs.Survivor.DebugLogs && (primeiroMask || maskAgora != ultimoMask)) {
			primeiroMask = false;
			ultimoMask = maskAgora;
			LOG("[KEY] mask=0x%02X | RBUTTON=%s LBUTTON=%s ALT=%s SHIFT=%s | evt=%u\n",
				(unsigned)maskAgora,
				HotkeyHook::StateWord(HotkeyHook::KEY_RBUTTON),
				HotkeyHook::StateWord(HotkeyHook::KEY_LBUTTON),
				HotkeyHook::StateWord(HotkeyHook::KEY_LEFT_ALT),
				HotkeyHook::StateWord(HotkeyHook::KEY_SHIFT),
				(unsigned)HotkeyHook::EventCount());
		}
	}

	if (Configs.Survivor.DebugLogs) {
		static auto lastKeyLog = std::chrono::steady_clock::now();
		const auto nowKeyLog = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::milliseconds>(nowKeyLog - lastKeyLog).count() > 1000) {
			lastKeyLog = nowKeyLog;
			// Retorno CRU do polling: sem mascara, sem `& 0x8000` — o valor que
			// o GetAsyncKeyState devolveu, bit a bit.
			const SHORT rawRButton = GetAsyncKeyState(VK_RBUTTON);
			LOG("[KEY] hook: ativo=%d |= 0x%02X | RBUTTON=%s LALT=%s SHIFT=%s | GetAsyncKeyState(RBUTTON)=0x%04X | evt=%u\n",
				(int)HotkeyHook::Installed(),
				(unsigned)HotkeyHook::DownMask(),
				HotkeyHook::StateWord(HotkeyHook::KEY_RBUTTON),
				HotkeyHook::StateWord(HotkeyHook::KEY_LEFT_ALT),
				HotkeyHook::StateWord(HotkeyHook::KEY_SHIFT),
				(unsigned)(unsigned short)rawRButton,
				(unsigned)HotkeyHook::EventCount());
		}
	}

	// Dynamic Topmost otimizado: só muda o Z-order quando a janela em foco muda
	HWND foreground = GetForegroundWindow();
	DWORD foregroundPid = 0;
	GetWindowThreadProcessId(foreground, &foregroundPid);

	bool isGameFocused = (GInitOk.load(std::memory_order_relaxed) && foregroundPid == Memory::current_process.PID);
	bool isOverlayFocused = (foreground == DirectOverlay::Overlay);
	bool isLoading = !GInitOk.load(std::memory_order_relaxed);

	static HWND lastForeground = NULL;
	if (foreground != lastForeground) {
		if (isGameFocused || isOverlayFocused || isLoading) {
			SetWindowPos(DirectOverlay::Overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		} else {
			SetWindowPos(DirectOverlay::Overlay, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
		lastForeground = foreground;
	}

	if (TestMouseEnabled) {
		// -------------------------------------------------------------------
		// `HotkeyHook::IsDown`, NAO `GetAsyncKeyState`.
		//
		// Isto estava com `GetAsyncKeyState(VK_MENU) & 0x8000` e o teste do ALT
		// simplesmente NAO DISPARAVA. O motivo ja' estava documentado em DOIS
		// lugares deste projeto e ninguem tinha ligado os pontos:
		//
		//   DirectOverlay.h: "o overlay e' WS_EX_NOACTIVATE ... a tecla nao
		//   chega num processo sem foco"
		//
		//   HotkeyHook.h (linha 176): "Neste ambiente ele devolve 0, entao quem
		//   manda e' o hook."
		//
		// Ou seja: `GetAsyncKeyState` devolve 0 NESTE processo, por design do
		// Windows (sem foco + sem nivel de integridade privilegiado). Foi por
		// isso que o menu usa `RegisterHotKey` e o aimbot usa o hook de baixo
		// nivel. Este teste ficou pra tras.
		//
		// `IsDown` le' o ESTADO DO HOOK e so' cai no polling como rede de
		// seguranca  entao funciona com o jogo em foco, que e' exatamente
		// quando o teste precisa funcionar.
		//
		// (KEY_LEFT_ALT e' VK_LMENU; o hook mapeia VK_MENU E VK_LMENU para o
		// mesmo indice  ver o switch em HotkeyHook.h.)
		// -------------------------------------------------------------------
		bool altDown = HotkeyHook::IsDown(HotkeyHook::KEY_LEFT_ALT);
		if (altDown && !TestMouseRunning.load(std::memory_order_relaxed)) {
			TestMouseRunning.store(true, std::memory_order_relaxed);
			if (!TestMouseThread.joinable()) {
				TestMouseThread = std::thread([] { 
					TargetProcess.driver.TestRandomMouseMoveLoop(TestMouseRunning); 
				});
			}
		} else if (!altDown && TestMouseRunning.load(std::memory_order_relaxed)) {
			TestMouseRunning.store(false, std::memory_order_relaxed);
			if (TestMouseThread.joinable()) {
				TestMouseThread.join();
			}
		}
	} else if (!TestMouseEnabled && TestMouseRunning.load(std::memory_order_relaxed)) {
		TestMouseRunning.store(false, std::memory_order_relaxed);
		if (TestMouseThread.joinable()) {
			TestMouseThread.join();
		}
	}

	SharedData frameData;
	{
		// INSTRUMENTADO: e' aqui que se paga a copia da lista de atores por
		// frame. O `snapshotNs` divide esse custo pelo numero de frames e diz
		// se vale trocar por um `shared_ptr<const SharedData>` (copia O(1)).
		Perf::Escopo eSnap(Perf::C.snapshotNs);
		std::lock_guard<std::mutex> lock(GDataMutex);
		frameData = GSharedData;
		Perf::Contar(Perf::C.snapshotN);
	}

	// -----------------------------------------------------------------
	// A CAMERA VEM DO SNAPSHOT  NAO DE UMA LEITURA NOVA AQUI.
	//
	// Era `Camera::GetCurrentView(CameraManagerAddr)` neste ponto: 3
	// `DeviceIoControl` POR FRAME, na THREAD DE RENDER. Dois problemas de uma
	// vez:
	//   1. o custo entra no orcamento do frame  o pior lugar para uma ida ao
	//      kernel, porque bloqueia o Present;
	//   2. o dado JA' vinha pronto: a MemoryLoop publica `CameraCache` no
	//      `GSharedData` no mesmo tick (Engine::RefreshViewMatrix).
	//
	// A defasagem maxima passa a ser 1 tick de memoria (~7 ms a 144 Hz), sem
	// efeito no desenho.
	// -----------------------------------------------------------------

	// Abre o menu na inicialização para o usuário poder configurar livremente
	static bool firstOpen = true;
	if (firstOpen) {
		ShowMenu = true;
		if (!DirectOverlay::ShowMenu) DirectOverlay::ToggleMenu();
		firstOpen = false;
	}

	// -----------------------------------------------------------------------
	// STREAMPROOF + MODO CAPTURA  o unico ponto que aplica a escolha do
	// usuario na janela.
	//
	// As chamadas sao baratas de proposito: `ApplyStreamProof` guarda o ultimo
	// valor e so' chama o sistema quando ele MUDA; `AplicarModoCaptura` e um
	// atomic compare-and-store. Por isso cabem aqui no frame, sem custo.
	//
	// Captura LIGADA tem prioridade sobre Streamproof: sem a janela visivel
	// para o DWM o EpicPen nao captura nada  os dois ligos ao mesmo tempo
	// se anulavam silenciosamente, e o que se anula e' a captura (o caso que
	// importa: quem liga captura quer ver o ESP no print).
	// -----------------------------------------------------------------------
	DirectOverlay::AplicarModoCaptura(Configs.Survivor.CaptureMode);
	DirectOverlay::ApplyStreamProof(Configs.Survivor.StreamProof && !Configs.Survivor.CaptureMode);

	// Qualidade do contorno do texto do ESP (9 = Ultra / 4 = Rapida).
	// Atribuicao idempotente por frame  nao ha custo em repetir o mesmo valor
	// (GEspStrokePasses e' lido por OSImGui::StrokeText, mesma thread).
	GEspStrokePasses = (Configs.Survivor.EspTextQuality >= 8) ? 8 : 4;

	if (ShowMenu) {
		ImGui::SetNextWindowSize(ImVec2(760, 700), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);

		// -------------------------------------------------------------------
		// CHROME DO MENU - layout do Neverlose
		//
		// Estrutura (a mesma da referencia: panel_side / panel_top / panel_body):
		//
		//   janela (panel_body)  ->  [ sidebar (panel_side) | cabecalho + corpo ]
		//
		// A barra de titulo do ImGui foi desligada (NoTitleBar) e o cabecalho e'
		// desenhado a' mao. O ARRASTO da janela, que era a unica coisa que a
		// barra de titulo dava de graca, e' feito no InvisibleButton do
		// cabecalho (padrao GetWindowPos + io.MouseDelta).
		//
		// Os fundos dos paineis entram com alpha 1.0 (ver ApplyProfessionalStyle:
		// com LWA_COLORKEY o alpha NAO e' transparencia, so' escurece).
		// -------------------------------------------------------------------
		const float kSidebarLargura = 172.0f;

		const ImVec4 acento     = ImVec4(0.545f, 0.361f, 0.965f, 1.00f);  // roxo #8B5CF6 (menu/loader)
		const ImVec4 acentoClr  = ImVec4(0.62f, 0.66f, 0.74f, 1.00f);

		const bool menuAberto = ImGui::Begin("Campbellpur", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoScrollWithMouse);
		// Posicao da janela PRINCIPAL, guardada aqui (dentro do Begin/End) porque
		// la' no cabecalho o GetWindowPos() ja' devolveria a posicao do
		// BeginChild, e o arrasto moveria o painel em vez da janela.
		const ImVec2 posJanela = ImGui::GetWindowPos();
		if (menuAberto) {
			// -----------------------------------------------------------------
			// MOLDURA DO MENU  1 px de luz + filete de acento no topo.
			//
			// Na borda: o tema zera WindowBorderSize, entao a moldura e'
			// DESENHADA com alpha de verdade. Branco a 6% compoe sobre o
			// painel; um contorno cinza-escuro seria comido pelo color key
			// (o preto e' a chave) e viraria recorte.
			// -----------------------------------------------------------------
			{
			const ImVec2 wp = ImGui::GetWindowPos();
			const ImVec2 ws = ImGui::GetWindowSize();
			ImDrawList* fdl = ImGui::GetBackgroundDrawList();
			fdl->AddRect(ImVec2(wp.x + 0.5f, wp.y + 0.5f),
			             ImVec2(wp.x + ws.x - 0.5f, wp.y + ws.y - 0.5f),
			             IM_COL32(255, 255, 255, 16), 12.0f, 0, 1.0f);
			fdl->AddRectFilledMultiColor(ImVec2(wp.x + 12.0f, wp.y), ImVec2(wp.x + ws.x - 12.0f, wp.y + 1.5f),
			                             IM_COL32(139, 92, 246, 0), IM_COL32(139, 92, 246, 170),
			                             IM_COL32(139, 92, 246, 170), IM_COL32(139, 92, 246, 0));
		}

			// ================= SIDEBAR =================
			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.057f, 0.062f, 0.091f, 1.00f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f, 10.0f));
			ImGui::BeginChild(false, "##sidebar", nullptr, ImVec2(kSidebarLargura, 0.0f), false);

			// marca: o nome REAL do projeto, no acento.
			//
			// Era "PUBG EXTERNAL": nome que nao existe em lugar nenhum do build
			// (o binario e' mtmacr0.exe) e que ainda gruda o cheat no jogo alvo,
			// em texto claro, na propria tela.
			ImGui::PushFont(DirectOverlay::UiTitleFont);
			ImGui::TextColored(acento, L("Campbellpur"));
			ImGui::PopFont();
			ImGui::SameLine(0.0f, 7.0f);
			// "external" fica como legenda do TIPO de programa, nao do nome.
			ImGui::TextDisabled(L("External"));

			ImGui::Dummy(ImVec2(0.0f, 1.0f));
			{
				const ImVec2 p = ImGui::GetCursorScreenPos();
				const float  w = ImGui::GetContentRegionAvail().x;
				ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
					ImVec2(p.x, p.y), ImVec2(p.x + w, p.y + 1.0f),
					IM_COL32(139, 92, 246, 150), IM_COL32(139, 92, 246, 0),
					IM_COL32(139, 92, 246, 0), IM_COL32(139, 92, 246, 150));
				ImGui::Dummy(ImVec2(0.0f, 10.0f));
			}

			// Abas: o indice tem que casar com a ordem dos blocos la' embaixo.
			// Nomes curtos e diretos (antigo: tudo em ingles generico 
			// "VISUALS" nao dizia QUAL visual, "MISC" nao dizia NADA).
			MenuSidebarItem("PLAYER ESP", 0, &ActiveTab);
#if !MENU_SO_ESP
			// AIMBOT nao existe no build publico (ver MenuVariant.h).
			MenuSidebarItem("AIMBOT",     1, &ActiveTab);
#endif
			MenuSidebarItem("HUD & RADAR", 2, &ActiveTab);
			MenuSidebarItem("WORLD",      3, &ActiveTab);
#if !MENU_SO_ESP
			// PLAYERS e'' tabela de diagnostico (atores crus)  so'' no dev.
			MenuSidebarItem("PLAYERS (RAW)", 4, &ActiveTab);
#endif
			MenuSidebarItem("SETTINGS", 5, &ActiveTab);
// ---------------------------------------------------------------------------
// Abas de diagnostico. Fora do build publico: sem o botao E sem o conteudo
// (os dois lados tem que sair juntos, senao o indice da aba muda de sentido).
// Ver Config\MenuVariant.h e tools\build_all.ps1 -Variant.
// ---------------------------------------------------------------------------
#if MENU_DEBUG_TABS
			MenuSidebarItem("DEBUG",    6, &ActiveTab);
			MenuSidebarItem("LOGS",     7, &ActiveTab);
#endif

			// rodape: os atalhos que antes ficavam embaixo do conteudo.
			{
				const float yRodape = ImGui::GetWindowHeight() - 52.0f;
				if (yRodape > ImGui::GetCursorPosY()) ImGui::SetCursorPosY(yRodape);
				ImGui::GetWindowDrawList()->AddRectFilled(
					ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y - 8.0f),
					ImVec2(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x,
					ImGui::GetCursorScreenPos().y - 7.0f),
					IM_COL32(255, 255, 255, 26));
				ImGui::TextDisabled(L("HOME  abrir / fechar"));
				ImGui::TextDisabled(L("END   panic / unload"));
			}

			ImGui::EndChild(false);
			ImGui::PopStyleVar();
			ImGui::PopStyleColor();

			ImGui::SameLine();

			// ================= COLUNA DA DIREITA =================
			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.039f, 0.050f, 0.077f, 1.00f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
			ImGui::BeginChild(false, "##conteudo", nullptr, ImVec2(0.0f, 0.0f), false);

			// ---------- cabecalho ----------
			{
				const ImVec2 hdrMin = ImGui::GetCursorScreenPos();
				const float  hdrW   = ImGui::GetContentRegionAvail().x;
				const float  hdrH   = 50.0f;
				const ImVec2 hdrMax = ImVec2(hdrMin.x + hdrW, hdrMin.y + hdrH);

				ImDrawList* dl = ImGui::GetWindowDrawList();
				dl->AddRectFilled(hdrMin, hdrMax, IM_COL32(10, 14, 21, 255), 9.0f);

				// arrasto da janela (a barra de titulo do ImGui esta' desligada).
				// SetWindowPos por NOME: a versao por posicao moveria o
				// BeginChild atual (o cabecalho mora dentro do ##conteudo).
				ImGui::InvisibleButton("##arrasto_menu", ImVec2(hdrW, hdrH));
				if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
					const ImVec2 delta = ImGui::GetIO().MouseDelta;
					ImGui::SetWindowPos("PubgExt", ImVec2(posJanela.x + delta.x, posJanela.y + delta.y));
				}
				const ImVec2 cursorDepois = ImGui::GetCursorScreenPos();

				// titulo (PushFont/PopFont no MESMO frame: ver o aviso em
				// ApplyProfessionalStyle sobre o crash 0xC0000005)
				ImGui::PushFont(DirectOverlay::UiTitleFont);
				const float tituloY = hdrMin.y + (hdrH - ImGui::GetTextLineHeight()) * 0.5f;
				ImGui::SetCursorScreenPos(ImVec2(hdrMin.x + 16.0f, tituloY));
				ImGui::TextColored(acento, L("Campbellpur"));
				// x do fim do titulo: o chip de versao entra A' DIREITA dele, e
				// depois de um Text o cursor ja' esta' na linha de baixo.
				const float fimTitulo = ImGui::GetItemRectMax().x;
				ImGui::PopFont();

				// "external" como legenda do tipo, no corpo (15 px)  separa
				// NOME de TIPO, que e' o que um cabecalho de produto faz.
				const float corpoY = hdrMin.y + (hdrH - ImGui::GetTextLineHeight()) * 0.5f;
				ImGui::SetCursorScreenPos(ImVec2(fimTitulo + 9.0f, corpoY));
				ImGui::TextDisabled(L("external"));
				// fim do "external": o chip de versao entra A' DIREITA dele. Depois
				// de um Text o cursor ja' desceu uma linha, entao o x do cursor
				// NAO serve de ancora aqui.
				const float fimExternal = ImGui::GetItemRectMax().x;

				// ---- chip de versao: fundo arredondado + "v2.0" ----
				{
					const char* versao = MTM_VERSAO_TAG;
					const ImVec2 tv = ImGui::CalcTextSize(versao);
					const ImVec2 cMin(fimExternal + 9.0f,
					                  hdrMin.y + (hdrH - 20.0f) * 0.5f);
					const ImVec2 cMax(cMin.x + tv.x + 14.0f, cMin.y + 20.0f);
					ImDrawList* hdrDl = ImGui::GetWindowDrawList();
					hdrDl->AddRectFilled(cMin, cMax, IM_COL32(139, 92, 246, 26), 6.0f);
					hdrDl->AddRect(cMin, cMax, IM_COL32(139, 92, 246, 70), 6.0f);
					hdrDl->AddText(ImVec2(cMin.x + 7.0f, cMin.y + (20.0f - tv.y) * 0.5f),
					               IM_COL32(167, 139, 250, 230), versao);
				}

				// ---------- status (pilula a' direita) ----------
				// Mesmas tres situacoes de antes, so' que agora resumidas numa
				// etiqueta em vez de uma linha inteira do cabecalho.
				std::string textoStatus;
				ImVec4      corStatus;
				if (frameData.UWorld != 0 && EngineInstance != nullptr) {
					textoStatus = L("READY");
					corStatus   = ImVec4(0.35f, 1.00f, 0.45f, 1.0f);
				} else if (TargetProcess.current_process.PID != 0) {
					static int dotCount = 0;
					static auto lastDot = std::chrono::steady_clock::now();
					auto nowDot = std::chrono::steady_clock::now();
					if (std::chrono::duration_cast<std::chrono::milliseconds>(nowDot - lastDot).count() > 400) {
						dotCount = (dotCount + 1) % 4;
						lastDot = nowDot;
					}
					textoStatus = L("WAITING FOR LOBBY") + std::string(dotCount, '.');
					corStatus   = ImVec4(1.00f, 0.85f, 0.20f, 1.0f);
				} else {
					textoStatus = L("WAITING FOR LOBBY");
					corStatus   = ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
				}
				{
					const ImVec2 medida = ImGui::CalcTextSize(textoStatus.c_str());
					const float  pilH   = 24.0f;
					const ImVec2 pilMax = ImVec2(hdrMax.x - 14.0f, hdrMin.y + (hdrH + pilH) * 0.5f);
					const ImVec2 pilMin = ImVec2(pilMax.x - (medida.x + 36.0f), pilMax.y - pilH);
					dl->AddRectFilled(pilMin, pilMax,
						IM_COL32((int)(corStatus.x * 255), (int)(corStatus.y * 255), (int)(corStatus.z * 255), 40), pilH * 0.5f);
					dl->AddCircleFilled(ImVec2(pilMin.x + 13.0f, (pilMin.y + pilMax.y) * 0.5f), 3.5f,
						IM_COL32((int)(corStatus.x * 255), (int)(corStatus.y * 255), (int)(corStatus.z * 255), 255));
					dl->AddText(ImVec2(pilMin.x + 24.0f, pilMin.y + (pilH - medida.y) * 0.5f),
						IM_COL32(226, 230, 240, 255), textoStatus.c_str());
				}

				// filete do cabecalho (acento desbotando para a direita)
				const float fileteY = hdrMax.y + 9.0f;
				dl->AddRectFilledMultiColor(ImVec2(hdrMin.x, fileteY), ImVec2(hdrMax.x, fileteY + 1.0f),
					IM_COL32(139, 92, 246, 140), IM_COL32(139, 92, 246, 0),
					IM_COL32(139, 92, 246, 0), IM_COL32(139, 92, 246, 140));

				ImGui::SetCursorScreenPos(cursorDepois);
				ImGui::Dummy(ImVec2(0.0f, 10.0f));
			}

			// ---------- corpo rolavel (uma aba por vez) ----------
			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
			ImGui::BeginChild(false, "##corpo", nullptr, ImVec2(0.0f, 0.0f), false);

#if MENU_SO_ESP
			// O build publico nao tem a aba AIMBOT (nem o botao na sidebar).
			// Se o config/estado deixou o indice nela, volta para VISUALS em
			// vez de desenhar uma aba que nao existe mais.
			if (ActiveTab == 1 || ActiveTab == 4) ActiveTab = 0;
#endif
			// contador de cartoes reinicia a cada aba (o id e' aba*1000+seq)
			g_cartaoSeq = 0;
			if (ActiveTab == 0) {

				// ---------------- Jogador ----------------
				Seccao(L("Jogador"));
				CheckboxPair(L("ESP Box"),  &Configs.Survivor.Box,
				             L("Skeleton"), &Configs.Survivor.Skeleton);

				CheckboxPair(L("Health Bar"), &Configs.Survivor.Health,
				             L("Nome"),       &Configs.Survivor.Name);

				CheckboxPair(L("Distancia [m]"),  &Configs.Survivor.Distance,
				             L("Nome da arma"),   &Configs.Survivor.Weapon);

				CheckboxPair(L("Time [S<n>]"),           &Configs.Survivor.ShowTeamInfo,
				             L("BOT/Player [BOT]/[P]"),  &Configs.Survivor.ShowBotFlag);

				ImGui::Checkbox(L("Snaplines (linha ate' o inimigo)"), &Configs.Survivor.ShowSnaplines);
				if (Configs.Survivor.ShowSnaplines) {
					ImGui::Indent(20.0f);
					ImGui::Checkbox(L("a partir do centro"), &Configs.Survivor.SnaplineFromCenter);
					ImGui::Unindent(20.0f);
				}

				ImGui::SetNextItemWidth(220.0f);
				ImGui::SliderInt(L("Max distance"), &Configs.Survivor.MaxDistance, 100, 3000, "%d m");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(L("jogador. veiculo e item tem teto proprio na aba WORLD"));

				// ---------------- Texto do ESP ----------------
				// Fundo escuro semi-transparente atras de TODO texto do ESP
				// (nome/distancia/arma, loot, veiculo, granada e o HUD). O
				// desenho e' feito por OSImGui::TextWithBg/StrokeTextWithBg,
				// chamados pelo PlayerEsp — o menu aqui so' liga/desliga.
				Seccao(L("Texto do ESP"));
				ImGui::Checkbox(L("Fundo atras do texto"), &Configs.Survivor.TextBackground);
				if (Configs.Survivor.TextBackground) {
					ImGui::SetNextItemWidth(220.0f);
					ImGui::SliderInt(L("Opacidade do fundo"), &Configs.Survivor.TextBgOpacity, 0, 255, "%d");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip(L("alpha do retangulo escuro (0 = invisivel, 255 = solido)"));
				}
#if !MENU_SO_ESP
				ImGui::TextDisabled(L("legibilidade sobre neve/ceu claro"));
#endif

				// ---------------- Qualidade do texto (performance) --------
				// EspTextQuality controla as passadas do contorno do StrokeText
				// (OS-ImGui.cpp): 9 = 8 contornos + corpo (Ultra), 4 = so' os 4
				// eixos + corpo (Rapida). Cada string de ESP paga 1 AddText por
				// passada; com 30+ jogadores o texto e' a maior fatia da CPU do
				// overlay  "Rapida" e' o primeiro item a mexer quando o FPS
				// cair em partida cheia.
				{
					static const char* kQualidades[] = { "Ultra (maxima nitidez)", "Rapida (menos CPU)" };
					int idxQualidade = (Configs.Survivor.EspTextQuality >= 8) ? 0 : 1;
					ImGui::SetNextItemWidth(220.0f);
					if (ImGui::Combo(L("Qualidade do texto do ESP"), &idxQualidade, kQualidades, 2))
						Configs.Survivor.EspTextQuality = (idxQualidade == 0) ? 9 : 4;
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip(L("Rapida desenha 5 camadas de texto por string em vez de 9.\n"
							"O contorno fica um pouco mais fino nos cantos; troca o FPS do\n"
							"overlay em troca de contorno perfeito. Nao mexe em mais nada."));
				}

				// ---------------- Visibilidade ----------------
				// O SINAL MEDIDO NA 2610.1.1.65: `LastTimeScreenRenderered`
				// (Class + 0x750, o mesmo slot do `Eyes`). Aqui ficam o
				// liga/desliga, a tolerancia e  o que importa  os NUMEROS
				// que o Engine mediu no ultimo tick.
				//
				// POR QUE OS NUMEROS FICAM NA TELA: "a cor nao muda" tem
				// quatro causas e o olho nao separa nenhuma 
				//   toggle desligado / base de tempo inexistente /
				//   campo que nao avanca para inimigo / tolerancia errada.
				// Sem a leitura ao vivo, diagnosticar isso exige abrir o
				// logs.txt no meio da partida.
				// ---------------------------------------------------------
				Seccao(L("Visibilidade"));

				ImGui::CheckboxW(L("Visibilidade (LastTimeScreenRenderered)"), &Configs.Survivor.VisCheck, 470.0f);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(L("pinta de 'Oculto' quem esta' atras de parede.\n"
						"desligado = uma cor so' (fail-open, comportamento antigo).\n"
						"os numeros abaixo dizem se o sinal esta' chegando."));

				if (Configs.Survivor.VisCheck) {
					ImGui::Indent(20.0f);

					ImGui::SetNextItemWidth(220.0f);
					ImGui::SliderInt(L("Tolerancia de visibilidade"), &Configs.Survivor.VisToleranceMs, 20, 400, "%d ms");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip(L("quanto tempo o campo pode ficar parado e ainda contar como 'desenhando'.\n"
							"100-250 ms e' a faixa util: abaixo pisca, acima demora a acusar oculto."));

#if !MENU_SO_ESP   // diagnostico de visibilidade: dev only
					// (no publico fica so' o toggle e a tolerancia)
					// ---- LEITURA AO VIVO (thread de memoria -> frameData) ----
					// -----------------------------------------------------------------
					// LEITURA AO VIVO  e agora ela mostra a coisa CERTA.
					//
					// A conta nao usa base de tempo: o veredito vem de "o campo
					// ficou parado quanto tempo". Entao o numero util e'' `parado`,
					// em ms, por ator  nao um "t" que dependia do GameState.
					// -----------------------------------------------------------------
					// -----------------------------------------------------------------
					// QUAL CAMPO ESTA'' EM USO  e os TRES numeros.
					//
					// A duvida "funciona?" virou medicao: submit (0x748) e render
					// (0x74C) acompanham o frustum; on-screen (0x750) e'' o que a
					// PAREDE corta. Com os tres na tela, da'' para ver em partida
					// qual deles congela  sem abrir log.
					// -----------------------------------------------------------------
					if (frameData.VisCampo >= 0) {
						static const char* kNome[3] = { "0x748 submit", "0x74C render", "0x750 on-screen" };
						ImGui::TextColored(ImVec4(0.40f, 0.90f, 0.40f, 1.0f),
						                   L("campo em uso: %s"), kNome[frameData.VisCampo]);
					} else {
						ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.30f, 1.0f),
						                   L("nenhum campo mudando - fail-open"));
					}

					{
						const ImVec4 verde(0.40f, 0.90f, 0.40f, 1.0f);
						const ImVec4 vermelho(0.95f, 0.35f, 0.35f, 1.0f);
						ImGui::TextColored(frameData.VisInimVis ? verde : vermelho,
						                   L("inimigo   submit=%.0f  render=%.0f  onScreen=%.0f ms  -> %s"),
						                   frameData.VisInimParado[0], frameData.VisInimParado[1],
						                   frameData.VisInimParado[2],
						                   frameData.VisInimVis ? L("visivel") : L("OCULTO"));
#if !MENU_SO_ESP
						// O pawn local e' diagnostico puro (em FPP o corpo dele nem e'
						// desenhado). Fora do build publico.
						ImGui::TextColored(frameData.VisLocalVis ? verde : vermelho,
						                   L("seu pawn  submit=%.0f  render=%.0f  onScreen=%.0f ms  -> %s"),
						                   frameData.VisLocalParado[0], frameData.VisLocalParado[1],
						                   frameData.VisLocalParado[2],
						                   frameData.VisLocalVis ? L("visivel") : L("OCULTO"));
#endif
					}
#if !MENU_SO_ESP
					ImGui::TextDisabled(L("%d atores avaliados, %d ocultos"), frameData.VisAtivos, frameData.VisOcultos);
#endif
#if !MENU_SO_ESP
					ImGui::TextDisabled(L("parado ha': tempo desde a ultima mudanca do campo do render"));
#endif

					// -----------------------------------------------------------------
					// AVISO DA COR  porque "nao vi diferenca" tambem pode ser a COR,
					// e nao o sinal. O default antigo do "Oculto" era branco, e o
					// config salvo continua branco: a caixa verde vira branca, muda
					// pouco, e o recurso parece morto mesmo funcionando.
					//
					// O aviso olha o valor REAL do config (nao o default), entao ele
					// desaparece sozinho quando o usuario escolhe uma cor.
					// -----------------------------------------------------------------
					{
						const Colour& oc = Configs.Survivor.TextColour;
						if (oc.r > 235 && oc.g > 235 && oc.b > 235)
							ImGui::TextDisabled(L("cor de 'Oculto' esta' branca - troque em Cores para destacar"));
					}
#endif

					ImGui::Unindent(20.0f);
				} else {
					ImGui::TextDisabled(L("desligado: ESP com uma cor so' (fail-open)"));
				}

				// ---------------- Mundo ----------------
				// A secao "Mundo" SAIU daqui inteira: veiculo e airdrop foram
				// para a aba WORLD, junto com o loot.
				//
				// POR QUE: os tres sao a MESMA coisa do ponto de vista de quem
				// usa  "o que existe no mapa e nao e' jogador". Estavam
				// separados so' por acidente historico (o loot saiu daqui
				// quando o catalogo cresceu e nao caberia mais), e isso deixava
				// o usuario procurando em duas abas coisas que ele pensa
				// juntas.
				//
				// Fica so' o ponteiro, no mesmo estilo do que ja' existia para
				// o loot  senao quem procurar aqui acha que a opcao sumiu.
				// ---------------- Cores ----------------
				// Quatro cores do ESP saem daqui. A quarta ("Oculto") passou a
				// ser ALCANCAVEL na 2610.1.1.65: com `VisCheck` ligado, quem
				// esta' atras de parede cai nela (ver PlayerEsp, escolha entre
				// BoxColour e TextColour). Desligado, o ESP continua fail-open
				// e ela nao aparece  o campo e' o MESMO, so' muda se o sinal
				// de visibilidade esta' sendo medido.
				Seccao(L("Cores"));
				ColourEdit4(L("Caixa do inimigo"), Configs.Survivor.BoxColour);
						ColourEdit4(L("Esqueleto"), Configs.Survivor.SkeletonColour);
						ColourEdit4(L("DBNO (deitado)"), Configs.Survivor.DbnoColour);
						ColourEdit4(L("Oculto (atras de parede)"), Configs.Survivor.TextColour);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(L("usada so' quando o vischeck esta' ligado (secao Visibilidade, acima)"));
#if !MENU_SO_ESP
				ImGui::TextDisabled(L("loot = cor por categoria (fixas no ESP)"));
#endif

				// =========================================================
				// (O bloco do VisCheck que ficava AQUI foi para a secao
				// "Visibilidade", acima de Mundo.) Ele nasceu junto das cores
				// com a justificativa de que "o efeito e' cor"  e essa
				// justificativa custou achar o controle: quem procurava
				// visibilidade olhava a secao "Visibilidade", que tinha sido
				// apagada. Controle de visibilidade mora na secao de
				// visibilidade, mesmo que o efeito apareca nas cores.
				// =========================================================

				// =========================================================
				// ESPESSURA DO TRACO  da CAIXA e do ESQUELETO.
				//
				// Fica junto das CORES de proposito: e' o mesmo tipo de
				// decisao visual ("como o ESP aparece"), e separar em outra
				// secao faria procurar aqui e achar la'.
				//
				// O desenho usa estes valores em PlayerEsp.cpp
				// (`Gui.Rectangle` da caixa e os 14 `Gui.Line` do esqueleto).
				// Ver o bloco em PlayerConfig.h para por que o default
				// afinou de 1.5 para 1.0.
				// =========================================================
				ImGui::SetNextItemWidth(220.0f);
				ImGui::SliderFloat(L("Espessura da caixa"), &Configs.Survivor.BoxThickness, 0.5f, 4.0f, "%.1f px");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(L("contorno da caixa do inimigo (1.0 = 1 pixel nitido)"));

				ImGui::SetNextItemWidth(220.0f);
				ImGui::SliderFloat(L("Espessura do esqueleto"), &Configs.Survivor.SkeletonThickness, 0.5f, 4.0f, "%.1f px");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(L("traco dos ossos (14 segmentos usam o mesmo valor)"));

				ImGui::Checkbox(L("Suavizar caixa"), &Configs.Survivor.BoxSmooth);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(L("interpolacao da caixa entre frames (mata o tremor do esqueleto animado)\ndesligado = caixa bruta, 100% instantanea"));

				// =========================================================
				// LAYOUT DO ESP — PREVIEW ARRASTAVEL
				//
				// A GEOMETRIA e' a mesma do ESP (ESP/EspLayout.h). Isso nao e'
				// detalhe: se o preview calculasse por conta propria, na
				// primeira divergencia ele passaria a mentir — voce arrasta
				// num lugar e o texto sai em outro.
				//
				// COMO FUNCIONA: cada elemento e' um chip arrastavel.
				//   - enquanto arrasta, o chip segue o mouse e o `dx/dy` do
				//     config guarda o deslocamento;
				//   - ao SOLTAR, ele encaixa na ancora mais proxima e o `dx/dy`
				//     vira o RESIDUO daquele encaixe.
				//
				// O empilhamento (`Pilha`) e' recalculado a cada frame na mesma
				// ordem do ESP, entao o que aparece aqui e' o que sai no jogo.
				// =========================================================
				Seccao(L("Layout do ESP"));
				ImGui::TextDisabled(L("arraste um elemento; solte perto de uma ancora pra encaixar"));

				{
					const float areaL = 268.0f;
					const float areaA = 400.0f;
					const ImVec2 areaP = ImGui::GetCursorScreenPos();

					// -----------------------------------------------------------------
					// DUMMY, E NAO `INVISIBLE_BUTTON`.
					//
					// A versao anterior reservava a area com um InvisibleButton. Ele e'
					// um BOTAO: fica HOVERED no frame em que o mouse desce, e o ImGui
					// entrega o `ActiveId` pro PRIMEIRO item que reage — ou seja, a area
					// roubava o clique dos chips, que sao submetidos DEPOIS.
					//
					// Era esse o "so' consegui arrastar o kills e com muito custo": o
					// chip so' vencia quando o ImGui ainda lembrava do hover do frame
					// anterior. `Dummy` reserva o mesmo espaco e NAO interage — so' os
					// chips disputam o mouse, e cada um tem o seu retangulo.
					// -----------------------------------------------------------------
					ImGui::Dummy(ImVec2(areaL, areaA));

					ImDrawList* dl = ImGui::GetWindowDrawList();
					const ImVec2 areaB(areaP.x + areaL, areaP.y + areaA);
					dl->AddRectFilled(areaP, areaB, IM_COL32(13, 14, 18, 255), 6.0f);
					dl->AddRect(areaP, areaB, IM_COL32(255, 255, 255, 20), 6.0f);

					// "Boneco" de referencia — a mesma nocao de caixa que o ESP
					// usa (topo = cabeca, base = pe', halfW = metade da largura).
					EspLayout::Caixa cx;
					cx.cx     = areaP.x + areaL * 0.5f;
					// O boneco fica mais para BAIXO do que o centro: os elementos
					// de TOPO empilham ACIMA dele (4 elementos x ~23 px = ~92 px)
					// e precisam caber dentro da area — antes eles saiam por cima
					// e ficavam fora da regiao reservada.
					cx.top    = areaP.y + 140.0f;
					cx.bottom = cx.top + 150.0f;
					cx.halfW  = 29.0f;

					dl->AddRect(ImVec2(cx.cx - cx.halfW, cx.top),
					            ImVec2(cx.cx + cx.halfW, cx.bottom),
					            IM_COL32(150, 122, 246, 170), 1.5f);

					// Guias: um ponto na posicao NOMINAL de cada ancora, pra dar
					// a referencia de onde o chip vai encaixar.
					{
						const float midY = (cx.top + cx.bottom) * 0.5f;
						const float gy[ESP_ANC_COUNT] = {
							cx.top - EspLayout::kFolga, cx.bottom + EspLayout::kFolga, midY, midY,
							cx.top - EspLayout::kFolga, cx.top - EspLayout::kFolga,
							cx.bottom + EspLayout::kFolga, cx.bottom + EspLayout::kFolga };
						const float gx[ESP_ANC_COUNT] = {
							cx.cx, cx.cx, cx.cx - cx.halfW, cx.cx + cx.halfW,
							cx.cx - cx.halfW, cx.cx + cx.halfW,
							cx.cx - cx.halfW, cx.cx + cx.halfW };
						for (int i = 0; i < ESP_ANC_COUNT; ++i)
							dl->AddCircleFilled(ImVec2(gx[i], gy[i]), 2.0f, IM_COL32(150, 122, 246, 80));
					}

					const ImGuiIO& io = ImGui::GetIO();
					EspLayout::Pilha pilha;
					int arrastando = -1;
					Vec2 centroArrasto(0.0f, 0.0f);

					for (int e = 0; e < ESP_EL_COUNT; ++e) {
						EspElementLayout& L = Configs.Survivor.EspLayout[e];
						const char* rot = EspElementRotulo(e);

						// -----------------------------------------------------------------
						// O TAMANHO DO CHIP ACOMPANHA A FORMA REAL DO ELEMENTO.
						//
						// A barra de vida tem DOIS formatos: horizontal nas ancoras de
						// topo/base e VERTICAL nas laterais (ver PlayerEsp.cpp). Mostrar
						// ela sempre como um retangulo de texto faria o preview mentir
						// justamente sobre a coisa que o usuario esta' posicionando — e
						// pior, o empilhamento sairia errado, porque a altura da barra
						// vertical e' a altura da CAIXA, nao a de uma linha de texto.
						// -----------------------------------------------------------------
						const bool ehBarra = (e == ESP_EL_HEALTH);
						const bool barraVert = ehBarra &&
							(L.anchor == ESP_ANC_LEFT || L.anchor == ESP_ANC_RIGHT);

						float w, h;
						if (ehBarra) {
							const float esp = 6.0f;   // espessura (o lado curto)
							w = barraVert ? esp : (cx.halfW * 2.0f);
							h = barraVert ? (cx.bottom - cx.top) : esp;
						} else {
							const ImVec2 ts = ImGui::CalcTextSize(rot);
							w = ts.x + 12.0f;
							h = ts.y + 6.0f;
						}

						// Estado da pilha ANTES deste elemento. Guardar e' o que
						// permite recalcular o encaixe sem avançar a pilha duas
						// vezes (o que deslocaria todos os elementos seguintes).
						const EspLayout::Pilha antes = pilha;

						bool kc = false;
						const Vec2 p = EspLayout::Resolver(cx, L.anchor, L.dx, L.dy, w, h, pilha, kc);
						const float x = kc ? (p.x - w * 0.5f) : p.x;
						const float y = p.y;

						ImGui::SetCursorScreenPos(ImVec2(x, y));
						char id[32];
						snprintf(id, sizeof(id), "##espel%d", e);
						ImGui::InvisibleButton(id, ImVec2(w, h));

						const bool ativo = ImGui::IsItemActive();
						if (ativo) {
							arrastando = e;

							// `io.MouseDelta` DIRETO, sem `IsMouseDragging`.
							//
							// O `IsMouseDragging(0)` do ImGui so' passa a reportar
							// depois do LIMIAR de arrasto (~6 px por padrao): o chip
							// ficava "duro", ignorando os primeiros pixels de
							// movimento. Com o delta cru o arrasto e' 1:1 desde o
							// primeiro pixel.
							L.dx += io.MouseDelta.x;
							L.dy += io.MouseDelta.y;

							// Trava: um chip arrastado longe demais nunca mais
							// seria alcancado para trazer de volta.
							if (L.dx >  240.0f) L.dx =  240.0f;
							if (L.dx < -240.0f) L.dx = -240.0f;
							if (L.dy >  240.0f) L.dy =  240.0f;
							if (L.dy < -240.0f) L.dy = -240.0f;

							centroArrasto = Vec2(x + w * 0.5f, y + h * 0.5f);
						}

						// ---- ENCAIXE, no frame em que solta ----
						if (ImGui::IsItemDeactivated()) {
							// Posicao do chip com o dx/dy JA' atualizado neste
							// frame (senao o encaixe atrasaria um frame).
							EspLayout::Pilha t1 = antes;
							bool k1 = false;
							const Vec2 at = EspLayout::Resolver(cx, L.anchor, L.dx, L.dy, w, h, t1, k1);
							const float atX = k1 ? (at.x - w * 0.5f) : at.x;
							const float atY = at.y;

							const int nova = EspLayout::AncoraMaisProxima(
								cx, atX + w * 0.5f, atY + h * 0.5f);

							// Posicao NOMINAL da ancora nova (dx/dy zerados) no
							// MESMO estado de pilha. O residuo guardado reproduz
							// exatamente o lugar onde o chip foi solto.
							EspLayout::Pilha t2 = antes;
							bool k2 = false;
							const Vec2 nat = EspLayout::Resolver(cx, nova, 0.0f, 0.0f, w, h, t2, k2);
							const float natX = k2 ? (nat.x - w * 0.5f) : nat.x;

							L.anchor = nova;
							L.dx = atX - natX;
							L.dy = atY - nat.y;
						}

						// ---- desenho do chip (por ultimo: fica por cima) ----
						const ImVec2 a(x, y), b(x + w, y + h);
						const bool quente = (arrastando == e);

						if (ehBarra) {
							// A barra e' desenhada COM A FORMA e com um
							// preenchimento de exemplo (62%) — so' para dar a
							// leitura de "isto e' a vida" sem precisar de rotulo
							// em cima (que nao cabe numa barra de 6 px).
							const float r = 3.0f;
							dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 200), r);
							if (barraVert) {
								const float fh = h * 0.62f;
								dl->AddRectFilled(ImVec2(x, y + h - fh),
								                  ImVec2(x + w, y + h),
								                  IM_COL32(0, 255, 80, 235), r);
							} else {
								const float fw = w * 0.62f;
								dl->AddRectFilled(ImVec2(x, y),
								                  ImVec2(x + fw, y + h),
								                  IM_COL32(0, 255, 80, 235), r);
							}
							dl->AddRect(a, b, quente ? IM_COL32(170, 140, 255, 220)
							                         : IM_COL32(255, 255, 255, 55), r);

							// O rotulo vai AO LADO (do lado oposto ao da caixa),
							// porque a barra e' fina demais para caber texto.
							const char* nome = "Vida";
							const ImVec2 tn = ImGui::CalcTextSize(nome);
							float lx, ly;
							if (barraVert) {
								lx = (L.anchor == ESP_ANC_LEFT) ? (x - tn.x - 6.0f)
								                                : (x + w + 6.0f);
								ly = y + h * 0.5f - tn.y * 0.5f;
							} else {
								lx = x + w * 0.5f - tn.x * 0.5f;
								ly = (L.anchor == ESP_ANC_BOTTOM ||
								      L.anchor == ESP_ANC_BOTTOM_LEFT ||
								      L.anchor == ESP_ANC_BOTTOM_RIGHT) ? (y + h + 2.0f)
								                                        : (y - tn.y - 2.0f);
								// Nao deixa o rotulo escapar por cima da area.
								if (ly < areaP.y + 2.0f) ly = y + h + 2.0f;
							}
							dl->AddText(ImVec2(lx, ly), IM_COL32(226, 231, 236, 235), nome);
						} else {
							dl->AddRectFilled(a, b, quente ? IM_COL32(70, 48, 130, 240)
							                               : IM_COL32(30, 33, 41, 240), 4.0f);
							dl->AddRect(a, b, quente ? IM_COL32(170, 140, 255, 170)
							                         : IM_COL32(255, 255, 255, 28), 4.0f);
							dl->AddText(ImVec2(x + 6.0f, y + 3.0f), IM_COL32(226, 231, 236, 255), rot);
						}
					}

					// -----------------------------------------------------------------
					// O QUE VAI ENCAIXAR, enquanto ainda esta' arrastando.
					//
					// Sem isto o encaixe parece aleatorio: voce solta e o chip pula
					// para uma ancora que nao estava indicada em lugar nenhum. Agora
					// a ancora-alvo fica marcada DURANTE o arrasto, entao o
					// comportamento fica previsivel antes de soltar.
					//
					// Desenhado DEPOIS dos chips para nao ser coberto por eles.
					// -----------------------------------------------------------------
					if (arrastando >= 0) {
						const int alvo = EspLayout::AncoraMaisProxima(cx, centroArrasto.x, centroArrasto.y);
						const float midY = (cx.top + cx.bottom) * 0.5f;
						const float gy[ESP_ANC_COUNT] = {
							cx.top - EspLayout::kFolga, cx.bottom + EspLayout::kFolga, midY, midY,
							cx.top - EspLayout::kFolga, cx.top - EspLayout::kFolga,
							cx.bottom + EspLayout::kFolga, cx.bottom + EspLayout::kFolga };
						const float gx[ESP_ANC_COUNT] = {
							cx.cx, cx.cx, cx.cx - cx.halfW, cx.cx + cx.halfW,
							cx.cx - cx.halfW, cx.cx + cx.halfW,
							cx.cx - cx.halfW, cx.cx + cx.halfW };

						dl->AddCircle(ImVec2(gx[alvo], gy[alvo]), 6.0f, IM_COL32(170, 140, 255, 230), 0, 2.0f);
						dl->AddText(ImVec2(areaP.x + 8.0f, areaP.y + areaA - 22.0f),
						            IM_COL32(170, 140, 255, 230), EspAnchorNome(alvo));
					}

					// Os chips usaram SetCursorScreenPos e mexeram no cursor:
					// devolve ele pra BAIXO da area, senao o proximo widget cai
					// no meio do preview.
					ImGui::SetCursorScreenPos(ImVec2(areaP.x, areaP.y + areaA + 8.0f));
				}

				if (ImGui::Button(L("Resetar layout do ESP"), ImVec2(200.0f, 0.0f))) {
					// Pega os defaults do PROPRIO PlayerConfig (config recem
					// construida) em vez de repetir a tabela aqui: uma copia a
					// menos pra sair de sinc com o cabecalho.
					PlayerConfig padrao("Default");
					for (int e = 0; e < ESP_EL_COUNT; ++e)
						Configs.Survivor.EspLayout[e] = padrao.EspLayout[e];
				}

			}

			if (ActiveTab == 1) {

				// ---------------- Geral ----------------
				Seccao(L("Geral"));
				CheckboxPair(L("Enable aimbot"),     &Configs.Aimbot.Enabled,
				             L("Visibility check"),  &Configs.Aimbot.VisibilityCheck);

				CheckboxPair(L("Auto-bone selection"), &Configs.Aimbot.AutoBone,
				             L("Lock-on target"),      &Configs.Aimbot.LockOnTarget);

				const char* keys[] = { "Left Alt", "Right Click", "Shift", "Mouse 5", "Mouse 4", "Left Mouse" };
				ImGui::SetNextItemWidth(200.0f);
				ImGui::Combo(L("Hotkey"), &Configs.Aimbot.Hotkey, keys, IM_ARRAYSIZE(keys));

				if (!Configs.Aimbot.AutoBone) {
					const char* bones[] = { "Head", "Neck", "Chest", "Pelvis" };
					ImGui::SetNextItemWidth(200.0f);
					ImGui::Combo(L("Bone alvo"), &Configs.Aimbot.TargetBone, bones, IM_ARRAYSIZE(bones));
				}

				// ---------------------------------------------------------
				// DESTRAVAR O EIXO VERTICAL
				//
				// Marcado: a mira corrige SO' o horizontal (yaw) e o pitch fica
				// inteiro com a mao do jogador.
				//
				// O rotulo diz "(aimbot + RCS)" de proposito: neste cheat os
				// dois movem a vista pelo MESMO caminho (InjectMouseMove) e o
				// RCS e' quem mais puxa o pitch — se o gate valesse so' no
				// aimbot, o recuo continuaria subindo a mira. Ver o bloco de
				// comentario em SDK/Aimbot.cpp.
				// ---------------------------------------------------------
				ImGui::Checkbox(L("Destravar eixo vertical (so' horizontal)"), &Configs.Aimbot.AimbotUnlockVertical);
				ImGui::TextDisabled(L("o pitch nunca e' puxado: aimbot e RCS so' corrigem yaw"));

				ImGui::Checkbox(L("Draw FOV circle"), &GDrawFovCircle);
				ImGui::SetNextItemWidth(220.0f);
				ImGui::SliderFloat(L("FOV"), &Configs.Aimbot.FOV, 1.0f, 90.0f, "%.1f deg");

				ImGui::Spacing();
				ImGui::TextDisabled(L("Smoothing / tracking"));
				ImGui::SetNextItemWidth(220.0f);
				ImGui::SliderFloat(L("AR smooth"), &Configs.Aimbot.AR_SmoothValue, 0.1f, 10.0f, "%.1f");
				ImGui::SetNextItemWidth(220.0f);
				ImGui::SliderFloat(L("Sniper smooth"), &Configs.Aimbot.SR_baseSmooth, 0.1f, 10.0f, "%.1f");
				ImGui::SetNextItemWidth(220.0f);
				ImGui::SliderInt(L("Max delta (anti-snap)"), &Configs.Aimbot.MaxDelta, 1, 50);
				ImGui::SetNextItemWidth(220.0f);
				ImGui::SliderFloat(L("Aggression"), &Configs.Aimbot.Aggression, 0.5f, 3.0f, "%.2f");
				ImGui::SetNextItemWidth(220.0f);
				ImGui::SliderFloat(L("Driver scale"), &Configs.Aimbot.DriverScale, 0.10f, 0.50f, "%.2f");

				// ---------------- Predicao ----------------
				Seccao(L("Predicao"));
				ImGui::Checkbox(L("Prediction (previsao balistica)"), &Configs.Survivor.Prediction);
				if (ImGui::IsItemHovered())
				ImGui::SetTooltip(L("antecipa o alvo pelo tempo de voo da bala\n(bullet speed / gravity lidos do jogo)"));

				// ---------------- Recoil (RCS) ----------------
				Seccao(L("Recoil (RCS)"));
				ImGui::Checkbox(L("Enable RCS (no-recoil)"), &Configs.Aimbot.RCS);
				if (ImGui::IsItemHovered())
				ImGui::SetTooltip(L("compensa o recuo real lendo o acumulador da arma\n"
					"(nao precisa de tabela por arma)"));

				// ---------------------------------------------------------
				// CALIBRACAO DA SENSIBILIDADE
				//
				// `Deg per count` e' o unico numero que o RCS nao deduz
				// sozinho: depende da sensibilidade in-game, que nao esta'
				// legivel em memoria. O botao MEDE em vez de chutar — pulso de
				// mouse conhecido, le o giro da vista, divide, e desfaz.
				// ---------------------------------------------------------
				ImGui::SetNextItemWidth(220.0f);
				if (ImGui::Button(L("Calibrate sensitivity"), ImVec2(220.0f, 0.0f))) {
					Aimbot::RequestSensitivityCalibration();
				}
				if (ImGui::IsItemHovered())
				ImGui::SetTooltip(L("mede quantos counts giram 1 grau.\n"
					"empunhe a arma, NAO atire, e clique.\n"
					"a mira vai dar um tranco curto e voltar."));

				{
					const Aimbot::RcsReadout rc = Aimbot::GetRcsReadout();
					if (rc.calibrating) {
						ImGui::ProgressBar(rc.calibProgress, ImVec2(220.0f, 0.0f));
						ImGui::SameLine();
						ImGui::TextDisabled(L("%s"), rc.calibMsg);
					}
				}

				ImGui::SetNextItemWidth(220.0f);
				ImGui::SliderFloat(L("Deg per count"), &Configs.Aimbot.RCSDegPerCount, 0.005f, 0.060f, "%.4f");
				if (ImGui::IsItemHovered())
				ImGui::SetTooltip(L("graus que a vista gira por 1 count de mouse, na FOV 90.\n"
					"o botao Calibrate preenche isto sozinho.\n"
					"manual: mira ainda SOBE -> baixe;  mira DESCE -> suba"));

				ImGui::SetNextItemWidth(220.0f);
				ImGui::SliderFloat(L("Vertical (pitch)"), &Configs.Aimbot.AR_RecoilValue, 0.0f, 150.0f, "%.0f%%");
				if (ImGui::IsItemHovered())
				ImGui::SetTooltip(L("quanto do recuo vertical anular.\n"
					"100%% = anula tudo;  70%% = deixa 30%% (menos obvio)"));
				ImGui::SetNextItemWidth(220.0f);
				ImGui::SliderFloat(L("Horizontal (yaw)"), &Configs.Aimbot.AR_RecoilYaw, 0.0f, 150.0f, "%.0f%%");

				ImGui::SetNextItemWidth(220.0f);
				ImGui::SliderInt(L("Max counts/tick"), &Configs.Aimbot.RCSMaxCounts, 5, 60);
				if (ImGui::IsItemHovered())
				ImGui::SetTooltip(L("trava de seguranca: um read ruim nunca vira um flick"));

				ImGui::Checkbox(L("Auto-calibrate"), &Configs.Aimbot.RCSAutoCalibrate);
				if (ImGui::IsItemHovered())
				ImGui::SetTooltip(L("mede o quanto a mira AINDA subiu depois da compensacao\n" 
					"e ajusta o ganho sozinho durante a rajada.\n\n" 
					"corrige diferenca de arma/acessorio. NAO e salvo: reaprende a cada partida."));

				// Largura propria (220 px): sem isto o hitbox do checkbox
				// cobriria a linha inteira e o botao ao lado ficaria DENTRO da
				// area de clique dele  clicar em "Reset RCS state" podia
				// inverter a direcao no mesmo clique.
				ImGui::CheckboxW(L("Invert direction"), &Configs.Aimbot.RCSInvert, 220.0f);
				ImGui::SameLine(240.0f);
				if (ImGui::Button(L("Reset RCS state"))) {
					// forca reancorar no proximo frame (troca de arma / bug).
					// Zera tambem o multiplicador da auto-calibracao.
					Aimbot::ResetRcs();
				}

				// ---------------------------------------------------------
				// HUMANIZACAO DO RCS
				//
				// O RCS puxava o valor TEORICO exato, todo frame, desde o
				// primeiro. Uma mao nao faz isso: reage depois de ~90 ms e
				// varia o quanto puxa. Ver o bloco de comentario em
				// Config/AimbotConfig.h e em SDK/Aimbot.cpp.
				//
				// A leitura do que ele aplicou (rampa/jitter/adiou) esta' no
				// log [RCS] 1x/s  e' como se mede se a forca esta' alta demais.
				// ---------------------------------------------------------
				ImGui::Checkbox(L("Humanizar o RCS"), &Configs.Aimbot.RCSHumanize);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(L("reacao + variacao por frame + cadencia irregular.\n"
						"a media da compensacao nao muda; o padrao muda."));

				if (Configs.Aimbot.RCSHumanize) {
					ImGui::Indent(20.0f);
					ImGui::SetNextItemWidth(220.0f);
					ImGui::SliderInt(L("Forca da humanizacao"), &Configs.Aimbot.RCSHumanizeStrength, 0, 100, "%d%%");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip(L("0%% = so' a reacao. 100%% = variacao bem visivel.\n"
							"acima de ~60%% a compensacao fica pior que a teorica."));

					ImGui::SetNextItemWidth(220.0f);
					ImGui::SliderInt(L("Reacao no inicio da rajada"), &Configs.Aimbot.RCSReactionMs, 0, 300, "%d ms");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip(L("quanto tempo ate' a compensacao chegar no maximo.\n"
							"0 = compensa no talo desde o primeiro frame (sem reacao)."));

					// -----------------------------------------------------
					// SERVO LAG  o controle que conserta o sinal.
					//
					// Sem ele a vista NUNCA sobe durante a rajada: a
					// compensacao sai no mesmo frame do tiro. Com ele a
					// vista sobe e e' puxada de volta, que e' o que a
					// telemetria de um jogador mostra.
					// -----------------------------------------------------
					ImGui::SetNextItemWidth(220.0f);
					ImGui::SliderInt(L("Suavidade (servo lag)"), &Configs.Aimbot.RCSLagMs, 0, 250, "%d ms");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip(L("espalha a compensacao em vez de aplicar tudo no frame do tiro.\n"
							"a vista passa a SOBIR e ser puxada de volta (dente de serra),\n"
							"como a de um jogador. nada se perde: so' muda o instante.\n"
							"0 = aplica tudo no frame (comportamento antigo)."));

					ImGui::SetNextItemWidth(220.0f);
					ImGui::SliderFloat(L("Ruido (passeio)"), &Configs.Aimbot.RCSNoiseCounts, 0.0f, 4.0f, "%.1f counts");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip(L("erro de compensacao com INERCIA (~200 ms), em counts.\n"
							"ruido branco some quando alguem faz media numa janela;\n"
							"um passeio nao. 0 = sem ruido."));
					ImGui::Unindent(20.0f);
				}

				// ---------------------------------------------------------
				// LEITURA AO VIVO
				//
				// Existe para dar para calibrar OLHANDO o menu, em vez de
				// abrir o logs.txt e ligar o DebugLogs. Tudo vem de atomics
				// escritos pela thread de memoria.
				// ---------------------------------------------------------
				{
					const Aimbot::RcsReadout r = Aimbot::GetRcsReadout();
					Seccao(L("RCS ao vivo"));
					ImGui::Text(L("counts/grau %.1f    gain x%.2f"), r.countsPerDeg, r.gainMul);
					ImGui::Text(L("dreno  Y %.3f  R %.3f  ->  %.2f graus"), r.dropY, r.dropR, r.degY);
					ImGui::Text(L("injetado  dx %d   dy %d"), r.cx, r.cy);
					// ---- mao (servo lag) ----
					// `devendo` e' o number que denuncia o servo: durante a
					// rajada ele fica ALTO e serpenteia. Cravado em 0.00 com
					// `Suavidade` > 0 significa que o servo nao esta' sendo
					// alimentado  sensor morto, arma nao lida, ou toggle.
					ImGui::Text(L("mao  lat %.0f ms   ganho %.3f   passo %.2f   devendo %.2f   ruido %.2f"),
						r.lagMs, r.gainHz, r.stepY, r.owedY, r.noiseY);
					if (Configs.Aimbot.RCSAutoCalibrate) {
						ImGui::Text(L("residual da janela  %.3f graus"), r.residual);
					}
					ImGui::TextColored(
						r.firing ? ImVec4(0.40f, 0.90f, 0.40f, 1.00f) : ImVec4(0.55f, 0.55f, 0.55f, 1.00f),
						"%s", r.firing ? "atirando" : "parado");
				}

			}

			if (ActiveTab == 2) {

				// ---------------- Espectadores ----------------
				Seccao(L("Espectadores"));
				ImGui::Checkbox(L("Show spectators counter"), &Configs.Survivor.ShowSpectators);

				// ---------------- HUD de tela ----------------
				// Contador de espectadores + "Closest Enemy". A posicao agora
				// vem do config (HudX/HudY) e o HUD e' ARRASTAVEL com o botao
				// esquerdo — mas so' com o MENU ABERTO: fechado, a janela do
				// overlay tem WS_EX_TRANSPARENT e o clique vai pro jogo.
				Seccao(L("HUD (espectadores / mais perto)"));
				ImGui::Checkbox(L("HUD arrastavel com o mouse"), &Configs.Survivor.HudDraggable);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(L("arraste com o botao esquerdo com o MENU ABERTO\n(o clique nao chega no overlay com o menu fechado)"));
				ImGui::SetNextItemWidth(200.0f);
				ImGui::SliderFloat(L("HUD X"), &Configs.Survivor.HudX, 0.0f, 2560.0f, "%.0f");
				ImGui::SetNextItemWidth(200.0f);
				ImGui::SliderFloat(L("HUD Y"), &Configs.Survivor.HudY, 0.0f, 1440.0f, "%.0f");
				ImGui::TextDisabled(L("tambem da' pra arrastar direto na tela (menu aberto)"));

				// ---------------- Radar ----------------
				Seccao(L("Radar"));
				ImGui::Checkbox(L("Enable radar"), &Configs.Survivor.Radar);
				if (Configs.Survivor.Radar) {
					ImGui::Indent(20.0f);
					ImGui::SetNextItemWidth(200.0f);
					ImGui::SliderFloat(L("Tamanho"), &Configs.Survivor.RadarSize, 100.0f, 400.0f, "%.0f");
					ImGui::SetNextItemWidth(200.0f);
					ImGui::SliderFloat(L("Alcance"), &Configs.Survivor.RadarRange, 50.0f, 500.0f, "%.0fm");
					ImGui::SetNextItemWidth(200.0f);
					ImGui::SliderFloat(L("Pos X"), &Configs.Survivor.RadarX, 0.0f, 1920.0f, "%.0f");
					ImGui::SetNextItemWidth(200.0f);
					ImGui::SliderFloat(L("Pos Y"), &Configs.Survivor.RadarY, 0.0f, 1080.0f, "%.0f");
					ImGui::Checkbox(L("North-up (fixo no norte)"), &Configs.Survivor.RadarNorthUp);
					ImGui::SetNextItemWidth(200.0f);
					ImGui::SliderFloat(L("Arrow offset (deg)"), &Configs.Survivor.RadarArrowOffset, -180.0f, 180.0f, "%.0f");
					if (ImGui::IsItemHovered())
					ImGui::SetTooltip(L("encaixe o radar em cima do minimapa do jogo"));

					// Blue Zone so' aparece dentro do radar (Radar::Render
					// retorna cedo se o radar estiver desligado).
					ImGui::Checkbox(L("Blue Zone (risco)"), &Configs.Survivor.ZoneRadar);
					if (ImGui::IsItemHovered())
					ImGui::SetTooltip(L("Le GameState/PlayerArray. Causou BSOD (PAGE_FAULT) numa partida real - ligue por sua conta e risco."));
					ImGui::Unindent(20.0f);
				}

				// ---------------- Overlay ----------------
				Seccao(L("Overlay"));
				ImGui::Checkbox(L("Streamproof (some de OBS/Discord)"), &Configs.Survivor.StreamProof);
				ImGui::Checkbox(L("Modo captura (EpicPen/print/OBS)"), &Configs.Survivor.CaptureMode);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(L("Deixa o overlay visivel para capturas: forca WDA_NONE (por cima\n"
						"da Streamproof) e recria a janela no backend LEGADO na hora.\n"
						"Use antes de tirar print com o EpicPen e depois desligue se quiser\n"
						"voltar a ficar invisivel nas streams. Ao ligar, um flash da janela\n"
						"e' esperado  ela e' recriada em vida."));

				// ---------------- Mira central custom ----------------
				Seccao(L("Mira central custom"));
				const char* estilos[] = { "Desligada", "Ponto", "Cruz", "Circulo" };
				ImGui::SetNextItemWidth(200.0f);
				ImGui::Combo(L("Estilo"), &Configs.Survivor.CrosshairStyle, estilos, IM_ARRAYSIZE(estilos));
				if (Configs.Survivor.CrosshairStyle > 0) {
					ImGui::SetNextItemWidth(200.0f);
					ImGui::SliderFloat(L("Mira tamanho"), &Configs.Survivor.CrosshairSize, 2.0f, 30.0f, "%.0f");
					ImGui::SetNextItemWidth(200.0f);
					ImGui::SliderFloat(L("Mira vao"), &Configs.Survivor.CrosshairGap, 0.0f, 20.0f, "%.0f");
				}

			}

			if (ActiveTab == 3) {

				// -----------------------------------------------------------------
				// OBJETOS DO MUNDO  secao nova, no TOPO da aba.
				//
				// Veiculo e airdrop vieram de VISUALS: os tres (junto com o
				// loot logo abaixo) sao a mesma pergunta para quem usa  "o que
				// existe no mapa que nao e' jogador". Estavam em duas abas por
				// acidente historico.
				//
				// A ordem importa: primeiro o que e' OBJETO e e' ligado por um
				// toggle so' (veiculo, airdrop), depois o LOOT, que tem o
				// catalogo e o filtro por item. Assim a aba le de cima para
				// baixo, do simples para o detalhado.
				// -----------------------------------------------------------------
				Seccao(L("Objetos do mundo"));
				ImGui::Checkbox(L("Veiculos"), &Configs.Survivor.Vehicle);
				// Teto PROPRIO do veiculo (ver PlayerConfig::MaxDistVehicle).
				ImGui::SetNextItemWidth(220.0f);
				ImGui::SliderInt(L("Distancia maxima: veiculo"), &Configs.Survivor.MaxDistVehicle, 50, 2000, "%d m");
				ImGui::Checkbox(L("Airdrop / caixa de suprimento"), &Configs.Survivor.AirdropEsp);
#if !MENU_SO_ESP
				ImGui::TextDisabled(L("granadas (frag/molotov): sempre desenhadas - sem toggle no config"));
#endif

				ImGui::Separator();

				Seccao(L("Loot ESP"));
				ImGui::Checkbox(L("Loot ESP (master)"), &Configs.Survivor.Loot);
				// Teto PROPRIO do item (ver PlayerConfig::MaxDistItem). E' o
				// slider que mais alivia o frame: loot e' a maioria dos atores.
				ImGui::SetNextItemWidth(220.0f);
				ImGui::SliderInt(L("Distancia maxima: item"), &Configs.Survivor.MaxDistItem, 20, 600, "%d m");

				// SNAPSHOT do catalogo. A thread de memoria continua
				// inserindo enquanto a gente desenha: iterar o map original
				// aqui seria corrida (o rebalanceamento da arvore invalida
				// o iterador). Copia sob o mutex e desenha a copia — o lock
				// NAO fica preso durante o ImGui.
				std::map<std::string, bool> catalogo;
				{
					std::lock_guard<std::mutex> lock(G_LootCatalogMutex);
					catalogo = G_LootCatalog;
				}

				// Espelha no snapshot o mesmo que foi aplicado no catalogo,
				// senao o checkbox so' reflete o clique no frame seguinte.
				auto patchSnapshot = [&](int catIdx, bool visivel) {
					for (auto& kv : catalogo)
					if (catIdx < 0 || (int)ClassifyLootRaw(kv.first) == catIdx)
					kv.second = visivel;
				};

				// Mesmo espelhamento, mas por SUBSTRING: usado pelos botoes
				// "Ligar/Desligar os que casam". A regra vem do config
				// (LootRawCasaAlgumFiltro), nao e' reimplementada aqui.
				auto patchSnapshotMatching = [&](std::map<std::string, bool>& snap, bool visivel) {
					for (auto& kv : snap)
					if (Configs.Survivor.LootRawCasaAlgumFiltro(kv.first))
						kv.second = visivel;
				};

				// =========================================================
				// FILTRO POR SUBSTRING  o mecanismo UNICO para arma, mira e
				// acessorio.
				//
				// Por que depois do snapshot: a lista mostra quantos itens do
				// catalogo casam com cada substring, e essa conta e' feita
				// sobre a copia (o map original nao pode ser iterado aqui).
				//
				// Por que a lista e' um std::vector e nao um campo por
				// categoria: o usuario cria quantas quiser, e a regra e' a
				// mesma para todas. "Weapon" pega arma, "Scope" pega mira,
				// "Muzzle" pega acessorio de cano  sem um `if` por tipo.
				// =========================================================
				{
					auto& subs = Configs.Survivor.LootSubFilters;

					Seccao(L("Filtro por nome (substring)"));

					// ---- linha de entrada: campo + Adicionar ----
					// `static` porque o buffer do InputText tem que sobreviver
					// entre frames (o ImGui guarda o cursor/estado por ID, nao
					// o texto). Estamos na thread de render, entao nao ha'
					// corrida com o ESP.
					static char subNovo[64] = {};
					ImGui::SetNextItemWidth(220.0f);
					const bool entrou = ImGui::InputText(L("##novosub"), subNovo, IM_ARRAYSIZE(subNovo),
					                                     ImGuiInputTextFlags_EnterReturnsTrue);
					ImGui::SameLine();
					const bool clicouAdd = ImGui::Button(L("Adicionar"));

					if (entrou || clicouAdd) {
						// Trim simples: espaco nas pontas vem de colar texto e
						// faria a substring nao casar com nada, sem aviso.
						std::string s(subNovo);
						while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
						while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.pop_back();
						if (!s.empty()) {
							// Duplicata (case-insensitive) nao entra duas
							// vezes: a segunda so' confundiria a contagem.
							bool jaTem = false;
							for (const auto& f : subs) {
								if (f.sub.size() == s.size() &&
								    _stricmp(f.sub.c_str(), s.c_str()) == 0) { jaTem = true; break; }
							}
							if (!jaTem) {
								LootSubFilter nf;
								nf.sub = s;
								nf.on  = true;
								subs.push_back(nf);
							}
							subNovo[0] = '\0';
						}
					}

					// ---- modo: excluir x incluir ----
					{
						static const char* modos[] = {
							L("esconder os que casam"),
							L("mostrar so' os que casam")
						};
						ImGui::SetNextItemWidth(220.0f);
						if (Configs.Survivor.LootSubFilterMode < 0 ||
						    Configs.Survivor.LootSubFilterMode > 1)
							Configs.Survivor.LootSubFilterMode = 0;
						ImGui::Combo(L("modo"), &Configs.Survivor.LootSubFilterMode, modos, IM_ARRAYSIZE(modos));
					}

					if (subs.empty()) {
						ImGui::TextDisabled(L("nenhuma substring - lista vazia nao filtra nada"));
					} else {
						// ---- lista: cada substring com o seu on/off ----
						// Um indice a remover, aplicado DEPOIS do laco: apagar
						// dentro do `for` invalidaria o iterador no meio do
						// ImGui (e o PushID seguinte iria pro lugar errado).
						int remover = -1;
						for (int i = 0; i < (int)subs.size(); i++) {
							ImGui::PushID(i);

							// LARGURA DA COLUNA DO TOGGLE  e nao a linha inteira.
							// Com o `Checkbox` antigo (largura = regiao de
							// conteudo) o trilho ia para a borda direita da aba
							// e o `SameLine` seguinte jogava o NOME DO FILTRO
							// para fora da tela. Aqui o toggle ocupa 44 px e o
							// rotulo comeca logo depois dele.
							ImGui::CheckboxW("##on", &subs[i].on, 44.0f);
							ImGui::SameLine();
							ImGui::Text(L("%s"), subs[i].sub.c_str());
							ImGui::SameLine();

							int nCasa = 0;
							for (const auto& kv : catalogo) {
								if (CasaSubstringCI(kv.first, subs[i].sub)) nCasa++;
							}
							ImGui::TextDisabled(L("%d casam"), nCasa);

							ImGui::SameLine();
							if (ImGui::SmallButton("x")) remover = i;

							ImGui::PopID();
						}
						if (remover >= 0 && remover < (int)subs.size())
							subs.erase(subs.begin() + remover);

						// ---- acao em massa sobre o filtro INDIVIDUAL ----
						// O substring nao substitui o individual: um decide por
						// padrao, o outro por item. Estes dois botoes levam o
						// padrao para o individual de uma vez  e' o que faz
						// "esconder tudo que tem 'Muzzle'" continuar valendo
						// mesmo se a substring for apagada depois.
						// LARGURA DIVIDIDA, nao automatica: dois botoes de texto
						// longo lado a lado passavam da coluna e eram CORTADOS pela
						// borda do painel.
						{
							const float metade = (ImGui::GetContentRegionAvail().x - 8.0f) * 0.5f;
							if (ImGui::Button(L("Ligar os que casam"), ImVec2(metade, 0.0f))) {
								LootSetVisibleMatching(true);
								patchSnapshotMatching(catalogo, true);
							}
							ImGui::SameLine(0.0f, 8.0f);
							if (ImGui::Button(L("Desligar os que casam"), ImVec2(metade, 0.0f))) {
								LootSetVisibleMatching(false);
								patchSnapshotMatching(catalogo, false);
							}
						}
				}
				}


				Seccao(L("Catalogo"));
				if (ImGui::Button(L("Enable All"))) {
					LootSetVisibleCategory(-1, true);
					patchSnapshot(-1, true);
				}
				ImGui::SameLine();
				if (ImGui::Button(L("Disable All"))) {
					LootSetVisibleCategory(-1, false);
					patchSnapshot(-1, false);
				}
				ImGui::SameLine();
				ImGui::TextDisabled(L("%d itens no catalogo"), (int)catalogo.size());

				if (catalogo.empty()) {
#if !MENU_SO_ESP
					ImGui::TextDisabled(L("entre numa partida para o catalogo se popular"));
#endif
				}

				// Um CollapsingHeader por categoria, na ordem do enum
				// LootCategory (ESP/LootCatalog.h).
				for (int ci = 0; ci < (int)LootCategory::COUNT; ci++) {
					LootCategory cat = (LootCategory)ci;
					bool* geral = LootCategoryBool(Configs.Survivor, cat);

					int n = 0;
					for (auto& kv : catalogo)
					if ((int)ClassifyLootRaw(kv.first) == ci) n++;

					char header[96];
					snprintf(header, sizeof(header), "%s (%d)###wcat%d",
						LootCategoryName(cat), n, ci);
					if (!ImGui::CollapsingHeader(header)) continue;

					// PushID por categoria: os checkboxes la' dentro usam o
					// nome do item como label e dois itens nunca sao iguais,
					// mas o ID do widget precisa ser unico por grupo.
					ImGui::PushID(ci);

					// Mesma razao do `##on` do filtro por substring: o toggle
					// precisa de largura PROPRIA (44 px) para o texto do
					// `SameLine` seguinte cair ao lado dele, e nao no fim da aba.
					if (ImGui::CheckboxW(L("##geral"), geral, 44.0f)) {
						// O checkbox geral e' ACAO, nao so' um portao de
						// desenho: liga/desliga todos os itens do grupo.
						LootSetVisibleCategory(ci, *geral);
						patchSnapshot(ci, *geral);
					}
					ImGui::SameLine();
					ImGui::Text(L("todos os %d itens do grupo"), n);

					if (n == 0) {
						ImGui::TextDisabled(L("nenhum item deste grupo visto ainda"));
					} else {
						// border = false: a lista usa so' o ChildBg (mais escuro
						// que o fundo da aba). Com a borda ligada o ImGui
						// desenhava uma moldura escura em volta da lista.
						ImGui::BeginChild(false, "##itens", nullptr, ImVec2(0, 150), false);
						for (auto& kv : catalogo) {
							if ((int)ClassifyLootRaw(kv.first) != ci) continue;

							// PushID(nome BRUTO): loop de checkboxes com
							// label repetido colidiria o ID do widget 
							// aqui o ID e' o proprio nome do item.
							ImGui::PushID(kv.first.c_str());
							bool visivel = kv.second;
							// LABEL = nome limpo (o que a pessoa le).
							// A CHAVE continua sendo o nome BRUTO (kv.first)  e' ela que
							// persiste no LootItemFilter. Separar label de chave deixa a
							// lista clean sem arriscar a config salva.
							const std::string& rotulo = CleanEntityName(kv.first); // ref p/ o memo (28/10)
							if (ImGui::Checkbox(rotulo.c_str(), &visivel)) {
								LootSetVisibleItem(kv.first, visivel);
								kv.second = visivel;   // snapshot em dia
							}
							// MARCA O QUE O FILTRO DE SUBSTRING ESTA' ESCONDENDO.
							// Sem isso o checkbox aparece marcado e o item nao
							// aparece no mapa  e o usuario conclui que "o ESP
							// esta' quebrado". O individual nao vence o padrao:
							// por isso a marca, em vez de reescrever o checkbox.
							if (!Configs.Survivor.LootRawPassaSubFiltro(kv.first)) {
								ImGui::SameLine();
								ImGui::TextDisabled(L("(filtrado)"));
							}
							ImGui::PopID();
						}
			// Fecha o ULTIMO cartao da aba ativa: sem isto a altura dele nunca e'
			// medida e ele fica sem fundo.
			CartaoFechar(ImGui::GetCursorScreenPos().y);
						ImGui::EndChild(false);
					}

					ImGui::PopID();
				}

				ImGui::Separator();
#if !MENU_SO_ESP
				ImGui::TextDisabled(L("a lista e' do que o cheat JA' VIU: entra sozinho conforme voce' anda pelo mapa"));
#endif

			}

			if (ActiveTab == 4) {
				ImGui::Text(L("Captured actors (first 30)"));
				if (ImGui::BeginTable("PlayerTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 480))) {
					ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 60.0f);
					ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthFixed, 40.0f);
					ImGui::TableSetupColumn("HP", ImGuiTableColumnFlags_WidthFixed, 50.0f);
					ImGui::TableSetupColumn("Dist", ImGuiTableColumnFlags_WidthFixed, 60.0f);
					ImGui::TableSetupColumn("Name");
					ImGui::TableHeadersRow();

					int idx = 0;
					for (auto& actor : frameData.Actors) {
						if (idx++ >= 30) break;
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::Text(L("%s"), actor->PlayerType == 1 ? "Bot" : (actor->PlayerType == 2 ? "Player" : "Other"));
						ImGui::TableSetColumnIndex(1);
						ImGui::Text(L("%d"), actor->LastTeamNum);
						ImGui::TableSetColumnIndex(2);
						ImGui::Text(L("%.0f"), actor->Health);
						ImGui::TableSetColumnIndex(3);
						float dist = Vector3::Distance(Vector3(frameData.CameraCache.POV.Location.X, frameData.CameraCache.POV.Location.Y, frameData.CameraCache.POV.Location.Z), actor->GetPosition()) / 100.0f;
						ImGui::Text(L("%.0fm"), dist);
						ImGui::TableSetColumnIndex(4);
						ImGui::Text(L("%s"), actor->objName.c_str());
					}
					ImGui::EndTable();
				}
			}

			if (ActiveTab == 5) {

				// -----------------------------------------------------------------
				// IDIOMA  PRIMEIRA COISA DA ABA, e nao por acaso.
				//
				// O combo NAO passa por `L(...)`: se ele usasse o texto traduzido
				// como rotulo, trocar de idioma mudaria o proprio ID do widget no
				// meio do frame. O rotulo fica em dois idiomas fixos ("Idioma /
				// Language") e a LISTA de opcoes tambem  e' a unica coisa da UI
				// que nao deve mudar junto com o idioma.
				// -----------------------------------------------------------------
				{
					const char* idiomas[] = { "Portugues (BR)", "English" };
					int& lang = Configs.Survivor.Language;
					if (lang < 0 || lang >= LANG_COUNT) lang = LANG_PT_BR;

					ImGui::SetNextItemWidth(220.0f);
					if (ImGui::Combo("Idioma / Language", &lang, idiomas, IM_ARRAYSIZE(idiomas))) {
						Lang::Set(lang);
					}
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip(L("muda o idioma do menu, do HUD e dos rotulos do ESP na hora"));
				}
				ImGui::Separator();

				Seccao(L("Configuracao"));
				if (ImGui::Button(L("Save Configuration"), ImVec2(240, 40))) {
					SaveConfig(L"Default");
					LOG("[cfg] salvo pelo botao\n");
				}
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0.40f, 0.90f, 0.40f, 1.00f), L("auto-salva ao fechar o menu (HOME)"));
				ImGui::TextDisabled(L(LIT("arquivo: %LOCALAPPDATA%\\RenderCache\\Default.json")));

				Seccao(L("Keybinds"));
				ImGui::BulletText(L("Menu (abrir/fechar): HOME"));
				ImGui::BulletText(L("Panic / unload: END"));
				ImGui::TextDisabled(L("hotkeys globais (RegisterHotKey) - funcionam com o jogo em foco"));

				Seccao(L("Performance"));
				ImGui::SetNextItemWidth(220.0f);
				ImGui::SliderInt(L("Memory FPS"), &Configs.Survivor.MemoryUpdateRate, 30, 240);
				if (ImGui::IsItemHovered())
				ImGui::SetTooltip(L("taxa do loop de leitura de memoria (nao e' o FPS do overlay)"));

			}

#if MENU_DEBUG_TABS
			if (ActiveTab == 6) {
				ImGui::Checkbox(L("Debug logs (CENSO / LOOTDIAG)"), &Configs.Survivor.DebugLogs);

				// =============================================================
				// DRIVER  veredito do anti-cheat e mapeamento manual
				// =============================================================
				//
				// POR QUE ESTES DOIS BOTOES EXISTEM
				//
				// O gate automatico espera o TslGame.exe e SO' ENTAO checa o
				// anti-cheat. Medido no teste 2: quando o jogo aparece na lista
				// de processos, o BattlEye JA' subiu o BEDaisy.sys  a janela
				// entre os dois nao existe do jeito que eu implementei. Resultado
				// pratico: o gate bloqueia sempre e o cheat nunca mapeia.
				//
				// Estes botoes devolvem o CONTROLE para quem esta' na frente da
				// maquina:
				//
				//   VERIFICAR AC : roda a MESMA varredura do gate, mas escrevendo
				//                  no log o que ela viu (modulo, padrao, veredito).
				//                  Responde "o gate esta' certo?" com dado.
				//
				//   MAP NOW      : mapeia na hora, sem o gate. Quem aperta assume
				//                  a decisao  e' o caminho para quando o gate e' o
				//                  obstaculo e o usuario sabe que a maquina esta'
				//                  limpa.
				//
				// NENHUM DOS DOIS e' automatico. O gate continua no lugar dele
				// no startup: isto e' override explicito, nao politica.
				// =============================================================
				Seccao(L("Driver / Anti-cheat"));

				if (ImGui::Button(L("VERIFICAR AC (log detalhado)"))) {
					AntiCheat::Estado ac = AntiCheat::ChecarComRelatorio();
					LOG("[ac] (pelo menu) Ativo=%d resumo=%s\n",
					    (int)ac.Ativo(), ac.Resumo().c_str());
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(L("roda a varredura do gate e escreve o de-para no logs.txt"));

				ImGui::SameLine();

				// Vermelho de proposito: nao e' botao de rotina.
				//
				// As TRES cores sao empurradas (normal / hover / active): o
				// `ButtonEx` deste fork agora le' `ImGuiCol_Button*`, e sem as
				// tres o botao vermelho ficaria com hover CIANO do tema  o
				// aviso vermelho desapareceria justamente quando a mao esta' em
				// cima do botao que mapeia o driver.
				ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.12f, 0.12f, 1.00f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.16f, 0.16f, 1.00f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.88f, 0.22f, 0.22f, 1.00f));
				if (ImGui::Button(L("MAP NOW (mapeia sem o gate)"))) {
					LOG("[!] MAP NOW pelo menu  mapeando SEM o gate\n");

					if (TargetProcess.driver.DriverVersionOk()) {
						LOG("[*] ja' existe driver desta build (0x%08X)  nada a fazer\n",
						    TargetProcess.driver.driverVersion);
					} else {
						TargetProcess.driver.Cleanup();
						if (SubirDriverKevent("MAP NOW")) {
							// Avisa o gate (que pode estar girando neste instante)
							// que o mapeamento JA' aconteceu. Ver GMapeadoManual.
							GMapeadoManual.store(true, std::memory_order_relaxed);
						}
					}
				}
				ImGui::PopStyleColor(3);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(L("mapeia o driver AGORA, sem checar anti-cheat.\n"
					                    "O gate automatico nao roda mais nesta sessao."));

				ImGui::TextDisabled(L("driver: %s   seed: 0x%016llX"),
				    TargetProcess.driver.HandleValido() ? "aberto" : "FECHADO",
				    (unsigned long long)TargetProcess.driver.GetSeed());

				// ---------------------------------------------------------
				// TELEMETRIA KEVENT v5  saude do protocolo em tempo real.
				//
				// badSig    sobiu  -> alguem tocou na shared memory que nao era
				//                       o cliente (o driver rejeitou o lote). Em
				//                       sessao sadia este numero NAO move.
				// throttled sobe  -> a taxa de leitura estourou o teto do driver.
				// watchdog  sobe  -> o lote ficou parado >60 s e o driver se
				//                       limpou sozinho (cliente travou/desligou).
				// ---------------------------------------------------------
				if (TargetProcess.driver.UsandoKevent()) {
					uint32_t bs = 0, th = 0, wd = 0;
					TargetProcess.driver.KeventEstatisticas(&bs, &th, &wd);
					const uint32_t g = TargetProcess.driver.KeventLastGlobalStatus();
					const char* statusTxt = (g == KMP_OK)                ? "ok"
					                     : (g == KMP_ERR_BAD_SIGNATURE) ? "BAD SIG!"
					                     : (g != 0)                     ? "erro"
					                                                       : "sem resposta";
					ImGui::TextColored((g == KMP_ERR_BAD_SIGNATURE)
					                        ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 0.55f),
					                   L("kevent v5: last=%s  badSig=%u  throttled=%u  watchdog=%u"),
					                   statusTxt, bs, th, wd);

					// Aviso unico no log: primeira evidencia de assinatura invalida.
					static bool avisouBadSig = false;
					if (!avisouBadSig && (bs > 0 || g == KMP_ERR_BAD_SIGNATURE)) {
						LOG("[kdm] AVISO: assinatura invalida no KEVENT (badSig=%u) - "
						    "a shared memory pode estar sendo tocada por outro processo\n", bs);
						avisouBadSig = true;
					}
				}
				// =============================================================
				// PAINEL DE DESEMPENHO  INSTRUMENTACAO DA AUDITORIA
				// =============================================================
				//
				// POR QUE ESTE PAINEL EXISTE
				//
				// Otimizacao sem medicao e'' chute. Este projeto ja'' pagou caro
				// por isso tres vezes (`MaskRight`, `srand`, `NameShlMask`)  nas
				// tres, um "conserto" em cima de sintoma. Aqui a regra e'' inversa:
				// mede, muda UMA coisa, mede de novo no mesmo cenario.
				//
				// NAO escreve arquivo: o usuario pediu explicitamente nada de
				// log.txt. A saida e'' esta janela, e o contador so'' roda com o
				// painel ligado (`Perf::Ativo()`), entao medir nao contamina a
				// medicao da versao normal.
				//
				// COMO LER
				//
				//   CPU do overlay por frame  = build (ms) + present (ms)
				//   `present` inclui a espera do VBLANK: ~1/refresh por
				//   definicao. NAO serve para comparar backend.
				//   IOCTL/s x latencia = tempo gasto DENTRO do kernel. E'' o
				//   numero que diz se a aquisicao de memoria esta'' saturando.
				// =============================================================
				Seccao(L("Desempenho (medicao)"));

				static bool perfLigado = false;
				if (ImGui::Checkbox(L("Medir (liga os contadores)"), &perfLigado)) {
					Perf::Ligar(perfLigado);
					if (perfLigado) Perf::ZerarHistogramas();
				}
				ImGui::SameLine();
				if (ImGui::Button(L("Zerar"))) Perf::ZerarHistogramas();

				if (perfLigado) {
					// ---- taxa por segundo, calculada entre frames ----
					static uint64_t pFrames = 0, pBuild = 0, pPresent = 0;
					static uint64_t pMemTicks = 0, pMemNs = 0;
					static uint64_t pReads = 0, pIoNs = 0, pSnapNs = 0, pSnapN = 0;
					static auto     pT = std::chrono::steady_clock::now();

					const auto agora = std::chrono::steady_clock::now();
					const double dt = std::chrono::duration<double>(agora - pT).count();
					char linha[256];

					if (dt >= 0.5) {
						const uint64_t frames = Perf::C.renderFrames.load(std::memory_order_relaxed);
						const uint64_t build  = Perf::C.buildNs.load(std::memory_order_relaxed);
						const uint64_t pres   = Perf::C.presentNs.load(std::memory_order_relaxed);
						const uint64_t mtick  = Perf::C.memTicks.load(std::memory_order_relaxed);
						const uint64_t mns    = Perf::C.memNs.load(std::memory_order_relaxed);
						const uint64_t reads  = Perf::C.singleReads.load(std::memory_order_relaxed);
						const uint64_t ions   = Perf::C.ioctlNs.load(std::memory_order_relaxed);
						const uint64_t snNs   = Perf::C.snapshotNs.load(std::memory_order_relaxed);
						const uint64_t snN    = Perf::C.snapshotN.load(std::memory_order_relaxed);

						const double dF = (double)(frames - pFrames);
						const double dM = (double)(mtick  - pMemTicks);
						const double dR = (double)(reads  - pReads);
						const double dI = (double)(ions   - pIoNs);
						const double dS = (double)(snNs   - pSnapNs);
						const double dSn= (double)(snN    - pSnapN);

						pFrames = frames; pMemTicks = mtick; pReads = reads;
						pIoNs = ions; pSnapNs = snNs; pSnapN = snN;
						pBuild = build; pPresent = pres; pMemNs = mns;
						pT = agora;

						// ---- RENDER ----
						snprintf(linha, sizeof(linha),
							"render: %.0f fps | build %.2f ms/frame | present %.2f ms/frame",
							dF / dt,
							dF > 0 ? (double)(build - pBuild) / dF / 1e6 : 0.0,
							dF > 0 ? (double)(pres - pPresent) / dF / 1e6 : 0.0);
						ImGui::TextUnformatted(linha);

						snprintf(linha, sizeof(linha),
							"  build P95 %.2f ms  P99 %.2f ms  (cauda = microtravamento)",
							Perf::Percentil(Perf::histBuild, 0.95),
							Perf::Percentil(Perf::histBuild, 0.99));
						ImGui::TextUnformatted(linha);

						// ---- MEMORIA ----
						snprintf(linha, sizeof(linha),
							"memoria: %.0f ticks/s | %.2f ms/tick",
							dM / dt, dM > 0 ? (double)(mns - pMemNs) / dM / 1e6 : 0.0);
						ImGui::TextUnformatted(linha);

						// ---- MEMORIA: LEITURAS (o suspeito n.1) ----
						snprintf(linha, sizeof(linha),
							"leituras: %.0f IOCTL/s | %.1f us por IOCTL | %.1f%% de um nucleo",
							dR / dt,
							dR > 0 ? (double)dI / dR / 1000.0 : 0.0,
							dR > 0 ? 100.0 * (double)dI / (dt * 1e9) : 0.0);
						ImGui::TextUnformatted(linha);

						// ---- DIVISAO DO TEMPO DO TICK (onde os ms vao) ----
						//
						// Percentual do tick de memoria em cada subsistema. E'' o
						// numero que ORDENA o ataque: o maior vira o proximo alvo,
						// e os pequenos ficam parados em vez de virarem "otimizacao"
						// sem efeito.
						if (dM > 0.0) {
							const double inv = 100.0 / (double)(mns - pMemNs > 0 ? (mns - pMemNs) : 1);
							struct { const char* nome; std::atomic<uint64_t>* c; } subs[] = {
								{ "Cache",        &Perf::C.nsCache      },
								{ "Players",      &Perf::C.nsPlayers    },
								{ "Weapons",      &Perf::C.nsWeapons    },
								{ "Grenades",     &Perf::C.nsGrenades   },
								{ "Vehicles",     &Perf::C.nsVehicles   },
								{ "Loot",         &Perf::C.nsLoot       },
								{ "ViewMatrix",   &Perf::C.nsViewMatrix },
								{ "Aimbot",       &Perf::C.nsAimbot     },
								{ "publish(4x)",  &Perf::C.nsPublish    },
							};
							char buf[320] = {0};
							size_t usado = 0;
							for (auto& s : subs) {
								const double pc = (double)s.c->load(std::memory_order_relaxed) * inv;
								if (pc < 0.5) continue;   // abaixo de 0,5% nao interessa
								const int r = snprintf(buf + usado, sizeof(buf) - usado,
													   "%s%s %.0f%%", usado ? " |" : "", s.nome, pc);
								if (r > 0) usado += (size_t)r;
								if (usado >= sizeof(buf) - 32) break;
							}
							ImGui::Text(L("  divisao do tick:%s %s"), usado ? "" : " (sem amostra)", buf);
						}

						// ---- OCLUSAO: se subir, o loop virou spin livre ----
						//
						// Sem tratamento, `DXGI_STATUS_OCCLUDED` faz o Present
						// voltar na hora e o loop gira milhares de vezes/s. Um
						// numero aqui > 0 com `render` acima do refresh CONFIRMA
						// o cenario  e deixa de ser teoria.
						{
							const uint64_t ocl = Perf::C.oclusoes.load(std::memory_order_relaxed);
							if (ocl) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
								L("  OCLUSOES: %llu (Present devolveu DXGI_STATUS_OCCLUDED)"),
								(unsigned long long)ocl);
						}

						// ---- TEXTO: a estimativa do auditor, medida ----
						//
						// O auditor CALCULOU "9 AddText por string" e "11 medicoes,
						// 10 redundantes". Aqui os dois viram numero real. Se
						// `addText/frame` for da ordem de milhares, o contorno de
						// 8 copias e' o dominante do ESP  e nao a leitura de
						// memoria, que e' onde todo mundo olha primeiro.
						{
							static uint64_t pAdd = 0, pCalc = 0, pHit = 0, pMiss = 0;
							const uint64_t a2 = Perf::C.addText.load(std::memory_order_relaxed);
							const uint64_t c2 = Perf::C.calcText.load(std::memory_order_relaxed);
							const uint64_t h2 = Perf::C.memoHit.load(std::memory_order_relaxed);
							const uint64_t m2 = Perf::C.memoMiss.load(std::memory_order_relaxed);
							snprintf(linha, sizeof(linha),
								"texto: %.0f AddText/frame | %.0f CalcTextSizeA/frame | memo hit %.1f%%",
								dF > 0 ? (double)(a2 - pAdd) / dF : 0.0,
								dF > 0 ? (double)(c2 - pCalc) / dF : 0.0,
								(h2 + m2) > 0 ? 100.0 * (double)h2 / (double)(h2 + m2) : 0.0);
							ImGui::TextUnformatted(linha);
							pAdd = a2; pCalc = c2; pHit = h2; pMiss = m2;
						}

						// ---- SNAPSHOT ----
						snprintf(linha, sizeof(linha),
							"snapshot por frame: %.3f ms  (%llu copias)",
							dSn > 0 ? (double)dS / dSn / 1e6 : 0.0,
							(unsigned long long)dSn);
						ImGui::TextUnformatted(linha);

						// ---- TAMANHO DAS LISTAS ----
						{
							std::lock_guard<std::mutex> lk(GDataMutex);
							snprintf(linha, sizeof(linha),
								"listas: actors=%zu grenade=%zu vehicle=%zu loot=%zu",
								GSharedData.Actors.size(), GSharedData.GrenadeActors.size(),
								GSharedData.VehicleActors.size(), GSharedData.LootActors.size());
							ImGui::TextUnformatted(linha);
						}
					}
				}

				if (ImGui::IsItemHovered())
				ImGui::SetTooltip(L("liga os logs pesados de diagnostico.\n"
					"use so' quando o PUBG atualizar e os offsets mudarem"));
				// O [VIS] (medicao #2 da visibilidade) e o [DEATH] (medicao #3:
				// qual campo vira na eliminacao) saem junto com este checkbox:
				// 1 linha por ator, 1x/s. Ficam na aba LOGS.
				ImGui::TextDisabled(L("inclui [VIS] do mesh e [DEATH] da morte (1x/s, aba LOGS)"));
				ImGui::Separator();
				ImGui::TextColored(ImVec4(0.00f, 0.83f, 1.00f, 1.0f), L("Core Engine"));
				ImGui::BulletText(L("UWorld: 0x%llX"), frameData.UWorld);
				ImGui::BulletText(L("GNames: 0x%llX"), frameData.GNames);

				ImGui::TextColored(ImVec4(1.00f, 0.00f, 1.00f, 1.0f), L("Local Player"));
				ImGui::BulletText(L("Pawn: 0x%llX"), frameData.AcknowledgedPawn);
				ImGui::BulletText(L("Team: %d"), Local.Teamid);
				ImGui::BulletText(L("Spectators: %d"), Local.SpectatedCount);
				if (EngineInstance) {
					ImGui::BulletText(L("Bullet Speed: %.1f m/s"), EngineInstance->GetCurrentBulletSpeed());
					ImGui::BulletText(L("Gravity: %.2f"), EngineInstance->GetCurrentGravity());
				}

				ImGui::TextColored(ImVec4(1.00f, 1.00f, 0.00f, 1.0f), L("Camera"));
				auto& pov = frameData.CameraCache.POV;
				ImGui::BulletText(L("Pos: [%.0f, %.0f, %.0f]"), pov.Location.X, pov.Location.Y, pov.Location.Z);
				ImGui::BulletText(L("FOV: %.1f"), pov.FOV);

				ImGui::TextColored(ImVec4(0.00f, 1.00f, 0.00f, 1.0f), L("Actors"));
				ImGui::BulletText(L("Total: %zu"), frameData.Actors.size());

				ImGui::Separator();
				if (ImGui::Button(L("COPY FULL DEBUG REPORT"), ImVec2(220, 36))) {
					std::ostringstream oss;
					oss << "=== GLOBAL STATE ===\n";
					oss << "UWorld: 0x" << std::hex << frameData.UWorld << "\n";
					oss << "GNames: 0x" << frameData.GNames << "\n";
					oss << "AcknowledgedPawn: 0x" << frameData.AcknowledgedPawn << "\n";
					oss << "LocalTeamID: " << std::dec << Local.Teamid << "\n";
					oss << "LocalSpectators: " << Local.SpectatedCount << "\n";
					oss << "Actors Found: " << frameData.Actors.size() << "\n";
					oss << "Camera FOV: " << pov.FOV << "\n";
					oss << "Camera Pos: [" << pov.Location.X << ", " << pov.Location.Y << ", " << pov.Location.Z << "]\n";
					oss << "Camera Rot: [" << pov.Rotation.Pitch << ", " << pov.Rotation.Yaw << ", " << pov.Rotation.Roll << "]\n\n";

					oss << "=== ACTOR SNAPSHOT ===\n";
					for (size_t i = 0; i < frameData.Actors.size(); ++i) {
						auto& a = frameData.Actors[i];
						Vector3 pos = a->GetPosition();
						float dist = Vector3::Distance(Vector3(pov.Location.X, pov.Location.Y, pov.Location.Z), pos) / 100.0f;
						oss << "--- ACTOR[" << std::dec << i << "] ---\n";
						oss << "Name: " << a->objName << "\n";
						oss << "PlayerType: " << a->PlayerType << " | id: " << a->id << "\n";
						oss << "Team: " << a->LastTeamNum << " | Health: " << a->Health << "\n";
						oss << "isDie: " << (a->isDie ? "true" : "false") << " | isCheck: " << (a->isCheck ? "true" : "false") << "\n";
						oss << "Class: 0x" << std::hex << a->Class << "\n";
						oss << "Mesh: 0x" << a->Mesh << " | BoneArray: 0x" << a->BoneArray << "\n";
						oss << "RootComponent: 0x" << a->RootComponent << "\n";
						oss << "WorldPos: [" << std::dec << pos.x << ", " << pos.y << ", " << pos.z << "]\n";
						oss << "Distance: " << dist << "m\n\n";
					}
					std::string str = oss.str();
					ImGui::SetClipboardText(str.c_str());
				}

				ImGui::Separator();
				ImGui::TextColored(ImVec4(0.90f, 0.90f, 0.40f, 1.0f), L("Diagnostics"));
				ImGui::Checkbox(L("Enable ALT mouse-move test"), &TestMouseEnabled);
				ImGui::Checkbox(L("Drawing Test (white box + actor ids)"), &RunDrawingTest);
			}
#endif // MENU_DEBUG_TABS

#if MENU_DEBUG_TABS
			if (ActiveTab == 7) {
				std::string logCopy;
				{
					std::lock_guard<std::mutex> lock(GLiveLogsMutex);
					logCopy = GLiveLogs;
				}
				if (logCopy.empty()) logCopy = "Waiting for logs...\n";
				logCopy.reserve(logCopy.size() + 1);
				ImGui::InputTextMultiline("##logs", logCopy.data(), logCopy.capacity(), ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
			}
#endif // MENU_DEBUG_TABS

			ImGui::EndChild(false);
			ImGui::PopStyleColor();

			ImGui::EndChild(false);
			ImGui::PopStyleVar();
			ImGui::PopStyleColor();
		}
		ImGui::End();
	}
	// =======================================================================
	// GATE DE DESENHO DE JOGO (ESP, FOV circle, radar, HUD)
	//
	// Antes: `frameData.UWorld != 0`. Isso NAO separa "partida" de "lobby" — o
	// lobby tambem tem UWorld. Consequencia medida pelo usuario: ao sair da
	// partida o gate continuava aberto e o ESP desenhava a ULTIMA lista de
	// atores congelada por cima da tela do lobby, para sempre.
	//
	// Agora quem manda e' `frameData.DataValid`, publicado pelo MemoryLoop a
	// partir de Engine::bInMatch (ver SharedState.h e
	// Engine::InvalidateSnapshot): true SO' quando o Cache() leu um estado de
	// partida consistente. Falso = nada de jogo e' desenhado, imediatamente
	// (no frame seguinte a invalidacao).
	//
	// Cobre TODOS os desenhos de dado de partida num lugar so':
	//   - jogador: caixa, esqueleto, nome, distancia, arma, [S<n>]/[BOT],
	//     snapline, barra de vida      -> DrawPlayerEsp (data.Actors)
	//   - loot / veiculo / granada     -> DrawPlayerEsp (LootActors,
	//                                     VehicleActors, GrenadeActors)
	//   - HUD "Closest Enemy"          -> derivado de data.Actors
	//   - HUD "Spectators"             -> global Local.SpectatedCount, que o
	//                                     Engine::InvalidateSnapshot zera
	//   - radar (pontos + zona azul)   -> Radar::Render (data.Actors,
	//                                     data.HasGameState)
	//   - FOV circle do aimbot e o Drawing Test
	//
	// Como o flag e' recalculado a cada tick do MemoryLoop, ele volta a true
	// no primeiro frame de uma partida nova: o ESP nao fica "morto" depois de
	// uma limpeza (era o risco de resolver isso com flag grudenta).
	//
	// O MENU fica fora deste bloco: ele continua funcionando no lobby.
	// =======================================================================
	if (frameData.DataValid) {
		if (RunDrawingTest) {
			float cx = Gui.Window.Size.x / 2.0f;
			float cy = Gui.Window.Size.y / 2.0f;
			Gui.Rectangle(Vec2(cx - 50, cy - 50), Vec2(100, 100), IM_COL32(255, 255, 255, 255), 2.0f);
		}
		if (Configs.Aimbot.Enabled && GDrawFovCircle) {
			float cx = Gui.Window.Size.x / 2.0f;
			float cy = Gui.Window.Size.y / 2.0f;
			float fovRad = Configs.Aimbot.FOV * 3.14159265f / 180.0f;
			float radius = std::tan(fovRad * 0.5f) * Gui.Window.Size.y;
			if (radius < 10.0f) radius = 10.0f;
			if (radius > Gui.Window.Size.y * 0.5f) radius = Gui.Window.Size.y * 0.5f;
			Gui.Circle(Vec2(cx, cy), radius, IM_COL32(255, 255, 255, 80), 1.0f, 64);
		}

		extern void DrawPlayerEsp(const SharedData& data);
		DrawPlayerEsp(frameData);
		
		GRadar.Render(frameData);
	}

	// Mira central custom: e' utilitario de TELA, nao dado de partida — o
	// usuario pode querer a mira no lobby tambem (era o comportamento de antes,
	// quando o gate era `UWorld != 0`). Por isso ela fica FORA do gate novo e
	// mantem o gate antigo: nao le' nada do snapshot, entao nao existe "dado
	// antigo" para ela desenhar.
	if (frameData.UWorld != 0) {
		extern void DrawCrosshair();
		DrawCrosshair();
	}
}

// ============================================================================
// ENTRY POINT
// ============================================================================
//
// Estrategia de overlay desta versao:
//   1. a janela abre PRIMEIRO, so com uma tela de status. Se algo der errado
//      (driver, offsets, jogo fechado) o usuario ve a mensagem em vez de um
//      processo que morre sem explicacao.
//   2. o memory loop e o Engine sobem em thread separada.
//   3. nada e desreferenciado sem checagem de nulo no RenderFrame.
//
// Isso tambem mata a janela preta: antes o overlay so aparecia depois de
// InitializeFeatures() + ProbeReadable() completarem, e essas duas tem while() de
// espera.
// ============================================================================

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* exceptionInfo) {
	LOG("[FATAL] Unhandled Exception: 0x%X at 0x%p\n", 
		exceptionInfo->ExceptionRecord->ExceptionCode, 
		exceptionInfo->ExceptionRecord->ExceptionAddress);
	return EXCEPTION_CONTINUE_SEARCH;
}

// ===========================================================================
// BattlEye  o "sinal de vida" que decide SE vale a pena mapear agora.
//
// POR QUE ISTO EXISTE
//
// O kdmapper NAO passa pelo NtLoadDriver para o NOSSO driver: ele aloca pool,
// copia a imagem e chama o entry point na mao. E' por isso que o callback de
// IMAGEM CARREGADA do BattlEye nao ve o nosso driver  essa e' a vantagem
// principal da tecnica.
//
// Mas duas coisas nossas SAO observaveis com o BE ativo:
//
//   1. o passo do driver Intel usa NtLoadDriver de verdade (service.cpp),
//      e esse dispara os callbacks de imagem e de registro;
//   2. o nosso driver cria um DEVICE OBJECT e um SYMLINK  IoCreateDevice e
//      IoCreateSymbolicLink sao visiveis para quem tem
//      ObRegisterCallbacks/CmRegisterCallbackEx registrado.
//
// Entao mapear com o BE rodando e' mensuravelmente pior do que mapear fora
// desse intervalo. Nao torna seguro  so' escolhe a hora menos ruim.
//
// O QUE ESTA FUNCAO NAO E'
//
// Nao e' um detector de anti-cheat. E' heuristica: procura os vestigios mais
// faceis (processo do servico e device do driver). BE ativo de um jeito que
// isto nao pega continua ativo, e um BE instalado mas parado pode dar falso
// positivo. Serve para AVISAR, com informacao, na hora de decidir.
// ===========================================================================
// Espera o processo aparecer SEM tocar no driver.
//
// Por que nao usar o laco antigo (`TargetProcess.Init`): ele chama
// `driver.Initialize()` e faz leitura de UWorld. Antes do mapeamento isso nao
// tem driver nenhum pra ler, e cada tentativa custaria segundos. Esta versao e'
// so' a lista de processos.
// ===========================================================================
// STEAM + PUBG AUTO-LAUNCH (naya — LAUNCH click pe khud launch karega)
//
// Kya karta hai:
//   1. Steam ka install path registry se dhundta hai
//   2. Agar Steam band hai to steam.exe chalata hai
//   3. Steam ready hone ka wait karta hai (steamwebhelper.exe check)
//   4. Phir steam://rungameid/578080 (PUBG appid) ke zariye game launch karta
//
// NAYA — sirf helpers. Koi existing code touch nahi.
// ===========================================================================

// Steam ka install path registry se nikalta hai.
// HKLM\SOFTWARE\WOW6432Node\Valve\Steam\InstallPath (64-bit systems)
// HKLM\SOFTWARE\Valve\Steam\InstallPath (32-bit fallback)
static std::string LocalizarSteamExe()
{
	auto lerReg = [](HKEY root, const wchar_t* subkey) -> std::string {
		HKEY h = nullptr;
		if (RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS)
			return "";
		wchar_t buf[MAX_PATH] = { 0 };
		DWORD tam = sizeof(buf);
		DWORD tipo = 0;
		LONG r = RegQueryValueExW(h, L"InstallPath", nullptr, &tipo, (LPBYTE)buf, &tam);
		RegCloseKey(h);
		if (r != ERROR_SUCCESS || tipo != REG_SZ) return "";
		// UTF-16 -> UTF-8 (paths sao ASCII na pratica, mas seguro converter)
		char out[MAX_PATH * 2] = { 0 };
		int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out) - 1, nullptr, nullptr);
		if (n <= 0) return "";
		return std::string(out);
		};

	std::string base = lerReg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam");
	if (base.empty()) base = lerReg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam");
	if (base.empty()) return "";

	std::filesystem::path exe = std::filesystem::path(base) / "steam.exe";
	std::error_code ec;
	if (!std::filesystem::exists(exe, ec)) return "";
	return exe.string();
}

// Steam ka core process (steamwebhelper.exe) chal raha hai?
// steam.exe akela starter hai; steamwebhelper actual UI + IPC karta hai.
// Jab tak steamwebhelper na aaye, steam://rungameid kaam nahi karega.
static bool SteamPronto()
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return false;
	bool achou = false;
	PROCESSENTRY32W pe{};
	pe.dwSize = sizeof(pe);
	if (Process32FirstW(snap, &pe)) {
		do {
			if (_wcsicmp(pe.szExeFile, L"steamwebhelper.exe") == 0) {
				achou = true;
				break;
			}
		} while (Process32NextW(snap, &pe));
	}
	CloseHandle(snap);
	return achou;
}

// steam://rungameid/578080 se PUBG launch karta hai.
// 578080 = PUBG: BATTLEGROUNDS (appid).
// Ek chhoti si .cmd file banata hai jo ShellExecute se chalti hai —
// ShellExecuteW directly bhi kaam karta, par .cmd me output capture safe hai.
static bool LancarPubgViaSteam()
{
	// steam:// protocol Windows me Steam ke sath register hota hai.
	// Seedha ShellExecuteW kafi hai — koi temp file ki zaroorat nahi.
	HINSTANCE r = ShellExecuteW(nullptr, L"open",
		L"steam://rungameid/578080",
		nullptr, nullptr, SW_SHOWNORMAL);
	// ShellExecuteW return: > 32 = success. <= 32 = error code.
	return ((INT_PTR)r > 32);
}

// ===========================================================================
// AguardarSteamELancarPubg — main flow jo LAUNCH pe chalta hai.
//
// Returns:
//   true  = PUBG process launch ho gaya (TslGame.exe dhundhne ke liye ready)
//   false = user ne CANCEL kiya (GIsRunning false) ya total timeout
//
// Messages design (user ke mutabiq):
//   - Steam band hai to messages alternate karte hain, user pareshan na ho
//   - Steam already open hai to 1s me done
//   - Har ~3s me message change — user ko lagta hai kuch ho raha hai
// ===========================================================================
static bool AguardarSteamELancarPubg()
{
	// ---- 1) Steam ka path dhundo ----
	LoaderStatus("Opening Steam App.....");
	std::this_thread::sleep_for(std::chrono::milliseconds(600));

	const std::string steamExe = LocalizarSteamExe();
	const bool steamJaRodando = ProcessoExiste("steam.exe");

	if (steamExe.empty() && !steamJaRodando) {
		// Steam install hi nahi hai — cheat aage nahi badh sakta
		LOG("[-] Steam not found (registry + process both empty)\n");
		LoaderStatus("Steam not found - install Steam first");
		return false;
	}

	// ---- 2) Agar Steam band hai to chalao ----
	if (!steamJaRodando && !steamExe.empty()) {
		LOG("[*] Steam not running, launching: %s\n", steamExe.c_str());
		STARTUPINFOA si{};
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi{};
		std::string cmd = "\"" + steamExe + "\"";
		std::vector<char> cmdBuf(cmd.begin(), cmd.end());
		cmdBuf.push_back('\0');
		BOOL ok = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr,
			FALSE, 0, nullptr, nullptr, &si, &pi);
		if (ok) {
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
			LOG("[+] Steam launched (pid %lu)\n", pi.dwProcessId);
		}
		else {
			LOG("[-] CreateProcess for Steam failed (err=%lu)\n", GetLastError());
		}
	}

	{
		static const char* kMsgs[] = {
			"Waiting for Steam :)",
			"Steam App process is almost opened just wait...",
			"Do not click anything wait patiently ....",
		};
		int idx = 0;
		int tentativas = 0;
		while (tentativas < 20 && GIsRunning) {   // 20 x 3s = 60s cap
			if (SteamPronto()) break;
			LoaderStatus("%s", kMsgs[idx % 3]);
			idx++;
			std::this_thread::sleep_for(std::chrono::milliseconds(3000));
			tentativas++;
		}
		if (!GIsRunning) return false;
		if (!SteamPronto()) {
			LOG("[-] Steam not ready after 60s\n");
			LoaderStatus("Steam not ready - try again");
			return false;
		}
	}
	LOG("[+] Steam ready (steamwebhelper.exe up)\n");

	// ---- 4) PUBG launch via steam:// protocol ----
	LoaderStatus("Checking Game Library...");
	std::this_thread::sleep_for(std::chrono::milliseconds(800));

	if (!LancarPubgViaSteam()) {
		LOG("[-] ShellExecuteW(steam://rungameid/578080) failed\n");
		LoaderStatus("Failed to launch PUBG via Steam");
		return false;
	}
	LOG("[+] steam://rungameid/578080 sent\n");

	LoaderStatus("PUBG Game found and Running");
	std::this_thread::sleep_for(std::chrono::milliseconds(1200));

	LoaderStatus("Waiting for process TslGame.exe...");
	return true;
}
static bool ProcessoExiste(const char* nome)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return false;

	bool achou = false;
	PROCESSENTRY32W pe{};
	pe.dwSize = sizeof(pe);

	if (Process32FirstW(snap, &pe)) {
		do {
			char exe[64] = { 0 };
			for (int i = 0; i < 63 && pe.szExeFile[i]; i++)
				exe[i] = (pe.szExeFile[i] < 128) ? (char)pe.szExeFile[i] : '?';
			if (_stricmp(exe, nome) == 0) { achou = true; break; }
		} while (Process32NextW(snap, &pe));
	}
	CloseHandle(snap);
	return achou;
}

static bool GateAntiCheat(int tentativasMax)
{
	for (int i = 0; i < tentativasMax && GIsRunning; i++) {
		if (GMapeadoManual.load(std::memory_order_relaxed)) {
			LOG("[+] MAP NOW rodou durante o gate — driver ja' esta' no ar.\n");
			LOG("[+] Nao ha' o que bloquear: seguindo para a inicializacao.\n");
			return true;
		}

		AntiCheat::Estado e = AntiCheat::Checar();

		if (!e.Ativo()) {
			if (i > 0)
				LOG("[+] anti-cheat exited after %d attempt(s) - mapping now\n", i);
			else
				LOG("[+] AntiCheat Clean - Mapping Now\n");

			// Servico INSTALADO nao bloqueia, mas merece uma linha: se o modulo
			// aparecer depois, este log e' o que explica o que mudou.
			if (e.servico)
				LOG("[*] (service present but stopped: %s)\n", e.Resumo().c_str());
			return true;
		}

		if (i == 0) {
			LOG("[!] ==================================================\n");
			LOG("[!] AntiCheat is Active - Mapping BLOCKED\n");
			LOG("[!]   %s\n", e.Resumo().c_str());
			LOG(LIT("[!] kdmapper does not trigger the BE image-load callback,\n"));
			LOG("[!] but the donor driver step uses an actual NtLoadDriver call, and\n");
			LOG("[!] our driver's IoCreateDevice/IoCreateSymbolicLink are visible\n");
			LOG("[!] while BE is active. Mapping now is choosing the worst possible time.\n");
			LOG("[!] Trying every 10s (or close the game / service).\n");
			LOG("[!] ==================================================\n");
			LoaderStatus("Found AntiCheat Traces | Mapping Blocked\n");	
		}
		else if (i % 1 == 0) {
			LOG("[*] AntiCheat still active (%d attempts): %s\n", i, e.Resumo().c_str());
			LoaderStatus("Still Found | Clean it Yourself (%d/10 attempts)", i);
		}

		std::this_thread::sleep_for(std::chrono::seconds(10));
	}

	if (!GIsRunning) return false;

	LOG("[-] anti-cheat did not exit within ceiling. Proceeding WITHOUT driver.\n");
	LOG("[-] (cheat opens, but without memory read - no ESP)\n");
	LoaderStatus("anti-cheat timeout - proceeding without driver");
	return false;
}

static std::atomic_bool GDriverJaDescarregado{ false };
#define KMP_PAGES_MAGIC 0x3147504Au   // 'KPG1'
struct KmpPagesState {
    uint32_t Magic;       // 'KPG1'
    uint32_t Version;     // =1 (reserva de mudanca de formato)
    uint64_t Base;        // base da alocacao (para o MmFreeIndependentPages)
    uint64_t PageBytes;   // tamanho alocado (o MESMO que o unload usaria)
    uint64_t BootId;      // BootTime do boot que fez a carga
};

static std::wstring CaminhoPaginasResiduas()
{
    wchar_t local[1024] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local))) return L"";
    return std::wstring(local) + L"\\RenderCache\\kmp_pages.bin";
}

static uint64_t BootIdAtual()
{
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);   // retorna void; valor zero e' praticamente impossivel
    const uint64_t agora = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    const uint64_t up100 = GetTickCount64() * 100ULL;   // uptime em 100ns
    const uint64_t boot  = (agora > up100) ? (agora - up100) : 0;
    return boot ? boot : 0xDEADBEEFC0DEULL;
}

static void GravarPaginasResiduas(uint64_t base, uint64_t size)
{
    if (base == 0 || size == 0) return;
    const std::wstring caminho = CaminhoPaginasResiduas();
    if (caminho.empty()) return;

    {
        HANDLE hPrev = CreateFileW(caminho.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hPrev != INVALID_HANDLE_VALUE) {
            KmpPagesState antigo{}; DWORD n = 0;
            ReadFile(hPrev, &antigo, sizeof(antigo), &n, nullptr);
            CloseHandle(hPrev);
            if (n == sizeof(antigo) && antigo.Magic == KMP_PAGES_MAGIC && antigo.Base != base)
                LOG("[!] page record overwritten (old base 0x%llX); removed from tracking until reboot)\n",
                    (unsigned long long)antigo.Base);
        }
    }

    KmpPagesState st{};
    st.Magic     = KMP_PAGES_MAGIC;
    st.Version   = 1;
    st.Base      = base;
    st.PageBytes = size;
    st.BootId    = BootIdAtual();

    HANDLE h = CreateFileW(caminho.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LOG("[!] failed to save page state (err=%lu); future release lost\n", GetLastError());
        return;
    }
    DWORD n = 0;
    WriteFile(h, &st, sizeof(st), &n, nullptr);
    CloseHandle(h);
    LOG("[*] Pages from 0x%llX (0x%llX bytes) remain resident until the next start, with the game closed.\n",
        (unsigned long long)base, (unsigned long long)size);
}

// Apaga o registro apos LIBERACAO bem-sucedida (DescarregarDriver).
//
// O ciclo completo (hardening-p4):
//     mapa ok  -> GravarPaginasResiduas   (SubirDriverKevent)
//     free ok  -> ApagarRegistroPaginasResiduas (DescarregarDriver /
//                                               TentarLiberarPaginasResiduas)
// Qualquer saida entre os dois pontos  fim normal com jogo aberto, crash,
// taskkill, queda de luz  termina com o registro no disco e a liberacao no
// proximo start com jogo fechado.
static void ApagarRegistroPaginasResiduas()
{
    const std::wstring caminho = CaminhoPaginasResiduas();
    if (!caminho.empty()) _wremove(caminho.c_str());
}

// Tenta liberar as paginas que a execucao anterior deixou para tras.
// Barato quando nao ha nada (o state file nem existe na maioria dos starts).
static void TentarLiberarPaginasResiduas(const char* origem)
{
    const std::wstring caminho = CaminhoPaginasResiduas();
    if (caminho.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (GetFileAttributesExW(caminho.c_str(), GetFileExInfoStandard, &fa) == INVALID_FILE_ATTRIBUTES)
        return;

    KmpPagesState st{};
    HANDLE h = CreateFileW(caminho.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { _wremove(caminho.c_str()); return; }
    DWORD lido = 0;
    ReadFile(h, &st, sizeof(st), &lido, nullptr);
    CloseHandle(h);
    if (lido != sizeof(st) || st.Magic != KMP_PAGES_MAGIC ||
        st.Version != 1 || st.Base == 0 || st.PageBytes == 0 ||
        (st.Base & 0xFFFULL) != 0 || st.Base < 0x100000000ULL ||
        st.PageBytes > (64u * 1024 * 1024)) {
        _wremove(caminho.c_str());   // registro corrompido: limpa e segue
        return;
    }
    if (st.BootId != BootIdAtual()) {
        LOG("[*] %s: The page registration is from a previous boot; there is nothing left to release.\n", origem);
        _wremove(caminho.c_str());
        return;
    }
    // Com o jogo aberto nao se carrega doador: e' o mesmo motivo da politica
    // do unload (o momento mais observado do sistema inteiro).
    if (ProcessoExiste(ProcessName.c_str())) {
        LOG("[*] %s: The game is open; pages from the previous run remain.\n", origem);
        return;
    }

    LOG("[*] %s: releasing the resident pages from the previous execution (base=0x%llX size=0x%llX)...\n",
        origem, (unsigned long long)st.Base, (unsigned long long)st.PageBytes);

    if (!NT_SUCCESS(intel_driver::Load())) {
        LOG("[-] %s: unable to load the donor for release; will retry at the next start\n", origem);
        return;
    }

    const bool ok = intel_driver::MmFreeIndependentPages(st.Base, (uint32_t)st.PageBytes);
    intel_driver::Unload();
    if (ok) {
        _wremove(caminho.c_str());
        LOG("[+] %s: pages from the previous execution released (0x%llX, 0x%llX bytes)\n",
            origem, (unsigned long long)st.Base, (unsigned long long)st.PageBytes);
    } else {
        LOG("[-] %s: MmFreeIndependentPages Pages failed on the old base; record discarded\n", origem);
        _wremove(caminho.c_str());
    }
}

static void DescarregarDriver()
{
    // Guarda de idempotencia: este processo chama isto uma vez so', mas a
    // funcao e' publica dentro do arquivo e um segundo caminho de saida (falha
    // do overlay, panic) poderia chamar de novo. Um segundo SELFDESTRUCT
    // chegaria num device que nao existe  inofensivo, mas confunde o log.
    if (GDriverJaDescarregado.exchange(true))
        return;

    // -----------------------------------------------------------------------
    // DOIS BACKENDS, DOIS CAMINHOS DE DESCARGA.
    //
    //   IOCTL  -> existe device: UnmapComInfo (device+symlink) e depois
    //             GET_IMAGE_INFO devolve base/tamanho para o mapper liberar.
    //
    //   KEVENT -> NAO existe device. Nao ha' o que desmapear do namespace: o
    //             que existe e' uma thread de kernel + secao + eventos, e ela
    //             para pelo handshake `KMP_MAGIC_STOP` dentro do Cleanup().
    //             A base vem publicada no cabecalho (Hdr.ImageBase) e o
    //             tamanho sai do PE embutido (o driver mapeado nao tem
    //             headers para ler o proprio SizeOfImage).
    //
    // Antes disto o KEVENT nem chegava aqui: o `if (!HandleValido()) return;`
    // olhava o handle do DEVICE, que num backend sem device e' sempre invalido.
    // Resultado: sessao KEVENT nunca liberava nada.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // SO' EXISTE UM TRANSPORTE: KEVENT.
    //
    // (O bloco antigo ramificava entre IOCTL e KEVENT. O IOCTL foi removido em
    // 20/10/2026  ver km_safe_source.c em _legacy e ARQUITETURA.md.)
    // -----------------------------------------------------------------------
    if (!TargetProcess.driver.HandleValido())
        return;

    // -----------------------------------------------------------------------
    // JOGO ABERTO NA HORA DE DESCARREGAR.
    //
    // Tecnicamente o unload NAO depende do jogo: a sessao e' parada antes
    // (`GIsRunning = false`), o servidor recebe o STOP e nada mais le' memoria
    // do TslGame. Nao ha' risco de bugcheck por causa do jogo aberto.
    //
    // O problema e' outro, e e' de exposicao: para LIBERAR as paginas o unload
    // carrega o driver doador de novo (`intel_driver::Load`). Fazer isso com o
    // BattlEye ativo e' o pior momento possivel  e' o evento que ele mais
    // observa. Com o jogo fechado, o mesmo carregamento passa despercebido.
    //
    // Na pratica este caso ficou pequeno: com o lazy unmap (logo abaixo da
    // parada confirmada) o unload com jogo aberto NAO carrega o doador  ele
    // marca as paginas e sai. O aviso fica por cobertura dos casos em que
    // base/tamanho vieram invalidos.
    // -----------------------------------------------------------------------
    {
        const int pidJogo = TargetProcess.driver.GetProcessId(L"TslGame.exe");
        if (pidJogo > 0) {
            LOG("[!] Game is OPEN (pid %d) and you asked to unload it.\n", pidJogo);
            LOG("[!] If you need to load the donor now, close PUBG first: this is the most noticeable moment.\n");
        }
    }

    LOG("[*] unloading the driver (kdmapper doesn't do its own)...\n");

    // 1) para a thread de memoria
    GIsRunning = false;

    // 2) espera o init terminar, SE ele estiver rodando.
    if (GInitRunning.load(std::memory_order_relaxed)) {
        int espera = 0;
        while (!GInitDone.load(std::memory_order_relaxed) && espera < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            espera++;
        }
    }

    uint64_t imgBase = 0, imgSize = 0;

    // O Cleanup do backend faz o handshake de parada (KMP_MAGIC_STOP) e espera
    // `Hdr.Ativo == 0`. A base vem do driver; o tamanho, do PE embutido.
    // -----------------------------------------------------------------------
    // O TAMANHO CERTO E' O QUE FOI ALOCADO, NAO O SizeOfImage DO PE.
    //
    // O mapper mapeia a imagem SEM os headers:
    //     image_size = SizeOfImage - SizeOfHeaders
    // e aloca exatamente isso. Liberar SizeOfImage e' liberar uma pagina a
    // mais que o alocado  e o gerenciador de memoria cobra com
    // PFN_LIST_CORRUPT (0x4E). Medido em 20/10/2026, no primeiro teste que
    // usou o tamanho corrigido (antes era 0x400, que so' vazava).
    //
    // Fonte primaria: o proprio mapper registra base e tamanho no momento
    // da alocacao. O PE fica de reserva para o caso de o mapa ter sido
    // feito por outro caminho.
    // -----------------------------------------------------------------------
    imgBase = TargetProcess.driver.KeventImageBase();
    if (imgBase == 0) imgBase = (uint64_t)DriverKeventAllocBase();
    imgSize = (uint64_t)DriverKeventAllocSize();
    if (imgSize == 0) {
        const uint64_t s = (uint64_t)DriverKeventImageSize();
        imgSize = (s > 0x1000) ? (s - 0x1000) : s;   // desconta SizeOfHeaders
    }
    TargetProcess.driver.Cleanup();
    // O keyread de kernel some com o driver: sem limpar o provider, o ultimo
    // mask publicado (com o bit ALT/SHIFT congelado "down" e a flag KROK)
    // continuaria dizendo que o modificador esta' pressionado para sempre.
    HotkeyHook::SetKernelProvider(nullptr, nullptr);

    // -----------------------------------------------------------------------
    // SEM CONFIRMACAO DE PARADA, NAO SE LIBERA PAGINA.
    //
    // `Hdr.Ativo == 0` e o driver dizendo "o servidor morreu". Se o timeout de
    // 2 s estourar, a thread pode ainda estar DENTRO do codigo  e o
    // `MmFreeIndependentPages` logo abaixo desmonta paginas em uso. O sintoma e
    // KMODE_EXCEPTION_NOT_HANDLED com o IP na imagem que acabou de sumir.
    //
    // Custo de NAO liberar: ~19 KB residentes e inertes (sem device, sem canal,
    // ninguem fala com ela ate o reboot). Custo de liberar sem confirmacao:
    // tela azul.
    // -----------------------------------------------------------------------
    if (!TargetProcess.driver.KeventParadaOk()) {
		LOG("[!] The KEVENT server did NOT confirm the shutdown within 2 s (Active != 0).\n");
		LOG("[!] The pages will NOT be released: if the thread is still alive, releasing them would crash the machine.\n");
		LOG("[!] The map record remains on disk: it will be released upon the next start, once the game is closed.\n");
        return;
    }
    LOG("[+] KEVENT server stopped and CONFIRMED (Active=0)\n");

    // -----------------------------------------------------------------------
    // LAZY UNMAP: com o jogo ABERTO as paginas ficam onde estao e a base vai
    // para o state file  liberar agora exigiria carregar o doador com o AC
    // ativo (o momento mais observado). A libera acontece no proximo start
    // com jogo fechado (TentarLiberarPaginasResiduas). Sem o bootId la'
    // dentro isso seria um bugcheck de graa apos reboot.
    // -----------------------------------------------------------------------
    if (imgBase != 0 && imgSize != 0 && ProcessoExiste(ProcessName.c_str())) {
        GravarPaginasResiduas(imgBase, imgSize);
        return;
    }

    if (imgBase == 0 || imgSize == 0) {
        LOG("[!] base/KEVENT sizes unavailable (base=0x%llX size=0x%llX)\n",
            (unsigned long long)imgBase, (unsigned long long)imgSize);
        LOG("[!] The pages will NOT be released; the code remains resident and inert.\n");
        return;
    }
    // 4) AS PAGINAS  agora e' o MAPPER que libera, e nao o driver.
    //
    // ==================== POR QUE A RESPONSABILIDADE MUDOU ====================
    //
    // Na 2.0 o driver se liberava sozinho, resolvendo o MmFreeIndependentPages
    // por VARREDURA DE ASSINATURA no ntoskrnl. O He apertou END e levou
    // KERNEL_SECURITY_CHECK_FAILURE: se o padrao casa no lugar errado, chama-se
    // uma funcao qualquer do kernel com a assinatura errada.
    //
    // O `intel_driver::MmFreeIndependentPages` (user-mode) JA' EXISTIA, foi
    // escrito por quem testou, e chama a funcao pela "call gadget" do driver
    // doador  caminho conhecido, em uso neste projeto desde a 1.1. Nao havia
    // motivo NENHUM para duplicar isso dentro do kernel.
    //
    // O mapper precisa do driver doador carregado para ter o gadget, entao:
    //
    //     intel_driver::Load()                 -> carrega o doador (ja' testado)
    //     MmFreeIndependentPages(base, size)   -> a liberacao
    //     intel_driver::Unload()               -> tira o doador
    //
    // Se `imgBase`/`imgSize` vierem zero (mapper sem passAllocationPtr, ou
    // driver anterior a 2.1), a gente NAO tenta e diz isso no log. O device ja'
    // saiu do namespace; o codigo fica residente e INERTE. E' o modo degradado,
    // e ele e' seguro  sem device e sem symlink, nao ha' como falar com ele.
    // -----------------------------------------------------------------------
    if (imgBase == 0 || imgSize == 0) {
		LOG("[!] The driver did not report a base/size (GET_IMAGE_INFO returned 0).\n");
		LOG("[!] The pages will NOT be released; the code remains resident and inert.\n");
		LOG("[!] Harmless: no device, no symlink, and it does not respond to anything.\n");
        return;
    }

    LOG("[*] releasing the pages via the mapper (base=0x%llX size=0x%llX)...\n",
        (unsigned long long)imgBase, (unsigned long long)imgSize);

    if (!NT_SUCCESS(intel_driver::Load())) {
		LOG("[-] Failed to load the donor driver to release the pages.\n");
		LOG("[-] The registration remains: will retry on the next startup with the game closed.\n");
        return;
    }

    {
        const bool ok = intel_driver::MmFreeIndependentPages(imgBase, (uint32_t)imgSize);

        if (ok) {
            ApagarRegistroPaginasResiduas();
            LOG("[+] PAGES UNLOCKED by the mapper (0x%llX, 0x%llX bytes)\n",
                (unsigned long long)imgBase, (unsigned long long)imgSize);
        } else {
            LOG("[-] MmFreeIndependentPages Failed. The record remains for the next attempt.\n");
        }
    }

    intel_driver::Unload();
}

// ===========================================================================
// GARANTIR SEED ??o nome do device deriva DELE.
//
// BUG QUE ISTO CORRIGE (medido no loader, 18/10/2026): o seed era sorteado
// dentro do `InitializeFeaturesThread`, e o LOADER nao passa mais por la' ??// `GetSeed()` devolvia 0, o device era derivado de zero, e o mapeamento
// "sumia" sem mensagem: o driver recusa seed invalido e o device nunca nasce.
// O log parava em "anti-cheat limpo ??mapeando agora" e nada mais.
//
// Agora quem mapeia chama ISTO antes. Idempotente: se ja' existe seed nesta
// sessao, nao sorteia outro (o nome do device nao pode mudar no meio).
// ===========================================================================
static void GarantirSeed()
{
	if (TargetProcess.driver.GetSeed() != 0) return;

	LARGE_INTEGER freq{}, cont{};
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&cont);

	uint64_t s = (uint64_t)cont.QuadPart
	           ^ ((uint64_t)GetCurrentProcessId() << 32)
	           ^ (uint64_t)GetCurrentThreadId()
	           ^ 0x9E3779B97F4A7C15ULL;

	// seed degenerado derrubaria a derivacao (o driver devolve
	// STATUS_INVALID_PARAMETER). Um ajuste barato fecha essa porta.
	if (s == 0 || s == ~0ULL) s ^= freq.QuadPart | 1ULL;

	TargetProcess.driver.SetSeed(s);
	LOG("[*] session seed: 0x%016llX\n", (unsigned long long)s);
	// O nome do device sai do seed. Imprimir aqui e' o que permite usar o
	// probe_driver.ps1 / TestHarness.exe contra ESTA sessao.
	LOG("[*] session device: %s\n", DeviceNameAtual(s).c_str());
}


void InitializeFeaturesThread() {
	GarantirSeed();   // o nome do device deriva daqui (ver GarantirSeed)
	// -----------------------------------------------------------------------
	// ORDEM FINAL (hardening-p3)  gate ANTES de esperar o jogo.
	//
	//   1. gate de anti-cheat (com o jogo fechado passa de primeira)
	//   2. mapeia o driver (janela otima: o AC nem existe ainda)
	//   3. so' AGORA espera o TslGame.exe aparecer
	//
	// O raciocinio anterior era o oposto (esperar o jogo, esperar o BE sair,
	// mapear). Ele se apoiava num callback de imagem que veria qualquer carga
	// nova apos o boot  mas a imagem mapeada A MAO nao entra em
	// PsLoadedModuleList: o snapshot de modulos nao a ve, e callback de imagem
	// so' dispara em cargas novas. Logo, mapear ANTES do jogo abrir e' a
	// janela MENOS observavel que existe: o BE sobe depois da imagem ja' estar
	// la', como se fosse do kernel desde sempre.
	//
	// Este e' o MESMO desenho do loader (CarregarDriverThread): um fluxo unico
	// em todo o app  gate -> subir -> esperar o jogo.
	// -----------------------------------------------------------------------

	// Auto-loader do driver embutido  MAS SO' DEPOIS DO GATE.
	if (!TargetProcess.driver.Initialize()) {
		LOG("[*] Driver absent. Will map after the gate..\n");
	} else if (!TargetProcess.driver.DriverVersionOk()) {
		// -------------------------------------------------------------------
		// VERSAO ERRADA -> FECHAR O HANDLE E MAPEAR ESTA BUILD.
		//
		// ARMADILHA QUE ISTO CONSERTA (custou 3 rodadas de diagnostico):
		//
		//   O kdmapper NAO descarrega o driver que mapeou. Entao, depois de
		//   qualquer mudanca no driver, a proxima execucao encontra o driver
		//   ANTIGO ainda vivo, o Initialize() acima devolve true, e o mapeamento
		//   do embutido NUNCA ACONTECE. O cheat roda contra o driver velho.
		//
		//   Em silencio. O PING respondia (as duas versoes tem PING), o device
		//   abria, e so' o IOCTL novo falhava com err=1. Parecia bug no IOCTL.
		//
		// Na 1.2 o nome do device sai do SEED, entao a carga nova nao colide com
		// a antiga: basta fechar o nosso handle e mapear. O handle TEM que
		// fechar  um handle aberto fica preso ao device velho e nao segue o
		// symlink novo.
		// -------------------------------------------------------------------
		LOG("[!] driver loaded = 0x%08X, expected = 0x%08X\n",
		    TargetProcess.driver.driverVersion, KM_DRIVER_VERSION_ESPERADA);
		TargetProcess.driver.Cleanup();
	} else {
		LOG("[+] existing driver is from this build (version 0x%08X)\n",
		    TargetProcess.driver.driverVersion);
	}

	// -----------------------------------------------------------------------
	// O MAPEAMENTO E O GATE  na ordem nova.
	//
	// Se ja' existe driver desta build, nao ha' nada a mapear e o gate nao
	// precisa rodar: ele seria uma espera sem efeito.
	// -----------------------------------------------------------------------
	if (!TargetProcess.driver.DriverVersionOk()) {

		// ---- 1) gate de anti-cheat ANTES de esperar o jogo ----
		// Com o jogo fechado o BE nem esta' carregado: o gate costuma passar
		// na primeira volta (ver a razao no bloco do topo desta funcao).
		if (GateAntiCheat(30)) {

			// ---- 2) sobe o transporte KEVENT (unico caminho) ----
			LOG("[*] loading the KEVENT transport (seed 0x%llX)...\n",
			    (unsigned long long)TargetProcess.driver.GetSeed());
			if (!SubirDriverKevent("init")) {
				LOG("[-] no transport, no ESP. Gate still spinning.\n");
			}
		}
	}

	LOG("[*] Waiting for %s to open...\n", ProcessName.c_str());

	int waitCount = 0;
	while (GIsRunning) {
		if (TargetProcess.Init(ProcessName, true, false)) {
			LOG("[+] %s Found! Initiating memory routines.\n", ProcessName.c_str());
			break;
		}
		if (waitCount % 5 == 0) {
			LOG("[*] Process not found, waiting...\n");
		}
		waitCount++;
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	if (!GIsRunning) { GInitRunning.store(false, std::memory_order_relaxed); return; }

	// ProbeReadable tem retry: nao pode travar a UI, por isso roda aqui fora.
	//
	// silencioso=true: o sucesso do retry nao entra no log. Sem isso, o
	// logs.txt enchia de centenas de "leitura ok, base tem assinatura MZ".
	{
		int tentativas = 0;
		while (GIsRunning && !TargetProcess.ProbeReadable(true)) {
			if (++tentativas == 1) {
				LOG("[*] waiting for the base address to become readable (retry 3s)...\n");
			}
			std::this_thread::sleep_for(std::chrono::seconds(3));
		}
		if (tentativas > 0 && GIsRunning) {
			LOG("[+] base address readable after %d attempt(s).\n", tentativas);
		}
	}
	if (!GIsRunning) { GInitRunning.store(false, std::memory_order_relaxed); return; }

	EngineInstance = std::make_shared<Engine>();

	GInitRunning.store(false, std::memory_order_relaxed);
	GInitDone.store(true, std::memory_order_relaxed);
	LOG("[+] Engine loaded. Awaiting UWorld synchronization via Memory Loop....\n");

	std::thread memThread(MemoryLoop);
	memThread.detach();
}

// ===========================================================================
// SESSAO  CARREGAR / DESCARREGAR / INICIAR
//
// Tres acoes distintas, cada uma numa thread porque todas bloqueiam:
//   CarregarDriverThread    gate de anti-cheat + mapeamento (30 x 10 s no pior
//                           caso: nao pode travar a UI)
//   IniciarSessaoThread     espera o TslGame.exe aparecer (500 ms) e so''
//                           entao abre o processo, sobe a Engine e a
//                           MemoryLoop
//   DescarregarDriverThread para a sessao (se houver) e faz o UNMAP
//
// `PararSessao` e' o caminho do END: derruba a sessao e volta pro LOADER, SEM
// tocar no driver  o unmap e' decisao explicita do botao.
// ===========================================================================
// ------------------------------------------------------------------
// CONTADORES DE INJECAO NO FIM DE SESSAO (diagnostico permanente).
//
// A proxima vez que o aimbot "parar de funcionar", a primeira pergunta e'
// "o mouse foi injetado por kernel ou caiu pro SendInput?"  e a resposta
// nao fica em lugar nenhum: o SendInput e' o caminho mais visivel dos tres
// (todo o input user-mode passa por um ponto unico e observavel). Logar a
// proporcao ok/fail no fim de cada sessao vira o indicador permanente de
// degradacao: fail > 0 em sessao real e' anomalia a explicar, nao ruido.
// ------------------------------------------------------------------
static void LogarContagemInjecao(const char* origem)
{
	const uint64_t ok = g_mouseInjOk.load(std::memory_order_relaxed);
	const uint64_t fail = g_mouseInjFail.load(std::memory_order_relaxed);
	LOG("[stats] %s: kernel mouse injection ok=%llu | fail(SendInput)=%llu | lastErr=%lu\n",
		origem, (unsigned long long)ok, (unsigned long long)fail,
		(unsigned long)g_mouseInjLastErr.load(std::memory_order_relaxed));
}

static void PararSessao()
{
	if (GFase.load() == FASE_LOADER) return;

	LoaderStatus("session ending...");
	GIsRunning = false;

	// Espera a MemoryLoop sair (ela le' com device aberto; derrubar a Engine
	// com a thread viva seria use-after-free). 3 s de teto.
	for (int i = 0; i < 60 && GMemThreadViva.load(); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(550));

	// NAO fecha o menu: o LOADER vem em seguida e e' desenhado na MESMA janela,
	// que precisa continuar clicavel (ver DesenharLoader).

	EngineInstance.reset();
	GInitDone.store(false);
	GFase = FASE_LOADER;
	LoaderStatus("session ended - driver still loaded");
}

static void CarregarDriverThread()
{
	GSessaoOcupada.store(true);

	// =====================================================================
	// FIX: GIsRunning reset karo — pehle session ne false set kiya tha
	//
	// BUG: UNLOAD ke baad PararSessao() ne GIsRunning = false set kiya.
	// Ab 2nd LOAD pe GateAntiCheat(30) ka `for (... && GIsRunning; ...)` loop
	// turant exit ho jata hai (GIsRunning false hai), gate false return karta
	// hai, aur "AntiCheat is watching you - Mapping Abort" dikhta hai.
	//
	// Loader process RUN kar raha hai (UI responsive hai, thread spawn ho raha
	// hai), matlab GIsRunning = true hona chahiye. FALSE sirf session ke andar
	// hona chahiye jab MemoryLoop ko terminate karna ho.
	// =====================================================================
	GIsRunning = true;

	LoaderStatus("Loading the driver Wait...");
	// NAYA: UI ko 300ms do yellow status dikhane ke liye
	std::this_thread::sleep_for(std::chrono::milliseconds(2200));

	TentarLiberarPaginasResiduas("loader");
	GarantirSeed();

	LoaderStatus("Checking AntiCheat...");
	if (!GateAntiCheat(10)) {
		LOG("[-] loader: AntiCheat is Active - Mapping Abort\n");
		LoaderStatus("AntiCheat is Watching You ~ Better to Restart Pc");
		GSessaoOcupada.store(false);
		return;
	}

	LoaderStatus("mapping driver...");
	LOG("[*] loader: mapping (seed 0x%llX)...\n", (unsigned long long)TargetProcess.driver.GetSeed());

	// CADA ETAPA LOGADA SEPARADAMENTE. "falha ao mapear" sozinho nao diz se o
	// kdmapper nao rodou, se o device nao abriu (carga anterior viva) ou se a
	// versao respondeu outra  e' o mesmo diagnostico que a aba DEBUG dava.
	// -------------------------------------------------------------------
	// SELECAO DE BACKEND  por variavel de ambiente, para o teste A/B.
	//
	//   set MTMACR0_BACKEND_KEVENT=1
	//   mtmacr0.exe
	//
	// Sem a variavel, o caminho e' o de sempre (IOCTL). Com ela, o cheat usa o
	// transporte de shared memory + KEVENT. ESCOPO: ESP puro  o driver de
	// KEVENT nao tem injecao de mouse, entao aimbot e RCS nao vao funcionar.
	// -------------------------------------------------------------------
	// -----------------------------------------------------------------------
	// UM TRANSPORTE SO (full KEVENT, 20/10/2026).
	//
	// O IOCTL foi removido: sem device, sem symbolic link, sem IRP. O switch de
	// backend e a variavel MTMACR0_BACKEND_KEVENT deixaram de existir porque nao
	// ha mais o que escolher.
	// -----------------------------------------------------------------------
	LoaderStatus("mapping driver...");
	if (!SubirDriverKevent("loader")) {
		LoaderStatus("KEVENT failure - check logs.txt");
		GSessaoOcupada.store(false);
		return;
	}

	// Agar driver waqai load hua hai to success message
	if (TargetProcess.driver.DriverVersionOk()) {
		LOG("[+] driver mapped by loader (0x%08X)\n", TargetProcess.driver.driverVersion);
		LoaderStatus("Driver Loaded and Mapped Successfully");
	}
	else {
		LOG("[-] driver mapping FAILED - version check failed\n");
		LoaderStatus("driver mapping FAILED - check logs");
	}
	

	// -----------------------------------------------------------------------
	// MODO FUMAA (MTMACR0_KEVENT_SMOKE=1)  TESTA O CICLO DE VIDA SEM O JOGO.
	//
	// Depois de um 0x1E no load, a pergunta "quem estourou?" tem trs respostas
	// possveis e todas moram em lugares diferentes: o boot do driver (seed,
	// rendezvous), o transporte (leitura) ou o mouse. Rodar direto no jogo
	// testa os trs de uma vez e, se cair, no diz qual foi. warka dung
	//
	// Este modo faz o mnimo: mapeia, abre o canal, l a verso, PARA o servidor
	// e libera as pginas  sem jogo, sem leitura de memria alheia, sem mouse.
	//
	//   caiu aqui          ->  o ciclo de vida (boot ou parada)
	//   passou             -> o transporte est de p; o problema  READ ou MOUSE
	//   com NO_KERNEL_MOUSE -> separa READ de MOUSE
	// -----------------------------------------------------------------------
	{
		char* v = nullptr; size_t n = 0;
		const bool fumaca = (_dupenv_s(&v, &n, "MTMACR0_KEVENT_SMOKE") == 0 && v != nullptr && v[0] == '1');
		if (v != nullptr) free(v);

		if (fumaca) {
			LogarExposicao();
			LOG("[smoke] Life cycle OK up to this point: open channel, version 0x%08X\n",
			    TargetProcess.driver.driverVersion);
			LOG("[smoke] stopping the server and releasing the pages (no game, no reading, no mouse)...\n");
			DescarregarDriver();
			LogarContagemInjecao("smoke");
			LOG("[smoke] END: if the machine has reached this point without a bugcheck, its lifecycle is in good order.\n");
			LoaderStatus("[smoke] ok - lifecycle validated");
			GSessaoOcupada.store(false);
			return;
		}
	}
	
	GSessaoOcupada.store(false);
}

static void DescarregarDriverThread()
{
	GSessaoOcupada.store(true);
	LoaderStatus("unloading the driver wait...");
	PararSessao();
	DescarregarDriver();

	// Ye choti si wait driver handle ko properly close hone ka time deti hai
	// Bina iske, UI frame me driverOk abhi bhi true aa sakta hai
	std::this_thread::sleep_for(std::chrono::milliseconds(2000));

	// Double-check: agar driver abhi bhi alive hai to dobara cleanup karo
	if (TargetProcess.driver.DriverVersionOk()) {
		TargetProcess.driver.Cleanup();
	}

	if (!TargetProcess.driver.DriverVersionOk()) {
		LoaderStatus("driver unloaded successfully (pages free)");
	}
	else {
		LoaderStatus("unload failed - driver still active");
	}
	GSessaoOcupada.store(false);
}

static void IniciarSessaoThread(bool carregarDriver)
{
	GSessaoOcupada.store(true);
	GIsRunning = true;
	GInitRunning.store(true, std::memory_order_relaxed);

	// Sem seed nao existe nome de device para abrir (ver GarantirSeed).
	if (carregarDriver) GarantirSeed();


	if (carregarDriver && !TargetProcess.driver.DriverVersionOk()) {
		LoaderStatus("Checking AntiCheat wait...");
		if (!GateAntiCheat(30)) {
			LoaderStatus("anti-cheat active - mapping aborted");
			GFase = FASE_LOADER; GInitRunning.store(false); GSessaoOcupada.store(false);
			return;
		}
		LoaderStatus("loading the KEVENT transport...");
		if (!SubirDriverKevent("session")) {
			LoaderStatus("failed to start KEVENT - check logs.txt");
			GFase = FASE_LOADER; GInitRunning.store(false); GSessaoOcupada.store(false);
			return;
		}
		LOG("[+] KEVENT transport active via session (0x%08X)\n", TargetProcess.driver.driverVersion);
	}

	// =====================================================================
// STEAM + PUBG AUTO-LAUNCH (naya flow)
// Pehle Steam check/launch, phir steam:// protocol se PUBG launch,
// phir TslGame.exe ka wait.
// Cancel click se har step check karta hai (GIsRunning).
// =====================================================================
	if (!AguardarSteamELancarPubg()) {
		// Steam/PUBG launch fail hua ya user ne cancel kiya
		if (GIsRunning) {
			// Actually fail hua (cancel nahi)
			GFase = FASE_LOADER;
		}
		GInitRunning.store(false);
		GSessaoOcupada.store(false);
		return;
	}

	// Agar user ne beech me CANCEL kiya to abort
	if (!GIsRunning || GFase.load() == FASE_LOADER) {
		GInitRunning.store(false);
		GSessaoOcupada.store(false);
		return;
	}

	// =====================================================================
	// Ab TslGame.exe ka wait (yeh purana loop hi hai, messages thodi alag)
	// =====================================================================
	LoaderStatus("Waiting for process TslGame.exe...");
	{
		// Pubg game launch hota hai slow — har 5s me message change
		static const char* kPubgMsgs[] = {
			"PUBG is loading, please wait :)",
			"Loading game assets, do not click anywhere...",
			"Game is almost ready, wait patiently ....",
		};
		int idx = 0;
		int tickCount = 0;
		while (GIsRunning && GFase.load() == FASE_AGUARDANDO &&
			!ProcessoExiste(ProcessName.c_str())) {
			if (tickCount % 5 == 0) {   // har 5 x 1600ms = 8s
				LoaderStatus("%s", kPubgMsgs[idx % 3]);
				idx++;
			}
			tickCount++;
			std::this_thread::sleep_for(std::chrono::milliseconds(1600));
		}
	}

	if (!GIsRunning || GFase.load() == FASE_LOADER) {
		GInitRunning.store(false); GSessaoOcupada.store(false); return;
	}

	// =====================================================================
// Process attach + readable base — lobby messages (naya, sirf status)
// =====================================================================
	{
		static const char* kInitMsgs[] = {
			"Attaching to game process...",
			"Reading game memory base...",
			"Waiting for lobby synchronization ....",
		};
		int idx = 0;

		LoaderStatus(kInitMsgs[0]);
		while (GIsRunning && !TargetProcess.Init(ProcessName, true, false)) {
			LoaderStatus("%s", kInitMsgs[idx % 3]);
			idx++;
			std::this_thread::sleep_for(std::chrono::seconds(2));
		}

		LoaderStatus(kInitMsgs[1]);
		while (GIsRunning && !TargetProcess.ProbeReadable(true)) {
			LoaderStatus("%s", kInitMsgs[idx % 3]);
			idx++;
			std::this_thread::sleep_for(std::chrono::seconds(2));
		}
	}

	if (!GIsRunning) {
		GInitRunning.store(false); GSessaoOcupada.store(false); GFase = FASE_LOADER; return;
	}

	if (!GIsRunning || GFase.load() == FASE_LOADER) {
		GInitRunning.store(false); GSessaoOcupada.store(false); return;
	}

	LoaderStatus("loading engine...");
	EngineInstance = std::make_shared<Engine>();
	GInitDone.store(true, std::memory_order_relaxed);

	std::thread(MemoryLoop).detach();

	GFase = FASE_RODANDO;
	if (!DirectOverlay::ShowMenu) DirectOverlay::ToggleMenu();   // abre o menu
	LOG("[+] session active\n");
	LoaderStatus("session active");
	GInitRunning.store(false, std::memory_order_relaxed);
		
	GSessaoOcupada.store(false);
}

// ------------------------------------------------------------------
// FIM DE EXECUCAO: LOG TRUNCADO PARA O TAIL (~4 KB).
//
// O logs.txt e' truncado no COMECO de cada execucao mesmo (ver o bloco
// la' embaixo no Run)  este tail existe para os casos em que a proxima
// execucao NUNCA aconteceu: crash, queda de energia, o usuario fechar a VM.
// Sem ele, o log guardaria a sessao inteira; com ele, sobeem as ultimas
// linhas  que sao exatamente as que interessam no pos-mortem.
// ------------------------------------------------------------------
static void ManterTailDoLog(size_t maxBytes)
{
	std::ifstream f(CaminhoDoLog(), std::ios::binary);
	if (!f) return;
	f.seekg(0, std::ios::end);
	const std::streamoff total = f.tellg();
	if (total <= (std::streamoff)maxBytes) return;
	f.seekg(total - (std::streamoff)maxBytes);
	std::string resto((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	f.close();
	// Se cortamos no meio de uma linha, descarta o fragmento ate o \n cheio:
	const size_t nl = resto.find('\n');
	if (nl != std::string::npos && nl + 1 < resto.size()) resto.erase(0, nl + 1);
	std::ofstream out(CaminhoDoLog(), std::ios::trunc | std::ios::binary);
	if (out) out << resto;
}

int Run() {
	SetUnhandledExceptionFilter(CrashHandler);

	// -----------------------------------------------------------------
	// O DIAGNOSTICO DO MAPPER PASSA A ENTRAR NO ARQUIVO.
	//
	// `kdmLog` (o logger do kdmapper) ia para `std::wcout`. Este e' um app de
	// JANELA: sem console, tudo que o mapper tinha a dizer era descartado 
	// inclusive "Can't exploit intel driver, is there any antivirus or
	// anticheat running?", que e' exatamente a resposta que se procura quando
	// o mapeamento falha.
	//
	// Com o sink registrado, cada linha do mapper vira uma linha `[kdm]` no
	// logs.txt. Sem sink (OffsetDumper) o comportamento antigo continua.
	// -----------------------------------------------------------------
	kdmUtils::g_logSink = [](const wchar_t* s) {
		if (!s || !*s) return;
		LOG("[kdm] %ls", s);
	};

	// -----------------------------------------------------------------
	// Log SEMPRE limpo a cada execucao.
	//
	// Antes o logs.txt era so' append, entao o diagnostico de hoje se
	// misturava com o de ontem e ficava impossivel isolar um teste. Agora
	// cada execucao comeca do zero (o BaseLog continua em append, mas o
	// arquivo ja' nasce truncado).
	//
	// A PRIMEIRA LINHA DIZ ONDE ESTE ARQUIVO ESTA'. Existe porque o caminho
	// tem fallback (ver CaminhoDoLog) e porque "nao achei o log" era um dos
	// sintomas: agora o proprio log responde onde ele mora.
	// -----------------------------------------------------------------
	{
		const std::string caminho = CaminhoDoLog().string();
		std::ofstream fresh(CaminhoDoLog(), std::ios::trunc);
		if (fresh.is_open()) {
			fresh << "[*] loading driver...\n";
			fresh << "[*] log em: " << caminho << "\n";
			fresh.close();
		}
		// O `ArquivoDeLog()` e' aberto de forma preguicosa no primeiro LOG; se
		// ELE tambem falhar, o BaseLog escreve no vazio. Nao da' para fazer
		// mais nada aqui sem console  mas pelo menos a falha do trunc acima
		// significa "nenhum candidato era gravavel", e isso e' ambiente, nao
		// codigo.
	}

	// -----------------------------------------------------------------
	// Console de diagnostico — SO quando pedido explicitamente
	//
	// Uma janela de console preta com "[*] aguardando TslGame.exe" e um
	// marcador de cheat que qualquer anti-cheat ve. Por isso o console e
	// desligado por padrao e tudo vai pro logs.txt.
	//
	// Para testar, ligue com a variavel de ambiente:
	//
	//     set PUBGEXT_CONSOLE=1
	//     RuntimeBroker.exe
	//
	// Sem a variavel, comportamento normal: nenhuma janela, nada em stdout.
	// Assim nao existe risco de esquecer o console ligado numa sessao real.
	// -----------------------------------------------------------------
	{
		char buf[8] = { 0 };
		const DWORD n = GetEnvironmentVariableA("PUBGEXT_CONSOLE", buf, sizeof(buf));
		if (n > 0 && buf[0] == '1') {
			// So aloca console se os handles de std ainda NAO existirem.
			//
			// App de subsystem GUI lancado normalmente tem os tres em NULL,
			// entao AllocConsole e o certo. Mas se alguem ja redirecionou o
			// stdout (ex.: `app.exe > out.txt`), o handle e valido e apontar
			// ele pro CONOUT$ de um console recem-criado quebra o processo.
			const HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
			const HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
			const bool jaTemSaida =
				(hOut && hOut != INVALID_HANDLE_VALUE) ||
				(hErr && hErr != INVALID_HANDLE_VALUE);

			if (!jaTemSaida && AllocConsole()) {
				FILE* fDummy = nullptr;
				freopen_s(&fDummy, "CONIN$",  "r", stdin);
				freopen_s(&fDummy, "CONOUT$", "w", stdout);
				freopen_s(&fDummy, "CONOUT$", "w", stderr);
				SetConsoleTitleA("PubgExt - diagnostico (feche para sair)");
				SetConsoleOutputCP(CP_UTF8);   // o log tem acentos
				// A PARTIR DAQUI o BaseLog espelha em stdout. Sem isto o
				// `printf` dele rodava num processo sem console nenhum 
				// chamada de sistema por linha, para escrever no vazio.
				GConsoleAtivo = true;
				printf("[console] PUBGEXT_CONSOLE=1 - mirrored output here\n");
				fflush(stdout);
			}
		}
	}

	timeBeginPeriod(1);


	LOG("[*] Setup configuration loader...\n");
	SetUpConfig();

	// Paginas residentes de uma execucao anterior (lazy unmap): liberar o mais
	// cedo possivel, com o jogo fechado  ANTES do overlay existir. Na maioria
	// dos starts o state file nem existe e isto custa uma stat.
	TentarLiberarPaginasResiduas("start");

#if MENU_SO_ESP
	// BUILD PUBLICO = SO' ESP. As flags sao forcadas DEPOIS do config, senao
	// um JSON antigo com "Enabled": true religaria o aimbot nesta variante.
	// O `Aimbot::Tick` tambem nao e' chamado (ver MemoryLoop).
	Configs.Aimbot.Enabled = false;
	Configs.Aimbot.RCS     = false;
#endif

	// -----------------------------------------------------------------------
	// IDIOMA  aplica o que veio do config ANTES de desenhar qualquer coisa.
	//
	// Tem que ser aqui, e nao dentro do RenderFrame: o menu, o HUD e os rotulos
	// do ESP passam por `L(...)`, e sem esta chamada o primeiro frame sairia no
	// idioma padrao mesmo com o config em ingles.
	// -----------------------------------------------------------------------
	Lang::Set(Configs.Survivor.Language);

	LOG("[*] Ready | Menu Key: HOME\n");

	// -----------------------------------------------------------------------
	// MODO MAP-ONLY  carrega o driver e SAI.
	//
	//     set PUBGEXT_MAP_ONLY=1
	//
	// Existe por um motivo especifico: o harness de validacao do driver
	// (tools\TestHarness.cpp) precisa do SEED da sessao, e o seed e' sorteado
	// aqui dentro. Sem este modo, a unica forma de obter um seed e' rodar o
	// cheat inteiro  overlay, espera do jogo, tudo  so' pra ler uma linha do
	// log.
	//
	// Com ele: o driver e' mapeado (com o gate de anti-cheat), o seed e o nome
	// do device vao pro log, e o processo termina sem abrir overlay nenhum.
	// Isso e' o que torna o teste na VM um comando so'.
	//
	// O gate NAO e' pulado: se o anti-cheat estiver ativo, o modo map-only
	// espera igual. Ele nao existe para contornar a politica  existe para
	// separar "carregar o driver" de "abrir o cheat".
	// -----------------------------------------------------------------------
	{
		char mapOnly[8] = { 0 };
		size_t n = 0;
		if (getenv_s(&n, mapOnly, sizeof(mapOnly), "PUBGEXT_MAP_ONLY") == 0 && n > 0 && mapOnly[0] == '1') {
			LOG("[*] PUBGEXT_MAP_ONLY=1 -> loading the driver and exiting (without overlay)\n");

			std::thread bootstrap(InitializeFeaturesThread);
			bootstrap.detach();

			// InitializeFeaturesThread mapeia e depois ENTRA no laco de espera
			// do TslGame.exe. Aqui a gente espera so' o driver aparecer e sai 
			// sem jogo, o laco nao terminaria nunca.
			{
				int espera = 0;
				while (espera < 120) {
					if (true &&
					    TargetProcess.driver.DriverVersionOk()) {
						LOG("[+] active driver (0x%08X) map-only mode completed\n",
						    TargetProcess.driver.driverVersion);
						break;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(500));
					espera++;
				}
				if (espera >= 120)
					LOG("[-] map-only: the driver did not become ready within 60s\n");
			}

			// Encerra o processo. A thread do bootstrap morre junto; o driver
			// fica carregado no kernel, que e' o objetivo.
			LOG("[*] exiting (the driver remains loaded)\n");
			GIsRunning = false;
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			timeEndPeriod(1);
			return 0;
		}
	}

	LOG("[*] Initializing direct transparent overlay...\n");
	// -----------------------------------------------------------------------
	// .ini DA UI  o ImGui so' aceita um caminho, entao ele vem daqui.
	//
	// Sem esta linha o ImGui grava `imgui.ini` ao lado do .exe (o padrao dele
	// e' relativo ao diretorio de trabalho). Ver o comentario em
	// DirectOverlay::Run, onde o valor e' aplicado.
	// -----------------------------------------------------------------------
	{
		std::filesystem::path pasta = PastaDeArtefatos();
		if (!pasta.empty()) {
			std::error_code ec;
			std::filesystem::create_directories(pasta, ec);
			DirectOverlay::UiIniPath = (pasta / "ui.bin").string();
		}
	}

	bool overlayOk = DirectOverlay::Run([&]() {
		Gui.Window.Size = ImVec2((float)DirectOverlay::ScreenWidth, (float)DirectOverlay::ScreenHeight);
		Gui.Window.hWnd = DirectOverlay::Overlay;

		// A INICIALIZACAO NAO E' MAIS AUTOMATICA.
		//
		// Era: no primeiro frame disparava `InitializeFeaturesThread`, que
		// esperava o jogo, mapeava o driver e subia a engine  sem nenhum
		// controle do usuario. Agora quem manda e' o LOADER: carregar o
		// driver, descarregar e iniciar a sessao sao TRES acoes separadas
		// (ver DesenharLoader).
		RenderFrame();
	});

	if (!overlayOk) {
		LOG("[-] Overlay cannot be created (D3D11 or window). Aborting.\n");
	}

	// -----------------------------------------------------------------------
	// O END CHEGA AQUI.
	//
	// O handler do VK_END (DirectOverlay.h) so' faz PostQuitMessage(0)  ele
	// derruba o laco de render, e o controle volta pra ESTA linha. Entao e' aqui
	// que o driver tem que sair do kernel, e nao dentro do handler da janela:
	// la' a gente estaria fazendo um SELFDESTRUCT (com sleeps de 150ms + 2s de
	// teto) dentro do WndProc, travando a fila de mensagens do processo.
	//
	// A ordem importa: ANTES do timeEndPeriod, porque o Unmap espera o kernel
	// com uma resolucao de timer de 1ms. Depois do timeEndPeriod o
	// Sleep(150) do meio da sequencia viraria um sleep grosseiro.
	// -----------------------------------------------------------------------
	GIsRunning = false;
	DescarregarDriver();
	ManterTailDoLog(4096);   // se vier crash/queda antes do proximo start, o log guarda o fim

	timeEndPeriod(1);
	return overlayOk ? 0 : 1;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
	return Run();
}
