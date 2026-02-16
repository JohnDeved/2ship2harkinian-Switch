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

typedef enum {
    PROFILE_COUNTER_DL_ITERATIONS, // number of DrawAndRunGraphicsCommands calls per frame
    PROFILE_COUNTER_DL_COMMANDS,   // total GBI commands across all buffers
    PROFILE_COUNTER_DL_TRIANGLES,  // G_TRI1 + 2*G_TRI2 triangle count
    PROFILE_COUNTER_DL_VERTICES,   // total vertices loaded (from G_VTX)
    PROFILE_COUNTER_DL_TEX_LOADS,  // G_SETTIMG (texture image source changes)
    PROFILE_COUNTER_DL_MTX_LOADS,  // G_MTX (matrix push/loads)
    PROFILE_COUNTER_DL_PIPE_SYNCS, // G_RDPPIPESYNC (RDP state barrier)
    PROFILE_COUNTER_DL_SUBCALLS,   // G_DL (display list call/branch)
    PROFILE_COUNTER_DL_SETCOMBINE, // G_SETCOMBINE (shader/combiner changes)

    // Fast3D internal counters (libultraship, actual backend activity)
    PROFILE_COUNTER_GL_DRAW_CALLS,
    PROFILE_COUNTER_GL_BATCH_FLUSHES,
    PROFILE_COUNTER_GL_STATE_FLUSHES,
    PROFILE_COUNTER_GL_SHADER_SWITCHES,
    PROFILE_COUNTER_GL_SHADER_COMPILATIONS,
    PROFILE_COUNTER_GL_TEXTURE_BINDS,
    PROFILE_COUNTER_GL_TEXTURE_CACHE_MISSES,
    PROFILE_COUNTER_GL_VERTICES_SUBMITTED,
    PROFILE_COUNTER_GL_TRIANGLES_SUBMITTED,
    PROFILE_COUNTER_GL_TIME_TOTAL_MS,
    PROFILE_COUNTER_GL_TIME_TRI_MS,
    PROFILE_COUNTER_GL_TIME_TEX_MS,
    PROFILE_COUNTER_GL_TIME_SHADER_MS,
    PROFILE_COUNTER_GL_TIME_DRAW_MS,
    PROFILE_COUNTER_GL_TIME_VTX_MS,
    PROFILE_COUNTER_GL_AVG_BATCH_SIZE,
    PROFILE_COUNTER_MAX
} ProfileCounter;

// Per-buffer display list statistics (OPA, XLU, Overlay, Work, Debug)
#define PROFILE_DL_BUFFER_COUNT 5
#define PROFILE_DL_BUF_OPA 0
#define PROFILE_DL_BUF_XLU 1
#define PROFILE_DL_BUF_OVERLAY 2
#define PROFILE_DL_BUF_WORK 3
#define PROFILE_DL_BUF_DEBUG 4

typedef struct {
    int commands;
    int triangles;
    int vertices;
    int texLoads;
    int mtxLoads;
    int pipeSyncs;
    int subcalls;
    int setCombine;
} DLBufferStats;

void FrameProfiler_StartPhase(ProfilePhase phase);
void FrameProfiler_EndPhase(ProfilePhase phase);
void FrameProfiler_EndFrame(void);
float FrameProfiler_GetPhaseAvgMs(ProfilePhase phase);
void FrameProfiler_AddCounter(ProfileCounter counter, float value);
float FrameProfiler_GetCounterAvg(ProfileCounter counter);
int FrameProfiler_IsEnabled(void);

// Scan all 5 DL buffers from the graphics context. Call from graph.c after
// GameState_Update has filled the buffers but before Graph_ProcessGfxCommands.
struct GraphicsContext;
void FrameProfiler_ScanAllBuffers(struct GraphicsContext* gfxCtx);

// Get per-buffer stats (averaged over ring buffer). bufIdx = PROFILE_DL_BUF_*
DLBufferStats FrameProfiler_GetBufferStats(int bufIdx);

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
