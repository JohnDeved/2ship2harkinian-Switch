#ifndef POST_PROCESS_H
#define POST_PROCESS_H

#ifdef __cplusplus

#include <string>
#include <vector>
#include <cstdint>

#ifdef ENABLE_OPENGL
#include "effect_module.hpp"
#endif

void PostProcess_RenderMenuOptions();

// ─── Overlay API ────────────────────────────────────────────────────────────
// Functions used by the ReShade overlay window to inspect/modify effect state.

// Get the list of available .fx effect names
const std::vector<std::string>& PostProcess_GetAvailableEffects();

// Get/set the currently selected effect index
int PostProcess_GetSelectedEffectIndex();
void PostProcess_SetSelectedEffectIndex(int idx);

// Is the post-process system enabled?
bool PostProcess_IsEnabled();
void PostProcess_SetEnabled(bool enabled);

// Get the name of the currently loaded effect
const std::string& PostProcess_GetLoadedEffectName();

// Get the uniforms of the currently loaded effect
#ifdef ENABLE_OPENGL
const std::vector<reshadefx::uniform>& PostProcess_GetUniforms();
#endif

// Get a mutable pointer to the uniform data buffer
uint8_t* PostProcess_GetUniformData();
size_t PostProcess_GetUniformDataSize();

// Force reload of the current effect
void PostProcess_ForceReload();

// Rescan available effects
void PostProcess_RescanEffects();

extern "C" {
#endif // __cplusplus

#ifdef __cplusplus
}
#endif

#endif // POST_PROCESS_H
