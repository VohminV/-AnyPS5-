#pragma once
// Graphite design system for the launcher — everything drawn by Dear ImGui.
// Deep blue-gray surfaces, moderate green accent, large type, TV-friendly.
#include "imgui.h"
#include <string>
#include <vector>

namespace launcher::theme {

struct Toast {
    std::string text;
    float ttl = 4.0f;
    bool error = false;
};

void ApplyGraphiteStyle(float dpiScale = 1.0f);
// Segoe UI with Cyrillic coverage; falls back to default font if missing.
bool LoadFonts(float dpiScale);

// Custom components (all drawn, no system controls):
bool Toggle(const char* id, bool* v);                       // animated switch
bool BigButton(const char* label, const ImVec2& size);      // hero CTA
bool CardButton(const char* id, const ImVec2& size);        // returns true on click; caller draws content inside via BeginCardContent
int Segmented(const char* id, const char* const* items, int count, int current); // returns selected
void StatRow(const char* label, const char* value, bool ok = true);
void Chip(const char* text, bool ok);
void SectionTitle(const char* title);

void PushToast(const std::string& text, bool error = false);
void DrawToasts(float dpiScale);

} // namespace launcher::theme
