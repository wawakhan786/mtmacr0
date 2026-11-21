// C++20, EmbeddedFonts.h: Windows executable font resources for Dear ImGui.
#pragma once
#include <Windows.h>
#include "../OS-ImGui/imgui/imgui.h"
namespace Ui {
struct FontResource { void* data = nullptr; int size = 0; };
inline FontResource embedded_font(int id) {
    const auto module = GetModuleHandleW(nullptr);
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) return {};
    const auto handle = LoadResource(module, resource);
    return {handle ? LockResource(handle) : nullptr, static_cast<int>(SizeofResource(module, resource))};
}
inline ImFont* add_embedded_font(ImFontAtlas* atlas, int id, float size, ImFontConfig config = {}) {
    const auto resource = embedded_font(id);
    if (!resource.data || resource.size <= 0) return nullptr;
    config.FontDataOwnedByAtlas = false; // Resource memory belongs to the executable.
    return atlas->AddFontFromMemoryTTF(resource.data, resource.size, size, &config, atlas->GetGlyphRangesDefault());
}
}
