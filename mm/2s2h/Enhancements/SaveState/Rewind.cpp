#include "Rewind.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <new>
#include <vector>

#include "imgui.h"

#include "2s2h/BenPort.h"
#include "2s2h/ShipInit.hpp"
#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
#include "z64save.h"
#include "system_malloc.h"
#include "z64malloc.h"
#include "main.h"
#include "variables.h"
#include "functions.h"
#include "sequence.h"
#include "z64effect_ss.h"
#include "z64pause_menu.h"
#include "z64game.h"
extern Arena sZeldaArena;
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
    uintptr_t segmentsCopy[NUM_SEGMENTS];
    Arena systemArenaCopy;
    Arena zeldaArenaCopy;
    AudioContext audioCtxCopy;
    ActiveSequence activeSeqsCopy[NUM_SEQ_PLAYERS];
    ActorOverlay actorOverlayTableCopy[ACTOR_ID_MAX];
    EffectSsOverlay effectSsOverlayTableCopy[EFFECT_SS_TYPE_MAX];
    KaleidoMgrOverlay kaleidoMgrOverlayTableCopy[KALEIDO_OVL_MAX];
    GameStateOverlay gameStateOverlayTableCopy[GAMESTATE_ID_MAX];
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
            } catch (const std::bad_alloc&) { return false; }
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
    memcpy(state.segmentsCopy, gSegments, sizeof(gSegments));
    memcpy(&state.systemArenaCopy, &gSystemArena, sizeof(Arena));
    memcpy(&state.zeldaArenaCopy, &sZeldaArena, sizeof(Arena));
    memcpy(&state.audioCtxCopy, &gAudioCtx, sizeof(AudioContext));
    memcpy(state.activeSeqsCopy, gActiveSeqs, sizeof(ActiveSequence) * NUM_SEQ_PLAYERS);
    memcpy(state.actorOverlayTableCopy, gActorOverlayTable, sizeof(gActorOverlayTable));
    memcpy(state.effectSsOverlayTableCopy, gEffectSsOverlayTable, sizeof(gEffectSsOverlayTable));
    memcpy(state.kaleidoMgrOverlayTableCopy, gKaleidoMgrOverlayTable, sizeof(gKaleidoMgrOverlayTable));
    memcpy(state.gameStateOverlayTableCopy, gGameStateOverlayTable, sizeof(gGameStateOverlayTable));
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
    memcpy(gSegments, state.segmentsCopy, sizeof(gSegments));
    memcpy(&gSystemArena, &state.systemArenaCopy, sizeof(Arena));
    memcpy(&sZeldaArena, &state.zeldaArenaCopy, sizeof(Arena));
    memcpy(&gAudioCtx, &state.audioCtxCopy, sizeof(AudioContext));
    memcpy(gActiveSeqs, state.activeSeqsCopy, sizeof(ActiveSequence) * NUM_SEQ_PLAYERS);
    memcpy(gActorOverlayTable, state.actorOverlayTableCopy, sizeof(gActorOverlayTable));
    memcpy(gEffectSsOverlayTable, state.effectSsOverlayTableCopy, sizeof(gEffectSsOverlayTable));
    memcpy(gKaleidoMgrOverlayTable, state.kaleidoMgrOverlayTableCopy, sizeof(gKaleidoMgrOverlayTable));
    memcpy(gGameStateOverlayTable, state.gameStateOverlayTableCopy, sizeof(gGameStateOverlayTable));

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
static PlayState* sLastPlayState = nullptr;
static int sRewindTotalFrames = 0;
static int sRewindPosition = 0;

// Baseline snapshots for diffing (one copy of each heap, ~52 MB total)
static std::vector<uint8_t> sBaselineSys;
static std::vector<uint8_t> sBaselineAudio;
static RewindSmallState sBaselineSmallState;
static bool sHasBaseline = false;

// ImGui progress bar overlay shown during rewind
class RewindOverlay : public Ship::GuiWindow {
  public:
    using Ship::GuiWindow::GuiWindow;
    void InitElement() override {}
    void DrawElement() override {}
    void UpdateElement() override {}
    void Draw() override {
        if (!sIsRewinding || sRewindTotalFrames <= 0) {
            return;
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        float barWidth = 300.0f;
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + (viewport->WorkSize.x - barWidth) / 2,
                                       viewport->WorkPos.y + viewport->WorkSize.y - 50),
                                ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(barWidth, 0), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.65f));
        ImGui::Begin("##RewindProgress", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);
        float progress = static_cast<float>(sRewindPosition) / static_cast<float>(sRewindTotalFrames);
        char overlay[64];
        snprintf(overlay, sizeof(overlay), "%d / %d", sRewindPosition, sRewindTotalFrames);
        ImGui::ProgressBar(progress, ImVec2(barWidth - 16, 0), overlay);
        ImGui::End();
        ImGui::PopStyleColor();
    }
};
static std::shared_ptr<RewindOverlay> sRewindOverlay;

static void RewindCapture() {
    // Only capture during actual gameplay (not title screen, file select, etc.)
    if (!gPlayState || !gSystemHeap || !gAudioHeap || gSaveContext.gameMode != GAMEMODE_NORMAL ||
        GET_PLAYER(gPlayState) == NULL) {
        return;
    }

    // Skip capturing when UI is open (pause menu, text boxes, transitions)
    // Restoring UI state causes visual glitches and broken menu rendering.
    if (gPlayState->pauseCtx.state != PAUSE_STATE_OFF || gPlayState->msgCtx.msgMode != MSGMODE_NONE ||
        gPlayState->transitionMode != TRANS_MODE_OFF) {
        return;
    }

    // Scene changed: flush old diffs (they belong to the previous scene's heaps)
    if (gPlayState != sLastPlayState) {
        sLastPlayState = gPlayState;
        sRewindBuffer.clear();
        sRewindMemUsage = 0;
        sHasBaseline = false;
        sFrameCounter = 0;
        sRewindTotalFrames = 0;
        sRewindPosition = 0;
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
    } catch (const std::bad_alloc&) { return; }

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

// Apply the most recent diff frame for one rewind step, then pop it.
static void RewindApply() {
    if (sRewindBuffer.empty()) {
        Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGameOverlay()->TextDrawNotification(
            1.0f, true, "rewind buffer empty");
        return;
    }

    // Save freshly-read controller input before heap restore — gPlayState->state.input
    // lives on the system heap and would be overwritten with old captured input, causing
    // OnPassPlayerInputs to think the user released the rewind buttons.
    Input savedInput[MAXCONTROLLERS];
    if (gPlayState) {
        memcpy(savedInput, gPlayState->state.input, sizeof(savedInput));
    }

    // Apply the back frame (most recent diff) to step back one capture
    const DiffFrame& frame = sRewindBuffer.back();
    ApplyPageDiff(gSystemHeap, frame.sysPageIndices, frame.sysPageData, SYSTEM_HEAP_SIZE);
    ApplyPageDiff(gAudioHeap, frame.audioPageIndices, frame.audioPageData, AUDIO_HEAP_SIZE);
    RestoreSmallState(frame.smallState);

    // Restore current controller input so OnPassPlayerInputs sees actual button state
    if (gPlayState) {
        memcpy(gPlayState->state.input, savedInput, sizeof(savedInput));
    }

    // Pop the applied frame and update tracking
    sRewindMemUsage -= sRewindBuffer.back().GetBytes();
    sRewindBuffer.pop_back();
    sRewindPosition++;
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
    sLastPlayState = nullptr;
    sRewindTotalFrames = 0;
    sRewindPosition = 0;
}

void RegisterRewind() {
    bool rewindEnabled = CVarGetInteger("gCheats.RewindEnabled", 0);

    if (!rewindEnabled) {
        RewindClear();
    }

    // Register ImGui overlay window (once)
    if (!sRewindOverlay) {
        auto context = Ship::Context::GetInstance();
        if (context && context->GetWindow() && context->GetWindow()->GetGui()) {
            sRewindOverlay = std::make_shared<RewindOverlay>("gWindows.RewindOverlay", "Rewind Overlay");
            context->GetWindow()->GetGui()->AddGuiWindow(sRewindOverlay);
            sRewindOverlay->Show();
        }
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
                sRewindTotalFrames = static_cast<int>(sRewindBuffer.size());
                sRewindPosition = 0;
            }
            sRewindRequested.store(true);
            memset(input, 0, sizeof(Input));
        } else if (sIsRewinding) {
            // Rewind released: keep remaining buffer, refresh baseline to current state
            sIsRewinding = false;
            sRewindPosition = 0;
            if (gSystemHeap && gAudioHeap && sBaselineSys.size() == SYSTEM_HEAP_SIZE &&
                sBaselineAudio.size() == AUDIO_HEAP_SIZE) {
                memcpy(sBaselineSys.data(), gSystemHeap, SYSTEM_HEAP_SIZE);
                memcpy(sBaselineAudio.data(), gAudioHeap, AUDIO_HEAP_SIZE);
                CaptureSmallState(sBaselineSmallState);
            }
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

static RegisterShipInitFunc initRewind(RegisterRewind, { "gCheats.RewindEnabled", "gCheats.RewindCaptureInterval",
                                                         "gCheats.RewindMaxMemoryMB" });
