#pragma once

// ---------------------------------------------------------------------------
// Fonte usada pelo ESP (e so' por ele).
//
// O DirectOverlay carrega DUAS fontes: a primeira (tamanho da UI) vira a padrao
// do ImGui; a segunda fica aqui e o OSImGui::Text usa esta. Assim o ESP pode ter
// tamanho/oversample proprios sem mexer no menu.
// ---------------------------------------------------------------------------
#include <imgui.h>
inline ImFont* GEspFont = nullptr;

// ---------------------------------------------------------------------------
// Numero de passadas de contorno do StrokeText (qualidade do texto do ESP).
//
// Controlado pelo config EspTextQuality (Main.cpp -> OSImGui::StrokeText):
//   9 -> contorno cheio: 8 passadas + corpo (Ultra, padrao)
//   4 -> so' os 4 eixos + corpo (Rapida): ~45% menos trabalho de texto por
//        string — cada AddText e' um lote de indices no drawlist, e com 30+
//        jogadores na tela o texto e' a maior fatia da CPU do overlay.
// O valor vem do config em cada sessao; e' so' lido pela thread de render
// (mesma thread que escreve), entao nao precisa de atomic.
// ---------------------------------------------------------------------------
inline int GEspStrokePasses = 8;
#include "OS-ImGui_Base.h"

namespace RenderCore
{
    class OSImGui_External : public OSImGui_Base
    {
    private:
        WindowType Type = NEW;
    public:
        void NewWindow(std::string WindowName, Vec2 WindowSize, std::function<void()> CallBack);
        void AttachAnotherWindow(std::string DestWindowName, std::string DestWindowClassName, std::function<void()> CallBack);
    private:
        void MainLoop();
        bool UpdateWindowData();
        bool PeekEndMessage();
    };
}