#pragma once
// ===========================================================================
// IDIOMA DO CHEAT
//
// A CHAVE DA TABELA E' O PROPRIO TEXTO EM PORTUGUES. Isso foi escolha
// deliberada, e o motivo e' nao ter que reescrever ~140 call sites:
//
//     ImGui::Checkbox("Enable aimbot", &x);        ->  antes
//     ImGui::Checkbox(L("Enable aimbot"), &x);     ->  depois
//
// O `L()` traduz na hora do desenho. Se FALTAR traducao, o texto sai em
// portugues (fallback visivel) em vez de vazio — e a ferramenta
// `tools\check_lang.ps1` lista exatamente quais estao faltando, comparando o
// que esta' no codigo com o que esta' nesta tabela. "100% em ingles" passa a
// ser VERIFICAVEL em vez de esperanca.
//
// POR QUE A TABELA E' UM ARRAY E NAO UM `unordered_map` DIRETO: ela e' escrita
// e lida a mao; o mapa e' construido UMA vez, na primeira traducao, e fica
// estatico. O lookup e' O(1) e so' acontece com o idioma em ingles.
// ===========================================================================

#include "pch.h"
#include <cstring>
#include <string>
#include <unordered_map>

enum CheatLanguage : int
{
	LANG_PT_BR = 0,
	LANG_EN_US = 1,
	LANG_COUNT
};

namespace Lang {

	struct Par { const char* pt; const char* en; };

	// -----------------------------------------------------------------------
	// TABELA DE TRADUCAO
	//
	// Ordem: a mesma da extracao do codigo (alfabetica), para bater o olho e
	// conferir. Strings de FORMATO ("%d", "%.1f", "GNames: 0x%llX") NAO entram:
	// elas nao tem o que traduzir e o fallback ja' devolve elas mesmas.
	// -----------------------------------------------------------------------
	inline const Par kTabela[] = {
		// ---- abas / cabecalhos ----
		{ "Geral",                              "General" },
		{ "Visual",                             "Visuals" },
		{ "Jogador",                            "Player" },
		{ "Mundo",                              "World" },
		{ "Configuracao",                       "Configuration" },
		{ "Keybinds",                           "Keybinds" },
		{ "Performance",                        "Performance" },
		{ "Cores",                              "Colours" },
		{ "Estilo",                             "Style" },
		{ "Texto do ESP",                       "ESP text" },
		{ "Layout do ESP",                      "ESP layout" },
		{ "Espectadores",                       "Spectators" },
		{ "Visibilidade",                       "Visibility" },
		{ "Predicao",                           "Prediction" },
		{ "Recoil (RCS)",                       "Recoil (RCS)" },
		{ "RCS ao vivo",                        "RCS live" },
		{ "Smoothing / tracking",               "Smoothing / tracking" },
		{ "Catalogo",                           "Catalog" },

		// ---- controles ----
		{ "Enable aimbot",                      "Enable aimbot" },
		{ "Visibility check",                   "Visibility check" },
		{ "Auto-bone selection",                "Auto-bone selection" },
		{ "Lock-on target",                     "Lock-on target" },
		{ "Bone alvo",                          "Target bone" },
		{ "Hotkey",                             "Hotkey" },
		{ "Destravar eixo vertical (so' horizontal)", "Unlock vertical axis (horizontal only)" },
		{ "o pitch nunca e' puxado: aimbot e RCS so' corrigem yaw",
		  "pitch is never pulled: aimbot and RCS only correct yaw" },
		{ "Draw FOV circle",                    "Draw FOV circle" },
		{ "FOV",                                "FOV" },
		{ "AR smooth",                          "AR smooth" },
		{ "Sniper smooth",                      "Sniper smooth" },
		{ "Max delta (anti-snap)",              "Max delta (anti-snap)" },
		{ "Aggression",                         "Aggression" },
		{ "Driver scale",                       "Driver scale" },
		{ "Prediction (previsao balistica)",    "Prediction (ballistic lead)" },
		{ "Enable RCS (no-recoil)",             "Enable RCS (no-recoil)" },
		{ "Calibrate sensitivity",              "Calibrate sensitivity" },
		{ "Deg per count",                      "Deg per count" },
		{ "Vertical (pitch)",                   "Vertical (pitch)" },
		{ "Horizontal (yaw)",                   "Horizontal (yaw)" },
		{ "Max counts/tick",                    "Max counts/tick" },
		{ "Auto-calibrate",                     "Auto-calibrate" },
		{ "Invert direction",                   "Invert direction" },
		{ "Reset RCS state",                    "Reset RCS state" },
		{ "counts/grau %.1f    gain x%.2f",     "counts/deg %.1f    gain x%.2f" },
		{ "dreno  Y %.3f  R %.3f  ->  %.2f graus", "drain  Y %.3f  R %.3f  ->  %.2f deg" },
		{ "injetado  dx %d   dy %d",            "injected  dx %d   dy %d" },
		{ "mao  lat %.0f ms   ganho %.3f   passo %.2f   devendo %.2f   ruido %.2f",
		  "hand  lat %.0f ms   gain %.3f   step %.2f   owed %.2f   noise %.2f" },
		{ "residual da janela  %.3f graus",     "window residual  %.3f deg" },

		// ---- ESP / jogador ----
		{ "ESP Box",                            "ESP Box" },
		{ "Skeleton",                           "Skeleton" },
		{ "Health Bar",                         "Health bar" },
		{ "Nome",                               "Name" },
		{ "Nome da arma",                       "Weapon name" },
		{ "Distancia [m]",                      "Distance [m]" },
		{ "Time [S<n>]",                        "Team [S<n>]" },
		{ "BOT/Player [BOT]/[P]",               "Bot/Player [BOT]/[P]" },
		{ "Snaplines (linha ate' o inimigo)",   "Snaplines (line to the enemy)" },
		{ "a partir do centro",                 "from the centre" },
		{ "Loot ESP (master)",                  "Loot ESP (master)" },
		{ "Loot ESP",                           "Loot ESP" },
		{ "Veiculos",                           "Vehicles" },
		{ "Airdrop / caixa de suprimento",      "Airdrop / supply crate" },
		{ "Enable radar",                       "Enable radar" },
		{ "North-up (fixo no norte)",           "North-up (locked north)" },
		{ "Arrow offset (deg)",                 "Arrow offset (deg)" },
		{ "Alcance",                            "Range" },
		{ "Tamanho",                            "Size" },
		{ "Pos X",                              "Pos X" },
		{ "Pos Y",                              "Pos Y" },
		{ "Mira central custom",                "Custom crosshair" },
		{ "Mira tamanho",                       "Crosshair size" },
		{ "Mira vao",                           "Crosshair gap" },
		{ "Fundo atras do texto",               "Text background" },
		{ "Opacidade do fundo",                 "Background opacity" },
		{ "Streamproof (some de OBS/Discord)",  "Streamproof (hidden from OBS/Discord)" },
		{ "Show spectators counter",            "Show spectator counter" },
		{ "HUD (espectadores / mais perto)",    "HUD (spectators / closest)" },
		{ "HUD arrastavel com o mouse",         "Draggable HUD" },
		{ "HUD X",                              "HUD X" },
		{ "HUD Y",                              "HUD Y" },

		// ---- layout arrastavel ----
		{ "arraste um elemento; solte perto de uma ancora pra encaixar",
		  "drag an element; drop it near an anchor to snap" },
		{ "Resetar layout do ESP",              "Reset ESP layout" },
		{ "Barra de vida",                      "Health bar" },
		{ "Arma",                               "Weapon" },
		{ "Time",                               "Team" },
		{ "Bot/Player",                         "Bot/Player" },
		{ "Distancia",                          "Distance" },
		{ "topo",                               "top" },
		{ "base",                               "bottom" },
		{ "esquerda",                           "left" },
		{ "direita",                            "right" },
		{ "topo/esq",                           "top/left" },
		{ "topo/dir",                           "top/right" },
		{ "base/esq",                           "bottom/left" },
		{ "base/dir",                           "bottom/right" },
		{ "Vida",                               "Health" },

		// ---- mundo / loot ----
		{ "loot (itens no chao): aba WORLD",    "loot (items on the ground): WORLD tab" },
		{ "granadas (frag/molotov): sempre desenhadas - sem toggle no config",
		  "grenades (frag/molotov): always drawn - no config toggle" },
		{ "loot = cor por categoria (fixas no ESP)",
		  "loot = colour per category (fixed in the ESP)" },
		{ "entre numa partida para o catalogo se popular",
		  "join a match for the catalog to populate" },
		{ "nenhum item deste grupo visto ainda",
		  "no item from this group seen yet" },
		{ "%d itens no catalogo",               "%d items in the catalog" },
		{ "todos os %d itens do grupo",         "all %d items in the group" },
		{ "a lista e' do que o cheat JA' VIU: entra sozinho conforme voce' anda pelo mapa",
		  "the list is what the cheat has ALREADY SEEN: it fills in as you walk the map" },
		{ "Enable All",                         "Enable All" },
		{ "Disable All",                        "Disable All" },
		{ "Captured actors (first 30)",         "Captured actors (first 30)" },

		// ---- distancia maxima por categoria ----
		{ "jogador. veiculo e item tem teto proprio na aba WORLD",
		  "player. vehicle and item have their own cap on the WORLD tab" },
		{ "Distancia maxima: veiculo",          "Max distance: vehicle" },
		{ "Distancia maxima: item",             "Max distance: item" },

		// ---- espessura do traco (aba VISUAL, secao Cores) ----
		{ "Espessura da caixa",                 "Box thickness" },
		// ---- cores do ESP (widget ColourEdit4) ----
		{ "Caixa do inimigo",                   "Enemy box" },
		{ "Esqueleto",                          "Skeleton" },
		{ "DBNO (deitado)",                     "DBNO (knocked)" },
		{ "Oculto (atras de parede)",           "Hidden (behind a wall)" },
		{ "clique para escolher a cor",         "click to pick the colour" },
		{ "A (transparencia)",                  "A (alpha)" },
		{ "R",                                  "R" },
		{ "G",                                  "G" },
		{ "B",                                  "B" },
		{ "%d, %d, %d, %d",                     "%d, %d, %d, %d" },
		{ "#%02X%02X%02X  %d, %d, %d, %d",      "#%02X%02X%02X  %d, %d, %d, %d" },
		{ "Espessura do esqueleto",             "Skeleton thickness" },
		{ "Suavizar caixa",                     "Smooth box" },
		{ "interpolacao da caixa entre frames (mata o tremor do esqueleto animado)\ndesligado = caixa bruta, 100% instantanea",
		  "box interpolation between frames (kills the animated-skeleton jitter)\noff = raw box, 100% instant" },
		{ "contorno da caixa do inimigo (1.0 = 1 pixel nitido)",
		  "enemy box outline (1.0 = 1 crisp pixel)" },
		{ "traco dos ossos (14 segmentos usam o mesmo valor)",
		  "bone stroke (all 14 segments use the same value)" },

		// ---- filtro por substring (aba WORLD) ----
		{ "Filtro por nome (substring)",        "Name filter (substring)" },
		{ "Adicionar",                          "Add" },
		{ "modo",                               "mode" },
		{ "esconder os que casam",              "hide the matching ones" },
		{ "mostrar so' os que casam",           "show only the matching ones" },
		{ "nenhuma substring - lista vazia nao filtra nada",
		  "no substring - an empty list filters nothing" },
		{ "%d casam",                           "%d match" },
		{ "Ligar os que casam",                 "Turn the matching ones on" },
		{ "Desligar os que casam",              "Turn the matching ones off" },
		{ "(filtrado)",                         "(filtered)" },
		{ "buscar no catalogo",                 "search the catalog" },

		// ---- visibilidade (LastTimeScreenRenderered) ----
		{ "Visibilidade (LastTimeScreenRenderered)",
		  "Visibility (LastTimeScreenRenderered)" },
		{ "mede quem esta' na tela e quem esta' atras de parede.\ndesligado = uma cor so' (fail-open, comportamento antigo).",
		  "measures who is on screen and who is behind a wall.\noff = a single colour (fail-open, previous behaviour)." },
		{ "Tolerancia de visibilidade",         "Visibility tolerance" },
		{ "atraso ate' quanto ainda conta como 'na tela'.\nbaixo demais = pisca em inimigo visivel; alto demais = nunca acusa oculto.",
		  "how much delay still counts as 'on screen'.\ntoo low = flickers on a visible enemy; too high = never reports hidden." },
		{ "usada so' quando o vischeck esta' ligado abaixo",
		  "only used when the visibility check below is on" },
		{ "usada so' quando o vischeck esta' ligado (secao Visibilidade, acima)",
		  "only used when the visibility check is on (Visibility section, above)" },
		{ "pinta de 'Oculto' quem esta' atras de parede.\ndesligado = uma cor so' (fail-open, comportamento antigo).\nos numeros abaixo dizem se o sinal esta' chegando.",
		  "paints whoever is behind a wall with the 'Hidden' colour.\noff = a single colour (fail-open, previous behaviour).\nthe numbers below tell whether the signal is arriving." },
		{ "desligado: ESP com uma cor so' (fail-open)",
		  "off: ESP with a single colour (fail-open)" },
		{ "base: pawn local (lr=%.2f) | %d atores, %d ocultos",
		  "base: local pawn (lr=%.2f) | %d actors, %d hidden" },
		{ "base: GameState (t=%.2f) | %d atores, %d ocultos",
		  "base: GameState (t=%.2f) | %d actors, %d hidden" },
		{ "base: sem base (fail-open) | %d atores",
		  "base: none (fail-open) | %d actors" },
		{ "GameState",                          "GameState" },
		// ---- marca / cabecalho ----
		{ "mtmacr0",                            "mtmacr0" },
		{ "external",                           "external" },
		// ---- loader ----
		{ "pronto",                             "ready" },
		{ "idioma",                             "language" },
		// ---- visibilidade (regra nova: por mudanca do campo) ----
		{ "campo: vivo",                        "field: alive" },
		{ "campo em uso: %s",                   "field in use: %s" },
		{ "nenhum campo mudando - fail-open",   "no field changing - fail-open" },
		{ "inimigo   submit=%.0f  render=%.0f  onScreen=%.0f ms  -> %s",
		  "enemy   submit=%.0f  render=%.0f  onScreen=%.0f ms  -> %s" },
		{ "seu pawn  submit=%.0f  render=%.0f  onScreen=%.0f ms  -> %s",
		  "your pawn  submit=%.0f  render=%.0f  onScreen=%.0f ms  -> %s" },
		{ "campo: MORTO (fail-open) - o offset nao muda nesta build",
		  "field: DEAD (fail-open) - the offset does not change on this build" },
		{ "regra: o campo mudou nos ultimos %d ms = visivel",
		  "rule: the field changed within the last %d ms = visible" },
		{ "seu pawn   parado ha %.0f ms  -> %s",
		  "your pawn   idle for %.0f ms  -> %s" },
		{ "inimigo    parado ha %.0f ms  -> %s",
		  "enemy       idle for %.0f ms  -> %s" },
		{ "%d atores avaliados, %d ocultos",
		  "%d actors evaluated, %d hidden" },
		{ "parado ha': tempo desde a ultima mudanca do campo do render",
		  "idle for: time since the render field last changed" },
		{ "quanto tempo o campo pode ficar parado e ainda contar como 'desenhando'.\n100-250 ms e' a faixa util: abaixo pisca, acima demora a acusar oculto.",
		  "how long the field may stay unchanged and still count as 'being drawn'.\n100-250 ms is the useful range: lower flickers, higher takes long to report hidden" },
		{ "Carregar driver",                    "Load driver" },
		{ "Descarregar driver",                 "Unload driver" },
		{ "Iniciar PUBG",                       "Start PUBG" },
		{ "Cancelar",                           "Cancel" },
		{ "Sair",                               "Exit" },
		{ "carregado",                          "loaded" },
		{ "nao carregado",                      "not loaded" },
		{ "conectado",                          "connected" },
		{ "aguardando",                         "waiting" },
		{ "driver: %s",                         "driver: %s" },
		{ "jogo:   %s",                         "game:   %s" },
		{ "HOME abre o menu  |  END encerra a sessao e volta aqui",
		  "HOME opens the menu  |  END ends the session and returns here" },
		{ "pawn local",                         "local pawn" },
		{ "sem base (fail-open)",               "no base (fail-open)" },
		{ "local   lr=%.2f  dly=%.0f ms  -> %s",
		  "local   lr=%.2f  dly=%.0f ms  -> %s" },
		{ "inimigo lr=%.2f  dly=%.0f ms  -> %s",
		  "enemy   lr=%.2f  dly=%.0f ms  -> %s" },
		{ "visivel",                            "visible" },
		{ "OCULTO",                             "HIDDEN" },
		{ "inimigo = o mais proximo; atras de parede o 'dly' sobe para milhares de ms",
		  "enemy = the nearest one; behind a wall the 'dly' climbs to thousands of ms" },
		{ "cor de 'Oculto' esta' branca - troque em Cores para destacar",
		  "'Hidden' colour is white - change it in Colours to stand out" },

		// ---- humanizacao do RCS ----
		{ "Humanizar o RCS",                    "Humanise the RCS" },
		{ "reacao + variacao por frame + cadencia irregular.\na media da compensacao nao muda; o padrao muda.",
		  "reaction + per-frame variation + irregular cadence.\nthe average compensation stays; the pattern changes." },
		{ "Forca da humanizacao",               "Humanisation strength" },
		{ "0%% = so' a reacao. 100%% = variacao bem visivel.\nacima de ~60%% a compensacao fica pior que a teorica.",
		  "0%% = reaction only. 100%% = clearly visible variation.\nabove ~60%% the compensation gets worse than theoretical." },
		{ "Reacao no inicio da rajada",         "Reaction at burst start" },
		{ "quanto tempo ate' a compensacao chegar no maximo.\n0 = compensa no talo desde o primeiro frame (sem reacao).",
		  "how long until compensation reaches its maximum.\n0 = full compensation from the first frame (no reaction)." },
		{ "Suavidade (servo lag)",              "Smoothing (servo lag)" },
		{ "espalha a compensacao em vez de aplicar tudo no frame do tiro.\n"
		  "a vista passa a SOBIR e ser puxada de volta (dente de serra),\n"
		  "como a de um jogador. nada se perde: so' muda o instante.\n"
		  "0 = aplica tudo no frame (comportamento antigo).",
		  "spreads the compensation instead of applying it all on the shot frame.\n"
		  "the view now RISES and gets pulled back (sawtooth), like a player's.\n"
		  "nothing is lost: only the timing changes.\n"
		  "0 = apply everything on the frame (old behaviour)." },
		{ "Ruido (passeio)",                    "Noise (wandering)" },
		{ "erro de compensacao com INERCIA (~200 ms), em counts.\n"
		  "ruido branco some quando alguem faz media numa janela;\n"
		  "um passeio nao. 0 = sem ruido.",
		  "compensation error with INERTIA (~200 ms), in counts.\n"
		  "white noise disappears when someone averages over a window;\n"
		  "a random walk does not. 0 = no noise." },

		// ---- aba DEBUG (driver / medicao) ----
		// Faltavam: a aba DEBUG e' compilada atras de MENU_DEBUG_TABS e nunca
		// passou pela revisao de idioma. E' texto que aparece na tela, entao
		// entra na tabela como qualquer outro.
		{ "Driver / Anti-cheat",                "Driver / Anti-cheat" },
		{ "VERIFICAR AC (log detalhado)",       "CHECK AC (detailed log)" },
		{ "roda a varredura do gate e escreve o de-para no logs.txt",
		  "runs the gate scan and writes the mapping to logs.txt" },
		{ "MAP NOW (mapeia sem o gate)",        "MAP NOW (maps without the gate)" },
		{ "mapeia o driver AGORA, sem checar anti-cheat.\nO gate automatico nao roda mais nesta sessao.",
		  "maps the driver NOW, without checking anti-cheat.\nThe automatic gate no longer runs this session." },
		{ "driver: %s   seed: 0x%016llX",       "driver: %s   seed: 0x%016llX" },
		{ "Desempenho (medicao)",               "Performance (measurement)" },
		{ "Medir (liga os contadores)",         "Measure (enables the counters)" },
		{ "Zerar",                              "Reset" },
		{ "  divisao do tick:%s %s",            "  tick split:%s %s" },
		{ "  OCLUSOES: %llu (Present devolveu DXGI_STATUS_OCCLUDED)",
		  "  OCCLUSIONS: %llu (Present returned DXGI_STATUS_OCCLUDED)" },
		{ "Bullet Speed: %.1f m/s",             "Bullet speed: %.1f m/s" },
		{ "Gravity: %.2f",                      "Gravity: %.2f" },

		// ---- rodape / settings ----
		//
		// A string de rodape mostra o caminho do config. Ela e' montada em
		// runtime pelo L(...) a partir do par PT/EN — e o par estava com o nome
		// da pasta em texto claro. Cifrado por LIT() pelo mesmo motivo dos
		// outros: o conjunto `mtmacr0.exe` + `RenderCache` + `Default.json` e'
		// um artefato reconhecivel.
		{ "Save Configuration",                 "Save Configuration" },
		{ LIT("arquivo: %LOCALAPPDATA%\\RenderCache\\Default.json"),
		  LIT("file: %LOCALAPPDATA%\\RenderCache\\Default.json") },
		{ "HOME  abrir / fechar",               "HOME  open / close" },
		{ "END   panic / unload",               "END   panic / unload" },
		{ "Menu (abrir/fechar): HOME",          "Menu (open/close): HOME" },
		{ "Panic / unload: END",                "Panic / unload: END" },
		{ "hotkeys globais (RegisterHotKey) - funcionam com o jogo em foco",
		  "global hotkeys (RegisterHotKey) - work with the game focused" },
		{ "Idioma / Language",                  "Language" },
		{ "%s",                                 "%s" },
		{ "v2.1",                               "v2.1" },
		{ "Overlay",                            "Overlay" },
		{ "Radar",                              "Radar" },
		{ "COPY FULL DEBUG REPORT",             "COPY FULL DEBUG REPORT" },
		{ "Team: %d",                           "Team: %d" },
		{ "Spectators: %d",                     "Spectators: %d" },

		// ---- estados de tela ----
		{ "AGUARDANDO PUBG",                    "WAITING FOR PUBG" },
		{ "AGUARDANDO LOBBY",                   "WAITING FOR LOBBY" },
		{ "Inicializando",                      "Initialising" },
		{ "Pronto",                             "Ready" },

		// ---- tooltips ----
		{ "taxa do loop de leitura de memoria (nao e' o FPS do overlay)",
		  "memory read loop rate (not the overlay FPS)" },
		{ "liga os logs pesados de diagnostico.\nuse so' quando o PUBG atualizar e os offsets mudarem",
		  "enables the heavy diagnostic logs.\nuse only when PUBG updates and offsets change" },
		{ "inclui [VIS] do mesh e [DEATH] da morte (1x/s, aba LOGS)",
		  "also enables the [VIS] mesh and [DEATH] logs (1/s, LOGS tab)" },
		{ "guardada no config, mas o ESP nao usa mais:\nnao existe sinal de oclusao medido (fail-open)",
		  "stored in the config but no longer used by the ESP:\nno measured occlusion signal exists (fail-open)" },
		{ "cor 'Oculto' fica guardada, mas nao e' usada pelo ESP",
		  "the 'Hidden' colour is stored but unused by the ESP" },
		{ "sem sinal de oclusao medido: ESP fail-open (1 cor)",
		  "no measured occlusion signal: ESP fail-open (1 colour)" },
		{ "legibilidade sobre neve/ceu claro",  "readability over snow / bright sky" },
		{ "alpha do retangulo escuro (0 = invisivel, 255 = solido)",
		  "alpha of the dark box (0 = invisible, 255 = solid)" },
		{ "antecipa o alvo pelo tempo de voo da bala\n(bullet speed / gravity lidos do jogo)",
		  "leads the target by the bullet flight time\n(bullet speed / gravity read from the game)" },
		{ "compensa o recuo real lendo o acumulador da arma\n(nao precisa de tabela por arma)",
		  "compensates real recoil by reading the weapon accumulator\n(no per-weapon table needed)" },
		{ "mede quantos counts giram 1 grau.\nempunhe a arma, NAO atire, e clique.\na mira vai dar um tranco curto e voltar.",
		  "measures how many counts rotate 1 degree.\nhold the weapon, do NOT fire, then click.\nthe view will jerk briefly and return." },
		{ "graus que a vista gira por 1 count de mouse, na FOV 90.\no botao Calibrate preenche isto sozinho.\nmanual: mira ainda SOBE -> baixe;  mira DESCE -> suba",
		  "degrees the view rotates per 1 mouse count, at FOV 90.\nthe Calibrate button fills this in automatically.\nmanual: view still RISES -> lower it;  view DROPS -> raise it" },
		{ "quanto do recuo vertical anular.\n100%% = anula tudo;  70%% = deixa 30%% (menos obvio)",
		  "how much vertical recoil to cancel.\n100%% = cancels all;  70%% = leaves 30%% (less obvious)" },
		{ "trava de seguranca: um read ruim nunca vira um flick",
		  "safety cap: a bad read never becomes a flick" },
		{ "graus que a vista gira por 1 count de mouse, na FOV 90.\n",
		  "degrees the view rotates per 1 mouse count, at FOV 90.\n" },
		{ "quanto do recuo vertical anular.\n",
		  "how much vertical recoil to cancel.\n" },
		{ "liga os logs pesados de diagnostico.\n",
		  "enables the heavy diagnostic logs.\n" },
		{ "mede quantos counts giram 1 grau.\n",
		  "measures how many counts rotate 1 degree.\n" },
		{ "antecipa o alvo pelo tempo de voo da bala\n",
		  "leads the target by the bullet flight time\n" },
		{ "compensa o recuo real lendo o acumulador da arma\n",
		  "compensates real recoil by reading the weapon accumulator\n" },
		{ "arraste com o botao esquerdo com o MENU ABERTO\n(o clique nao chega no overlay com o menu fechado)",
		  "drag with the left button while the MENU IS OPEN\n(the click never reaches the overlay with the menu closed)" },
		{ "tambem da' pra arrastar direto na tela (menu aberto)",
		  "you can also drag it on screen (menu open)" },
		{ "encaixe o radar em cima do minimapa do jogo",
		  "dock the radar on top of the game minimap" },
		{ "Le GameState/PlayerArray. Causou BSOD (PAGE_FAULT) numa partida real - ligue por sua conta e risco.",
		  "Reads GameState/PlayerArray. It caused a BSOD (PAGE_FAULT) in a real match - enable at your own risk." },
		// ---- completadas pelo check_lang.ps1 (estavam sem traducao) ----
		{ "Actors",                             "Actors" },
		{ "auto-salva ao fechar o menu (HOME)", "auto-saves when the menu closes (HOME)" },
		{ "Blue Zone (risco)",                  "Blue Zone (risk)" },
		{ "Camera",                             "Camera" },
		{ "Core Engine",                        "Core Engine" },
		{ "Debug logs (CENSO / LOOTDIAG)",      "Debug logs (CENSUS / LOOTDIAG)" },
		{ "Diagnostics",                        "Diagnostics" },
		{ "Drawing Test (white box + actor ids)", "Drawing test (white box + actor ids)" },
		{ "Enable ALT mouse-move test",         "Enable ALT mouse-move test" },
		{ "FOV: %.1f",                          "FOV: %.1f" },
		{ "GNames: 0x%llX",                     "GNames: 0x%llX" },
		{ "Kills",                              "Kills" },
		{ "Local Player",                       "Local player" },
		{ "Max distance",                       "Max distance" },
		{ "mede o quanto a mira AINDA subiu depois da compensacao\ne ajusta o ganho sozinho durante a rajada.\n\ncorrige diferenca de arma/acessorio. NAO e salvo: reaprende a cada partida.",
		  "measures how much the view STILL rose after compensation\nand adjusts the gain by itself during the burst.\n\ncorrects per-weapon/attachment differences. NOT saved: it relearns every match." },
		{ "Memory FPS",                         "Memory FPS" },
		{ "Transporte KEVENT (shared memory)",  "KEVENT transport (shared memory)" },
		{ "sem device e sem IOCTL. ESP puro: aimbot e RCS NAO funcionam neste modo.\n"
		  "vale para a proxima sessao.",
		  "no device, no IOCTL. ESP only: aimbot and RCS do NOT work in this mode.\n"
		  "applies to the next session." },
		{ "KEVENT: aimbot e RCS inativos",      "KEVENT: aimbot and RCS inactive" },
		{ "muda o idioma do menu, do HUD e dos rotulos do ESP na hora",
		  "switches the language of the menu, HUD and ESP labels immediately" },
		{ "ONLINE",                             "ONLINE" },
		{ "Pawn: 0x%llX",                       "Pawn: 0x%llX" },
		{ "Pos: [%.0f, %.0f, %.0f]",            "Pos: [%.0f, %.0f, %.0f]" },
		{ "Total: %zu",                         "Total: %zu" },
		{ "UWorld: 0x%llX",                     "UWorld: 0x%llX" },
		{ "Closest Enemy: %dm",                 "Closest enemy: %dm" },
		{ "Airdrop",                            "Airdrop" },
		{ "Objetos do mundo",                   "World objects" },
		{ "veiculos e airdrop: aba WORLD",      "vehicles and airdrops: WORLD tab" },
		{ "Deathbox",                           "Deathbox" },	};

	// -----------------------------------------------------------------------
	// ESTADO
	// -----------------------------------------------------------------------
	inline int g_lang = LANG_PT_BR;

	inline void Set(int l) { g_lang = (l == LANG_EN_US) ? LANG_EN_US : LANG_PT_BR; }
	inline int  Get() { return g_lang; }

	// -----------------------------------------------------------------------
	// O MAPA E' CONSTRUIDO UMA VEZ, NA PRIMEIRA TRADUCAO.
	//
	// Um `for` linear sobre a tabela seria ~200 `strcmp` POR ROTULO POR FRAME
	// (200 rotulos x 144 fps = 5,7 M comparacoes/s). O mapa deixa isso em O(1).
	// -----------------------------------------------------------------------
	inline const std::unordered_map<std::string, const char*>& Mapa() {
		static const std::unordered_map<std::string, const char*> m = [] {
			std::unordered_map<std::string, const char*> r;
			r.reserve(sizeof(kTabela) / sizeof(kTabela[0]) * 2);
			for (const auto& p : kTabela) r.emplace(p.pt, p.en);
			return r;
		}();
		return m;
	}

	// Traduz. Sem entrada na tabela devolve o PROPRIO texto em portugues —
	// fallback visivel, e a ferramenta `tools\check_lang.ps1` acusa qual falta.
	inline const char* T(const char* pt) {
		if (!pt) return "";
		if (g_lang == LANG_PT_BR) return pt;
		const auto& m = Mapa();
		auto it = m.find(pt);
		return (it != m.end()) ? it->second : pt;
	}

	inline const char* Nome(int l) { return (l == LANG_EN_US) ? "English" : "Portugues (BR)"; }

} // namespace Lang

// Atalho de chamada. `L("texto em portugues")` devolve o texto no idioma ativo.
#define L(txt) Lang::T(txt)
