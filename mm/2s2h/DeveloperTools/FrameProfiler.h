#pragma once

#ifdef __cplusplus
#include <ship/window/gui/GuiWindow.h>

extern "C" {
#endif

/**
 * Lightweight per-frame profiler for measuring time spent in each major phase
 * of the game loop. Uses clock_gettime(CLOCK_MONOTONIC) on Unix/Switch and
 * QueryPerformanceCounter on Windows.
 *
 * Usage: Call FrameProfiler_StartPhase / FrameProfiler_EndPhase around each
 * phase you want to measure. At the end of the frame, call FrameProfiler_EndFrame
 * to finalize and rotate the ring buffer.
 */

typedef enum {
    PROFILE_PHASE_COLLISION_AT,
    PROFILE_PHASE_COLLISION_OC,
    PROFILE_PHASE_COLLISION_DAMAGE,
    PROFILE_PHASE_ACTOR_UPDATE,
    PROFILE_PHASE_EFFECTS,
    PROFILE_PHASE_ACTOR_DRAW,
    PROFILE_PHASE_SCENE_DRAW,
    PROFILE_PHASE_PLAY_UPDATE,
    PROFILE_PHASE_PLAY_DRAW,
    PROFILE_PHASE_FRAME_INTERP,
    PROFILE_PHASE_AUDIO_WAIT,
    PROFILE_PHASE_DL_PROCESS,
    PROFILE_PHASE_GFX_COMMANDS,
    PROFILE_PHASE_TOTAL_FRAME,
    PROFILE_PHASE_MAX
} ProfilePhase;

void FrameProfiler_StartPhase(ProfilePhase phase);
void FrameProfiler_EndPhase(ProfilePhase phase);
void FrameProfiler_EndFrame(void);
float FrameProfiler_GetPhaseAvgMs(ProfilePhase phase);
int FrameProfiler_IsEnabled(void);

#ifdef __cplusplus
}

class FrameProfilerWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void InitElement() override;
    void DrawElement() override;
    void UpdateElement() override;
};

#endif
