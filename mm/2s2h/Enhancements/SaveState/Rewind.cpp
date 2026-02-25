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

// Small state data that lives outside the two main heaps.
// gAudioHeap is allocated once at boot and never moves, so all pointers inside
// AudioContext (seqPlayers[].scriptState.pc, channels[], etc.) remain valid after
// restoring the heap contents — no pointer relocation is needed.
struct RewindSmallState {
    SaveContext saveContextCopy;
    LightsBuffer lightBufferCopy;
    MtxF mtxStackCopy[MATRIX_STACK_SIZE];
    MtxF currentMtxCopy;
    AudioContext audioCtxCopy;
    ActiveSequence activeSeqsCopy[NUM_SEQ_PLAYERS];
};

struct DiffFrame {
    std::vector<uint32_t> sysPageIndices;
    std::vector<uint8_t> sysPageData;
    std::vector<uint32_t> audioPageIndices;
    std::vector<uint8_t> audioPageData;
    RewindSmallState smallState;

    size_t GetBytes() const {
        return sysPageData.capacity() + audioPageData.capacity() + sizeof(RewindSmallState) +
               sysPageIndices.capacity() * sizeof(uint32_t) + audioPageIndices.capacity() * sizeof(uint32_t);
    }
};

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
            memcpy(pageData.data() + pos, oldData + offset, len);
        }
    }
    return true;
}

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
        memcpy(state.mtxStackCopy, sMatrixStack, sizeof(MtxF) * MATRIX_STACK_SIZE);
    }
    if (sCurrentMatrix != nullptr) {
        memcpy(&state.currentMtxCopy, sCurrentMatrix, sizeof(MtxF));
    }
    memcpy(&state.audioCtxCopy, &gAudioCtx, sizeof(AudioContext));
    memcpy(state.activeSeqsCopy, gActiveSeqs, sizeof(ActiveSequence) * NUM_SEQ_PLAYERS);
}

static void RestoreSmallState(const RewindSmallState& state) {
    memcpy(&gSaveContext, &state.saveContextCopy, sizeof(SaveContext));
    memcpy(&sLightsBuffer, &state.lightBufferCopy, sizeof(LightsBuffer));
    if (sMatrixStack != nullptr) {
        memcpy(sMatrixStack, state.mtxStackCopy, sizeof(MtxF) * MATRIX_STACK_SIZE);
    }
    if (sCurrentMatrix != nullptr) {
        memcpy(sCurrentMatrix, &state.currentMtxCopy, sizeof(MtxF));
    }
    memcpy(&gAudioCtx, &state.audioCtxCopy, sizeof(AudioContext));
    memcpy(gActiveSeqs, state.activeSeqsCopy, sizeof(ActiveSequence) * NUM_SEQ_PLAYERS);

    if (gPlayState != nullptr) {
        gPlayState->pauseCtx.state = 0;
        gPlayState->pauseCtx.debugEditor = 0;
    }

    Audio_Update();
}

// --- Ring buffer (shared between main thread and worker) ---
static std::deque<DiffFrame> sRewindBuffer;
static std::mutex sBufferMutex;
static size_t sRewindMemUsage = 0;

// --- Main-thread-only state ---
static bool sIsRewinding = false;
static std::atomic<bool> sRewindRequested{ false };
static int sFrameCounter = 0;
static PlayState* sLastPlayState = nullptr;
static std::deque<DiffFrame> sRewindLocal; // Local copy used during rewind (main thread only)

// --- Worker thread (runs on Core 2) ---
static std::thread sWorkerThread;
static std::mutex sWorkerMutex;
static std::condition_variable sWorkerCV;
static std::atomic<bool> sWorkerRunning{ false };
static std::atomic<bool> sWorkerHasPrev{ false };

struct CaptureJob {
    std::vector<uint8_t> sysHeapCopy;
    std::vector<uint8_t> audioHeapCopy;
    RewindSmallState smallState;
};

static CaptureJob sJobBuffer;
static bool sJobPending = false; // Protected by sWorkerMutex

// Worker-owned buffers (never accessed by main thread)
static std::vector<uint8_t> sWorkerPrevSys;
static std::vector<uint8_t> sWorkerPrevAudio;
static std::vector<uint8_t> sWorkerSysCopy;
static std::vector<uint8_t> sWorkerAudioCopy;
static RewindSmallState sWorkerPrevSmallState;

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
            sWorkerCV.wait(lock, [] { return sJobPending || !sWorkerRunning.load(); });

            if (!sWorkerRunning.load()) {
                break;
            }
            if (!sJobPending) {
                continue;
            }

            // Copy from shared job buffer into worker-owned buffers
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

        // First capture: store as baseline
        if (!sWorkerHasPrev.load()) {
            sWorkerPrevSys.swap(sWorkerSysCopy);
            sWorkerPrevAudio.swap(sWorkerAudioCopy);
            sWorkerPrevSmallState = capturedSmallState;
            sWorkerHasPrev.store(true);
            continue;
        }

        // Compute page-level diff
        DiffFrame frame;
        if (!ComputePageDiff(sWorkerPrevSys.data(), sWorkerSysCopy.data(), SYSTEM_HEAP_SIZE,
                             frame.sysPageIndices, frame.sysPageData) ||
            !ComputePageDiff(sWorkerPrevAudio.data(), sWorkerAudioCopy.data(), AUDIO_HEAP_SIZE,
                             frame.audioPageIndices, frame.audioPageData)) {
            continue;
        }

        frame.smallState = sWorkerPrevSmallState;
        size_t frameBytes = frame.GetBytes();

        int maxMemMB = CVarGetInteger("gCheats.RewindMaxMemoryMB", DEFAULT_MAX_MEMORY_MB);
        maxMemMB = std::clamp(maxMemMB, MIN_MAX_MEMORY_MB, MAX_MAX_MEMORY_MB);
        size_t maxMemBytes = static_cast<size_t>(maxMemMB) * 1024 * 1024;

        if (frameBytes <= maxMemBytes) {
            std::lock_guard<std::mutex> lock(sBufferMutex);
            while (!sRewindBuffer.empty() && sRewindMemUsage + frameBytes > maxMemBytes) {
                sRewindMemUsage -= sRewindBuffer.front().GetBytes();
                sRewindBuffer.pop_front();
            }
            sRewindMemUsage += frameBytes;
            sRewindBuffer.push_back(std::move(frame));
        }

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
    sWorkerHasPrev.store(false);
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

    // Scene changed: flush old diffs (they belong to the previous scene's heaps)
    if (gPlayState != sLastPlayState) {
        sLastPlayState = gPlayState;
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

    if (!sWorkerRunning.load()) {
        StartWorkerThread();
    }

    // Skip if worker still processing previous job
    {
        std::lock_guard<std::mutex> lock(sWorkerMutex);
        if (sJobPending) {
            return;
        }
    }

    // Ensure persistent buffers are allocated
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

    memcpy(sJobBuffer.sysHeapCopy.data(), gSystemHeap, SYSTEM_HEAP_SIZE);
    memcpy(sJobBuffer.audioHeapCopy.data(), gAudioHeap, AUDIO_HEAP_SIZE);
    CaptureSmallState(sJobBuffer.smallState);

    {
        std::lock_guard<std::mutex> lock(sWorkerMutex);
        sJobPending = true;
    }
    sWorkerCV.notify_one();
}

// RewindStep pops from the local buffer (no worker interaction needed)
static bool RewindStep() {
    if (sRewindLocal.empty()) {
        return false;
    }

    const DiffFrame& frame = sRewindLocal.back();
    ApplyPageDiff(gSystemHeap, frame.sysPageIndices, frame.sysPageData, SYSTEM_HEAP_SIZE);
    ApplyPageDiff(gAudioHeap, frame.audioPageIndices, frame.audioPageData, AUDIO_HEAP_SIZE);
    RestoreSmallState(frame.smallState);
    sRewindLocal.pop_back();
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
    }
    sJobBuffer.sysHeapCopy.clear();
    sJobBuffer.sysHeapCopy.shrink_to_fit();
    sJobBuffer.audioHeapCopy.clear();
    sJobBuffer.audioHeapCopy.shrink_to_fit();
    sWorkerPrevSys.clear();
    sWorkerPrevSys.shrink_to_fit();
    sWorkerPrevAudio.clear();
    sWorkerPrevAudio.shrink_to_fit();
    sWorkerSysCopy.clear();
    sWorkerSysCopy.shrink_to_fit();
    sWorkerAudioCopy.clear();
    sWorkerAudioCopy.shrink_to_fit();
    sWorkerHasPrev.store(false);
    sRewindLocal.clear();
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

    // Input hook: detect M1+DPad Left, suppress game input, set rewind flag
    COND_HOOK(OnPassPlayerInputs, rewindEnabled, [](Input* input) {
        if (!gPlayState) {
            return;
        }

        bool m1Held = CHECK_BTN_ALL(input->cur.button, BTN_CUSTOM_MODIFIER1);
        bool dpadLeftHeld = CHECK_BTN_ALL(input->cur.button, BTN_DLEFT);

        if (m1Held && dpadLeftHeld) {
            // On first rewind frame: snapshot the shared buffer into a local copy
            // so the worker thread can't interfere during rewind
            if (!sIsRewinding) {
                sIsRewinding = true;
                std::lock_guard<std::mutex> lock(sBufferMutex);
                sRewindLocal = std::move(sRewindBuffer);
                sRewindBuffer.clear();
                sRewindMemUsage = 0;
            }
            sRewindRequested.store(true);
            memset(input, 0, sizeof(Input));
        } else if (sIsRewinding) {
            // Rewind released: discard everything and start fresh
            sIsRewinding = false;
            sRewindLocal.clear();
            {
                std::lock_guard<std::mutex> lock(sBufferMutex);
                sRewindBuffer.clear();
                sRewindMemUsage = 0;
            }
            sWorkerHasPrev.store(false);
        }
    });

    // Safe-point hook: perform actual rewind at frame boundary
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
