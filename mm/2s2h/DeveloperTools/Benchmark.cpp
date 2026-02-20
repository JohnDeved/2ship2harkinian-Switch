#include "Benchmark.h"
#include "FrameProfiler.h"

#include <imgui.h>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <ctime>
#include <string>
#include <vector>
#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
}

#include "build.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

// ── Benchmark scene definitions ────────────────────────────────────────
// Each entry defines a scene to test with its entrance, human-readable name,
// and the number of frames to measure. Scenes are chosen to stress different
// parts of the rendering pipeline (many actors, large geometry, effects, etc.)

struct BenchmarkScene {
    u16 entrance;
    const char* name;
    int settleFrames; // frames to wait for scene to fully load / stabilize
    int measureFrames; // frames to collect profiler data
};

// These scenes were selected for their complexity and rendering cost:
//   South Clock Town — high actor count, many NPCs and objects
//   North Clock Town — dense geometry with playground and Great Fairy fountain entrance
//   East Clock Town  — moderate complexity, shops and actors
//   Termina Field    — large open area, distant actors, skybox
//   Woodfall         — swamp effects, water rendering
//   Great Bay Coast  — water rendering, large area
//   Ikana Canyon     — tall geometry, varied lighting
static const BenchmarkScene sBenchmarkScenes[] = {
    { ENTRANCE(SOUTH_CLOCK_TOWN, 0), "South Clock Town", 120, 300 },
    { ENTRANCE(NORTH_CLOCK_TOWN, 0), "North Clock Town", 120, 300 },
    { ENTRANCE(EAST_CLOCK_TOWN, 0), "East Clock Town", 120, 300 },
    { ENTRANCE(TERMINA_FIELD, 0), "Termina Field", 120, 300 },
    { ENTRANCE(WOODFALL, 0), "Woodfall", 120, 300 },
    { ENTRANCE(GREAT_BAY_COAST, 0), "Great Bay Coast", 120, 300 },
    { ENTRANCE(IKANA_CANYON, 0), "Ikana Canyon", 120, 300 },
};
static constexpr int BENCHMARK_SCENE_COUNT = sizeof(sBenchmarkScenes) / sizeof(sBenchmarkScenes[0]);

// ── Per-scene result storage ───────────────────────────────────────────

struct BenchmarkResult {
    const char* name;
    float totalFrameMs;
    float fps;
    float actorUpdateMs;
    float actorDrawMs;
    float dlProcessMs;
    float gfxCommandsMs;
    float dlTriangles;
    float dlCommands;
    float glDrawCalls;
    float glTriangles;
    float glAvgBatchSize;
    float glShaderSwitches;
};

// ── State machine ──────────────────────────────────────────────────────

enum BenchmarkState {
    BENCH_IDLE,
    BENCH_STARTING,   // one-frame delay to init save context
    BENCH_WARPING,    // scene transition triggered, waiting for load
    BENCH_SETTLING,   // scene loaded, waiting for stabilization
    BENCH_MEASURING,  // collecting profiler data
    BENCH_NEXT_SCENE, // transitioning to next scene
    BENCH_DONE,       // all scenes measured, results ready
};

static BenchmarkState sState = BENCH_IDLE;
static int sCurrentScene = 0;
static int sFrameCounter = 0;
static bool sSceneReady = false;
static HOOK_ID sSceneInitHookId = 0;
static HOOK_ID sUpdateHookId = 0;
static bool sHooksRegistered = false;

static std::vector<BenchmarkResult> sResults;
static std::string sLastExportPath;
static float sExportMsgTimer = 0.0f;

// Accumulator for averaging measurements over the measuring period
struct MeasureAccum {
    double totalFrameMs;
    double actorUpdateMs;
    double actorDrawMs;
    double dlProcessMs;
    double gfxCommandsMs;
    double dlTriangles;
    double dlCommands;
    double glDrawCalls;
    double glTriangles;
    double glAvgBatchSize;
    double glShaderSwitches;
    int frameCount;
};
static MeasureAccum sAccum;

static void ResetAccum() {
    memset(&sAccum, 0, sizeof(sAccum));
}

// ── Scene transition helper ────────────────────────────────────────────

static void BenchmarkWarpToScene(u16 entrance) {
    if (gPlayState == NULL) {
        return;
    }
    gPlayState->nextEntrance = Entrance_Create(entrance >> 9, 0, entrance & 0xF);
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_INSTANT;
    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;
}

// ── Hook callbacks ─────────────────────────────────────────────────────

static void OnBenchmarkSceneInit(s8 sceneId, s8 spawnNum) {
    if (sState == BENCH_WARPING) {
        sSceneReady = true;
    }
}

static void OnBenchmarkUpdate() {
    if (sState == BENCH_IDLE || sState == BENCH_DONE) {
        return;
    }

    // Keep the profiler enabled while the benchmark is running
    FrameProfiler_KeepAlive();

    if (gPlayState == NULL) {
        return;
    }

    switch (sState) {
        case BENCH_STARTING: {
            // Initialize a clean save state for deterministic conditions
            gSaveContext.save.time = CLOCK_TIME(8, 0);
            gSaveContext.save.day = 1;
            sCurrentScene = 0;
            sResults.clear();
            sState = BENCH_WARPING;
            sSceneReady = false;
            BenchmarkWarpToScene(sBenchmarkScenes[sCurrentScene].entrance);
            break;
        }
        case BENCH_WARPING: {
            if (sSceneReady) {
                sSceneReady = false;
                sFrameCounter = 0;
                sState = BENCH_SETTLING;
            }
            break;
        }
        case BENCH_SETTLING: {
            sFrameCounter++;
            if (sFrameCounter >= sBenchmarkScenes[sCurrentScene].settleFrames) {
                sFrameCounter = 0;
                ResetAccum();
                sState = BENCH_MEASURING;
            }
            break;
        }
        case BENCH_MEASURING: {
            // Collect per-frame data from profiler
            sAccum.totalFrameMs += FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_TOTAL_FRAME);
            sAccum.actorUpdateMs += FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_ACTOR_UPDATE);
            sAccum.actorDrawMs += FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_ACTOR_DRAW);
            sAccum.dlProcessMs += FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_DL_PROCESS);
            sAccum.gfxCommandsMs += FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_GFX_COMMANDS);
            sAccum.dlTriangles += FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_TRIANGLES);
            sAccum.dlCommands += FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_COMMANDS);
            sAccum.glDrawCalls += FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_DRAW_CALLS);
            sAccum.glTriangles += FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_TRIANGLES_SUBMITTED);
            sAccum.glAvgBatchSize += FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_AVG_BATCH_SIZE);
            sAccum.glShaderSwitches += FrameProfiler_GetCounterAvg(PROFILE_COUNTER_GL_SHADER_SWITCHES);
            sAccum.frameCount++;

            sFrameCounter++;
            if (sFrameCounter >= sBenchmarkScenes[sCurrentScene].measureFrames) {
                // Store results for this scene
                int n = sAccum.frameCount > 0 ? sAccum.frameCount : 1;
                BenchmarkResult result;
                result.name = sBenchmarkScenes[sCurrentScene].name;
                result.totalFrameMs = (float)(sAccum.totalFrameMs / n);
                result.fps = (result.totalFrameMs > 0.01f) ? (1000.0f / result.totalFrameMs) : 0.0f;
                result.actorUpdateMs = (float)(sAccum.actorUpdateMs / n);
                result.actorDrawMs = (float)(sAccum.actorDrawMs / n);
                result.dlProcessMs = (float)(sAccum.dlProcessMs / n);
                result.gfxCommandsMs = (float)(sAccum.gfxCommandsMs / n);
                result.dlTriangles = (float)(sAccum.dlTriangles / n);
                result.dlCommands = (float)(sAccum.dlCommands / n);
                result.glDrawCalls = (float)(sAccum.glDrawCalls / n);
                result.glTriangles = (float)(sAccum.glTriangles / n);
                result.glAvgBatchSize = (float)(sAccum.glAvgBatchSize / n);
                result.glShaderSwitches = (float)(sAccum.glShaderSwitches / n);
                sResults.push_back(result);

                sCurrentScene++;
                if (sCurrentScene >= BENCHMARK_SCENE_COUNT) {
                    sState = BENCH_DONE;
                } else {
                    sState = BENCH_WARPING;
                    sSceneReady = false;
                    BenchmarkWarpToScene(sBenchmarkScenes[sCurrentScene].entrance);
                }
            }
            break;
        }
        default:
            break;
    }
}

// ── Hook registration ──────────────────────────────────────────────────

static void RegisterBenchmarkHooks() {
    if (sHooksRegistered) {
        return;
    }
    sHooksRegistered = true;

    sSceneInitHookId =
        GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneInit>(OnBenchmarkSceneInit);
    sUpdateHookId =
        GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameStateUpdate>(OnBenchmarkUpdate);
}

static void RegisterBenchmark() {
    RegisterBenchmarkHooks();
}

RegisterShipInitFunc initFuncBenchmark(RegisterBenchmark, {});

// ── Export benchmark report ────────────────────────────────────────────

static void ExportBenchmarkReport() {
    if (sResults.empty()) {
        return;
    }

    time_t now = std::time(nullptr);
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &now);
#else
    localtime_r(&now, &tmBuf);
#endif
    char timeBuf[64];
    std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", &tmBuf);

    std::string branch = (gGitBranch[0] != '\0') ? gGitBranch : "unknown";
    std::string commitFull = (gGitCommitHash[0] != '\0') ? gGitCommitHash : "unknown";
    std::string commitShort = (commitFull.size() > 7) ? commitFull.substr(0, 7) : commitFull;

    std::string branchSafe = branch;
    for (char& c : branchSafe) {
        if (c == '/' || c == '\\' || c == ' ' || c == ':')
            c = '-';
    }

    std::string filename = "benchmark_" + branchSafe + "_" + commitShort + "_" + std::string(timeBuf) + ".txt";
    std::string filepath = Ship::Context::GetPathRelativeToAppDirectory(filename);

    std::ofstream out(filepath);
    if (!out.is_open()) {
        sLastExportPath = "ERROR: Could not write " + filepath;
        sExportMsgTimer = 5.0f;
        return;
    }

    out << "=== 2S2H Benchmark Report ===" << std::endl;
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
    out << "Scenes: " << sResults.size() << std::endl;
    out << std::endl;

    // Compute overall averages
    float totalFpsSum = 0.0f;
    float totalFrameMsSum = 0.0f;
    for (const auto& r : sResults) {
        totalFpsSum += r.fps;
        totalFrameMsSum += r.totalFrameMs;
    }
    int count = (int)sResults.size();
    out << "--- Overall ---" << std::endl;
    out << std::fixed << std::setprecision(2);
    out << "Average FPS:       " << (totalFpsSum / count) << std::endl;
    out << "Average Frame:     " << (totalFrameMsSum / count) << " ms" << std::endl;
    out << std::endl;

    // Per-scene results
    out << "--- Per-Scene Results ---" << std::endl;
    for (const auto& r : sResults) {
        out << std::endl;
        out << "Scene: " << r.name << std::endl;
        out << "  Frame Time:       " << std::fixed << std::setprecision(2) << r.totalFrameMs << " ms ("
            << std::setprecision(1) << r.fps << " FPS)" << std::endl;
        out << "  Actor Update:     " << std::setprecision(2) << r.actorUpdateMs << " ms" << std::endl;
        out << "  Actor Draw:       " << r.actorDrawMs << " ms" << std::endl;
        out << "  DL Process:       " << r.dlProcessMs << " ms" << std::endl;
        out << "  GFX Commands:     " << r.gfxCommandsMs << " ms" << std::endl;
        out << "  DL Triangles:     " << std::setprecision(0) << r.dlTriangles << std::endl;
        out << "  DL Commands:      " << r.dlCommands << std::endl;
        out << "  GL Draw Calls:    " << r.glDrawCalls << std::endl;
        out << "  GL Triangles:     " << r.glTriangles << std::endl;
        out << "  GL Avg Batch:     " << std::setprecision(1) << r.glAvgBatchSize << std::endl;
        out << "  GL Shader Sw:     " << std::setprecision(0) << r.glShaderSwitches << std::endl;
    }

    out.close();
    sLastExportPath = "Saved: " + filepath;
    sExportMsgTimer = 5.0f;
}

// ── ImGui window ───────────────────────────────────────────────────────

void BenchmarkWindow::InitElement() {
}

void BenchmarkWindow::UpdateElement() {
}

void BenchmarkWindow::DrawElement() {
    ImGui::Text("Automated Performance Benchmark");
    ImGui::Separator();
    ImGui::TextWrapped("Warps through a set of heavy scenes, collects profiler data for each, "
                       "and produces a deterministic performance report for comparison across builds.");
    ImGui::Spacing();

    bool isRunning = (sState != BENCH_IDLE && sState != BENCH_DONE);
    bool canStart = (gPlayState != NULL && !isRunning);

    if (!canStart) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Run Benchmark")) {
        sState = BENCH_STARTING;
    }
    if (!canStart) {
        ImGui::EndDisabled();
        if (gPlayState == NULL) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "Load a save file first");
        }
    }

    // Progress display
    if (isRunning) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Status: Running...");

        const char* stateStr = "Unknown";
        switch (sState) {
            case BENCH_STARTING:
                stateStr = "Initializing...";
                break;
            case BENCH_WARPING:
                stateStr = "Loading scene...";
                break;
            case BENCH_SETTLING:
                stateStr = "Stabilizing...";
                break;
            case BENCH_MEASURING:
                stateStr = "Measuring...";
                break;
            case BENCH_NEXT_SCENE:
                stateStr = "Next scene...";
                break;
            default:
                break;
        }

        ImGui::Text("Scene %d/%d: %s", sCurrentScene + 1, BENCHMARK_SCENE_COUNT,
                     (sCurrentScene < BENCHMARK_SCENE_COUNT) ? sBenchmarkScenes[sCurrentScene].name : "");
        ImGui::Text("Phase: %s", stateStr);

        if (sState == BENCH_SETTLING) {
            float pct = (float)sFrameCounter / sBenchmarkScenes[sCurrentScene].settleFrames;
            ImGui::ProgressBar(pct, ImVec2(-1, 0), "Settling");
        } else if (sState == BENCH_MEASURING) {
            float pct = (float)sFrameCounter / sBenchmarkScenes[sCurrentScene].measureFrames;
            ImGui::ProgressBar(pct, ImVec2(-1, 0), "Measuring");
        }

        // Overall progress
        float overallPct = (float)sResults.size() / BENCHMARK_SCENE_COUNT;
        ImGui::ProgressBar(overallPct, ImVec2(-1, 0), "Overall");
    }

    // Results table
    if (!sResults.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Results:");

        if (ImGui::BeginTable("BenchmarkResults", 7,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Scene");
            ImGui::TableSetupColumn("Frame ms");
            ImGui::TableSetupColumn("FPS");
            ImGui::TableSetupColumn("Actor Upd");
            ImGui::TableSetupColumn("Actor Draw");
            ImGui::TableSetupColumn("DL Process");
            ImGui::TableSetupColumn("GL Draws");
            ImGui::TableHeadersRow();

            for (const auto& r : sResults) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s", r.name);
                ImGui::TableNextColumn();
                // Color code: green < 16.67ms, yellow < 25ms, red >= 25ms
                ImVec4 color = (r.totalFrameMs < 16.67f)  ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f)
                               : (r.totalFrameMs < 25.0f) ? ImVec4(1.0f, 1.0f, 0.2f, 1.0f)
                                                           : ImVec4(1.0f, 0.3f, 0.2f, 1.0f);
                ImGui::TextColored(color, "%.2f", r.totalFrameMs);
                ImGui::TableNextColumn();
                ImGui::TextColored(color, "%.1f", r.fps);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", r.actorUpdateMs);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", r.actorDrawMs);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", r.dlProcessMs);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", r.glDrawCalls);
            }
            ImGui::EndTable();
        }

        // Export button
        ImGui::Spacing();
        if (ImGui::Button("Export Report")) {
            ExportBenchmarkReport();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(saves to app directory)");

        if (sExportMsgTimer > 0.0f) {
            sExportMsgTimer -= ImGui::GetIO().DeltaTime;
            ImGui::TextWrapped("%s", sLastExportPath.c_str());
        }
    }

    // Scene list reference
    if (ImGui::TreeNode("Benchmark Scenes")) {
        for (int i = 0; i < BENCHMARK_SCENE_COUNT; i++) {
            ImGui::BulletText("%s (settle: %d frames, measure: %d frames)", sBenchmarkScenes[i].name,
                              sBenchmarkScenes[i].settleFrames, sBenchmarkScenes[i].measureFrames);
        }
        ImGui::TreePop();
    }
}
