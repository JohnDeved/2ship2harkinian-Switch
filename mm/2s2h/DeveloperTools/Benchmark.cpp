#include "Benchmark.h"
#include "FrameProfiler.h"

#include <imgui.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cfloat>
#include <fstream>
#include <sstream>
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

struct BenchmarkScene {
    u16 entrance;
    const char* name;
    int settleFrames; // frames to wait for scene to fully load / stabilize
    int measureFrames; // frames to collect profiler data
};

// Full benchmark: 7 scenes covering a wide range of rendering workloads
static const BenchmarkScene sBenchmarkScenesFull[] = {
    { ENTRANCE(SOUTH_CLOCK_TOWN, 0), "South Clock Town", 120, 300 },
    { ENTRANCE(NORTH_CLOCK_TOWN, 0), "North Clock Town", 120, 300 },
    { ENTRANCE(EAST_CLOCK_TOWN, 0), "East Clock Town", 120, 300 },
    { ENTRANCE(TERMINA_FIELD, 0), "Termina Field", 120, 300 },
    { ENTRANCE(WOODFALL, 0), "Woodfall", 120, 300 },
    { ENTRANCE(GREAT_BAY_COAST, 0), "Great Bay Coast", 120, 300 },
    { ENTRANCE(IKANA_CANYON, 0), "Ikana Canyon", 120, 300 },
};
static constexpr int BENCHMARK_FULL_COUNT = sizeof(sBenchmarkScenesFull) / sizeof(sBenchmarkScenesFull[0]);

// Quick benchmark: 2 most demanding scenes for fast regression checks
static const BenchmarkScene sBenchmarkScenesQuick[] = {
    { ENTRANCE(EAST_CLOCK_TOWN, 0), "East Clock Town", 120, 300 },
    { ENTRANCE(GREAT_BAY_COAST, 0), "Great Bay Coast", 120, 300 },
};
static constexpr int BENCHMARK_QUICK_COUNT = sizeof(sBenchmarkScenesQuick) / sizeof(sBenchmarkScenesQuick[0]);

enum BenchmarkMode {
    BENCH_MODE_FULL,
    BENCH_MODE_QUICK,
};

// Active scene list (set when benchmark starts)
static BenchmarkMode sBenchMode = BENCH_MODE_FULL;
static const BenchmarkScene* sActiveScenes = sBenchmarkScenesFull;
static int sActiveSceneCount = BENCHMARK_FULL_COUNT;

// Frame time thresholds for color-coded results (ms)
static constexpr float BENCHMARK_TARGET_60FPS_MS = 1000.0f / 60.0f; // ~16.67ms
static constexpr float BENCHMARK_WARN_40FPS_MS = 1000.0f / 40.0f;   // 25.0ms

// ── Per-scene result storage ───────────────────────────────────────────
// Stores ALL profiler phases and counters for complete data capture.

struct BenchmarkResult {
    const char* name;
    float fpsAvg;
    float fpsMin;
    float fpsMax;
    float phases[PROFILE_PHASE_MAX];
    float counters[PROFILE_COUNTER_MAX];
    DLBufferStats bufferStats[PROFILE_DL_BUFFER_COUNT];
};

// ── State machine ──────────────────────────────────────────────────────

enum BenchmarkState {
    BENCH_IDLE,
    BENCH_STARTING,   // one-frame delay to init save context
    BENCH_WARPING,    // scene transition triggered, waiting for load
    BENCH_SETTLING,   // scene loaded, waiting for stabilization
    BENCH_MEASURING,  // collecting profiler data
    BENCH_DONE,       // all scenes measured, results ready
};

static BenchmarkState sState = BENCH_IDLE;
static int sCurrentScene = 0;
static int sFrameCounter = 0;
static bool sSceneReady = false;
static HOOK_ID sSceneInitHookId = 0;
static HOOK_ID sUpdateHookId = 0;
static bool sHooksRegistered = false;

// Saved clock state to restore after benchmark completes
static s32 sSavedTimeSpeed = 0;
static u16 sSavedTime = 0;
static bool sClockSaved = false;

static std::vector<BenchmarkResult> sResults;
static std::string sLastExportPath;
static float sExportMsgTimer = 0.0f;

// ── Baseline comparison ────────────────────────────────────────────────

struct BaselineData {
    std::string branch;
    std::string commit;
    std::string timestamp;
    std::string mode; // "Full" or "Quick"
    std::vector<BenchmarkResult> results;
    // Scene names stored separately for serialization (since BenchmarkResult::name is a const char*)
    std::vector<std::string> sceneNames;
    bool loaded = false;
};
static BaselineData sBaseline;
static std::string sBaselineMsgText;
static float sBaselineMsgTimer = 0.0f;

static const char* BASELINE_FILENAME = "benchmark_baseline.txt";

// Find matching baseline result for a scene name (returns nullptr if not found)
static const BenchmarkResult* FindBaselineForScene(const char* sceneName) {
    if (!sBaseline.loaded) {
        return nullptr;
    }
    for (size_t i = 0; i < sBaseline.results.size(); i++) {
        if (sBaseline.sceneNames[i] == sceneName) {
            return &sBaseline.results[i];
        }
    }
    return nullptr;
}

static void SaveBaseline() {
    if (sResults.empty()) {
        return;
    }

    std::string filepath = Ship::Context::GetPathRelativeToAppDirectory(BASELINE_FILENAME);
    std::ofstream out(filepath);
    if (!out.is_open()) {
        sBaselineMsgText = "ERROR: Could not write baseline";
        sBaselineMsgTimer = 5.0f;
        return;
    }

    std::string branch = (gGitBranch[0] != '\0') ? gGitBranch : "unknown";
    std::string commit = (gGitCommitHash[0] != '\0') ? gGitCommitHash : "unknown";

    time_t now = std::time(nullptr);
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &now);
#else
    localtime_r(&now, &tmBuf);
#endif
    char timeBuf[64];
    std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", &tmBuf);

    out << "BASELINE_V2" << std::endl;
    out << "branch=" << branch << std::endl;
    out << "commit=" << commit << std::endl;
    out << "timestamp=" << timeBuf << std::endl;
    out << "mode=" << (sBenchMode == BENCH_MODE_QUICK ? "Quick" : "Full") << std::endl;
    out << "scene_count=" << sResults.size() << std::endl;

    for (const auto& r : sResults) {
        out << "SCENE=" << r.name << std::endl;
        // Write all phases
        out << "phases=";
        for (int i = 0; i < PROFILE_PHASE_MAX; i++) {
            if (i > 0) out << ",";
            out << std::fixed << std::setprecision(6) << r.phases[i];
        }
        out << std::endl;
        // Write all counters
        out << "counters=";
        for (int i = 0; i < PROFILE_COUNTER_MAX; i++) {
            if (i > 0) out << ",";
            out << std::fixed << std::setprecision(6) << r.counters[i];
        }
        out << std::endl;
        out << "fps_stats=" << std::fixed << std::setprecision(6) << r.fpsAvg << "," << r.fpsMin << "," << r.fpsMax
            << std::endl;
    }

    out.close();

    // Also store in memory
    sBaseline.branch = branch;
    sBaseline.commit = commit;
    sBaseline.timestamp = timeBuf;
    sBaseline.mode = (sBenchMode == BENCH_MODE_QUICK ? "Quick" : "Full");
    sBaseline.results = sResults;
    sBaseline.sceneNames.clear();
    for (const auto& r : sResults) {
        sBaseline.sceneNames.push_back(r.name);
    }
    // Fix name pointers to point to our owned strings
    for (size_t i = 0; i < sBaseline.results.size(); i++) {
        sBaseline.results[i].name = sBaseline.sceneNames[i].c_str();
    }
    sBaseline.loaded = true;

    sBaselineMsgText = "Baseline saved (" + std::string(timeBuf) + ")";
    sBaselineMsgTimer = 5.0f;
}

static void LoadBaseline() {
    std::string filepath = Ship::Context::GetPathRelativeToAppDirectory(BASELINE_FILENAME);
    std::ifstream in(filepath);
    if (!in.is_open()) {
        sBaseline.loaded = false;
        return;
    }

    std::string line;
    // Check header
    if (!std::getline(in, line)) {
        sBaseline.loaded = false;
        return;
    }
    const bool isV2 = (line == "BASELINE_V2");
    if (line != "BASELINE_V1" && !isV2) {
        sBaseline.loaded = false;
        return;
    }

    sBaseline.results.clear();
    sBaseline.sceneNames.clear();
    sBaseline.branch.clear();
    sBaseline.commit.clear();
    sBaseline.timestamp.clear();
    sBaseline.mode.clear();

    try {
        while (std::getline(in, line)) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);

            if (key == "branch") {
                sBaseline.branch = val;
            } else if (key == "commit") {
                sBaseline.commit = val;
            } else if (key == "timestamp") {
                sBaseline.timestamp = val;
            } else if (key == "mode") {
                sBaseline.mode = val;
            } else if (key == "scene_count") {
                // Informational only; actual count derived from parsed scenes
            } else if (key == "SCENE") {
                sBaseline.sceneNames.push_back(val);
                BenchmarkResult result;
                memset(&result, 0, sizeof(result));
                result.name = nullptr; // set later

                // Read phases line
                if (std::getline(in, line) && line.substr(0, 7) == "phases=") {
                    std::istringstream ss(line.substr(7));
                    std::string token;
                    int idx = 0;
                    while (std::getline(ss, token, ',') && idx < PROFILE_PHASE_MAX) {
                        result.phases[idx++] = std::stof(token);
                    }
                }
                // Read counters line
                if (std::getline(in, line) && line.substr(0, 9) == "counters=") {
                    std::istringstream ss(line.substr(9));
                    std::string token;
                    int idx = 0;
                    while (std::getline(ss, token, ',') && idx < PROFILE_COUNTER_MAX) {
                        result.counters[idx++] = std::stof(token);
                    }
                }
                if (isV2) {
                    std::streampos beforeFpsLine = in.tellg();
                    if (std::getline(in, line)) {
                        if (line.substr(0, 10) == "fps_stats=") {
                            std::istringstream ss(line.substr(10));
                            std::string token;
                            if (std::getline(ss, token, ',')) {
                                result.fpsAvg = std::stof(token);
                            }
                            if (std::getline(ss, token, ',')) {
                                result.fpsMin = std::stof(token);
                            }
                            if (std::getline(ss, token, ',')) {
                                result.fpsMax = std::stof(token);
                            }
                        } else if (beforeFpsLine != std::streampos(-1)) {
                            in.seekg(beforeFpsLine);
                        }
                    }
                }

                sBaseline.results.push_back(result);
            }
        }
    } catch (...) {
        // Corrupted baseline file — discard partial data
        sBaseline.results.clear();
        sBaseline.sceneNames.clear();
        sBaseline.loaded = false;
        return;
    }

    // Fix name pointers
    for (size_t i = 0; i < sBaseline.results.size(); i++) {
        sBaseline.results[i].name = sBaseline.sceneNames[i].c_str();
    }

    sBaseline.loaded = !sBaseline.results.empty();
}

static void ClearBaseline() {
    sBaseline.results.clear();
    sBaseline.sceneNames.clear();
    sBaseline.loaded = false;

    // Also delete the file
    std::string filepath = Ship::Context::GetPathRelativeToAppDirectory(BASELINE_FILENAME);
    std::remove(filepath.c_str());

    sBaselineMsgText = "Baseline cleared";
    sBaselineMsgTimer = 3.0f;
}

// Delta formatting helper: returns colored text for a delta value
// For timing values (ms), negative = improvement (green), positive = regression (red)
static ImVec4 DeltaColor(float delta, bool lowerIsBetter = true) {
    if (lowerIsBetter) {
        if (delta < -0.5f) return ImVec4(0.2f, 1.0f, 0.2f, 1.0f);  // green = improved
        if (delta > 0.5f) return ImVec4(1.0f, 0.3f, 0.2f, 1.0f);   // red = regressed
    } else {
        if (delta > 0.5f) return ImVec4(0.2f, 1.0f, 0.2f, 1.0f);   // green = improved (higher is better)
        if (delta < -0.5f) return ImVec4(1.0f, 0.3f, 0.2f, 1.0f);  // red = regressed
    }
    return ImVec4(0.7f, 0.7f, 0.7f, 1.0f); // neutral
}

static const char* DeltaSign(float delta) {
    return (delta > 0.0f) ? "+" : "";
}

// Generic accumulator: captures all phases and counters
struct MeasureAccum {
    double phases[PROFILE_PHASE_MAX];
    double counters[PROFILE_COUNTER_MAX];
    double bufferStats[PROFILE_DL_BUFFER_COUNT][8]; // 8 fields per DLBufferStats
    double fpsSum;
    float fpsMin;
    float fpsMax;
    int fpsSampleCount;
    int frameCount;
};
static MeasureAccum sAccum;

static void ResetAccum() {
    memset(&sAccum, 0, sizeof(sAccum));
    sAccum.fpsMin = FLT_MAX;
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

// ── Start helper ───────────────────────────────────────────────────────

static void StartBenchmark(BenchmarkMode mode) {
    sBenchMode = mode;
    if (mode == BENCH_MODE_QUICK) {
        sActiveScenes = sBenchmarkScenesQuick;
        sActiveSceneCount = BENCHMARK_QUICK_COUNT;
    } else {
        sActiveScenes = sBenchmarkScenesFull;
        sActiveSceneCount = BENCHMARK_FULL_COUNT;
    }
    sState = BENCH_STARTING;
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
        // Restore clock state if benchmark was running when play state became NULL
        // (e.g., during a scene transition or shutdown) to avoid leaving the clock frozen
        if (sClockSaved) {
            R_TIME_SPEED = sSavedTimeSpeed;
            gSaveContext.save.time = sSavedTime;
            sClockSaved = false;
        }
        sState = BENCH_DONE;
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
            BenchmarkWarpToScene(sActiveScenes[sCurrentScene].entrance);
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
            // Save clock state on first entry so we can restore after benchmark
            if (!sClockSaved) {
                sSavedTimeSpeed = R_TIME_SPEED;
                sSavedTime = gSaveContext.save.time;
                sClockSaved = true;
            }
            // Freeze the in-game clock so game state (NPC schedules, actor load)
            // stays consistent regardless of tick rate. Without this, frame-ahead
            // makes the game tick faster, advancing MM's day cycle and loading
            // different NPCs — causing 40ms+ game logic swings between runs.
            R_TIME_SPEED = 0;
            gSaveContext.save.time = CLOCK_TIME(8, 0);

            sFrameCounter++;
            if (sFrameCounter >= sActiveScenes[sCurrentScene].settleFrames) {
                sFrameCounter = 0;
                ResetAccum();
                sState = BENCH_MEASURING;
            }
            break;
        }
        case BENCH_MEASURING: {
            R_TIME_SPEED = 0;
            gSaveContext.save.time = CLOCK_TIME(8, 0);
            // Collect ALL profiler phases and counters each frame
            for (int i = 0; i < PROFILE_PHASE_MAX; i++) {
                sAccum.phases[i] += FrameProfiler_GetPhaseAvgMs((ProfilePhase)i);
            }
            for (int i = 0; i < PROFILE_COUNTER_MAX; i++) {
                sAccum.counters[i] += FrameProfiler_GetCounterAvg((ProfileCounter)i);
            }
            const FrameProfilerRenderMetrics fpsMetrics =
                FrameProfiler_MakeRenderMetrics(FrameProfiler_GetPhaseAvgMs(PROFILE_PHASE_TOTAL_FRAME),
                                                FrameProfiler_GetCounterAvg(PROFILE_COUNTER_DL_ITERATIONS));
            if (fpsMetrics.renderFps > 0.01f) {
                sAccum.fpsSum += fpsMetrics.renderFps;
                sAccum.fpsMin = std::min(sAccum.fpsMin, fpsMetrics.renderFps);
                sAccum.fpsMax = std::max(sAccum.fpsMax, fpsMetrics.renderFps);
                sAccum.fpsSampleCount++;
            }
            for (int b = 0; b < PROFILE_DL_BUFFER_COUNT; b++) {
                DLBufferStats bs = FrameProfiler_GetBufferStats(b);
                sAccum.bufferStats[b][0] += bs.commands;
                sAccum.bufferStats[b][1] += bs.triangles;
                sAccum.bufferStats[b][2] += bs.vertices;
                sAccum.bufferStats[b][3] += bs.texLoads;
                sAccum.bufferStats[b][4] += bs.mtxLoads;
                sAccum.bufferStats[b][5] += bs.pipeSyncs;
                sAccum.bufferStats[b][6] += bs.subcalls;
                sAccum.bufferStats[b][7] += bs.setCombine;
            }
            sAccum.frameCount++;

            sFrameCounter++;
            if (sFrameCounter >= sActiveScenes[sCurrentScene].measureFrames) {
                // Store averaged results for this scene
                int n = sAccum.frameCount > 0 ? sAccum.frameCount : 1;
                BenchmarkResult result;
                result.name = sActiveScenes[sCurrentScene].name;
                FrameProfilerRenderMetrics avgMetrics = FrameProfiler_MakeRenderMetrics(
                    (float)(sAccum.phases[PROFILE_PHASE_TOTAL_FRAME] / n),
                    (float)(sAccum.counters[PROFILE_COUNTER_DL_ITERATIONS] / n));
                if (sAccum.fpsSampleCount > 0) {
                    result.fpsAvg = (float)(sAccum.fpsSum / sAccum.fpsSampleCount);
                    result.fpsMin = (sAccum.fpsMin == FLT_MAX) ? result.fpsAvg : sAccum.fpsMin;
                    result.fpsMax = sAccum.fpsMax;
                } else {
                    result.fpsAvg = avgMetrics.renderFps;
                    result.fpsMin = result.fpsAvg;
                    result.fpsMax = result.fpsAvg;
                }
                for (int i = 0; i < PROFILE_PHASE_MAX; i++) {
                    result.phases[i] = (float)(sAccum.phases[i] / n);
                }
                for (int i = 0; i < PROFILE_COUNTER_MAX; i++) {
                    result.counters[i] = (float)(sAccum.counters[i] / n);
                }
                int sumCmds = 0, sumTris = 0, sumVerts = 0, sumTex = 0;
                int sumMtx = 0, sumSync = 0, sumSub = 0, sumComb = 0;
                for (int b = 0; b < PROFILE_DL_BUFFER_COUNT; b++) {
                    result.bufferStats[b].commands = (int)(sAccum.bufferStats[b][0] / n + 0.5);
                    result.bufferStats[b].triangles = (int)(sAccum.bufferStats[b][1] / n + 0.5);
                    result.bufferStats[b].vertices = (int)(sAccum.bufferStats[b][2] / n + 0.5);
                    result.bufferStats[b].texLoads = (int)(sAccum.bufferStats[b][3] / n + 0.5);
                    result.bufferStats[b].mtxLoads = (int)(sAccum.bufferStats[b][4] / n + 0.5);
                    result.bufferStats[b].pipeSyncs = (int)(sAccum.bufferStats[b][5] / n + 0.5);
                    result.bufferStats[b].subcalls = (int)(sAccum.bufferStats[b][6] / n + 0.5);
                    result.bufferStats[b].setCombine = (int)(sAccum.bufferStats[b][7] / n + 0.5);
                    sumCmds += result.bufferStats[b].commands;
                    sumTris += result.bufferStats[b].triangles;
                    sumVerts += result.bufferStats[b].vertices;
                    sumTex += result.bufferStats[b].texLoads;
                    sumMtx += result.bufferStats[b].mtxLoads;
                    sumSync += result.bufferStats[b].pipeSyncs;
                    sumSub += result.bufferStats[b].subcalls;
                    sumComb += result.bufferStats[b].setCombine;
                }
                // Keep top-line DL totals aligned with per-buffer breakdown in benchmark reports.
                result.counters[PROFILE_COUNTER_DL_COMMANDS] = (float)sumCmds;
                result.counters[PROFILE_COUNTER_DL_TRIANGLES] = (float)sumTris;
                result.counters[PROFILE_COUNTER_DL_VERTICES] = (float)sumVerts;
                result.counters[PROFILE_COUNTER_DL_TEX_LOADS] = (float)sumTex;
                result.counters[PROFILE_COUNTER_DL_MTX_LOADS] = (float)sumMtx;
                result.counters[PROFILE_COUNTER_DL_PIPE_SYNCS] = (float)sumSync;
                result.counters[PROFILE_COUNTER_DL_SUBCALLS] = (float)sumSub;
                result.counters[PROFILE_COUNTER_DL_SETCOMBINE] = (float)sumComb;
                sResults.push_back(result);

                sCurrentScene++;
                if (sCurrentScene >= sActiveSceneCount) {
                    sState = BENCH_DONE;
                    // Restore clock state that was frozen during benchmark
                    if (sClockSaved) {
                        R_TIME_SPEED = sSavedTimeSpeed;
                        gSaveContext.save.time = sSavedTime;
                        sClockSaved = false;
                    }
                } else {
                    sState = BENCH_WARPING;
                    sSceneReady = false;
                    BenchmarkWarpToScene(sActiveScenes[sCurrentScene].entrance);
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
    LoadBaseline();
}

RegisterShipInitFunc initFuncBenchmark(RegisterBenchmark, {});


// ── Helper: get FPS from a result ──────────────────────────────────────

static FrameProfilerRenderMetrics ResultRenderMetrics(const BenchmarkResult& r) {
    return FrameProfiler_MakeRenderMetrics(r.phases[PROFILE_PHASE_TOTAL_FRAME],
                                           r.counters[PROFILE_COUNTER_DL_ITERATIONS]);
}

static float ResultAvgFps(const BenchmarkResult& r) {
    return (r.fpsAvg > 0.01f) ? r.fpsAvg : ResultRenderMetrics(r).renderFps;
}

static float ResultMinFps(const BenchmarkResult& r) {
    return (r.fpsMin > 0.01f) ? r.fpsMin : ResultAvgFps(r);
}

static float ResultMaxFps(const BenchmarkResult& r) {
    return (r.fpsMax > 0.01f) ? r.fpsMax : ResultAvgFps(r);
}

static float ResultRenderFrameMs(const BenchmarkResult& r) {
    return ResultRenderMetrics(r).renderFrameMs;
}

static float ResultTickFps(const BenchmarkResult& r) {
    return ResultRenderMetrics(r).tickFps;
}

// ── Phase / counter name tables ────────────────────────────────────────

static const char* sPhaseNames[PROFILE_PHASE_MAX] = {
    "Collision AT", "Collision OC", "Collision Damage", "Actor Update", "Effects",    "Actor Draw",   "Scene Draw",
    "Play Update",  "Play Draw",    "Frame Interp",     "Audio Wait",   "DL Process", "GFX Commands", "Total Frame",
};

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
    out << "Mode: " << (sBenchMode == BENCH_MODE_QUICK ? "Quick" : "Full") << std::endl;
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
    int count = (int)sResults.size();
    float totalFpsAvgSum = 0.0f;
    float totalTickFpsSum = 0.0f;
    float totalFrameMsSum = 0.0f;
    float fpsMin = FLT_MAX;
    float fpsMax = 0.0f;
    for (const auto& r : sResults) {
        const float sceneFpsAvg = ResultAvgFps(r);
        totalFpsAvgSum += sceneFpsAvg;
        totalTickFpsSum += ResultTickFps(r);
        totalFrameMsSum += ResultRenderFrameMs(r);
        fpsMin = std::min(fpsMin, ResultMinFps(r));
        fpsMax = std::max(fpsMax, ResultMaxFps(r));
    }
    if (fpsMin == FLT_MAX) {
        fpsMin = 0.0f;
    }
    out << "--- Overall ---" << std::endl;
    out << std::fixed << std::setprecision(2);
    out << "Rendered FPS (avg/min/max): " << (totalFpsAvgSum / count) << " / " << fpsMin << " / " << fpsMax
        << std::endl;
    out << "Average Tick FPS:   " << (totalTickFpsSum / count) << std::endl;
    out << "Average Frame:      " << (totalFrameMsSum / count) << " ms" << std::endl;
    out << std::endl;

    // Baseline comparison
    if (sBaseline.loaded) {
        out << "--- Baseline Comparison ---" << std::endl;
        std::string baseCommit = sBaseline.commit.size() > 7 ? sBaseline.commit.substr(0, 7) : sBaseline.commit;
        out << "Baseline: " << sBaseline.branch << " @ " << baseCommit
            << " (" << sBaseline.timestamp << ", " << sBaseline.mode << ")" << std::endl;
        out << std::endl;
        out << "  Scene                     Frame ms  Baseline   Delta    Delta %" << std::endl;
        out << "  -------------------------------------------------------------------------" << std::endl;
        for (const auto& r : sResults) {
            float frameMs = ResultRenderFrameMs(r);
            const BenchmarkResult* base = FindBaselineForScene(r.name);
            if (base) {
                float baseMs = ResultRenderFrameMs(*base);
                float delta = frameMs - baseMs;
                float deltaPct = (baseMs > 0.01f) ? (delta / baseMs * 100.0f) : 0.0f;
                const char* indicator = (delta < -0.5f) ? " FASTER" : (delta > 0.5f) ? " SLOWER" : " ~same";
                char line[256];
                snprintf(line, sizeof(line), "  %-24s %7.2f  %7.2f  %+7.2f  %+5.1f%%%s",
                         r.name, frameMs, baseMs, delta, deltaPct, indicator);
                out << line << std::endl;
            } else {
                char line[256];
                snprintf(line, sizeof(line), "  %-24s %7.2f     -        -       -   (no baseline)",
                         r.name, frameMs);
                out << line << std::endl;
            }
        }
        out << std::endl;
    }

    // Per-scene detailed results
    out << "--- Per-Scene Results ---" << std::endl;
    static const char* sBufNames[] = { "OPA (opaque)", "XLU (translucent)", "Overlay", "Work", "Debug" };

    for (const auto& r : sResults) {
        const FrameProfilerRenderMetrics renderMetrics = ResultRenderMetrics(r);
        const float totalMs = renderMetrics.totalMs;
        const float dlIter = renderMetrics.dlIterations;
        const float renderFrameMs = renderMetrics.renderFrameMs;
        const float tickFps = renderMetrics.tickFps;
        const float fpsAvg = ResultAvgFps(r);
        const float fpsMinScene = ResultMinFps(r);
        const float fpsMaxScene = ResultMaxFps(r);

        out << std::endl;
        out << "========================================" << std::endl;
        out << "Scene: " << r.name << std::endl;
        out << "========================================" << std::endl;
        out << std::endl;

        // Summary
        out << "  Summary:" << std::endl;
        out << "    Total Update Time:          " << std::fixed << std::setprecision(2) << totalMs
            << " ms (" << std::setprecision(0) << dlIter << " DL iterations)" << std::endl;
        out << "    Game Tick:                  " << std::setprecision(2) << totalMs << " ms ("
            << std::setprecision(1) << tickFps << " FPS-equivalent)" << std::endl;
        out << "    Rendered FPS (avg/min/max): " << std::setprecision(1) << fpsAvg << " / " << fpsMinScene << " / "
            << fpsMaxScene << std::endl;
        out << "    Per Rendered Frame:         " << std::setprecision(2) << renderFrameMs << " ms" << std::endl;
        if (renderFrameMs > BENCHMARK_TARGET_60FPS_MS) {
            float overhead = ((renderFrameMs / BENCHMARK_TARGET_60FPS_MS) - 1.0f) * 100.0f;
            out << "    Performance:                 " << std::setprecision(1) << overhead << "% over budget" << std::endl;
        } else {
            float headroom = ((BENCHMARK_TARGET_60FPS_MS - renderFrameMs) / BENCHMARK_TARGET_60FPS_MS) * 100.0f;
            out << "    Performance:                 " << std::setprecision(1) << headroom << "% headroom" << std::endl;
        }
        out << std::endl;

        // System telemetry (clocks/memory/thermals/power).
        float sysRamUsage = r.counters[PROFILE_COUNTER_SYS_RAM_USAGE_PCT];
        float sysRamUsedMb = r.counters[PROFILE_COUNTER_SYS_RAM_USED_MB];
        float sysRamTotalMb = r.counters[PROFILE_COUNTER_SYS_RAM_TOTAL_MB];
        float sysCpuClockMhz = r.counters[PROFILE_COUNTER_SYS_CPU_CLOCK_MHZ];
        float sysGpuClockMhz = r.counters[PROFILE_COUNTER_SYS_GPU_CLOCK_MHZ];
        float sysEmcClockMhz = r.counters[PROFILE_COUNTER_SYS_EMC_CLOCK_MHZ];
        float sysSocTempC = r.counters[PROFILE_COUNTER_SYS_SOC_TEMP_C];
        float sysPcbTempC = r.counters[PROFILE_COUNTER_SYS_PCB_TEMP_C];
        float sysSkinTempC = r.counters[PROFILE_COUNTER_SYS_SKIN_TEMP_C];
        float sysBatteryTempC = r.counters[PROFILE_COUNTER_SYS_BATTERY_TEMP_C];
        float sysBatteryChargePct = r.counters[PROFILE_COUNTER_SYS_BATTERY_CHARGE_PCT];
        float sysBatteryAgePct = r.counters[PROFILE_COUNTER_SYS_BATTERY_AGE_PCT];
        float sysBatteryVoltageMv = r.counters[PROFILE_COUNTER_SYS_BATTERY_VOLTAGE_MV];
        float sysChargerType = r.counters[PROFILE_COUNTER_SYS_CHARGER_TYPE];
        float sysChargerVoltageLimitMv = r.counters[PROFILE_COUNTER_SYS_CHARGER_VOLTAGE_LIMIT_MV];
        float sysChargerCurrentLimitMa = r.counters[PROFILE_COUNTER_SYS_CHARGER_CURRENT_LIMIT_MA];

        bool hasSystemTelemetry =
            sysRamTotalMb > 0.01f || sysCpuClockMhz > 0.01f || sysGpuClockMhz > 0.01f || sysEmcClockMhz > 0.01f ||
            sysSocTempC > 0.01f || sysPcbTempC > 0.01f || sysSkinTempC > 0.01f || sysBatteryChargePct > 0.01f ||
            sysBatteryVoltageMv > 0.01f || sysChargerVoltageLimitMv > 0.01f || sysChargerCurrentLimitMa > 0.01f;
        if (hasSystemTelemetry) {
            out << "  System Telemetry:" << std::endl;
            if (sysRamTotalMb > 0.01f) {
                out << "    RAM Usage:                   " << std::setprecision(0) << sysRamUsedMb << " / "
                    << sysRamTotalMb << " MB (" << std::setprecision(1) << sysRamUsage << "%)" << std::endl;
            }
            if (sysCpuClockMhz > 0.01f || sysGpuClockMhz > 0.01f || sysEmcClockMhz > 0.01f) {
                out << "    Clocks (CPU/GPU/EMC):        " << std::setprecision(0) << sysCpuClockMhz << " / "
                    << sysGpuClockMhz << " / " << sysEmcClockMhz << " MHz" << std::endl;
            }
            if (sysSocTempC > 0.01f || sysPcbTempC > 0.01f || sysSkinTempC > 0.01f) {
                out << "    Temps (SoC/PCB/Skin):        " << std::setprecision(1) << sysSocTempC << " / "
                    << sysPcbTempC << " / " << sysSkinTempC << " C" << std::endl;
            }
            if (sysBatteryChargePct > 0.01f || sysBatteryVoltageMv > 0.01f) {
                out << "    Battery (chg/age/temp):      " << std::setprecision(1) << sysBatteryChargePct << "% / "
                    << sysBatteryAgePct << "% / " << sysBatteryTempC << " C" << std::endl;
                out << "    Battery Voltage:             " << std::setprecision(0) << sysBatteryVoltageMv << " mV"
                    << std::endl;
            }
            if (sysChargerVoltageLimitMv > 0.01f || sysChargerCurrentLimitMa > 0.01f || sysChargerType > 0.01f) {
                out << "    Charger:                     " << FrameProfiler_ChargerTypeToString(sysChargerType) << " ("
                    << std::setprecision(0) << sysChargerVoltageLimitMv << " mV / "
                    << sysChargerCurrentLimitMa << " mA limit)" << std::endl;
            }
            out << std::endl;
        }

        // Per-phase breakdown
        out << "  Per-Phase Breakdown:" << std::endl;
        for (int i = 0; i < PROFILE_PHASE_MAX; i++) {
            float ms = r.phases[i];
            float pct = (totalMs > 0.01f) ? (ms / totalMs * 100.0f) : 0.0f;
            char line[256];
            snprintf(line, sizeof(line), "    %-24s %6.2f ms (%5.1f%%)", sPhaseNames[i], ms, pct);
            out << line << std::endl;
        }
        out << std::endl;

        // Display List statistics
        float dlCmds = r.counters[PROFILE_COUNTER_DL_COMMANDS];
        float tris = r.counters[PROFILE_COUNTER_DL_TRIANGLES];
        float verts = r.counters[PROFILE_COUNTER_DL_VERTICES];
        float texLoads = r.counters[PROFILE_COUNTER_DL_TEX_LOADS];
        float mtxLoads = r.counters[PROFILE_COUNTER_DL_MTX_LOADS];
        float pipeSyncs = r.counters[PROFILE_COUNTER_DL_PIPE_SYNCS];
        float subcalls = r.counters[PROFILE_COUNTER_DL_SUBCALLS];
        float setCombine = r.counters[PROFILE_COUNTER_DL_SETCOMBINE];

        float dlReplayIters = r.counters[PROFILE_COUNTER_DL_REPLAY_ITERATIONS];
        float dlReplayFallbacks = r.counters[PROFILE_COUNTER_DL_REPLAY_FALLBACKS];
        float dlReplayBranchZ = r.counters[PROFILE_COUNTER_DL_REPLAY_BRANCHZ_FRAMES];
        float dlReplayCooldown = r.counters[PROFILE_COUNTER_DL_REPLAY_COOLDOWN_SKIPS];

        out << "  Display List Statistics:" << std::endl;
        out << "    DL Iterations:              " << std::setprecision(0) << dlIter << std::endl;
        if (dlReplayIters > 0.0f) {
            out << "    DL Replay Iterations:       " << std::setprecision(0) << dlReplayIters << std::endl;
            out << "    DL Replay Fallbacks:        " << std::setprecision(0) << dlReplayFallbacks << std::endl;
            out << "    DL Replay Branch-Z Blocks:  " << std::setprecision(0) << dlReplayBranchZ << std::endl;
            out << "    DL Replay Cooldown Skips:   " << std::setprecision(0) << dlReplayCooldown << std::endl;
        }
        out << "    Total Commands:             " << std::setprecision(0) << dlCmds << std::endl;
        out << "    Triangles (DL):             " << std::setprecision(0) << tris << std::endl;
        out << "    Vertices:                   " << std::setprecision(0) << verts << std::endl;
        out << "    Texture Loads:              " << std::setprecision(0) << texLoads << std::endl;
        out << "    Matrix Loads:               " << std::setprecision(0) << mtxLoads << std::endl;
        out << "    Pipe Syncs:                 " << std::setprecision(0) << pipeSyncs << std::endl;
        out << "    DL Subcalls:                " << std::setprecision(0) << subcalls << std::endl;
        out << "    SetCombine:                 " << std::setprecision(0) << setCombine << std::endl;
        out << std::endl;

        // Backend counters
        float glDrawCalls = r.counters[PROFILE_COUNTER_GL_DRAW_CALLS];
        float glBatchFlushes = r.counters[PROFILE_COUNTER_GL_BATCH_FLUSHES];
        float glBufferFullFlushes = r.counters[PROFILE_COUNTER_GL_BUFFER_FULL_FLUSHES];
        float glShaderSwitches = r.counters[PROFILE_COUNTER_GL_SHADER_SWITCHES];
        float glShaderCompiles = r.counters[PROFILE_COUNTER_GL_SHADER_COMPILATIONS];
        float glTextureBinds = r.counters[PROFILE_COUNTER_GL_TEXTURE_BINDS];
        float glTextureMisses = r.counters[PROFILE_COUNTER_GL_TEXTURE_CACHE_MISSES];
        float glVerts = r.counters[PROFILE_COUNTER_GL_VERTICES_SUBMITTED];
        float glTris = r.counters[PROFILE_COUNTER_GL_TRIANGLES_SUBMITTED];
        // Derive avg batch from authoritative totals. The raw AVG counter can be
        // inflated when multiple DL iterations contribute within one game tick.
        float glAvgBatch = (glDrawCalls > 0.5f) ? (glTris / glDrawCalls) : 0.0f;

        out << "  Fast3D Backend (OpenGL):" << std::endl;
        out << "    GL Draw Calls:              " << std::setprecision(0) << glDrawCalls << std::endl;
        float glBufferFullFlushesClamped = std::clamp(glBufferFullFlushes, 0.0f, glBatchFlushes);
        float glStateFlushesFromBatch = std::max(0.0f, glBatchFlushes - glBufferFullFlushesClamped);
        out << "    GL Batch Flushes:           " << std::setprecision(0) << glBatchFlushes
            << " (state: " << glStateFlushesFromBatch << ", buf-full: " << glBufferFullFlushesClamped
            << ")" << std::endl;
        out << "    GL Shader Switches:         " << std::setprecision(0) << glShaderSwitches
            << " (compiles: " << glShaderCompiles << ")" << std::endl;
        out << "    GL Texture Binds:           " << std::setprecision(0) << glTextureBinds
            << " (cache misses: " << glTextureMisses << ")" << std::endl;
        out << "    GL Triangles Submitted:     " << std::setprecision(0) << glTris << std::endl;
        out << "    GL Vertices Submitted:      " << std::setprecision(0) << glVerts << std::endl;
        out << "    GL Avg Batch Size:          " << std::setprecision(1) << glAvgBatch << " tris/draw" << std::endl;
        out << std::endl;

        // Timing breakdown
        float glTimeTotal = r.counters[PROFILE_COUNTER_GL_TIME_TOTAL_MS];
        float glTimeDispatch = r.counters[PROFILE_COUNTER_GL_TIME_DISPATCH_MS];
        float glTimeTri = r.counters[PROFILE_COUNTER_GL_TIME_TRI_MS];
        float glTimeTex = r.counters[PROFILE_COUNTER_GL_TIME_TEX_MS];
        float glTimeShader = r.counters[PROFILE_COUNTER_GL_TIME_SHADER_MS];
        float glTimeDraw = r.counters[PROFILE_COUNTER_GL_TIME_DRAW_MS];
        float glTimeVboUpload = r.counters[PROFILE_COUNTER_GL_TIME_VBO_UPLOAD_MS];
        float glTimeGlDraw = r.counters[PROFILE_COUNTER_GL_TIME_GL_DRAW_MS];
        float glTimeVtx = r.counters[PROFILE_COUNTER_GL_TIME_VTX_MS];
        float glTimeMtx = r.counters[PROFILE_COUNTER_GL_TIME_MTX_MS];
        float glTimeDepth = r.counters[PROFILE_COUNTER_GL_TIME_DEPTH_MS];
        float glTimeSetup = r.counters[PROFILE_COUNTER_GL_TIME_SETUP_MS];

        out << "  Fast3D Timing Breakdown:" << std::endl;
        out << "    Total:                      " << std::setprecision(2) << glTimeTotal << " ms" << std::endl;
        float glTimeTriExcl = glTimeTri > glTimeDraw ? glTimeTri - glTimeDraw : 0.0f;
        float glTimeDrawOther =
            glTimeDraw > (glTimeVboUpload + glTimeGlDraw) ? glTimeDraw - glTimeVboUpload - glTimeGlDraw : 0.0f;
        out << "    Triangle Processing:        " << std::setprecision(2) << glTimeTri << " ms" << std::endl;
        out << "      Per-vertex work:          " << std::setprecision(2) << glTimeTriExcl << " ms" << std::endl;
        out << "      Draw Submit:              " << std::setprecision(2) << glTimeDraw << " ms" << std::endl;
        out << "        VBO Upload:             " << std::setprecision(2) << glTimeVboUpload << " ms" << std::endl;
        out << "        glDrawArrays:           " << std::setprecision(2) << glTimeGlDraw << " ms" << std::endl;
        out << "        State/Uniform setup:    " << std::setprecision(2) << glTimeDrawOther << " ms" << std::endl;
        out << "    Vertex Transform:           " << std::setprecision(2) << glTimeVtx << " ms" << std::endl;
        out << "    Texture Setup:              " << std::setprecision(2) << glTimeTex << " ms" << std::endl;
        out << "    Matrix Operations:          " << std::setprecision(2) << glTimeMtx << " ms" << std::endl;
        out << "    Dispatch (command walk):    " << std::setprecision(2) << glTimeDispatch << " ms" << std::endl;

        // Dispatch handler breakdown
        float glTimeTexLoading = r.counters[PROFILE_COUNTER_GL_TIME_TEXTURE_LOADING_MS];
        float glTimeRectDraw = r.counters[PROFILE_COUNTER_GL_TIME_RECT_DRAWING_MS];
        float glTimeDlOps = r.counters[PROFILE_COUNTER_GL_TIME_DL_OPS_MS];
        float glTimeCombSetup = r.counters[PROFILE_COUNTER_GL_TIME_COMBINER_SETUP_MS];
        float glTimeFbOps = r.counters[PROFILE_COUNTER_GL_TIME_FRAMEBUFFER_OPS_MS];
        float glTimeHandlersAccounted = glTimeTexLoading + glTimeRectDraw + glTimeDlOps + glTimeCombSetup + glTimeFbOps;
        float glTimeDispatchOther = glTimeDispatch > glTimeHandlersAccounted ? glTimeDispatch - glTimeHandlersAccounted : 0.0f;
        out << "      Texture Loading:          " << std::setprecision(2) << glTimeTexLoading << " ms" << std::endl;
        out << "      Rectangle Drawing:        " << std::setprecision(2) << glTimeRectDraw << " ms" << std::endl;
        out << "      Display List Ops:         " << std::setprecision(2) << glTimeDlOps << " ms" << std::endl;
        out << "      Combiner Setup:           " << std::setprecision(2) << glTimeCombSetup << " ms" << std::endl;
        out << "      Framebuffer Ops:          " << std::setprecision(2) << glTimeFbOps << " ms" << std::endl;
        out << "      Other (trivial cmds):     " << std::setprecision(2) << glTimeDispatchOther << " ms" << std::endl;

        out << "    Pixel Depth Readback:       " << std::setprecision(2) << glTimeDepth << " ms" << std::endl;
        out << "    Frame Setup:                " << std::setprecision(2) << glTimeSetup << " ms" << std::endl;
        out << "    Shader Compile/Switch:      " << std::setprecision(2) << glTimeShader << " ms" << std::endl;
        out << std::endl;

        // Time distribution percentages
        if (glTimeTotal > 0.01f) {
            out << "  Time Distribution (% of Fast3D total):" << std::endl;
            out << "    Triangle Processing:        " << std::setprecision(1) << (glTimeTri / glTimeTotal * 100.0f) << "%" << std::endl;
            out << "      Per-vertex work:          " << (glTimeTriExcl / glTimeTotal * 100.0f) << "%" << std::endl;
            out << "      Draw Submit:              " << (glTimeDraw / glTimeTotal * 100.0f) << "%" << std::endl;
            out << "        VBO Upload:             " << (glTimeVboUpload / glTimeTotal * 100.0f) << "%" << std::endl;
            out << "        glDrawArrays:           " << (glTimeGlDraw / glTimeTotal * 100.0f) << "%" << std::endl;
            out << "    Vertex Transform:           " << (glTimeVtx / glTimeTotal * 100.0f) << "%" << std::endl;
            out << "    Texture Setup:              " << (glTimeTex / glTimeTotal * 100.0f) << "%" << std::endl;
            out << "    Dispatch:                   " << (glTimeDispatch / glTimeTotal * 100.0f) << "%" << std::endl;
            out << std::endl;
        }

        // CPU vs GL driver time
        float glDriverTime = glTimeVboUpload + glTimeGlDraw + glTimeSetup + glTimeDepth;
        float cpuTime = glTimeTotal > glDriverTime ? glTimeTotal - glDriverTime : 0.0f;
        out << "  CPU vs GL Driver:" << std::endl;
        out << "    GL Driver:                  " << std::setprecision(2) << glDriverTime << " ms";
        if (glTimeTotal > 0.01f)
            out << " (" << std::setprecision(1) << (glDriverTime / glTimeTotal * 100.0f) << "%)";
        out << std::endl;
        out << "    CPU Processing:             " << std::setprecision(2) << cpuTime << " ms";
        if (glTimeTotal > 0.01f)
            out << " (" << std::setprecision(1) << (cpuTime / glTimeTotal * 100.0f) << "%)";
        out << std::endl;
        out << std::endl;

        // Flush cause breakdown
        float flushTexture = r.counters[PROFILE_COUNTER_GL_FLUSH_CAUSE_TEXTURE];
        float flushSampler = r.counters[PROFILE_COUNTER_GL_FLUSH_CAUSE_SAMPLER];
        float flushShader = r.counters[PROFILE_COUNTER_GL_FLUSH_CAUSE_SHADER];
        float flushAlpha = r.counters[PROFILE_COUNTER_GL_FLUSH_CAUSE_ALPHA];
        float flushDepthVp = r.counters[PROFILE_COUNTER_GL_FLUSH_CAUSE_DEPTH_VIEWPORT];
        float flushCombiner = r.counters[PROFILE_COUNTER_GL_FLUSH_CAUSE_COMBINER];
        float texReloadSkips = r.counters[PROFILE_COUNTER_GL_TEXTURE_RELOAD_SKIPS];

        out << "  Flush Cause Breakdown:" << std::endl;
        out << "    Texture change:             " << std::setprecision(0) << flushTexture << std::endl;
        out << "    Shader switch:              " << flushShader << std::endl;
        out << "    Sampler params:             " << flushSampler << std::endl;
        out << "    Alpha blend:                " << flushAlpha << std::endl;
        out << "    Depth/VP/Scissor:           " << flushDepthVp << std::endl;
        out << "    New combiner:               " << flushCombiner << std::endl;
        out << "    Tex reload skips (saved):   " << texReloadSkips << std::endl;
        out << std::endl;

        // Batch size histogram
        float batchHist[5];
        float totalDrawsHist = 0.0f;
        for (int b = 0; b < 5; b++) {
            batchHist[b] = r.counters[PROFILE_COUNTER_GL_BATCH_HIST_0 + b];
            totalDrawsHist += batchHist[b];
        }
        float maxBatchSeen = r.counters[PROFILE_COUNTER_GL_MAX_BATCH_SIZE];
        // Sanity clamp impossible max values against non-empty histogram buckets.
        // This avoids contradictory output such as "129+ bucket: 0" with max > 128.
        const float histEps = 0.001f;
        if (batchHist[4] <= histEps && maxBatchSeen > 128.0f) maxBatchSeen = 128.0f;
        if (batchHist[3] <= histEps && maxBatchSeen > 32.0f) maxBatchSeen = 32.0f;
        if (batchHist[2] <= histEps && maxBatchSeen > 8.0f) maxBatchSeen = 8.0f;
        if (batchHist[1] <= histEps && maxBatchSeen > 2.0f) maxBatchSeen = 2.0f;
        if (batchHist[0] <= histEps && maxBatchSeen > 0.0f) maxBatchSeen = 0.0f;
        static const char* bucketLabels[] = { "1-2 tris", "3-8 tris", "9-32 tris", "33-128 tris", "129+ tris" };
        out << "  Batch Size Distribution:" << std::endl;
        for (int b = 0; b < 5; b++) {
            float pct = (totalDrawsHist > 0.5f) ? (batchHist[b] / totalDrawsHist * 100.0f) : 0.0f;
            int barLen = (int)(pct / 2.0f + 0.5f);
            if (barLen > 40) barLen = 40;
            char bar[42];
            for (int i = 0; i < barLen; i++)
                bar[i] = '#';
            bar[barLen] = '\0';
            out << "    " << std::setw(12) << std::left << bucketLabels[b] << " " << std::setw(5) << std::right
                << std::setprecision(0) << batchHist[b] << " draws (" << std::setw(4) << std::setprecision(1) << pct
                << "%) " << bar << std::endl;
        }
        out << "    Max batch size:             " << std::setprecision(0) << maxBatchSeen << " tris" << std::endl;
        out << std::endl;

        // Efficiency metrics
        float drawsPerShader = (glShaderSwitches > 0.5f) ? (glDrawCalls / glShaderSwitches) : 0.0f;
        float cacheMissRate = (glTextureBinds > 0.5f) ? (glTextureMisses / glTextureBinds * 100.0f) : 0.0f;
        float emptyFlushes = std::max(0.0f, glBatchFlushes - glDrawCalls);
        float emptyPct = (glBatchFlushes > 0.5f) ? (emptyFlushes / glBatchFlushes * 100.0f) : 0.0f;

        out << "  Efficiency Metrics:" << std::endl;
        out << "    Draws per shader:           " << std::setprecision(1) << drawsPerShader << std::endl;
        out << "    Tex cache miss rate:        " << std::setprecision(1) << cacheMissRate << "%" << std::endl;
        out << "    Empty flushes:              " << std::setprecision(0) << emptyFlushes << " (" << std::setprecision(1) << emptyPct << "%)" << std::endl;
        if (glDrawCalls > 0.5f) {
            out << "    Per draw call cost:         " << std::setprecision(1)
                << (glTimeDraw * 1000.0f / glDrawCalls) << " us" << std::endl;
        }
        out << std::endl;

        // Per-buffer breakdown
        out << "  Per-Buffer Breakdown:" << std::endl;
        out << "    Buffer               Cmds   Tris  Verts   Tex   Mtx  Sync SubDL Comb" << std::endl;
        for (int b = 0; b < PROFILE_DL_BUFFER_COUNT; b++) {
            const DLBufferStats& bs = r.bufferStats[b];
            char line[256];
            snprintf(line, sizeof(line), "    %-20s %5d  %5d  %5d  %4d  %4d  %4d  %4d  %4d", sBufNames[b],
                     bs.commands, bs.triangles, bs.vertices, bs.texLoads, bs.mtxLoads, bs.pipeSyncs, bs.subcalls,
                     bs.setCombine);
            out << line << std::endl;
        }
        out << std::endl;

        // Memory bandwidth estimate
        float estimatedFloatsPerVert = 20.0f;
        float vboKB = glVerts * estimatedFloatsPerVert * 4.0f / 1024.0f;
        out << "  Memory Bandwidth:" << std::endl;
        out << "    VBO data per frame:         " << std::setprecision(0) << vboKB << " KB" << std::endl;
        if (renderFrameMs > 0.01f) {
            float mbPerSec = (vboKB / 1024.0f) * fpsAvg;
            out << "    VBO throughput:             " << std::setprecision(1) << mbPerSec << " MB/s" << std::endl;
        }
        out << std::endl;

        // Estimated unaccounted time — total frame time minus selected phases.
        // Phases may overlap (FrameProfiler accumulates per phase independently),
        // so accountedMs can exceed totalMs. Clamped to 0 since negative values
        // indicate overlap, not missing time.
        float accountedMs =
            r.phases[PROFILE_PHASE_PLAY_UPDATE] + r.phases[PROFILE_PHASE_PLAY_DRAW] +
            r.phases[PROFILE_PHASE_AUDIO_WAIT] + r.phases[PROFILE_PHASE_FRAME_INTERP] +
            r.phases[PROFILE_PHASE_DL_PROCESS];
        float unaccountedMs = totalMs - accountedMs;
        if (unaccountedMs < 0.0f) {
            unaccountedMs = 0.0f;
        }
        float unaccountedPct = (totalMs > 0.01f) ? (unaccountedMs / totalMs * 100.0f) : 0.0f;
        out << "  Unaccounted Time:             " << std::setprecision(2) << unaccountedMs
            << " ms (" << std::setprecision(1) << unaccountedPct << "%)" << std::endl;
        out << std::endl;

        // Per-unit cost analysis
        float dlMs = r.phases[PROFILE_PHASE_DL_PROCESS];
        out << "  Per-Unit Cost Analysis:" << std::endl;
        if (dlMs > 0.1f && glTris > 0.0f) {
            out << "    Cost per GL triangle:       " << std::setprecision(2) << (dlMs * 1000.0f / glTris) << " us" << std::endl;
            float dlCmdsLocal = r.counters[PROFILE_COUNTER_DL_COMMANDS];
            if (dlCmdsLocal > 0) {
                out << "    Cost per DL command:        " << std::setprecision(2) << (dlMs * 1000.0f / dlCmdsLocal) << " us" << std::endl;
            }
            if (glDrawCalls > 0) {
                out << "    Cost per GL draw call:      " << std::setprecision(2) << (dlMs * 1000.0f / glDrawCalls) << " us" << std::endl;
            }
            if (glShaderSwitches > 0) {
                out << "    Cost per shader switch:     " << std::setprecision(2) << (dlMs * 1000.0f / glShaderSwitches) << " us" << std::endl;
            }
        }
        out << std::endl;

        // Flush efficiency analysis
        float stateActualFlushes = glStateFlushesFromBatch;
        emptyFlushes = std::max(0.0f, glBatchFlushes - glDrawCalls);
        emptyPct = (glBatchFlushes > 0.5f) ? (emptyFlushes / glBatchFlushes * 100.0f) : 0.0f;
        float effectiveBatch = (glDrawCalls > 0.5f) ? (glTris / glDrawCalls) : 0.0f;
        out << "  Flush Efficiency:" << std::endl;
        out << "    Total Flushes:              " << std::setprecision(0) << glBatchFlushes << std::endl;
        out << "    State-change driven:        " << std::setprecision(0) << stateActualFlushes << std::endl;
        out << "    Buffer-full:                " << std::setprecision(0) << glBufferFullFlushes << std::endl;
        out << "    Empty (no geometry):        " << std::setprecision(0) << emptyFlushes
            << " (" << std::setprecision(1) << emptyPct << "%)" << std::endl;
        out << "    Effective batch size:       " << std::setprecision(1) << effectiveBatch << " tris/draw" << std::endl;
#ifdef __SWITCH__
        out << "    Buffer utilization:         " << std::setprecision(1) << (effectiveBatch / 1024.0f * 100.0f) << "% (of 1024 MAX_TRI_BUFFER)" << std::endl;
#else
        out << "    Buffer utilization:         " << std::setprecision(1) << (effectiveBatch / 256.0f * 100.0f) << "% (of 256 MAX_TRI_BUFFER)" << std::endl;
#endif
        if (glDrawCalls > 0.5f) {
            out << "    State changes per draw:     " << std::setprecision(2) << (stateActualFlushes / glDrawCalls) << std::endl;
        }
        out << std::endl;

        // Core utilization
        float core0Ms =
            r.phases[PROFILE_PHASE_COLLISION_AT] + r.phases[PROFILE_PHASE_COLLISION_DAMAGE] +
            r.phases[PROFILE_PHASE_ACTOR_UPDATE] + r.phases[PROFILE_PHASE_ACTOR_DRAW] +
            r.phases[PROFILE_PHASE_SCENE_DRAW] + r.phases[PROFILE_PHASE_FRAME_INTERP] +
            r.phases[PROFILE_PHASE_DL_PROCESS] + r.phases[PROFILE_PHASE_AUDIO_WAIT];
        float core1Ms = r.phases[PROFILE_PHASE_COLLISION_OC];
        float imbalance = (core0Ms > 0.01f) ? (core1Ms / core0Ms) : 0.0f;
        out << "  Core Utilization:" << std::endl;
        out << "    Core 0 (main):              " << std::setprecision(2) << core0Ms << " ms" << std::endl;
        out << "    Worker core:                " << std::setprecision(2) << core1Ms << " ms" << std::endl;
        out << "    Worker/Core 0 ratio:        " << std::setprecision(1) << (imbalance * 100.0f) << "%" << std::endl;
        out << std::endl;

        // Game logic vs rendering breakdown (key for frame-ahead analysis)
        float gameLogicMs = r.phases[PROFILE_PHASE_PLAY_UPDATE] + r.phases[PROFILE_PHASE_PLAY_DRAW];
        float renderMs = r.phases[PROFILE_PHASE_DL_PROCESS];
        float renderPerIter = (dlIter > 0.5f) ? (renderMs / dlIter) : renderMs;
        float fast3dPerIter = (dlIter > 0.5f) ? (glTimeTotal / dlIter) : glTimeTotal;
        float vsyncPeriod = 16.67f;
        float vsyncGapDL = vsyncPeriod - renderPerIter;
        float vsyncGapGL = vsyncPeriod - fast3dPerIter;
        out << "  Frame-Ahead Analysis:" << std::endl;
        out << "    Game logic (serial):        " << std::setprecision(2) << gameLogicMs << " ms" << std::endl;
        out << "    DL Process per iteration:   " << std::setprecision(2) << renderPerIter
            << " ms (includes vsync wait)" << std::endl;
        out << "    Fast3D per iteration:       " << std::setprecision(2) << fast3dPerIter
            << " ms (active GL only)" << std::endl;
        out << "    Vsync period:               " << std::setprecision(2) << vsyncPeriod << " ms" << std::endl;
        out << "    Vsync gap (DL average):     " << std::setprecision(2) << vsyncGapDL << " ms" << std::endl;
        out << "    Vsync gap (last sub-frame): " << std::setprecision(2) << vsyncGapGL << " ms"
            << (vsyncGapGL >= gameLogicMs ? " (enough to hide game logic!)" :
               vsyncGapGL > 0 ? " (partially hides game logic)" : " (no gap - rendering exceeds vsync)")
            << std::endl;
        float theoreticalMin = vsyncPeriod * (dlIter > 1.0f ? dlIter - 1.0f : 0.0f) + fast3dPerIter +
            (gameLogicMs > vsyncGapGL ? gameLogicMs - vsyncGapGL : 0.0f);
        float theoreticalFps = (theoreticalMin > 0.01f) ? (dlIter * 1000.0f / theoreticalMin) : 0.0f;
        out << "    Theoretical min tick:       " << std::setprecision(2) << theoreticalMin << " ms" << std::endl;
        out << "    Theoretical max FPS:        " << std::setprecision(1) << theoreticalFps << std::endl;
        out << std::endl;

        // Automated analysis with ranked sub-phases and recommendations
        int worstPhase = -1;
        float worstMs = 0.0f;
        for (int i = 0; i < PROFILE_PHASE_MAX; i++) {
            bool isLeaf = (i != PROFILE_PHASE_TOTAL_FRAME && i != PROFILE_PHASE_PLAY_UPDATE &&
                           i != PROFILE_PHASE_PLAY_DRAW && i != PROFILE_PHASE_GFX_COMMANDS);
            if (isLeaf && r.phases[i] > worstMs) {
                worstMs = r.phases[i];
                worstPhase = i;
            }
        }
        out << "  Analysis:" << std::endl;
        if (worstPhase >= 0 && totalMs > 0.01f) {
            float worstPct = worstMs / totalMs * 100.0f;
            out << "    Bottleneck: " << sPhaseNames[worstPhase] << " (" << std::setprecision(2) << worstMs
                << " ms, " << std::setprecision(1) << worstPct << "%)" << std::endl;
        }
        if (worstPhase == PROFILE_PHASE_DL_PROCESS && glTimeTotal > 0.01f) {
            float glTimeTriExclLocal = glTimeTri > glTimeDraw ? glTimeTri - glTimeDraw : 0.0f;
            struct GlPhaseInfo { const char* name; float ms; };
            GlPhaseInfo glPhases[] = {
                {"tri (per-vertex)", glTimeTriExclLocal}, {"draw (GL submit)", glTimeDraw},
                {"  VBO upload", glTimeVboUpload}, {"  glDrawArrays", glTimeGlDraw},
                {"vtx (vertex xform)", glTimeVtx}, {"tex (texture setup)", glTimeTex},
                {"mtx (matrix ops)", glTimeMtx}, {"dispatch (cmd walk)", glTimeDispatch},
                {"depth (readback)", glTimeDepth}, {"setup (frame init)", glTimeSetup},
                {"shader (compile)", glTimeShader}
            };
            int numGlPhases = sizeof(glPhases) / sizeof(glPhases[0]);
            for (int i = 0; i < numGlPhases - 1; i++) {
                for (int j = i + 1; j < numGlPhases; j++) {
                    if (glPhases[j].ms > glPhases[i].ms) {
                        GlPhaseInfo tmp = glPhases[i];
                        glPhases[i] = glPhases[j];
                        glPhases[j] = tmp;
                    }
                }
            }
            out << "    Top GL sub-phases:" << std::endl;
            for (int i = 0; i < 3 && i < numGlPhases; i++) {
                if (glPhases[i].ms < 0.01f) break;
                float pct = glPhases[i].ms / glTimeTotal * 100.0f;
                out << "      #" << (i + 1) << " " << glPhases[i].name << ": " << std::setprecision(2)
                    << glPhases[i].ms << " ms (" << std::setprecision(1) << pct << "%)" << std::endl;
            }
        }
        // Warnings
        if (emptyPct > 30.0f) {
            out << "    WARNING: " << std::setprecision(1) << emptyPct << "% empty flushes — consider lazy state application" << std::endl;
        }
        if (effectiveBatch < 20.0f && glDrawCalls > 100.0f) {
            out << "    WARNING: Low batch size (" << std::setprecision(1) << effectiveBatch << " tris/draw) — state changes fragment batches" << std::endl;
        }
        if (glTimeDraw > 10.0f && glDrawCalls > 500.0f) {
            float costPerDraw = glTimeDraw * 1000.0f / glDrawCalls;
            out << "    WARNING: High GL draw overhead: " << std::setprecision(0) << glDrawCalls << " calls x "
                << std::setprecision(1) << costPerDraw << " us = " << std::setprecision(2) << glTimeDraw << " ms" << std::endl;
        }
        if (unaccountedMs > 3.0f) {
            out << "    WARNING: " << std::setprecision(2) << unaccountedMs << " ms unaccounted — hidden overhead (ImGui, audio, vsync sleep, stats processing)" << std::endl;
        }
        out << std::endl;
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
    if (ImGui::Button("Full Benchmark")) {
        StartBenchmark(BENCH_MODE_FULL);
    }
    ImGui::SameLine();
    if (ImGui::Button("Quick Benchmark")) {
        StartBenchmark(BENCH_MODE_QUICK);
    }
    if (!canStart) {
        ImGui::EndDisabled();
        if (gPlayState == NULL) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "Load a save file first");
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(Full: %d scenes, Quick: %d scenes)", BENCHMARK_FULL_COUNT, BENCHMARK_QUICK_COUNT);

    // Progress display
    if (isRunning) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Status: Running %s benchmark...", sBenchMode == BENCH_MODE_QUICK ? "Quick" : "Full");

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
            default:
                break;
        }

        ImGui::Text("Scene %d/%d: %s", sCurrentScene + 1, sActiveSceneCount,
                     (sCurrentScene < sActiveSceneCount) ? sActiveScenes[sCurrentScene].name : "Done");
        ImGui::Text("Phase: %s", stateStr);

        if (sCurrentScene < sActiveSceneCount) {
            if (sState == BENCH_SETTLING) {
                float pct = (float)sFrameCounter / sActiveScenes[sCurrentScene].settleFrames;
                ImGui::ProgressBar(pct, ImVec2(-1, 0), "Settling");
            } else if (sState == BENCH_MEASURING) {
                float pct = (float)sFrameCounter / sActiveScenes[sCurrentScene].measureFrames;
                ImGui::ProgressBar(pct, ImVec2(-1, 0), "Measuring");
            }
        }

        // Overall progress
        float overallPct = (float)sResults.size() / sActiveSceneCount;
        ImGui::ProgressBar(overallPct, ImVec2(-1, 0), "Overall");
    }

    // Results
    if (!sResults.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Results (%s):", sBenchMode == BENCH_MODE_QUICK ? "Quick" : "Full");

        // Baseline info
        if (sBaseline.loaded) {
            std::string baseCommit =
                sBaseline.commit.size() > 7 ? sBaseline.commit.substr(0, 7) : sBaseline.commit;
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Baseline: %s @ %s (%s, %s)",
                               sBaseline.branch.c_str(), baseCommit.c_str(), sBaseline.timestamp.c_str(),
                               sBaseline.mode.c_str());
        }

        // Summary table -- add delta columns when baseline exists
        int colCount = sBaseline.loaded ? 12 : 10;
        if (ImGui::BeginTable("BenchmarkSummary", colCount,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollX)) {
            ImGui::TableSetupColumn("Scene");
            ImGui::TableSetupColumn("Frame ms");
            if (sBaseline.loaded) {
                ImGui::TableSetupColumn("vs Base");
            }
            ImGui::TableSetupColumn("FPS Avg");
            if (sBaseline.loaded) {
                ImGui::TableSetupColumn("vs Base");
            }
            ImGui::TableSetupColumn("FPS Min");
            ImGui::TableSetupColumn("FPS Max");
            ImGui::TableSetupColumn("DL Process");
            ImGui::TableSetupColumn("GL Draws");
            ImGui::TableSetupColumn("GL Tris");
            ImGui::TableSetupColumn("Batch Avg");
            ImGui::TableSetupColumn("Shader Sw");
            ImGui::TableHeadersRow();

            for (const auto& r : sResults) {
                float frameMs = ResultRenderFrameMs(r);
                float fpsAvg = ResultAvgFps(r);
                float fpsMin = ResultMinFps(r);
                float fpsMax = ResultMaxFps(r);
                const BenchmarkResult* base = FindBaselineForScene(r.name);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s", r.name);

                ImGui::TableNextColumn();
                ImVec4 color = (frameMs < BENCHMARK_TARGET_60FPS_MS)  ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f)
                               : (frameMs < BENCHMARK_WARN_40FPS_MS) ? ImVec4(1.0f, 1.0f, 0.2f, 1.0f)
                                                                     : ImVec4(1.0f, 0.3f, 0.2f, 1.0f);
                ImGui::TextColored(color, "%.2f", frameMs);

                if (sBaseline.loaded) {
                    ImGui::TableNextColumn();
                    if (base) {
                        float baseMs = ResultRenderFrameMs(*base);
                        float deltaMs = frameMs - baseMs;
                        ImGui::TextColored(DeltaColor(deltaMs, true), "%s%.2f", DeltaSign(deltaMs), deltaMs);
                    } else {
                        ImGui::TextDisabled("-");
                    }
                }

                ImGui::TableNextColumn();
                ImGui::TextColored(color, "%.1f", fpsAvg);

                if (sBaseline.loaded) {
                    ImGui::TableNextColumn();
                    if (base) {
                        float baseFps = ResultAvgFps(*base);
                        float deltaFps = fpsAvg - baseFps;
                        ImGui::TextColored(DeltaColor(deltaFps, false), "%s%.1f", DeltaSign(deltaFps), deltaFps);
                    } else {
                        ImGui::TextDisabled("-");
                    }
                }

                ImGui::TableNextColumn();
                ImGui::Text("%.1f", fpsMin);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", fpsMax);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", r.phases[PROFILE_PHASE_DL_PROCESS]);
                float glDrawCalls = r.counters[PROFILE_COUNTER_GL_DRAW_CALLS];
                float glTris = r.counters[PROFILE_COUNTER_GL_TRIANGLES_SUBMITTED];
                float glAvgBatch = (glDrawCalls > 0.5f) ? (glTris / glDrawCalls) : 0.0f;
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", glDrawCalls);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", glTris);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", glAvgBatch);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", r.counters[PROFILE_COUNTER_GL_SHADER_SWITCHES]);
            }
            ImGui::EndTable();
        }

        // Per-scene detail sections
        for (size_t si = 0; si < sResults.size(); si++) {
            const auto& r = sResults[si];
            char treeLabel[128];
            snprintf(treeLabel, sizeof(treeLabel), "%s (%.1f/%.1f/%.1f FPS avg/min/max)###scene%zu", r.name,
                     ResultAvgFps(r), ResultMinFps(r), ResultMaxFps(r), si);
            if (ImGui::TreeNode(treeLabel)) {
                // Phase breakdown
                if (ImGui::TreeNode("Phase Timing")) {
                    float totalMs = r.phases[PROFILE_PHASE_TOTAL_FRAME];
                    for (int i = 0; i < PROFILE_PHASE_MAX; i++) {
                        float ms = r.phases[i];
                        float pct = (totalMs > 0.01f) ? (ms / totalMs * 100.0f) : 0.0f;
                        ImGui::Text("%-22s %6.2f ms (%5.1f%%)", sPhaseNames[i], ms, pct);
                    }
                    ImGui::TreePop();
                }

                // Fast3D timing
                if (ImGui::TreeNode("Fast3D Timing")) {
                    ImGui::Text("Total:              %.2f ms", r.counters[PROFILE_COUNTER_GL_TIME_TOTAL_MS]);
                    ImGui::Text("  Dispatch:         %.2f ms", r.counters[PROFILE_COUNTER_GL_TIME_DISPATCH_MS]);
                    ImGui::Text("  Triangle:         %.2f ms", r.counters[PROFILE_COUNTER_GL_TIME_TRI_MS]);
                    ImGui::Text("    Draw Submit:    %.2f ms", r.counters[PROFILE_COUNTER_GL_TIME_DRAW_MS]);
                    ImGui::Text("      VBO Upload:   %.2f ms", r.counters[PROFILE_COUNTER_GL_TIME_VBO_UPLOAD_MS]);
                    ImGui::Text("      glDrawArrays: %.2f ms", r.counters[PROFILE_COUNTER_GL_TIME_GL_DRAW_MS]);
                    ImGui::Text("  Vertex:           %.2f ms", r.counters[PROFILE_COUNTER_GL_TIME_VTX_MS]);
                    ImGui::Text("  Texture:          %.2f ms", r.counters[PROFILE_COUNTER_GL_TIME_TEX_MS]);
                    ImGui::Text("  Matrix:           %.2f ms", r.counters[PROFILE_COUNTER_GL_TIME_MTX_MS]);
                    ImGui::Text("  Depth:            %.2f ms", r.counters[PROFILE_COUNTER_GL_TIME_DEPTH_MS]);
                    ImGui::Text("  Setup:            %.2f ms", r.counters[PROFILE_COUNTER_GL_TIME_SETUP_MS]);
                    ImGui::Text("  Shader:           %.2f ms", r.counters[PROFILE_COUNTER_GL_TIME_SHADER_MS]);
                    ImGui::TreePop();
                }

                // Flush causes
                if (ImGui::TreeNode("Flush Causes")) {
                    ImGui::Text("Texture:    %.0f", r.counters[PROFILE_COUNTER_GL_FLUSH_CAUSE_TEXTURE]);
                    ImGui::Text("Shader:     %.0f", r.counters[PROFILE_COUNTER_GL_FLUSH_CAUSE_SHADER]);
                    ImGui::Text("Sampler:    %.0f", r.counters[PROFILE_COUNTER_GL_FLUSH_CAUSE_SAMPLER]);
                    ImGui::Text("Alpha:      %.0f", r.counters[PROFILE_COUNTER_GL_FLUSH_CAUSE_ALPHA]);
                    ImGui::Text("Depth/VP:   %.0f", r.counters[PROFILE_COUNTER_GL_FLUSH_CAUSE_DEPTH_VIEWPORT]);
                    ImGui::Text("Combiner:   %.0f", r.counters[PROFILE_COUNTER_GL_FLUSH_CAUSE_COMBINER]);
                    ImGui::Text("Tex skips:  %.0f", r.counters[PROFILE_COUNTER_GL_TEXTURE_RELOAD_SKIPS]);
                    ImGui::TreePop();
                }

                // Batch histogram
                if (ImGui::TreeNode("Batch Size Distribution")) {
                    static const char* bucketLabels[] = { "1-2", "3-8", "9-32", "33-128", "129+" };
                    float totalH = 0.0f;
                    float hist[5];
                    for (int b = 0; b < 5; b++) {
                        hist[b] = r.counters[PROFILE_COUNTER_GL_BATCH_HIST_0 + b];
                        totalH += hist[b];
                    }
                    for (int b = 0; b < 5; b++) {
                        float pct = (totalH > 0.5f) ? (hist[b] / totalH * 100.0f) : 0.0f;
                        ImGui::Text("%-8s %5.0f draws (%4.1f%%)", bucketLabels[b], hist[b], pct);
                    }
                    float maxBatchSeen = r.counters[PROFILE_COUNTER_GL_MAX_BATCH_SIZE];
                    const float histEps = 0.001f;
                    if (hist[4] <= histEps && maxBatchSeen > 128.0f) maxBatchSeen = 128.0f;
                    if (hist[3] <= histEps && maxBatchSeen > 32.0f) maxBatchSeen = 32.0f;
                    if (hist[2] <= histEps && maxBatchSeen > 8.0f) maxBatchSeen = 8.0f;
                    if (hist[1] <= histEps && maxBatchSeen > 2.0f) maxBatchSeen = 2.0f;
                    if (hist[0] <= histEps && maxBatchSeen > 0.0f) maxBatchSeen = 0.0f;
                    ImGui::Text("Max batch: %.0f tris", maxBatchSeen);
                    ImGui::TreePop();
                }

                // Per-buffer stats
                if (ImGui::TreeNode("Per-Buffer Stats")) {
                    static const char* bufNames[] = { "OPA", "XLU", "Overlay", "Work", "Debug" };
                    if (ImGui::BeginTable("BufStats", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                        ImGui::TableSetupColumn("Buffer");
                        ImGui::TableSetupColumn("Cmds");
                        ImGui::TableSetupColumn("Tris");
                        ImGui::TableSetupColumn("Verts");
                        ImGui::TableSetupColumn("Tex");
                        ImGui::TableSetupColumn("Mtx");
                        ImGui::TableHeadersRow();
                        for (int b = 0; b < PROFILE_DL_BUFFER_COUNT; b++) {
                            const DLBufferStats& bs = r.bufferStats[b];
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", bufNames[b]);
                            ImGui::TableNextColumn();
                            ImGui::Text("%d", bs.commands);
                            ImGui::TableNextColumn();
                            ImGui::Text("%d", bs.triangles);
                            ImGui::TableNextColumn();
                            ImGui::Text("%d", bs.vertices);
                            ImGui::TableNextColumn();
                            ImGui::Text("%d", bs.texLoads);
                            ImGui::TableNextColumn();
                            ImGui::Text("%d", bs.mtxLoads);
                        }
                        ImGui::EndTable();
                    }
                    ImGui::TreePop();
                }

                ImGui::TreePop();
            }
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

        // Baseline buttons
        ImGui::Spacing();
        if (ImGui::Button("Save as Baseline")) {
            SaveBaseline();
        }
        ImGui::SameLine();
        if (sBaseline.loaded) {
            if (ImGui::Button("Clear Baseline")) {
                ClearBaseline();
            }
        } else {
            ImGui::TextDisabled("No baseline loaded");
        }

        if (sBaselineMsgTimer > 0.0f) {
            sBaselineMsgTimer -= ImGui::GetIO().DeltaTime;
            ImGui::TextWrapped("%s", sBaselineMsgText.c_str());
        }
    }

    // Scene list reference
    if (ImGui::TreeNode("Full Scenes")) {
        for (int i = 0; i < BENCHMARK_FULL_COUNT; i++) {
            ImGui::BulletText("%s (settle: %d, measure: %d)", sBenchmarkScenesFull[i].name,
                              sBenchmarkScenesFull[i].settleFrames, sBenchmarkScenesFull[i].measureFrames);
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Quick Scenes")) {
        for (int i = 0; i < BENCHMARK_QUICK_COUNT; i++) {
            ImGui::BulletText("%s (settle: %d, measure: %d)", sBenchmarkScenesQuick[i].name,
                              sBenchmarkScenesQuick[i].settleFrames, sBenchmarkScenesQuick[i].measureFrames);
        }
        ImGui::TreePop();
    }
}
