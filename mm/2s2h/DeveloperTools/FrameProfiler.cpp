#include "FrameProfiler.h"

#include <imgui.h>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <fstream>
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
static std::atomic<int> sRingIndex{ 0 };
static std::atomic<int> sEnabled{ 0 };
static int sDrawCountdown = 0; // main-thread only

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
    sPhaseRing[phase][sRingIndex.load(std::memory_order_relaxed)] = (float)elapsed / 1e6f; // ns → ms
}

extern "C" void FrameProfiler_EndFrame(void) {
    if (!sEnabled.load(std::memory_order_relaxed))
        return;
    int next = (sRingIndex.load(std::memory_order_relaxed) + 1) % PROFILE_RING_SIZE;
    // Clear the next slot so phases not measured this frame show 0
    for (int i = 0; i < PROFILE_PHASE_MAX; i++) {
        sPhaseRing[i][next] = 0.0f;
    }
    for (int i = 0; i < PROFILE_COUNTER_MAX; i++) {
        sCounterRing[i][next] = 0.0f;
    }
    for (int b = 0; b < PROFILE_DL_BUFFER_COUNT; b++) {
        for (int f = 0; f < PROFILE_DL_FIELD_COUNT; f++) {
            sBufferStatsRing[b][f][next] = 0.0f;
        }
    }
    sRingIndex.store(next, std::memory_order_relaxed);

    // Auto-disable when window hasn't drawn recently
    if (sDrawCountdown > 0) {
        sDrawCountdown--;
    } else {
        sEnabled.store(0, std::memory_order_relaxed);
    }
}

extern "C" float FrameProfiler_GetPhaseAvgMs(ProfilePhase phase) {
    float sum = 0.0f;
    for (int i = 0; i < PROFILE_RING_SIZE; i++) {
        sum += sPhaseRing[phase][i];
    }
    return sum / PROFILE_RING_SIZE;
}

extern "C" void FrameProfiler_AddCounter(ProfileCounter counter, float value) {
    if (!sEnabled.load(std::memory_order_relaxed))
        return;
    // Note: Counters are assumed to be incremented from the main thread only.
    // If a counter is used from worker threads, it must be made atomic.
    sCounterRing[counter][sRingIndex.load(std::memory_order_relaxed)] += value;
}

extern "C" float FrameProfiler_GetCounterAvg(ProfileCounter counter) {
    float sum = 0.0f;
    for (int i = 0; i < PROFILE_RING_SIZE; i++) {
        sum += sCounterRing[counter][i];
    }
    return sum / PROFILE_RING_SIZE;
}

extern "C" int FrameProfiler_IsEnabled(void) {
    return sEnabled.load(std::memory_order_relaxed);
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
        sBufferStatsRing[b][0][ri] = (float)s.commands;
        sBufferStatsRing[b][1][ri] = (float)s.triangles;
        sBufferStatsRing[b][2][ri] = (float)s.vertices;
        sBufferStatsRing[b][3][ri] = (float)s.texLoads;
        sBufferStatsRing[b][4][ri] = (float)s.mtxLoads;
        sBufferStatsRing[b][5][ri] = (float)s.pipeSyncs;
        sBufferStatsRing[b][6][ri] = (float)s.subcalls;
        sBufferStatsRing[b][7][ri] = (float)s.setCombine;

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

    for (int i = 0; i < PROFILE_RING_SIZE; i++) {
        stats.commands += (int)sBufferStatsRing[bufIdx][0][i];
        stats.triangles += (int)sBufferStatsRing[bufIdx][1][i];
        stats.vertices += (int)sBufferStatsRing[bufIdx][2][i];
        stats.texLoads += (int)sBufferStatsRing[bufIdx][3][i];
        stats.mtxLoads += (int)sBufferStatsRing[bufIdx][4][i];
        stats.pipeSyncs += (int)sBufferStatsRing[bufIdx][5][i];
        stats.subcalls += (int)sBufferStatsRing[bufIdx][6][i];
        stats.setCombine += (int)sBufferStatsRing[bufIdx][7][i];
    }
    // Average
    stats.commands /= PROFILE_RING_SIZE;
    stats.triangles /= PROFILE_RING_SIZE;
    stats.vertices /= PROFILE_RING_SIZE;
    stats.texLoads /= PROFILE_RING_SIZE;
    stats.mtxLoads /= PROFILE_RING_SIZE;
    stats.pipeSyncs /= PROFILE_RING_SIZE;
    stats.subcalls /= PROFILE_RING_SIZE;
    stats.setCombine /= PROFILE_RING_SIZE;
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
    "Core 1", // OC (worker thread)
    "Core 0", // Damage
    "Core 0", // Actor Update
    "Core 1", // Effects (worker thread)
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
    "DL Iterations",      "DL Commands",       "Triangles",          "Vertices",         "Tex Loads",
    "Matrix Loads",       "Pipe Syncs",        "DL Subcalls",        "SetCombine",       "GL Draw Calls",
    "GL Batch Flushes",   "GL State Flushes",  "GL Shader Switches", "GL Shader Compiles", "GL Texture Binds",
    "GL Tex Cache Miss",  "GL Vert Submitted", "GL Tri Submitted",   "GL Time Total ms", "GL Time Dispatch ms",
    "GL Time Tri ms",     "GL Time Tex ms",    "GL Time Shader ms",  "GL Time Draw ms",  "GL Time Vtx ms",
    "GL Time Mtx ms",     "GL Time Depth ms",  "GL Time Setup ms",   "GL Depth Queries", "GL Avg Batch Size",
};

// ── Helper functions ───────────────────────────────────────────────────

static const float PROFILE_TARGET_FRAME_MS = 16.6f;

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
    float fps = (totalMs > 0.01f) ? (1000.0f / totalMs) : 0.0f;

    out << "=== 2S2H Frame Profiler Snapshot ===" << std::endl;
    out << "Timestamp: " << timeBuf << std::endl;
    out << "Branch:    " << branch << std::endl;
    out << "Commit:    " << commitFull << std::endl;
    out << "Build:     " << gBuildVersion << " (" << gBuildDate << ")" << std::endl;
#ifdef __SWITCH__
    out << "Platform:  Nintendo Switch (NX)  [Tegra X1, 4xA57 @1020MHz, GLES 3.2]" << std::endl;
#elif defined(_WIN32)
    out << "Platform:  Windows" << std::endl;
#elif defined(__APPLE__)
    out << "Platform:  macOS" << std::endl;
#elif defined(__linux__)
    out << "Platform:  Linux" << std::endl;
#else
    out << "Platform:  Unknown" << std::endl;
#endif
    out << std::endl;

    out << "--- Summary (60-frame average) ---" << std::endl;
    char summaryBuf[256];
    snprintf(summaryBuf, sizeof(summaryBuf),
             "Total Frame: %.2f ms  (%.1f FPS)  |  Target: 16.60 ms (60 FPS)  |  Over budget: %.2f ms (%.0fx)",
             totalMs, fps, totalMs - PROFILE_TARGET_FRAME_MS,
             totalMs / PROFILE_TARGET_FRAME_MS);
    out << summaryBuf << std::endl;
    out << std::endl;

    out << "--- Per-Phase Breakdown ---" << std::endl;
    out << "Phase                          Time (ms)    % Frame   Budget %" << std::endl;
    out << "─────────────────────────────  ─────────  ─────────  ────────" << std::endl;
    int worstPhase = -1;
    float worstMs = 0.0f;
    for (int i = 0; i < PROFILE_PHASE_MAX; i++) {
        float ms = FrameProfiler_GetPhaseAvgMs((ProfilePhase)i);
        float pct = (totalMs > 0.01f) ? (ms / totalMs * 100.0f) : 0.0f;
        float budgetPct = ms / PROFILE_TARGET_FRAME_MS * 100.0f;
        char lineBuf[256];
        snprintf(lineBuf, sizeof(lineBuf), "%-7s %-22s %8.2f ms  %7.2f%%  %7.1f%%",
                 sPhaseCoreLabels[i], sPhaseNames[i], ms, pct, budgetPct);
        out << lineBuf << std::endl;
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
    float dlIter = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_ITERATIONS);
    float dlCmds = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_COMMANDS);
    float tris = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_TRIANGLES);
    float verts = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_VERTICES);
    float texLoads = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_TEX_LOADS);
    float mtxLoads = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_MTX_LOADS);
    float pipeSyncs = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_PIPE_SYNCS);
    float subcalls = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_SUBCALLS);
    float setCombine = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_SETCOMBINE);
    float glDrawCalls = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_DRAW_CALLS);
    float glBatchFlushes = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_BATCH_FLUSHES);
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
    float glTimeVtx = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_VTX_MS);
    float glTimeMtx = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_MTX_MS);
    float glTimeDepth = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_DEPTH_MS);
    float glTimeSetup = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_SETUP_MS);
    float glDepthQueries = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_PIXEL_DEPTH_QUERIES);
    float glAvgBatch = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_AVG_BATCH_SIZE);

    // --- N64 Display List Input ---
    out << "N64 DL Input (what the game generates):" << std::endl;
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "  DL Iterations: %.0f  |  Commands: %.0f  |  DL Subcalls: %.0f", dlIter, dlCmds, subcalls);
        out << buf << std::endl;
        snprintf(buf, sizeof(buf), "  Triangles: %.0f  |  Vertices: %.0f  |  Matrix Loads: %.0f", tris, verts, mtxLoads);
        out << buf << std::endl;
        snprintf(buf, sizeof(buf), "  Tex Loads: %.0f  |  SetCombine: %.0f  |  PipeSyncs: %.0f", texLoads, setCombine, pipeSyncs);
        out << buf << std::endl;
    }
    out << std::endl;

    // --- GL Output (what Fast3D translates to) ---
    out << "GL Output (what Fast3D translates to):" << std::endl;
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "  Draw Calls: %.0f  |  Batch Flushes: %.0f (state: %.0f)", glDrawCalls, glBatchFlushes, glStateFlushes);
        out << buf << std::endl;
        snprintf(buf, sizeof(buf), "  Triangles: %.0f  |  Vertices: %.0f  |  Avg Batch: %.1f tris/draw", glTris, glVerts, glAvgBatch);
        out << buf << std::endl;
        snprintf(buf, sizeof(buf), "  Shader Switches: %.0f (compiles: %.0f)  |  Texture Binds: %.0f (misses: %.0f)", glShaderSwitches, glShaderCompiles, glTextureBinds, glTextureMisses);
        out << buf << std::endl;
        if (glDepthQueries > 0.5f) {
            snprintf(buf, sizeof(buf), "  Pixel Depth Queries: %.0f", glDepthQueries);
            out << buf << std::endl;
        }
    }
    out << std::endl;

    // --- DL→GL Translation Amplification ---
    out << "DL→GL Translation Amplification:" << std::endl;
    {
        char buf[256];
        float drawAmp = (pipeSyncs > 0.5f) ? (glDrawCalls / pipeSyncs) : 0.0f;
        float triAmp = (tris > 0.5f) ? (glTris / tris) : 0.0f;
        float vtxAmp = (verts > 0.5f) ? (glVerts / verts) : 0.0f;
        snprintf(buf, sizeof(buf), "  Draw calls:  %.0f DL PipeSyncs → %.0f GL draws  (%.1fx amplification)", pipeSyncs, glDrawCalls, drawAmp);
        out << buf << std::endl;
        snprintf(buf, sizeof(buf), "  Triangles:   %.0f DL tris → %.0f GL tris  (%.1fx amplification)", tris, glTris, triAmp);
        out << buf << std::endl;
        snprintf(buf, sizeof(buf), "  Vertices:    %.0f DL verts → %.0f GL verts  (%.1fx amplification)", verts, glVerts, vtxAmp);
        out << buf << std::endl;
        if (drawAmp > 5.0f) {
            out << "  ⚠ High draw amplification: each N64 draw generates " << drawAmp << " GL draws on average." << std::endl;
            out << "    This is caused by state changes (combiner/texture/blend) fragmenting batches." << std::endl;
        }
    }
    out << std::endl;

    // --- GL Timing Breakdown (vertical table) ---
    out << "--- GL Timing Breakdown ---" << std::endl;
    float dlMs = FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_DL_PROCESS);
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "DL Process Total: %.2f ms  |  GL Timed Total: %.2f ms  |  Overhead: %.2f ms",
                 dlMs, glTimeTotal, dlMs - glTimeTotal);
        out << buf << std::endl;
        out << std::endl;

        out << "Sub-Phase                 Time (ms)    % of DL   μs/GL-draw   Description" << std::endl;
        out << "────────────────────────  ─────────  ─────────  ──────────  ──────────────────────────────" << std::endl;

        struct GlTimingRow {
            const char* name;
            float ms;
            const char* desc;
        };
        GlTimingRow rows[] = {
            {"Triangle (VBO fill)",     glTimeTri,      "Per-vertex color combiner + texture coord + VBO writes"},
            {"Draw Submit (GL calls)",  glTimeDraw,     "glDrawArrays + glUniform + state setup per draw call"},
            {"Vertex Transform",        glTimeVtx,      "Matrix × vertex position + lighting + normal transform"},
            {"Texture Setup",           glTimeTex,      "Texture format conversion + upload + cache lookup"},
            {"Matrix Ops",              glTimeMtx,      "4x4 matrix multiply + stack push/pop per G_MTX"},
            {"Dispatch (residual)",     glTimeDispatch, "GBI opcode decode + command loop overhead"},
            {"Pixel Depth",             glTimeDepth,    "glReadPixels for z-buffer readback (if used)"},
            {"Frame Setup/Teardown",    glTimeSetup,    "Per-frame initialization + buffer swap overhead"},
            {"Shader Setup",            glTimeShader,   "Shader compile + program switch overhead"},
        };
        int numRows = sizeof(rows) / sizeof(rows[0]);
        for (int i = 0; i < numRows; i++) {
            if (rows[i].ms < 0.001f) continue;
            float pct = (glTimeTotal > 0.01f) ? (rows[i].ms / glTimeTotal * 100.0f) : 0.0f;
            float usPerDraw = (glDrawCalls > 0.5f) ? (rows[i].ms * 1000.0f / glDrawCalls) : 0.0f;
            snprintf(buf, sizeof(buf), "%-24s  %8.2f ms  %7.1f%%  %9.1f  %s",
                     rows[i].name, rows[i].ms, pct, usPerDraw, rows[i].desc);
            out << buf << std::endl;
        }
    }
    out << std::endl;

    // --- Cost Metrics ---
    out << "--- Cost Metrics ---" << std::endl;
    if (dlMs > 0.1f) {
        char buf[256];
        if (glTris > 0.5f) {
            snprintf(buf, sizeof(buf), "Per GL triangle:    %.1f μs  (%.0f tris in %.2f ms)", dlMs * 1000.0f / glTris, glTris, dlMs);
            out << buf << std::endl;
        }
        if (glDrawCalls > 0.5f) {
            snprintf(buf, sizeof(buf), "Per GL draw call:   %.1f μs  (%.0f draws in %.2f ms)", dlMs * 1000.0f / glDrawCalls, glDrawCalls, dlMs);
            out << buf << std::endl;
        }
        if (dlCmds > 0.5f) {
            snprintf(buf, sizeof(buf), "Per DL command:     %.1f μs  (%.0f cmds in %.2f ms)", dlMs * 1000.0f / dlCmds, dlCmds, dlMs);
            out << buf << std::endl;
        }
        if (glShaderSwitches > 0.5f) {
            snprintf(buf, sizeof(buf), "Per shader switch:  %.1f μs  (%.0f switches in %.2f ms)", dlMs * 1000.0f / glShaderSwitches, glShaderSwitches, dlMs);
            out << buf << std::endl;
        }
    }
    out << std::endl;

    // --- Per-buffer breakdown ---
    static const char* sBufExportNames[] = { "OPA (opaque)", "XLU (translucent)", "Overlay", "Work", "Debug" };
    out << "--- Per-Buffer Breakdown ---" << std::endl;
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "%-20s %6s %6s %6s %5s %5s %5s %5s %5s  %% Cmds",
                 "Buffer", "Cmds", "Tris", "Verts", "Tex", "Mtx", "Sync", "SubDL", "Comb");
        out << buf << std::endl;
        for (int b = 0; b < PROFILE_DL_BUFFER_COUNT; b++) {
            DLBufferStats bs = FrameProfiler_GetBufferStats(b);
            float cmdPct = (dlCmds > 0.5f) ? ((float)bs.commands / dlCmds * 100.0f) : 0.0f;
            snprintf(buf, sizeof(buf), "%-20s %6d %6d %6d %5d %5d %5d %5d %5d  %5.1f%%",
                     sBufExportNames[b], bs.commands, bs.triangles, bs.vertices,
                     bs.texLoads, bs.mtxLoads, bs.pipeSyncs, bs.subcalls, bs.setCombine, cmdPct);
            out << buf << std::endl;
        }
    }
    out << std::endl;

    // Unaccounted time
    {
        char buf[256];
        float accountedMs = GetAccountedTimeMs();
        float unaccountedMs = totalMs - accountedMs;
        float unaccountedPct = (totalMs > 0.01f) ? (unaccountedMs / totalMs * 100.0f) : 0.0f;
        snprintf(buf, sizeof(buf), "Unaccounted: %.2f ms (%.1f%%)  [OS scheduling, vsync, GL driver internal]",
                 unaccountedMs, unaccountedPct);
        out << buf << std::endl;
    }
    out << std::endl;

    // --- Flush efficiency analysis ---
    out << "--- Flush Efficiency ---" << std::endl;
    float emptyFlushes = glBatchFlushes - glDrawCalls;
    float bufferFullFlushes = glBatchFlushes - glStateFlushes;
    float emptyPct = (glBatchFlushes > 0.5f) ? (emptyFlushes / glBatchFlushes * 100.0f) : 0.0f;
    float effectiveBatch = (glDrawCalls > 0.5f) ? (glTris / glDrawCalls) : 0.0f;
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Total Flushes: %.0f", glBatchFlushes);
        out << buf << std::endl;
        snprintf(buf, sizeof(buf), "  State-change driven: %.0f  |  Buffer-full: %.0f  |  Empty (no geometry): %.0f (%.1f%%)",
                 glStateFlushes, bufferFullFlushes, emptyFlushes, emptyPct);
        out << buf << std::endl;
        snprintf(buf, sizeof(buf), "Actual Draw Calls: %.0f  |  Effective Batch: %.1f tris/draw", glDrawCalls, effectiveBatch);
        out << buf << std::endl;
#ifdef __SWITCH__
        snprintf(buf, sizeof(buf), "MAX_TRI_BUFFER: 1024  |  Utilization: %.2f%%  |  State changes/draw: %.2f",
                 effectiveBatch / 1024.0f * 100.0f, glDrawCalls > 0.5f ? glStateFlushes / glDrawCalls : 0.0f);
#else
        snprintf(buf, sizeof(buf), "MAX_TRI_BUFFER: 256  |  Utilization: %.2f%%  |  State changes/draw: %.2f",
                 effectiveBatch / 256.0f * 100.0f, glDrawCalls > 0.5f ? glStateFlushes / glDrawCalls : 0.0f);
#endif
        out << buf << std::endl;
    }
    out << std::endl;

    // Frame stability analysis from ring buffer
    out << "--- Frame Stability ---" << std::endl;
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
            float cov = (mean > 0.01f) ? (stddev / mean * 100.0f) : 0.0f;
            char buf[256];
            snprintf(buf, sizeof(buf), "Frames sampled: %d", validFrames);
            out << buf << std::endl;
            snprintf(buf, sizeof(buf), "Frame Time:  Min: %.2f ms  Max: %.2f ms  Range: %.2f ms",
                     minFrame, maxFrame, maxFrame - minFrame);
            out << buf << std::endl;
            snprintf(buf, sizeof(buf), "             Mean: %.2f ms  StdDev: %.2f ms  CoV: %.1f%%",
                     mean, stddev, cov);
            out << buf << std::endl;
            if (cov < 1.0f) {
                out << "             Stability: EXCELLENT (very consistent frame times)" << std::endl;
            } else if (cov < 3.0f) {
                out << "             Stability: GOOD (minor variation)" << std::endl;
            } else if (cov < 5.0f) {
                out << "             Stability: FAIR (noticeable hitches)" << std::endl;
            } else {
                out << "             Stability: POOR (significant jitter)" << std::endl;
            }
            // Count frames with spikes (>2 stddev from mean)
            int spikes = 0;
            float spikeThreshold = mean + 2.0f * stddev;
            for (int f = 0; f < PROFILE_RING_SIZE; f++) {
                float val = sPhaseRing[PROFILE_PHASE_TOTAL_FRAME][f];
                if (val > spikeThreshold) spikes++;
            }
            if (spikes > 0) {
                snprintf(buf, sizeof(buf), "Spikes (>2σ): %d/%d frames above %.2f ms", spikes, validFrames, spikeThreshold);
                out << buf << std::endl;
            }
            // DL Process stability
            float dlMin = 1e9f, dlMax = 0.0f;
            for (int f = 0; f < PROFILE_RING_SIZE; f++) {
                float val = sPhaseRing[PROFILE_PHASE_DL_PROCESS][f];
                if (val > 0.01f) {
                    if (val < dlMin) dlMin = val;
                    if (val > dlMax) dlMax = val;
                }
            }
            if (validFrames > 0) {
                snprintf(buf, sizeof(buf), "DL Process:  Min: %.2f ms  Max: %.2f ms  Range: %.2f ms",
                         dlMin, dlMax, dlMax - dlMin);
                out << buf << std::endl;
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
    float core1Ms =
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_COLLISION_OC) + FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_EFFECTS);
    float imbalance = (core0Ms > 0.01f) ? (core1Ms / core0Ms) : 0.0f;
    float gameLogicMs =
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_ACTOR_UPDATE) +
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_ACTOR_DRAW) +
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_SCENE_DRAW) +
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_COLLISION_AT) +
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_COLLISION_OC) +
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_COLLISION_DAMAGE) +
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_EFFECTS);

    out << "--- Core Utilization ---" << std::endl;
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Core 0 Active: %.2f ms  |  Core 1 Active: %.2f ms  |  Ratio: %.2f%%",
                 core0Ms, core1Ms, imbalance * 100.0f);
        out << buf << std::endl;
        snprintf(buf, sizeof(buf), "Game Logic Total: %.2f ms (%.1f%% of frame)  |  Rendering: %.2f ms (%.1f%% of frame)",
                 gameLogicMs, totalMs > 0.01f ? gameLogicMs / totalMs * 100.0f : 0.0f,
                 dlMs, totalMs > 0.01f ? dlMs / totalMs * 100.0f : 0.0f);
        out << buf << std::endl;
        if (imbalance < 0.05f) {
            out << "⚠ Extreme core imbalance — Core 1 is nearly idle. Main thread is bottleneck." << std::endl;
        }
    }
    out << std::endl;

    // Automated analysis with ranked sub-phases and specific recommendations
    out << "--- Analysis & Recommendations ---" << std::endl;
    if (worstPhase >= 0 && totalMs > 0.01f) {
        float worstPct = worstMs / totalMs * 100.0f;
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "BOTTLENECK: %s (%.2f ms, %.1f%% of frame, %.1fx over budget)",
                     sPhaseNames[worstPhase], worstMs, worstPct, worstMs / PROFILE_TARGET_FRAME_MS);
            out << buf << std::endl;
        }
        if (worstPhase == PROFILE_PHASE_DL_PROCESS) {
            out << std::endl;
            out << "The Fast3D display list interpreter is the dominant cost." << std::endl;
            out << "This is CPU-bound N64-to-GL translation, single-threaded on Core 0." << std::endl;
            out << "Game logic (" << gameLogicMs << " ms) is NOT the bottleneck." << std::endl;
            out << std::endl;

            // Ranked GL sub-phases (all non-zero)
            struct GlPhaseInfo { const char* name; float ms; const char* hint; };
            GlPhaseInfo glPhases[] = {
                {"tri (VBO fill)", glTimeTri, "Pre-compute combiner inputs, NEON vectorize, reduce per-vertex branching"},
                {"draw (GL submit)", glTimeDraw, "Reduce draw calls via batching, skip empty flushes, lazy state changes"},
                {"vtx (vertex transform)", glTimeVtx, "NEON matrix*vertex, batch lighting, reduce per-vertex allocations"},
                {"tex (texture setup)", glTimeTex, "NEON format conversion, larger texture cache, reduce SETTIMG count"},
                {"mtx (matrix ops)", glTimeMtx, "NEON 4x4 multiply, reduce matrix stack pushes from game code"},
                {"dispatch (cmd walk)", glTimeDispatch, "Optimize opcode dispatch, reduce total DL command count"},
                {"depth (pixel readback)", glTimeDepth, "Quantize coordinates, cache results, batch readback queries"},
                {"setup (frame init)", glTimeSetup, "Minimize per-frame buffer allocation and state initialization"},
                {"shader (compile/switch)", glTimeShader, "Pre-compile all variants, cache program objects, reduce switches"}
            };
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
            out << "Priority Ranking (GL sub-phases by time):" << std::endl;
            for (int i = 0; i < numPhases; i++) {
                if (glPhases[i].ms < 0.01f) break;
                float pct = (glTimeTotal > 0.01f) ? (glPhases[i].ms / glTimeTotal * 100.0f) : 0.0f;
                char buf[512];
                snprintf(buf, sizeof(buf), "  #%d %-24s %7.2f ms (%5.1f%%)  → %s",
                         i + 1, glPhases[i].name, glPhases[i].ms, pct, glPhases[i].hint);
                out << buf << std::endl;
            }

            // Specific recommendations based on data thresholds
            out << std::endl;
            out << "Data-Driven Recommendations:" << std::endl;
            int recCount = 0;
            if (emptyPct > 30.0f) {
                char buf[256];
                snprintf(buf, sizeof(buf), "  [%d] %.0f%% empty flushes (%.0f/%.0f). Defer GL state until geometry is submitted.",
                         ++recCount, emptyPct, emptyFlushes, glBatchFlushes);
                out << buf << std::endl;
            }
            if (effectiveBatch < 20.0f && glDrawCalls > 100.0f) {
                char buf[256];
                snprintf(buf, sizeof(buf), "  [%d] Batch size %.1f tris/draw is low. %.0f state flushes across %.0f draws = %.1f changes/draw.",
                         ++recCount, effectiveBatch, glStateFlushes, glDrawCalls, glStateFlushes / glDrawCalls);
                out << buf << std::endl;
            }
            if (imbalance < 0.1f && core0Ms > 30.0f) {
                char buf[256];
                snprintf(buf, sizeof(buf), "  [%d] Core 0=%.1fms vs Core 1=%.1fms (%.1f%%). Offload DL/interp to worker thread.",
                         ++recCount, core0Ms, core1Ms, imbalance * 100.0f);
                out << buf << std::endl;
            }
            if (glTimeDraw > 10.0f && glDrawCalls > 500.0f) {
                char buf[256];
                float costPerDraw = glTimeDraw * 1000.0f / glDrawCalls;
                snprintf(buf, sizeof(buf), "  [%d] Draw overhead: %.0f calls × %.1f μs = %.1f ms. 50%% reduction saves ~%.1f ms.",
                         ++recCount, glDrawCalls, costPerDraw, glTimeDraw, glTimeDraw * 0.5f);
                out << buf << std::endl;
            }
            if (glTimeTri > 15.0f) {
                char buf[256];
                float usPerGlTri = (glTris > 0.5f) ? (glTimeTri * 1000.0f / glTris) : 0.0f;
                snprintf(buf, sizeof(buf), "  [%d] VBO fill: %.1f ms (%.1f μs/tri for %.0f tris). Inner loop is dominant cost.",
                         ++recCount, glTimeTri, usPerGlTri, glTris);
                out << buf << std::endl;
            }
            if (recCount == 0) {
                out << "  No critical issues detected. Profile is well-optimized for current workload." << std::endl;
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

    // Raw ring buffer data — compact format (2 decimal places)
    int ringIdx = sRingIndex.load(std::memory_order_relaxed);
    out << "--- Raw Ring Buffer (last " << PROFILE_RING_SIZE << " frames, ms) ---" << std::endl;
    out << "Current index: " << ringIdx << std::endl;
    // Only output phases with meaningful data (skip near-zero phases)
    for (int phase = 0; phase < PROFILE_PHASE_MAX; phase++) {
        float avg = FrameProfiler_GetPhaseAvgMs((ProfilePhase)phase);
        if (avg < 0.001f) continue; // skip phases with <1μs average
        out << sPhaseNames[phase] << ":";
        for (int f = 0; f < PROFILE_RING_SIZE; f++) {
            int idx = (ringIdx + 1 + f) % PROFILE_RING_SIZE;
            char valBuf[16];
            snprintf(valBuf, sizeof(valBuf), " %.2f", sPhaseRing[phase][idx]);
            out << valBuf;
        }
        out << std::endl;
    }

    out.close();
    sLastExportPath = filepath;
    sExportMsgTimer = 5.0f;
}

// ── ImGui Window ───────────────────────────────────────────────────────

void FrameProfilerWindow::InitElement() {
}

void FrameProfilerWindow::UpdateElement() {
}

void FrameProfilerWindow::DrawElement() {
    // Enable profiling and reset the keepalive countdown
    sEnabled.store(1, std::memory_order_relaxed);
    sDrawCountdown = PROFILE_KEEPALIVE_FRAMES;

    float totalMs = FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_TOTAL_FRAME);
    float fps = (totalMs > 0.01f) ? (1000.0f / totalMs) : 0.0f;

    ImGui::Text("Frame: %.1f ms  (%.0f FPS)  Target: %.1f ms (60 FPS)", totalMs, fps, PROFILE_TARGET_FRAME_MS);
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

    float dlIter = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_ITERATIONS);
    float dlCmds = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_COMMANDS);
    float tris = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_TRIANGLES);
    float verts = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_VERTICES);
    float texLoads = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_TEX_LOADS);
    float mtxLoads = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_MTX_LOADS);
    float pipeSyncs = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_PIPE_SYNCS);
    float subcalls = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_SUBCALLS);
    float setCombine = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_SETCOMBINE);
    float glDrawCalls = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_DRAW_CALLS);
    float glBatchFlushes = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_BATCH_FLUSHES);
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
    float glTimeVtx = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_VTX_MS);
    float glTimeMtx = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_MTX_MS);
    float glTimeDepth = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_DEPTH_MS);
    float glTimeSetup = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TIME_SETUP_MS);
    float glDepthQueries = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_PIXEL_DEPTH_QUERIES);
    float glAvgBatch = FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_AVG_BATCH_SIZE);

    ImGui::Text("DL Iterations: %.0f   Total Commands: %.0f", dlIter, dlCmds);
    ImGui::Text("Triangles: %.0f   Vertices: %.0f", tris, verts);
    ImGui::Text("Tex Loads: %.0f   Matrix Loads: %.0f   SetCombine: %.0f", texLoads, mtxLoads, setCombine);
    ImGui::Text("Pipe Syncs: %.0f   DL Subcalls: %.0f", pipeSyncs, subcalls);

    // Estimated draw calls (each PipeSync potentially flushes a draw call)
    float estDrawCalls = pipeSyncs;
    ImGui::Text("Est. Draw Calls: ~%.0f", estDrawCalls);
    ImGui::Text("GL Draw Calls: %.0f  Batch Flushes: %.0f (state: %.0f)", glDrawCalls, glBatchFlushes, glStateFlushes);
    ImGui::Text("GL Shader Switches: %.0f (compiles: %.0f)", glShaderSwitches, glShaderCompiles);
    ImGui::Text("GL Texture Binds: %.0f (cache misses: %.0f)", glTextureBinds, glTextureMisses);
    ImGui::Text("GL Submitted: %.0f tris, %.0f verts  Avg batch: %.1f tris/draw", glTris, glVerts, glAvgBatch);
    if (glDepthQueries > 0.0f) {
        ImGui::Text("Pixel Depth Queries: %.0f", glDepthQueries);
    }

    // Detailed timing breakdown with visual bar chart
    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "--- Fast3D Timing Breakdown (%.2f ms total) ---", glTimeTotal);
    float timingBarMax = glTimeTotal > 0.01f ? glTimeTotal : 1.0f;

    struct TimingEntry { const char* name; float ms; ImVec4 color; };
    TimingEntry timings[] = {
        { "Triangle Processing", glTimeTri, ImVec4(1.0f, 0.4f, 0.4f, 1.0f) },
        { "Draw Submit (GL)",    glTimeDraw, ImVec4(1.0f, 0.7f, 0.3f, 1.0f) },
        { "Vertex Load",         glTimeVtx, ImVec4(0.4f, 0.8f, 1.0f, 1.0f) },
        { "Texture Setup",       glTimeTex, ImVec4(0.4f, 1.0f, 0.6f, 1.0f) },
        { "Matrix Ops",          glTimeMtx, ImVec4(0.8f, 0.6f, 1.0f, 1.0f) },
        { "Frame Setup/Teardown",glTimeSetup, ImVec4(0.6f, 0.6f, 0.6f, 1.0f) },
        { "Pixel Depth",         glTimeDepth, ImVec4(1.0f, 1.0f, 0.4f, 1.0f) },
        { "Shader Setup",        glTimeShader, ImVec4(0.4f, 1.0f, 1.0f, 1.0f) },
        { "Dispatch (residual)", glTimeDispatch, ImVec4(0.7f, 0.7f, 0.7f, 1.0f) },
    };
    for (const auto& t : timings) {
        if (t.ms < 0.001f) continue; // skip zero entries
        float frac = t.ms / timingBarMax;
        if (frac > 1.0f) frac = 1.0f;
        float pct = glTimeTotal > 0.01f ? (t.ms / glTimeTotal * 100.0f) : 0.0f;
        ImGui::TextColored(t.color, "  %-22s %6.2f ms (%4.1f%%)", t.name, t.ms, pct);
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
    float core1Ms =
        FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_COLLISION_OC) + FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_EFFECTS);

    ImGui::Text("Core 0 Active: %5.1f ms  |  Core 1 Active: %5.1f ms", core0Ms, core1Ms);
    float imbalance = (core0Ms > 0.01f) ? (core1Ms / core0Ms) : 0.0f;
    ImGui::Text("Core utilization ratio: %.0f%% (1.0 = perfectly balanced)", imbalance * 100.0f);

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
