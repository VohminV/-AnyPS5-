#define IMGUI_DEFINE_MATH_OPERATORS
#include "Theme.hpp"
#include "imgui_internal.h"
#include <windows.h>

namespace launcher::theme {
namespace {
std::vector<Toast> g_toasts;
}

void ApplyGraphiteStyle(float dpi) {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 12.0f * dpi;
    s.ChildRounding = 10.0f * dpi;
    s.FrameRounding = 8.0f * dpi;
    s.GrabRounding = 8.0f * dpi;
    s.PopupRounding = 12.0f * dpi;
    s.ScrollbarRounding = 8.0f * dpi;
    s.FramePadding = ImVec2(14 * dpi, 10 * dpi);
    s.ItemSpacing = ImVec2(12 * dpi, 10 * dpi);
    s.WindowPadding = ImVec2(20 * dpi, 18 * dpi);
    s.IndentSpacing = 24 * dpi;
    s.ScrollbarSize = 14 * dpi;

    ImVec4* c = s.Colors;
    const ImVec4 bg(0.070f, 0.072f, 0.086f, 1.0f);      // deep graphite
    const ImVec4 surface(0.118f, 0.125f, 0.153f, 1.0f); // blue-gray card
    const ImVec4 surface2(0.157f, 0.169f, 0.204f, 1.0f);
    const ImVec4 accent(0.063f, 0.486f, 0.063f, 1.0f);  // xbox green, moderate
    const ImVec4 accentHov(0.106f, 0.612f, 0.106f, 1.0f);
    const ImVec4 accentAct(0.043f, 0.376f, 0.043f, 1.0f);
    const ImVec4 text(0.910f, 0.918f, 0.945f, 1.0f);
    const ImVec4 dim(0.604f, 0.627f, 0.686f, 1.0f);

    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = dim;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = surface;
    c[ImGuiCol_PopupBg] = ImVec4(0.098f, 0.104f, 0.129f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.208f, 0.227f, 0.278f, 1.0f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = surface2;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.196f, 0.216f, 0.263f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.235f, 0.259f, 0.314f, 1.0f);
    c[ImGuiCol_TitleBg] = surface;
    c[ImGuiCol_TitleBgActive] = surface2;
    c[ImGuiCol_TitleBgCollapsed] = surface;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.278f, 0.306f, 0.369f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.357f, 0.392f, 0.467f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.85f, 0.35f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.35f, 0.85f, 0.35f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.95f, 0.45f, 1.0f);
    c[ImGuiCol_Button] = surface2;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.243f, 0.267f, 0.322f, 1.0f);
    c[ImGuiCol_ButtonActive] = accentAct;
    c[ImGuiCol_Header] = surface2;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.243f, 0.267f, 0.322f, 1.0f);
    c[ImGuiCol_HeaderActive] = accentAct;
    c[ImGuiCol_Separator] = ImVec4(0.208f, 0.227f, 0.278f, 1.0f);
    c[ImGuiCol_Tab] = surface;
    c[ImGuiCol_TabHovered] = ImVec4(0.243f, 0.267f, 0.322f, 1.0f);
    c[ImGuiCol_TabActive] = surface2;
    c[ImGuiCol_NavHighlight] = ImVec4(0.35f, 0.85f, 0.35f, 1.0f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(0.35f, 0.85f, 0.35f, 1.0f);
    // (tables/plots keep defaults tinted by above)
    (void)accentHov;
}

bool LoadFonts(float dpi) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.SizePixels = 20.0f * dpi;
    const char* segoe = "C:\\Windows\\Fonts\\segoeui.ttf";
    DWORD a = GetFileAttributesA(segoe);
    if (a != INVALID_FILE_ATTRIBUTES) {
        io.Fonts->AddFontFromFileTTF(segoe, 20.0f * dpi, nullptr, io.Fonts->GetGlyphRangesCyrillic());
        return true;
    }
    io.Fonts->AddFontDefault();
    return false;
}

bool Toggle(const char* id, bool* v) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (win->SkipItems) return false;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiStyle& style = g.Style;
    const ImGuiID imid = win->GetID(id);
    const float h = ImGui::GetFrameHeight() * 1.1f;
    const float w = h * 1.9f;
    const ImVec2 pos = win->DC.CursorPos;
    const ImRect bb(pos, pos + ImVec2(w, h));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, imid)) return false;
    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, imid, &hovered, &held);
    if (pressed) *v = !*v;
    const float t = *v ? 1.0f : 0.0f;
    // animate knob
    ImU32 bg = ImGui::GetColorU32(*v ? ImVec4(0.106f, 0.612f, 0.106f, 1.0f)
        : hovered ? ImVec4(0.243f, 0.267f, 0.322f, 1.0f) : ImVec4(0.157f, 0.169f, 0.204f, 1.0f));
    win->DrawList->AddRectFilled(bb.Min, bb.Max, bg, h * 0.5f);
    const float knobR = h * 0.5f - style.FramePadding.y * 0.5f;
    const float knobX = bb.Min.x + knobR + style.FramePadding.y * 0.5f + t * (w - 2 * knobR - style.FramePadding.y);
    win->DrawList->AddCircleFilled(ImVec2(knobX, bb.Min.y + h * 0.5f), knobR, IM_COL32(240, 240, 245, 255));
    return pressed;
}

bool BigButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.063f, 0.486f, 0.063f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.106f, 0.612f, 0.106f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.043f, 0.376f, 0.043f, 1.0f));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    return r;
}

bool CardButton(const char* id, const ImVec2& size) {
    ImGui::PushID(id);
    bool r = ImGui::InvisibleButton("##card", size);
    ImGui::PopID();
    return r;
}

int Segmented(const char* id, const char* const* items, int count, int current) {
    int sel = current;
    ImGui::PushID(id);
    for (int i = 0; i < count; ++i) {
        if (i) ImGui::SameLine(0, 4);
        bool active = (i == current);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.063f, 0.486f, 0.063f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.106f, 0.612f, 0.106f, 1.0f));
        }
        if (ImGui::Button(items[i])) sel = i;
        if (active) ImGui::PopStyleColor(2);
    }
    ImGui::PopID();
    return sel;
}

void StatRow(const char* label, const char* value, bool ok) {
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(220.0f * ImGui::GetIO().FontGlobalScale);
    if (ok) ImGui::TextWrapped("%s", value);
    else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
        ImGui::TextWrapped("%s", value);
        ImGui::PopStyleColor();
    }
}

void Chip(const char* text, bool ok) {
    ImVec4 bg = ok ? ImVec4(0.063f, 0.38f, 0.063f, 1.0f) : ImVec4(0.45f, 0.25f, 0.15f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
    ImGui::SmallButton(text);
    ImGui::PopStyleColor(3);
}

void SectionTitle(const char* title) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.9f, 0.65f, 1.0f));
    ImGui::Text("%s", title);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();
}

void PushToast(const std::string& text, bool error) {
    g_toasts.push_back({text, 4.0f, error});
    if (g_toasts.size() > 4) g_toasts.erase(g_toasts.begin());
}

void DrawToasts(float dpi) {
    const float dt = ImGui::GetIO().DeltaTime;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 base(vp->Pos.x + vp->Size.x - 360 * dpi - 16 * dpi, vp->Pos.y + vp->Size.y - 16 * dpi);
    int n = 0;
    for (auto it = g_toasts.begin(); it != g_toasts.end();) {
        it->ttl -= dt;
        if (it->ttl <= 0) { it = g_toasts.erase(it); continue; }
        ImGui::SetNextWindowPos(ImVec2(base.x, base.y - n * (56 * dpi + 8)), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(360 * dpi, 0), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, it->error ? ImVec4(0.35f, 0.12f, 0.10f, 0.96f) : ImVec4(0.10f, 0.16f, 0.11f, 0.96f));
        ImGui::Begin(("##toast" + std::to_string(n)).c_str(), nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextWrapped("%s", it->text.c_str());
        ImGui::End();
        ImGui::PopStyleColor();
        ++n; ++it;
    }
}

} // namespace launcher::theme
