#include "Rewind.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <new>
#include <vector>

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

// Compare heap against baseline, save old pages into diff, update baseline in-place.
// Single pass: memcmp to detect changes, then memcpy to save old + update baseline.
static bool ComputeAndUpdateDiff(uint8_t* baseline, const uint8_t* current, size_t heapSize,
                                 std::vector<uint32_t>& indices, std::vector<uint8_t>& pageData) {
    indices.clear();
    pageData.clear();
    size_t pageCount = (heapSize + PAGE_SIZE - 1) / PAGE_SIZE;
    for (size_t i = 0; i < pageCount; i++) {
        size_t offset = i * PAGE_SIZE;
        size_t len = std::min(PAGE_SIZE, heapSize - offset);
        if (memcmp(baseline + offset, current + offset, len) != 0) {
            indices.push_back(static_cast<uint32_t>(i));
            size_t pos = pageData.size();
            try {
                pageData.resize(pos + PAGE_SIZE, 0);
            } catch (const std::bad_alloc&) {
                return false;
            }
            // Save old baseline page (for rewind)
            memcpy(pageData.data() + pos, baseline + offset, len);
            // Update baseline to current
            memcpy(baseline + offset, current + offset, len);
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

// --- All state is main-thread-only (no worker thread) ---
static std::deque<DiffFrame> sRewindBuffer;
static size_t sRewindMemUsage = 0;
static bool sIsRewinding = false;
static std::atomic<bool> sRewindRequested{ false };
static int sFrameCounter = 0;
static int sRewindStepCounter = 0;
static PlayState* sLastPlayState = nullptr;

// Baseline snapshots for diffing (one copy of each heap, ~52 MB total)
static std::vector<uint8_t> sBaselineSys;
static std::vector<uint8_t> sBaselineAudio;
static RewindSmallState sBaselineSmallState;
static bool sHasBaseline = false;

static void RewindCapture() {
    // Only capture during actual gameplay (not title screen, file select, etc.)
    if (!gPlayState || !gSystemHeap || !gAudioHeap || gSaveContext.gameMode != GAMEMODE_NORMAL ||
        GET_PLAYER(gPlayState) == NULL) {
        return;
    }

    // Scene changed: flush old diffs (they belong to the previous scene's heaps)
    if (gPlayState != sLastPlayState) {
        sLastPlayState = gPlayState;
        sRewindBuffer.clear();
        sRewindMemUsage = 0;
        sHasBaseline = false;
        sFrameCounter = 0;
    }

    int captureInterval = CVarGetInteger("gCheats.RewindCaptureInterval", DEFAULT_CAPTURE_INTERVAL);
    captureInterval = std::clamp(captureInterval, 1, 30);

    sFrameCounter++;
    if (sFrameCounter % captureInterval != 0) {
        return;
    }

    // Allocate baseline buffers on first capture
    try {
        if (sBaselineSys.size() != SYSTEM_HEAP_SIZE) {
            sBaselineSys.resize(SYSTEM_HEAP_SIZE);
        }
        if (sBaselineAudio.size() != AUDIO_HEAP_SIZE) {
            sBaselineAudio.resize(AUDIO_HEAP_SIZE);
        }
    } catch (const std::bad_alloc&) {
        return;
    }

    // First capture: just store baseline, no diff yet
    if (!sHasBaseline) {
        memcpy(sBaselineSys.data(), gSystemHeap, SYSTEM_HEAP_SIZE);
        memcpy(sBaselineAudio.data(), gAudioHeap, AUDIO_HEAP_SIZE);
        CaptureSmallState(sBaselineSmallState);
        sHasBaseline = true;
        return;
    }

    // Compute page-level diff inline (compare current heap vs baseline)
    // This also updates the baseline in-place for changed pages
    DiffFrame frame;
    if (!ComputeAndUpdateDiff(sBaselineSys.data(), gSystemHeap, SYSTEM_HEAP_SIZE, frame.sysPageIndices,
                              frame.sysPageData) ||
        !ComputeAndUpdateDiff(sBaselineAudio.data(), gAudioHeap, AUDIO_HEAP_SIZE, frame.audioPageIndices,
                              frame.audioPageData)) {
        return;
    }

    // Store the PREVIOUS small state (matches the old heap pages in the diff)
    frame.smallState = sBaselineSmallState;
    CaptureSmallState(sBaselineSmallState);

    size_t frameBytes = frame.GetBytes();

    int maxMemMB = CVarGetInteger("gCheats.RewindMaxMemoryMB", DEFAULT_MAX_MEMORY_MB);
    maxMemMB = std::clamp(maxMemMB, MIN_MAX_MEMORY_MB, MAX_MAX_MEMORY_MB);
    size_t maxMemBytes = static_cast<size_t>(maxMemMB) * 1024 * 1024;

    if (frameBytes > maxMemBytes) {
        return;
    }

    // Evict old frames to stay under memory cap
    while (!sRewindBuffer.empty() && sRewindMemUsage + frameBytes > maxMemBytes) {
        sRewindMemUsage -= sRewindBuffer.front().GetBytes();
        sRewindBuffer.pop_front();
    }
    sRewindMemUsage += frameBytes;
    sRewindBuffer.push_back(std::move(frame));
}

// Apply current back frame to freeze game state, advancing to next diff every captureInterval frames.
// This matches rewind consumption rate to capture rate so the buffer doesn't drain instantly.
static void RewindApply() {
    if (sRewindBuffer.empty()) {
        Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGameOverlay()->TextDrawNotification(
            1.0f, true, "rewind buffer empty");
        return;
    }

    int captureInterval = CVarGetInteger("gCheats.RewindCaptureInterval", DEFAULT_CAPTURE_INTERVAL);
    captureInterval = std::clamp(captureInterval, 1, 30);

    // Advance to next diff every captureInterval frames (matching capture rate)
    sRewindStepCounter++;
    if (sRewindStepCounter >= captureInterval && sRewindBuffer.size() > 1) {
        sRewindStepCounter = 0;
        sRewindMemUsage -= sRewindBuffer.back().GetBytes();
        sRewindBuffer.pop_back();
    }

    // Re-apply current back frame every frame to prevent game-state drift
    // (the game loop still runs with zeroed input between rewind steps)
    const DiffFrame& frame = sRewindBuffer.back();
    ApplyPageDiff(gSystemHeap, frame.sysPageIndices, frame.sysPageData, SYSTEM_HEAP_SIZE);
    ApplyPageDiff(gAudioHeap, frame.audioPageIndices, frame.audioPageData, AUDIO_HEAP_SIZE);
    RestoreSmallState(frame.smallState);
}

static void RewindClear() {
    sRewindBuffer.clear();
    sRewindMemUsage = 0;
    sBaselineSys.clear();
    sBaselineSys.shrink_to_fit();
    sBaselineAudio.clear();
    sBaselineAudio.shrink_to_fit();
    sHasBaseline = false;
    sIsRewinding = false;
    sRewindRequested.store(false);
    sFrameCounter = 0;
    sRewindStepCounter = 0;
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
            if (!sIsRewinding) {
                sIsRewinding = true;
                sRewindStepCounter = 0;
            }
            sRewindRequested.store(true);
            memset(input, 0, sizeof(Input));
        } else if (sIsRewinding) {
            // Rewind released: discard history and rebuild baseline
            sIsRewinding = false;
            sRewindBuffer.clear();
            sRewindMemUsage = 0;
            sHasBaseline = false;
        }
    });

    // Safe-point hook: perform actual rewind at frame boundary
    COND_HOOK(OnGameStateMainStart, rewindEnabled, []() {
        if (sRewindRequested.exchange(false) && gPlayState && gSaveContext.gameMode == GAMEMODE_NORMAL) {
            RewindApply();
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
