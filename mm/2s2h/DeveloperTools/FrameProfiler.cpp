#include "FrameProfiler.h"

#include <imgui.h>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <ctime>
#include <cmath>
#include <libultraship/libultraship.h>

extern "C" {
#include "gfx.h"
}

#include "build.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

// Ring buffer depth — average over this many frames for stable display
#define PROFILE_RING_SIZE 60

// Frames to keep profiler running after the window stops drawing
#define PROFILE_KEEPALIVE_FRAMES 2

// ── Timing helper ──────────────────────────────────────────────────────

static inline uint64_t ProfileGetNs(void) {
#ifdef _WIN32
    static LARGE_INTEGER sFreq;
    static bool sFreqInitialized = false;
    if (!sFreqInitialized) {
        QueryPerformanceFrequency(&sFreq);
        sFreqInitialized = true;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000000000ULL / sFreq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

// ── Per-phase data ─────────────────────────────────────────────────────
// Each phase enum value is exclusively started/ended by one specific thread,
// so sPhaseStart[phase] and sPhaseRing[phase][...] have single-writer semantics.
// sRingIndex and sEnabled are shared between main thread and workers — use atomics.

static uint64_t sPhaseStart[PROFILE_PHASE_MAX];
static float sPhaseRing[PROFILE_PHASE_MAX][PROFILE_RING_SIZE]; // ms per frame
static float sCounterRing[PROFILE_COUNTER_MAX][PROFILE_RING_SIZE];
// Number of fields in DLBufferStats tracked per buffer
#define PROFILE_DL_FIELD_COUNT 8

static float sBufferStatsRing[PROFILE_DL_BUFFER_COUNT][PROFILE_DL_FIELD_COUNT][PROFILE_RING_SIZE];
static float sPhaseRunningSum[PROFILE_PHASE_MAX];
static float sCounterRunningSum[PROFILE_COUNTER_MAX];
static float sBufferStatsRunningSum[PROFILE_DL_BUFFER_COUNT][PROFILE_DL_FIELD_COUNT];
static int sRunningSumRecalcCountdown = 0; // recompute true sums periodically to prevent float drift
static std::atomic<int> sRingIndex{ 0 };
static std::atomic<int> sEnabled{ 0 };
static int sDrawCountdown = 0; // main-thread only
static bool sProfilingActive = true; // user toggle — when false, zero overhead (sEnabled stays 0)

// ── C API ──────────────────────────────────────────────────────────────

extern "C" void FrameProfiler_StartPhase(ProfilePhase phase) {
    if (!sEnabled.load(std::memory_order_relaxed))
        return;
    sPhaseStart[phase] = ProfileGetNs();
}

extern "C" void FrameProfiler_EndPhase(ProfilePhase phase) {
    if (!sEnabled.load(std::memory_order_relaxed))
        return;
    uint64_t elapsed = ProfileGetNs() - sPhaseStart[phase];
    const float elapsedMs = (float)elapsed / 1e6f;
    const int ri = sRingIndex.load(std::memory_order_relaxed);
    sPhaseRing[phase][ri] += elapsedMs; // ns → ms
    sPhaseRunningSum[phase] += elapsedMs;
}

extern "C" void FrameProfiler_EndFrame(void) {
    if (!sEnabled.load(std::memory_order_relaxed))
        return;
    int next = (sRingIndex.load(std::memory_order_relaxed) + 1) % PROFILE_RING_SIZE;
    // Clear the next slot so phases not measured this frame show 0
    for (int i = 0; i < PROFILE_PHASE_MAX; i++) {
        sPhaseRunningSum[i] -= sPhaseRing[i][next];
        sPhaseRing[i][next] = 0.0f;
    }
    for (int i = 0; i < PROFILE_COUNTER_MAX; i++) {
        sCounterRunningSum[i] -= sCounterRing[i][next];
        sCounterRing[i][next] = 0.0f;
    }
    for (int b = 0; b < PROFILE_DL_BUFFER_COUNT; b++) {
        for (int f = 0; f < PROFILE_DL_FIELD_COUNT; f++) {
            sBufferStatsRunningSum[b][f] -= sBufferStatsRing[b][f][next];
            sBufferStatsRing[b][f][next] = 0.0f;
        }
    }
    sRingIndex.store(next, std::memory_order_relaxed);

    // Periodically recompute running sums from scratch to prevent float drift.
    // This runs once every PROFILE_RING_SIZE frames (~2 seconds at 60fps).
    if (--sRunningSumRecalcCountdown <= 0) {
        sRunningSumRecalcCountdown = PROFILE_RING_SIZE;
        for (int i = 0; i < PROFILE_PHASE_MAX; i++) {
            float sum = 0.0f;
            for (int j = 0; j < PROFILE_RING_SIZE; j++) sum += sPhaseRing[i][j];
            sPhaseRunningSum[i] = sum;
        }
        for (int i = 0; i < PROFILE_COUNTER_MAX; i++) {
            float sum = 0.0f;
            for (int j = 0; j < PROFILE_RING_SIZE; j++) sum += sCounterRing[i][j];
            sCounterRunningSum[i] = sum;
        }
        for (int b = 0; b < PROFILE_DL_BUFFER_COUNT; b++) {
            for (int f = 0; f < PROFILE_DL_FIELD_COUNT; f++) {
                float sum = 0.0f;
                for (int j = 0; j < PROFILE_RING_SIZE; j++) sum += sBufferStatsRing[b][f][j];
                sBufferStatsRunningSum[b][f] = sum;
            }
        }
    }

    // Auto-disable when window hasn't drawn recently
    if (sDrawCountdown > 0) {
        sDrawCountdown--;
    } else {
        sEnabled.store(0, std::memory_order_relaxed);
    }
}

extern "C" float FrameProfiler_GetPhaseAvgMs(ProfilePhase phase) {
    return sPhaseRunningSum[phase] / PROFILE_RING_SIZE;
}

extern "C" void FrameProfiler_AddCounter(ProfileCounter counter, float value) {
    if (!sEnabled.load(std::memory_order_relaxed))
        return;
    // Note: Counters are assumed to be incremented from the main thread only.
    // If a counter is used from worker threads, it must be made atomic.
    const int ri = sRingIndex.load(std::memory_order_relaxed);
    sCounterRing[counter][ri] += value;
    sCounterRunningSum[counter] += value;
}

extern "C" float FrameProfiler_GetCounterAvg(ProfileCounter counter) {
    return sCounterRunningSum[counter] / PROFILE_RING_SIZE;
}

extern "C" int FrameProfiler_IsEnabled(void) {
    return sEnabled.load(std::memory_order_relaxed);
}

extern "C" void FrameProfiler_KeepAlive(void) {
    sEnabled.store(1, std::memory_order_relaxed);
    sDrawCountdown = PROFILE_KEEPALIVE_FRAMES;
}

// ── GBI Display List Scanner ───────────────────────────────────────────
// Scan a single linear DL buffer (start to end pointer) counting commands by type.
// Each Gfx command is 8 bytes (2 × uint32_t). Stops at G_ENDDL or buffer end.

static DLBufferStats ScanBuffer(const Gfx* start, const Gfx* end) {
    DLBufferStats stats = {};
    if (start == NULL || end == NULL || end <= start)
        return stats;

    const uint32_t* cmd = (const uint32_t*)start;
    const uint32_t* limit = (const uint32_t*)end;

    while (cmd < limit) {
        uint32_t w0 = cmd[0];
        uint8_t opcode = (w0 >> 24) & 0xFF;
        stats.commands++;

        switch (opcode) {
            case 0x05: // G_TRI1
                stats.triangles += 1;
                break;
            case 0x06: // G_TRI2
                stats.triangles += 2;
                break;
            case 0x01: // G_VTX
                stats.vertices += ((w0 >> 12) & 0xFF);
                break;
            case 0xFD: // G_SETTIMG
            case 0x23: // G_SETTIMG_OTR_HASH
            case 0x24: // G_SETTIMG_OTR_FILEPATH
                stats.texLoads++;
                break;
            case 0xDA: // G_MTX
            case 0x20: // G_MTX_OTR
                stats.mtxLoads++;
                break;
            case 0xE7: // G_RDPPIPESYNC
                stats.pipeSyncs++;
                break;
            case 0xDE: // G_DL
            case 0x21: // G_DL_OTR_HASH
            case 0x22: // G_DL_OTR_FILEPATH
                stats.subcalls++;
                break;
            case 0xFC: // G_SETCOMBINE
                stats.setCombine++;
                break;
            case 0xDF: // G_ENDDL
                return stats;
            default:
                break;
        }
        cmd += 2; // each Gfx command is 8 bytes
    }
    return stats;
}

extern "C" void FrameProfiler_ScanAllBuffers(GraphicsContext* gfxCtx) {
    if (!sEnabled.load(std::memory_order_relaxed) || gfxCtx == NULL)
        return;

    int ri = sRingIndex.load(std::memory_order_relaxed);

    // Buffer mapping: OPA=0, XLU=1, Overlay=2, Work=3, Debug=4
    struct {
        Gfx* start;
        Gfx* end;
    } buffers[PROFILE_DL_BUFFER_COUNT] = {
        { (Gfx*)gfxCtx->polyOpa.start, gfxCtx->polyOpa.p }, { (Gfx*)gfxCtx->polyXlu.start, gfxCtx->polyXlu.p },
        { (Gfx*)gfxCtx->overlay.start, gfxCtx->overlay.p }, { (Gfx*)gfxCtx->work.start, gfxCtx->work.p },
        { (Gfx*)gfxCtx->debug.start, gfxCtx->debug.p },
    };

    int totalCmds = 0, totalTris = 0, totalVerts = 0;
    int totalTexLoads = 0, totalMtxLoads = 0, totalPipeSyncs = 0;
    int totalSubcalls = 0, totalSetCombine = 0;

    for (int b = 0; b < PROFILE_DL_BUFFER_COUNT; b++) {
        DLBufferStats s = ScanBuffer(buffers[b].start, buffers[b].end);

        // Store per-buffer stats in ring buffer
        const float vals[PROFILE_DL_FIELD_COUNT] = {
            (float)s.commands, (float)s.triangles, (float)s.vertices, (float)s.texLoads,
            (float)s.mtxLoads, (float)s.pipeSyncs, (float)s.subcalls,  (float)s.setCombine
        };
        for (int f = 0; f < PROFILE_DL_FIELD_COUNT; f++) {
            const float oldVal = sBufferStatsRing[b][f][ri];
            const float newVal = vals[f];
            sBufferStatsRing[b][f][ri] = newVal;
            sBufferStatsRunningSum[b][f] += newVal - oldVal;
        }

        totalCmds += s.commands;
        totalTris += s.triangles;
        totalVerts += s.vertices;
        totalTexLoads += s.texLoads;
        totalMtxLoads += s.mtxLoads;
        totalPipeSyncs += s.pipeSyncs;
        totalSubcalls += s.subcalls;
        totalSetCombine += s.setCombine;
    }

    // Write totals to the counter ring for backward compatibility
    FrameProfiler_AddCounter(PROFILE_COUNTER_DL_COMMANDS, (float)totalCmds);
    FrameProfiler_AddCounter(PROFILE_COUNTER_DL_TRIANGLES, (float)totalTris);
    FrameProfiler_AddCounter(PROFILE_COUNTER_DL_VERTICES, (float)totalVerts);
    FrameProfiler_AddCounter(PROFILE_COUNTER_DL_TEX_LOADS, (float)totalTexLoads);
    FrameProfiler_AddCounter(PROFILE_COUNTER_DL_MTX_LOADS, (float)totalMtxLoads);
    FrameProfiler_AddCounter(PROFILE_COUNTER_DL_PIPE_SYNCS, (float)totalPipeSyncs);
    FrameProfiler_AddCounter(PROFILE_COUNTER_DL_SUBCALLS, (float)totalSubcalls);
    FrameProfiler_AddCounter(PROFILE_COUNTER_DL_SETCOMBINE, (float)totalSetCombine);
}

extern "C" DLBufferStats FrameProfiler_GetBufferStats(int bufIdx) {
    DLBufferStats stats = {};
    if (bufIdx < 0 || bufIdx >= PROFILE_DL_BUFFER_COUNT)
        return stats;

    stats.commands = (int)(sBufferStatsRunningSum[bufIdx][0] / PROFILE_RING_SIZE);
    stats.triangles = (int)(sBufferStatsRunningSum[bufIdx][1] / PROFILE_RING_SIZE);
    stats.vertices = (int)(sBufferStatsRunningSum[bufIdx][2] / PROFILE_RING_SIZE);
    stats.texLoads = (int)(sBufferStatsRunningSum[bufIdx][3] / PROFILE_RING_SIZE);
    stats.mtxLoads = (int)(sBufferStatsRunningSum[bufIdx][4] / PROFILE_RING_SIZE);
    stats.pipeSyncs = (int)(sBufferStatsRunningSum[bufIdx][5] / PROFILE_RING_SIZE);
    stats.subcalls = (int)(sBufferStatsRunningSum[bufIdx][6] / PROFILE_RING_SIZE);
    stats.setCombine = (int)(sBufferStatsRunningSum[bufIdx][7] / PROFILE_RING_SIZE);
    return stats;
}

// ── Phase names for display ────────────────────────────────────────────

static const char* sPhaseNames[PROFILE_PHASE_MAX] = {
    "Collision AT", "Collision OC", "Collision Damage", "Actor Update", "Effects",    "Actor Draw",   "Scene Draw",
    "Play Update",  "Play Draw",    "Frame Interp",     "Audio Wait",   "DL Process", "GFX Commands", "Total Frame",
};

// Core assignment labels for display
static const char* sPhaseCoreLabels[PROFILE_PHASE_MAX] = {
    "Core 0", // AT (main thread)
    "Core 3", // OC (worker thread on Switch; may fall back)
    "Core 0", // Damage
    "Core 0", // Actor Update
    "Core 0", // Effects
    "Core 0", // Actor Draw
    "Core 0", // Scene Draw
    "Core 0", // Play Update
    "Core 0", // Play Draw
    "Core 0", // Frame Interp
    "Core 0", // Audio Wait
    "Core 0", // DL Process
    "Core 0", // GFX Commands
    "Core 0", // Total
};

static const char* sCounterNames[PROFILE_COUNTER_MAX] = {
    "DL Iterations", "DL Replay Iters", "DL Replay Fallback", "DL Replay BranchZ", "DL Replay Cooldown",
    "DL Commands", "Triangles", "Vertices", "Tex Loads", "Matrix Loads", "Pipe Syncs", "DL Subcalls", "SetCombine",
    "GL Draw Calls", "GL Batch Flushes", "GL BufFull Flushes", "GL State Flushes", "GL Shader Switches",
    "GL Shader Compiles", "GL Texture Binds", "GL Tex Cache Miss", "GL Vert Submitted", "GL Tri Submitted",
    "GL Time Total ms", "GL Time Dispatch ms", "GL Time Tri ms", "GL Time Tex ms", "GL Time Shader ms",
    "GL Time Draw ms", "GL Time VBO Upload ms", "GL Time glDraw ms", "GL Time Vtx ms", "GL Time Mtx ms",
    "GL Time Depth ms", "GL Time Setup ms", "GL Depth Queries", "GL Avg Batch Size",
    "GL Time TexLoad ms", "GL Time RectDraw ms", "GL Time DLOps ms", "GL Time CombSetup ms", "GL Time FBOps ms",
    "Flush:Texture", "Flush:Sampler", "Flush:Shader", "Flush:Alpha", "Flush:Depth/VP", "Flush:Combiner",
    "Tex Reload Skips", "Batch:1-2 tris", "Batch:3-8 tris", "Batch:9-32 tris", "Batch:33-128 tris",
    "Batch:129+ tris", "Max Batch Size", "Sys CPU Usage %", "Sys GPU Usage Est %", "Sys RAM Usage %",
    "Sys RAM Used MB", "Sys RAM Total MB", "Sys CPU Clock MHz", "Sys GPU Clock MHz", "Sys EMC Clock MHz",
};

// ── Helper functions ───────────────────────────────────────────────────

static const float PROFILE_TARGET_FRAME_MS = 1000.0f / 60.0f;

// Returns true for leaf phases (not parent/aggregate phases) used for bottleneck detection
static bool IsLeafPhase(int phase) {
    return phase != PROFILE_PHASE_TOTAL_FRAME && phase != PROFILE_PHASE_PLAY_UPDATE &&
           phase != PROFILE_PHASE_PLAY_DRAW && phase != PROFILE_PHASE_GFX_COMMANDS;
}

// Sum of top-level non-overlapping phases (for computing unaccounted time)
static float GetAccountedTimeMs(void) {
    return FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_PLAY_UPDATE) +
           FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_PLAY_DRAW) +
           FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_AUDIO_WAIT) +
           FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_FRAME_INTERP) +
           FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_DL_PROCESS);
}

// ── Snapshot export ─────────────────────────────────────────────────────

static std::string sLastExportPath;
static float sExportMsgTimer = 0.0f;

// Previous snapshot key metrics for comparison
struct SnapshotBaseline {
    bool valid = false;
    std::string commitShort;
    float renderFrameMs;
    float fps;
    float dlProcessMs;
    float glTimeTri;
    float glTimeDraw;
    float glTimeVtx;
    float glTimeTex;
    float glDrawCalls;
    float glAvgBatch;
    float glTris;
    float emptyFlushPct;
};
static SnapshotBaseline sPrevSnapshot;

static std::string FrameProfiler_GetBaselinePath(void) {
    return Ship::Context::GetPathRelativeToAppDirectory("profiler_baseline.txt");
}

static void FrameProfiler_SaveBaseline(const SnapshotBaseline& b) {
    std::ofstream f(FrameProfiler_GetBaselinePath());
    if (!f.is_open()) return;
    f << b.commitShort << "\n"
      << b.renderFrameMs << "\n" << b.fps << "\n" << b.dlProcessMs << "\n"
      << b.glTimeTri << "\n" << b.glTimeDraw << "\n" << b.glTimeVtx << "\n"
      << b.glTimeTex << "\n" << b.glDrawCalls << "\n" << b.glAvgBatch << "\n"
      << b.glTris << "\n" << b.emptyFlushPct << "\n";
}

static void FrameProfiler_LoadBaseline(void) {
    std::ifstream f(FrameProfiler_GetBaselinePath());
    if (!f.is_open()) return;
    std::string commit;
    if (!std::getline(f, commit) || commit.empty()) return;
    SnapshotBaseline b;
    if (!(f >> b.renderFrameMs >> b.fps >> b.dlProcessMs
            >> b.glTimeTri >> b.glTimeDraw >> b.glTimeVtx >> b.glTimeTex
            >> b.glDrawCalls >> b.glAvgBatch >> b.glTris >> b.emptyFlushPct))
        return;
    b.valid = true;
    b.commitShort = commit;
    sPrevSnapshot = b;
}

static void FrameProfiler_ExportSnapshot(void) {
    // Build timestamped filename
    time_t now = std::time(nullptr);
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &now);
#else
    localtime_r(&now, &tmBuf);
#endif
    char timeBuf[64];
    std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", &tmBuf);

    // Build git info for filename and header
    std::string branch = (gGitBranch[0] != '\0') ? gGitBranch : "unknown";
    std::string commitFull = (gGitCommitHash[0] != '\0') ? gGitCommitHash : "unknown";
    std::string commitShort = (commitFull.size() > 7) ? commitFull.substr(0, 7) : commitFull;

    // Sanitize branch name for filename (replace / and spaces with -)
    std::string branchSafe = branch;
    for (char& c : branchSafe) {
        if (c == '/' || c == '\\' || c == ' ' || c == ':')
            c = '-';
    }

    std::string filename = "profiler_" + branchSafe + "_" + commitShort + "_" + std::string(timeBuf) + ".txt";
    std::string filepath = Ship::Context::GetPathRelativeToAppDirectory(filename);

    std::ofstream out(filepath);
    if (!out.is_open()) {
        sLastExportPath = "ERROR: Could not write " + filepath;
        sExportMsgTimer = 5.0f;
        return;
    }

    float totalMs = FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_TOTAL_FRAME);
    float dlIter = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_ITERATIONS);
    if (dlIter < 1.0f) dlIter = 1.0f;
    float renderFrameMs = totalMs / dlIter;
    float fps = (renderFrameMs > 0.01f) ? (1000.0f / renderFrameMs) : 0.0f;

    out << "=== 2S2H Frame Profiler Snapshot ===" << std::endl;
    out << "Timestamp: " << timeBuf << std::endl;
    out << "Branch: " << branch << std::endl;
    out << "Commit: " << commitFull << std::endl;
    out << "Build: " << gBuildVersion << " (" << gBuildDate << ")" << std::endl;
#ifdef __SWITCH__
    out << "Platform: Nintendo Switch (NX)" << std::endl;
#elif defined(_WIN32)
    out << "Platform: Windows" << std::endl;
#elif defined(__APPLE__)
    out << "Platform: macOS" << std::endl;
#elif defined(__linux__)
    out << "Platform: Linux" << std::endl;
#else
    out << "Platform: Unknown" << std::endl;
#endif
    out << std::endl;

    out << "--- Summary (" << PROFILE_RING_SIZE << "-frame average) ---" << std::endl;
    out << "Total Update Time:              " << std::fixed << std::setprecision(2) << totalMs << " ms (" << std::setprecision(0) << dlIter << " DL iterations)" << std::endl;
    out << "Per Rendered Frame:             " << std::fixed << std::setprecision(2) << renderFrameMs << " ms" << std::endl;
    out << "FPS:                            " << std::fixed << std::setprecision(1) << fps << std::endl;
    out << "Target (60 FPS):                " << std::fixed << std::setprecision(2) << PROFILE_TARGET_FRAME_MS << " ms" << std::endl;
    if (renderFrameMs > PROFILE_TARGET_FRAME_MS) {
        float overhead = ((renderFrameMs / PROFILE_TARGET_FRAME_MS) - 1.0f) * 100.0f;
        out << "Performance:                    " << std::fixed << std::setprecision(1) << overhead << "% over budget" << std::endl;
    } else {
        float headroom = ((PROFILE_TARGET_FRAME_MS - renderFrameMs) / PROFILE_TARGET_FRAME_MS) * 100.0f;
        out << "Performance:                    " << std::fixed << std::setprecision(1) << headroom << "% headroom remaining" << std::endl;
    }
    out << std::endl;

    out << "--- Per-Phase Breakdown ---" << std::endl;
    int worstPhase = -1;
    float worstMs = 0.0f;
    for (int i = 0; i < PROFILE_PHASE_MAX; i++) {
        float ms = FrameProfiler_GetPhaseAvgMs((ProfilePhase)i);
        float pct = (totalMs > 0.01f) ? (ms / totalMs * 100.0f) : 0.0f;
        char line[256];
        snprintf(line, sizeof(line), "%-7s %-22s %6.2f ms (%5.1f%%)", 
                 sPhaseCoreLabels[i], sPhaseNames[i], ms, pct);
        out << line << std::endl;
        // Track worst leaf phase
        if (IsLeafPhase(i)) {
            if (ms > worstMs) {
                worstMs = ms;
                worstPhase = i;
            }
        }
    }
    out << std::endl;

    // Counters
    out << "--- Display List Statistics ---" << std::endl;
    float dlCmds = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_COMMANDS);
    float dlReplayIters = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_REPLAY_ITERATIONS);
    float dlReplayFallbacks = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_REPLAY_FALLBACKS);
    float dlReplayBranchZFrames = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_REPLAY_BRANCHZ_FRAMES);
    float dlReplayCooldownSkips = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_REPLAY_COOLDOWN_SKIPS);
    float tris = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_TRIANGLES);
    float verts = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_VERTICES);
    float texLoads = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_TEX_LOADS);
    float mtxLoads = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_MTX_LOADS);
    float pipeSyncs = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_PIPE_SYNCS);
    float subcalls = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_SUBCALLS);
    float setCombine = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_SETCOMBINE);
    float glDrawCalls = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_DRAW_CALLS);
    float glBatchFlushes = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_BATCH_FLUSHES);
    float glBufferFullFlushes = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_BUFFER_FULL_FLUSHES);
    float glStateFlushes = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_STATE_FLUSHES);
    float glShaderSwitches = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_SHADER_SWITCHES);
    float glShaderCompiles = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_SHADER_COMPILATIONS);
    float glTextureBinds = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TEXTURE_BINDS);
    float glTextureMisses = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TEXTURE_CACHE_MISSES);
    float glVerts = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_VERTICES_SUBMITTED);
    float glTris = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TRIANGLES_SUBMITTED);
    float glTimeTotal = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_TOTAL_MS);
    float glTimeDispatch = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_DISPATCH_MS);
    float glTimeTri = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_TRI_MS);
    float glTimeTex = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_TEX_MS);
    float glTimeShader = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_SHADER_MS);
    float glTimeDraw = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_DRAW_MS);
    float glTimeVboUpload = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_VBO_UPLOAD_MS);
    float glTimeGlDraw = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_GL_DRAW_MS);
    float glTimeVtx = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_VTX_MS);
    float glTimeMtx = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_MTX_MS);
    float glTimeDepth = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_DEPTH_MS);
    float glTimeSetup = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_SETUP_MS);
    float glDepthQueries = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_PIXEL_DEPTH_QUERIES);
    float glAvgBatch = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_AVG_BATCH_SIZE);

    // GBI/Display List counters (game-side)
    out << "DL Iterations:                  " << std::fixed << std::setprecision(0) << dlIter << std::endl;
    out << "DL Replay Iterations:           " << std::fixed << std::setprecision(0) << dlReplayIters << std::endl;
    out << "DL Replay Fallback Iterations:  " << std::fixed << std::setprecision(0) << dlReplayFallbacks << std::endl;
    out << "DL Replay Branch-Z Blocks:      " << std::fixed << std::setprecision(0) << dlReplayBranchZFrames << std::endl;
    out << "DL Replay Cooldown Skips:       " << std::fixed << std::setprecision(0) << dlReplayCooldownSkips << std::endl;
    out << "Total Commands (all buffers):   " << std::fixed << std::setprecision(0) << dlCmds << std::endl;
    out << "Triangles:                      " << std::fixed << std::setprecision(0) << tris << std::endl;
    out << "Vertices:                       " << std::fixed << std::setprecision(0) << verts << std::endl;
    out << "Texture Loads (G_SETTIMG):      " << std::fixed << std::setprecision(0) << texLoads << std::endl;
    out << "Matrix Loads (G_MTX):           " << std::fixed << std::setprecision(0) << mtxLoads << std::endl;
    out << "Pipe Syncs (G_RDPPIPESYNC):     " << std::fixed << std::setprecision(0) << pipeSyncs << std::endl;
    out << "DL Subcalls (G_DL):             " << std::fixed << std::setprecision(0) << subcalls << std::endl;
    out << "SetCombine (G_SETCOMBINE):      " << std::fixed << std::setprecision(0) << setCombine << std::endl;
    out << std::endl;
    
    // Derived metrics for context
    float vertsPerTri = (tris > 0.5f) ? (verts / tris) : 0.0f;
    out << "Derived Metrics:" << std::endl;
    out << "  Vertices per Triangle:        " << std::fixed << std::setprecision(2) << vertsPerTri << " (~3.0 for indexed lists, ~1.0 for strips/fans, >3.0 indicates duplication)" << std::endl;
    out << "  Commands per Triangle:        " << std::fixed << std::setprecision(2) << (tris > 0.5f ? dlCmds / tris : 0.0f) << std::endl;
    out << std::endl;
    
    // Fast3D/GL backend counters
    out << "Fast3D Backend (OpenGL):" << std::endl;
    out << "Est. Draw Calls (from PipeSyncs): ~" << std::fixed << std::setprecision(0) << pipeSyncs << " (upper bound, actual may differ)" << std::endl;
    out << "GL Draw Calls (actual):         " << std::fixed << std::setprecision(0) << glDrawCalls << std::endl;
    out << "GL Batch Flushes:               " << std::fixed << std::setprecision(0) << glBatchFlushes 
        << " (state: " << std::fixed << std::setprecision(0) << (glBatchFlushes - glBufferFullFlushes) 
        << ", buf-full: " << std::setprecision(0) << glBufferFullFlushes << ")" << std::endl;
    out << "GL Shader Switches:             " << std::fixed << std::setprecision(0) << glShaderSwitches 
        << " (compiles: " << std::fixed << std::setprecision(0) << glShaderCompiles << ")" << std::endl;
    out << "GL Texture Binds:               " << std::fixed << std::setprecision(0) << glTextureBinds 
        << " (cache misses: " << std::fixed << std::setprecision(0) << glTextureMisses << ")" << std::endl;
    out << "GL Triangles Submitted:         " << std::fixed << std::setprecision(0) << glTris << std::endl;
    out << "GL Vertices Submitted:          " << std::fixed << std::setprecision(0) << glVerts << std::endl;
    out << "GL Avg Batch Size:              " << std::fixed << std::setprecision(1) << glAvgBatch << " tris/draw" << std::endl;
    if (glDepthQueries > 0.0f) {
        out << "Pixel Depth Queries:            " << std::fixed << std::setprecision(0) << glDepthQueries << std::endl;
    }
    out << std::endl;
    
    // Efficiency metrics
    out << "Backend Efficiency Metrics:" << std::endl;
    float drawsPerShader = (glShaderSwitches > 0.5f) ? (glDrawCalls / glShaderSwitches) : 0.0f;
    float texBindsPerDraw = (glDrawCalls > 0.5f) ? (glTextureBinds / glDrawCalls) : 0.0f;
    float cacheMissRate = (glTextureBinds > 0.5f) ? (glTextureMisses / glTextureBinds * 100.0f) : 0.0f;
    out << "  Draw calls per shader:        " << std::fixed << std::setprecision(1) << drawsPerShader << " (higher is better for batching)" << std::endl;
    out << "  Texture binds per draw:       " << std::fixed << std::setprecision(2) << texBindsPerDraw << " (lower is better)" << std::endl;
    out << "  Texture cache miss rate:      " << std::fixed << std::setprecision(1) << cacheMissRate << "%" << std::endl;
    out << std::endl;
    
    // GL timing breakdown (multi-line for readability)
    out << "Fast3D Timing Breakdown:" << std::endl;
    out << "  Total:                        " << std::fixed << std::setprecision(2) << glTimeTotal << " ms" << std::endl;
    float glTimeTriExcl = glTimeTri > glTimeDraw ? glTimeTri - glTimeDraw : 0.0f;
    float glTimeDrawOther = glTimeDraw > (glTimeVboUpload + glTimeGlDraw) ? glTimeDraw - glTimeVboUpload - glTimeGlDraw : 0.0f;
    out << "  Triangle Processing:          " << std::fixed << std::setprecision(2) << glTimeTri << " ms (incl. draw submit)" << std::endl;
    out << "    Per-vertex work:            " << std::fixed << std::setprecision(2) << glTimeTriExcl << " ms" << std::endl;
    out << "    Draw Submit (GL calls):     " << std::fixed << std::setprecision(2) << glTimeDraw << " ms" << std::endl;
    out << "      VBO Upload:               " << std::fixed << std::setprecision(2) << glTimeVboUpload << " ms (glBufferData/glBufferSubData)" << std::endl;
    out << "      glDrawArrays:             " << std::fixed << std::setprecision(2) << glTimeGlDraw << " ms" << std::endl;
    out << "      State/Uniform setup:      " << std::fixed << std::setprecision(2) << glTimeDrawOther << " ms (depth/decal/uniforms)" << std::endl;
    out << "  Vertex Transform:             " << std::fixed << std::setprecision(2) << glTimeVtx << " ms" << std::endl;
    out << "  Texture Setup:                " << std::fixed << std::setprecision(2) << glTimeTex << " ms" << std::endl;
    out << "  Matrix Operations:            " << std::fixed << std::setprecision(2) << glTimeMtx << " ms" << std::endl;
    out << "  Dispatch (command walk):      " << std::fixed << std::setprecision(2) << glTimeDispatch << " ms" << std::endl;
    out << "  Pixel Depth Readback:         " << std::fixed << std::setprecision(2) << glTimeDepth << " ms" << std::endl;
    out << "  Frame Setup:                  " << std::fixed << std::setprecision(2) << glTimeSetup << " ms" << std::endl;
    out << "  Shader Compile/Switch:        " << std::fixed << std::setprecision(2) << glTimeShader << " ms" << std::endl;

    // GL timing percentage breakdown
    out << std::endl;
    out << "Fast3D Time Distribution (% of DL Process):" << std::endl;
    if (glTimeTotal > 0.01f) {
        out << "  Triangle Processing:          " << std::fixed << std::setprecision(1) << (glTimeTri / glTimeTotal * 100.0f) << "% (incl. draw submit)" << std::endl;
        out << "    Per-vertex work:            " << std::fixed << std::setprecision(1) << (glTimeTriExcl / glTimeTotal * 100.0f) << "%" << std::endl;
        out << "    Draw Submit (GL calls):     " << std::fixed << std::setprecision(1) << (glTimeDraw / glTimeTotal * 100.0f) << "%" << std::endl;
        out << "      VBO Upload:               " << std::fixed << std::setprecision(1) << (glTimeVboUpload / glTimeTotal * 100.0f) << "%" << std::endl;
        out << "      glDrawArrays:             " << std::fixed << std::setprecision(1) << (glTimeGlDraw / glTimeTotal * 100.0f) << "%" << std::endl;
        out << "      State/Uniform setup:      " << std::fixed << std::setprecision(1) << (glTimeDrawOther / glTimeTotal * 100.0f) << "%" << std::endl;
        out << "  Vertex Transform:             " << std::fixed << std::setprecision(1) << (glTimeVtx / glTimeTotal * 100.0f) << "%" << std::endl;
        out << "  Texture Setup:                " << std::fixed << std::setprecision(1) << (glTimeTex / glTimeTotal * 100.0f) << "%" << std::endl;
        out << "  Matrix Operations:            " << std::fixed << std::setprecision(1) << (glTimeMtx / glTimeTotal * 100.0f) << "%" << std::endl;
        out << "  Dispatch (command walk):      " << std::fixed << std::setprecision(1) << (glTimeDispatch / glTimeTotal * 100.0f) << "%" << std::endl;
        out << "  Pixel Depth Readback:         " << std::fixed << std::setprecision(1) << (glTimeDepth / glTimeTotal * 100.0f) << "%" << std::endl;
        out << "  Frame Setup:                  " << std::fixed << std::setprecision(1) << (glTimeSetup / glTimeTotal * 100.0f) << "%" << std::endl;
        out << "  Shader Compile/Switch:        " << std::fixed << std::setprecision(1) << (glTimeShader / glTimeTotal * 100.0f) << "%" << std::endl;
    }
    out << std::endl;

    // CPU vs GL summary — shows total time in GL driver calls vs CPU-side processing
    out << "--- CPU vs GL Driver Time ---" << std::endl;
    float glDriverTime = glTimeVboUpload + glTimeGlDraw + glTimeSetup + glTimeDepth;
    float cpuProcessingTime = glTimeTotal > glDriverTime ? glTimeTotal - glDriverTime : 0.0f;
    out << "GL Driver (VBO + Draw + Setup + Depth): " << std::fixed << std::setprecision(2) << glDriverTime << " ms";
    if (glTimeTotal > 0.01f) {
        out << " (" << std::setprecision(1) << (glDriverTime / glTimeTotal * 100.0f) << "%)";
    }
    out << std::endl;
    out << "  VBO Upload (glBufferData/Sub):  " << std::fixed << std::setprecision(2) << glTimeVboUpload << " ms" << std::endl;
    out << "  glDrawArrays:                   " << std::fixed << std::setprecision(2) << glTimeGlDraw << " ms" << std::endl;
    out << "  Frame Setup (FB/clear/resolve):  " << std::fixed << std::setprecision(2) << glTimeSetup << " ms" << std::endl;
    out << "  Pixel Depth (glReadPixels):      " << std::fixed << std::setprecision(2) << glTimeDepth << " ms" << std::endl;
    out << "CPU Processing (DL + tri + tex + vtx + mtx + shader): " << std::fixed << std::setprecision(2) << cpuProcessingTime << " ms";
    if (glTimeTotal > 0.01f) {
        out << " (" << std::setprecision(1) << (cpuProcessingTime / glTimeTotal * 100.0f) << "%)";
    }
    out << std::endl;
    out << "  DL Dispatch (command walk):     " << std::fixed << std::setprecision(2) << glTimeDispatch << " ms" << std::endl;
    out << "  Triangle Processing (CPU):      " << std::fixed << std::setprecision(2) << glTimeTriExcl << " ms" << std::endl;
    out << "  Vertex Transform:               " << std::fixed << std::setprecision(2) << glTimeVtx << " ms" << std::endl;
    out << "  Texture Setup:                  " << std::fixed << std::setprecision(2) << glTimeTex << " ms" << std::endl;
    out << "  Matrix Operations:              " << std::fixed << std::setprecision(2) << glTimeMtx << " ms" << std::endl;
    out << "  Shader Compile/Switch:          " << std::fixed << std::setprecision(2) << glTimeShader << " ms" << std::endl;
    if (glDrawCalls > 0.5f) {
        out << "Per GL Draw Call: " << std::fixed << std::setprecision(1)
            << (glTimeVboUpload * 1000.0f / glDrawCalls) << " us VBO + "
            << (glTimeGlDraw * 1000.0f / glDrawCalls) << " us draw = "
            << (glTimeDraw * 1000.0f / glDrawCalls) << " us total" << std::endl;
    }
    out << std::endl;

    // Per-unit cost analysis
    float dlMs = FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_DL_PROCESS);
    out << "Per-Unit Cost Analysis:" << std::endl;
    if (dlMs > 0.1f && glTris > 0.0f) {
        out << "  Cost per GL triangle:         " << std::fixed << std::setprecision(2) << (dlMs * 1000.0f / glTris) << " us" << std::endl;
        if (dlCmds > 0) {
            out << "  Cost per DL command:          " << std::fixed << std::setprecision(2) << (dlMs * 1000.0f / dlCmds) << " us" << std::endl;
        }
        if (glDrawCalls > 0) {
            out << "  Cost per GL draw call:        " << std::fixed << std::setprecision(2) << (dlMs * 1000.0f / glDrawCalls) << " us" << std::endl;
        }
        if (glShaderSwitches > 0) {
            out << "  Cost per shader switch:       " << std::fixed << std::setprecision(2) << (dlMs * 1000.0f / glShaderSwitches) << " us" << std::endl;
        }
    }
    out << std::endl;

    // Per-buffer breakdown
    static const char* sBufExportNames[] = { "OPA (opaque)", "XLU (translucent)", "Overlay", "Work", "Debug" };
    out << "--- Per-Buffer Breakdown ---" << std::endl;
    out << "Buffer               Cmds   Tris  Verts   Tex   Mtx  Sync SubDL Comb" << std::endl;
    for (int b = 0; b < PROFILE_DL_BUFFER_COUNT; b++) {
        DLBufferStats bs = FrameProfiler_GetBufferStats(b);
        char line[256];
        snprintf(line, sizeof(line), "%-20s %5d  %5d  %5d  %4d  %4d  %4d  %4d  %4d",
                 sBufExportNames[b], bs.commands, bs.triangles, bs.vertices,
                 bs.texLoads, bs.mtxLoads, bs.pipeSyncs, bs.subcalls, bs.setCombine);
        out << line << std::endl;
    }
    out << std::endl;

    // Unaccounted time
    float accountedMs = GetAccountedTimeMs();
    float unaccountedMs = totalMs - accountedMs;
    float unaccountedPct = (totalMs > 0.01f) ? (unaccountedMs / totalMs * 100.0f) : 0.0f;
    out << "Unaccounted Time: " << std::fixed << std::setprecision(2) << unaccountedMs 
        << " ms (" << std::fixed << std::setprecision(1) << unaccountedPct << "%)" << std::endl;
    out << std::endl;

    // Flush efficiency analysis
    out << "--- Flush Efficiency ---" << std::endl;
    float emptyFlushes = glBatchFlushes - glDrawCalls;
    float stateActualFlushes = glBatchFlushes - glBufferFullFlushes;
    float emptyPct = (glBatchFlushes > 0.5f) ? (emptyFlushes / glBatchFlushes * 100.0f) : 0.0f;
    float effectiveBatch = (glDrawCalls > 0.5f) ? (glTris / glDrawCalls) : 0.0f;
    out << "Total Flushes:                  " << std::fixed << std::setprecision(0) << glBatchFlushes << std::endl;
    out << "  State-change driven:          " << std::fixed << std::setprecision(0) << stateActualFlushes << std::endl;
    out << "  Buffer-full:                  " << std::fixed << std::setprecision(0) << glBufferFullFlushes << std::endl;
    out << "  Empty (no geometry):          " << std::fixed << std::setprecision(0) << emptyFlushes 
        << " (" << std::fixed << std::setprecision(1) << emptyPct << "%)" << std::endl;
    out << "State change events:            " << std::fixed << std::setprecision(0) << glStateFlushes 
        << " (includes " << std::setprecision(0) << (glStateFlushes - stateActualFlushes) << " with empty buffer)" << std::endl;
    out << "Actual Draw Calls:              " << std::fixed << std::setprecision(0) << glDrawCalls << std::endl;
    out << "Effective Batch Size:           " << std::fixed << std::setprecision(1) << effectiveBatch << " tris/draw" << std::endl;
#ifdef __SWITCH__
    out << "MAX_TRI_BUFFER:                 1024" << std::endl;
    out << "Buffer Utilization:             " << std::fixed << std::setprecision(1) << (effectiveBatch / 1024.0f * 100.0f) << "% (how full each batch is on average)" << std::endl;
#else
    out << "MAX_TRI_BUFFER:                 256" << std::endl;
    out << "Buffer Utilization:             " << std::fixed << std::setprecision(1) << (effectiveBatch / 256.0f * 100.0f) << "% (how full each batch is on average)" << std::endl;
#endif
    if (glDrawCalls > 0.5f) {
        out << "State changes per draw:         " << std::fixed << std::setprecision(2) << (stateActualFlushes / glDrawCalls) << std::endl;
    }
    out << std::endl;

    // Flush cause breakdown
    float flushTexture = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_FLUSH_CAUSE_TEXTURE);
    float flushSampler = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_FLUSH_CAUSE_SAMPLER);
    float flushShader = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_FLUSH_CAUSE_SHADER);
    float flushAlpha = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_FLUSH_CAUSE_ALPHA);
    float flushDepthVp = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_FLUSH_CAUSE_DEPTH_VIEWPORT);
    float flushCombiner = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_FLUSH_CAUSE_COMBINER);
    float texReloadSkips = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TEXTURE_RELOAD_SKIPS);
    out << "--- Flush Cause Breakdown ---" << std::endl;
    out << "Shader switch:                  " << std::fixed << std::setprecision(0) << flushShader << std::endl;
    out << "Texture change:                 " << std::fixed << std::setprecision(0) << flushTexture << std::endl;
    out << "Sampler params:                 " << std::fixed << std::setprecision(0) << flushSampler << std::endl;
    out << "Alpha blend:                    " << std::fixed << std::setprecision(0) << flushAlpha << std::endl;
    out << "Depth/VP/Scissor:               " << std::fixed << std::setprecision(0) << flushDepthVp << std::endl;
    out << "New combiner:                   " << std::fixed << std::setprecision(0) << flushCombiner << std::endl;
    out << "Tex reload skips (saved):       " << std::fixed << std::setprecision(0) << texReloadSkips << std::endl;
    out << std::endl;

    // Batch size distribution histogram
    out << "--- Batch Size Distribution ---" << std::endl;
    static const char* bucketLabels[] = {"1-2 tris", "3-8 tris", "9-32 tris", "33-128 tris", "129+ tris"};
    float batchHist[5];
    float totalDrawsHist = 0.0f;
    for (int b = 0; b < 5; b++) {
        batchHist[b] = FrameProfiler_GetCounterAvg((ProfileCounter)(PROFILE_COUNTER_GL_BATCH_HIST_0 + b));
        totalDrawsHist += batchHist[b];
    }
    float maxBatchSeen = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_MAX_BATCH_SIZE);
    for (int b = 0; b < 5; b++) {
        float pct = (totalDrawsHist > 0.5f) ? (batchHist[b] / totalDrawsHist * 100.0f) : 0.0f;
        // Visual bar: each # = 2%
        int barLen = (int)(pct / 2.0f + 0.5f);
        if (barLen > 40) barLen = 40;
        char bar[42];
        for (int i = 0; i < barLen; i++) bar[i] = '#';
        bar[barLen] = '\0';
        out << "  " << std::setw(12) << std::left << bucketLabels[b] << " " 
            << std::setw(5) << std::right << std::fixed << std::setprecision(0) << batchHist[b]
            << " draws (" << std::setw(4) << std::setprecision(1) << pct << "%) " << bar << std::endl;
    }
    out << "Max batch size seen:            " << std::fixed << std::setprecision(0) << maxBatchSeen << " tris" << std::endl;
    if (totalDrawsHist > 0.5f && batchHist[0] / totalDrawsHist > 0.5f) {
        out << "  WARNING: >50% of draws have only 1-2 triangles. State changes are fragmenting batches severely." << std::endl;
    }
    out << std::endl;

    // Memory bandwidth estimation
    out << "--- Memory Bandwidth Estimate ---" << std::endl;
    // Each vertex has floatsPerVert floats (varies 18-22). Use average of 20.
    float estimatedFloatsPerVert = 20.0f;
    float vboBytes = glVerts * estimatedFloatsPerVert * 4.0f; // 4 bytes per float
    float vboKB = vboBytes / 1024.0f;
    float vboMB = vboKB / 1024.0f;
    out << "VBO data per frame:             " << std::fixed << std::setprecision(0) << vboKB << " KB (" << std::setprecision(2) << vboMB << " MB)" << std::endl;
    if (dlIter > 0.5f) {
        out << "VBO data per DL iteration:      " << std::fixed << std::setprecision(0) << (vboKB / dlIter) << " KB" << std::endl;
    }
    if (renderFrameMs > 0.01f) {
        float mbPerSec = vboMB * fps;
        out << "VBO throughput:                 " << std::fixed << std::setprecision(1) << mbPerSec << " MB/s (upload to GPU)" << std::endl;
    }
    out << std::endl;

    // Frame stability analysis from ring buffer
    out << "--- Frame Stability (over " << PROFILE_RING_SIZE << " frames) ---" << std::endl;
    {
        float minFrame = 1e9f, maxFrame = 0.0f;
        float sum = 0.0f, sumSq = 0.0f;
        int validFrames = 0;
        for (int f = 0; f < PROFILE_RING_SIZE; f++) {
            float val = sPhaseRing[PROFILE_PHASE_TOTAL_FRAME][f];
            if (val > 0.01f) {
                if (val < minFrame) minFrame = val;
                if (val > maxFrame) maxFrame = val;
                sum += val;
                sumSq += val * val;
                validFrames++;
            }
        }
        if (validFrames > 1) {
            float mean = sum / validFrames;
            float variance = (sumSq / validFrames) - (mean * mean);
            float stddev = (variance > 0.0f) ? sqrtf(variance) : 0.0f;
            float cov = (mean > 0.01f ? stddev / mean * 100.0f : 0.0f);
            out << "Frames sampled:                 " << validFrames << std::endl;
            out << "Min:                            " << std::fixed << std::setprecision(2) << minFrame << " ms" << std::endl;
            out << "Max:                            " << std::fixed << std::setprecision(2) << maxFrame << " ms" << std::endl;
            out << "Range:                          " << std::fixed << std::setprecision(2) << (maxFrame - minFrame) << " ms" << std::endl;
            out << "Mean:                           " << std::fixed << std::setprecision(2) << mean << " ms" << std::endl;
            out << "Standard Deviation:             " << std::fixed << std::setprecision(2) << stddev << " ms" << std::endl;
            out << "Coefficient of Variation (CoV): " << std::fixed << std::setprecision(1) << cov << "% (lower = more stable)" << std::endl;
            // Count frames with spikes (>2 stddev from mean)
            int spikes = 0;
            float spikeThreshold = mean + 2.0f * stddev;
            for (int f = 0; f < PROFILE_RING_SIZE; f++) {
                float val = sPhaseRing[PROFILE_PHASE_TOTAL_FRAME][f];
                if (val > spikeThreshold) spikes++;
            }
            if (spikes > 0) {
                out << "Frame Spikes (>2σ):             " << spikes << "/" << validFrames 
                    << " frames above " << std::fixed << std::setprecision(2) << spikeThreshold << " ms" << std::endl;
            }
            // DL Process stability
            float dlMin = 1e9f, dlMax = 0.0f, dlSum = 0.0f;
            for (int f = 0; f < PROFILE_RING_SIZE; f++) {
                float val = sPhaseRing[PROFILE_PHASE_DL_PROCESS][f];
                if (val > 0.01f) {
                    if (val < dlMin) dlMin = val;
                    if (val > dlMax) dlMax = val;
                    dlSum += val;
                }
            }
            if (validFrames > 0) {
                out << std::endl;
                out << "DL Process Stability:" << std::endl;
                out << "  Min:                          " << std::fixed << std::setprecision(2) << dlMin << " ms" << std::endl;
                out << "  Max:                          " << std::fixed << std::setprecision(2) << dlMax << " ms" << std::endl;
                out << "  Range:                        " << std::fixed << std::setprecision(2) << (dlMax - dlMin) << " ms" << std::endl;
            }
        }
    }
    out << std::endl;

    float core0Ms =
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_COLLISION_AT) +
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_COLLISION_DAMAGE) +
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_ACTOR_UPDATE) +
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_ACTOR_DRAW) + FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_SCENE_DRAW) +
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_FRAME_INTERP) +
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_DL_PROCESS) + FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_AUDIO_WAIT);
    float core1Ms = FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_COLLISION_OC);
    float imbalance = (core0Ms > 0.01f) ? (core1Ms / core0Ms) : 0.0f;

    out << "--- Core Utilization (Multi-threading Status) ---" << std::endl;
    out << "Core 0 Active Time:             " << std::fixed << std::setprecision(2) << core0Ms << " ms (main thread)" << std::endl;
    out << "Worker Core Active Time:        " << std::fixed << std::setprecision(2) << core1Ms << " ms (Switch: usually Core 3)" << std::endl;
    out << "Worker/Core 0 Ratio:            " << std::fixed << std::setprecision(1) << (imbalance * 100.0f) << "% (100% = balanced)" << std::endl;
    if (imbalance < 0.5f) {
        out << "Note: Worker core is significantly underutilized. Consider moving more work to worker threads." << std::endl;
    }
    out << std::endl;

    // Automated analysis with ranked sub-phases and specific recommendations
    out << "--- Analysis ---" << std::endl;
    if (worstPhase >= 0 && totalMs > 0.01f) {
        float worstPct = worstMs / totalMs * 100.0f;
        out << "Bottleneck: " << sPhaseNames[worstPhase] << " (" << worstMs << " ms, " << worstPct << "% of frame)"
            << std::endl;
        if (worstPhase == PROFILE_PHASE_DL_PROCESS) {
            out << "The display list interpreter (libultraship Fast3D) dominates frame time." << std::endl;
            out << "This is CPU-bound N64 DL-to-GL translation running single-threaded on Core 0." << std::endl;
            out << std::endl;

            // Ranked GL sub-phases (top 3)
            struct GlPhaseInfo { const char* name; float ms; const char* hint; };
            GlPhaseInfo glPhases[] = {
                {"tri (per-vertex work)", glTimeTriExcl, "Pre-compute combiner inputs, NEON vectorize inner loop, reduce per-vertex work"},
                {"draw (GL submit)", glTimeDraw, "Reduce draw calls by batching, minimize state-change flushes, lazy state"},
                {"  VBO upload", glTimeVboUpload, "Reduce vertex count, use indexed geometry, or batch multiple draws into one VBO upload"},
                {"  glDrawArrays", glTimeGlDraw, "Reduce draw call count via state sorting, or merge compatible draws"},
                {"vtx (vertex transform)", glTimeVtx, "NEON-optimize matrix*vertex, reduce lighting calculations"},
                {"tex (texture setup)", glTimeTex, "NEON texture conversion, increase texture cache hit rate"},
                {"mtx (matrix ops)", glTimeMtx, "NEON 4x4 matrix multiply, reduce matrix stack depth"},
                {"dispatch (cmd walk)", glTimeDispatch, "Reduce GBI command count, optimize opcode dispatch table"},
                {"depth (pixel readback)", glTimeDepth, "Quantize coordinates, cache readback results"},
                {"setup (frame init)", glTimeSetup, "Minimize per-frame initialization overhead"},
                {"shader (compile/switch)", glTimeShader, "Pre-compile shaders, cache shader programs"}
            };
            // Sort by time (simple selection sort, small array)
            int numPhases = sizeof(glPhases) / sizeof(glPhases[0]);
            for (int i = 0; i < numPhases - 1; i++) {
                for (int j = i + 1; j < numPhases; j++) {
                    if (glPhases[j].ms > glPhases[i].ms) {
                        GlPhaseInfo tmp = glPhases[i];
                        glPhases[i] = glPhases[j];
                        glPhases[j] = tmp;
                    }
                }
            }
            out << std::endl;
            out << "GL Sub-Phase Ranking (by time):" << std::endl;
            for (int i = 0; i < 3 && i < numPhases; i++) {
                if (glPhases[i].ms < 0.01f) break;
                float pct = (glTimeTotal > 0.01f) ? (glPhases[i].ms / glTimeTotal * 100.0f) : 0.0f;
                out << "  #" << (i + 1) << " " << glPhases[i].name << ": " << glPhases[i].ms << " ms (" << pct << "% of DL)" << std::endl;
                out << "     → " << glPhases[i].hint << std::endl;
            }

            // Specific recommendations based on data thresholds
            out << std::endl;
            out << "Specific Recommendations:" << std::endl;
            if (emptyPct > 30.0f) {
                out << "  ⚠ " << emptyPct << "% of flushes are empty (no geometry). Consider lazy state application" << std::endl;
                out << "    to defer GL state changes until geometry is actually submitted." << std::endl;
            }
            if (effectiveBatch < 20.0f && glDrawCalls > 100.0f) {
                out << "  ⚠ Low batch size (" << effectiveBatch << " tris/draw). State changes fragment batches." << std::endl;
                out << "    " << stateActualFlushes << " state-driven flushes across " << glDrawCalls << " draws = " << (stateActualFlushes / glDrawCalls) << " state changes/draw." << std::endl;
            }
            if (imbalance < 0.1f && core0Ms > 30.0f) {
                out << "  ⚠ Core imbalance: Core 0 = " << core0Ms << " ms, worker core = " << core1Ms << " ms (" << (imbalance * 100.0f) << "%)." << std::endl;
                out << "    Move DL processing or frame interp to a worker thread for ~2x throughput." << std::endl;
            }
            if (glTimeDraw > 10.0f && glDrawCalls > 500.0f) {
                float costPerDraw = glTimeDraw * 1000.0f / glDrawCalls;
                out << "  ⚠ High GL draw overhead: " << glDrawCalls << " calls × " << costPerDraw << " us/call = " << glTimeDraw << " ms." << std::endl;
                out << "    Reducing draw calls by 50% would save ~" << (glTimeDraw * 0.5f) << " ms/frame." << std::endl;
            }
            if (totalDrawsHist > 0.5f && batchHist[0] / totalDrawsHist > 0.5f) {
                float tinyPct = batchHist[0] / totalDrawsHist * 100.0f;
                out << "  ⚠ " << tinyPct << "% of draws have only 1-2 triangles. Each tiny draw has the same" << std::endl;
                out << "    GL overhead as a 100-triangle draw. Merging adjacent same-state triangles" << std::endl;
                out << "    into single draws would massively reduce draw call count." << std::endl;
            }
        } else if (worstPhase == PROFILE_PHASE_ACTOR_UPDATE) {
            out << "Actor updates dominate. Parallelize BgCheck queries across worker threads." << std::endl;
        } else if (worstPhase == PROFILE_PHASE_ACTOR_DRAW) {
            out << "Actor draw (skeleton/matrix) dominates. Pre-compute matrices on worker threads." << std::endl;
        } else if (worstPhase == PROFILE_PHASE_FRAME_INTERP) {
            out << "Frame interpolation dominates. Consider parallel segment processing." << std::endl;
        }
    }
    out << std::endl;

    // Comparison with previous snapshot (if available)
    if (sPrevSnapshot.valid) {
        out << std::endl;
        out << "--- Comparison with Previous Snapshot (" << sPrevSnapshot.commitShort << ") ---" << std::endl;
        auto delta = [&](const char* label, float current, float previous, bool lowerIsBetter) {
            float diff = current - previous;
            float pctChange = (previous > 0.01f) ? (diff / previous * 100.0f) : 0.0f;
            const char* arrow = (diff > 0.01f) ? "▲" : (diff < -0.01f) ? "▼" : "=";
            const char* verdict = "";
            if (std::fabs(pctChange) > 1.0f) {
                if ((diff < 0 && lowerIsBetter) || (diff > 0 && !lowerIsBetter))
                    verdict = " [IMPROVED]";
                else
                    verdict = " [REGRESSED]";
            }
            char line[256];
            snprintf(line, sizeof(line), "  %-28s %8.2f → %8.2f  (%s%+.1f%%)%s",
                     label, previous, current, arrow, pctChange, verdict);
            out << line << std::endl;
        };
        delta("Rendered frame (ms)",   renderFrameMs, sPrevSnapshot.renderFrameMs, true);
        delta("FPS",                    fps, sPrevSnapshot.fps, false);
        float dlMs2 = FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_DL_PROCESS);
        delta("DL Process (ms)",        dlMs2, sPrevSnapshot.dlProcessMs, true);
        delta("GL Tri Processing (ms)", glTimeTri, sPrevSnapshot.glTimeTri, true);
        delta("GL Draw Submit (ms)",    glTimeDraw, sPrevSnapshot.glTimeDraw, true);
        delta("GL Vertex Load (ms)",    glTimeVtx, sPrevSnapshot.glTimeVtx, true);
        delta("GL Texture Setup (ms)",  glTimeTex, sPrevSnapshot.glTimeTex, true);
        delta("GL Draw Calls",          glDrawCalls, sPrevSnapshot.glDrawCalls, true);
        delta("GL Avg Batch Size",      glAvgBatch, sPrevSnapshot.glAvgBatch, false);
        delta("GL Triangles",           glTris, sPrevSnapshot.glTris, false); // more is neutral
        delta("Empty flush %",          emptyPct, sPrevSnapshot.emptyFlushPct, true);
        out << std::endl;
    }

    // Raw ring buffer data for detailed analysis
    int ringIdx = sRingIndex.load(std::memory_order_relaxed);
    out << "--- Raw Ring Buffer (last " << PROFILE_RING_SIZE << " frames, ms) ---" << std::endl;
    out << "Current index: " << ringIdx << std::endl;
    for (int phase = 0; phase < PROFILE_PHASE_MAX; phase++) {
        out << sPhaseNames[phase] << ":";
        for (int f = 0; f < PROFILE_RING_SIZE; f++) {
            // Read oldest-to-newest
            int idx = (ringIdx + 1 + f) % PROFILE_RING_SIZE;
            out << " " << sPhaseRing[phase][idx];
        }
        out << std::endl;
    }

    // Raw counter ring buffer for key counters (per-frame data for offline analysis)
    out << std::endl;
    out << "--- Raw Counter Ring Buffer (per-frame) ---" << std::endl;
    static const ProfileCounter countersToDump[] = {
        PROFILE_COUNTER_GL_DRAW_CALLS, PROFILE_COUNTER_GL_BATCH_FLUSHES,
        PROFILE_COUNTER_GL_TRIANGLES_SUBMITTED, PROFILE_COUNTER_GL_AVG_BATCH_SIZE,
        PROFILE_COUNTER_GL_FLUSH_CAUSE_TEXTURE, PROFILE_COUNTER_GL_FLUSH_CAUSE_SHADER,
        PROFILE_COUNTER_GL_TEXTURE_RELOAD_SKIPS, PROFILE_COUNTER_GL_MAX_BATCH_SIZE,
    };
    for (auto c : countersToDump) {
        out << sCounterNames[c] << ":";
        for (int f = 0; f < PROFILE_RING_SIZE; f++) {
            int idx = (ringIdx + 1 + f) % PROFILE_RING_SIZE;
            out << " " << sCounterRing[c][idx];
        }
        out << std::endl;
    }

    out.close();

    // Save current values as baseline for next comparison
    sPrevSnapshot.valid = true;
    sPrevSnapshot.commitShort = commitShort;
    sPrevSnapshot.renderFrameMs = renderFrameMs;
    sPrevSnapshot.fps = fps;
    sPrevSnapshot.dlProcessMs = FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_DL_PROCESS);
    sPrevSnapshot.glTimeTri = glTimeTri;
    sPrevSnapshot.glTimeDraw = glTimeDraw;
    sPrevSnapshot.glTimeVtx = glTimeVtx;
    sPrevSnapshot.glTimeTex = glTimeTex;
    sPrevSnapshot.glDrawCalls = glDrawCalls;
    sPrevSnapshot.glAvgBatch = glAvgBatch;
    sPrevSnapshot.glTris = glTris;
    sPrevSnapshot.emptyFlushPct = emptyPct;
    FrameProfiler_SaveBaseline(sPrevSnapshot);

    sLastExportPath = filepath;
    sExportMsgTimer = 5.0f;
}

// ── ImGui Window ───────────────────────────────────────────────────────

void FrameProfilerWindow::InitElement() {
    FrameProfiler_LoadBaseline();
}

void FrameProfilerWindow::UpdateElement() {
}

// Helper: show a delta indicator next to a metric value
static void ShowDelta(float current, float baseline, bool lowerIsBetter) {
    if (!sPrevSnapshot.valid) return;
    float delta = current - baseline;
    if (std::abs(delta) < 0.01f) return; // no meaningful change
    bool improved = lowerIsBetter ? (delta < 0) : (delta > 0);
    ImVec4 color = improved ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
    ImGui::SameLine();
    if (std::abs(delta) >= 1.0f) {
        ImGui::TextColored(color, "(%+.1f)", delta);
    } else {
        ImGui::TextColored(color, "(%+.2f)", delta);
    }
}

void FrameProfilerWindow::DrawElement() {
    // On/off toggle — when OFF, zero overhead (no clock_gettime, no counter collection)
    ImGui::Checkbox("Profiling Active", &sProfilingActive);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When OFF: zero performance impact on game\n"
                          "Window stays open showing last captured data");
    }
    if (!sProfilingActive) {
        // Ensure profiling is disabled — zero overhead path
        sEnabled.store(0, std::memory_order_relaxed);
        sDrawCountdown = 0;
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "(PAUSED - showing last captured data)");
    } else {
        // Enable profiling and reset the keepalive countdown
        sEnabled.store(1, std::memory_order_relaxed);
        sDrawCountdown = PROFILE_KEEPALIVE_FRAMES;
        // Show baseline info on the same line as the checkbox
        if (sPrevSnapshot.valid) {
            ImGui::SameLine();
            ImGui::TextDisabled("Baseline: %s (%.1f FPS)", sPrevSnapshot.commitShort.c_str(), sPrevSnapshot.fps);
        }
    }
    ImGui::Separator();

    float totalMs = FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_TOTAL_FRAME);
    float dlIter = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_ITERATIONS);
    if (dlIter < 1.0f) dlIter = 1.0f;
    float renderFrameMs = totalMs / dlIter;
    float fps = (renderFrameMs > 0.01f) ? (1000.0f / renderFrameMs) : 0.0f;

    // Enhanced summary with color coding and deltas
    if (renderFrameMs > 20.0f) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Frame: %.2f ms  (%.1f FPS)", renderFrameMs, fps);
    } else if (renderFrameMs > PROFILE_TARGET_FRAME_MS) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Frame: %.2f ms  (%.1f FPS)", renderFrameMs, fps);
    } else {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Frame: %.2f ms  (%.1f FPS)", renderFrameMs, fps);
    }
    ShowDelta(renderFrameMs, sPrevSnapshot.renderFrameMs, true);  // lower frame time = better
    ImGui::SameLine();
    ImGui::TextDisabled("Target: %.2f ms (60 FPS)", PROFILE_TARGET_FRAME_MS);
    
    // Show FPS delta
    if (sPrevSnapshot.valid) {
        float fpsDelta = fps - sPrevSnapshot.fps;
        if (std::abs(fpsDelta) >= 0.1f) {
            ImVec4 fpsColor = (fpsDelta > 0) ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
            ImGui::TextColored(fpsColor, "FPS vs baseline: %+.1f FPS (%s)", fpsDelta, fpsDelta > 0 ? "IMPROVED" : "REGRESSED");
        }
    }

    // Show performance headroom or overhead
    if (renderFrameMs > PROFILE_TARGET_FRAME_MS) {
        float overhead = ((renderFrameMs / PROFILE_TARGET_FRAME_MS) - 1.0f) * 100.0f;
        ImGui::Text("Performance: %.1f%% over budget", overhead);
    } else {
        float headroom = ((PROFILE_TARGET_FRAME_MS - renderFrameMs) / PROFILE_TARGET_FRAME_MS) * 100.0f;
        ImGui::Text("Performance: %.1f%% headroom", headroom);
    }
    ImGui::Separator();

    // Bar chart with percentages
    float maxMs = totalMs > PROFILE_TARGET_FRAME_MS ? totalMs : PROFILE_TARGET_FRAME_MS;
    int worstPhase = -1;
    float worstMs = 0.0f;
    for (int i = 0; i < PROFILE_PHASE_MAX; i++) {
        if (i == PROFILE_PHASE_TOTAL_FRAME)
            continue;

        float ms = FrameProfiler_GetPhaseAvgMs((ProfilePhase)i);
        float frac = ms / maxMs;
        if (frac > 1.0f)
            frac = 1.0f;
        float pct = (totalMs > 0.01f) ? (ms / totalMs * 100.0f) : 0.0f;

        ImGui::Text("%-7s %-18s %5.1f ms (%4.1f%%)", sPhaseCoreLabels[i], sPhaseNames[i], ms, pct);
        // Show delta for DL Process phase (the main bottleneck)
        if (i == PROFILE_PHASE_DL_PROCESS && sPrevSnapshot.valid) {
            ShowDelta(ms, sPrevSnapshot.dlProcessMs, true);
        }
        ImGui::SameLine();
        ImGui::ProgressBar(frac, ImVec2(200, 0), "");

        // Track worst leaf phase
        if (IsLeafPhase(i)) {
            if (ms > worstMs) {
                worstMs = ms;
                worstPhase = i;
            }
        }
    }

    ImGui::Separator();

    // DL Rendering Statistics
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "--- Display List Stats (per frame avg) ---");

    float dlCmds = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_COMMANDS);
    float dlReplayIters = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_REPLAY_ITERATIONS);
    float dlReplayFallbacks = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_REPLAY_FALLBACKS);
    float dlReplayBranchZFrames = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_REPLAY_BRANCHZ_FRAMES);
    float dlReplayCooldownSkips = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_REPLAY_COOLDOWN_SKIPS);
    float tris = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_TRIANGLES);
    float verts = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_VERTICES);
    float texLoads = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_TEX_LOADS);
    float mtxLoads = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_MTX_LOADS);
    float pipeSyncs = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_PIPE_SYNCS);
    float subcalls = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_SUBCALLS);
    float setCombine = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_SETCOMBINE);
    float glDrawCalls = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_DRAW_CALLS);
    float glBatchFlushes = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_BATCH_FLUSHES);
    float glBufferFullFlushes = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_BUFFER_FULL_FLUSHES);
    float glStateFlushes = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_STATE_FLUSHES);
    float glShaderSwitches = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_SHADER_SWITCHES);
    float glShaderCompiles = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_SHADER_COMPILATIONS);
    float glTextureBinds = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TEXTURE_BINDS);
    float glTextureMisses = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TEXTURE_CACHE_MISSES);
    float glVerts = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_VERTICES_SUBMITTED);
    float glTris = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TRIANGLES_SUBMITTED);
    float glTimeTotal = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_TOTAL_MS);
    float glTimeDispatch = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_DISPATCH_MS);
    float glTimeTri = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_TRI_MS);
    float glTimeTex = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_TEX_MS);
    float glTimeShader = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_SHADER_MS);
    float glTimeDraw = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_DRAW_MS);
    float glTimeVboUpload = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_VBO_UPLOAD_MS);
    float glTimeGlDraw = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_GL_DRAW_MS);
    float glTimeVtx = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_VTX_MS);
    float glTimeMtx = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_MTX_MS);
    float glTimeDepth = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_DEPTH_MS);
    float glTimeSetup = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_SETUP_MS);
    float glDepthQueries = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_PIXEL_DEPTH_QUERIES);
    float glAvgBatch = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_AVG_BATCH_SIZE);

    ImGui::Text("DL Iterations: %.0f   Total Commands: %.0f", dlIter, dlCmds);
    ImGui::Text("Replay Iters: %.0f   Fallbacks: %.0f   BranchZ: %.0f   Cooldown Skips: %.0f",
                dlReplayIters, dlReplayFallbacks, dlReplayBranchZFrames, dlReplayCooldownSkips);
    ImGui::Text("Triangles: %.0f   Vertices: %.0f", tris, verts);
    
    // Add derived metric with tooltip
    float vertsPerTri = (tris > 0.5f) ? (verts / tris) : 0.0f;
    ImGui::Text("Verts/Tri: %.2f", vertsPerTri);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Vertices per triangle\n"
                          "~3.0 = indexed triangle lists (typical)\n"
                          "~1.0 = triangle strips/fans (highly efficient)\n"
                          ">3.0 = vertex duplication or non-indexed geometry");
    }
    
    ImGui::Text("Tex Loads: %.0f   Matrix Loads: %.0f   SetCombine: %.0f", texLoads, mtxLoads, setCombine);
    ImGui::Text("Pipe Syncs: %.0f   DL Subcalls: %.0f", pipeSyncs, subcalls);

    ImGui::Separator();
    
    // Estimated draw calls (each PipeSync potentially flushes a draw call)
    float estDrawCalls = pipeSyncs;
    ImGui::Text("Est. Draw Calls: ~%.0f", estDrawCalls);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Upper bound estimate based on PipeSync commands\nActual draw calls may be lower due to batching");
    }
    
    float stateActualFlushesGui = glBatchFlushes - glBufferFullFlushes;
    ImGui::Text("GL Draw Calls: %.0f", glDrawCalls);
    ShowDelta(glDrawCalls, sPrevSnapshot.glDrawCalls, true);  // fewer draws = better
    ImGui::SameLine();
    ImGui::Text("  Flushes: %.0f (state: %.0f, buf-full: %.0f)", glBatchFlushes, stateActualFlushesGui, glBufferFullFlushes);
    
    // Flush efficiency warning
    float emptyFlushes = glBatchFlushes - glDrawCalls;
    float emptyPct = (glBatchFlushes > 0.5f) ? (emptyFlushes / glBatchFlushes * 100.0f) : 0.0f;
    if (emptyPct > 30.0f) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "Warning: High empty flushes");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%.0f%% of flushes are empty (no geometry)\nConsider lazy state application to reduce overhead", emptyPct);
        }
    }
    
    ImGui::Text("GL Shader Switches: %.0f (compiles: %.0f)", glShaderSwitches, glShaderCompiles);
    
    // Add efficiency metric
    float drawsPerShader = (glShaderSwitches > 0.5f) ? (glDrawCalls / glShaderSwitches) : 0.0f;
    ImGui::SameLine();
    ImGui::TextDisabled("(%.1f draws/shader)", drawsPerShader);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Draw calls per shader switch\nHigher is better (indicates better batching)");
    }
    
    ImGui::Text("GL Texture Binds: %.0f (cache misses: %.0f)", glTextureBinds, glTextureMisses);
    
    // Cache miss rate
    float cacheMissRate = (glTextureBinds > 0.5f) ? (glTextureMisses / glTextureBinds * 100.0f) : 0.0f;
    ImGui::SameLine();
    ImGui::TextDisabled("(%.1f%% miss rate)", cacheMissRate);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Texture cache miss rate\nLower is better (indicates good texture locality)");
    }
    
    ImGui::Text("GL Submitted: %.0f tris, %.0f verts  Avg batch: %.1f tris/draw", glTris, glVerts, glAvgBatch);
    ShowDelta(glAvgBatch, sPrevSnapshot.glAvgBatch, false);  // bigger batches = better
    if (glDepthQueries > 0.0f) {
        ImGui::Text("Pixel Depth Queries: %.0f", glDepthQueries);
    }

    // Flush cause breakdown (collapsible)
    float flushTexture = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_FLUSH_CAUSE_TEXTURE);
    float flushSampler = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_FLUSH_CAUSE_SAMPLER);
    float flushShader = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_FLUSH_CAUSE_SHADER);
    float flushAlpha = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_FLUSH_CAUSE_ALPHA);
    float flushDepthVp = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_FLUSH_CAUSE_DEPTH_VIEWPORT);
    float flushCombiner = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_FLUSH_CAUSE_COMBINER);
    float texReloadSkips = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TEXTURE_RELOAD_SKIPS);
    if (ImGui::TreeNode("Flush Cause Breakdown")) {
        float totalFlushCauses = flushTexture + flushSampler + flushShader + flushAlpha + flushDepthVp + flushCombiner;
        auto flushBar = [&](const char* label, float count, ImVec4 color) {
            if (count < 0.5f) return;
            float pct = (totalFlushCauses > 0.5f) ? (count / totalFlushCauses * 100.0f) : 0.0f;
            ImGui::TextColored(color, "  %-18s %5.0f (%4.1f%%)", label, count, pct);
            ImGui::SameLine();
            ImGui::ProgressBar(count / (totalFlushCauses > 0.5f ? totalFlushCauses : 1.0f), ImVec2(120, 0), "");
        };
        flushBar("Shader switch", flushShader, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        flushBar("Texture change", flushTexture, ImVec4(1.0f, 0.7f, 0.3f, 1.0f));
        flushBar("Sampler params", flushSampler, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        flushBar("Alpha blend", flushAlpha, ImVec4(0.4f, 1.0f, 0.6f, 1.0f));
        flushBar("Depth/VP/Scissor", flushDepthVp, ImVec4(0.8f, 0.6f, 1.0f, 1.0f));
        flushBar("New combiner", flushCombiner, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        if (texReloadSkips > 0.5f) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "  Tex reload skips:  %.0f (saved flushes)", texReloadSkips);
        }
        ImGui::TreePop();
    }

    // Batch size distribution (collapsible)
    if (ImGui::TreeNode("Batch Size Distribution")) {
        static const char* bucketLabels[] = {"1-2 tris", "3-8 tris", "9-32 tris", "33-128 tris", "129+ tris"};
        static const ImVec4 bucketColors[] = {
            ImVec4(1.0f, 0.3f, 0.3f, 1.0f),  // red = tiny
            ImVec4(1.0f, 0.6f, 0.3f, 1.0f),  // orange
            ImVec4(1.0f, 1.0f, 0.3f, 1.0f),  // yellow
            ImVec4(0.5f, 1.0f, 0.3f, 1.0f),  // green
            ImVec4(0.3f, 1.0f, 0.3f, 1.0f),  // bright green = large batches
        };
        float batchHist[5];
        float totalDrawsHist = 0.0f;
        for (int b = 0; b < 5; b++) {
            batchHist[b] = FrameProfiler_GetCounterAvg((ProfileCounter)(PROFILE_COUNTER_GL_BATCH_HIST_0 + b));
            totalDrawsHist += batchHist[b];
        }
        float maxBatchSeen = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_MAX_BATCH_SIZE);

        for (int b = 0; b < 5; b++) {
            if (batchHist[b] < 0.5f) continue;
            float pct = (totalDrawsHist > 0.5f) ? (batchHist[b] / totalDrawsHist * 100.0f) : 0.0f;
            ImGui::TextColored(bucketColors[b], "  %-12s %5.0f draws (%4.1f%%)", bucketLabels[b], batchHist[b], pct);
            ImGui::SameLine();
            ImGui::ProgressBar(batchHist[b] / (totalDrawsHist > 0.5f ? totalDrawsHist : 1.0f), ImVec2(120, 0), "");
        }
        ImGui::Text("  Max batch seen: %.0f tris", maxBatchSeen);
        if (totalDrawsHist > 0.5f && batchHist[0] / totalDrawsHist > 0.5f) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "  Warning: >50%% of draws have only 1-2 triangles");
        }
        ImGui::TreePop();
    }

    // Detailed timing breakdown with visual bar chart
    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "--- Fast3D Timing Breakdown (%.2f ms total) ---", glTimeTotal);
    float timingBarMax = glTimeTotal > 0.01f ? glTimeTotal : 1.0f;

    float glTimeTriExcl = glTimeTri > glTimeDraw ? glTimeTri - glTimeDraw : 0.0f;
    float glTimeDrawOther = glTimeDraw > (glTimeVboUpload + glTimeGlDraw) ? glTimeDraw - glTimeVboUpload - glTimeGlDraw : 0.0f;
    struct TimingEntry { const char* name; float ms; ImVec4 color; float baseline; };
    TimingEntry timings[] = {
        { "Triangle Processing", glTimeTri, ImVec4(1.0f, 0.4f, 0.4f, 1.0f), sPrevSnapshot.glTimeTri },
        { "  Per-vertex work",   glTimeTriExcl, ImVec4(1.0f, 0.5f, 0.5f, 1.0f), 0.0f },
        { "  Draw Submit (GL)",  glTimeDraw, ImVec4(1.0f, 0.7f, 0.3f, 1.0f), sPrevSnapshot.glTimeDraw },
        { "    VBO Upload",      glTimeVboUpload, ImVec4(1.0f, 0.8f, 0.4f, 1.0f), 0.0f },
        { "    glDrawArrays",    glTimeGlDraw, ImVec4(1.0f, 0.6f, 0.2f, 1.0f), 0.0f },
        { "    State/Uniforms",  glTimeDrawOther, ImVec4(0.9f, 0.7f, 0.5f, 1.0f), 0.0f },
        { "Vertex Load",         glTimeVtx, ImVec4(0.4f, 0.8f, 1.0f, 1.0f), sPrevSnapshot.glTimeVtx },
        { "Texture Setup",       glTimeTex, ImVec4(0.4f, 1.0f, 0.6f, 1.0f), sPrevSnapshot.glTimeTex },
        { "Matrix Ops",          glTimeMtx, ImVec4(0.8f, 0.6f, 1.0f, 1.0f), 0.0f },
        { "Frame Setup/Teardown",glTimeSetup, ImVec4(0.6f, 0.6f, 0.6f, 1.0f), 0.0f },
        { "Pixel Depth",         glTimeDepth, ImVec4(1.0f, 1.0f, 0.4f, 1.0f), 0.0f },
        { "Shader Setup",        glTimeShader, ImVec4(0.4f, 1.0f, 1.0f, 1.0f), 0.0f },
        { "Dispatch (cmd walk)", glTimeDispatch, ImVec4(0.7f, 0.7f, 0.7f, 1.0f), 0.0f },
    };
    for (const auto& t : timings) {
        if (t.ms < 0.001f) continue; // skip zero entries
        float frac = t.ms / timingBarMax;
        if (frac > 1.0f) frac = 1.0f;
        float pct = glTimeTotal > 0.01f ? (t.ms / glTimeTotal * 100.0f) : 0.0f;
        ImGui::TextColored(t.color, "  %-22s %6.2f ms (%4.1f%%)", t.name, t.ms, pct);
        if (sPrevSnapshot.valid && t.baseline > 0.01f) {
            ShowDelta(t.ms, t.baseline, true);  // lower time = better
        }
        ImGui::SameLine();
        ImGui::ProgressBar(frac, ImVec2(150, 0), "");
    }

    // Cost-per-unit estimates
    float dlMs = FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_DL_PROCESS);
    if (dlMs > 0.1f && tris > 0.0f) {
        float usPerTri = (dlMs * 1000.0f) / tris;
        ImGui::Text("Cost: %.1f us/tri", usPerTri);
        if (dlCmds > 0.0f) {
            ImGui::SameLine();
            ImGui::Text("  %.1f us/cmd", (dlMs * 1000.0f) / dlCmds);
        }
        if (estDrawCalls > 0.0f) {
            ImGui::SameLine();
            ImGui::Text("  %.0f us/draw", (dlMs * 1000.0f) / estDrawCalls);
        }
    }

    // State change rate — flushes per triangle indicates how much state fragmentation exists
    if (glTris > 0.5f) {
        float totalFlushCauses = flushTexture + flushSampler + flushShader + flushAlpha + flushDepthVp + flushCombiner;
        float stateChangesPerTri = totalFlushCauses / glTris;
        ImVec4 scColor = (stateChangesPerTri > 0.3f) ? ImVec4(1.0f, 0.5f, 0.2f, 1.0f) : 
                         (stateChangesPerTri > 0.15f) ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f) :
                         ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
        ImGui::TextColored(scColor, "State changes/tri: %.3f", stateChangesPerTri);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Ratio of state-change flushes to triangles\n"
                              "<0.15 = well batched\n"
                              "0.15-0.30 = moderate fragmentation\n"
                              ">0.30 = severe fragmentation (nearly 1 flush per 3 tris)");
        }
    }

    // CPU vs GL Driver summary
    if (glTimeTotal > 0.01f) {
        ImGui::Separator();
        float glDriverTime = glTimeVboUpload + glTimeGlDraw + glTimeSetup + glTimeDepth;
        float cpuTime = glTimeTotal > glDriverTime ? glTimeTotal - glDriverTime : 0.0f;
        float glPct = glDriverTime / glTimeTotal * 100.0f;
        float cpuPct = cpuTime / glTimeTotal * 100.0f;
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "CPU vs GL: CPU %.1f ms (%.0f%%) | GL %.1f ms (%.0f%%)",
                           cpuTime, cpuPct, glDriverTime, glPct);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("GL Driver = VBO upload + glDrawArrays + frame setup + depth readback\n"
                              "CPU = DL dispatch + triangle processing + vertex transform + texture + matrix + shader\n\n"
                              "If GL-bound: reduce draw calls or VBO data size\n"
                              "If CPU-bound: optimize DL dispatch or per-triangle work");
        }
        if (glDrawCalls > 0.5f) {
            ImGui::Text("  Per draw: %.1f us VBO + %.1f us draw = %.1f us total",
                        glTimeVboUpload * 1000.0f / glDrawCalls,
                        glTimeGlDraw * 1000.0f / glDrawCalls,
                        glTimeDraw * 1000.0f / glDrawCalls);
        }
    }

    // Per-buffer breakdown
    static const char* sBufNames[] = { "OPA (opaque)", "XLU (translucent)", "Overlay", "Work", "Debug" };
    if (ImGui::TreeNode("Per-Buffer Breakdown")) {
        ImGui::Text("%-20s %6s %6s %6s %5s %5s %5s", "Buffer", "Cmds", "Tris", "Verts", "Tex", "Mtx", "Sync");
        for (int b = 0; b < PROFILE_DL_BUFFER_COUNT; b++) {
            DLBufferStats bs = FrameProfiler_GetBufferStats(b);
            if (bs.commands > 0) {
                ImGui::Text("%-20s %6d %6d %6d %5d %5d %5d", sBufNames[b], bs.commands, bs.triangles, bs.vertices,
                            bs.texLoads, bs.mtxLoads, bs.pipeSyncs);
            }
        }
        ImGui::TreePop();
    }

    // Memory bandwidth estimation (collapsible)
    if (ImGui::TreeNode("Memory & Bandwidth")) {
        float estimatedFloatsPerVert = 20.0f;
        float vboBytes = glVerts * estimatedFloatsPerVert * 4.0f;
        float vboKB = vboBytes / 1024.0f;
        float vboMB = vboKB / 1024.0f;
        ImGui::Text("VBO data/frame: %.0f KB (%.2f MB)", vboKB, vboMB);
        if (dlIter > 0.5f) {
            ImGui::Text("VBO data/DL iter: %.0f KB", vboKB / dlIter);
        }
        if (renderFrameMs > 0.01f) {
            float mbPerSec = vboMB * fps;
            ImGui::Text("VBO throughput: %.1f MB/s", mbPerSec);
        }
        ImGui::Text("Avg vertex size: ~%.0f floats (%.0f bytes)", estimatedFloatsPerVert, estimatedFloatsPerVert * 4);
        ImGui::TreePop();
    }

    // Unaccounted time
    float accountedMs = GetAccountedTimeMs();
    float unaccountedMs = totalMs - accountedMs;
    float unaccountedPct = (totalMs > 0.01f) ? (unaccountedMs / totalMs * 100.0f) : 0.0f;
    ImGui::Text("Unaccounted:       %5.1f ms (%4.1f%%)", unaccountedMs, unaccountedPct);

    ImGui::Separator();

    // Core breakdown
    float core0Ms =
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_COLLISION_AT) +
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_COLLISION_DAMAGE) +
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_ACTOR_UPDATE) +
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_ACTOR_DRAW) + FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_SCENE_DRAW) +
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_FRAME_INTERP) +
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_DL_PROCESS) + FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_AUDIO_WAIT);
    float core1Ms = FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_COLLISION_OC);

    ImGui::Text("Core 0 Active: %5.2f ms  |  Worker Core Active: %5.2f ms", core0Ms, core1Ms);
    float imbalance = (core0Ms > 0.01f) ? (core1Ms / core0Ms) : 0.0f;
    
    // Color-code the utilization ratio
    ImVec4 ratioColor = ImVec4(0.5f, 0.8f, 1.0f, 1.0f);  // default blue
    if (imbalance < 0.3f) {
        ratioColor = ImVec4(1.0f, 0.5f, 0.2f, 1.0f);  // orange for severe imbalance
    } else if (imbalance < 0.6f) {
        ratioColor = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);  // yellow for moderate imbalance
    } else if (imbalance > 0.8f && imbalance < 1.2f) {
        ratioColor = ImVec4(0.3f, 1.0f, 0.3f, 1.0f);  // green for well balanced
    }
    
    ImGui::TextColored(ratioColor, "Core utilization ratio: %.1f%% (100%% = balanced)", imbalance * 100.0f);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Ratio of worker-core work to Core 0 work\nIdeal: 80-120%% (well balanced)\n<50%%: Consider moving work to worker threads");
    }

    ImGui::Separator();

    // Automated analysis
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "--- Analysis ---");
    if (worstPhase >= 0 && totalMs > 0.01f) {
        float worstPct = worstMs / totalMs * 100.0f;
        ImGui::TextWrapped("Bottleneck: %s (%.1f ms, %.0f%% of frame)", sPhaseNames[worstPhase], worstMs, worstPct);

        if (worstPhase == PROFILE_PHASE_DL_PROCESS) {
            ImGui::TextWrapped("The display list interpreter (libultraship Fast3D) dominates frame time. "
                               "This is CPU-bound N64 DL-to-GL translation running single-threaded on Core 0.");
            if (dlMs > 0.1f && tris > 0.0f) {
                float usPerTri = (dlMs * 1000.0f) / tris;
                if (usPerTri > 5.0f) {
                    ImGui::TextWrapped("At %.0f us/tri, per-triangle overhead is high. This suggests draw call "
                                       "overhead or state changes dominate (not raw vertex throughput). "
                                       "Focus: batch draw calls, reduce SetCombine/PipeSync count.",
                                       usPerTri);
                } else {
                    ImGui::TextWrapped("At %.0f us/tri, per-triangle cost is moderate. "
                                       "Reducing triangle count (LOD, draw distance) would help.",
                                       usPerTri);
                }
            }
            ImGui::TextWrapped("Priority: (1) Profile inside Fast3D interpreter (libultraship), "
                               "(2) Render thread offload, (3) Reduce DL complexity.");
        } else if (worstPhase == PROFILE_PHASE_ACTOR_UPDATE) {
            ImGui::TextWrapped("Actor updates are the bottleneck. Consider parallelizing BgCheck queries "
                               "across worker threads (Phase 3 in optimization plan).");
        } else if (worstPhase == PROFILE_PHASE_ACTOR_DRAW) {
            ImGui::TextWrapped("Actor drawing (skeleton/matrix/DL generation) is the bottleneck. "
                               "Consider pre-computing matrices on worker threads (Phase 4 in optimization plan).");
        } else if (worstPhase == PROFILE_PHASE_FRAME_INTERP) {
            ImGui::TextWrapped("Frame interpolation is the bottleneck. Consider parallel segment "
                               "processing across worker threads (Phase 5 in optimization plan).");
        }
    }

    ImGui::Separator();

    // Frame history graph — shows last 60 frames as a line plot
    if (ImGui::TreeNode("Frame History (last 60 frames)")) {
        int ringIdx = sRingIndex.load(std::memory_order_relaxed);
        
        // Total frame time graph
        {
            float frameHistory[PROFILE_RING_SIZE];
            float histMin = 1e9f, histMax = 0.0f;
            for (int f = 0; f < PROFILE_RING_SIZE; f++) {
                int idx = (ringIdx + 1 + f) % PROFILE_RING_SIZE;
                frameHistory[f] = sPhaseRing[PROFILE_PHASE_TOTAL_FRAME][idx];
                if (frameHistory[f] > 0.01f) {
                    if (frameHistory[f] < histMin) histMin = frameHistory[f];
                    if (frameHistory[f] > histMax) histMax = frameHistory[f];
                }
            }
            char overlay[64];
            snprintf(overlay, sizeof(overlay), "Total Frame (%.1f-%.1f ms)", histMin, histMax);
            ImGui::PlotLines("##frame_total", frameHistory, PROFILE_RING_SIZE, 0, overlay,
                             0.0f, histMax * 1.2f, ImVec2(ImGui::GetContentRegionAvail().x, 60));
        }

        // DL Process graph
        {
            float dlHistory[PROFILE_RING_SIZE];
            float histMin = 1e9f, histMax = 0.0f;
            for (int f = 0; f < PROFILE_RING_SIZE; f++) {
                int idx = (ringIdx + 1 + f) % PROFILE_RING_SIZE;
                dlHistory[f] = sPhaseRing[PROFILE_PHASE_DL_PROCESS][idx];
                if (dlHistory[f] > 0.01f) {
                    if (dlHistory[f] < histMin) histMin = dlHistory[f];
                    if (dlHistory[f] > histMax) histMax = dlHistory[f];
                }
            }
            char overlay[64];
            snprintf(overlay, sizeof(overlay), "DL Process (%.1f-%.1f ms)", histMin, histMax);
            ImGui::PlotLines("##frame_dl", dlHistory, PROFILE_RING_SIZE, 0, overlay,
                             0.0f, histMax * 1.2f, ImVec2(ImGui::GetContentRegionAvail().x, 60));
        }

        ImGui::TreePop();
    }

    // Per-phase stability (min/max/jitter)
    if (ImGui::TreeNode("Phase Stability (min/max/jitter)")) {
        for (int phase = 0; phase < PROFILE_PHASE_MAX; phase++) {
            if (phase == PROFILE_PHASE_TOTAL_FRAME) continue;
            float avg = FrameProfiler_GetPhaseAvgMs((ProfilePhase)phase);
            if (avg < 0.01f) continue; // skip phases with no time

            float pMin = 1e9f, pMax = 0.0f;
            for (int f = 0; f < PROFILE_RING_SIZE; f++) {
                float val = sPhaseRing[phase][f];
                if (val > 0.001f) {
                    if (val < pMin) pMin = val;
                    if (val > pMax) pMax = val;
                }
            }
            float jitter = pMax - pMin;
            ImVec4 jitterColor = (jitter > avg * 0.5f) ? ImVec4(1.0f, 0.5f, 0.2f, 1.0f) : ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
            ImGui::TextColored(jitterColor, "%-18s avg %5.2f  min %5.2f  max %5.2f  jitter %5.2f ms",
                               sPhaseNames[phase], avg, pMin, pMax, jitter);
        }
        ImGui::TreePop();
    }

    ImGui::Separator();

    // Export button
    if (ImGui::Button("Export Snapshot")) {
        FrameProfiler_ExportSnapshot();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(saves to app directory)");

    // Show export status message
    if (sExportMsgTimer > 0.0f) {
        sExportMsgTimer -= ImGui::GetIO().DeltaTime;
        ImGui::TextWrapped("%s", sLastExportPath.c_str());
    }
}
