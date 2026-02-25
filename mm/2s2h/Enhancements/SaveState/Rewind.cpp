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

// A diff frame stores only pages that changed since the previous snapshot
struct DiffFrame {
    std::vector<uint32_t> sysPageIndices;
    std::vector<uint8_t> sysPageData;
    std::vector<uint32_t> audioPageIndices;
    std::vector<uint8_t> audioPageData;
    RewindSmallState smallState;

    size_t GetBytes() const {
        return sysPageData.size() + audioPageData.size() + sizeof(RewindSmallState) +
               sysPageIndices.size() * sizeof(uint32_t) + audioPageIndices.size() * sizeof(uint32_t);
    }
};

// Compute page-level diff between two heap copies (both are stable buffers, no live data)
static bool ComputePageDiff(const uint8_t* oldData, const uint8_t* newData, size_t heapSize,
                            std::vector<uint32_t>& indices, std::vector<uint8_t>& pageData) {
    indices.clear();
    pageData.clear();
    size_t pageCount = (heapSize + PAGE_SIZE - 1) / PAGE_SIZE;
    for (size_t i = 0; i < pageCount; i++) {
        size_t offset = i * PAGE_SIZE;
        size_t len = std::min(PAGE_SIZE, heapSize - offset);
        if (memcmp(oldData + offset, newData + offset, len) != 0) {
            indices.push_back(static_cast<uint32_t>(i));
            size_t pos = pageData.size();
            try {
                pageData.resize(pos + PAGE_SIZE, 0);
            } catch (const std::bad_alloc&) {
                return false;
            }
            // Store the OLD data so we can restore it when rewinding
            memcpy(pageData.data() + pos, oldData + offset, len);
        }
    }
    return true;
}

// Apply a diff: restore pages from the diff into a live heap
static void ApplyPageDiff(uint8_t* heap, const std::vector<uint32_t>& indices, const std::vector<uint8_t>& pageData,
                          size_t heapSize) {
    for (size_t i = 0; i < indices.size(); i++) {
        size_t offset = static_cast<size_t>(indices[i]) * PAGE_SIZE;
        size_t len = std::min(PAGE_SIZE, heapSize - offset);
        memcpy(heap + offset, pageData.data() + i * PAGE_SIZE, len);
    }
}

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
        // Unrelocate pointers: store as offsets from gAudioHeap
        // Validate pointer is within audio heap range to avoid bogus offsets
        uintptr_t heapBase = (uintptr_t)gAudioHeap;
        uintptr_t heapEnd = heapBase + AUDIO_HEAP_SIZE;
        uintptr_t pc = (uintptr_t)src->pc;
        dst->pc = (pc >= heapBase && pc < heapEnd) ? (u8*)(pc - heapBase) : (u8*)0;
        for (int j = 0; j < 4; j++) {
            uintptr_t sp = (uintptr_t)src->stack[j];
            dst->stack[j] = (sp >= heapBase && sp < heapEnd) ? (u8*)(sp - heapBase) : (u8*)0;
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
        const SeqScriptState* src = &state.seqScriptStateCopy[i];
        SeqScriptState* dst = &gAudioCtx.seqPlayers[i].scriptState;
        dst->value = src->value;
        dst->depth = src->depth;
        memcpy(dst->remLoopIters, src->remLoopIters, sizeof(dst->remLoopIters));
        // Relocate offsets back to absolute pointers
        uintptr_t heapBase = (uintptr_t)gAudioHeap;
        uintptr_t pcOff = (uintptr_t)src->pc;
        dst->pc = (pcOff < AUDIO_HEAP_SIZE) ? (u8*)(heapBase + pcOff) : (u8*)heapBase;
        for (int j = 0; j < 4; j++) {
            uintptr_t spOff = (uintptr_t)src->stack[j];
            dst->stack[j] = (spOff < AUDIO_HEAP_SIZE) ? (u8*)(heapBase + spOff) : (u8*)heapBase;
        }
    }

    if (gPlayState != nullptr) {
        gPlayState->pauseCtx.state = 0;
        gPlayState->pauseCtx.debugEditor = 0;
    }

    Audio_Update();
}

// Ring buffer and state
static std::deque<DiffFrame> sRewindBuffer;
static bool sRewindInitialized = false;
static bool sIsRewinding = false;
static int sFrameCounter = 0;
static size_t sRewindMemUsage = 0;

static constexpr int DEFAULT_CAPTURE_INTERVAL = 10;
static constexpr int DEFAULT_MAX_MEMORY_MB = 64;

// Worker thread for diff computation on Core 2.
// The worker owns its own prev-snapshot copies and the incoming heap copy buffer.
// This avoids any shared mutable state between main and worker threads
// (except the job handoff via sPendingJob and the result buffer via sRewindBuffer).
static std::thread sWorkerThread;
static std::mutex sWorkerMutex;
static std::condition_variable sWorkerCV;
static std::atomic<bool> sWorkerRunning{ false };
static std::mutex sBufferMutex;

// Job handed from main thread to worker: contains a snapshot of the live heaps
struct CaptureJob {
    // Pre-allocated persistent buffers (resized once, reused)
    std::vector<uint8_t> sysHeapCopy;
    std::vector<uint8_t> audioHeapCopy;
    RewindSmallState smallState;
    bool ready = false;
};

static CaptureJob sJobBuffer; // Persistent, reused each capture
static bool sJobPending = false;

// Worker's own copies of previous frame for diffing (no sharing with main thread)
static std::vector<uint8_t> sWorkerPrevSys;
static std::vector<uint8_t> sWorkerPrevAudio;
static bool sWorkerHasPrev = false;

static void WorkerThreadFunc() {
#ifdef __SWITCH__
    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, (1U << 2));
    if (R_FAILED(rc)) {
        SPDLOG_WARN("[2S2H] Rewind: Failed to pin worker to Core 2 (rc=0x{:X})", rc);
    }
#endif

    while (sWorkerRunning.load()) {
        CaptureJob localJob;
        {
            std::unique_lock<std::mutex> lock(sWorkerMutex);
            sWorkerCV.wait(lock, [] { return sJobPending || !sWorkerRunning.load(); });
            if (!sWorkerRunning.load()) {
                break;
            }
            if (!sJobPending) {
                continue;
            }
            // Move data out of shared job buffer under lock
            localJob.sysHeapCopy.swap(sJobBuffer.sysHeapCopy);
            localJob.audioHeapCopy.swap(sJobBuffer.audioHeapCopy);
            localJob.smallState = sJobBuffer.smallState;
            sJobPending = false;
        }

        // First frame: just store as the baseline, no diff to compute
        if (!sWorkerHasPrev) {
            sWorkerPrevSys = std::move(localJob.sysHeapCopy);
            sWorkerPrevAudio = std::move(localJob.audioHeapCopy);
            sWorkerHasPrev = true;
            continue;
        }

        // Compute diff between worker's previous snapshot and the new capture
        DiffFrame frame;
        if (!ComputePageDiff(sWorkerPrevSys.data(), localJob.sysHeapCopy.data(), SYSTEM_HEAP_SIZE,
                             frame.sysPageIndices, frame.sysPageData) ||
            !ComputePageDiff(sWorkerPrevAudio.data(), localJob.audioHeapCopy.data(), AUDIO_HEAP_SIZE,
                             frame.audioPageIndices, frame.audioPageData)) {
            continue;
        }

        frame.smallState = localJob.smallState;
        size_t frameBytes = frame.GetBytes();

        int maxMemMB = CVarGetInteger("gCheats.RewindMaxMemoryMB", DEFAULT_MAX_MEMORY_MB);
        size_t maxMemBytes = static_cast<size_t>(maxMemMB) * 1024 * 1024;

        {
            std::lock_guard<std::mutex> lock(sBufferMutex);
            while (!sRewindBuffer.empty() && sRewindMemUsage + frameBytes > maxMemBytes) {
                sRewindMemUsage -= sRewindBuffer.front().GetBytes();
                sRewindBuffer.pop_front();
            }
            sRewindMemUsage += frameBytes;
            sRewindBuffer.push_back(std::move(frame));
        }

        // Update worker's previous snapshots
        sWorkerPrevSys.swap(localJob.sysHeapCopy);
        sWorkerPrevAudio.swap(localJob.audioHeapCopy);
    }
}

static void StartWorkerThread() {
    if (sWorkerRunning.load()) {
        return;
    }
    sWorkerRunning.store(true);
    sWorkerHasPrev = false;
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

    if (!sRewindInitialized) {
        sRewindInitialized = true;
        StartWorkerThread();
    }

    // Check if worker is still processing previous job
    {
        std::lock_guard<std::mutex> lock(sWorkerMutex);
        if (sJobPending) {
            return;
        }
    }

    // Ensure persistent buffers are allocated (first time only, then reused)
    try {
        if (sJobBuffer.sysHeapCopy.size() != SYSTEM_HEAP_SIZE) {
            sJobBuffer.sysHeapCopy.resize(SYSTEM_HEAP_SIZE);
        }
        if (sJobBuffer.audioHeapCopy.size() != AUDIO_HEAP_SIZE) {
            sJobBuffer.audioHeapCopy.resize(AUDIO_HEAP_SIZE);
        }
    } catch (const std::bad_alloc&) {
        return;
    }

    // Copy live heaps into the job buffer (must happen on main thread for consistency)
    memcpy(sJobBuffer.sysHeapCopy.data(), gSystemHeap, SYSTEM_HEAP_SIZE);
    memcpy(sJobBuffer.audioHeapCopy.data(), gAudioHeap, AUDIO_HEAP_SIZE);
    CaptureSmallState(sJobBuffer.smallState);

    {
        std::lock_guard<std::mutex> lock(sWorkerMutex);
        sJobPending = true;
    }
    sWorkerCV.notify_one();
}

static bool RewindStep() {
    // Worker must be stopped before we touch the buffer and live heaps
    // (StopWorkerThread is a no-op if already stopped)
    StopWorkerThread();

    std::lock_guard<std::mutex> lock(sBufferMutex);

    if (sRewindBuffer.empty()) {
        return false;
    }

    const DiffFrame& frame = sRewindBuffer.back();

    // Apply the page diffs (restores heap pages to the older state)
    ApplyPageDiff(gSystemHeap, frame.sysPageIndices, frame.sysPageData, SYSTEM_HEAP_SIZE);
    ApplyPageDiff(gAudioHeap, frame.audioPageIndices, frame.audioPageData, AUDIO_HEAP_SIZE);

    RestoreSmallState(frame.smallState);

    sRewindMemUsage -= frame.GetBytes();
    sRewindBuffer.pop_back();

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
        sJobPending = false;
        sJobBuffer.sysHeapCopy.clear();
        sJobBuffer.sysHeapCopy.shrink_to_fit();
        sJobBuffer.audioHeapCopy.clear();
        sJobBuffer.audioHeapCopy.shrink_to_fit();
    }
    sWorkerPrevSys.clear();
    sWorkerPrevSys.shrink_to_fit();
    sWorkerPrevAudio.clear();
    sWorkerPrevAudio.shrink_to_fit();
    sWorkerHasPrev = false;
    sRewindInitialized = false;
    sFrameCounter = 0;
}

void RegisterRewind() {
    bool rewindEnabled = CVarGetInteger("gCheats.RewindEnabled", 0);

    if (!rewindEnabled) {
        RewindClear();
    }

    COND_HOOK(OnGameStateUpdate, rewindEnabled, []() {
        if (!gPlayState || sIsRewinding) {
            return;
        }
        RewindCapture();
    });

    COND_HOOK(OnPassPlayerInputs, rewindEnabled, [](Input* input) {
        if (!gPlayState) {
            sIsRewinding = false;
            return;
        }

        bool m1Held = CHECK_BTN_ALL(input->cur.button, BTN_CUSTOM_MODIFIER1);
        bool dpadLeftHeld = CHECK_BTN_ALL(input->cur.button, BTN_DLEFT);

        if (m1Held && dpadLeftHeld) {
            sIsRewinding = true;
            memset(input, 0, sizeof(Input));
            if (!RewindStep()) {
                Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGameOverlay()->TextDrawNotification(
                    1.0f, true, "rewind buffer empty");
            }
        } else {
            if (sIsRewinding) {
                sIsRewinding = false;
                // Restart worker with fresh baseline after rewind ends
                sRewindInitialized = false;
                sWorkerHasPrev = false;
            }
        }
    });
}

static RegisterShipInitFunc initRewind(RegisterRewind,
                                       { "gCheats.RewindEnabled", "gCheats.RewindCaptureInterval",
                                         "gCheats.RewindMaxMemoryMB" });

extern "C" void ProcessRewind() {
    // Rewind processing is handled entirely via hooks
}
