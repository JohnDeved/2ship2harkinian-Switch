#include "FrameProfiler.h"

#include <imgui.h>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <ctime>
#include <libultraship/libultraship.h>

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
    "DL Iterations",
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

    std::string filename = "profiler_snapshot_" + std::string(timeBuf) + ".txt";
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
    out << std::endl;

    out << "--- Summary (60-frame average) ---" << std::endl;
    out << "Total Frame: " << totalMs << " ms (" << fps << " FPS)" << std::endl;
    out << "Target: 16.6 ms (60 FPS)" << std::endl;
    out << std::endl;

    out << "--- Per-Phase Breakdown ---" << std::endl;
    int worstPhase = -1;
    float worstMs = 0.0f;
    for (int i = 0; i < PROFILE_PHASE_MAX; i++) {
        float ms = FrameProfiler_GetPhaseAvgMs((ProfilePhase)i);
        float pct = (totalMs > 0.01f) ? (ms / totalMs * 100.0f) : 0.0f;
        out << sPhaseCoreLabels[i] << "  " << sPhaseNames[i] << ": " << ms << " ms (" << pct << "%)" << std::endl;
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
    out << "--- Counters ---" << std::endl;
    for (int i = 0; i < PROFILE_COUNTER_MAX; i++) {
        float avg = FrameProfiler_GetCounterAvg((ProfileCounter)i);
        out << sCounterNames[i] << ": " << avg << std::endl;
    }
    out << std::endl;

    // Unaccounted time
    float accountedMs = GetAccountedTimeMs();
    float unaccountedMs = totalMs - accountedMs;
    float unaccountedPct = (totalMs > 0.01f) ? (unaccountedMs / totalMs * 100.0f) : 0.0f;
    out << "Unaccounted: " << unaccountedMs << " ms (" << unaccountedPct << "%)" << std::endl;
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

    out << "--- Core Utilization ---" << std::endl;
    out << "Core 0 Active: " << core0Ms << " ms" << std::endl;
    out << "Core 1 Active: " << core1Ms << " ms" << std::endl;
    out << "Utilization Ratio: " << (imbalance * 100.0f) << "%" << std::endl;
    out << std::endl;

    // Automated analysis
    out << "--- Analysis ---" << std::endl;
    if (worstPhase >= 0 && totalMs > 0.01f) {
        float worstPct = worstMs / totalMs * 100.0f;
        out << "Bottleneck: " << sPhaseNames[worstPhase] << " (" << worstMs << " ms, " << worstPct << "% of frame)"
            << std::endl;
        if (worstPhase == PROFILE_PHASE_DL_PROCESS) {
            out << "The display list interpreter (libultraship Fast3D) dominates frame time." << std::endl;
            out << "This is CPU-bound N64 DL-to-GL translation running single-threaded on Core 0." << std::endl;
            out << "Game logic optimizations (BgCheck, actors, etc.) will have minimal impact." << std::endl;
            out << "Priority: (1) Optimize Fast3D interpreter, (2) Render thread offload, (3) Reduce DL complexity."
                << std::endl;
        } else if (worstPhase == PROFILE_PHASE_ACTOR_UPDATE) {
            out << "Actor updates dominate. Parallelize BgCheck queries across worker threads." << std::endl;
        } else if (worstPhase == PROFILE_PHASE_ACTOR_DRAW) {
            out << "Actor draw (skeleton/matrix) dominates. Pre-compute matrices on worker threads." << std::endl;
        } else if (worstPhase == PROFILE_PHASE_FRAME_INTERP) {
            out << "Frame interpolation dominates. Consider parallel segment processing." << std::endl;
        }
    }
    out << std::endl;

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

    // Counters
    for (int i = 0; i < PROFILE_COUNTER_MAX; i++) {
        float avg = FrameProfiler_GetCounterAvg((ProfileCounter)i);
        ImGui::Text("%-18s %.1f", sCounterNames[i], avg);
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
                               "This is CPU-bound N64 DL-to-GL translation running single-threaded on Core 0. "
                               "Game logic optimizations (BgCheck, actors, etc.) will have minimal impact.");
            ImGui::TextWrapped("Priority actions: (1) Optimize Fast3D interpreter in libultraship, "
                               "(2) Move DL processing to a dedicated render thread, "
                               "(3) Reduce DL complexity (draw distance, LOD).");
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
