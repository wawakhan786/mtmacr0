#pragma once
#include "../Config/ConfigUtilities.h"   // AutoSaveConfig
#include "../Perf.h"                     // instrumentacao (Present / frame)
#include "../Misc/HotkeyHook.h"          // hook de baixo nivel do hotkey do aimbot
#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")
#include <dxgi1_2.h>
#pragma comment(lib, "dxgi.lib")
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
// ---------------------------------------------------------------------------
// DirectComposition ó o UNICO caminho para swapchain com ALPHA.
//
// A DXGI Debug Layer diz o motivo, sem ambiguidade:
//
//   "DXGI ERROR: IDXGIFactory::CreateSwapChain: Alpha blended swapchains must
//    be created with CreateSwapChainForComposition, or
//    CreateSwapChainForCoreWindow with the DXGI_SWAP_CHAIN_FLAG_FOREGROUND_LAYER
//    flag."
//
// Ou seja: `CreateSwapChainForHwnd` NUNCA cria swapchain com alpha. Nao e'
// questao de escolher FLIP_DISCARD ou FLIP_SEQUENTIAL ó e' o ALPHA em si.
// Doc: learn.microsoft.com/en-us/answers/questions/1321741
//
// Foi essa a causa do 0x887A0001 (DXGI_ERROR_INVALID_CALL) que fez a primeira
// tentativa ser abandonada. E eu errei ao atribuir ao WS_EX_LAYERED: a janela
// estava com exstyle=0x00200088, sem layered, e falhou do mesmo jeito.
// ---------------------------------------------------------------------------
#include <dcomp.h>
#pragma comment(lib, "dcomp.lib")
#include "../Security/ImportHash.h"   // resolucao de import por hash (streamproof)
#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"
#include <functional>
#include <string>
#include <random>
#include <sstream>
#include "../UI/EspTypography.h"   // tipografia do ESP em tamanho nativo (freetype) ó transplante
#include "../UI/EmbeddedFonts.h"   // Inter-Medium embutida no .exe (resource 201)

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// BaseLog esta em Main.cpp e o LOG() vem do pch.h. O header tambem e usado
// fora do projeto (overlay.cpp), entao cai num fallback silencioso.
#ifndef LOG
#define LOG(fmt, ...) ((void)0)
#endif

namespace DirectOverlay {
    inline HWND Overlay = nullptr;
    inline ID3D11Device* Device = nullptr;
    inline IDXGISwapChain* SwapChain = nullptr;
    inline ID3D11DeviceContext* DeviceContext = nullptr;
    inline ID3D11RenderTargetView* TargetView = nullptr;

// ---------------------------------------------------------------------------
// DirectComposition ó os tres objetos do pipeline.
//
// `CreateSwapChainForComposition` NAO entrega a swapchain pra ninguem sozinha:
// ela nao tem HWND e nao aparece na tela. Quem a apresenta e' a arvore de
// composicao:
//
//     IDCompositionDevice  -> o device de composicao (ligado ao DXGI device)
//     IDCompositionTarget  -> amarra a arvore a uma HWND
//     IDCompositionVisual  -> o no' da arvore; recebe a swapchain em SetContent
//
// E o `Commit()` no fim publica a arvore. Sem ele nada aparece ó e' a chamada
// que o exemplo da Microsoft faz por ultimo.
// ---------------------------------------------------------------------------
inline IDCompositionDevice* DcompDevice = nullptr;
inline IDCompositionTarget* DcompTarget = nullptr;
inline IDCompositionVisual* DcompVisual = nullptr;

// ---------------------------------------------------------------------------
// QUAL BACKEND INICIALIZOU ó resposta direta para "o que esta' rodando?".
//
// O log precisa dizer QUAL caminho subiu, com QUAL API, e qual HRESULT
// apareceu quando o outro falhou. Guardar numa string evita deduzir pelo
// conjunto de linhas do log.
// ---------------------------------------------------------------------------
inline const char* BackendAtual  = "nenhum";
inline long        BackendHrFalha = 0;
    inline int ScreenWidth = 0;
    inline int ScreenHeight = 0;
    inline bool ShowMenu = false;

// ---------------------------------------------------------------------------
// QUAL CAMINHO DE APRESENTACAO ESTA' ATIVO.
//
//   true  -> FLIP (moderno): janela WS_EX_NOREDIRECTIONBITMAP, swapchain
//            FLIP_DISCARD com alpha PREMULTIPLIED, clique-atraves por
//            WM_NCHITTEST/HTTRANSPARENT. SEM layered, SEM color key.
//   false -> LEGADO: janela WS_EX_LAYERED + LWA_COLORKEY, swapchain DISCARD,
//            clique-atraves alternando WS_EX_TRANSPARENT.
//
// Existe porque o `ToggleMenu` faz coisas DIFERENTES nos dois: no legado ele
// precisa mexer no ex-style; no flip quem decide o clique e' o WM_NCHITTEST,
// que le' o `ShowMenu` direto.
// ---------------------------------------------------------------------------
inline bool UsandoFlipModel = false;

// =================================================================
// MODO CAPTURA (EpicPen / print de tela / OBS).
//
// O cliente usa o EpicPen para capturar a sobreposicao (ESP + HUD)
// e mostrar ao dono do account. Para a janela aparecer em qualquer
// tipo de captura (GDI, PrintWindow, Desktop Duplication), o caminho
// seguro e' o LEGADO ó janela layered com colorkey, o modelo de
// composicao mais antigo do Windows, visivel para TUDO ó com a
// affinity de captura desligada (WDA_NONE), por cima da Streamproof.
//
// Mecanica:
//   - AplicarModoCaptura(true) [chamado do menu, thread de render]:
//     so' grava a bandeira. Nao recria nada aqui.
//   - O loop Run() percebe `CapturaLigada && UsandoFlipModel` e
//     chama TrocarParaLegado() UMA vez (a troca e' idempotente).
//   - Trocar de estilo de janela exige recriar a janela e o device:
//     nao da' para converter em vida (ver o comentario do
//     CriarJanelaOverlay).
//
// Depois de ligar, o backend FICA legado ate' fechar o processo
// (desligar nao volta para o DComp automaticamente ó o DComp so'
// existe quando pedido pela MTMACR0_BACKEND=dcomp mesmo).
// =================================================================
inline std::atomic<bool> CapturaLigada{false};

// Chamado do RenderFrame (mesma thread que o loop Run()).
static void AplicarModoCaptura(bool ativo) {
    if (ativo == CapturaLigada.load(std::memory_order_relaxed)) return;
    CapturaLigada.store(ativo, std::memory_order_relaxed);
    LOG(LIT("[ov] modo captura: %s\n"), ativo ? "LIGADO" : "desligado");
}

// ---------------------------------------------------------------------------
// DESTRUINDO PARA RECRIAR ó evita que o fallback mate o proprio cheat.
//
// `CriarJanelaOverlay` destroi a janela antes de criar a nova (nao da' para
// trocar o ex-style de uma janela viva). So' que o `WndProc` responde a
// `WM_DESTROY` com `PostQuitMessage(0)` ó e o `WM_QUIT` fica na fila.
//
// Resultado medido: o fallback recriava a janela com sucesso, o loop de render
// comecava, e SAIA NA PRIMEIRA ITERACAO. O log terminava em "entrando no loop
// de render" sem nenhum erro, porque nao houve erro: o cheat pediu pra sair.
//
// Com esta flag, o WM_DESTROY da recriacao e' ignorado. O WM_DESTROY de
// verdade (fim do processo) continua derrubando tudo, que e' o que se quer.
// ---------------------------------------------------------------------------
inline bool DestruindoParaRecriar = false;

// ---------------------------------------------------------------------------
// COR DE FUNDO DO FRAME.
//
// Nos dois caminhos e' preto, mas o SIGNIFICADO muda e isso importa:
//
//   LEGADO (LWA_COLORKEY): o preto e' a CHAVE de transparencia. Todo pixel
//                          preto vira transparente ó por isso o projeto tem a
//                          regra "nunca desenhe preto puro". O alpha do
//                          ImGui nao faz transparencia de verdade: o que
//                          aparece e' RGB sobre o fundo preto.
//
//   FLIP (alpha real):     o alpha e' a transparencia. Preto puro com alpha
//                          255 e' PRETO OPACO ó desenha mesmo. E qualquer
//                          alpha intermediario compoe com o jogo de verdade,
//                          em vez de virar cinza.
//
// Em ambos o fundo limpa em (0,0,0,0), que e' transparente nos dois. Mas o
// que o ImGui desenha muda de aparencia entre os dois caminhos.
// ---------------------------------------------------------------------------
    inline std::function<void()> RenderCallback = nullptr;
    inline std::string WindowClass;
    inline std::string WindowTitle;
    // True enquanto o callback de render pode ser chamado com seguranca.
    inline bool Ready = false;

    // Caminho do .ini da UI. Preenchido por quem chama ANTES de Run() ó ver o
    // comentario em Run(), onde ele e' atribuido a io.IniFilename. Precisa ser
    // um static de verdade: o ImGui guarda o `const char*`.
    inline std::string UiIniPath;

    // -----------------------------------------------------------------
    // Fontes do menu.
    //
    // UiBodyFont  : regular ~15px. E' a PRIMEIRA do atlas, logo a fonte
    //               padrao do ImGui -> todo o menu herda ela sem PushFont.
    // UiTitleFont : semibold/bold ~17px, so' para titulos e headers.
    // GEspFont    : (em OS-ImGui_External.h) fonte do ESP, separada.
    // -----------------------------------------------------------------
    inline ImFont* UiBodyFont  = nullptr;
    inline ImFont* UiTitleFont = nullptr;

    // -----------------------------------------------------------------
    // Identidade da janela.
    //
    // O que estava aqui antes: classe "PUBG_OVERLAY" e titulo "PUBG_OVERLAY".
    // Isso e string de blacklist em qualquer anti-cheat √¢‚Ç¨‚Äù basta um EnumWindows
    // e comparar o class name.
    //
    // Agora: nome gerado a partir de prefixos de classes de sistema REAIS,
    // com sufixo aleatorio. O titulo imita janela de shell. Nenhuma dessas
    // strings aparece literalmente no binario (montadas em runtime).
    // -----------------------------------------------------------------
    inline std::string MakeClassName() {
        // -----------------------------------------------------------------
        // CIFRADOS COM LIT() NA 1.3 ó a licao aqui e' sobre o que "runtime"
        // quer dizer.
        //
        // A intencao desta funcao sempre foi boa: montar o nome em runtime pra
        // nao deixar literal no binario. Mas a intencao nao estava sendo
        // cumprida, e eu MEDI: as dez strings de `prefixes[]` apareciam em
        // TEXTO CLARO no .rdata do mtmacr0.exe.
        //
        // O motivo e' obvio depois de ver: `prefixes[]` e' um array de
        // `const char*` com literais ó o compilador poe o array em .rdata e os
        // literais junto. O sorteio acontece em runtime, mas o SORTEIO nao
        // esconde nada: o conjunto inteiro continua legivel no arquivo.
        //
        // Ou seja: o "montado em runtime" protegia contra comparar o
        // `GetClassName` num `EnumWindows`, e NAO protegia contra `strings`.
        // Sao defesas diferentes, e eu tinha anotado uma como se fosse a outra.
        //
        // -----------------------------------------------------------------
        // ========== E DEPOIS EU QUEBREI, DE UM JEITO PIOR. LEIA. ==========
        //
        // A primeira correcao foi trocar os literais por LIT():
        //
        //     static const char* prefixes[] = {
        //         LIT("ApplicationFrameWindow"), ...     // <- ERRADO
        //     };
        //
        // Isso NAO funciona. `LIT()` devolve `crypt_get()` ó um ponteiro para o
        // `_storage` de um objeto TEMPORARIO. O temporario morre no fim da
        // expressao do inicializador, e o array fica guardando ponteiros para
        // stack ja' sobrescrita.
        //
        // O SINTOMA, medido no teste 1 (log real):
        //
        //     [ov] classe='_9797' titulo='???'
        //
        // Prefixo VAZIO e titulo com lixo binario. O `_9797` e' so' o sufixo
        // aleatorio ó ele sobreviveu porque e' montado pelo `ostringstream`,
        // que nao depende do ponteiro.
        //
        // A REGRA, e ela vale para todo o projeto:
        //
        //     LIT() e' seguro quando o ponteiro e' consumido NA MESMA
        //     EXPRESSAO.     `std::string s = LIT("x");`  /  `LOG(LIT("..."));`
        //
        //     LIT() NAO e' seguro guardado num container de PONTEIROS.
        //
        // Por isso agora o array e' de `std::string`: o construtor COPIA dentro
        // da expressao, enquanto o temporario ainda esta' vivo. Custa uma
        // alocacao por entrada, uma vez, no start do overlay.
        //
        // (O `ApplyStreamProof` usa LIT() com `LOG(LIT(...))` ó mesma expressao
        // ó e por isso funcionou. O log do teste 1 prova:
        //  `streamproof desligado: SetWindowDisplayAffinity=1 (err=0)`.)
        //
        // O QUE ISSO NAO RESOLVE (o limite honesto desta funcao)
        //
        // Um anti-cheat com callback no kernel acha a JANELA, nao o nome dela.
        // E ao achar, vai ver uma classe chamada `ApplicationFrameWindow` cujo
        // processo dono e' `mtmacr0.exe` ó a INCONSISTENCIA e' o achado. Nome
        // de classe melhor nao conserta isso; so' nao ter janela conserta.
        // -----------------------------------------------------------------
        static const std::string prefixes[] = {
            LIT("Windows.UI.Core.CoreWindow"), LIT("ApplicationFrameWindow"),
            LIT("Microsoft.UI.Content.DesktopChildSiteBridge"), LIT("DirectUIHWND"),
            LIT("Shell_TrayWnd"), LIT("WorkerW"), LIT("Progman"), LIT("TaskManagerWindow"),
            LIT("XamlExplorerHostIslandWindow"), LIT("Windows.Internal.Shell.TabProxyWindow")
        };
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> pick(0, (int)(sizeof(prefixes) / sizeof(prefixes[0])) - 1);
        std::uniform_int_distribution<> suffix(0x1000, 0xFFFF);
        std::ostringstream ss;
        ss << prefixes[pick(gen)] << "_" << std::hex << suffix(gen);
        return ss.str();
    }

    inline std::string MakeWindowTitle() {
        // Mesmo tratamento dos prefixos de classe, e pelo MESMO motivo o array
        // e' de `std::string` e nao de `const char*`: ver o bloco longo do
        // `MakeClassName` sobre o ponteiro dangling do LIT().
        //
        // Foi este array que produziu o `titulo='???'` do teste 1.
        static const std::string titles[] = {
            LIT("Windows Shell Experience Host"),
            LIT("Microsoft Text Input Application"),
            LIT("Windows Input Experience"),
            LIT("Desktop Window Manager"),
        };
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> pick(0, (int)(sizeof(titles) / sizeof(titles[0])) - 1);
        return titles[pick(gen)];
    }

    // -----------------------------------------------------------------
    // Hotkey do menu
    //
    // O overlay e criado com WS_EX_NOACTIVATE: ele NUNCA recebe foco de
    // input. Por isso `GetAsyncKeyState(VK_HOME) & 1` nunca disparava ‚Äî a
    // tecla nao chega num processo sem foco. Era esse o bug do menu que nao
    // abria.
    //
    // RegisterHotKey resolve: o Windows entrega WM_HOTKEY pra nossa janela
    // independente de foco, e cobre o jogo em fullscreen.
    // -----------------------------------------------------------------
    inline constexpr int kHotkeyIdHome = 0x5047;   // 'PG'
    inline constexpr int kHotkeyIdEnd  = 0x5048;

    // ---------------------------------------------------------------------------
// PEDIDO DE FIM DE SESSAO ó o END deixou de matar o processo.
//
// O cheat agora tem DUAS fases: LOADER (carrega/descarrega driver, espera o
// jogo) e SESSAO (menu + ESP). O END encerra a SESSAO e volta pro loader, para
// o usuario descarregar o driver na mao ó e nao derruba o processo junto.
//
// Quem executa o encerramento e' o Main.cpp (RenderFrame): aqui so' se levanta
// a flag. Fazer o SELFDESTRUCT dentro do WndProc travaria a fila de mensagens.
// ---------------------------------------------------------------------------
inline std::atomic<bool> PedidoFimSessao{false};

inline void ToggleMenu() {
        ShowMenu = !ShowMenu;
        if (!Overlay) return;

        // -------------------------------------------------------------------
        // O EX-STYLE SO' E' MEXIDO NO CAMINHO LEGADO.
        //
        // LEGADO: o clique-atraves vem do par WS_EX_LAYERED + WS_EX_TRANSPARENT.
        //         Com o menu aberto tiramos o TRANSPARENT pra janela aceitar o
        //         clique do ImGui; fechado, devolvemos.
        //
        // FLIP:   nao ha' WS_EX_TRANSPARENT nem LAYERED. Quem decide e' o
        //         `WM_NCHITTEST` no WndProc, que le' o proprio `ShowMenu`.
        //
        // Mexer no ex-style nos dois caminhos seria pior que inutil: no flip
        // ele adicionaria WS_EX_LAYERED a uma janela que ja' tem swapchain de
        // composicao ó e swapchain FLIP numa janela layered e' exatamente a
        // combinacao que o DXGI recusa com 0x887A0001.
        // -------------------------------------------------------------------
        // Nos DOIS caminhos o WS_EX_TRANSPARENT e'' o que passa o clique ó e''
        // o unico mecanismo que vale para OUTRO processo.
        //
        //   menu ABERTO -> tira o TRANSPARENT: a janela aceita o clique do ImGui
        //   menu FECHADO -> poe o TRANSPARENT: o clique atravessa pro jogo
        //
        // O WS_EX_LAYERED so'' entra no caminho legado, onde a transparencia
        // depende dele (LWA_COLORKEY). No flip a transparencia e'' o alpha do
        // swapchain, e o LAYERED quebraria a composicao.
        LONG ex = GetWindowLongA(Overlay, GWL_EXSTYLE);
        if (ShowMenu) {
            ex = ex & ~WS_EX_TRANSPARENT;
        } else {
            ex = ex | WS_EX_TRANSPARENT;
        }
        if (!UsandoFlipModel) {
            ex = ex | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
        }
        SetWindowLongA(Overlay, GWL_EXSTYLE, ex);

        // No caminho FLIP o SWP_FRAMECHANGED e' o que faz o Windows reenviar
        // o WM_NCHITTEST ó e' assim que a troca de estado do menu passa a
        // valer sem mexer em estilo nenhum.

        // SetWindowLongA sozinho NAO aplica o novo ex-style: o Windows so
        // recalcula o hit-test quando a janela e reposicionada com
        // SWP_FRAMECHANGED. Sem isto o WS_EX_TRANSPARENT continua valendo pro
        // sistema e o ImGui nunca recebe o clique.
        //
        // O RegisterHotKey entrega a tecla sem ativar a janela, entao tambem
        // precisamos subir ela no z-order explicitamente ‚Äî senao o menu abre
        // atras do que estiver na frente.
        SetWindowPos(Overlay, ShowMenu ? HWND_TOPMOST : HWND_TOPMOST,
                     0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

        // O ImGui guarda o estado do mouse entre frames; ao voltar a receber
        // input, um estado velho faria o primeiro clique acertar o lugar
        // errado. Por isso, ao ABRIR o menu, limpamos o estado de botoes e
        // teclas. NAO usamos MouseDrawCursor: ele desenha um cursor de software
        // por cima do cursor do sistema e fica "piscando" (bug do mouse).
        if (ShowMenu) {
            ImGuiIO& io = ImGui::GetIO();
            io.MouseDrawCursor = false;
            io.MouseDown[0] = false;
            io.MouseDown[1] = false;
            io.MouseDown[2] = false;
            io.MouseWheel = 0.0f;
            io.KeyCtrl = false;
            io.KeyShift = false;
            io.KeyAlt = false;
        }

        LOG("[ov] menu %s (exstyle=0x%08lX)\n", ShowMenu ? "OPENED" : "closed",
            (unsigned long)GetWindowLongA(Overlay, GWL_EXSTYLE));

        // -------------------------------------------------------------------
        // AUTO-SAVE ao FECHAR o menu
        //
        // A infraestrutura de config ja' existia (SaveConfig/LoadConfig, JSON
        // em Documents\Hunt\Default.json, botao no menu), mas dependia de
        // apertar o botao ó e ninguem lembra. DaÌ o "tenho que configurar toda
        // vez".
        //
        // Fechar o menu e' o momento natural: voce acabou de ajustar. Salva
        // aqui (com throttle de 2s dentro do AutoSaveConfig) e o problema
        // deixa de existir.
        // -------------------------------------------------------------------
        if (!ShowMenu) {
            AutoSaveConfig();
        }
    }

    inline LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;

        // -------------------------------------------------------------------
        // CLIQUE-ATRAVES SEM `WS_EX_LAYERED` ó o coracao da mudanca.
        //
        // No caminho LEGADO o clique atravessava por
        // `WS_EX_LAYERED | WS_EX_TRANSPARENT`. E `WS_EX_LAYERED` e' justamente
        // o que IMPEDE o swapchain FLIP ó foi ele que fez a tentativa anterior
        // de alpha premultiplied morrer com `DXGI_ERROR_INVALID_CALL`
        // (0x887A0001), e o projeto voltou pra tras por causa disso.
        //
        // `HTTRANSPARENT` faz o mesmo pelo caminho moderno: o WM_NCHITTEST
        // responde "esse ponto nao e' meu" e o sistema entrega o clique pro que
        // esta' embaixo. Sem layered, sem color key, sem trocar ex-style.
        //
        // Com o menu ABERTO devolvemos o hit-test normal, senao o ImGui nunca
        // recebe o clique e o menu fica morto.
        //
        // So' age no caminho FLIP: no legado quem manda e' o ex-style, e mexer
        // nos dois ao mesmo tempo daria clique-atraves com o menu aberto.
        // -------------------------------------------------------------------
        if (msg == WM_NCHITTEST && UsandoFlipModel) {
            if (!ShowMenu) return HTTRANSPARENT;
            return DefWindowProcA(hWnd, msg, wParam, lParam);
        }

        // A hotkey global chega aqui, nao em GetAsyncKeyState.
        if (msg == WM_HOTKEY && wParam == (WPARAM)kHotkeyIdHome) {
            ToggleMenu();
            return 0;
        }

        // END = ENCERRAR o cheat (panic key).
        //
        // PostQuitMessage derruba o laco de render; o Run() chama
        // DestroyOverlay(), DESCARREGA O DRIVER e o processo sai limpo.
        //
        // O DESCARREGAMENTO NAO ACONTECE AQUI, e sim no Run() (Main.cpp,
        // funcao DescarregarDriver). Fazer aqui seria um SELFDESTRUCT ó com
        // sleep de 150ms mais um teto de 2s do lado do kernel ó dentro do
        // WndProc, travando a fila de mensagens do processo inteiro.
        //
        // (Este comentario dizia que o driver "NAO sai da memoria do kernel,
        // isso so' com reboot". Era verdade ate' a 1.1, quando a unica forma de
        // liberar as paginas era o kdmapper, que ja' tinha ido embora. Na 1.2 o
        // driver sabe se liberar pelo IOCTL_KM_SELFDESTRUCT.)
        if (msg == WM_HOTKEY && wParam == (WPARAM)kHotkeyIdEnd) {
            // A flag e lida pelo RenderFrame (Main.cpp), que decide:
            //   - sessao ativa -> encerra a sessao e volta pro LOADER
            //   - no LOADER    -> sai do processo
            // Ver DirectOverlay::PedidoFimSessao.
            LOG("[ov] END pressed -> ending session\n");
            PedidoFimSessao.store(true, std::memory_order_relaxed);
            return 0;
        }

        if (msg == WM_DESTROY) {
            // NAO derruba o processo quando a destruicao e' do fallback
            // (CriarJanelaOverlay destruindo a janela antiga pra criar a nova).
            // Sem esta guarda o WM_QUIT entra na fila e o loop de render sai na
            // primeira iteracao ó o fallback matava o proprio cheat.
            if (DestruindoParaRecriar) return 0;
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }

    inline bool RegisterHotkeys() {
        // MOD_NOREPEAT: segurar a tecla nao alterna em loop
        SetLastError(0);
        const BOOL a = RegisterHotKey(Overlay, kHotkeyIdHome, MOD_NOREPEAT, VK_HOME);
        const DWORD e1 = GetLastError();
        SetLastError(0);
        const BOOL b = RegisterHotKey(Overlay, kHotkeyIdEnd, MOD_NOREPEAT, VK_END);
        const DWORD e2 = GetLastError();
        LOG("[ov] RegisterHotKey HOME=%d (err=%lu)  END=%d (err=%lu)\n",
            (int)a, e1, (int)b, e2);
        return a != FALSE;
    }

    inline void UnregisterHotkeys() {
        if (Overlay) {
            UnregisterHotKey(Overlay, kHotkeyIdHome);
            UnregisterHotKey(Overlay, kHotkeyIdEnd);
        }
    }

    // -----------------------------------------------------------------
    // Tema do menu: chrome escuro do Neverlose + acento CIANO do projeto.
    // Chama uma vez depois de ImGui::CreateContext().
    // -----------------------------------------------------------------
    inline void ApplyProfessionalStyle() {
        ImGuiStyle& style = ImGui::GetStyle();

        // =================================================================
        // A "MOLDURA PRETA" ó causa raiz e correcao
        //
        // NAO era WindowBorderSize, NAO era ImGuiCol_Border e NAO era o
        // WindowBg. E' a SOMBRA DE JANELA deste fork do ImGui, que vem ligada
        // por default com 100 pixels:
        //
        //   imgui.cpp:1163  WindowShadowSize = 100.0f   (construtor de
        //                   ImGuiStyle; o StyleColorsDark chamado logo depois
        //                   nao mexe nisso)
        //   imgui_draw.cpp:238 / 304 / 371
        //                   Colors[ImGuiCol_WindowShadow] = (0.08, 0.08, 0.08, 0.35)
        //   imgui.cpp:5943  RenderWindowDecorations():
        //                     if (WindowShadowSize > 0.0f && Colors[WindowShadow].w > 0.0f)
        //                         RenderWindowShadow(window);
        //   imgui.cpp:5993  RenderWindowShadow() -> AddShadowRect(..., 100px, ...)
        //
        // O overlay e' WS_EX_LAYERED + LWA_COLORKEY com o PRETO como chave
        // (CreateOverlay -> SetLayeredWindowAttributes(RGB(0,0,0), LWA_COLORKEY)).
        // Color key NAO faz blend: ele COMPARA o pixel com a chave. A sombra
        // tem RGB (0.08,0.08,0.08) e alpha que cai de 0.35 ate' 0 ao longo dos
        // 100px; composta sobre o backbuffer limpo em preto (o blend do ImGui
        // e' SRC_ALPHA/INV_SRC_ALPHA) o resultado e'
        //
        //     rgb = 0.08 * (0.35 * queda) * 255 = 7.1 * queda
        //
        // Entao TODO pixel do halo com queda >= ~0.07 sai com RGB 1..7: nao e'
        // a chave, logo fica OPACO e praticamente PRETO. A queda so' desce
        // abaixo de 0.07 perto da borda externa do halo, e o degrade suave
        // vira um anel PRETO de ~29px, de borda DURA (o corte e' o limiar do
        // color key) e arredondado (WindowRounding + o canto da sombra) ó
        // exatamente a "moldura preta grossa e arredondada".
        //
        // Por isso clarear WindowBg/ChildBg/TitleBg nao mudava nada: a sombra
        // e' desenhada FORA do retangulo da janela (o proprio ImGui recorta o
        // miolo com ImDrawFlags_ShadowCutOutShapeBackground, imgui.cpp:6000).
        //
        // Conclusao: com color key sombra NUNCA pode parecer sombra ó um
        // degrade que termina em preto vira sempre um anel solido. Zero nela.
        // =================================================================
        style.WindowShadowSize        = 0.0f;                                 // imgui.cpp:1163
        style.Colors[ImGuiCol_WindowShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f); // imgui.cpp:5944
        // Ja' e' 0 por default (imgui.cpp:1164), mas explicito: com offset o
        // halo sairia deslocado e viraria uma tarja preta de um lado so'.
        style.WindowShadowOffsetDist  = 0.0f;

        // ---- geometria ----
        // Cantos: o Neverlose usa rounding 12 nos paineis (gui.cpp:2101).
        style.WindowRounding    = 12.0f;
        style.ChildRounding     = 10.0f;
        style.FrameRounding     = 6.0f;
        style.PopupRounding     = 12.0f;
        style.ScrollbarRounding = 6.0f;
        style.GrabRounding      = 6.0f;
        style.TabRounding       = 6.0f;

        // WindowPadding 8: e' a folga entre a borda arredondada da janela e os
        // paineis de dentro (sidebar / corpo). Sem ela o canto reto de um
        // BeginChild comeria o canto arredondado da janela.
        style.WindowPadding     = ImVec2(8.0f, 8.0f);
        style.FramePadding      = ImVec2(11.0f, 6.0f);
        style.ItemSpacing       = ImVec2(9.0f, 8.0f);
        style.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
        style.CellPadding       = ImVec2(10.0f, 6.0f);
        style.IndentSpacing     = 20.0f;
        style.ScrollbarSize     = 10.0f;
        style.GrabMinSize       = 12.0f;

        // ---- ZERO molduras ----
        // Nenhuma espessura de borda: o visual do Neverlose nao usa contorno,
        // usa contraste de painel + hairline de Separator.
        //
        // (Estas quatro linhas NAO eram a causa da moldura preta que o usuario
        // reportou ó ver o bloco sobre WindowShadowSize no topo da funcao. Elas
        // continuam 0 porque borda aqui nao tem funcao nenhuma.)
        style.WindowBorderSize  = 0.0f;
        style.ChildBorderSize   = 0.0f;
        style.PopupBorderSize   = 0.0f;
        style.FrameBorderSize   = 0.0f;
        style.SeparatorTextBorderSize = 1.0f;   // fina: so' um risco discreto
        // Obs.: TabBarBorderSize (folga/overline das abas) so' existe a partir
        // do ImGui 1.89.7. Aqui e' 1.89.6. O menu nao usa mais TabBar (a
        // sidebar trocou), entao o campo nao faria falta nem se existisse.
        style.WindowMinSize     = ImVec2(320.0f, 200.0f);

        // SeparatorText (os titulos de grupo do menu): o texto encosta na
        // margem e o filete preenche o resto da linha ó o agrupamento do
        // Neverlose e' texto + hairline, nao uma caixa.
        style.SeparatorTextAlign      = ImVec2(0.0f, 0.5f);
        style.SeparatorTextPadding    = ImVec2(0.0f, 7.0f);

        // Antialiasing do desenho (linhas/curvas do ESP e do menu).
        // Vem ANTES do PushFont porque o ImGui copia estes flags para o
        // draw list no inicio do frame (latch) - nao adianta mudar depois.
        style.AntiAliasedLines       = true;
        style.AntiAliasedLinesUseTex = true;   // linhas via textura: mais nitidas
        style.AntiAliasedFill        = true;

        // ===================================================================
        // ATENCAO: NAO chame PushFont aqui.
        //
        // Isto estava aqui e DERRUBAVA o cheat com 0xC0000005 no 1o frame:
        //
        //     aqui (1x, no init)        ImGui::PushFont(UiTitleFont);
        //     Main.cpp (todo frame)     ImGui::PopFont();
        //
        // O ImGui::NewFrame() RESETA a pilha de fontes a cada frame. Entao um
        // Push feito FORA de um frame e' descartado ó e o PopFont() do frame 1
        // tira de uma pilha VAZIA -> access violation.
        //
        // Regra: Push e Pop TEM que ficar no MESMO frame. O Push do titulo
        // agora vive no Main.cpp, colado no TextColored que o usa.
        // ===================================================================

        // ===================================================================
        // PALETA ó chrome do Neverlose (gui.cpp:2117-2142, tema Dark)
        //
        // REGRA DESTE OVERLAY (LWA_COLORKEY): alpha NAO e' transparencia.
        // O backbuffer e' limpo em (0,0,0) e o preto e' a chave; o blend do
        // ImGui e' SRC_ALPHA/INV_SRC_ALPHA sobre esse preto, entao o que se ve
        // e' rgb*alpha. Um WindowBg a 0.96 nao "deixa o jogo aparecer": ele so'
        // fica 4% mais escuro. Ja' o que e' desenhado POR CIMA de um painel
        // (painel = ja' opaco no framebuffer) usa alpha de verdade.
        //
        // Por isso: os FUNDOS entram com o RGB ja' multiplicado pelo alpha que
        // a referencia usa (chega no MESMO pixel que o Neverlose obtem com
        // painel translucido sobre preto) e alpha 1.0; hairlines / hover /
        // acento usam alpha real.
        //
        // Bonus: com o painel mais escuro, o fringe de anti-aliasing da borda
        // arredondada (rgb*cobertura) arredonda para 0 em quase toda a borda ó
        // ou seja, ele tambem some pelo color key, sem precisar desligar o AA.
        // ===================================================================
        const ImVec4 panelSide  = ImVec4(0.057f, 0.062f, 0.091f, 1.00f);  // side (0.071,0.078,0.114) * 0.80
        const ImVec4 panelTop   = ImVec4(0.039f, 0.053f, 0.081f, 1.00f);  // top  (0.043,0.059,0.090) * 0.90
        const ImVec4 panelBody  = ImVec4(0.039f, 0.050f, 0.077f, 1.00f);  // body (0.043,0.055,0.086) * 0.90
        const ImVec4 hairline   = ImVec4(1.000f, 1.000f, 1.000f, 0.12f);  // divider do Neverlose: branco 30/255
        const ImVec4 bordaFina  = ImVec4(0.118f, 0.129f, 0.173f, 1.00f);  // border do Neverlose

        // Acento: o CIANO do projeto, de proposito. O Neverlose usa um azul
        // (0.318, 0.486, 0.992 ó gui.hpp:61); trocar o acento deixaria o menu
        // ciano->azul enquanto o ESP/aimbot (que nao e' menu e nao foi mexido)
        // continua ciano. Alem disso o ciano tem contraste bem maior com o
        // painel azul-marinho do que o azul da referencia. Adotamos o CHROME
        // dela (paineis, hairlines, tipografia de grupo) e mantemos o acento.
        // ACENTO ROXO (pedido do He, 19/10/2026). Antes era ciano.
const ImVec4 cyan      = ImVec4(0.545f, 0.361f, 0.965f, 1.00f);   // #8B5CF6
        const ImVec4 cyanHover = ImVec4(0.655f, 0.545f, 0.980f, 1.00f);   // #A78BFA
        const ImVec4 cyanDim   = ImVec4(0.42f, 0.28f, 0.80f, 1.00f);      // #6B47CC
        const ImVec4 cyanFaint = ImVec4(0.545f, 0.361f, 0.965f, 0.30f);   // roxo 30%

        ImVec4* c = style.Colors;
        c[ImGuiCol_Text]            = ImVec4(0.922f, 0.922f, 0.941f, 1.00f);   // text do Neverlose
        c[ImGuiCol_TextDisabled]    = ImVec4(0.510f, 0.520f, 0.560f, 1.00f);   // text_disabled
        c[ImGuiCol_WindowBg]        = panelBody;
        c[ImGuiCol_ChildBg]         = panelBody;
        c[ImGuiCol_PopupBg]         = ImVec4(0.055f, 0.062f, 0.086f, 1.00f);
        // Bordas: todas as espessuras sao 0, entao isto so' serve para widgets
        // que puxam ImGuiCol_Border na mao (OSImGui::SwitchButton).
        c[ImGuiCol_Border]          = bordaFina;
        c[ImGuiCol_BorderShadow]    = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
        c[ImGuiCol_FrameBg]         = ImVec4(0.098f, 0.110f, 0.149f, 1.00f);   // frame_inactive
        c[ImGuiCol_FrameBgHovered]  = ImVec4(0.129f, 0.145f, 0.196f, 1.00f);
        c[ImGuiCol_FrameBgActive]   = ImVec4(0.157f, 0.176f, 0.235f, 1.00f);
        // Barra de titulo na cor do painel de topo (a janela do menu nem usa
        // mais barra de titulo: NoTitleBar + cabecalho desenhado a' mao).
        c[ImGuiCol_TitleBg]         = panelTop;
        c[ImGuiCol_TitleBgActive]   = panelTop;
        c[ImGuiCol_TitleBgCollapsed]= panelBody;
        c[ImGuiCol_MenuBarBg]       = panelTop;
        c[ImGuiCol_ScrollbarBg]     = ImVec4(0.031f, 0.039f, 0.059f, 1.00f);
        c[ImGuiCol_ScrollbarGrab]   = ImVec4(1.000f, 1.000f, 1.000f, 0.10f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.000f, 1.000f, 1.000f, 0.18f);
        c[ImGuiCol_ScrollbarGrabActive]  = cyan;
        c[ImGuiCol_CheckMark]       = cyan;
        c[ImGuiCol_SliderGrab]      = cyan;
        c[ImGuiCol_SliderGrabActive]= cyanHover;
        c[ImGuiCol_Button]          = ImVec4(0.102f, 0.102f, 0.133f, 1.00f);   // button do Neverlose
        // Hover/active com o acento ROXO (e nao o "escurece no hover" da
        // referencia: aquele recesso so' le' certo com translucidez/blur
        // atras, que o color key nao tem ó aqui ele so' ficaria menos
        // legivel). 28/10: estes tres eram TEAL (0.10, 0.80, 0.70) ó resto
        // da era ciana que nunca acompanhou a troca de acento para o roxo
        // de 19/10. Era o botao que brilhava esmeralda num menu roxo: o
        // "feio" mais visivel do chrome inteiro.
        c[ImGuiCol_ButtonHovered]   = ImVec4(0.545f, 0.361f, 0.965f, 0.25f);
        c[ImGuiCol_ButtonActive]    = ImVec4(0.545f, 0.361f, 0.965f, 0.45f);
        c[ImGuiCol_Header]          = ImVec4(0.153f, 0.169f, 0.212f, 1.00f);   // tab_active
        c[ImGuiCol_HeaderHovered]   = cyanFaint;
        c[ImGuiCol_HeaderActive]    = ImVec4(0.545f, 0.361f, 0.965f, 0.40f);
        c[ImGuiCol_Separator]       = hairline;
        c[ImGuiCol_SeparatorHovered]= cyanDim;
        c[ImGuiCol_SeparatorActive] = cyan;
        c[ImGuiCol_ResizeGrip]      = ImVec4(1.000f, 1.000f, 1.000f, 0.08f);
        c[ImGuiCol_ResizeGripHovered] = cyanDim;
        c[ImGuiCol_ResizeGripActive]  = cyan;
        // Abas: o menu nao usa mais TabBar (a sidebar trocou), mas as cores
        // ficam coerentes para qualquer popup/tab futura. Nesta versao do ImGui
        // (1.89.6) os nomes sao TabActive / TabUnfocused*; ImGuiCol_TabSelected
        // so' aparece a partir de 1.90.9.
        c[ImGuiCol_Tab]             = ImVec4(0.098f, 0.110f, 0.145f, 1.00f);
        c[ImGuiCol_TabHovered]      = cyanFaint;
        c[ImGuiCol_TabActive]       = ImVec4(0.153f, 0.169f, 0.212f, 1.00f);
        c[ImGuiCol_TabUnfocused]    = ImVec4(0.086f, 0.095f, 0.125f, 1.00f);
        c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.130f, 0.145f, 0.184f, 1.00f);
        c[ImGuiCol_PlotLines]       = cyanDim;
        c[ImGuiCol_PlotLinesHovered]= cyanHover;
        c[ImGuiCol_PlotHistogram]   = cyanDim;
        c[ImGuiCol_PlotHistogramHovered] = cyanHover;
        // Tabela do PLAYERS
        c[ImGuiCol_TableHeaderBg]     = ImVec4(0.098f, 0.110f, 0.145f, 1.00f);
        c[ImGuiCol_TableBorderStrong] = bordaFina;
        c[ImGuiCol_TableBorderLight]  = ImVec4(1.000f, 1.000f, 1.000f, 0.06f);
        c[ImGuiCol_TableRowBg]        = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
        c[ImGuiCol_TableRowBgAlt]     = ImVec4(1.000f, 1.000f, 1.000f, 0.03f);
        c[ImGuiCol_TextSelectedBg]    = ImVec4(0.545f, 0.361f, 0.965f, 0.35f);
        c[ImGuiCol_NavHighlight]      = cyan;
        // Dialogs: fundo escuro AZULADO, nunca preto puro (LWA_COLORKEY).
        c[ImGuiCol_ModalWindowDimBg]  = ImVec4(0.02f, 0.03f, 0.04f, 0.60f);
    }

    // Cria o device, o target view e inicializa o ImGui.
    // Separado de CreateOverlay() para poder checar cada passo.
    // -----------------------------------------------------------------------
    // CreateDevice ó FLIP primeiro, LEGADO como reserva.
    //
    // ===========================================================================
    // POR QUE OS DOIS CAMINHOS
    // ===========================================================================
    //
    // O caminho LEGADO (WS_EX_LAYERED + LWA_COLORKEY + SWAP_EFFECT_DISCARD) tem
    // tres custos medidos, e eles se somam no mesmo lugar ó a composicao:
    //
    //   1. janela layered NAO usa o plano de hardware: o DWM compoe ela todo
    //      frame;
    //   2. LWA_COLORKEY e' PIOR que LWA_ALPHA ó o DWM compara POR PIXEL na
    //      janela inteira, todo frame;
    //   3. uma topmost layered sobre o jogo tira ele do `Independent Flip`,
    //      que e' o caminho de scanout direto. O jogo paga junto.
    //
    // O FLIP_DISCARD resolve os tres: nao ha' copia de backbuffer (DISCARD
    // era o modelo blt), o alpha e' real (nao ha' comparacao de cor) e sem
    // WS_EX_LAYERED o DWM deixa o jogo em Independent Flip.
    //
    // ===========================================================================
    // E POR QUE ISSO NUNCA TINHA SIDO FEITO
    // ===========================================================================
    //
    // O comentario que estava aqui dizia:
    //
    //     "O alpha premultiplied via CreateSwapChainForHwnd dava
    //      DXGI_ERROR_INVALID_CALL (0x887A0001), entao voltamos para a antiga"
    //
    // O erro tem causa exata: `DXGI_ALPHA_MODE_PREMULTIPLIED` e' INCOMPATIVEL
    // com `WS_EX_LAYERED`. A tentativa anterior trocou o swapchain e manteve o
    // estilo da janela ó e o DXGI recusou, com razao. Por isso o fallback aqui
    // RECRIA a janela em vez de so' trocar o swapchain.
    //
    // O LEGADO CONTINUA NO CODIGO de proposito: maquina com driver antigo, GPU
    // sem suporte a flip em composicao, ou Windows sem DWM ligado voltam pra
    // ele sozinhos, em vez de ficarem sem overlay. Um overlay que nao aparece e'
    // pior que um overlay mais caro.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    inline bool CriarJanelaOverlay(bool legado) {
        if (Overlay) { DestruindoParaRecriar = true; DestroyWindow(Overlay); DestruindoParaRecriar = false; Overlay = nullptr; }

        // WS_EX_TRANSPARENT no caminho flip: e'' a UNICA forma cross-process de o
        // clique atravessar. O `WM_NCHITTEST`/`HTTRANSPARENT` NAO serve para
        // outro processo ó a doc diz "in the same thread", e o jogo nao e''.
        //
        // O `WS_EX_LAYERED` continua FORA de proposito: ele e'' o que torna a
        // swapchain de composicao inviavel e o que o DWM paga por pixel.
        //
        // HIPOTESE A TESTAR: o comentario antigo do projeto diz que
        // "WS_EX_TRANSPARENT sozinho nao passa o clique (precisa de layered)".
        // Isso foi medido naquela epoca, mas numa janela que NAO era
        // WS_EX_NOREDIRECTIONBITMAP. Se nao funcionar, a alternativa e'' aceitar
        // o layered so'' para input ó e isso esta'' anotado como proximo passo.
        // ===========================================================================
        // TESTE DECISIVO: WS_EX_LAYERED junto com WS_EX_NOREDIRECTIONBITMAP.
        //
        // ESTADO ATUAL DO CONHECIMENTO (medido, nao suposto):
        //   - WM_NCHITTEST/HTTRANSPARENT  -> NAO atravessa processo. Testado,
        //                                    falhou. A doc diz "same thread".
        //   - WS_EX_TRANSPARENT sozinho   -> NAO atravessa. Testado, falhou.
        //                                    O comentario antigo do projeto ja
        //                                    dizia isso.
        //   - WS_EX_LAYERED | TRANSPARENT -> atravessa. E'' o que o caminho
        //                                    LEGADO usa, e funciona.
        //
        // Logo: o clique-atraves cross-process PRECISA do LAYERED.
        //
        // A pergunta que sobra e'' UMA: `CreateSwapChainForComposition` aceita
        // uma janela layered? A doc exige NOREDIRECTIONBITMAP e nao proibe o
        // LAYERED explicitamente ó mas proibir ou nao, quem decide e'' o DXGI.
        //
        // Se ACEITAR -> ganhamos os dois: composicao E clique.
        // Se RECUSAR -> a escolha e'' binaria e volta pro usuario decidir.
        //
        // Nao chamamos SetLayeredWindowAttributes de proposito: o conteudo
        // (alpha real) vem do swapchain de composicao. O LAYERED aqui esta'' so''
        // pelo hit-test.
        // ===========================================================================
        const DWORD exFlip   = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT;
        const DWORD exLegado = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_LAYERED;

        Overlay = CreateWindowExA(
            legado ? exLegado : exFlip,
            WindowClass.c_str(), WindowTitle.c_str(),
            WS_POPUP | WS_VISIBLE,
            0, 0, ScreenWidth, ScreenHeight,
            NULL, NULL, GetModuleHandleA(NULL), NULL);

        if (!Overlay) {
            LOG("[-] CreateWindowExA falhou (%s): %lu\n",
                legado ? "legado" : "flip", GetLastError());
            return false;
        }

        if (legado) {
            // Color key: preto (0,0,0) vira transparente. So' no caminho legado
            // ó no flip a transparencia e' o ALPHA do swapchain, e chamar isto
            // la' reintroduziria o layered (e o DXGI_ERROR_INVALID_CALL).
            SetLayeredWindowAttributes(Overlay, RGB(0, 0, 0), 0, LWA_COLORKEY);
        }

        ShowWindow(Overlay, SW_SHOWNOACTIVATE);
        UpdateWindow(Overlay);

        LONG exAtual = GetWindowLongA(Overlay, GWL_EXSTYLE);
        LOG("[ov] window %s: 0x%p  exstyle=0x%08lX  visible=%d\n",
            legado ? "LEGACY (layered+colorkey)" : "FLIP (noreredirectionbitmap)",
            (void*)Overlay, (unsigned long)exAtual, (int)IsWindowVisible(Overlay));
        return true;
    }
    inline bool CriarBackBufferView() {
        ID3D11Texture2D* pBackBuffer = nullptr;
        if (FAILED(SwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))) || !pBackBuffer) {
            LOG("[-] SwapChain->GetBuffer(0) falhou\n");
            return false;
        }
        const HRESULT hrRtv = Device->CreateRenderTargetView(pBackBuffer, NULL, &TargetView);
        pBackBuffer->Release();
        if (FAILED(hrRtv) || !TargetView) {
            LOG("[-] CreateRenderTargetView falhou: 0x%08lX\n", (unsigned long)hrRtv);
            return false;
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // CriarDeviceComposicao ó o pipeline CORRETO, com DirectComposition.
    //
    // ===========================================================================
    // POR QUE NAO `CreateSwapChainForHwnd`
    // ===========================================================================
    //
    // A DXGI Debug Layer responde sem ambiguidade:
    //
    //   "Alpha blended swapchains must be created with
    //    CreateSwapChainForComposition, or CreateSwapChainForCoreWindow with
    //    the DXGI_SWAP_CHAIN_FLAG_FOREGROUND_LAYER flag."
    //
    // `CreateSwapChainForHwnd` NAO cria swapchain com alpha ó nao existe
    // combinacao de SwapEffect que resolva. Eu tentei FLIP_DISCARD e
    // FLIP_SEQUENTIAL em sequencia e os dois devolveram 0x887A0001, porque o
    // problema nunca foi o SwapEffect: era o AlphaMode.
    //
    // ===========================================================================
    // AS QUATRO ETAPAS (a ordem importa)
    // ===========================================================================
    //
    //   1. device D3D11 + factory DXGI
    //   2. CreateSwapChainForComposition  (SEM hwnd ó ela nao aparece sozinha)
    //   3. DCompositionCreateDevice -> Target(hwnd) -> Visual -> SetContent
    //   4. Commit()  <- e' o que publica a arvore; sem ele, nada na tela
    //
    // ===========================================================================
    // DETALHES QUE NAO SAO OPCIONAIS
    // ===========================================================================
    //
    //   FORMATO: `CreateSwapChainForComposition` exige B8G8R8A8_UNORM. O
    //   caminho antigo usava R8G8B8A8_UNORM, e trocar so' o efeito de swap
    //   mantendo o formato reprova.
    //
    //   HWND: `CreateTargetForHwnd(..., TRUE)` ó o TRUE e' topmost. E a janela
    //   PRECISA ter WS_EX_NOREDIRECTIONBITMAP, senao o DWM tenta desenhar o
    //   conteudo dela e a composicao conflita.
    //
    //   DEVICE DE COMPOSICAO: `DCompositionCreateDevice(dxgiDev, ...)` amarra
    //   os dois devices. Passar NULL tambem funciona (cria um interno), mas
    //   amarrar evita copia entre devices.
    // -----------------------------------------------------------------------
    inline bool CriarDeviceComposicao() {
        if (BackendHrFalha) { /* limpamos abaixo, mantemos o motivo da ultima */ }

        // ---- 1) device D3D11 -------------------------------------------------
        UINT createDeviceFlags = 0;
        D3D_FEATURE_LEVEL featureLevel;
        const D3D_FEATURE_LEVEL fls[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

        HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, createDeviceFlags,
                                       fls, 2, D3D11_SDK_VERSION,
                                       &Device, &featureLevel, &DeviceContext);
        if (FAILED(hr)) {
            BackendHrFalha = (long)hr;
            LOG("[ov] ERRO DComp: D3D11CreateDevice falhou: 0x%08lX\n", (unsigned long)hr);
            return false;
        }

        IDXGIDevice*   dxgiDev = nullptr;
        IDXGIAdapter*  adapter = nullptr;
        IDXGIFactory2* factory = nullptr;
        IDXGISwapChain1* sc1   = nullptr;
        bool ok = false;

        do {
            if (FAILED(hr = Device->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
                BackendHrFalha = (long)hr; LOG("[ov] ERRO DComp: QueryInterface(IDXGIDevice) 0x%08lX\n", (unsigned long)hr); break;
            }
            if (FAILED(hr = dxgiDev->GetAdapter(&adapter))) {
                BackendHrFalha = (long)hr; LOG("[ov] ERRO DComp: GetAdapter 0x%08lX\n", (unsigned long)hr); break;
            }
            if (FAILED(hr = adapter->GetParent(IID_PPV_ARGS(&factory)))) {
                BackendHrFalha = (long)hr; LOG("[ov] ERRO DComp: GetParent(IDXGIFactory2) 0x%08lX\n", (unsigned long)hr); break;
            }

            // ---- 2) swapchain de composicao ---------------------------------
            DXGI_SWAP_CHAIN_DESC1 sd1;
            ZeroMemory(&sd1, sizeof(sd1));
            // TAMANHO EXPLICITO ó obrigatorio aqui.
            //
            // No `CreateSwapChainForHwnd` o 0 significa "usa o tamanho da
            // janela". Em `CreateSwapChainForComposition` NAO EXISTE janela: a
            // swapchain e' um buffer solto que o visual vai receber. Com 0/0 o
            // DXGI devolve DXGI_ERROR_INVALID_CALL (0x887A0001) na hora.
            //
            // Por isso o overlay e' dimensionado na criacao, com o tamanho da
            // tela ó e nao em resposta a WM_SIZE. Trocar de resolucao exige
            // recriar a swapchain.
            sd1.Width       = (UINT)ScreenWidth;
            sd1.Height      = (UINT)ScreenHeight;
            // OBRIGATORIO em composicao (o caminho antigo usava R8G8B8A8_UNORM).
            sd1.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd1.SampleDesc.Count   = 1;
            sd1.SampleDesc.Quality = 0;
            sd1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            // 2 e' o minimo para FLIP. 3 da' mais folga ao compositor.
            sd1.BufferCount = 2;
            sd1.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            sd1.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;
            sd1.Scaling     = DXGI_SCALING_STRETCH;
            sd1.Flags       = 0;

            hr = factory->CreateSwapChainForComposition(Device, &sd1, nullptr, &sc1);
            if (FAILED(hr) || !sc1) {
                BackendHrFalha = (long)hr;
                LOG("[ov] ERRO DComp: CreateSwapChainForComposition falhou: 0x%08lX\n", (unsigned long)hr);
                break;
            }
            LOG("[ov] DComp: swapchain de composicao criada "
                "(B8G8R8A8_UNORM, FLIP_SEQUENTIAL, PREMULTIPLIED, buffers=%u)\n",
                (unsigned)sd1.BufferCount);

            // ---- 3) a arvore de composicao ---------------------------------
            hr = DCompositionCreateDevice(dxgiDev, __uuidof(IDCompositionDevice),
                                          (void**)&DcompDevice);
            if (FAILED(hr) || !DcompDevice) {
                BackendHrFalha = (long)hr;
                LOG("[ov] ERRO DComp: DCompositionCreateDevice falhou: 0x%08lX\n", (unsigned long)hr);
                break;
            }
            hr = DcompDevice->CreateTargetForHwnd(Overlay, TRUE, &DcompTarget);
            if (FAILED(hr) || !DcompTarget) {
                BackendHrFalha = (long)hr;
                LOG("[ov] ERRO DComp: CreateTargetForHwnd falhou: 0x%08lX "
                    "(a janela tem WS_EX_NOREDIRECTIONBITMAP?)\n", (unsigned long)hr);
                break;
            }
            hr = DcompDevice->CreateVisual(&DcompVisual);
            if (FAILED(hr) || !DcompVisual) {
                BackendHrFalha = (long)hr;
                LOG("[ov] ERRO DComp: CreateVisual falhou: 0x%08lX\n", (unsigned long)hr);
                break;
            }
            // A swapchain entra no visual. A partir daqui ela esta' na arvore.
            hr = DcompVisual->SetContent(sc1);
            if (FAILED(hr)) {
                BackendHrFalha = (long)hr;
                LOG("[ov] ERRO DComp: Visual->SetContent falhou: 0x%08lX\n", (unsigned long)hr);
                break;
            }
            hr = DcompTarget->SetRoot(DcompVisual);
            if (FAILED(hr)) {
                BackendHrFalha = (long)hr;
                LOG("[ov] ERRO DComp: Target->SetRoot falhou: 0x%08lX\n", (unsigned long)hr);
                break;
            }

            // ---- 4) COMMIT ó publica a arvore ------------------------------
            // Sem esta chamada a swapchain existe, o visual existe, e NADA
            // aparece na tela. E' o passo que mais se esquece.
            hr = DcompDevice->Commit();
            if (FAILED(hr)) {
                BackendHrFalha = (long)hr;
                LOG("[ov] ERRO DComp: Commit falhou: 0x%08lX\n", (unsigned long)hr);
                break;
            }

            SwapChain = sc1;   // IDXGISwapChain1 herda IDXGISwapChain
            ok = true;
        } while (false);

        if (factory) factory->Release();
        if (adapter) adapter->Release();
        if (dxgiDev) dxgiDev->Release();

        if (!ok) {
            // A swapchain criada antes de uma falha posterior precisa sair, senao
            // vaza ó e o fallback vai criar outro device em cima.
            if (sc1) sc1->Release();
            return false;
        }
        if (!CriarBackBufferView()) return false;

        UsandoFlipModel = true;
        BackendAtual = "DComp (CreateSwapChainForComposition + FLIP_SEQUENTIAL + PREMULTIPLIED)";
        return true;
    }

    inline bool CriarDeviceLegado() {
        DXGI_SWAP_CHAIN_DESC sd;
        ZeroMemory(&sd, sizeof(sd));
        sd.BufferCount = 2;
        sd.BufferDesc.Width = 0;
        sd.BufferDesc.Height = 0;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 0;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = Overlay;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        UINT createDeviceFlags = 0;
        D3D_FEATURE_LEVEL featureLevel;
        const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
            &SwapChain, &Device, &featureLevel, &DeviceContext);
        if (hr == DXGI_ERROR_UNSUPPORTED) {
            hr = D3D11CreateDeviceAndSwapChain(
                NULL, D3D_DRIVER_TYPE_WARP, NULL, createDeviceFlags,
                featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
                &SwapChain, &Device, &featureLevel, &DeviceContext);
        }
        if (FAILED(hr) || !SwapChain || !Device || !DeviceContext) {
            LOG("[-] D3D11CreateDeviceAndSwapChain falhou: 0x%08lX\n", (unsigned long)hr);
            return false;
        }

        if (!CriarBackBufferView()) return false;

        UsandoFlipModel = false;
        return true;
    }

    inline void LiberarDevice() {
        // ORDEM IMPORTA: a arvore de composicao segura a swapchain (o visual
        // tem uma referencia dela via SetContent). Soltar a swapchain primeiro
        // deixaria o visual apontando para memoria liberada.
        //
        // `Commit()` depois de esvaziar tambem nao e' opcional: sem ele o DWM
        // continua com a arvore antiga ate' o device de composicao morrer.
        if (DcompTarget) { DcompTarget->Release(); DcompTarget = nullptr; }
        if (DcompVisual) { DcompVisual->Release(); DcompVisual = nullptr; }
        if (DcompDevice) {
            DcompDevice->Commit();   // publica a arvore vazia
            DcompDevice->Release();
            DcompDevice = nullptr;
        }
        if (TargetView)    { TargetView->Release();    TargetView = nullptr; }
        if (SwapChain)     { SwapChain->Release();     SwapChain = nullptr; }
        if (DeviceContext) { DeviceContext->Release(); DeviceContext = nullptr; }
        if (Device)        { Device->Release();        Device = nullptr; }
    }

    inline bool CreateDevice() {
        if (Overlay == nullptr) return false;

        // ===================================================================
        // QUAL BACKEND E'' O PADRAO, E POR QUE
        // ===================================================================
        //
        // O LEGADO e'' o padrao. O DComp funciona, e'' mais correto e usa o
        // caminho moderno ó mas ele NAO consegue clique-atraverso:
        //
        //   DirectComposition define como os PIXELS sao compostos.
        //   USER32 define qual HWND recebe o mouse.
        //   Sao mecanismos com mascaras INDEPENDENTES.
        //
        // O alpha do visual nao vira a regiao de hit-test. O resultado medido
        // foi uma janela que desenha certo e come TODOS os cliques da tela ó
        // um overlay que nao deixa clicar no jogo e'' pior que um overlay mais
        // caro.
        //
        //   LEGADO: transparencia OK  clique OK    custo layered
        //   DComp : transparencia OK  clique NAO   sem custo layered
        //
        // Nao existe combinacao que de'' as tres numa janela fullscreen. Nao e''
        // parametro faltando: e'' limite de arquitetura, confirmado na doc
        // ("o hit-test da janela continua uniforme").
        //
        // O DComp FICA no codigo porque o ganho dele nunca foi MEDIDO. Ele sobe
        // com a variavel de ambiente abaixo, e ai'' da'' para comparar no
        // PresentMon em vez de discutir.
        // ===================================================================
        char cBackend[16] = { 0 };
        const DWORD nBackend = GetEnvironmentVariableA("MTMACR0_BACKEND", cBackend, sizeof(cBackend));
        const bool pediuDcomp = (nBackend > 0 && (cBackend[0] == 'd' || cBackend[0] == 'D'));

        if (pediuDcomp) {
            LOG("[ov] MTMACR0_BACKEND=dcomp -> tentando o caminho de composicao\n");
            if (CriarDeviceComposicao()) {
                LOG("[ov] BACKEND: %s\n", BackendAtual);
                return true;
            }
            LOG("[ov] DComp pedido mas falhou (hr=0x%08lX) -> caindo no legado\n",
                (unsigned long)BackendHrFalha);
        } else {
            LOG("[ov] default backend = LEGACY. (DComp: set Campbellpur_BACKEND=dcomp; "
                "it does NOT have click-through)\n");
        }

        // ---- RESERVA: recria a janela SEM flip e refaz o device ------------
        LOG("[ov] DComp unavailable (hr=0x%08lX) -> FALLBACK to layered + colorkey + DISCARD\n",
            (unsigned long)BackendHrFalha);
        LiberarDevice();
        if (!CriarJanelaOverlay(true)) {
            LOG("[-] not even the legacy path managed to create the window\n");
            return false;
        }
        if (!CriarDeviceLegado()) return false;

        BackendAtual = "LEGADO (D3D11CreateDeviceAndSwapChain + DISCARD + LWA_COLORKEY)";
        LOG("[ov] BACKEND: %s\n", BackendAtual);
        return true;
    }

    // -----------------------------------------------------------------------
    // CriarJanelaOverlay ó a janela, nos DOIS estilos.
    //
    //   legado = false -> FLIP: WS_EX_NOREDIRECTIONBITMAP, SEM layered.
    //                     E' o que permite swapchain FLIP_DISCARD com alpha
    //                     premultiplied. Clique-atraves por WM_NCHITTEST.
    //
    //   legado = true  -> o de antes: WS_EX_LAYERED + WS_EX_TRANSPARENT +
    //                     LWA_COLORKEY. Clique-atraves pelo ex-style.
    //
    // Existe como funcao separada porque o FALLBACK precisa RECRIAR a janela:
    // nao da' para trocar de estilo com a janela viva, e um swapchain de
    // composicao criado para o estilo errado devolve DXGI_ERROR_INVALID_CALL.
    // Foi exatamente esse erro que fez a tentativa anterior de alpha
    // premultiplied ser abandonada ó a janela ainda tinha WS_EX_LAYERED.

    inline bool CreateOverlay() {
        ScreenWidth = GetSystemMetrics(SM_CXSCREEN);
        ScreenHeight = GetSystemMetrics(SM_CYSCREEN);
        LOG("[ov] screen %dx%d\n", ScreenWidth, ScreenHeight);
        if (ScreenWidth <= 0 || ScreenHeight <= 0) {
            LOG("[-] GetSystemMetrics devolveu 0x0\n");
            return false;
        }

        WindowClass = MakeClassName();
        WindowTitle = MakeWindowTitle();
        LOG("[ov] class='%s' title='%s'\n", WindowClass.c_str(), WindowTitle.c_str());

        WNDCLASSEXA wc = { 0 };
        wc.cbSize = sizeof(WNDCLASSEXA);
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = WindowClass.c_str();
        if (!RegisterClassExA(&wc)) {
            LOG("[-] RegisterClassExA falhou: %lu\n", GetLastError());
            return false;
        }

        // Cria no estilo FLIP. Se o device/swapchain flip falhar mais tarde, o
        // `CreateDevice` recria a janela no estilo LEGADO ó ver CriarJanelaOverlay.
        if (!CriarJanelaOverlay(false)) return false;

        // (DWM blur-behind removido: ele era para a janela NAO-layered. Com
        //  WS_EX_LAYERED + LWA_COLORKEY a transparencia vem do color key, e o
        //  blur-behind podia conflitar.)

        // Composicao tem que estar ligada para o alpha por pixel valer
        BOOL compEnabled = FALSE;
        DwmIsCompositionEnabled(&compEnabled);
        LOG("[ov] DWM composition: %s\n", compEnabled ? "enabled" : "DISABLED");

        LOG("[ov] window created: 0x%p  visible=%d\n", (void*)Overlay,
            (int)IsWindowVisible(Overlay));

        // Nao usamos DwmExtendFrameIntoClientArea aqui (o ImGui renderiza com
        // alpha no swapchain).
        //
        // Sobre SetWindowDisplayAffinity: ver `ApplyStreamProof` logo abaixo.
        // O resumo e' que a opcao existe, e' OPT-IN e fica desligada por
        // padrao, porque WDA_EXCLUDEFROMCAPTURE e' um marcador de overlay de
        // cheat ó quem liga, assume o marcador.
        return true;
    }

    // -----------------------------------------------------------------------
    // STREAMPROOF ó opt-in, e aplicado SO' quando o valor MUDA.
    //
    // HISTORICO: este projeto tinha REMOVIDO o `SetWindowDisplayAffinity` de
    // proposito (era a linha que o comentario antigo citava), mas o checkbox
    // "Streamproof" continuou no menu gravando no config ó e NINGUEM lia.
    // Ou seja: a opcao nao fazia nada, e um checkbox que mente e' pior que um
    // checkbox ausente. O testador reportou exatamente isso.
    //
    // DECISAO: a opcao volta a funcionar, DESLIGADA por padrao. O marcador no
    // binario so' passa a existir para quem liga ó e' escolha de quem usa, nao
    // um padrao silencioso.
    //
    // O cache de `aplicado` existe porque isto e' chamado do loop de render:
    // SetWindowDisplayAffinity e' chamada de sistema e nao roda por frame a
    // toa. -1 = nunca aplicado (o primeiro frame sempre aplica).
    //
    // -----------------------------------------------------------------------
    // RESOLVIDO POR HASH (1.3) ó a funcao NAO esta' mais na IAT.
    //
    // `dumpbin /imports` mostrava:
    //
    //     USER32.dll
    //       394 SetWindowDisplayAffinity
    //
    // E `SetWindowDisplayAffinity` e' assinatura de OVERLAY: a API existe para
    // dizer ao DWM "nao me capture", e e' usada por OBS e gravadores ó e por
    // overlay de cheat. O nome na IAT respondia "este processo tem streamproof
    // de overlay" sem heuristica nenhuma.
    //
    // Agora o ponteiro vem do ImportHash: PEB -> lista de modulos ->
    // `user32.dll` (por hash do nome) -> tabela de exports -> nome com o hash
    // `0xAF56FC315BAC0CBB`. Nenhum `GetProcAddress` novo na IAT, nenhum nome de
    // funcao no .rdata.
    //
    // POR QUE `user32.dll` ESTA' SEMPRE CARREGADO AQUI
    //
    // A resolucao exige que o modulo ja' esteja na lista do loader. Neste ponto
    // do codigo a janela do overlay JA' foi criada (`CreateWindowExA` em
    // `CriarJanelaOverlay`) ó o `CreateWindowEx` carrega user32 por definicao.
    // Se por algum motivo nao estiver, `pSetWDA` fica nulo e a funcao
    // simplesmente nao aplica; o log diz que foi isso, em vez de mentir.
    //
    // O `static` no ponteiro garante resolucao UMA vez. O custo em runtime e'
    // uma indirecao ó o mesmo `call` indireto que a IAT pagaria.
    // -----------------------------------------------------------------------
    inline void ApplyStreamProof(bool ligado) {
        static int aplicado = -1;
        const int quer = ligado ? 1 : 0;
        if (quer == aplicado) return;
        if (!Overlay) return;

        aplicado = quer;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
        const DWORD aff = ligado ? WDA_EXCLUDEFROMCAPTURE : 0x00000000 /*WDA_NONE*/;

        // Resolvido uma vez, na primeira chamada que realmente aplica.
        using SetWDA_t = BOOL (WINAPI*)(HWND, DWORD);
        static SetWDA_t pSetWDA = (SetWDA_t)ImportHash::ResolveExport(
            ImportHash::kUser32_dll, ImportHash::kSetWindowDisplayAffinity);

        if (!pSetWDA) {
            // Cifrada: esta mensagem cita o NOME da API, e era a ultima
            // ocorrencia dele em texto claro no binario. O import saiu da IAT,
            // mas o texto do log ia junto ó mesma licao do MakeClassName:
            // remover a chamada nao remove a string.
            LOG(LIT("[ov] streamproof: SetWindowDisplayAffinity NAO RESOLVIDO "
                    "(user32.dll fora da lista de modulos?). Opcao ignorada.\n"));
            return;
        }

        SetLastError(0);
        const BOOL ok = pSetWDA(Overlay, aff);
        // O nome aparece SO' no texto do log ó que e' cifrado pelo LIT().
        LOG(LIT("[ov] streamproof %s: SetWindowDisplayAffinity=%d (err=%lu)\n"),
            ligado ? "ENABLED" : "disabled", (int)ok, (unsigned long)GetLastError());
    }

    // -----------------------------------------------------------------
    // MODO CAPTURA ó recria a janela + o device em LEGADO.
    inline void DestroyOverlay();
    //
    // Chamado do loop de render quando `CapturaLigada` fica true com o
    // backend em FLIP. A ordem importa:
    //   1. ImGui_ImplDX11_Shutdown ó a textura da fonte esta' amarrada ao
    //      device atual; solta o backend ANTES de matar o device.
    //   2. DestroyOverlay ó janela, objetos D3D/DComp, classe, hotkeys.
    //   3. Registra a classe de novo (o passo 2 desregou) e
    //      CriarJanelaOverlay(true) ó janela LEGADO (layered + colorkey).
    //   4. CriarDeviceLegado ó device/swapchain novos sobre a janela nova.
    //
    // Se qualquer passo falhar, tenta voltar ao caminho padrao: melhor ESP
    // visivel na tela sem aparecer no EpicPen do que overlay morto.
    // -----------------------------------------------------------------
    inline bool TrocarParaLegado() {
        if (!Ready) return false;
        if (!UsandoFlipModel) return true;   // ja' e' LEGADO

        LOG(LIT("[ov] captura: recriando janela/device para LEGADO...\n"));
        ImGui_ImplDX11_Shutdown();
        DestroyOverlay();

        WindowClass = MakeClassName();
        WindowTitle = MakeWindowTitle();
        WNDCLASSEXA wc = { 0 };
        wc.cbSize        = sizeof(WNDCLASSEXA);
        wc.style         = CS_CLASSDC;
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.lpszClassName = WindowClass.c_str();

        if (!RegisterClassExA(&wc) || !CriarJanelaOverlay(true) || !CriarDeviceLegado()) {
            LOG(LIT("[ov] LEGADO falhou na troca; tentando de novo pelo caminho do start\n"));
            // O DestroyOverlay ja' desregou a classe: o CreateOverlay re-faz
            // classe + janela FLIP e a partir dai' o mesmo caminho do
            // fallback nativo do CreateDevice (janela LEGADO + device legado).
            DestroyOverlay();
            if (!CreateOverlay() || !CriarJanelaOverlay(true) || !CriarDeviceLegado()) {
                LOG(LIT("[ov] overlay sem device apos a troca; aguardando re-tentativa\n"));
                Ready = false;
                return false;
            }
            ImGui_ImplWin32_Init(Overlay);
            ImGui_ImplDX11_Init(Device, DeviceContext);
            RegisterHotkeys();
            Ready = true;
            UsandoFlipModel = false;
            BackendAtual    = "LEGADO (modo captura)";
            return true;
        }

        ImGui_ImplWin32_Init(Overlay);
        ImGui_ImplDX11_Init(Device, DeviceContext);
        RegisterHotkeys();
        UsandoFlipModel = false;
        BackendAtual    = "LEGADO (modo captura)";
        Ready           = true;
        LOG(LIT("[ov] LEGADO pronto para captura\n"));
        return true;
    }

    inline void DestroyOverlay() {
        Ready = false;
        UnregisterHotkeys();
        // Volta ao default: o proximo CreateDevice decide de novo qual caminho
        // usar, em vez de herdar o estado da janela que acabou de morrer.
        UsandoFlipModel = false;
        // Uma funcao so' para soltar: ela conhece a ORDEM (a arvore de
        // composicao segura a swapchain) e libera os objetos DComp antes dos
        // D3D11. Liberar na mao aqui era como o DComp vazava.
        LiberarDevice();
        if (Overlay) {
            DestroyWindow(Overlay);
            Overlay = nullptr;
        }
        if (!WindowClass.empty()) {
            UnregisterClassA(WindowClass.c_str(), GetModuleHandleA(NULL));
            WindowClass.clear();
        }
    }

    // Retorna false se qualquer passo falhar. NAO entra em loop sem device.
    inline bool Run(std::function<void()> renderFn) {
        RenderCallback = renderFn;

        LOG("[ov] Run: CreateOverlay...\n");
        if (!CreateOverlay()) { LOG("[ov] CreateOverlay FALHOU\n"); DestroyOverlay(); return false; }

        LOG("[ov] Run: CreateDevice...\n");
        if (!CreateDevice())  { LOG("[ov] CreateDevice FALHOU\n");  DestroyOverlay(); return false; }

        LOG("[ov] Run: ImGui init...\n");
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;

        // -------------------------------------------------------------------
        // .ini DA UI ó FORA DA PASTA DO .EXE.
        //
        // O padrao do ImGui e' "imgui.ini" relativo ao diretorio de trabalho,
        // ou seja, ao lado do .exe. Junto com `logs.txt` e o binario, isso
        // monta no disco um conjunto de tres arquivos que descreve o programa
        // inteiro ó e `imgui.ini` e' um nome CONHECIDO (e' o arquivo de
        // configuracao do ImGui; quem procura overlay de cheat reconhece).
        //
        // O caminho vem de fora (`UiIniPath`, preenchido pelo Main.cpp, que e'
        // quem conhece a politica de pastas do projeto). O DirectOverlay NAO
        // inclui ConfigUtilities.h de proposito: ele e' a camada de janela, e
        // nao deve saber onde o projeto guarda arquivo.
        //
        // `IniFilename` e' um `const char*` que o ImGui GUARDA ó por isso o
        // ponteiro precisa continuar vivo. `UiIniPath` e' um static do modulo,
        // entao a vida dele e' a do processo. Vazio -> nullptr -> ImGui para de
        // salvar sozinho (o layout volta ao padrao a cada execucao), que e'
        // melhor que escrever no lugar errado.
        // -------------------------------------------------------------------
        io.IniFilename = UiIniPath.empty() ? nullptr : UiIniPath.c_str();

        // -------------------------------------------------------------------
        // FONTES  (fix do texto pixelado do ESP + tipografia do menu)
        //
        // Tres fontes, e a ORDEM importa: no ImGui a PRIMEIRA do atlas vira a
        // padrao da UI (o menu). Entao:
        //
        //   1a -> 15px medium   : corpo do menu. Vira a fonte padrao (Fonts[0]),
        //                         o que mantem o layout do menu intacto.
        //   2a -> 17px semibold : titulos/headers (17px = os cabecalhos do
        //                         Neverlose, main.cpp:104-108). O menu da
        //                         PushFont(UiTitleFont) so' onde quer destaque.
        //   3a -> 15px (ESP)    : guardada em GEspFont, usada SO' pelo ESP.
        //                         Face/tamanho/oversample IDENTICOS aos de
        //                         antes ó ver o bloco "ESP - NAO MEXER".
        //
        // Por que nao uma fonte grande + downscale: carregar a 32px e desenhar
        // a 15px e' reduzir 2.13x, e o downscale bilinear do ImGui aliasa nessa
        // proporcao. Carregando no tamanho de uso o glifo sai ~1:1 do atlas.
        // -------------------------------------------------------------------
        {
            // Helper local: primeira TTF existente da lista (ou nullptr).
            auto EscolherFonte = [](const char* const* lista, int n) -> const char* {
                for (int i = 0; i < n; ++i) {
                    if (GetFileAttributesA(lista[i]) != INVALID_FILE_ATTRIBUTES) return lista[i];
                }
                return nullptr;
            };

            // -----------------------------------------------------------------
            // FONTES DO PROJETO (OS-ImGui\fonts)
            //
            // A tipografia do menu agora e' a da referencia: Inter (Medium no
            // corpo, SemiBold nos titulos) ó no Neverlose e' Inter-Medium 15px
            // + Inter-SemiBold 17px (main.cpp:69 / 104-108).
            //
            // Licenca: Inter e' SIL Open Font License 1.1 (o proprio name table
            // do arquivo carrega "SIL Open Font License"), que permite copiar e
            // redistribuir. NAO copiamos de la': SSTBold/SSTMedium (Sony,
            // proprietarias), PingFang (Apple, proprietaria) e fa-solid-900
            // (Font Awesome Free: OFL + CC-BY, mas exigiria puxar a tabela
            // ICON_FA_* so' para ter icones que o menu nao usa).
            //
            // O build nao copia assets, e o exe roda de
            // Build\Release\PubgExt\, entao a pasta e' procurada nesta ordem:
            //   1. <pasta do exe>\fonts\        (se o .ttf for publicado junto)
            //   2. fonts\                       (CWD)
            //   3. OS-ImGui\fonts\              (CWD = PubgExt\)
            //   4. PubgExt\OS-ImGui\fonts\      (CWD = raiz do repo)
            // Se nada for achado, cai no C:\Windows\Fonts como antes.
            // -----------------------------------------------------------------
            char dirExe[MAX_PATH] = { 0 };
            GetModuleFileNameA(NULL, dirExe, MAX_PATH);
            std::string baseExe(dirExe);
            {
                const size_t barra = baseExe.find_last_of("\\/");
                baseExe = (barra == std::string::npos) ? std::string() : baseExe.substr(0, barra + 1);
            }

            // Primeiro caminho existente para <nome> nas pastas de fontes do
            // projeto (string vazia se nao houver). Devolve std::string porque
            // o caminho precisa sobreviver ate' o AddFontFromFileTTF.
            auto FonteLocal = [&](const char* nome) -> std::string {
                const std::string prefixos[] = {
                    baseExe + "fonts\\",                                    // .ttf publicado junto do exe
                    baseExe + "..\\..\\..\\PubgExt\\OS-ImGui\\fonts\\",      // Build\Release\PubgExt -> raiz do repo
                    baseExe + "..\\..\\PubgExt\\OS-ImGui\\fonts\\",          // Build\Release -> raiz do repo
                    baseExe + "..\\..\\..\\..\\PubgExt\\OS-ImGui\\fonts\\",  // Debug/x64 -> raiz do repo
                    std::string("fonts\\"),
                    std::string("OS-ImGui\\fonts\\"),
                    std::string("PubgExt\\OS-ImGui\\fonts\\"),
                };
                for (const std::string& prefixo : prefixos) {
                    const std::string caminho = prefixo + nome;
                    if (GetFileAttributesA(caminho.c_str()) != INVALID_FILE_ATTRIBUTES) return caminho;
                }
                return std::string();
            };

            const std::string interCorpo  = FonteLocal("Inter-Medium.ttf");
            const std::string interTitulo = FonteLocal("Inter-SemiBold.ttf");

            // CORPO do menu: Inter Medium 15px; fallback Segoe UI (o de antes).
            const char* corpoFallback[] = {
                "C:\\Windows\\Fonts\\segoeui.ttf",   // Segoe UI
                "C:\\Windows\\Fonts\\tahoma.ttf",
                "C:\\Windows\\Fonts\\verdana.ttf",
                "C:\\Windows\\Fonts\\arial.ttf",
                "C:\\Windows\\Fonts\\consola.ttf",
            };
            // TITULOS: Inter SemiBold 17px; fallback semibold/bold do Windows.
            const char* tituloFallback[] = {
                "C:\\Windows\\Fonts\\segoeuisb.ttf", // Segoe UI Semibold
                "C:\\Windows\\Fonts\\seguisb.ttf",   // Segoe UI Semibold (legado)
                "C:\\Windows\\Fonts\\segoeuib.ttf",  // Segoe UI Bold
                "C:\\Windows\\Fonts\\verdana.ttf",
                "C:\\Windows\\Fonts\\arialbd.ttf",
            };

            const char* fonteCorpo  = !interCorpo.empty()  ? interCorpo.c_str()
                                    : EscolherFonte(corpoFallback,
                                        (int)(sizeof(corpoFallback) / sizeof(corpoFallback[0])));
            const char* fonteTitulo = !interTitulo.empty() ? interTitulo.c_str()
                                    : EscolherFonte(tituloFallback,
                                        (int)(sizeof(tituloFallback) / sizeof(tituloFallback[0])));

            // -----------------------------------------------------------------
            // ESP ó NAO MEXER.
            //
            // GEspFont desenha TODO o texto in-game (OSImGui::Text/StrokeText) e
            // e' o unico ponto que o redesign nao pode encostar. De proposito,
            // ela continua sendo carregada da MESMA lista de antes (Segoe UI
            // 15px, OversampleH/V = 3, PixelSnapH = false) ó so' que agora a
            // lista esta' separada da do menu. Se o Inter entrasse aqui, a
            // metrica de TODO texto do ESP mudaria (a Inter e' mais larga que a
            // Segoe UI) e isso seria regressao.
            // -----------------------------------------------------------------
            // -----------------------------------------------------------------
            // ESP ó VERDANA (pedido do He, 20/10/2026).
            //
            // "Verdana tem excelente legibilidade mesmo em 12-14 px". E' a fonte
            // do Windows com maior altura-x, entao em tamanho pequeno (o ESP
            // desenha a 11-14 px por causa do `FontSize`) ela continua legivel
            // onde a Segoe UI embola.
            //
            // Lista PROPRIA, nao a do menu: Verdana e' mais LARGA que a Segoe
            // UI, e o ESP mede o texto para dimensionar caixas e fundos ó se o
            // Verdana faltar, cai na Segoe UI (comportamento antigo) e o log
            // diz qual entrou.
            // -----------------------------------------------------------------
            const char* espFallback[] = {
                "C:\\Windows\\Fonts\\verdana.ttf",   // Verdana (preferida)
                "C:\\Windows\\Fonts\\segoeui.ttf",   // fallback antigo
                "C:\\Windows\\Fonts\\tahoma.ttf",
                "C:\\Windows\\Fonts\\arial.ttf",
                "C:\\Windows\\Fonts\\consola.ttf",
            };
            const char* fonteEsp = EscolherFonte(espFallback,
                (int)(sizeof(espFallback) / sizeof(espFallback[0])));

            // Nenhuma fonte vetorial viuvel: mantem o bitmap default (nunca
            // deixa o atlas vazio, senao o ImGui nao renderiza texto nenhum).
            if (!fonteCorpo) {
                io.Fonts->AddFontDefault();
                UiBodyFont  = io.Fonts->Fonts[0];
                UiTitleFont = UiBodyFont;
                GEspFont    = UiBodyFont;
                LOG("[ov] nenhuma TTF encontrada -> bitmap default\n");
            } else {
                // ---- 1a: corpo da UI (vira a fonte padrao) ----
                ImFontConfig cfgCorpo;
                cfgCorpo.OversampleH = 2;      // oversample 2 (H e V)
                cfgCorpo.OversampleV = 2;
                cfgCorpo.PixelSnapH  = true;   // texto de UI: alinha no pixel, mais nitido
                UiBodyFont = io.Fonts->AddFontFromFileTTF(fonteCorpo, 15.0f, &cfgCorpo,
                                                          io.Fonts->GetGlyphRangesDefault());
                if (!UiBodyFont) {
                    // Arquivo existe mas nao abriu/decodificou: volta pro default
                    // sem quebrar (o atlas ja' pode ter fontes, tudo bem).
                    io.Fonts->AddFontDefault();
                    UiBodyFont = io.Fonts->Fonts[0];
                }

                // ---- 2a: titulos/headers (17px, como os cabecalhos do Neverlose) ----
                // Se nao houver semibold/bold, reusa a MESMA TTF do corpo (fica
                // com o mesmo desenho, so' 2px maior = ainda hierarquiza).
                const char* ttfTitulo = fonteTitulo ? fonteTitulo : fonteCorpo;
                ImFontConfig cfgTitulo;
                cfgTitulo.OversampleH = 2;
                cfgTitulo.OversampleV = 2;
                cfgTitulo.PixelSnapH  = true;
                UiTitleFont = io.Fonts->AddFontFromFileTTF(ttfTitulo, 17.0f, &cfgTitulo,
                                                           io.Fonts->GetGlyphRangesDefault());
                if (!UiTitleFont) UiTitleFont = UiBodyFont;

                LOG("[ov] fonts: body=15px (%s)  title=17px (%s)\n",
                    fonteCorpo, ttfTitulo);
            }

            // -----------------------------------------------------------------
            // ESP ó tipografia NATIVA (transplante do modulo ESP da build 28/10).
            //
            // Substitui a carga antiga do ESP (uma unica TTF 15px, stb,
            // OversampleH/V=3) por duas familias rasterizadas pelo FreeType em
            // tamanhos nativos 12-15 (PixelSnapH, oversample 1):
            //
            //   familia 0 (verdana) : Verdana/Segoe do disco (fonteEsp acima);
            //   familia 1 (inter)   : Inter-Medium EMBUTIDA no proprio .exe
            //                         (resource RCDATA 201 ó EmbeddedFonts.rc).
            //
            // O MENU continua nas fontes carregadas acima (stb, do disco):
            // so' o renderer de texto do ESP mudou, como na build de origem.
            // FontBuilderIO vira FreeType A PARTIR daqui ó as fontes do menu ja'
            // foram adicionadas antes, entao nao sao afetadas.
            // -----------------------------------------------------------------
            {
                const auto medium_resource = Ui::embedded_font(201);
                EspTypography::load_fonts(io.Fonts, fonteEsp, nullptr,
                    medium_resource.data, medium_resource.size);
                GEspFont = EspTypography::font_for(15);
                LOG("[ov] ESP fonts: native raster 12-15 (verdana=%s, inter=embedded %d B)\n",
                    fonteEsp ? fonteEsp : "(none)", medium_resource.size);
            }

            // GEspFont NUNCA pode ficar nulo: OSImGui::Text testa o ponteiro,
            // mas manter isto aqui documenta a invariante.
            if (!GEspFont && !io.Fonts->Fonts.empty()) GEspFont = io.Fonts->Fonts[0];
        }

        ApplyProfessionalStyle();
        ImGui_ImplWin32_Init(Overlay);
        ImGui_ImplDX11_Init(Device, DeviceContext);

        Ready = true;

        // A hotkey global. Sem isto o menu nunca abre: a janela tem
        // WS_EX_NOACTIVATE e nunca recebe foco, entao GetAsyncKeyState nao ve
        // tecla nenhuma. Ver o comentario em ToggleMenu().
        RegisterHotkeys();

        // -------------------------------------------------------------------
        // HOOK DE BAIXO NIVEL (leitura do hotkey do AIMBOT).
        //
        // `Start()` sobe uma THREAD DEDICADA que instala
        // WH_MOUSE_LL/WH_KEYBOARD_LL e fica dormindo em GetMessage para sempre
        // (o message loop que o SetWindowsHookEx exige). Ela nao desenha e nao
        // le memoria: e' so' o pump do hook.
        //
        // NAO instalamos nesta thread (a do overlay) de proposito, e isso foi
        // MEDIDO: aqui o loop passa parte de cada frame bloqueado em
        // `Present(1,0)` sem bombear, e o sistema so' entrega o input depois
        // que o hook retorna. Medicao nesta maquina (WH_MOUSE_LL):
        //
        //     thread dedicada (GetMessage)      : avg 0.14ms  max 0.71ms
        //     esta thread bloqueada no Present  : avg 12.31ms max 32.24ms
        //
        // 12ms de mouse lag cairia tambem nas injecoes do RCS (SendInput), que
        // hoje funciona. Se quiser mesmo assim testar na thread do overlay:
        // trocar a linha abaixo por `HotkeyHook::InstallOnCurrentThread()`.
        //
        // Nao existe Uninstall(): o hook vive ate' o processo morrer (ver o
        // comentario grande no HotkeyHook.h). O DestroyOverlay() NAO derruba
        // ele, e nao deve: o END fecha o cheat e o processo sai logo abaixo.
        // -------------------------------------------------------------------
        HotkeyHook::Start();

        LOG("[ov] Run: entering render loop\n");

#ifndef DXGI_STATUS_OCCLUDED
#define DXGI_STATUS_OCCLUDED ((HRESULT)0x087A0002L)
#endif

        MSG msg;
        ZeroMemory(&msg, sizeof(msg));
        // OCLUIDO NO ULTIMO PRESENT ó quando true (e menu fechado, e sem
        // pedido de fim de sessao pendente), a proxima iteracao pula o
        // build inteiro. Ver o bloco "FRAME OCLUIDO" no topico do loop.
        bool ocluidoAgora = false;
        while (msg.message != WM_QUIT) {
            if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                continue;
            }

            // MODO CAPTURA: se o backend esta' em FLIP/DComp, recria em
            // LEGADO UMA vez (depois UsandoFlipModel == false e isto nunca
            // roda de novo ó a troca e' barata por ser unica).
            if (CapturaLigada.load(std::memory_order_relaxed) && UsandoFlipModel)
                TrocarParaLegado();

            // -------------------------------------------------------------
            // FRAME OCLUIDO SEM MENU ó pula o build INTEIRO.
            //
            // Antes: ocluido continuava fazendo NewFrame + RenderCallback
            // (snapshot + ESP com ~10k quads) + Clear + RenderDrawData por
            // volta, so' para o Present devolver OCCLUDED na hora. Agora o
            // ciclo vira Clear+Present (detectar de-oclusao) + 16ms: o custo
            // da tela cheia invisivel cai de "frame completo" para ~zero.
            //
            // Por que NAO pular com menu aberto: o usuario pode abrir o menu
            // num segundo que a janela esta' por baixo (alt-tab meio feito,
            // capture fullscreen) ó nesse caso ele espera VER o menu, entao
            // o build completo segue acontecendo.
            //
            // Por que NAO pular com PedidoFimSessao pendente: o END (panic)
            // e' consumido DENTRO do RenderFrame (ele chama PararSessao, que
            // unmapeia o driver com sleeps ó nao pode rodar no WndProc). Se
            // o frame inteiro sumir, o END so' surtiria efeito ao desocludir.
            //
            // De-oclusao: o Present para de devolver OCCLUDED, o flag cai e
            // a iteracao seguinte ja' e' frame completo, sem sleep extra. A
            // fila de input do backend Win32 (janela oculta, click-through)
            // processa la no primeiro NewFrame ó atraso maximo de um frame.
            // -------------------------------------------------------------
            if (ocluidoAgora && !ShowMenu && !PedidoFimSessao.load(std::memory_order_relaxed)) {
                DeviceContext->OMSetRenderTargets(1, &TargetView, NULL);
                const float clrOclu[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                DeviceContext->ClearRenderTargetView(TargetView, clrOclu);
                const HRESULT hrP = SwapChain->Present(1, 0);
                ocluidoAgora = (hrP == DXGI_STATUS_OCCLUDED);
                if (ocluidoAgora) {
                    Perf::Contar(Perf::C.oclusoes);
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
                continue;
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            // `buildNs` = ImGui::Render + o RenderCallback (que e' o RenderFrame:
            // snapshot + ESP + radar + crosshair). E' o custo de CPU REAL do
            // overlay por frame ó o que a gente quer reduzir.
            {
                // Medido com relogio proprio: o `Perf::Escopo` soma no
                // DESTRUTOR, entao ler a duracao para o histograma dentro do
                // proprio bloco pegaria zero.
                const auto tBuild = std::chrono::steady_clock::now();
                if (RenderCallback) RenderCallback();
                ImGui::Render();
                Perf::ContarFrame((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - tBuild).count());
            }
            const float clear_color_with_alpha[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            DeviceContext->OMSetRenderTargets(1, &TargetView, NULL);
            DeviceContext->ClearRenderTargetView(TargetView, clear_color_with_alpha);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            // O `Present(1,0)` INCLUI a espera do vblank ó ele mede ~1/refresh
            // por definicao, entao NAO serve para comparar backend. O que ele
            // serve e' para detectar present PATOLOGICO (um pico aqui e' o
            // driver ou o DWM travando, nao o nosso desenho).
            {
                Perf::Escopo ePerf(Perf::C.presentNs);
                const HRESULT hrPresent = SwapChain->Present(1, 0);

                // -------------------------------------------------------------
                // OCLUSao ó o unico caso em que este loop vira SPIN LIVRE.
                //
                // O `Present(1,0)` normalmente SEGURA a thread ate' o vblank, e
                // e' isso que limita o loop ao refresh do monitor. Mas quando o
                // conteudo esta' ocluido, o DXGI devolve `DXGI_STATUS_OCCLUDED`
                // IMEDIATAMENTE ó sem esperar nada.
                //
                // Com o HRESULT descartado (como estava), esse caso e' invisivel:
                // o loop passa a girar milhares de vezes por segundo, refazendo
                // TODO o trabalho por frame (ImGui + ESP + clear de tela cheia)
                // a cada volta. E' exatamente o sintoma de "microtravamento com o
                // jogo em foco": o jogo esta' em fullscreen exclusivo por cima, o
                // overlay segue no laco, e a CPU e' disputada.
                //
                // O `DXGI_STATUS_OCCLUDED` NAO e' erro (e' 0x087A0002, positivo),
                // entao `FAILED()` nao pega ó tem que comparar com o valor.
                // -------------------------------------------------------------
                // (o #define de DXGI_STATUS_OCCLUDED subiu para o inicio do
                // Run: o bloco "FRAME OCLUIDO" no topico do loop tambem usa)
                ocluidoAgora = (hrPresent == DXGI_STATUS_OCCLUDED);
                if (ocluidoAgora) {
                    Perf::Contar(Perf::C.oclusoes);
                    // Este frame ainda foi buildado completo; a partir do
                    // PROXIMO, o bloco do topico do loop pula ImGui+ESP+
                    // clear+drawdata ate' a de-oclusao. Dorme um frame a
                    // 60 Hz ó e' a diferenca entre "espera" e "queima de CPU".
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
            }
        }

        Ready = false;
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        DestroyOverlay();
        return true;
    }
}
