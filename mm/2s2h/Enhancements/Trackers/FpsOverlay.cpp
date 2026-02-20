#include "FpsOverlay.h"

#include <algorithm>
#include <imgui.h>
#include <libultraship/bridge/consolevariablebridge.h>

void FpsOverlayWindow::Draw() {
    if (!CVarGetInteger("gWindows.FpsOverlay", 0)) {
        return;
    }

    float scale = std::max(CVarGetFloat("gFpsOverlay.Scale", 1.0f), 1.0f);
    ImVec4 windowBG =
        !CVarGetInteger("gFpsOverlay.HideBackground", 0) ? ImVec4(0, 0, 0, 0.5f) : ImVec4(0, 0, 0, 0);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, windowBG);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);

    ImGui::Begin("FPS Overlay", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    ImGui::SetWindowFontScale(scale);

    float fps = ImGui::GetIO().Framerate;
    ImVec4 color;
    if (fps >= 50.0f) {
        color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); // Green
    } else if (fps >= 30.0f) {
        color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // Yellow
    } else {
        color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); // Red
    }

    ImGui::TextColored(color, "FPS: %.0f", fps);

    ImGui::End();

    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(2);
}

void FpsOverlayWindow::InitElement() {
}
