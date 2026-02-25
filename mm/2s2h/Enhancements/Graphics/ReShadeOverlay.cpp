#include "ReShadeOverlay.h"
#include "PostProcess.h"
#include <imgui.h>
#include <string>

#ifdef ENABLE_OPENGL
#include "effect_module.hpp"
#endif

// ─── Annotation helpers (matching ReShade's runtime_internal.hpp pattern) ───

#ifdef ENABLE_OPENGL

static std::string_view AnnotationString(const std::vector<reshadefx::annotation>& annotations,
                                         const std::string_view name, const std::string_view defaultValue = {}) {
    for (const auto& ann : annotations) {
        if (ann.name == name)
            return ann.value.string_data;
    }
    return defaultValue;
}

static float AnnotationFloat(const std::vector<reshadefx::annotation>& annotations, const std::string_view name,
                             size_t i = 0, float defaultValue = 0.0f) {
    for (const auto& ann : annotations) {
        if (ann.name == name) {
            if (i < 16)
                return ann.type.is_floating_point() ? ann.value.as_float[i] : static_cast<float>(ann.value.as_int[i]);
        }
    }
    return defaultValue;
}

static int AnnotationInt(const std::vector<reshadefx::annotation>& annotations, const std::string_view name,
                         size_t i = 0, int defaultValue = 0) {
    for (const auto& ann : annotations) {
        if (ann.name == name) {
            if (i < 16)
                return ann.type.is_integral() ? ann.value.as_int[i] : static_cast<int>(ann.value.as_float[i]);
        }
    }
    return defaultValue;
}

// Draw ImGui widgets for a single uniform variable based on its annotations
static bool DrawUniformWidget(const reshadefx::uniform& u, uint8_t* data) {
    if (!data)
        return false;

    // Skip special/source uniforms (timer, framecount, etc.)
    auto source = AnnotationString(u.annotations, "source");
    if (!source.empty())
        return false;

    // Get display label
    auto label = AnnotationString(u.annotations, "ui_label");
    std::string labelStr = label.empty() ? u.name : std::string(label);

    // Get tooltip
    auto tooltip = AnnotationString(u.annotations, "ui_tooltip");

    // Get UI type
    auto uiType = AnnotationString(u.annotations, "ui_type");

    bool changed = false;
    uint8_t* ptr = data + u.offset;

    // Handle different types
    if (u.type.is_floating_point()) {
        float* fptr = reinterpret_cast<float*>(ptr);
        int components = static_cast<int>(u.type.components());

        if (uiType == "color") {
            if (components >= 3) {
                if (components >= 4)
                    changed = ImGui::ColorEdit4(labelStr.c_str(), fptr);
                else
                    changed = ImGui::ColorEdit3(labelStr.c_str(), fptr);
            }
        } else if (uiType == "drag") {
            float fmin = AnnotationFloat(u.annotations, "ui_min", 0, 0.0f);
            float fmax = AnnotationFloat(u.annotations, "ui_max", 0, 1.0f);
            float step = AnnotationFloat(u.annotations, "ui_step", 0, 0.001f);
            if (components == 1)
                changed = ImGui::DragFloat(labelStr.c_str(), fptr, step, fmin, fmax);
            else if (components == 2)
                changed = ImGui::DragFloat2(labelStr.c_str(), fptr, step, fmin, fmax);
            else if (components == 3)
                changed = ImGui::DragFloat3(labelStr.c_str(), fptr, step, fmin, fmax);
            else if (components >= 4)
                changed = ImGui::DragFloat4(labelStr.c_str(), fptr, step, fmin, fmax);
        } else {
            // Default: slider
            float fmin = AnnotationFloat(u.annotations, "ui_min", 0, 0.0f);
            float fmax = AnnotationFloat(u.annotations, "ui_max", 0, 1.0f);
            float step = AnnotationFloat(u.annotations, "ui_step", 0, 0.001f);
            const char* format = "%.3f";
            if (components == 1)
                changed = ImGui::SliderFloat(labelStr.c_str(), fptr, fmin, fmax, format);
            else if (components == 2)
                changed = ImGui::SliderFloat2(labelStr.c_str(), fptr, fmin, fmax, format);
            else if (components == 3)
                changed = ImGui::SliderFloat3(labelStr.c_str(), fptr, fmin, fmax, format);
            else if (components >= 4)
                changed = ImGui::SliderFloat4(labelStr.c_str(), fptr, fmin, fmax, format);
        }
    } else if (u.type.base == reshadefx::type::t_int) {
        int* iptr = reinterpret_cast<int*>(ptr);
        int components = static_cast<int>(u.type.components());

        if (uiType == "combo") {
            auto items = AnnotationString(u.annotations, "ui_items");
            if (!items.empty() && components == 1) {
                // Parse \0-separated items from the annotation string
                std::vector<std::string> itemList;
                std::string current;
                for (char c : items) {
                    if (c == '\0')
                        break;
                    if (c == '\n') {
                        if (!current.empty())
                            itemList.push_back(current);
                        current.clear();
                    } else {
                        current += c;
                    }
                }
                if (!current.empty())
                    itemList.push_back(current);

                int val = *iptr;
                if (ImGui::BeginCombo(labelStr.c_str(), (val >= 0 && val < static_cast<int>(itemList.size()))
                                                            ? itemList[val].c_str()
                                                            : "Unknown")) {
                    for (int j = 0; j < static_cast<int>(itemList.size()); j++) {
                        bool isSelected = (j == val);
                        if (ImGui::Selectable(itemList[j].c_str(), isSelected)) {
                            *iptr = j;
                            changed = true;
                        }
                        if (isSelected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
        } else if (uiType == "radio") {
            auto items = AnnotationString(u.annotations, "ui_items");
            if (!items.empty() && components == 1) {
                std::vector<std::string> itemList;
                std::string current;
                for (char c : items) {
                    if (c == '\0')
                        break;
                    if (c == '\n') {
                        if (!current.empty())
                            itemList.push_back(current);
                        current.clear();
                    } else {
                        current += c;
                    }
                }
                if (!current.empty())
                    itemList.push_back(current);

                ImGui::Text("%s", labelStr.c_str());
                for (int j = 0; j < static_cast<int>(itemList.size()); j++) {
                    if (ImGui::RadioButton(itemList[j].c_str(), iptr, j))
                        changed = true;
                    if (j + 1 < static_cast<int>(itemList.size()))
                        ImGui::SameLine();
                }
            }
        } else {
            // Default: slider
            int imin = AnnotationInt(u.annotations, "ui_min", 0, 0);
            int imax = AnnotationInt(u.annotations, "ui_max", 0, 100);
            if (components == 1)
                changed = ImGui::SliderInt(labelStr.c_str(), iptr, imin, imax);
            else if (components == 2)
                changed = ImGui::SliderInt2(labelStr.c_str(), iptr, imin, imax);
            else if (components == 3)
                changed = ImGui::SliderInt3(labelStr.c_str(), iptr, imin, imax);
            else if (components >= 4)
                changed = ImGui::SliderInt4(labelStr.c_str(), iptr, imin, imax);
        }
    } else if (u.type.base == reshadefx::type::t_bool) {
        bool val = (*reinterpret_cast<uint32_t*>(ptr)) != 0;
        if (ImGui::Checkbox(labelStr.c_str(), &val)) {
            *reinterpret_cast<uint32_t*>(ptr) = val ? 1 : 0;
            changed = true;
        }
    }

    // Show tooltip if present
    if (!tooltip.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", std::string(tooltip).c_str());
    }

    return changed;
}

#endif // ENABLE_OPENGL

// ─── GuiWindow implementation ───────────────────────────────────────────────

void ReShadeOverlayWindow::InitElement() {
}

void ReShadeOverlayWindow::DrawElement() {
    ImGui::SetNextWindowSize(ImVec2(450, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("ReShade", nullptr, ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }

    // Title bar
    ImGui::TextColored(ImVec4(0.95f, 0.86f, 0.31f, 1.0f), "ReShade FX");
    ImGui::SameLine();
    ImGui::TextDisabled("(integrated)");
    ImGui::Separator();

    // Enable toggle
    bool enabled = PostProcess_IsEnabled();
    if (ImGui::Checkbox("Enable Effects", &enabled)) {
        PostProcess_SetEnabled(enabled);
    }

    if (!enabled) {
        ImGui::TextDisabled("Effects are disabled.");
        ImGui::End();
        return;
    }

    ImGui::Spacing();

    // Effect selector
    const auto& effects = PostProcess_GetAvailableEffects();
    if (effects.empty()) {
        ImGui::TextWrapped("No .fx effect files found.");
        ImGui::TextWrapped("Place ReShade FX shaders in: reshade-shaders/Shaders/");
        ImGui::TextWrapped("Download from: https://github.com/crosire/reshade-shaders");
        if (ImGui::Button("Rescan")) {
            PostProcess_RescanEffects();
        }
        ImGui::End();
        return;
    }

    int selectedIdx = PostProcess_GetSelectedEffectIndex();
    if (selectedIdx >= static_cast<int>(effects.size()))
        selectedIdx = 0;

    if (ImGui::BeginCombo("Effect##reshade_overlay", effects[selectedIdx].c_str())) {
        for (int i = 0; i < static_cast<int>(effects.size()); i++) {
            bool isSelected = (i == selectedIdx);
            if (ImGui::Selectable(effects[i].c_str(), isSelected)) {
                PostProcess_SetSelectedEffectIndex(i);
            }
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Controls
    if (ImGui::Button("Reload")) {
        PostProcess_ForceReload();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) {
        PostProcess_RescanEffects();
    }

    const std::string& loadedName = PostProcess_GetLoadedEffectName();
    if (loadedName.empty()) {
        ImGui::TextDisabled("No effect loaded yet. Effect will compile on next frame.");
        ImGui::End();
        return;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("Loaded: %s", loadedName.c_str());

#ifdef ENABLE_OPENGL
    // ─── Uniform parameters ─────────────────────────────────────────────────
    const auto& uniforms = PostProcess_GetUniforms();
    uint8_t* uniformData = PostProcess_GetUniformData();

    if (!uniforms.empty() && uniformData) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Parameters");
        ImGui::Spacing();

        // Group by ui_category
        std::string currentCategory;
        bool categoryCollapsed = false;

        for (const auto& u : uniforms) {
            // Skip special source-bound uniforms
            auto source = AnnotationString(u.annotations, "source");
            if (!source.empty())
                continue;

            // Category grouping
            auto category = AnnotationString(u.annotations, "ui_category");
            std::string catStr(category);
            if (catStr != currentCategory) {
                currentCategory = catStr;
                categoryCollapsed = false;
                if (!catStr.empty()) {
                    ImGui::Spacing();
                    auto catClosed = AnnotationInt(u.annotations, "ui_category_closed", 0, 0);
                    ImGuiTreeNodeFlags flags =
                        ImGuiTreeNodeFlags_DefaultOpen * (catClosed == 0) | ImGuiTreeNodeFlags_Framed;
                    categoryCollapsed = !ImGui::CollapsingHeader(catStr.c_str(), flags);
                }
            }

            // Skip drawing if the category header is collapsed
            if (categoryCollapsed)
                continue;

            DrawUniformWidget(u, uniformData);
        }

        // Reset button
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Reset All Parameters")) {
            PostProcess_ForceReload();
        }
    }
#endif // ENABLE_OPENGL

    ImGui::End();
}
