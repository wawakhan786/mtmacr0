// C++20, EspTypography.h, Dear ImGui: native-size fonts and compact contrast styles.
#pragma once
#include "../OS-ImGui/imgui/imgui.h"
#include "../../third_party/imgui_freetype/imgui_freetype.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

namespace EspTypography {
enum class Contrast : int { outline = 0, shadow = 1, background = 2 };
enum class Family : int { verdana = 0, inter = 1 };
inline ImFont* fonts[2][4] = {};
inline int active_family = 1;
inline Contrast active_style = Contrast::outline;
inline int background_opacity = 90;
inline int secondary_size = 13;
inline int clamp_size(int value) { return (std::clamp)(value, 12, 15); }
inline ImFont* font_for(float size) {
    if (!std::isfinite(size)) return ImGui::GetFont();
    const int native = clamp_size(static_cast<int>(std::round((std::clamp)(size,12.0f,15.0f))));
    ImFont* font = fonts[(std::clamp)(active_family, 0, 1)][native - 12];
    return font ? font : ImGui::GetFont();
}
inline float native_size(float size) { return static_cast<float>(clamp_size(static_cast<int>(std::isfinite(size) ? std::round((std::clamp)(size,12.0f,15.0f)) : 13))); }
inline void load_fonts(ImFontAtlas* atlas, const char* verdana, const char* inter, void* embedded_inter = nullptr, int embedded_size = 0) {
    atlas->FontBuilderIO = ImGuiFreeType::GetBuilderForFreeType();
    for (int family = 0; family < 2; ++family) {
        const char* path = family == 0 ? verdana : inter;
        for (int size = 12; size <= 15; ++size) {
            ImFontConfig config;
            config.PixelSnapH = true;
            config.OversampleH = config.OversampleV = 1;
            config.FontBuilderFlags = 0; // Native hinting and grayscale antialiasing.
            if (family == 1 && embedded_inter && embedded_size > 0) {
                config.FontDataOwnedByAtlas = false;
                fonts[family][size - 12] = atlas->AddFontFromMemoryTTF(embedded_inter, embedded_size, static_cast<float>(size), &config, atlas->GetGlyphRangesDefault());
                continue;
            }
            fonts[family][size - 12] = path && *path ? atlas->AddFontFromFileTTF(path, static_cast<float>(size), &config, atlas->GetGlyphRangesDefault()) : nullptr;
        }
    }
}
inline ImVec2 measure(const char* value, float size) {
    const float pixels = native_size(size);
    return font_for(pixels)->CalcTextSizeA(pixels, FLT_MAX, 0.0f, value);
}
inline ImVec2 origin(const char* value, ImVec2 p, float size, bool centered) {
    if (centered) p.x -= measure(value, size).x * 0.5f;
    return ImVec2(std::floor(p.x + 0.5f), std::floor(p.y + 0.5f));
}
inline int layers(Contrast style) { return style == Contrast::outline ? 5 : style == Contrast::shadow ? 2 : 1; }
inline void draw(ImDrawList* list, const char* value, ImVec2 p, ImU32 ink, float size,
                 bool centered, Contrast style, int opacity = 90) {
    if (!list || !value || !*value || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(size)) return;
    size = native_size(size);
    auto* font = font_for(size);
    p = origin(value, p, size, centered);
    const unsigned alpha = (ink >> IM_COL32_A_SHIFT) & 255;
    // Almost black also remains visible on the legacy color-key compositor.
    const ImU32 edge = IM_COL32(5, 7, 10, static_cast<int>(alpha * 0.88f));
    list->PushTextureID(font->ContainerAtlas->TexID);
    if (style == Contrast::background && opacity > 0) {
        const auto dimensions = measure(value, size);
        list->AddRectFilled(ImVec2(p.x - 2, p.y - 1), ImVec2(p.x + dimensions.x + 2, p.y + dimensions.y + 1),
            IM_COL32(18, 20, 25, (std::clamp)(opacity, 0, 255) * alpha / 255), 2);
    } else if (style == Contrast::outline) {
        const ImVec2 offsets[] = { ImVec2(-1,0), ImVec2(1,0), ImVec2(0,-1), ImVec2(0,1) };
        for (const auto& offset : offsets) list->AddText(font, size, ImVec2(p.x + offset.x, p.y + offset.y), edge, value);
    } else if (style == Contrast::shadow) {
        list->AddText(font, size, ImVec2(p.x + 1, p.y + 1), edge, value);
    }
    list->AddText(font, size, p, ink, value);
    list->PopTextureID();
}
inline void draw(ImDrawList* list, const char* value, ImVec2 p, ImU32 ink, float size, bool centered = false) {
    draw(list, value, p, ink, size, centered, active_style, background_opacity);
}
}
