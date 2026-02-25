#include "Rewind.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
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
static constexpr int DEFAULT_CAPTURE_INTERVAL = 10;
static constexpr int DEFAULT_MAX_MEMORY_MB = 64;
static constexpr int MIN_MAX_MEMORY_MB = 16;
static constexpr int MAX_MAX_MEMORY_MB = 256;

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
        // Use capacity() to track actual heap allocation (vectors may over-allocate)
        return sysPageData.capacity() + audioPageData.capacity() + sizeof(RewindSmallState) +
               sysPageIndices.capacity() * sizeof(uint32_t) + audioPageIndices.capacity() * sizeof(uint32_t);
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
    if (sMatrixStack != nullptr) {
        memcpy(&state.mtxStackCopy, sMatrixStack, sizeof(MtxF) * MATRIX_STACK_SIZE);
    }
    if (sCurrentMatrix != nullptr) {
        memcpy(&state.currentMtxCopy, sCurrentMatrix, sizeof(MtxF));
    }
    memcpy(&state.audioCtxCopy, &gAudioCtx, sizeof(AudioContext));
    memcpy(&state.activeSeqsCopy, gActiveSeqs, sizeof(ActiveSequence) * NUM_SEQ_PLAYERS);

    for (int i = 0; i < NUM_SEQ_PLAYERS; i++) {
        SeqScriptState* src = &gAudioCtx.seqPlayers[i].scriptState;
        SeqScriptState* dst = &state.seqScriptStateCopy[i];
        dst->value = src->value;
        dst->depth = src->depth;
        memcpy(dst->remLoopIters, src->remLoopIters, sizeof(dst->remLoopIters));
        // Unrelocate pointers: store as offsets from gAudioHeap
        // Use UINTPTR_MAX as sentinel for invalid/out-of-range pointers
        uintptr_t heapBase = (uintptr_t)gAudioHeap;
        uintptr_t heapEnd = heapBase + AUDIO_HEAP_SIZE;
        uintptr_t pc = (uintptr_t)src->pc;
        dst->pc = (pc >= heapBase && pc < heapEnd) ? (u8*)(pc - heapBase) : (u8*)UINTPTR_MAX;
        for (int j = 0; j < 4; j++) {
            uintptr_t sp = (uintptr_t)src->stack[j];
            dst->stack[j] = (sp >= heapBase && sp < heapEnd) ? (u8*)(sp - heapBase) : (u8*)UINTPTR_MAX;
        }
    }
}

static void RestoreSmallState(const RewindSmallState& state) {
    memcpy(&gSaveContext, &state.saveContextCopy, sizeof(SaveContext));
    memcpy(&sLightsBuffer, &state.lightBufferCopy, sizeof(LightsBuffer));
    if (sMatrixStack != nullptr) {
        memcpy(sMatrixStack, &state.mtxStackCopy, sizeof(MtxF) * MATRIX_STACK_SIZE);
    }
    if (sCurrentMatrix != nullptr) {
        memcpy(sCurrentMatrix, &state.currentMtxCopy, sizeof(MtxF));
    }
    memcpy(&gAudioCtx, &state.audioCtxCopy, sizeof(AudioContext));
    memcpy(gActiveSeqs, &state.activeSeqsCopy, sizeof(ActiveSequence) * NUM_SEQ_PLAYERS);

    for (int i = 0; i < NUM_SEQ_PLAYERS; i++) {
        const SeqScriptState* src = &state.seqScriptStateCopy[i];
        SeqScriptState* dst = &gAudioCtx.seqPlayers[i].scriptState;
        dst->value = src->value;
        dst->depth = src->depth;
        memcpy(dst->remLoopIters, src->remLoopIters, sizeof(dst->remLoopIters));
        // Relocate offsets back to absolute pointers
        // UINTPTR_MAX sentinel means the pointer was invalid/null → restore as nullptr
        uintptr_t heapBase = (uintptr_t)gAudioHeap;
        uintptr_t pcOff = (uintptr_t)src->pc;
        if (pcOff == UINTPTR_MAX || pcOff >= AUDIO_HEAP_SIZE) {
            dst->pc = nullptr;
        } else {
            dst->pc = (u8*)(heapBase + pcOff);
        }
        for (int j = 0; j < 4; j++) {
            uintptr_t spOff = (uintptr_t)src->stack[j];
            if (spOff == UINTPTR_MAX || spOff >= AUDIO_HEAP_SIZE) {
                dst->stack[j] = nullptr;
            } else {
                dst->stack[j] = (u8*)(heapBase + spOff);
            }
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
static std::atomic<bool> sRewindRequested{ false }; // Set by input hook, consumed by safe-point hook
static int sFrameCounter = 0;
static size_t sRewindMemUsage = 0;
static PlayState* sLastPlayState = nullptr; // Track scene transitions

// Worker thread for diff computation on Core 2.
// Uses double-buffering: main thread writes to sJobBuffer, worker reads from sWorkerBuffer.
// Worker owns its own prev-snapshot copies for diffing.
static std::thread sWorkerThread;
static std::mutex sWorkerMutex;
static std::condition_variable sWorkerCV;
static std::condition_variable sPauseAckCV; // For PauseWorker to wait on
static std::atomic<bool> sWorkerRunning{ false };
static std::atomic<bool> sWorkerPaused{ false };  // Cooperative pause for rewind steps
static std::atomic<bool> sWorkerAckPause{ false }; // Worker acknowledges pause
static std::mutex sBufferMutex;

// Double-buffered job: main thread writes to sJobBuffer, worker copies from it
struct CaptureJob {
    std::vector<uint8_t> sysHeapCopy;
    std::vector<uint8_t> audioHeapCopy;
    RewindSmallState smallState;
};

static CaptureJob sJobBuffer; // Main thread writes here (persistent, pre-allocated)
static bool sJobPending = false;

// Worker's own copies of previous frame for diffing (no sharing with main thread)
static std::vector<uint8_t> sWorkerPrevSys;
static std::vector<uint8_t> sWorkerPrevAudio;
static RewindSmallState sWorkerPrevSmallState; // Previous frame's small state for diff
static std::atomic<bool> sWorkerHasPrev{ false };
// Worker's own copy buffers (allocated once, reused)
static std::vector<uint8_t> sWorkerSysCopy;
static std::vector<uint8_t> sWorkerAudioCopy;

static void WorkerThreadFunc() {
#ifdef __SWITCH__
    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, (1U << 2));
    if (R_FAILED(rc)) {
        SPDLOG_WARN("[2S2H] Rewind: Failed to pin worker to Core 2 (rc=0x{:X})", rc);
    }
#endif

    RewindSmallState capturedSmallState{};

    while (sWorkerRunning.load()) {
        {
            std::unique_lock<std::mutex> lock(sWorkerMutex);
            sWorkerCV.wait(lock, [] {
                return sJobPending || sWorkerPaused.load() || !sWorkerRunning.load();
            });

            if (!sWorkerRunning.load()) {
                break;
            }

            // Handle cooperative pause: acknowledge and wait until unpaused
            if (sWorkerPaused.load()) {
                sWorkerAckPause.store(true);
                sPauseAckCV.notify_one(); // Wake up PauseWorker
                sWorkerCV.wait(lock, [] { return !sWorkerPaused.load() || !sWorkerRunning.load(); });
                sWorkerAckPause.store(false);
                if (!sWorkerRunning.load()) {
                    break;
                }
                continue;
            }

            if (!sJobPending) {
                continue;
            }

            // Copy data from shared job buffer (preserving sJobBuffer's capacity)
            try {
                if (sWorkerSysCopy.size() != SYSTEM_HEAP_SIZE) {
                    sWorkerSysCopy.resize(SYSTEM_HEAP_SIZE);
                }
                if (sWorkerAudioCopy.size() != AUDIO_HEAP_SIZE) {
                    sWorkerAudioCopy.resize(AUDIO_HEAP_SIZE);
                }
            } catch (const std::bad_alloc&) {
                sJobPending = false;
                continue;
            }
            memcpy(sWorkerSysCopy.data(), sJobBuffer.sysHeapCopy.data(), SYSTEM_HEAP_SIZE);
            memcpy(sWorkerAudioCopy.data(), sJobBuffer.audioHeapCopy.data(), AUDIO_HEAP_SIZE);
            capturedSmallState = sJobBuffer.smallState;
            sJobPending = false;
        }

        // First frame: just store as the baseline, no diff to compute
        if (!sWorkerHasPrev.load()) {
            sWorkerPrevSys.swap(sWorkerSysCopy);
            sWorkerPrevAudio.swap(sWorkerAudioCopy);
            sWorkerPrevSmallState = capturedSmallState;
            sWorkerHasPrev.store(true);
            continue;
        }

        // Compute diff between worker's previous snapshot and the new capture
        DiffFrame frame;
        if (!ComputePageDiff(sWorkerPrevSys.data(), sWorkerSysCopy.data(), SYSTEM_HEAP_SIZE,
                             frame.sysPageIndices, frame.sysPageData) ||
            !ComputePageDiff(sWorkerPrevAudio.data(), sWorkerAudioCopy.data(), AUDIO_HEAP_SIZE,
                             frame.audioPageIndices, frame.audioPageData)) {
            continue;
        }

        // Store the PREVIOUS small state with the diff (matches the old heap pages)
        frame.smallState = sWorkerPrevSmallState;
        size_t frameBytes = frame.GetBytes();

        int maxMemMB = CVarGetInteger("gCheats.RewindMaxMemoryMB", DEFAULT_MAX_MEMORY_MB);
        maxMemMB = std::clamp(maxMemMB, MIN_MAX_MEMORY_MB, MAX_MAX_MEMORY_MB);
        size_t maxMemBytes = static_cast<size_t>(maxMemMB) * 1024 * 1024;

        // Skip storing frames larger than the entire memory cap
        if (frameBytes <= maxMemBytes) {
            std::lock_guard<std::mutex> lock(sBufferMutex);
            while (!sRewindBuffer.empty() && sRewindMemUsage + frameBytes > maxMemBytes) {
                sRewindMemUsage -= sRewindBuffer.front().GetBytes();
                sRewindBuffer.pop_front();
            }
            sRewindMemUsage += frameBytes;
            sRewindBuffer.push_back(std::move(frame));
        }

        // Update worker's previous snapshots (swap to reuse capacity)
        sWorkerPrevSys.swap(sWorkerSysCopy);
        sWorkerPrevAudio.swap(sWorkerAudioCopy);
        sWorkerPrevSmallState = capturedSmallState;
    }
}

static void StartWorkerThread() {
    if (sWorkerRunning.load()) {
        return;
    }
    sWorkerRunning.store(true);
    sWorkerPaused.store(false);
    sWorkerAckPause.store(false);
    sWorkerHasPrev.store(false);
    sWorkerThread = std::thread(WorkerThreadFunc);
}

static void StopWorkerThread() {
    if (!sWorkerRunning.load()) {
        return;
    }
    sWorkerRunning.store(false);
    sWorkerPaused.store(false);
    sWorkerCV.notify_one();
    if (sWorkerThread.joinable()) {
        sWorkerThread.join();
    }
}

// Cooperative pause: ask worker to pause and wait for acknowledgement.
// Uses condition variable instead of busy-wait to avoid spinning on the main thread.
static void PauseWorker() {
    if (!sWorkerRunning.load()) {
        return;
    }
    sWorkerPaused.store(true);
    sWorkerCV.notify_one();
    // Wait for worker to acknowledge via condition variable
    std::unique_lock<std::mutex> lock(sWorkerMutex);
    sPauseAckCV.wait(lock, [] { return sWorkerAckPause.load() || !sWorkerRunning.load(); });
}

static void ResumeWorker() {
    sWorkerPaused.store(false);
    sWorkerCV.notify_one();
}

static void RewindCapture() {
    if (!gPlayState) {
        return;
    }

    // Detect scene transitions: if gPlayState pointer changed, the scene has been
    // torn down and re-initialized. Any existing diffs are from the old scene and
    // would corrupt state if applied to the new scene.
    if (gPlayState != sLastPlayState) {
        sLastPlayState = gPlayState;
        // Flush the ring buffer and reset the worker baseline
        {
            std::lock_guard<std::mutex> lock(sBufferMutex);
            sRewindBuffer.clear();
            sRewindMemUsage = 0;
        }
        sWorkerHasPrev.store(false);
        sFrameCounter = 0;
    }

    int captureInterval = CVarGetInteger("gCheats.RewindCaptureInterval", DEFAULT_CAPTURE_INTERVAL);
    captureInterval = std::clamp(captureInterval, 1, 30);

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
    // Cooperatively pause the worker instead of joining (avoids blocking if mid-diff)
    PauseWorker();

    std::lock_guard<std::mutex> lock(sBufferMutex);

    if (sRewindBuffer.empty()) {
        ResumeWorker();
        return false;
    }

    const DiffFrame& frame = sRewindBuffer.back();

    // Apply the page diffs (restores heap pages to the older state)
    ApplyPageDiff(gSystemHeap, frame.sysPageIndices, frame.sysPageData, SYSTEM_HEAP_SIZE);
    ApplyPageDiff(gAudioHeap, frame.audioPageIndices, frame.audioPageData, AUDIO_HEAP_SIZE);

    RestoreSmallState(frame.smallState);

    sRewindMemUsage -= frame.GetBytes();
    sRewindBuffer.pop_back();

    // Don't resume worker during rewind; it will be restarted when rewind ends
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
    sWorkerSysCopy.clear();
    sWorkerSysCopy.shrink_to_fit();
    sWorkerAudioCopy.clear();
    sWorkerAudioCopy.shrink_to_fit();
    sWorkerHasPrev.store(false);
    sRewindInitialized = false;
    sIsRewinding = false;
    sRewindRequested.store(false);
    sFrameCounter = 0;
    sLastPlayState = nullptr;
}

void RegisterRewind() {
    bool rewindEnabled = CVarGetInteger("gCheats.RewindEnabled", 0);

    if (!rewindEnabled) {
        RewindClear();
    }

    // Input hook: detect M1+DPad Left and suppress input, set rewind flag
    COND_HOOK(OnPassPlayerInputs, rewindEnabled, [](Input* input) {
        if (!gPlayState) {
            // Play state torn down: fully reset rewind system and worker/buffers
            RewindClear();
            return;
        }

        bool m1Held = CHECK_BTN_ALL(input->cur.button, BTN_CUSTOM_MODIFIER1);
        bool dpadLeftHeld = CHECK_BTN_ALL(input->cur.button, BTN_DLEFT);

        if (m1Held && dpadLeftHeld) {
            sIsRewinding = true;
            sRewindRequested.store(true);
            memset(input, 0, sizeof(Input));
        } else {
            if (sIsRewinding) {
                sIsRewinding = false;
                // Clear rewind history and reset baseline after rewind ends.
                // Old diffs from the pre-rewind timeline would be invalid since
                // the game state has diverged.
                {
                    std::lock_guard<std::mutex> lock(sBufferMutex);
                    sRewindBuffer.clear();
                    sRewindMemUsage = 0;
                }
                sRewindInitialized = false;
                sWorkerHasPrev.store(false);
                ResumeWorker();
            }
        }
    });

    // Safe-point hook: perform actual rewind at frame boundary (before main)
    COND_HOOK(OnGameStateMainStart, rewindEnabled, []() {
        if (sRewindRequested.exchange(false)) {
            if (!RewindStep()) {
                Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGameOverlay()->TextDrawNotification(
                    1.0f, true, "rewind buffer empty");
            }
        }
    });

    // Capture hook: record diffs when not rewinding
    COND_HOOK(OnGameStateUpdate, rewindEnabled, []() {
        if (!gPlayState) {
            // Scene torn down: flush rewind buffer to prevent stale cross-scene diffs.
            // Don't call full RewindClear() here — just reset the buffer and baseline.
            if (sLastPlayState != nullptr) {
                {
                    std::lock_guard<std::mutex> lock(sBufferMutex);
                    sRewindBuffer.clear();
                    sRewindMemUsage = 0;
                }
                sWorkerHasPrev.store(false);
                sLastPlayState = nullptr;
            }
            return;
        }
        if (sIsRewinding) {
            return;
        }
        RewindCapture();
    });
}

static RegisterShipInitFunc initRewind(RegisterRewind,
                                       { "gCheats.RewindEnabled", "gCheats.RewindCaptureInterval",
                                         "gCheats.RewindMaxMemoryMB" });

extern "C" void ProcessRewind() {
    // Rewind processing is handled entirely via hooks
}
