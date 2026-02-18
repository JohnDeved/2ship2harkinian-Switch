#include "QuickBarOverlay.h"

#include <imgui.h>
#include <cmath>
#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/window/Window.h>

#include "2s2h/Enhancements/Equipment/QuickBar.h"

extern "C" {
#include "variables.h"
#include "z64item.h"
}

// Map quest bits to ITEM_SONG_* for texture lookup
static int QuestBitToSongItemId(int questBit) {
    switch (questBit) {
        case QUEST_SONG_SONATA:        return ITEM_SONG_SONATA;
        case QUEST_SONG_LULLABY:       return ITEM_SONG_LULLABY;
        case QUEST_SONG_BOSSA_NOVA:    return ITEM_SONG_NOVA;
        case QUEST_SONG_ELEGY:         return ITEM_SONG_ELEGY;
        case QUEST_SONG_OATH:          return ITEM_SONG_OATH;
        case QUEST_SONG_SARIA:         return ITEM_SONG_SARIA;
        case QUEST_SONG_TIME:          return ITEM_SONG_TIME;
        case QUEST_SONG_HEALING:       return ITEM_SONG_HEALING;
        case QUEST_SONG_EPONA:         return ITEM_SONG_EPONA;
        case QUEST_SONG_SOARING:       return ITEM_SONG_SOARING;
        case QUEST_SONG_STORMS:        return ITEM_SONG_STORMS;
        case QUEST_SONG_SUN:           return ITEM_SONG_SUN;
        case QUEST_SONG_LULLABY_INTRO: return ITEM_SONG_LULLABY_INTRO;
        default: return -1;
    }
}

static ImTextureID GetItemTexture(int itemId, QuickBarCategory cat) {
    int texItemId = itemId;
    if (cat == QB_CAT_SONGS) {
        texItemId = QuestBitToSongItemId(itemId);
        if (texItemId < 0) return 0;
    }
    if (texItemId < 0 || texItemId >= (int)ARRAY_COUNT(gItemIcons)) return 0;
    const char* texName = (const char*)gItemIcons[texItemId];
    if (texName == nullptr) return 0;
    return Ship::Context::GetInstance()->GetWindow()->GetGui()->GetTextureByName(texName);
}

void QuickBarOverlayWindow::InitElement() {
}

void QuickBarOverlayWindow::Draw() {
    if (!gPlayState) return;
    if (!IsQuickBarEnabled()) return;

    QuickBarState& state = GetQuickBarState();
    ImVec2 viewport = ImGui::GetIO().DisplaySize;

    // ─── Active Tool chip (top-right corner) ────────────────────────────────
    if (state.activeToolItem >= 0) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.55f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.4f, 0.7f, 1.0f, 0.6f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));

        ImGuiWindowFlags chipFlags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs;

        // Position at top-right corner
        ImGui::SetNextWindowPos(ImVec2(viewport.x - 180, 10), ImGuiCond_Always);

        ImGui::Begin("ActiveToolChip", nullptr, chipFlags);

        ImTextureID tex = GetItemTexture(state.activeToolItem, QB_CAT_TOOLS);
        if (tex) {
            ImGui::Image(tex, ImVec2(24, 24));
            ImGui::SameLine(0, 6);
        }

        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
        ImGui::TextColored(ImVec4(1, 1, 1, 0.9f), "%s", QuickBar_GetItemName(state.activeToolItem));

        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }

    // ─── QuickBar overlay (center of screen, carousel-style) ────────────────
    if (!state.isOpen || state.currentItems.empty()) return;

    int itemCount = (int)state.currentItems.size();

    // Draw fullscreen tint overlay to indicate slow-motion / focus mode
    {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.02f, 0.08f, 0.35f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

        ImGuiWindowFlags tintFlags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(viewport);
        ImGui::Begin("QuickBarTint", nullptr, tintFlags);
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(1);
    }

    // Carousel parameters — selected item is always centered
    const float centerIconSize = 64.0f;   // Size of the selected (center) icon
    const float sideIconSize = 48.0f;     // Size of neighboring icons
    const float iconSpacing = 16.0f;      // Gap between icons
    const float barHeight = centerIconSize + 40.0f; // Bar height (icon + text)
    const float fadeDistance = 200.0f;     // Distance from center where icons start fading out

    float centerX = viewport.x * 0.5f;
    float centerY = viewport.y * 0.5f;

    // Draw the QuickBar background (thin translucent strip)
    float bgWidth = viewport.x * 0.6f;
    float bgX = (viewport.x - bgWidth) * 0.5f;
    float bgY = centerY - barHeight * 0.5f - 8.0f;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.03f, 0.08f, 0.7f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.5f, 0.8f, 0.3f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    ImGuiWindowFlags barFlags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;

    ImGui::SetNextWindowPos(ImVec2(bgX, bgY));
    ImGui::SetNextWindowSize(ImVec2(bgWidth, barHeight + 16.0f));

    ImGui::Begin("QuickBar", nullptr, barFlags);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();
    float iconY = winPos.y + 8.0f;

    // Draw each item relative to center: selected item at center, others offset
    for (int i = 0; i < itemCount; i++) {
        int offset = i - state.selectionIndex;
        bool isSelected = (offset == 0);

        float iconSize = isSelected ? centerIconSize : sideIconSize;

        // Calculate position: selected item at center, others spaced out
        float posX;
        if (isSelected) {
            posX = centerX - iconSize * 0.5f;
        } else {
            float dist = (float)offset;
            // Sum up widths to get exact position
            float px = centerX;
            if (offset > 0) {
                px += centerIconSize * 0.5f + iconSpacing;
                for (int j = 1; j < offset; j++) {
                    px += sideIconSize + iconSpacing;
                }
                posX = px;
            } else {
                px -= centerIconSize * 0.5f + iconSpacing;
                for (int j = -1; j > offset; j--) {
                    px -= sideIconSize + iconSpacing;
                }
                posX = px - sideIconSize;
            }
        }

        float posY = iconY + (centerIconSize - iconSize) * 0.5f;

        // Skip if fully off-screen
        if (posX + iconSize < bgX || posX > bgX + bgWidth) continue;

        // Calculate alpha based on distance from center (fade toward edges)
        float distFromCenter = std::fabs((posX + iconSize * 0.5f) - centerX);
        float alpha;
        if (isSelected) {
            alpha = 1.0f;
        } else {
            alpha = 1.0f - (distFromCenter / fadeDistance);
            if (alpha < 0.0f) alpha = 0.0f;
            if (alpha > 0.8f) alpha = 0.8f;
        }

        // Highlight outline for selected item
        if (isSelected) {
            float hlBorder = 3.0f;
            ImVec2 hlMin(posX - hlBorder, posY - hlBorder);
            ImVec2 hlMax(posX + iconSize + hlBorder, posY + iconSize + hlBorder);
            drawList->AddRect(hlMin, hlMax, IM_COL32(100, 180, 255, 220), 8.0f, 0, 2.5f);
        }

        ImTextureID tex = GetItemTexture(state.currentItems[i], state.openCategory);
        if (tex) {
            ImGui::SetCursorPos(ImVec2(posX - winPos.x, posY - winPos.y));
            ImGui::Image(tex, ImVec2(iconSize, iconSize), ImVec2(0, 0), ImVec2(1, 1),
                         ImVec4(1, 1, 1, alpha), ImVec4(0, 0, 0, 0));
        }
    }

    // Name text below the icons (centered)
    if (state.selectionIndex >= 0 && state.selectionIndex < (int)state.currentNames.size()) {
        const char* itemName = state.currentNames[state.selectionIndex];
        ImVec2 textSize = ImGui::CalcTextSize(itemName);
        float textX = (bgWidth - textSize.x) * 0.5f;
        ImGui::SetCursorPos(ImVec2(textX, 8.0f + centerIconSize + 6.0f));
        ImGui::TextColored(ImVec4(1, 1, 1, 0.95f), "%s", itemName);
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}
