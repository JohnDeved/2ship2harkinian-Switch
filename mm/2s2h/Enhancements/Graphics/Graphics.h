#ifndef GRAPHICS_H
#define GRAPHICS_H

void MotionBlur_RenderMenuOptions();

#ifdef __cplusplus

#include <vector>

const std::vector<const char*>* CRTFilter_GetShaderNames();

extern "C" {
#endif

void MotionBlur_Override(u8* status, s32* alpha);

#ifdef __cplusplus
};
#endif

#endif // GRAPHICS_H
