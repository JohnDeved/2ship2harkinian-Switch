#ifndef GRAPHICS_H
#define GRAPHICS_H

void MotionBlur_RenderMenuOptions();
void PostProcess_RenderMenuOptions();

#ifdef __cplusplus

extern "C" {
#endif

void MotionBlur_Override(u8* status, s32* alpha);

#ifdef __cplusplus
};
#endif

#endif // GRAPHICS_H
