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

typedef struct {
    Vec3f unk_00;
    Vec3f unk_0C;
    s16 unk_18;
    s16 unk_1A;
} struct_801F58B0;

typedef struct BunnyEarKinematics {
    Vec3s rot;
    Vec3s angVel;
} BunnyEarKinematics;

extern struct_801F58B0 D_801F58B0[3][3];
extern Vec3f D_801F59B0[2];
extern s32 D_801F59C8[2];
extern BunnyEarKinematics sBunnyEarKinematics;
extern Vec3f* sPlayerCurBodyPartPos;
extern s32 D_801F59E0;
extern s32 sPlayerLod;
extern Vec3f sPlayerGetItemRefPos;
extern PlayerModelType sPlayerLeftHandType;
extern PlayerModelType sPlayerRightHandType;
extern s32 D_801C0958;
extern f32 sControlStickMagnitude;
extern s16 sControlStickAngle;
extern s16 sControlStickWorldYaw;
extern s32 sUpperBodyIsBusy;
extern FloorType sPlayerFloorType;
extern u32 sPlayerTouchedWallFlags;
extern ConveyorSpeed sPlayerConveyorSpeedIndex;
extern s16 sPlayerIsOnFloorConveyor;
extern s16 sPlayerConveyorYaw;
extern f32 sPlayerYDistToFloor;
extern FloorProperty sPrevFloorProperty;
extern s32 sShapeYawToTouchedWall;
extern s32 sWorldYawToTouchedWall;
extern s16 sFloorPitchShape;
extern s32 sSavedCurrentMask;
extern Vec3f sInteractWallCheckResult;
extern f32 D_80862B3C;
extern FloorEffect sPlayerFloorEffect;
extern Input* sPlayerControlInput;
extern s32 sPlayerUseHeldItem;
extern s32 sPlayerHeldItemButtonIsHeldDown;
extern AdjLightSettings D_80862B50;
extern s32 D_80862B6C;
extern f32 sWaterSpeedFactor;
extern f32 sInvWaterSpeedFactor;

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
    s16 playerActorProfileObjectIdCopy;
    u8 playerActorProfileObjectIdValidCopy;
    EffectSsOverlay effectSsOverlayTableCopy[EFFECT_SS_TYPE_MAX];
    KaleidoMgrOverlay kaleidoMgrOverlayTableCopy[KALEIDO_OVL_MAX];
    GameStateOverlay gameStateOverlayTableCopy[GAMESTATE_ID_MAX];
    struct_801F58B0 playerMaskTrailCopy[3][3];
    Vec3f playerBubblePosCopy[2];
    s32 playerBubbleTimerCopy[2];
    BunnyEarKinematics bunnyEarKinematicsCopy;
    Vec3f* playerCurBodyPartPosCopy;
    s32 playerModelDListBaseIndexCopy;
    s32 playerLodCopy;
    Vec3f playerGetItemRefPosCopy;
    PlayerModelType playerLeftHandTypeCopy;
    PlayerModelType playerRightHandTypeCopy;
    s32 playerModelResetFlagCopy;
    f32 playerControlStickMagnitudeCopy;
    s16 playerControlStickAngleCopy;
    s16 playerControlStickWorldYawCopy;
    s32 playerUpperBodyIsBusyCopy;
    FloorType playerFloorTypeCopy;
    u32 playerTouchedWallFlagsCopy;
    ConveyorSpeed playerConveyorSpeedIndexCopy;
    s16 playerIsOnFloorConveyorCopy;
    s16 playerConveyorYawCopy;
    f32 playerYDistToFloorCopy;
    FloorProperty prevFloorPropertyCopy;
    s32 shapeYawToTouchedWallCopy;
    s32 worldYawToTouchedWallCopy;
    s16 floorPitchShapeCopy;
    s32 savedCurrentMaskCopy;
    Vec3f interactWallCheckResultCopy;
    f32 playerInteractWallDistCopy;
    FloorEffect playerFloorEffectCopy;
    Input* playerControlInputCopy;
    s32 playerUseHeldItemCopy;
    s32 playerHeldItemButtonIsHeldDownCopy;
    AdjLightSettings playerAdjLightSettingsCopy;
    s32 playerSkelMoveFlagsCopy;
    f32 waterSpeedFactorCopy;
    f32 invWaterSpeedFactorCopy;
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
    if (gActorOverlayTable[ACTOR_PLAYER].profile != nullptr) {
        state.playerActorProfileObjectIdCopy = gActorOverlayTable[ACTOR_PLAYER].profile->objectId;
        state.playerActorProfileObjectIdValidCopy = 1;
    } else {
        state.playerActorProfileObjectIdCopy = 0;
        state.playerActorProfileObjectIdValidCopy = 0;
    }
    memcpy(state.effectSsOverlayTableCopy, gEffectSsOverlayTable, sizeof(gEffectSsOverlayTable));
    memcpy(state.kaleidoMgrOverlayTableCopy, gKaleidoMgrOverlayTable, sizeof(gKaleidoMgrOverlayTable));
    memcpy(state.gameStateOverlayTableCopy, gGameStateOverlayTable, sizeof(gGameStateOverlayTable));
    memcpy(state.playerMaskTrailCopy, D_801F58B0, sizeof(D_801F58B0));
    memcpy(state.playerBubblePosCopy, D_801F59B0, sizeof(D_801F59B0));
    memcpy(state.playerBubbleTimerCopy, D_801F59C8, sizeof(D_801F59C8));
    memcpy(&state.bunnyEarKinematicsCopy, &sBunnyEarKinematics, sizeof(BunnyEarKinematics));
    state.playerCurBodyPartPosCopy = sPlayerCurBodyPartPos;
    state.playerModelDListBaseIndexCopy = D_801F59E0;
    state.playerLodCopy = sPlayerLod;
    memcpy(&state.playerGetItemRefPosCopy, &sPlayerGetItemRefPos, sizeof(Vec3f));
    state.playerLeftHandTypeCopy = sPlayerLeftHandType;
    state.playerRightHandTypeCopy = sPlayerRightHandType;
    state.playerModelResetFlagCopy = D_801C0958;
    state.playerControlStickMagnitudeCopy = sControlStickMagnitude;
    state.playerControlStickAngleCopy = sControlStickAngle;
    state.playerControlStickWorldYawCopy = sControlStickWorldYaw;
    state.playerUpperBodyIsBusyCopy = sUpperBodyIsBusy;
    state.playerFloorTypeCopy = sPlayerFloorType;
    state.playerTouchedWallFlagsCopy = sPlayerTouchedWallFlags;
    state.playerConveyorSpeedIndexCopy = sPlayerConveyorSpeedIndex;
    state.playerIsOnFloorConveyorCopy = sPlayerIsOnFloorConveyor;
    state.playerConveyorYawCopy = sPlayerConveyorYaw;
    state.playerYDistToFloorCopy = sPlayerYDistToFloor;
    state.prevFloorPropertyCopy = sPrevFloorProperty;
    state.shapeYawToTouchedWallCopy = sShapeYawToTouchedWall;
    state.worldYawToTouchedWallCopy = sWorldYawToTouchedWall;
    state.floorPitchShapeCopy = sFloorPitchShape;
    state.savedCurrentMaskCopy = sSavedCurrentMask;
    memcpy(&state.interactWallCheckResultCopy, &sInteractWallCheckResult, sizeof(Vec3f));
    state.playerInteractWallDistCopy = D_80862B3C;
    state.playerFloorEffectCopy = sPlayerFloorEffect;
    state.playerControlInputCopy = sPlayerControlInput;
    state.playerUseHeldItemCopy = sPlayerUseHeldItem;
    state.playerHeldItemButtonIsHeldDownCopy = sPlayerHeldItemButtonIsHeldDown;
    memcpy(&state.playerAdjLightSettingsCopy, &D_80862B50, sizeof(AdjLightSettings));
    state.playerSkelMoveFlagsCopy = D_80862B6C;
    state.waterSpeedFactorCopy = sWaterSpeedFactor;
    state.invWaterSpeedFactorCopy = sInvWaterSpeedFactor;
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
    if (state.playerActorProfileObjectIdValidCopy && gActorOverlayTable[ACTOR_PLAYER].profile != nullptr) {
        gActorOverlayTable[ACTOR_PLAYER].profile->objectId = state.playerActorProfileObjectIdCopy;
    }
    memcpy(gEffectSsOverlayTable, state.effectSsOverlayTableCopy, sizeof(gEffectSsOverlayTable));
    memcpy(gKaleidoMgrOverlayTable, state.kaleidoMgrOverlayTableCopy, sizeof(gKaleidoMgrOverlayTable));
    memcpy(gGameStateOverlayTable, state.gameStateOverlayTableCopy, sizeof(gGameStateOverlayTable));
    memcpy(D_801F58B0, state.playerMaskTrailCopy, sizeof(D_801F58B0));
    memcpy(D_801F59B0, state.playerBubblePosCopy, sizeof(D_801F59B0));
    memcpy(D_801F59C8, state.playerBubbleTimerCopy, sizeof(D_801F59C8));
    memcpy(&sBunnyEarKinematics, &state.bunnyEarKinematicsCopy, sizeof(BunnyEarKinematics));
    sPlayerCurBodyPartPos = state.playerCurBodyPartPosCopy;
    D_801F59E0 = state.playerModelDListBaseIndexCopy;
    sPlayerLod = state.playerLodCopy;
    memcpy(&sPlayerGetItemRefPos, &state.playerGetItemRefPosCopy, sizeof(Vec3f));
    sPlayerLeftHandType = state.playerLeftHandTypeCopy;
    sPlayerRightHandType = state.playerRightHandTypeCopy;
    D_801C0958 = state.playerModelResetFlagCopy;
    sControlStickMagnitude = state.playerControlStickMagnitudeCopy;
    sControlStickAngle = state.playerControlStickAngleCopy;
    sControlStickWorldYaw = state.playerControlStickWorldYawCopy;
    sUpperBodyIsBusy = state.playerUpperBodyIsBusyCopy;
    sPlayerFloorType = state.playerFloorTypeCopy;
    sPlayerTouchedWallFlags = state.playerTouchedWallFlagsCopy;
    sPlayerConveyorSpeedIndex = state.playerConveyorSpeedIndexCopy;
    sPlayerIsOnFloorConveyor = state.playerIsOnFloorConveyorCopy;
    sPlayerConveyorYaw = state.playerConveyorYawCopy;
    sPlayerYDistToFloor = state.playerYDistToFloorCopy;
    sPrevFloorProperty = state.prevFloorPropertyCopy;
    sShapeYawToTouchedWall = state.shapeYawToTouchedWallCopy;
    sWorldYawToTouchedWall = state.worldYawToTouchedWallCopy;
    sFloorPitchShape = state.floorPitchShapeCopy;
    sSavedCurrentMask = state.savedCurrentMaskCopy;
    memcpy(&sInteractWallCheckResult, &state.interactWallCheckResultCopy, sizeof(Vec3f));
    D_80862B3C = state.playerInteractWallDistCopy;
    sPlayerFloorEffect = state.playerFloorEffectCopy;
    // Keep this pointer bound to the current PlayState input array. Restoring
    // stale historical pointers can leave player code dereferencing invalid input.
    sPlayerControlInput = (gPlayState != nullptr) ? gPlayState->state.input : state.playerControlInputCopy;
    sPlayerUseHeldItem = state.playerUseHeldItemCopy;
    sPlayerHeldItemButtonIsHeldDown = state.playerHeldItemButtonIsHeldDownCopy;
    memcpy(&D_80862B50, &state.playerAdjLightSettingsCopy, sizeof(AdjLightSettings));
    D_80862B6C = state.playerSkelMoveFlagsCopy;
    sWaterSpeedFactor = state.waterSpeedFactorCopy;
    sInvWaterSpeedFactor = state.invWaterSpeedFactorCopy;

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

// When rewind is released mid-buffer, new captures append after old history.
// This snapshot ensures we can restore an exact branch handoff state before
// crossing from appended frames back into older preserved frames.
static std::vector<uint8_t> sBranchBoundarySys;
static std::vector<uint8_t> sBranchBoundaryAudio;
static RewindSmallState sBranchBoundarySmallState;
static bool sHasBranchBoundary = false;
static size_t sBranchBoundarySplit = 0;

static void ResetBranchBoundary() {
    sHasBranchBoundary = false;
    sBranchBoundarySplit = 0;
}

static void CaptureBranchBoundary(size_t splitIndex) {
    if (splitIndex == 0 || !gSystemHeap || !gAudioHeap) {
        ResetBranchBoundary();
        return;
    }

    try {
        if (sBranchBoundarySys.size() != SYSTEM_HEAP_SIZE) {
            sBranchBoundarySys.resize(SYSTEM_HEAP_SIZE);
        }
        if (sBranchBoundaryAudio.size() != AUDIO_HEAP_SIZE) {
            sBranchBoundaryAudio.resize(AUDIO_HEAP_SIZE);
        }
    } catch (const std::bad_alloc&) {
        ResetBranchBoundary();
        return;
    }

    memcpy(sBranchBoundarySys.data(), gSystemHeap, SYSTEM_HEAP_SIZE);
    memcpy(sBranchBoundaryAudio.data(), gAudioHeap, AUDIO_HEAP_SIZE);
    CaptureSmallState(sBranchBoundarySmallState);
    sBranchBoundarySplit = splitIndex;
    sHasBranchBoundary = true;
}

static void RestoreBranchBoundary() {
    if (!sHasBranchBoundary || !gSystemHeap || !gAudioHeap || sBranchBoundarySys.size() != SYSTEM_HEAP_SIZE ||
        sBranchBoundaryAudio.size() != AUDIO_HEAP_SIZE) {
        return;
    }

    memcpy(gSystemHeap, sBranchBoundarySys.data(), SYSTEM_HEAP_SIZE);
    memcpy(gAudioHeap, sBranchBoundaryAudio.data(), AUDIO_HEAP_SIZE);
    RestoreSmallState(sBranchBoundarySmallState);
}

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

// Mask/form swaps briefly run a special player update path that mutates object
// loading state outside normal per-frame gameplay assumptions. Capturing or
// applying rewind inside that window can deserialize inconsistent state.
static bool IsPlayerInFormTransition() {
    if (gPlayState == nullptr) {
        return false;
    }

    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return false;
    }

    // Only treat the dedicated replacement update as an active form transition.
    // shape.rot.{x,z} are used during normal movement/animation and are not
    // reliable transition markers.
    return player->actor.update == func_8012301C;
}

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

    if (IsPlayerInFormTransition()) {
        return;
    }

    // Scene changed: flush old diffs (they belong to the previous scene's heaps)
    if (gPlayState != sLastPlayState) {
        sLastPlayState = gPlayState;
        sRewindBuffer.clear();
        sRewindMemUsage = 0;
        sHasBaseline = false;
        ResetBranchBoundary();
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
        if (sHasBranchBoundary && sBranchBoundarySplit > 0) {
            sBranchBoundarySplit--;
            if (sBranchBoundarySplit == 0) {
                ResetBranchBoundary();
            }
        }
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

    // If a rewind step lands in the brief form-replacement path, consume a
    // few extra steps immediately so gameplay resumes on a stable update path.
    constexpr int kMaxTransitionSkip = 32;
    int stepsApplied = 0;
    do {
        if (sHasBranchBoundary && sBranchBoundarySplit > 0 && sRewindBuffer.size() == sBranchBoundarySplit) {
            RestoreBranchBoundary();
            ResetBranchBoundary();
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
        stepsApplied++;
    } while (!sRewindBuffer.empty() && IsPlayerInFormTransition() && stepsApplied < kMaxTransitionSkip);

    // While player update is the transient form-replacement path, the normal
    // OnPassPlayerInputs hook may not run, so keep requesting rewind from the
    // safe-point hook until we are back on a stable gameplay update.
    if (sIsRewinding && !sRewindBuffer.empty() && IsPlayerInFormTransition()) {
        sRewindRequested.store(true);
    }
}

static void RewindClear() {
    sRewindBuffer.clear();
    sRewindMemUsage = 0;
    sBaselineSys.clear();
    sBaselineSys.shrink_to_fit();
    sBaselineAudio.clear();
    sBaselineAudio.shrink_to_fit();
    sHasBaseline = false;
    ResetBranchBoundary();
    sBranchBoundarySys.clear();
    sBranchBoundarySys.shrink_to_fit();
    sBranchBoundaryAudio.clear();
    sBranchBoundaryAudio.shrink_to_fit();
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
                CaptureBranchBoundary(sRewindBuffer.size());
            } else {
                ResetBranchBoundary();
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
