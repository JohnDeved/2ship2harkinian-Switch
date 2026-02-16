#include "FrameProfiler.h"

#include <imgui.h>
#include <cstring>
#include <cstdint>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

// Ring buffer depth — average over this many frames for stable display
#define PROFILE_RING_SIZE 60

// ── Timing helper ──────────────────────────────────────────────────────

static inline uint64_t ProfileGetNs(void) {
#ifdef _WIN32
    LARGE_INTEGER counter, freq;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&freq);
    return (uint64_t)(counter.QuadPart * 1000000000ULL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

// ── Per-phase data ─────────────────────────────────────────────────────

static uint64_t sPhaseStart[PROFILE_PHASE_MAX];
static float    sPhaseRing[PROFILE_PHASE_MAX][PROFILE_RING_SIZE]; // ms per frame
static int      sRingIndex;
static int      sEnabled = 0;

// ── C API ──────────────────────────────────────────────────────────────

extern "C" void FrameProfiler_StartPhase(ProfilePhase phase) {
    if (!sEnabled) return;
    sPhaseStart[phase] = ProfileGetNs();
}

extern "C" void FrameProfiler_EndPhase(ProfilePhase phase) {
    if (!sEnabled) return;
    uint64_t elapsed = ProfileGetNs() - sPhaseStart[phase];
    sPhaseRing[phase][sRingIndex] = (float)elapsed / 1e6f; // ns → ms
}

extern "C" void FrameProfiler_EndFrame(void) {
    if (!sEnabled) return;
    sRingIndex = (sRingIndex + 1) % PROFILE_RING_SIZE;
    // Clear the next slot so phases not measured this frame show 0
    for (int i = 0; i < PROFILE_PHASE_MAX; i++) {
        sPhaseRing[i][sRingIndex] = 0.0f;
    }
}

extern "C" float FrameProfiler_GetPhaseAvgMs(ProfilePhase phase) {
    float sum = 0.0f;
    for (int i = 0; i < PROFILE_RING_SIZE; i++) {
        sum += sPhaseRing[phase][i];
    }
    return sum / PROFILE_RING_SIZE;
}

extern "C" int FrameProfiler_IsEnabled(void) {
    return sEnabled;
}

// ── Phase names for display ────────────────────────────────────────────

static const char* sPhaseNames[PROFILE_PHASE_MAX] = {
    "Collision AT",
    "Collision OC",
    "Collision Damage",
    "Actor Update",
    "Effects",
    "Actor Draw",
    "Frame Interp",
    "GFX Commands",
    "Worker Idle",
    "Total Frame",
};

// Core assignment labels for display
static const char* sPhaseCoreLabels[PROFILE_PHASE_MAX] = {
    "Pool  ",  // AT (now runs on worker pool)
    "Pool  ",  // OC (now runs on worker pool)
    "Core 0",  // Damage
    "Core 0",  // Actor Update
    "Core 1",  // Effects
    "Core 0",  // Actor Draw
    "Core 0",  // Frame Interp
    "Core 0",  // GFX Commands
    "Core 1",  // Worker Idle
    "Core 0",  // Total
};

// ── ImGui Window ───────────────────────────────────────────────────────

void FrameProfilerWindow::InitElement() {
}

void FrameProfilerWindow::UpdateElement() {
}

void FrameProfilerWindow::DrawElement() {
    sEnabled = 1;

    float totalMs = FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_TOTAL_FRAME);
    float fps = (totalMs > 0.01f) ? (1000.0f / totalMs) : 0.0f;

    ImGui::Text("Frame: %.1f ms  (%.0f FPS)  Target: 16.6 ms (60 FPS)", totalMs, fps);
    ImGui::Separator();

    // Bar chart-style display
    float maxMs = 16.6f; // target frame time for reference
    for (int i = 0; i < PROFILE_PHASE_MAX; i++) {
        if (i == PROFILE_PHASE_TOTAL_FRAME) continue; // shown above
        if (i == PROFILE_PHASE_WORKER_IDLE) continue;  // shown separately

        float ms = FrameProfiler_GetPhaseAvgMs((ProfilePhase)i);
        float frac = ms / maxMs;
        if (frac > 1.0f) frac = 1.0f;

        ImGui::Text("%-7s %-18s %5.1f ms", sPhaseCoreLabels[i], sPhaseNames[i], ms);
        ImGui::SameLine();
        ImGui::ProgressBar(frac, ImVec2(200, 0), "");
    }

    ImGui::Separator();

    // Worker idle
    float idleMs = FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_WORKER_IDLE);
    float idlePct = (totalMs > 0.01f) ? (idleMs / totalMs * 100.0f) : 0.0f;
    ImGui::Text("Core 1  Worker Idle     %5.1f ms (%.0f%% idle)", idleMs, idlePct);

    ImGui::Separator();

    // Breakdown summary — AT and OC now run on worker pool, not core 0
    float core0Ms = FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_COLLISION_DAMAGE) +
                    FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_ACTOR_UPDATE) +
                    FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_ACTOR_DRAW) +
                    FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_FRAME_INTERP) +
                    FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_GFX_COMMANDS);
    float poolMs  = FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_COLLISION_AT) +
                    FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_COLLISION_OC);
    float core1Ms = FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_EFFECTS);

    ImGui::Text("Core 0 Active: %5.1f ms  |  Pool (AT+OC): %5.1f ms  |  Worker: %5.1f ms",
                core0Ms, poolMs, core1Ms);
    float offloadMs = poolMs + core1Ms;
    float imbalance = (core0Ms > 0.01f) ? (offloadMs / core0Ms) : 0.0f;
    ImGui::Text("Core utilization ratio: %.0f%% (1.0 = perfectly balanced)", imbalance * 100.0f);
}
