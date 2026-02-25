#include "Rewind.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <thread>
#include <vector>
#include <spdlog/spdlog.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "2s2h/BenPort.h"
#include "2s2h/ShipInit.hpp"
#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
#include "z64save.h"
#include "variables.h"
#include "functions.h"
#include "sequence.h"
}

extern "C" PlayState* gPlayState;
extern "C" u8* gAudioHeap;
extern "C" u8* gSystemHeap;
extern "C" MtxF* sMatrixStack;
extern "C" MtxF* sCurrentMatrix;
extern "C" LightsBuffer sLightsBuffer;
extern "C" ActiveSequence gActiveSeqs[];

#define MATRIX_STACK_SIZE 20
#define NUM_SEQ_PLAYERS 5

// Page-level diff-based rewind buffer.
// Stores a full keyframe plus incremental diffs (only changed 4KB pages).
// Typical per-frame diffs are tiny (10-200 KB), making this very memory efficient.
static constexpr size_t PAGE_SIZE = 4096;

// Small struct data that lives outside heaps
struct RewindSmallState {
    SaveContext saveContextCopy;
    LightsBuffer lightBufferCopy;
    MtxF mtxStackCopy[MATRIX_STACK_SIZE];
    MtxF currentMtxCopy;
    AudioContext audioCtxCopy;
    ActiveSequence activeSeqsCopy[NUM_SEQ_PLAYERS];
    SeqScriptState seqScriptStateCopy[NUM_SEQ_PLAYERS];
};

// A diff frame stores only pages that changed since the previous frame
struct DiffFrame {
    // System heap diffs: list of (pageIndex, pageData) pairs packed into a buffer
    std::vector<uint32_t> sysPageIndices;
    std::vector<uint8_t> sysPageData;
    // Audio heap diffs
    std::vector<uint32_t> audioPageIndices;
    std::vector<uint8_t> audioPageData;
    // Small state is always stored in full (it's tiny)
    RewindSmallState smallState;

    size_t GetBytes() const {
        return sysPageData.size() + audioPageData.size() + sizeof(RewindSmallState) +
               sysPageIndices.size() * sizeof(uint32_t) + audioPageIndices.size() * sizeof(uint32_t);
    }
};

// Previous frame's heap snapshots for diffing
struct HeapSnapshot {
    std::vector<uint8_t> data;
    size_t size;

    HeapSnapshot() : size(0) {
    }

    bool Capture(const uint8_t* heap, size_t heapSize) {
        try {
            if (data.size() != heapSize) {
                data.resize(heapSize);
            }
        } catch (const std::bad_alloc&) { return false; }
        memcpy(data.data(), heap, heapSize);
        size = heapSize;
        return true;
    }

    // Compute diff: find pages that differ between this snapshot and live heap
    bool ComputeDiff(const uint8_t* liveHeap, size_t heapSize, std::vector<uint32_t>& indices,
                     std::vector<uint8_t>& pageData) const {
        indices.clear();
        pageData.clear();
        size_t pageCount = (heapSize + PAGE_SIZE - 1) / PAGE_SIZE;
        for (size_t i = 0; i < pageCount; i++) {
            size_t offset = i * PAGE_SIZE;
            size_t len = std::min(PAGE_SIZE, heapSize - offset);
            if (memcmp(data.data() + offset, liveHeap + offset, len) != 0) {
                indices.push_back(static_cast<uint32_t>(i));
                size_t pos = pageData.size();
                try {
                    pageData.resize(pos + PAGE_SIZE, 0);
                } catch (const std::bad_alloc&) { return false; }
                // Store the OLD data (snapshot) so we can restore it when rewinding
                memcpy(pageData.data() + pos, data.data() + offset, len);
            }
        }
        return true;
    }

    // Apply a diff: restore the pages from the diff into the live heap
    static void ApplyDiff(uint8_t* heap, const std::vector<uint32_t>& indices, const std::vector<uint8_t>& pageData,
                          size_t heapSize) {
        for (size_t i = 0; i < indices.size(); i++) {
            size_t offset = static_cast<size_t>(indices[i]) * PAGE_SIZE;
            size_t len = std::min(PAGE_SIZE, heapSize - offset);
            memcpy(heap + offset, pageData.data() + i * PAGE_SIZE, len);
        }
    }
};

static void CaptureSmallState(RewindSmallState& state) {
    memcpy(&state.saveContextCopy, &gSaveContext, sizeof(SaveContext));
    memcpy(&state.lightBufferCopy, &sLightsBuffer, sizeof(LightsBuffer));
    memcpy(&state.mtxStackCopy, sMatrixStack, sizeof(MtxF) * MATRIX_STACK_SIZE);
    memcpy(&state.currentMtxCopy, sCurrentMatrix, sizeof(MtxF));
    memcpy(&state.audioCtxCopy, &gAudioCtx, sizeof(AudioContext));
    memcpy(&state.activeSeqsCopy, gActiveSeqs, sizeof(ActiveSequence) * NUM_SEQ_PLAYERS);

    for (int i = 0; i < NUM_SEQ_PLAYERS; i++) {
        SeqScriptState* src = &gAudioCtx.seqPlayers[i].scriptState;
        SeqScriptState* dst = &state.seqScriptStateCopy[i];
        dst->value = src->value;
        dst->depth = src->depth;
        memcpy(dst->remLoopIters, src->remLoopIters, sizeof(dst->remLoopIters));
        dst->pc = (u8*)((uintptr_t)src->pc - (uintptr_t)gAudioHeap);
        for (int j = 0; j < 4; j++) {
            dst->stack[j] = (u8*)((uintptr_t)src->stack[j] - (uintptr_t)gAudioHeap);
        }
    }
}

static void RestoreSmallState(const RewindSmallState& state) {
    memcpy(&gSaveContext, &state.saveContextCopy, sizeof(SaveContext));
    memcpy(&sLightsBuffer, &state.lightBufferCopy, sizeof(LightsBuffer));
    memcpy(sMatrixStack, &state.mtxStackCopy, sizeof(MtxF) * MATRIX_STACK_SIZE);
    memcpy(sCurrentMatrix, &state.currentMtxCopy, sizeof(MtxF));
    memcpy(&gAudioCtx, &state.audioCtxCopy, sizeof(AudioContext));
    memcpy(gActiveSeqs, &state.activeSeqsCopy, sizeof(ActiveSequence) * NUM_SEQ_PLAYERS);

    for (int i = 0; i < NUM_SEQ_PLAYERS; i++) {
        SeqScriptState* src = const_cast<SeqScriptState*>(&state.seqScriptStateCopy[i]);
        SeqScriptState* dst = &gAudioCtx.seqPlayers[i].scriptState;
        dst->value = src->value;
        dst->depth = src->depth;
        memcpy(dst->remLoopIters, src->remLoopIters, sizeof(dst->remLoopIters));
        dst->pc = (u8*)((uintptr_t)src->pc + (uintptr_t)gAudioHeap);
        for (int j = 0; j < 4; j++) {
            dst->stack[j] = (u8*)((uintptr_t)src->stack[j] + (uintptr_t)gAudioHeap);
        }
    }

    if (gPlayState != nullptr) {
        gPlayState->pauseCtx.state = 0;
        gPlayState->pauseCtx.debugEditor = 0;
    }

    Audio_Update();
}

// The rewind buffer
static std::deque<DiffFrame> sRewindBuffer;
static HeapSnapshot sPrevSysSnapshot;
static HeapSnapshot sPrevAudioSnapshot;
static bool sRewindInitialized = false;
static bool sIsRewinding = false;
static int sFrameCounter = 0;
static size_t sRewindMemUsage = 0;

// Configuration defaults
static constexpr int DEFAULT_CAPTURE_INTERVAL = 10;
static constexpr int DEFAULT_MAX_MEMORY_MB = 64;

// Worker thread for diff computation on Core 2
struct CaptureJob {
    std::vector<uint8_t> sysHeapCopy;
    std::vector<uint8_t> audioHeapCopy;
    RewindSmallState smallState;
};

static std::thread sWorkerThread;
static std::mutex sWorkerMutex;
static std::condition_variable sWorkerCV;
static std::atomic<bool> sWorkerRunning{ false };
static std::unique_ptr<CaptureJob> sPendingJob;
static std::mutex sBufferMutex; // Protects sRewindBuffer and sRewindMemUsage

static void WorkerThreadFunc() {
#ifdef __SWITCH__
    // Pin worker thread to Core 2 (barely used by game/render)
    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, (1U << 2));
    if (R_FAILED(rc)) {
        SPDLOG_WARN("[2S2H] Rewind: Failed to pin worker to Core 2 (rc=0x{:X})", rc);
    }
#endif

    while (sWorkerRunning.load()) {
        std::unique_ptr<CaptureJob> job;
        {
            std::unique_lock<std::mutex> lock(sWorkerMutex);
            sWorkerCV.wait(lock, [] { return sPendingJob != nullptr || !sWorkerRunning.load(); });
            if (!sWorkerRunning.load()) {
                break;
            }
            job = std::move(sPendingJob);
        }

        if (!job) {
            continue;
        }

        // Compute diff between previous snapshot and captured heap
        DiffFrame frame;
        if (!sPrevSysSnapshot.ComputeDiff(job->sysHeapCopy.data(), SYSTEM_HEAP_SIZE, frame.sysPageIndices,
                                          frame.sysPageData) ||
            !sPrevAudioSnapshot.ComputeDiff(job->audioHeapCopy.data(), AUDIO_HEAP_SIZE, frame.audioPageIndices,
                                            frame.audioPageData)) {
            continue;
        }

        frame.smallState = job->smallState;
        size_t frameBytes = frame.GetBytes();

        int maxMemMB = CVarGetInteger("gCheats.RewindMaxMemoryMB", DEFAULT_MAX_MEMORY_MB);
        size_t maxMemBytes = static_cast<size_t>(maxMemMB) * 1024 * 1024;

        {
            std::lock_guard<std::mutex> lock(sBufferMutex);

            // Evict old frames if over memory budget
            while (!sRewindBuffer.empty() && sRewindMemUsage + frameBytes > maxMemBytes) {
                sRewindMemUsage -= sRewindBuffer.front().GetBytes();
                sRewindBuffer.pop_front();
            }

            sRewindMemUsage += frameBytes;
            sRewindBuffer.push_back(std::move(frame));
        }

        // Update previous snapshots from the captured data
        memcpy(sPrevSysSnapshot.data.data(), job->sysHeapCopy.data(), SYSTEM_HEAP_SIZE);
        memcpy(sPrevAudioSnapshot.data.data(), job->audioHeapCopy.data(), AUDIO_HEAP_SIZE);
    }
}

static void StartWorkerThread() {
    if (sWorkerRunning.load()) {
        return;
    }
    sWorkerRunning.store(true);
    sWorkerThread = std::thread(WorkerThreadFunc);
}

static void StopWorkerThread() {
    if (!sWorkerRunning.load()) {
        return;
    }
    sWorkerRunning.store(false);
    sWorkerCV.notify_one();
    if (sWorkerThread.joinable()) {
        sWorkerThread.join();
    }
}

static void RewindCapture() {
    if (!gPlayState) {
        return;
    }

    int captureInterval = CVarGetInteger("gCheats.RewindCaptureInterval", DEFAULT_CAPTURE_INTERVAL);
    if (captureInterval < 1) {
        captureInterval = 1;
    }

    sFrameCounter++;
    if (sFrameCounter % captureInterval != 0) {
        return;
    }

    // First capture: take base snapshot, no diff
    if (!sRewindInitialized) {
        if (!sPrevSysSnapshot.Capture(gSystemHeap, SYSTEM_HEAP_SIZE) ||
            !sPrevAudioSnapshot.Capture(gAudioHeap, AUDIO_HEAP_SIZE)) {
            return;
        }
        sRewindInitialized = true;
        sRewindMemUsage = SYSTEM_HEAP_SIZE + AUDIO_HEAP_SIZE; // Base snapshots
        StartWorkerThread();
        return;
    }

    // Check if worker is still processing previous job
    {
        std::lock_guard<std::mutex> lock(sWorkerMutex);
        if (sPendingJob) {
            return; // Worker hasn't consumed the last job yet, skip this frame
        }
    }

    // Snapshot heaps on the main thread (fast memcpy) and hand off to worker
    auto job = std::make_unique<CaptureJob>();
    try {
        job->sysHeapCopy.resize(SYSTEM_HEAP_SIZE);
        job->audioHeapCopy.resize(AUDIO_HEAP_SIZE);
    } catch (const std::bad_alloc&) {
        return;
    }
    memcpy(job->sysHeapCopy.data(), gSystemHeap, SYSTEM_HEAP_SIZE);
    memcpy(job->audioHeapCopy.data(), gAudioHeap, AUDIO_HEAP_SIZE);
    CaptureSmallState(job->smallState);

    {
        std::lock_guard<std::mutex> lock(sWorkerMutex);
        sPendingJob = std::move(job);
    }
    sWorkerCV.notify_one();
}

static bool RewindStep() {
    std::lock_guard<std::mutex> lock(sBufferMutex);

    if (sRewindBuffer.empty()) {
        return false;
    }

    const DiffFrame& frame = sRewindBuffer.back();

    // Apply the page diffs (restores pages to the state BEFORE this frame was captured)
    HeapSnapshot::ApplyDiff(gSystemHeap, frame.sysPageIndices, frame.sysPageData, SYSTEM_HEAP_SIZE);
    HeapSnapshot::ApplyDiff(gAudioHeap, frame.audioPageIndices, frame.audioPageData, AUDIO_HEAP_SIZE);

    // Restore small state
    RestoreSmallState(frame.smallState);

    sRewindMemUsage -= frame.GetBytes();
    sRewindBuffer.pop_back();

    // Update prev snapshots to match rewound state
    sPrevSysSnapshot.Capture(gSystemHeap, SYSTEM_HEAP_SIZE);
    sPrevAudioSnapshot.Capture(gAudioHeap, AUDIO_HEAP_SIZE);

    return true;
}

static void RewindClear() {
    StopWorkerThread();
    {
        std::lock_guard<std::mutex> lock(sBufferMutex);
        sRewindBuffer.clear();
        sRewindMemUsage = 0;
    }
    {
        std::lock_guard<std::mutex> lock(sWorkerMutex);
        sPendingJob.reset();
    }
    sRewindInitialized = false;
    sFrameCounter = 0;
}

void RegisterRewind() {
    bool rewindEnabled = CVarGetInteger("gCheats.RewindEnabled", 0);

    if (!rewindEnabled) {
        RewindClear();
    }

    // Capture snapshots every N frames during gameplay
    COND_HOOK(OnGameStateUpdate, rewindEnabled, []() {
        if (!gPlayState || sIsRewinding) {
            return;
        }
        RewindCapture();
    });

    // Detect M1 + DPad Left to trigger rewind and suppress game input
    COND_HOOK(OnPassPlayerInputs, rewindEnabled, [](Input* input) {
        if (!gPlayState) {
            sIsRewinding = false;
            return;
        }

        bool m1Held = CHECK_BTN_ALL(input->cur.button, BTN_CUSTOM_MODIFIER1);
        bool dpadLeftHeld = CHECK_BTN_ALL(input->cur.button, BTN_DLEFT);

        if (m1Held && dpadLeftHeld) {
            sIsRewinding = true;
            // Suppress all game input while rewinding
            memset(input, 0, sizeof(Input));
            // Step back one frame
            if (!RewindStep()) {
                Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGameOverlay()->TextDrawNotification(
                    1.0f, true, "rewind buffer empty");
            }
        } else {
            if (sIsRewinding) {
                sIsRewinding = false;
                // Re-capture snapshot after rewind ends so future diffs are relative to rewound state
                sRewindInitialized = false;
            }
        }
    });
}

static RegisterShipInitFunc initRewind(RegisterRewind,
                                       { "gCheats.RewindEnabled", "gCheats.RewindCaptureInterval",
                                         "gCheats.RewindMaxMemoryMB" });

extern "C" void ProcessRewind() {
    // Rewind processing is handled entirely via hooks, this is a no-op placeholder
    // for potential future per-frame work called from graph.c
}
