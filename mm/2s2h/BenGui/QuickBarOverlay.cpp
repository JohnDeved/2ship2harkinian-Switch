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

    // ─── D-pad HUD (matches native Dpad Equips rendering exactly) ──────────────
    // Native: gDPadTex at (271,55) 32x32 on 320x240 canvas
    // Icons: 16x16 at RIGHT(295,63), LEFT(263,63), DOWN(279,79), UP(279,47)
    {
        ImGuiWindowFlags dpadFlags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                     ImGuiWindowFlags_NoBackground;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

        // Scale from N64 320x240 canvas to actual viewport
        float scaleX = viewport.x / 320.0f;
        float scaleY = viewport.y / 240.0f;

        // D-pad background position/size matching native gDPadTex
        float dpadBgX = 271.0f * scaleX;
        float dpadBgY = 55.0f * scaleY;
        float dpadBgW = 32.0f * scaleX;
        float dpadBgH = 32.0f * scaleY;

        // Window covers the full D-pad area (icons extend beyond the 32x32 bg)
        float winLeft = 263.0f * scaleX - 2;
        float winTop = 47.0f * scaleY - 2;
        float winRight = (295.0f + 16.0f) * scaleX + 2;
        float winBottom = (79.0f + 16.0f) * scaleY + 2;

        ImGui::SetNextWindowPos(ImVec2(winLeft, winTop), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(winRight - winLeft, winBottom - winTop));
        ImGui::Begin("QuickBarDpad", nullptr, dpadFlags);

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Draw D-pad cross background (dark semi-transparent, matching native style)
        // Vertical bar
        float crossW = 14.0f * scaleX;
        float crossH = dpadBgH;
        float crossCX = dpadBgX + dpadBgW * 0.5f;
        float crossCY = dpadBgY + dpadBgH * 0.5f;
        dl->AddRectFilled(ImVec2(crossCX - crossW * 0.5f, crossCY - crossH * 0.5f - 4 * scaleY),
                          ImVec2(crossCX + crossW * 0.5f, crossCY + crossH * 0.5f + 4 * scaleY),
                          IM_COL32(0, 0, 0, 120), 2.0f);
        // Horizontal bar
        dl->AddRectFilled(ImVec2(crossCX - crossH * 0.5f - 4 * scaleX, crossCY - crossW * 0.5f),
                          ImVec2(crossCX + crossH * 0.5f + 4 * scaleX, crossCY + crossW * 0.5f),
                          IM_COL32(0, 0, 0, 120), 2.0f);

        // Draw item icons at exact native N64 coordinates
        struct DpadSlot {
            float nx, ny;  // N64 coordinates
            int itemId;
            QuickBarCategory cat;
        };

        DpadSlot slots[4] = {
            { 279.0f, 47.0f, state.lastUsed[QB_CAT_MASKS], QB_CAT_MASKS },     // UP
            { 295.0f, 63.0f, state.lastUsed[QB_CAT_TOOLS], QB_CAT_TOOLS },     // RIGHT
            { 279.0f, 79.0f, state.lastUsed[QB_CAT_BOTTLES], QB_CAT_BOTTLES },  // DOWN
            { 263.0f, 63.0f, ITEM_OCARINA_OF_TIME, QB_CAT_SONGS }               // LEFT (always ocarina)
        };

        float iconW = 16.0f * scaleX;
        float iconH = 16.0f * scaleY;

        for (int i = 0; i < 4; i++) {
            int itemId = slots[i].itemId;
            if (itemId < 0) continue;

            ImTextureID tex = GetItemTexture(itemId, slots[i].cat);
            if (tex) {
                float ix = slots[i].nx * scaleX;
                float iy = slots[i].ny * scaleY;
                // Convert from screen coords to window-local coords
                ImGui::SetCursorPos(ImVec2(ix - winLeft, iy - winTop));
                ImGui::Image(tex, ImVec2(iconW, iconH), ImVec2(0, 0), ImVec2(1, 1),
                             ImVec4(1, 1, 1, 0.9f), ImVec4(0, 0, 0, 0));
            }
        }

        ImGui::End();
        ImGui::PopStyleVar(3);
    }

    // ─── QuickBar overlay (BotW-style centered carousel) ────────────────────
    if (!state.isOpen || state.currentItems.empty()) return;

    int itemCount = (int)state.currentItems.size();

    // Fullscreen dim overlay
    {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.25f));
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

    // BotW-style tile parameters
    const float tileSize = 80.0f;        // Each tile is a square
    const float selectedScale = 1.15f;    // Selected tile is slightly larger
    const float tilePadding = 4.0f;       // Gap between tiles
    const float iconPad = 10.0f;          // Padding inside tile for icon
    const float minAlpha = 0.25f;         // Never fully disappear

    float centerX = viewport.x * 0.5f;
    float centerY = viewport.y * 0.5f;

    // Calculate visible area
    float maxVisibleWidth = viewport.x * 0.75f;

    ImGuiWindowFlags barFlags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
                                ImGuiWindowFlags_NoBackground;

    float winW = viewport.x;
    float winH = tileSize * selectedScale + 60.0f;
    float winY = centerY - winH * 0.5f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    ImGui::SetNextWindowPos(ImVec2(0, winY));
    ImGui::SetNextWindowSize(ImVec2(winW, winH));

    ImGui::Begin("QuickBar", nullptr, barFlags);

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Draw tiles centered on the selected item
    for (int i = 0; i < itemCount; i++) {
        int offset = i - state.selectionIndex;
        bool isSelected = (offset == 0);
        float scale = isSelected ? selectedScale : 1.0f;
        float curTileSize = tileSize * scale;

        // Calculate X position: selected at center, others offset
        float posX;
        if (isSelected) {
            posX = centerX - curTileSize * 0.5f;
        } else if (offset > 0) {
            // Right of center
            float px = centerX + (tileSize * selectedScale * 0.5f) + tilePadding;
            for (int j = 1; j < offset; j++) {
                px += tileSize + tilePadding;
            }
            posX = px;
        } else {
            // Left of center
            float px = centerX - (tileSize * selectedScale * 0.5f) - tilePadding;
            for (int j = -1; j > offset; j--) {
                px -= tileSize + tilePadding;
            }
            posX = px - tileSize;
        }

        float posY = centerY - curTileSize * 0.5f;

        // Skip tiles far off-screen
        if (posX + curTileSize < 0 || posX > viewport.x) continue;

        // Calculate alpha: fade based on distance from center but never fully disappear
        float tileCenterX = posX + curTileSize * 0.5f;
        float distFromCenter = std::fabs(tileCenterX - centerX);
        float alpha;
        if (isSelected) {
            alpha = 1.0f;
        } else {
            float fadeStart = tileSize * 0.5f;
            float fadeEnd = maxVisibleWidth * 0.5f;
            if (distFromCenter <= fadeStart) {
                alpha = 0.85f;
            } else if (distFromCenter >= fadeEnd) {
                alpha = minAlpha;
            } else {
                float t = (distFromCenter - fadeStart) / (fadeEnd - fadeStart);
                alpha = 0.85f - t * (0.85f - minAlpha);
            }
        }

        // Draw tile background (dark slate)
        ImU32 bgCol = IM_COL32(30, 35, 42, (int)(alpha * 220));
        drawList->AddRectFilled(ImVec2(posX, posY), ImVec2(posX + curTileSize, posY + curTileSize),
                                bgCol, 4.0f);

        // Selected tile: bright yellow/gold border (BotW style)
        if (isSelected) {
            drawList->AddRect(ImVec2(posX, posY), ImVec2(posX + curTileSize, posY + curTileSize),
                              IM_COL32(255, 215, 0, 230), 4.0f, 0, 3.0f);
        } else {
            // Subtle border on non-selected tiles
            ImU32 borderCol = IM_COL32(80, 90, 100, (int)(alpha * 120));
            drawList->AddRect(ImVec2(posX, posY), ImVec2(posX + curTileSize, posY + curTileSize),
                              borderCol, 4.0f, 0, 1.0f);
        }

        // Draw icon inside tile
        ImTextureID tex = GetItemTexture(state.currentItems[i], state.openCategory);
        if (tex) {
            float iconDrawSize = curTileSize - iconPad * 2 * scale;
            float iconX = posX + (curTileSize - iconDrawSize) * 0.5f;
            float iconYpos = posY + (curTileSize - iconDrawSize) * 0.5f;
            ImGui::SetCursorPos(ImVec2(iconX, iconYpos - winY));
            ImGui::Image(tex, ImVec2(iconDrawSize, iconDrawSize), ImVec2(0, 0), ImVec2(1, 1),
                         ImVec4(1, 1, 1, alpha), ImVec4(0, 0, 0, 0));
        }
    }

    // Divider line below selected item
    float selectedBottom = centerY + (tileSize * selectedScale * 0.5f);
    float dividerW = tileSize * 0.5f;
    drawList->AddLine(
        ImVec2(centerX - dividerW * 0.5f, selectedBottom + 6.0f),
        ImVec2(centerX + dividerW * 0.5f, selectedBottom + 6.0f),
        IM_COL32(255, 255, 255, 180), 2.0f);

    // Name text below the divider
    if (state.selectionIndex >= 0 && state.selectionIndex < (int)state.currentNames.size()) {
        const char* itemName = state.currentNames[state.selectionIndex];
        ImVec2 textSize = ImGui::CalcTextSize(itemName);
        float textX = centerX - textSize.x * 0.5f;
        float textY = selectedBottom + 14.0f;
        ImGui::SetCursorPos(ImVec2(textX, textY - winY));
        ImGui::TextColored(ImVec4(1, 1, 1, 0.95f), "%s", itemName);
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
}
