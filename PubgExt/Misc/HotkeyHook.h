#pragma once
// ===========================================================================
// HotkeyHook — leitura de tecla por HOOK DE BAIXO NIVEL (mensagem).
//
// ---------------------------------------------------------------------------
// POR QUE ISSO EXISTE (o problema que ele resolve)
// ---------------------------------------------------------------------------
// Neste ambiente o `GetAsyncKeyState` devolve 0 para QUALQUER tecla, mesmo o
// usuario segurando: o cheat esta' elevado (Admin) e o BattlEye/Xenuine segue
// ativo mesmo com o PUBG em segundo plano, entao o estado ASSINCRONO das
// teclas nao chega ate' o processo. Foi medido em campo:
//
//   - `[AIM] aguardando hotkey (1)...` repetindo -> o Tick roda, o gate
//     `Configs.Aimbot.Enabled` esta' ligado, mas `IsHotkeyDown` sempre false;
//   - trocar o hotkey -> mesmo resultado;
//   - com o PUBG em SEGUNDO PLANO (alt-tab) -> mesmo resultado (nao e' o jogo
//     capturando o input em foco);
//   - `RegisterHotKey` (HOME/END) -> FUNCIONA.
//
// Ou seja: o que CHEGA neste processo e' MENSAGEM, nao polling. Este modulo
// troca o polling pelo mesmo caminho que faz o `RegisterHotKey` funcionar:
// `SetWindowsHookEx`, onde o Windows ENTREGA cada evento de mouse/teclado
// global ao nosso processo.
//
// ---------------------------------------------------------------------------
// COMO FUNCIONA / ONDE E' INSTALADO
// ---------------------------------------------------------------------------
// `SetWindowsHookEx(WH_MOUSE_LL/WH_KEYBOARD_LL, ...)` faz o sistema chamar o
// nosso callback DENTRO da thread que instalou o hook — e essa thread PRECISA
// bombeiar mensagens (o sistema so' despacha a chamada quando a thread chama
// PeekMessage/GetMessage).
//
// Este modulo cria a PROPRIA thread de pump (`Start()` -> `PumpThreadProc`),
// que instala os dois hooks e fica dormindo em `GetMessage` para sempre. Ela
// NAO faz mais nada: nao desenha, nao le memoria.
//
// POR QUE UMA THREAD DEDICADA E NAO A THREAD DO OVERLAY (medido, nao achismo)
// ---------------------------------------------------------------------------
// A thread do overlay (`DirectOverlay::Run`) TAMBEM tem message loop e
// instalaria o hook igual — mas ela passa boa parte de cada frame BLOQUEADA em
// `SwapChain->Present(1,0)` esperando o vblank, e nesse intervalo ela nao
// bombeia. Como o sistema so' entrega o input ao app de destino DEPOIS que o
// hook retorna, isso vira atraso de mouse no sistema inteiro — inclusive nas
// injecoes do proprio aimbot/RCS, que usam `SendInput`.
//
// Medido nesta maquina (probe `hookprobe`, WH_MOUSE_LL, 6s, ~150-195 amostras,
// atraso = QPC(hook) - QPC(SendInput)):
//
//     thread dedicada com GetMessage bloqueante : avg 0.14ms   max 0.71ms
//     loop apertado de PeekMessage              : avg 0.12ms   max 1.32ms
//     thread bloqueada 16ms sem bombear         : avg 12.31ms  max 32.24ms
//       (== o padrao do loop do overlay durante Present(1,0))
//
// 12ms de mouse lag e' ~2 frames a 144Hz — e cairia justamente em cima do RCS,
// que hoje funciona. Por isso: thread dedicada. O requisito duro do
// SetWindowsHookEx (a thread que instala TEM que ter message loop) continua
// satisfeito: a thread dedicada roda `GetMessage` para sempre.
//
// Se ainda assim quiser o hook na thread do overlay, e' 1 linha: trocar
// `HotkeyHook::Start()` por `HotkeyHook::InstallOnCurrentThread()` dentro de
// `DirectOverlay::Run` — funciona, com o custo acima.
//
// O hook fica instalado pela vida do processo: NAO existe `Uninstall()` de
// proposito (o hook e' da thread; quando o processo morre, o sistema derruba
// ele sozinho). Remover o hook no meio da execucao so' criaria a chance de
// perde-lo justamente quando o usuario esta' jogando.
//
// ---------------------------------------------------------------------------
// THREAD-SAFETY
// ---------------------------------------------------------------------------
// Quem ESCREVE o estado e' o callback do hook, que roda na THREAD DEDICADA do
// pump (a que instalou os hooks). Quem LE e' o `Aimbot::Tick`, que roda na
// thread de memoria (`MemoryLoop`) — e o `RenderFrame`, na thread principal. O
// estado e' uma unica mascara `std::atomic<uint32_t>` (1 bit por tecla), entao
// leitura e escrita sao atomicas, sem lock e sem corrida entre as tres threads.
//
// ---------------------------------------------------------------------------
// POR QUE NAO PELA WndProc
// ---------------------------------------------------------------------------
// O overlay e' WS_EX_TRANSPARENT (click-through): ele nunca recebe
// WM_RBUTTONDOWN pela janela. So' um hook GLOBAL enxerga o botao antes de o
// clique atravessar.
// ===========================================================================

#include <atomic>
#include <cstdint>
#include <windows.h>

// O header e' incluido por Aimbot.cpp (que tem o LOG() do pch.h) e por
// DirectOverlay.h. Mesmo padrao do DirectOverlay.h: sem LOG, cai num fallback
// silencioso em vez de quebrar quem inclui o header fora do projeto.
#ifndef LOG
#define LOG(fmt, ...) ((void)0)
#endif

namespace HotkeyHook {

	// -----------------------------------------------------------------------
	// INDICES DAS TECLAS
	//
	// Sao EXATAMENTE os indices que o `Aimbot::IsHotkeyDown` ja' usava (o
	// `switch` antigo). O `Configs.Aimbot.Hotkey` guarda este indice, entao
	// mexer neles quebraria as configs salvas do usuario.
	// -----------------------------------------------------------------------
	enum : int {
		KEY_LEFT_ALT = 0,   // VK_LMENU
		KEY_RBUTTON  = 1,   // VK_RBUTTON   (botao direito)
		KEY_SHIFT    = 2,   // VK_LSHIFT
		KEY_MOUSE5   = 3,   // VK_XBUTTON2  (botao lateral de tras)
		KEY_MOUSE4   = 4,   // VK_XBUTTON1  (botao lateral da frente)
		KEY_LBUTTON  = 5,   // VK_LBUTTON   (botao esquerdo)
		KEY_COUNT    = 6,
	};

	// Estado: 1 bit por indice. Escrito pelo hook, lido pelo aimbot.
	inline std::atomic<uint32_t> g_downMask{ 0 };
	// True quando OS DOIS hooks foram instalados com sucesso.
	inline std::atomic<bool>     g_installed{ false };
	// Quantos eventos o hook ja' recebeu (mouse + teclado). Serve de prova de
	// vida: se sobe, o hook esta' recebendo input mesmo que nenhuma tecla
	// mapeada esteja pressionada.
	inline std::atomic<uint32_t> g_eventCount{ 0 };

	// -----------------------------------------------------------------------
	// KEYREAD DE KERNEL (driver v4) — fonte PRIMARIA para ALT/SHIFT.
	//
	// O driver conecta o kbdclass e publica o estado na secao (km_keyread.h);
	// os bits espelham estes indices (KMP_KM_ALT = bit 0 = KEY_LEFT_ALT,
	// KMP_KM_SHIFT = bit 2 = KEY_SHIFT). Vantagens sobre o hook LL:
	//   - nao depende da cadeia de hooks user-mode (que e' o ponto classico
	//     de deteccao e quebra se outro app instalar hook primeiro);
	//   - funciona com GetAsyncKeyState morto (o caso desta VM).
	//
	// O hook WH_KEYBOARD_LL NAO sai por isso: ele continua como fallback, e o
	// resultado final e' sempre OR das fontes (se qualquer uma ver a tecla,
	// ela esta' pressionada). A troca fica em UM lugar: SetKernelProvider,
	// chamado pelo SubirDriverKevent quando o driver confirma v4 + kbdclass
	// instalado, e zerado quando o driver descarrega.
	//
	// Ponteiros em atomic: o provider pode ser instalado por uma thread
	// (loader) enquanto outra (aimbot) le a cada frame. atomic<uintptr_t>
	// evita tearing sem trazer dependencia de <functional> para o header.
	// -----------------------------------------------------------------------
	typedef bool    (*KernelKeyOkFn)();
	typedef uint32_t (*KernelKeyMaskFn)();
	inline std::atomic<uintptr_t> g_kernelOkFn{ 0 };
	inline std::atomic<uintptr_t> g_kernelMaskFn{ 0 };

	inline void SetKernelProvider(KernelKeyOkFn ok, KernelKeyMaskFn mask) {
		// publica o mask ANTES do ok: quem ve ok=true ja' le um mask valido
		// (e o oposto, mask novo com ok ainda 0, so' deixa o fallback mandar).
		g_kernelMaskFn.store((uintptr_t)mask, std::memory_order_release);
		g_kernelOkFn.store((uintptr_t)ok, std::memory_order_release);
	}

	// Leitura do estado no kernel; devolve 0 se o provider nao existir ou o
	// driver nao estiver ativo (checagem por chamada: o driver pode morrer
	// entre frames — o `ok` responde pela validade da leitura).
	inline bool KernelKeyDown(int index) {
		if (index < 0 || index >= 32) return false;
		const KernelKeyOkFn ok = (KernelKeyOkFn)(uintptr_t)g_kernelOkFn.load(std::memory_order_acquire);
		if (!ok || !ok()) return false;
		const KernelKeyMaskFn mask = (KernelKeyMaskFn)(uintptr_t)g_kernelMaskFn.load(std::memory_order_relaxed);
		return mask != nullptr && (mask() & (1u << index)) != 0;
	}

	inline HHOOK g_hMouse = nullptr;
	inline HHOOK g_hKeyboard = nullptr;
	// Thread que instalou o hook (= a que bombeia as mensagens). So' para log.
	inline DWORD g_ownerThreadId = 0;
	// Handle/tid da thread dedicada do pump e sinalizacao do install.
	inline HANDLE g_hookThread = nullptr;
	inline DWORD  g_hookThreadId = 0;
	inline std::atomic<bool> g_started{ false };
	inline std::atomic<bool> g_installDone{ false };

	// -----------------------------------------------------------------------
	// VkFromIndex — o mesmo mapeamento do `switch` antigo do Aimbot.
	// Usado apenas pela rede de seguranca (o polling) da funcao IsDown.
	// -----------------------------------------------------------------------
	inline int VkFromIndex(int index) {
		switch (index) {
			case KEY_LEFT_ALT: return VK_LMENU;
			case KEY_RBUTTON:  return VK_RBUTTON;
			case KEY_SHIFT:    return VK_LSHIFT;
			case KEY_MOUSE5:   return VK_XBUTTON2;
			case KEY_MOUSE4:   return VK_XBUTTON1;
			case KEY_LBUTTON:  return VK_LBUTTON;
			default:           return 0;
		}
	}

	inline void SetKeyState(int index, bool down) {
		if (index < 0 || index >= KEY_COUNT) return;
		const uint32_t bit = (1u << index);
		// relaxed: o que importa e' a atomicidade do bit, nao uma ordem entre
		// teclas diferentes (o aimbot olha uma tecla por vez).
		if (down) g_downMask.fetch_or(bit, std::memory_order_relaxed);
		else      g_downMask.fetch_and(~bit, std::memory_order_relaxed);
	}

	inline bool IsDownRaw(int index) {
		if (index < 0 || index >= KEY_COUNT) return false;
		return (g_downMask.load(std::memory_order_relaxed) & (1u << index)) != 0;
	}

	inline bool Installed() { return g_installed.load(std::memory_order_relaxed); }
	inline uint32_t DownMask() { return g_downMask.load(std::memory_order_relaxed); }
	inline uint32_t EventCount() { return g_eventCount.load(std::memory_order_relaxed); }
	inline const char* StateWord(int index) { return IsDownRaw(index) ? "down" : "up"; }

	// -----------------------------------------------------------------------
	// IsDown — leitura publica de uma tecla.
	//
	// Ordem das fontes (OR — a primeira que afirmar vale):
	//   1. HOOK LL          — o que funciona neste ambiente (ver topo);
	//   2. KEYREAD DE KERNEL— v4: o kbdclass publicou o bit na secao;
	//   3. GetAsyncKeyState — rede de seguranca historica (morta nesta VM,
	//                         mas mantida: se o hook e o kernel falharem
	//                         JUNTOS, este e' o ultimo lugar a olhar).
	// -----------------------------------------------------------------------
	inline bool IsDown(int index) {
		if (IsDownRaw(index)) return true;
		if (KernelKeyDown(index)) return true;
		const int vk = VkFromIndex(index);
		return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
	}

	// -----------------------------------------------------------------------
	// WH_MOUSE_LL — botoes do mouse.
	//
	// O hook so' olha DOWN/UP de botao; movimento (WM_MOUSEMOVE) e' ignorado de
	// proposito, porque o RCS/aimbot INJETA movimento e nao queremos que a
	// nossa propria injecao vire evento de leitura.
	// -----------------------------------------------------------------------
	inline LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
		if (nCode == HC_ACTION) {
			g_eventCount.fetch_add(1, std::memory_order_relaxed);
			const MSLLHOOKSTRUCT* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
			switch (wParam) {
				case WM_RBUTTONDOWN: SetKeyState(KEY_RBUTTON, true);  break;
				case WM_RBUTTONUP:   SetKeyState(KEY_RBUTTON, false); break;
				case WM_LBUTTONDOWN: SetKeyState(KEY_LBUTTON, true);  break;
				case WM_LBUTTONUP:   SetKeyState(KEY_LBUTTON, false); break;
				case WM_XBUTTONDOWN:
				case WM_XBUTTONUP: {
					const bool down = (wParam == WM_XBUTTONDOWN);
					const WORD botao = HIWORD(ms->mouseData);   // XBUTTON1 / XBUTTON2
					if (botao == XBUTTON1)      SetKeyState(KEY_MOUSE4, down);  // Mouse 4
					else if (botao == XBUTTON2) SetKeyState(KEY_MOUSE5, down);  // Mouse 5
					break;
				}
				default: break;
			}
		}
		// Sempre repassa: nao somos um bloqueador, so' um observador. Se nao
		// chamar o proximo hook, o mouse do usuario trava no sistema inteiro.
		return CallNextHookEx(nullptr, nCode, wParam, lParam);
	}

	// -----------------------------------------------------------------------
	// WH_KEYBOARD_LL — teclado (Left Alt / Left Shift).
	//
	// Alt gera WM_SYSKEYDOWN/WM_SYSKEYUP, nao WM_KEYDOWN/UP — por isso os
	// quatro casos.
	//
	// Sobre os VKs: um hook de baixo nivel entrega os codigos ESPECIFICOS de
	// lado (VK_LMENU / VK_LSHIFT), que e' o que o config do aimbot promete
	// ("Left Alt", "Left Shift"). Os genericos (VK_MENU / VK_SHIFT) entram como
	// fallback para driver/teclado que reporte o codigo generico — nesse caso
	// nao ha' como distinguir o lado, e' melhor o hotkey funcionar.
	// -----------------------------------------------------------------------
	inline LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
		if (nCode == HC_ACTION) {
			g_eventCount.fetch_add(1, std::memory_order_relaxed);
			const KBDLLHOOKSTRUCT* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
			const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
			const bool up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);

			if (down || up) {
				int index = -1;
				switch (kb->vkCode) {
					case VK_LMENU:  case VK_MENU:   index = KEY_LEFT_ALT; break;
					case VK_LSHIFT: case VK_SHIFT:  index = KEY_SHIFT;    break;
					default: break;
				}
				if (index >= 0) SetKeyState(index, down);
			}
		}
		return CallNextHookEx(nullptr, nCode, wParam, lParam);
	}

	// -----------------------------------------------------------------------
	// InstallOnCurrentThread — instala os dois hooks NA THREAD ATUAL.
	//
	// TEM que ser chamado de uma thread com message loop (ver o bloco grande no
	// topo do arquivo: quem chama de fato e' a thread dedicada do `Start()`).
	// Idempotente: chamar de novo nao reinstala.
	//
	// `hMod = GetModuleHandleA(NULL)` (o proprio exe) e' obrigatorio para hook
	// GLOBAL (dwThreadId = 0): o sistema precisa do modulo que contem o
	// callback.
	// -----------------------------------------------------------------------
	inline bool InstallOnCurrentThread() {
		if (g_installed.load(std::memory_order_acquire)) return true;

		g_ownerThreadId = GetCurrentThreadId();
		const HINSTANCE hInst = GetModuleHandleA(nullptr);

		SetLastError(0);
		g_hMouse = SetWindowsHookExA(WH_MOUSE_LL, MouseProc, hInst, 0);
		const DWORD errMouse = GetLastError();

		SetLastError(0);
		g_hKeyboard = SetWindowsHookExA(WH_KEYBOARD_LL, KeyboardProc, hInst, 0);
		const DWORD errKeyboard = GetLastError();

		const bool ok = (g_hMouse != nullptr) && (g_hKeyboard != nullptr);
		g_installed.store(ok, std::memory_order_release);

		LOG("[KEY] HotkeyHook instalado: thread dedicada (pump GetMessage) tid=%lu | mouse_ll=0x%p (err=%lu) | keyboard_ll=0x%p (err=%lu) | ativo=%d\n",
			(unsigned long)g_ownerThreadId,
			(void*)g_hMouse, (unsigned long)errMouse,
			(void*)g_hKeyboard, (unsigned long)errKeyboard,
			(int)ok);
		return ok;
	}

	// -----------------------------------------------------------------------
	// Thread de pump.
	//
	// A ORDEM importa: instala os hooks primeiro e so' depois entra no
	// GetMessage — se ela instalasse e saisse, o Windows derrubaria os hooks ao
	// fim da thread.
	//
	// `GetMessage` bloqueia (0% de CPU) e acorda exatamente quando ha'
	// mensagem, inclusive a chamada do hook — e' o desenho medido em 0.14ms de
	// atraso medio (ver o topo do arquivo).
	// -----------------------------------------------------------------------
	inline DWORD WINAPI PumpThreadProc(LPVOID) {
		InstallOnCurrentThread();
		g_installDone.store(true, std::memory_order_release);

		MSG msg;
		while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
		return 0;
	}

	// -----------------------------------------------------------------------
	// Start — sobe a thread dedicada do hook. Idempotente.
	//
	// Chamado UMA vez, do `DirectOverlay::Run` (thread principal), logo depois
	// do `RegisterHotkeys()`: assim o hook nasce junto com o overlay e vive ate'
	// o processo morrer. A thread nunca e' removida nem esperada (detached): o
	// handle fica guardado so' por completude.
	// -----------------------------------------------------------------------
	inline bool Start() {
		if (g_started.exchange(true)) return g_installed.load(std::memory_order_acquire);

		g_hookThread = CreateThread(nullptr, 0, PumpThreadProc, nullptr, 0, &g_hookThreadId);
		if (!g_hookThread) {
			// Sem thread nao ha' hook. Loga e segue: o aimbot cai no fallback de
			// polling do `IsDown` e o `[KEY] hook: ativo=0` aparece no log.
			LOG("[-] HotkeyHook: CreateThread falhou (err=%lu) — sem hook de teclado\n",
				(unsigned long)GetLastError());
			return false;
		}

		// Espera curta SO' para o log de instalacao sair ordenado no logs.txt
		// (o install acontece na outra thread). Se estourar, quem manda e' o
		// `[KEY] hook: ativo=%d` do diagnostico.
		for (int i = 0; i < 400 && !g_installDone.load(std::memory_order_acquire); ++i) {
			Sleep(5);
		}
		return g_installed.load(std::memory_order_acquire);
	}

} // namespace HotkeyHook
