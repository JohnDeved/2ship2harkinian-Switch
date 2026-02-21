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
    PROFILE_COUNTER_DL_REPLAY_ITERATIONS, // number of RunReplay calls per frame
    PROFILE_COUNTER_DL_REPLAY_FALLBACKS,  // non-first iterations that fell back to full interpretation
    PROFILE_COUNTER_DL_REPLAY_BRANCHZ_FRAMES, // first iterations where replay was blocked by G_BRANCH_Z
    PROFILE_COUNTER_DL_REPLAY_COOLDOWN_SKIPS, // first iterations skipped due branch-z cooldown
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
    PROFILE_COUNTER_GL_BUFFER_FULL_FLUSHES,
    PROFILE_COUNTER_GL_STATE_FLUSHES,
    PROFILE_COUNTER_GL_SHADER_SWITCHES,
    PROFILE_COUNTER_GL_SHADER_COMPILATIONS,
    PROFILE_COUNTER_GL_TEXTURE_BINDS,
    PROFILE_COUNTER_GL_TEXTURE_CACHE_MISSES,
    PROFILE_COUNTER_GL_VERTICES_SUBMITTED,
    PROFILE_COUNTER_GL_TRIANGLES_SUBMITTED,
    PROFILE_COUNTER_GL_TIME_TOTAL_MS,
    PROFILE_COUNTER_GL_TIME_DISPATCH_MS,
    PROFILE_COUNTER_GL_TIME_TRI_MS,
    PROFILE_COUNTER_GL_TIME_TEX_MS,
    PROFILE_COUNTER_GL_TIME_SHADER_MS,
    PROFILE_COUNTER_GL_TIME_DRAW_MS,
    PROFILE_COUNTER_GL_TIME_VBO_UPLOAD_MS,
    PROFILE_COUNTER_GL_TIME_GL_DRAW_MS,
    PROFILE_COUNTER_GL_TIME_VTX_MS,
    PROFILE_COUNTER_GL_TIME_MTX_MS,
    PROFILE_COUNTER_GL_TIME_DEPTH_MS,
    PROFILE_COUNTER_GL_TIME_SETUP_MS,
    PROFILE_COUNTER_GL_PIXEL_DEPTH_QUERIES,
    PROFILE_COUNTER_GL_AVG_BATCH_SIZE,

    // Command handler timing breakdown (subset of dispatch time)
    PROFILE_COUNTER_GL_TIME_TEXTURE_LOADING_MS,
    PROFILE_COUNTER_GL_TIME_RECT_DRAWING_MS,
    PROFILE_COUNTER_GL_TIME_DL_OPS_MS,
    PROFILE_COUNTER_GL_TIME_COMBINER_SETUP_MS,
    PROFILE_COUNTER_GL_TIME_FRAMEBUFFER_OPS_MS,

    // Flush cause breakdown: which state triggered each batch-breaking flush
    PROFILE_COUNTER_GL_FLUSH_CAUSE_TEXTURE,
    PROFILE_COUNTER_GL_FLUSH_CAUSE_SAMPLER,
    PROFILE_COUNTER_GL_FLUSH_CAUSE_SHADER,
    PROFILE_COUNTER_GL_FLUSH_CAUSE_ALPHA,
    PROFILE_COUNTER_GL_FLUSH_CAUSE_DEPTH_VIEWPORT,
    PROFILE_COUNTER_GL_FLUSH_CAUSE_COMBINER,
    PROFILE_COUNTER_GL_TEXTURE_RELOAD_SKIPS,

    // Batch size histogram: draws per bucket (1-2, 3-8, 9-32, 33-128, 129+)
    PROFILE_COUNTER_GL_BATCH_HIST_0,       // 1-2 tris
    PROFILE_COUNTER_GL_BATCH_HIST_1,       // 3-8 tris
    PROFILE_COUNTER_GL_BATCH_HIST_2,       // 9-32 tris
    PROFILE_COUNTER_GL_BATCH_HIST_3,       // 33-128 tris
    PROFILE_COUNTER_GL_BATCH_HIST_4,       // 129+ tris
    PROFILE_COUNTER_GL_MAX_BATCH_SIZE,     // max batch size seen this frame

    // System telemetry (Switch: real values where available, otherwise proxy/zero)
    PROFILE_COUNTER_SYS_CPU_USAGE_PCT,     // CPU usage across CPU cores (0-100)
    PROFILE_COUNTER_SYS_GPU_USAGE_EST_PCT, // estimated GPU usage proxy (0-100)
    PROFILE_COUNTER_SYS_RAM_USAGE_PCT,     // process RAM usage percentage (0-100)
    PROFILE_COUNTER_SYS_RAM_USED_MB,       // process RAM used (MB)
    PROFILE_COUNTER_SYS_RAM_TOTAL_MB,      // process RAM budget/total (MB)
    PROFILE_COUNTER_SYS_CPU_CLOCK_MHZ,     // CPU clock (MHz)
    PROFILE_COUNTER_SYS_GPU_CLOCK_MHZ,     // GPU clock (MHz)
    PROFILE_COUNTER_SYS_EMC_CLOCK_MHZ,     // memory controller clock (MHz)

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

// Keep the profiler enabled for the next few frames, even if the profiler window
// is not open. Call each frame from systems that need profiler data (e.g. benchmark).
void FrameProfiler_KeepAlive(void);

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
