#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#include "OS-ImGui_Base.h"
#include "imgui_settings.h"   // KakaFontsInit + os ImFont* do tema
//#include "..\Font\HarmonyOS_SansSC_Bold.h"

namespace RenderCore
{
    bool OSImGui_Base::InitImGui(ID3D11Device* device, ID3D11DeviceContext* device_context)
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->AddFontDefault();
        (void)io;

        ImFontAtlas* fontAtlas = new ImFontAtlas();
        ImFontConfig arialConfig;
        arialConfig.FontDataOwnedByAtlas = false;
        ImFont* arialFont = fontAtlas->AddFontFromFileTTF("c:\\Windows\\Fonts\\msyh.ttc", 20.0f, &arialConfig, io.Fonts->GetGlyphRangesAll());

        ImFontConfig ESPConfig;
        ESPConfig.FontDataOwnedByAtlas = false;
        ImFont* ESPFont = fontAtlas->AddFontFromFileTTF("c:\\Windows\\Fonts\\msyh.ttc", 18.0f, &arialConfig, io.Fonts->GetGlyphRangesAll());
        io.Fonts = fontAtlas;

        // Fontes do tema do ImGui novo (calibri_bold, icomoon_menu, ...).
        // Precisa vir DEPOIS do atlas existir e ANTES do primeiro NewFrame.
        KakaFontsInit(io);

        // ImGui::StyleColorsEnemyMouse();
        ImGui::AimStarDefaultStyle();
        io.LogFilename = nullptr;

        if (!ImGui_ImplWin32_Init(Window.hWnd))
            throw OSException("platform backend init failed.");
        if (!ImGui_ImplDX11_Init(device, device_context))
            throw OSException("renderer backend init failed.");



        return true;
    }

    void OSImGui_Base::CleanImGui()
    {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        g_Device.CleanupDeviceD3D();
        DestroyWindow(Window.hWnd);
        UnregisterClassA(Window.ClassName.c_str(), Window.hInstance);
    }

    std::wstring OSImGui_Base::StringToWstring(std::string& str)
    {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        return converter.from_bytes(str);
    }
}
