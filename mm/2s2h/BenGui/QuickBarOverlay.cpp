#include "QuickBarOverlay.h"

#include <imgui.h>
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

// Category display names
static const char* GetCategoryName(QuickBarCategory cat) {
    switch (cat) {
        case QB_CAT_MASKS:   return "Masks";
        case QB_CAT_TOOLS:   return "Tools";
        case QB_CAT_SONGS:   return "Songs";
        case QB_CAT_BOTTLES: return "Bottles";
        default:             return "";
    }
}

void QuickBarOverlayWindow::InitElement() {
}

void QuickBarOverlayWindow::Draw() {
    if (!gPlayState) return;
    if (!IsQuickBarEnabled()) return;

    QuickBarState& state = GetQuickBarState();

    // Draw Active Tool chip (always visible when an active tool is set)
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

    // Draw QuickBar overlay when open
    if (!state.isOpen || state.currentItems.empty()) return;

    const float iconSize = 40.0f;
    const float iconPadding = 6.0f;
    const float barPaddingX = 12.0f;
    const float barPaddingY = 8.0f;
    const float highlightBorder = 3.0f;
    const float highlightScale = 1.15f;

    int itemCount = (int)state.currentItems.size();

    // Calculate bar dimensions
    float barWidth = (iconSize + iconPadding) * itemCount - iconPadding + barPaddingX * 2;
    float barHeight = iconSize + barPaddingY * 2;

    // Limit bar width to viewport
    ImVec2 viewport = ImGui::GetIO().DisplaySize;
    if (barWidth > viewport.x * 0.9f) {
        barWidth = viewport.x * 0.9f;
    }

    // Position at top-center
    float barX = (viewport.x - barWidth) * 0.5f;
    float barY = 40.0f;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.1f, 0.75f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.5f, 0.8f, 0.4f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(barPaddingX, barPaddingY));

    ImGuiWindowFlags barFlags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;

    ImGui::SetNextWindowPos(ImVec2(barX, barY));
    ImGui::SetNextWindowSize(ImVec2(barWidth, barHeight + 30));

    ImGui::Begin("QuickBar", nullptr, barFlags);

    // Category label
    ImGui::SetCursorPosX(barPaddingX);

    // Draw item icons
    float startX = barPaddingX;
    // If items overflow, scroll to keep selection visible
    float totalWidth = (iconSize + iconPadding) * itemCount - iconPadding;
    float visibleWidth = barWidth - barPaddingX * 2;
    float scrollOffset = 0;
    if (totalWidth > visibleWidth) {
        float selCenter = (iconSize + iconPadding) * state.selectionIndex + iconSize * 0.5f;
        scrollOffset = selCenter - visibleWidth * 0.5f;
        if (scrollOffset < 0) scrollOffset = 0;
        if (scrollOffset > totalWidth - visibleWidth) scrollOffset = totalWidth - visibleWidth;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();

    for (int i = 0; i < itemCount; i++) {
        float x = startX + (iconSize + iconPadding) * i - scrollOffset;
        if (x + iconSize < 0 || x > visibleWidth + barPaddingX) continue;

        bool isSelected = (i == state.selectionIndex);
        float drawSize = isSelected ? iconSize * highlightScale : iconSize;
        float offset = isSelected ? (iconSize - drawSize) * 0.5f : 0;

        ImVec2 iconPos(winPos.x + x + offset, winPos.y + barPaddingY + offset);

        // Highlight outline for selected item
        if (isSelected) {
            ImVec2 hlMin(iconPos.x - highlightBorder, iconPos.y - highlightBorder);
            ImVec2 hlMax(iconPos.x + drawSize + highlightBorder, iconPos.y + drawSize + highlightBorder);
            drawList->AddRect(hlMin, hlMax, IM_COL32(100, 180, 255, 220), 6.0f, 0, 2.5f);
        }

        ImTextureID tex = GetItemTexture(state.currentItems[i], state.openCategory);
        if (tex) {
            ImGui::SetCursorPos(ImVec2(x + offset, barPaddingY + offset));
            float alpha = isSelected ? 1.0f : 0.7f;
            ImGui::Image(tex, ImVec2(drawSize, drawSize), ImVec2(0, 0), ImVec2(1, 1),
                         ImVec4(1, 1, 1, alpha), ImVec4(0, 0, 0, 0));
        }
    }

    // Name text below the icons
    if (state.selectionIndex >= 0 && state.selectionIndex < (int)state.currentNames.size()) {
        const char* itemName = state.currentNames[state.selectionIndex];
        ImVec2 textSize = ImGui::CalcTextSize(itemName);
        float textX = (barWidth - textSize.x) * 0.5f;
        ImGui::SetCursorPos(ImVec2(textX, barPaddingY + iconSize + 4));
        ImGui::TextColored(ImVec4(1, 1, 1, 0.95f), "%s", itemName);
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}
