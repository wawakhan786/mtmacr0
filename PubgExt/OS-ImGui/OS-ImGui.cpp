#pragma once
#include "../Perf.h"   // contadores de AddText / CalcTextSizeA
#include "OS-ImGui.h"
#include "../UI/EspTypography.h"
#include <Shellapi.h>
#include <Windows.h>
#include <cmath>   // std::isfinite (guardas de coordenada — ver NotFinito abaixo)

#define STB_IMAGE_IMPLEMENTATION
#include "stb\stb_image.h"


// OS-ImGui Draw
namespace RenderCore
{
    // =====================================================================
    // [DRAW SANITY] GUARDA DE COORDENADA NAO-FINITA
    //
    // POR QUE: o 0xC0000005 do dump RuntimeBroker.exe.15084 terminou dentro do
    // ImGui porque uma coordenada inf/NaN (de uma projecao de jogador) chegou
    // no AddRect do ESP. Aqui e' o ULTIMO ponto antes do ImGui: se qualquer
    // primitiva receber NaN/+-inf, ela simplesmente NAO desenha.
    //
    // Isso e' o que impede o estrago de virar crash: dentro do ImGui,
    // `NaN < 0.5f` e' FALSO, entao um retangulo de tamanho NaN entra no caminho
    // do canto ARREDONDADO (PathRect:1484 -> PathArcToFast -> _PathArcToFastEx
    // -> _CalcCircleAutoSegmentCount) e la' o `(int)(radius + 0.999999f)` vira
    // INT_MIN -> `CircleSegmentCounts[INT_MIN]` -> leitura fora da tabela.
    //
    // CUSTO: 1 comparacao por primitiva. Sem alocacao, sem log, sem excecao.
    // O log de diagnostico ([DRAW] rejeitado) fica no lado do ESP
    // (ESP/DrawSanity.h), que e' quem conhece a posicao de mundo e o tipo.
    //
    // NAO muda o menu nem o ESP normal: coordenada valida passa direto.
    // =====================================================================
    static inline bool CfgFinito(float v) { return std::isfinite(v); }

    static inline bool CfgFinito2(float a, float b) { return std::isfinite(a) && std::isfinite(b); }

    bool OSImGui::LoadTextureFromFile(const char* filename, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height)
    { //shit coded by sysR@M
        int image_width = 0;
        int image_height = 0;
        unsigned char* image_data = stbi_load(filename, &image_width, &image_height, NULL, 4);
        if (image_data == NULL)
            return false;
        D3D11_TEXTURE2D_DESC desc;
        ZeroMemory(&desc, sizeof(desc));
        desc.Width = image_width;
        desc.Height = image_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        ID3D11Texture2D* pTexture = NULL;
        D3D11_SUBRESOURCE_DATA subResource;
        subResource.pSysMem = image_data;
        subResource.SysMemPitch = desc.Width * 4;
        subResource.SysMemSlicePitch = 0;
        g_Device.g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture);
        // Create texture view
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
        ZeroMemory(&srvDesc, sizeof(srvDesc));
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = desc.MipLevels;
        srvDesc.Texture2D.MostDetailedMip = 0;
        g_Device.g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
        pTexture->Release();
        *out_width = image_width;
        *out_height = image_height;
        stbi_image_free(image_data);
        return true;
    }

    bool OSImGui::LoadTextureFromMemory(unsigned char* Memory, UINT size, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height)
    {
        int image_width = 0;
        int image_height = 0;
        unsigned char* image_data = stbi_load_from_memory(Memory, size, &image_width, &image_height, NULL, 4);
        if (image_data == NULL)
            return false;
        D3D11_TEXTURE2D_DESC desc;
        ZeroMemory(&desc, sizeof(desc));
        desc.Width = image_width;
        desc.Height = image_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        ID3D11Texture2D* pTexture = NULL;
        D3D11_SUBRESOURCE_DATA subResource;
        subResource.pSysMem = image_data;
        subResource.SysMemPitch = desc.Width * 4;
        subResource.SysMemSlicePitch = 0;
        g_Device.g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture);
        // Create texture view
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
        ZeroMemory(&srvDesc, sizeof(srvDesc));
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = desc.MipLevels;
        srvDesc.Texture2D.MostDetailedMip = 0;
        g_Device.g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
        pTexture->Release();
        *out_width = image_width;
        *out_height = image_height;
        stbi_image_free(image_data);
        return true;
    }

    void OSImGui::ShowRawImage(unsigned char* image, int width, int height)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return;

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 pos = ImGui::GetCursorScreenPos();

        // Render image
        draw_list->AddImage(
            (void*)image,
            ImVec2(pos.x + width, pos.y + height),
            ImVec2(0, 0),
            ImVec2(1, 1)
        );
        // Update cursor pos
        ImGui::Dummy(ImVec2(width, height));
    }

    void OSImGui::SwitchButton(const char* str_id, bool* v)
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        float Height = ImGui::GetFrameHeight();
        // 28/10: trilho CURTO (18px) e centralizado na altura do frame — o
        // pill de altura cheia ficava como tijolo cinza dentro do painel.
        float TrackH = ImMin(Height, 18.0f);
        float Width  = TrackH * 1.9f;
        const float ty = p.y + (Height - TrackH) * 0.5f;
        const float Radius = TrackH * 0.5f - 3.0f;   // botao com folga no trilho

        ImGui::InvisibleButton(str_id, ImVec2(Width, Height));
        if (ImGui::IsItemClicked())
            *v = !(*v);
        // Animation
        float t = *v ? 1.0f : 0.f;
        ImGuiContext& g = *GImGui;
        float AnimationSpeed = 0.18f;   // 0.08 era quase instantaneo — nao lia
        if (g.LastActiveId == g.CurrentWindow->GetID(str_id))
        {
            float T_Animation = ImSaturate(g.LastActiveIdTimer / AnimationSpeed);
            t = *v ? (T_Animation) : (1.0f - T_Animation);
        }
        // Cores: OFF = trilho escuro + botao cinza-azulado; ON = trilho roxo
        // de acento (#8B5CF6) + botao quase branco. Antes o botao era um
        // lerp do Border (quase preto) sobre trilho cinza 87 — apagado no
        // painel escuro, e o acento nunca aparecia num toggle ligado.
        ImU32 corTrilho = ImGui::GetColorU32(
            ImLerp(ImVec4(0.145f, 0.165f, 0.235f, 1.0f), ImVec4(0.545f, 0.361f, 0.965f, 1.0f), t));
        ImU32 corBotao = ImGui::GetColorU32(
            ImLerp(ImVec4(0.550f, 0.580f, 0.660f, 1.0f), ImVec4(0.960f, 0.960f, 0.980f, 1.0f), t));
        // Rendering
        DrawList->AddRectFilled(ImVec2(p.x, ty), ImVec2(p.x + Width, ty + TrackH), corTrilho, TrackH);
        DrawList->AddRect(ImVec2(p.x + 0.5f, ty + 0.5f), ImVec2(p.x + Width - 0.5f, ty + TrackH - 0.5f),
                          IM_COL32(255, 255, 255, 18), TrackH);   // hairline p/ contorno no colorkey
        DrawList->AddCircleFilled(ImVec2(p.x + Radius + t * (Width - Radius * 2), ty + TrackH * 0.5f),
                                  Radius, corBotao, 360);

        ImGui::SameLine();
        ImGui::Text(str_id);
    }

    void OSImGui::MyProgressBar(float fraction, const ImVec2& Size, const char* overlay, ImVec4 Color)
    {
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Color);
        ImGui::ProgressBar(fraction, Size, overlay);
        ImGui::PopStyleColor();
    }

    void OSImGui::OpenWebpage(const char* url)
    {
        ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
    }

    void OSImGui::OpenWebpageButton(const char* label, const char* url)
    {
        if (ImGui::Button(label))
            OpenWebpage(url);
    }

    void OSImGui::MyText(const std::string& Text, bool isCenter) {
        auto windowWidth = ImGui::GetWindowSize().x;
        auto textWidth = ImGui::CalcTextSize(Text.c_str()).x;
        if (isCenter)
            ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
        ImGui::Text(Text.c_str());
    }

    void OSImGui::Text(const std::string& Text, Vec2 Pos, ImColor Color, float FontSize, bool KeepCenter ,bool IsItem)
    {
        if (!CfgFinito(FontSize)) return;
        FontSize = EspTypography::native_size(FontSize);
        // [GUARDA] Pos/FontSize NAO-FINITOS nao viram texto: com NaN o AddText
        // escreveria vertices NaN e, pior, serviria de entrada para as rotinas
        // de retangulo/circulo que estouram (ver a nota grande no topo).
        if (!CfgFinito2(Pos.x, Pos.y) || !CfgFinito(FontSize))
            return;

        auto Drawlist = ImGui::GetBackgroundDrawList();

		if (IsItem)
            Drawlist = ImGui::GetWindowDrawList();
        if (!KeepCenter)
        {
            Perf::Contar(Perf::C.addText);
            Drawlist->AddText(EspTypography::font_for(FontSize), FontSize, Pos.ToImVec2(), Color, Text.c_str());
        }
        else
        {
            // Medido: cada chamada aqui VARRE a string inteira glifo a glifo.
            Perf::Contar(Perf::C.calcText);
            float TextWidth = (EspTypography::font_for(FontSize))->CalcTextSizeA(FontSize, FLT_MAX, 0.f, Text.c_str()).x;
            ImVec2 Pos_ = { Pos.x - TextWidth / 2,Pos.y };
            Perf::Contar(Perf::C.addText);
            Drawlist->AddText(EspTypography::font_for(FontSize), FontSize, Pos_, Color, Text.c_str());
        }
    }

    // Shared native-size text renderer; retained signature supports existing callers.
    void OSImGui::StrokeText(const std::string& Text, Vec2 Pos, ImColor Color, float FontSize, bool KeepCenter, bool IsItem, int passes)
    {
        if (!CfgFinito2(Pos.x, Pos.y) || !CfgFinito(FontSize)) return;
        // Contrast is a visual choice, never degraded by entity distance.
        (void)passes;
        auto* list = IsItem ? ImGui::GetWindowDrawList() : ImGui::GetBackgroundDrawList();
        EspTypography::draw(list, Text.c_str(), Pos.ToImVec2(), Color, FontSize, KeepCenter);
        for (int i = 0; i < EspTypography::layers(EspTypography::active_style); ++i) Perf::Contar(Perf::C.addText);
    }

    static ImVec2 TextSizeOf(const std::string& Text, ImFont* Font, float FontSize)
    {
        if (Text.empty())
            return ImVec2(0.0f, 0.0f);
        // `Font` e' o MESMO ponteiro que o AddText vai usar: se um dia o Text
        // mudar de fonte, este calculo muda junto (era assim que o Text ja'
        // media a largura pra centralizar).
        return Font->CalcTextSizeA(FontSize, FLT_MAX, 0.0f, Text.c_str());
    }

    static void DrawTextPanel(ImDrawList* list, const ImVec2& min, const ImVec2& max, int opacity)
    {
        if (list && opacity > 0 && max.x > min.x && max.y > min.y)
            list->AddRectFilled(min, max, IM_COL32(18, 20, 25, (std::clamp)(opacity, 0, 255)), 2.0f);
    }

    void OSImGui::TextWithBg(const std::string& Text, Vec2 Pos, ImColor Color, float FontSize, bool KeepCenter, int Opacity, bool IsItem)
    {
        if (!CfgFinito(FontSize)) return;
        FontSize = EspTypography::native_size(FontSize);
        // -----------------------------------------------------------------
        // [GUARDA] FUNDO (E TEXTO) SO' COM Pos NAO-FINITO = NAO DESENHA NADA.
        //
        // Sem isto o retangulo do fundo e' calculado a partir de Pos e do
        // tamanho do texto: Pos NaN/inf -> Min/Max NaN/inf -> AddRectFilled com
        // canto arredondado -> estouro dentro do ImGui (o caminho exato do
        // 0xC0000005 do dump RuntimeBroker.exe.15084).
        //
        // Rejeitar o TEXTO tambem e' proposital: uma Pos absurda significa
        // "fora da tela" — desenhar o texto sem o fundo so' trocaria um
        // retangulo invalido por um texto invalido, que e' o que o fundo devia
        // estar cobrindo.
        // -----------------------------------------------------------------
        if (!CfgFinito2(Pos.x, Pos.y) || !CfgFinito(FontSize))
            return;

        ImDrawList* Drawlist = IsItem ? ImGui::GetWindowDrawList() : ImGui::GetBackgroundDrawList();

        // Fundo primeiro: a ordem no DrawList e' a ordem de desenho.
        if (Opacity > 0 && !Text.empty())
        {
            ImFont*   Font = EspTypography::font_for(FontSize);
            const ImVec2 Size = TextSizeOf(Text, Font, FontSize);

            // Padding: 3 px nas laterais (onde mora o contorno do StrokeText,
            // que sai 1 px pra fora) e 2 px em cima/embaixo.
            // -----------------------------------------------------------------
            // ALINHAMENTO: TOPO do fundo = TOPO do texto.
            //
            // Antes o fundo era CENTRADO em Pos.y (Pos.y - Size.y/2), mas o
            // AddText desenha o texto com Pos.y como TOPO. Numa fonte de 15px
            // o retangulo subia 7,5 px acima e a escrita caia na metade de
            // baixo, escapando por baixo do fundo.
            //
            // Padding: 3 px nas laterais (onde mora o contorno do StrokeText,
            // que sai 1 px pra fora) e 2 px em cima/embaixo.
            // -----------------------------------------------------------------
            // Padding: 5 px nas laterais (onde mora o contorno/sombra, que sai
            // 1 px pra fora) e 3 px em cima/embaixo. Era 3/2 — o texto ficava
            // encostado na borda e o painel parecia apertado.
            const ImVec2 Min(Pos.x - Size.x * 0.5f - 2.0f, Pos.y - 1.0f);
            const ImVec2 Max(Pos.x + Size.x * 0.5f + 2.0f, Pos.y + Size.y + 1.0f);

            DrawTextPanel(Drawlist, Min, Max, Opacity);
        }

        // O texto em si: mesma chamada do Text, pra nao existir duas rotinas de
        // desenho que possam divergir.
        if (!KeepCenter)
        {
            Perf::Contar(Perf::C.addText);
            Drawlist->AddText(EspTypography::font_for(FontSize), FontSize, Pos.ToImVec2(), Color, Text.c_str());
        }
        else
        {
            // Medido: cada chamada aqui VARRE a string inteira glifo a glifo.
            Perf::Contar(Perf::C.calcText);
            float TextWidth = (EspTypography::font_for(FontSize))->CalcTextSizeA(FontSize, FLT_MAX, 0.f, Text.c_str()).x;
            ImVec2 Pos_ = { Pos.x - TextWidth / 2,Pos.y };
            Perf::Contar(Perf::C.addText);
            Drawlist->AddText(EspTypography::font_for(FontSize), FontSize, Pos_, Color, Text.c_str());
        }
    }

    void OSImGui::StrokeTextWithBg(const std::string& Text, Vec2 Pos, ImColor Color, float FontSize, bool KeepCenter, int Opacity, bool IsItem, int passes)
    {
        if (!CfgFinito2(Pos.x, Pos.y) || !CfgFinito(FontSize)) return;
        (void)passes;
        auto* list = IsItem ? ImGui::GetWindowDrawList() : ImGui::GetBackgroundDrawList();
        EspTypography::draw(list, Text.c_str(), Pos.ToImVec2(), Color, FontSize, KeepCenter,
            Opacity > 0 ? EspTypography::Contrast::background : EspTypography::active_style, Opacity);
    }

    void OSImGui::Rectangle(Vec2 Pos, Vec2 Size, ImColor Color, float Thickness, float Rounding, bool IsItem)
    {
        // [GUARDA] Retangulo com coordenada nao-finita NAO desenha.
        //
        // Este e' literalmente o caminho do crash (PlayerEsp.cpp,
        // DrawPlayerEsp -> Gui.Rectangle -> AddRect -> PathRect ->
        // PathArcToFast): com tamanho
        // NaN, `PathRect` calcula `rounding = ImMin(0.0f, NaN) = NaN`, o teste
        // `rounding < 0.5f` da' FALSO (NaN nunca e' menor) e o ImGui entra no
        // caminho do canto arredondado com raio NaN.
        if (!CfgFinito2(Pos.x, Pos.y) || !CfgFinito2(Size.x, Size.y) || !CfgFinito(Rounding) ||
            !CfgFinito(Thickness))
            return;

        auto DrawList = ImGui::GetBackgroundDrawList();

        if (IsItem)
            DrawList = ImGui::GetWindowDrawList();
        DrawList->AddRect(Pos.ToImVec2(), { Pos.x + Size.x,Pos.y + Size.y }, Color, Rounding, 0, Thickness);
    }

    void OSImGui::RectangleFilled(Vec2 Pos, Vec2 Size, ImColor Color, float Rounding, int Nums, bool IsItem)
    {
        // [GUARDA] Este e' o caminho do EspPanelBg (fundo atras do texto do
        // HUD/veiculo/loot/granada). Ele passa Rounding = 3.0 (> 0), entao um
        // Pos NaN cai DIRETO no canto arredondado — mesma armadilha do crash.
        if (!CfgFinito2(Pos.x, Pos.y) || !CfgFinito2(Size.x, Size.y) || !CfgFinito(Rounding))
            return;

        auto DrawList = ImGui::GetBackgroundDrawList();

        if (IsItem)
            DrawList = ImGui::GetWindowDrawList();
        ImDrawFlags rounding_corners = ImDrawCornerFlags_All;
        ImVec2 a = Pos.ToImVec2();
        ImVec2 b = { Pos.x + Size.x,Pos.y + Size.y };
        Rounding = ImMin<float>(Rounding, fabsf(Size.x) * (((rounding_corners & ImDrawCornerFlags_Top) == ImDrawCornerFlags_Top) || ((rounding_corners & ImDrawCornerFlags_Bot) == ImDrawCornerFlags_Bot) ? 0.5f : 1.0f) - 1.0f);
        Rounding = ImMin<float>(Rounding, fabsf(Size.y) * (((rounding_corners & ImDrawCornerFlags_Left) == ImDrawCornerFlags_Left) || ((rounding_corners & ImDrawCornerFlags_Right) == ImDrawCornerFlags_Right) ? 0.5f : 1.0f) - 1.0f);

        if (Rounding <= 0.0f || rounding_corners == 0)
        {
            DrawList->PathLineTo(a);
            DrawList->PathLineTo(ImVec2(b.x, a.y));
            DrawList->PathLineTo(b);
            DrawList->PathLineTo(ImVec2(a.x, b.y));
        }
        else
        {
            DrawList->PathArcTo(ImVec2(a.x + Rounding, a.y + Rounding), Rounding, IM_PI, IM_PI / 2.f * 3.f, Nums);
            DrawList->PathArcTo(ImVec2(b.x - Rounding, a.y + Rounding), Rounding, IM_PI / 2.f * 3.f, IM_PI * 2.f, Nums);
            DrawList->PathArcTo(ImVec2(b.x - Rounding, b.y - Rounding), Rounding, 0.f, IM_PI / 2.f, Nums);
            DrawList->PathArcTo(ImVec2(a.x + Rounding, b.y - Rounding), Rounding, IM_PI / 2.f, IM_PI, Nums);
        }
        DrawList->PathFillConvex(Color);
    }

    void OSImGui::RectangleFilledGraident(Vec2 Pos, Vec2 Size, ImColor BgColor, ImColor TopColor, ImColor BotColor, float Rounding, int Nums, bool IsItem)
    {
        auto DrawList = ImGui::GetBackgroundDrawList();

        if (IsItem)
            DrawList = ImGui::GetWindowDrawList();
        ImDrawFlags rounding_corners = ImDrawCornerFlags_All;
        ImVec2 a = Pos.ToImVec2();
        ImVec2 b = { Pos.x + Size.x,Pos.y + Size.y };

        Rounding = ImMin<float>(Rounding, fabsf(Size.x) * (((rounding_corners & ImDrawCornerFlags_Top) == ImDrawCornerFlags_Top) || ((rounding_corners & ImDrawCornerFlags_Bot) == ImDrawCornerFlags_Bot) ? 0.5f : 1.0f) - 1.0f);
        Rounding = ImMin<float>(Rounding, fabsf(Size.y) * (((rounding_corners & ImDrawCornerFlags_Left) == ImDrawCornerFlags_Left) || ((rounding_corners & ImDrawCornerFlags_Right) == ImDrawCornerFlags_Right) ? 0.5f : 1.0f) - 1.0f);

        DrawList->AddRectFilledMultiColor(a, b, TopColor, TopColor, BotColor, BotColor, Rounding, rounding_corners);
    }

    void OSImGui::Line(Vec2 From, Vec2 To, ImColor Color, float Thickness, bool IsItem)
    {
        // [GUARDA] Snapline/skeleton com ponta nao-finita = linha nao desenhada.
        // (Aqui NAO ha' canto arredondado, entao o ImGui nao estoura; mas uma
        // coordenada NaN contamina o buffer de vertices do frame inteiro.)
        if (!CfgFinito2(From.x, From.y) || !CfgFinito2(To.x, To.y) || !CfgFinito(Thickness))
            return;

        auto DrawList = ImGui::GetBackgroundDrawList();

        if (IsItem)
            DrawList = ImGui::GetWindowDrawList();
        DrawList->AddLine(From.ToImVec2(), To.ToImVec2(), Color, Thickness);
    }

    void OSImGui::Circle(Vec2 Center, float Radius, ImColor Color, float Thickness, int Num)
    {
        // [GUARDA] Centro/Raio nao-finitos = nao desenha. O raio e' o pior dos
        // casos: `_CalcCircleAutoSegmentCount(raio)` faz `(int)(raio + 0.999999f)`
        // e para NaN/inf isso da' INT_MIN -> indice negativo na tabela
        // CircleSegmentCounts (64 bytes) dentro do ImGui.
        if (!CfgFinito2(Center.x, Center.y) || !CfgFinito(Radius) || !CfgFinito(Thickness))
            return;

        ImGui::GetBackgroundDrawList()->AddCircle(Center.ToImVec2(), Radius, Color, Num, Thickness);
    }

    void OSImGui::CircleFilled(Vec2 Center, float Radius, ImColor Color, int Num)
    {
        // [GUARDA] Ver a nota do Circle acima (mesma tabela de segmentos).
        if (!CfgFinito2(Center.x, Center.y) || !CfgFinito(Radius))
            return;

        ImGui::GetBackgroundDrawList()->AddCircleFilled(Center.ToImVec2(), Radius, Color, Num);
    }

    void OSImGui::ConnectPoints(std::vector<Vec2> Points, ImColor Color, float Thickness)
    {
        if (Points.size() <= 0)
            return;
        for (int i = 0; i < Points.size() - 1; i++)
        {
            Line(Points[i], Points[i + 1], Color, Thickness);
            if (i == Points.size() - 2)
                Line(Points[i + 1], Points[0], Color, Thickness);
        }
    }

    void OSImGui::Arc(ImVec2 Center, float Radius, ImColor Color, float Thickness, float Angle_begin, float Angle_end, float Nums)
    {
        ImDrawList* DrawList = ImGui::GetBackgroundDrawList();
        float angle = (Angle_end - Angle_begin) / Nums;
        for (int i = 0; i < Nums; i++)
        {
            float angle_ = i * angle + Angle_begin - IM_PI / 2;
            DrawList->PathLineTo({ Center.x - Radius * cos(angle_),Center.y - Radius * sin(angle_) });
        }
        DrawList->PathStroke(Color, false, Thickness);
    }

    void OSImGui::MyCheckBox(const char* str_id, bool* v)
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        float Height = ImGui::GetFrameHeight();
        float Width = Height * 1.7f;
        float Radius = Height / 2 - 2;

        ImGui::InvisibleButton(str_id, ImVec2(Width, Height));
        if (ImGui::IsItemClicked())
            *v = !(*v);
        // ����ƶ�����
        float t = *v ? 1.0f : 0.f;
        ImGuiContext& g = *GImGui;
        float AnimationSpeed = 0.08f;
        if (g.LastActiveId == g.CurrentWindow->GetID(str_id))
        {
            float T_Animation = ImSaturate(g.LastActiveIdTimer / AnimationSpeed);
            t = *v ? (T_Animation) : (1.0f - T_Animation);
        }
        // �����ͣ��ɫ
        ImU32 Color;
        if (ImGui::IsItemHovered())
            Color = ImGui::GetColorU32(ImLerp(ImVec4(0.85f, 0.24f, 0.15f, 1.0f), ImVec4(0.55f, 0.85f, 0.13f, 1.000f), t));
        else
            Color = ImGui::GetColorU32(ImLerp(ImVec4(0.90f, 0.29f, 0.20f, 1.0f), ImVec4(0.60f, 0.90f, 0.18f, 1.000f), t));
        // �������
        DrawList->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + Width, p.y + Height), Color, Height);
        DrawList->AddCircleFilled(ImVec2(p.x + Radius + t * (Width - Radius * 2) + (t == 0 ? 2 : -2), p.y + Radius + 2), Radius, IM_COL32(255, 255, 255, 255), 360);
        DrawList->AddCircle(ImVec2(p.x + Radius + t * (Width - Radius * 2) + (t == 0 ? 2 : -2), p.y + Radius + 2), Radius, IM_COL32(20, 20, 20, 80), 360, 1);

        ImGui::SameLine();
        ImGui::Text(str_id);
    }

    void OSImGui::MyCheckBox2(const char* str_id, bool* v)
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        float Height = ImGui::GetFrameHeight();
        float Width = Height * 1.7f;
        float Radius = Height / 2 - 2;

        ImGui::InvisibleButton(str_id, ImVec2(Width, Height));
        if (ImGui::IsItemClicked())
            *v = !(*v);
        // ����ƶ�����
        float t = *v ? 1.0f : 0.f;
        ImGuiContext& g = *GImGui;
        float AnimationSpeed = 0.15f;
        if (g.LastActiveId == g.CurrentWindow->GetID(str_id))
        {
            float T_Animation = ImSaturate(g.LastActiveIdTimer / AnimationSpeed);
            t = *v ? (T_Animation) : (1.0f - T_Animation);
        }
        // �����ͣ��ɫ
        ImU32 Color;
        if (ImGui::IsItemHovered())
            Color = ImGui::GetColorU32(ImLerp(ImVec4(0.08f, 0.18f, 0.21f, 1.0f), ImVec4(0.10f, 0.48f, 0.68f, 1.000f), t));
        else
            Color = ImGui::GetColorU32(ImLerp(ImVec4(0.12f, 0.22f, 0.25f, 1.0f), ImVec4(0.14f, 0.52f, 0.72f, 1.000f), t));
        // �������
        DrawList->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + Width, p.y + Height), Color, 360);
        DrawList->AddCircleFilled(ImVec2(p.x + Radius + 2 + t * (Width - (Radius + 2) * 2), p.y + Radius + 2), Radius + 2, IM_COL32(255, 255, 255, 255), 360);
        DrawList->AddCircleFilled(ImVec2(p.x + Radius + t * (Width - Radius * 2) + (t == 0 ? 2 : -2), p.y + Radius + 2), Radius, IM_COL32(230, 230, 230, 255), 360);
        if (*v)
            DrawList->AddText(ImVec2(p.x + 45, p.y + 2), ImColor{ 255,255,255,255 }, str_id);
        else
            DrawList->AddText(ImVec2(p.x + 45, p.y + 2), ImColor{ 185,185,185,255 }, str_id);

    }

    void OSImGui::MyCheckBox3(const char* str_id, bool* v)
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        float Height = ImGui::GetFrameHeight();
        float Width = Height;
        float Left = 8;
        float Right = Left * 1.5f;
        ImGui::InvisibleButton(str_id, ImVec2(Width, Height));

        if (ImGui::IsItemClicked())
            *v = !(*v);
        // ����ƶ�����
        float t = *v ? 1.0f : 0.f;
        ImGuiContext& g = *GImGui;
        float AnimationSpeed = 0.12f;
        if (g.LastActiveId == g.CurrentWindow->GetID(str_id))
        {
            float T_Animation = ImSaturate(g.LastActiveIdTimer / AnimationSpeed);
            t = *v ? (T_Animation) : (1.0f - T_Animation);
        }
        // �����ͣ��ɫ
        ImU32 Color;
        ImU32 TickColor1, TickColor2;
        if (ImGui::IsItemHovered())
            Color = ImGui::GetColorU32(ImLerp(ImVec4(0.75f, 0.75f, 0.75f, 1.0f), ImVec4(0.05f, 0.85f, 0.25f, 1.000f), t));
        else
            Color = ImGui::GetColorU32(ImLerp(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), ImVec4(0.1f, 0.9f, 0.3f, 1.000f), t));

        TickColor1 = IM_COL32(255, 255, 255, 255 * t);
        TickColor2 = IM_COL32(180, 180, 180, 255 * (1 - t));

        float Size = Width;
        float Scale = (float)(Size) / 20.0f;
        // ��ɫ
        DrawList->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + Width, p.y + Height), Color, 5, 15);
        // ѡ�й�
        DrawList->AddLine(ImVec2(p.x + 3 * Scale, p.y + Size / 2 - 2 * Scale), ImVec2(p.x + Size / 2 - 1 * Scale, p.y + Size - 5 * Scale), TickColor1, 3 * Scale);
        DrawList->AddLine(ImVec2(p.x + Size - 3 * Scale - 1, p.y + 3 * Scale + 1), ImVec2(p.x + Size / 2 - 1 * Scale, p.y + Size - 5 * Scale), TickColor1, 3 * Scale);
        // δѡ�й�
        DrawList->AddLine(ImVec2(p.x + 3 * Scale, p.y + Size / 2 - 2 * Scale), ImVec2(p.x + Size / 2 - 1 * Scale, p.y + Size - 5 * Scale), TickColor2, 3 * Scale);
        DrawList->AddLine(ImVec2(p.x + Size - 3 * Scale - 1, p.y + 3 * Scale + 1), ImVec2(p.x + Size / 2 - 1 * Scale, p.y + Size - 5 * Scale), TickColor2, 3 * Scale);
        ImGui::SameLine();
        ImGui::Text(str_id);
    }

    void OSImGui::MyCheckBox4(const char* str_id, bool* v)
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        float Height = ImGui::GetFrameHeight();
        float Width = Height;
        ImGui::InvisibleButton(str_id, ImVec2(Width, Height));

        if (ImGui::IsItemClicked())
            *v = !(*v);
        // �������
        float t = *v ? 1.0f : 0.f;
        ImGuiContext& g = *GImGui;
        float AnimationSpeed = 0.12f;
        if (g.LastActiveId == g.CurrentWindow->GetID(str_id))
        {
            float T_Animation = ImSaturate(g.LastActiveIdTimer / AnimationSpeed);
            t = *v ? (T_Animation) : (1.0f - T_Animation);
        }
        // bg 0.74 0.72 0.81-> 0.69 0.77 0.76
        ImU32 BgColor;
        if (ImGui::IsItemHovered())
            BgColor = ImGui::GetColorU32(ImVec4(0.69f, 0.69f, 0.69f, 1.0f));
        else
            BgColor = ImGui::GetColorU32(ImVec4(0.74f, 0.74f, 0.74f, 1.0f));
        DrawList->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + Width, p.y + Width), BgColor);

        ImU32 FrColor;
        FrColor = ImGui::GetColorU32(ImVec4(0.f, 0.f, 0.f, 0.5f * t));
        DrawList->AddRectFilled(ImVec2(p.x + Width / 5, p.y + Width / 5), ImVec2(p.x + Width - Width / 5, p.y + Width - Width / 5), FrColor);

        ImGui::SameLine();
        ImGui::Text(str_id);
    }

    void OSImGui::ShadowRectFilled(Vec2 Pos, Vec2 Size, ImColor RectColor, ImColor ShadowColor, float ShadowThickness, Vec2 ShadowOffset, float Rounding)
    {
        ImDrawList* DrawList = ImGui::GetBackgroundDrawList();
        ImDrawFlags Flags = (Rounding > 0.f) ? ImDrawFlags_RoundCornersMask_ : ImDrawFlags_None;
        DrawList->AddShadowRect(Pos.ToImVec2(), { Pos.x + Size.x,Pos.y + Size.y }, ShadowColor, ShadowThickness, ShadowOffset.ToImVec2(), Flags, Rounding);
        DrawList->AddRectFilled(Pos.ToImVec2(), { Pos.x + Size.x,Pos.y + Size.y }, RectColor, Rounding);
    }

    void OSImGui::ShadowCircle(Vec2 Pos, float Radius, ImColor CircleColor, ImColor ShadowColor, float ShadowThickness, Vec2 ShadowOffset, int Num)
    {
        ImDrawList* DrawList = ImGui::GetBackgroundDrawList();
        DrawList->AddShadowCircle(Pos.ToImVec2(), Radius, ShadowColor, ShadowThickness, ShadowOffset.ToImVec2(), ImDrawFlags_None, Num);
        DrawList->AddCircleFilled(Pos.ToImVec2(), Radius, CircleColor, Num);
    }

    bool OSImGui::SliderScalarEx1(const char* label, ImGuiDataType data_type, void* p_data, const void* p_min, const void* p_max, const char* format, ImGuiSliderFlags flags)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return false;

        ImGuiContext& g = *GImGui;
        const ImGuiStyle& style = g.Style;
        const ImGuiID id = window->GetID(label);
        const float w = ImGui::CalcItemWidth();

        const ImVec2 label_size = ImGui::CalcTextSize(label, NULL, true);
        const ImRect frame_bb(window->DC.CursorPos, (Vec2(window->DC.CursorPos) + Vec2(w, label_size.y + style.FramePadding.y * 2.0f)).ToImVec2());
        const ImRect total_bb(frame_bb.Min, (Vec2(frame_bb.Max) + Vec2(label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f, 0.0f)).ToImVec2());

        const bool temp_input_allowed = (flags & ImGuiSliderFlags_NoInput) == 0;
        ImGui::ItemSize(total_bb, style.FramePadding.y);
        if (!ImGui::ItemAdd(total_bb, id, &frame_bb, temp_input_allowed ? ImGuiItemFlags_Inputable : 0))
            return false;

        // Default format string when passing NULL
        if (format == NULL)
            format = ImGui::DataTypeGetInfo(data_type)->PrintFmt;

        const bool hovered = ImGui::ItemHoverable(frame_bb, id, NULL);
        bool temp_input_is_active = temp_input_allowed && ImGui::TempInputIsActive(id);
        if (!temp_input_is_active)
        {
            // Tabbing or CTRL-clicking on Slider turns it into an input box
            const bool input_requested_by_tabbing = temp_input_allowed && (g.LastItemData.StatusFlags & ImGuiItemStatusFlags_FocusedByTabbing) != 0;
            const bool clicked = hovered && ImGui::IsMouseClicked(0, id);
            const bool make_active = (input_requested_by_tabbing || clicked || g.NavActivateId == id);
            if (make_active && clicked)
                ImGui::SetKeyOwner(ImGuiKey_MouseLeft, id);
            if (make_active && temp_input_allowed)
                if (input_requested_by_tabbing || (clicked && g.IO.KeyCtrl) || (g.NavActivateId == id && (g.NavActivateFlags & ImGuiActivateFlags_PreferInput)))
                    temp_input_is_active = true;

            if (make_active && !temp_input_is_active)
            {
                ImGui::SetActiveID(id, window);
                ImGui::SetFocusID(id, window);
                ImGui::FocusWindow(window);
                g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
            }
        }

        if (temp_input_is_active)
        {
            // Only clamp CTRL+Click input when ImGuiSliderFlags_AlwaysClamp is set
            const bool is_clamp_input = (flags & ImGuiSliderFlags_AlwaysClamp) != 0;
            return ImGui::TempInputScalar(frame_bb, id, label, data_type, p_data, format, is_clamp_input ? p_min : NULL, is_clamp_input ? p_max : NULL);
        }

        float grab_radius = 8;

        // Draw frame
        ImRect frame_sc = frame_bb;
        float frame_height_origin = frame_sc.GetHeight();
        frame_sc.Min.y += frame_height_origin / 3;
        frame_sc.Max.y -= frame_height_origin / 3;
        const ImU32 frame_col = ImGui::GetColorU32(g.ActiveId == id ? ImGuiCol_FrameBgActive : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
        ImGui::RenderNavHighlight(frame_bb, id);
        window->DrawList->AddRectFilled(frame_sc.Min, frame_sc.Max, frame_col, grab_radius);

        // Slider behavior
        ImRect grab_bb;
        const bool value_changed = ImGui::SliderBehavior(frame_bb, id, data_type, p_data, p_min, p_max, format, flags, &grab_bb);
        if (value_changed)
            ImGui::MarkItemEdited(id);

        // Render grab
        if (grab_bb.Max.x > grab_bb.Min.x)
        {
            window->DrawList->AddShadowCircle(grab_bb.GetCenter(), grab_radius, ImColor(30, 30, 30, 255), 9, { 0,0 });
            window->DrawList->AddCircleFilled(grab_bb.GetCenter(), grab_radius, ImColor(220, 220, 220, 255));
        }

        // Display value using user-provided display format so user can add prefix/suffix/decorations to the value.
        char value_buf[64];
        const char* value_buf_end = value_buf + ImGui::DataTypeFormatString(value_buf, IM_ARRAYSIZE(value_buf), data_type, p_data, format);
        if (g.LogEnabled)
            ImGui::LogSetNextTextDecoration("{", "}");
        ImGui::RenderTextClipped(frame_bb.Min, frame_bb.Max, value_buf, value_buf_end, NULL, ImVec2(0.5f, 0.5f));


        // label
        if (label_size.x > 0.0f)
            ImGui::RenderText(ImVec2(frame_bb.Max.x + style.ItemInnerSpacing.x, frame_bb.Min.y + style.FramePadding.y), label);


        IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags | (temp_input_allowed ? ImGuiItemStatusFlags_Inputable : 0));
        return value_changed;
    }

    bool OSImGui::SliderScalarEx2(const char* label, ImGuiDataType data_type, void* p_data, const void* p_min, const void* p_max, const char* format, ImGuiSliderFlags flags)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return false;

        ImGuiContext& g = *GImGui;
        const ImGuiStyle& style = g.Style;
        const ImGuiID id = window->GetID(label);
        const float w = ImGui::CalcItemWidth();

        const ImVec2 label_size = ImGui::CalcTextSize(label, NULL, true);
        const ImRect frame_bb(window->DC.CursorPos, (Vec2(window->DC.CursorPos) + Vec2(w, label_size.y + style.FramePadding.y * 2.0f)).ToImVec2());
        const ImRect total_bb(frame_bb.Min, (Vec2(frame_bb.Max) + Vec2(label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f, 0.0f)).ToImVec2());

        const bool temp_input_allowed = (flags & ImGuiSliderFlags_NoInput) == 0;
        ImGui::ItemSize(total_bb, style.FramePadding.y);
        if (!ImGui::ItemAdd(total_bb, id, &frame_bb, temp_input_allowed ? ImGuiItemFlags_Inputable : 0))
            return false;

        // Default format string when passing NULL
        if (format == NULL)
            format = ImGui::DataTypeGetInfo(data_type)->PrintFmt;

        const bool hovered = ImGui::ItemHoverable(frame_bb, id, NULL);
        bool temp_input_is_active = temp_input_allowed && ImGui::TempInputIsActive(id);
        if (!temp_input_is_active)
        {
            // Tabbing or CTRL-clicking on Slider turns it into an input box
            const bool input_requested_by_tabbing = temp_input_allowed && (g.LastItemData.StatusFlags & ImGuiItemStatusFlags_FocusedByTabbing) != 0;
            const bool clicked = hovered && ImGui::IsMouseClicked(0, id);
            const bool make_active = (input_requested_by_tabbing || clicked || g.NavActivateId == id);
            if (make_active && clicked)
                ImGui::SetKeyOwner(ImGuiKey_MouseLeft, id);
            if (make_active && temp_input_allowed)
                if (input_requested_by_tabbing || (clicked && g.IO.KeyCtrl) || (g.NavActivateId == id && (g.NavActivateFlags & ImGuiActivateFlags_PreferInput)))
                    temp_input_is_active = true;

            if (make_active && !temp_input_is_active)
            {
                ImGui::SetActiveID(id, window);
                ImGui::SetFocusID(id, window);
                ImGui::FocusWindow(window);
                g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
            }
        }

        if (temp_input_is_active)
        {
            // Only clamp CTRL+Click input when ImGuiSliderFlags_AlwaysClamp is set
            const bool is_clamp_input = (flags & ImGuiSliderFlags_AlwaysClamp) != 0;
            return ImGui::TempInputScalar(frame_bb, id, label, data_type, p_data, format, is_clamp_input ? p_min : NULL, is_clamp_input ? p_max : NULL);
        }

        float grab_radius = 8;

        // Draw frame
        ImRect frame_sc = frame_bb;
        float frame_height_origin = frame_sc.GetHeight();
        frame_sc.Min.y += frame_height_origin / 3;
        frame_sc.Max.y -= frame_height_origin / 3;
        const ImU32 frame_col = ImGui::ColorConvertFloat4ToU32(g.ActiveId == id ? ImColor(60, 60, 60, 255) : hovered ? ImColor(130, 130, 130, 255) : ImColor(87, 87, 87, 255));
        ImGui::RenderNavHighlight(frame_bb, id);
        window->DrawList->AddRectFilled(frame_sc.Min, frame_sc.Max, frame_col, grab_radius);


        
        // Slider behavior
        ImRect grab_bb;
        const bool value_changed = ImGui::SliderBehavior(frame_bb, id, data_type, p_data, p_min, p_max, format, flags, &grab_bb);
        if (value_changed)
            ImGui::MarkItemEdited(id);

        window->DrawList->AddRectFilled(frame_sc.Min, ImVec2(grab_bb.Max.x , frame_sc.Max.y), ImGui::GetColorU32(ImGuiCol_Border), grab_radius);

        // Render grab
        if (grab_bb.Max.x > grab_bb.Min.x)
        {
            window->DrawList->AddRectFilled(
                { grab_bb.GetCenter().x - grab_radius * 1.5f, grab_bb.GetCenter().y - grab_radius },
                { grab_bb.GetCenter().x + grab_radius * 1.5f, grab_bb.GetCenter().y + grab_radius },
                ImColor(220, 220, 220, 255), 3);
            window->DrawList->AddLine(
                { grab_bb.GetCenter().x - grab_radius * 0.5f - 1, grab_bb.GetCenter().y - grab_radius * 0.75f },
                { grab_bb.GetCenter().x - grab_radius * 0.5f - 1, grab_bb.GetCenter().y + grab_radius * 0.75f },
                ImColor(150, 150, 150, 255), 1.3f);
            window->DrawList->AddLine(
                { grab_bb.GetCenter().x - 1, grab_bb.GetCenter().y - grab_radius * 0.75f },
                { grab_bb.GetCenter().x - 1, grab_bb.GetCenter().y + grab_radius * 0.75f },
                ImColor(150, 150, 150, 255), 1.3f);
            window->DrawList->AddLine(
                { grab_bb.GetCenter().x + grab_radius * 0.5f - 1, grab_bb.GetCenter().y - grab_radius * 0.75f },
                { grab_bb.GetCenter().x + grab_radius * 0.5f - 1, grab_bb.GetCenter().y + grab_radius * 0.75f },
                ImColor(150, 150, 150, 255), 1.3f);
        }

        // Display value using user-provided display format so user can add prefix/suffix/decorations to the value.
        char value_buf[64];
        const char* value_buf_end = value_buf + ImGui::DataTypeFormatString(value_buf, IM_ARRAYSIZE(value_buf), data_type, p_data, format);
        if (g.LogEnabled)
            ImGui::LogSetNextTextDecoration("{", "}");
        ImGui::RenderTextClipped(frame_bb.Min, frame_bb.Max, value_buf, value_buf_end, NULL, ImVec2(0.5f, 0.5f));


        // label
        if (label_size.x > 0.0f)
            ImGui::RenderText(ImVec2(frame_bb.Max.x + style.ItemInnerSpacing.x, frame_bb.Min.y + style.FramePadding.y), label);


        IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags | (temp_input_allowed ? ImGuiItemStatusFlags_Inputable : 0));
        return value_changed;
    }
}

