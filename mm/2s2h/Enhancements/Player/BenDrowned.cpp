#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/Enhancements/Player/BenDrowned.h"
#include "2s2h/Enhancements/FrameInterpolation/FrameInterpolation.h"
#include "2s2h/CustomMessage/CustomMessage.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

// Engine camera helper used to smoothly interpolate gameplay camera values during the visibility jumpscare zoom.
extern f32 Camera_ScaledStepToCeilF(f32 target, f32 cur, f32 stepScale, f32 minDiff);

extern "C" {
#include "functions.h"
#include "sfx.h"
#include "variables.h"
#include "z64debug_display.h"
#include "overlays/actors/ovl_En_Torch2/z_en_torch2.h"
#include "overlays/effects/ovl_Effect_Ss_Dust/z_eff_ss_dust.h"
}

#define CVAR_NAME "gEnhancements.Player.BenDrowned"
#define CVAR CVarGetInteger(CVAR_NAME, 0)

// --- History recording ---
#define HISTORY_SIZE 48
#define HISTORY_DISTINCT_CHECK_WINDOW 12
#define RECORD_INTERVAL_FRAMES 20
#define DEFAULT_HISTORY_POINT_MIN_DIST 20.0f

// --- Spawn / movement distances (defaults) ---
#define DEFAULT_MIN_SPAWN_DIST 50.0f
#define DEFAULT_DISTANT_SPAWN_DIST 140.0f
#define DEFAULT_MAX_NEARBY_DIST 225.0f
#define DEFAULT_MOVE_THRESHOLD_DIST 15.0f
#define DEFAULT_MIN_REPOSITION_DISTANCE 30.0f
#define DEFAULT_FALLBACK_STALK_DISTANCE 80.0f
#define DEFAULT_PROXIMITY_RUMBLE_DIST 100.0f

// --- Tuning validation ---
#define TUNING_MIN_DISTANCE 1.0f

// --- Persisted tuning CVars ---
#define TUNING_CVAR_BASE "gDeveloperTools.BenDrowned.Tuning"
#define TUNING_CVAR_MOVE_COOLDOWN TUNING_CVAR_BASE ".MoveCooldown"
#define TUNING_CVAR_RESPAWN_COOLDOWN TUNING_CVAR_BASE ".RespawnCooldown"
#define TUNING_CVAR_DIALOGUE_COOLDOWN TUNING_CVAR_BASE ".DialogueCooldown"
#define TUNING_CVAR_LAUGH_BASE TUNING_CVAR_BASE ".LaughBase"
#define TUNING_CVAR_LAUGH_RANDOM TUNING_CVAR_BASE ".LaughRandom"
#define TUNING_CVAR_COLOR_DISTORT_BASE TUNING_CVAR_BASE ".ColorDistortBase"
#define TUNING_CVAR_COLOR_DISTORT_RANDOM TUNING_CVAR_BASE ".ColorDistortRandom"
#define TUNING_CVAR_COLOR_DISTORT_DURATION TUNING_CVAR_BASE ".ColorDistortDuration"
#define TUNING_CVAR_DISAPPEAR_CHANCE TUNING_CVAR_BASE ".DisappearChance"
#define TUNING_CVAR_DIALOGUE_CHANCE TUNING_CVAR_BASE ".DialogueChance"
#define TUNING_CVAR_LAUGH_MIN_PITCH TUNING_CVAR_BASE ".LaughMinPitch"
#define TUNING_CVAR_LAUGH_MAX_PITCH TUNING_CVAR_BASE ".LaughMaxPitch"
#define TUNING_CVAR_HISTORY_POINT_MIN_DIST TUNING_CVAR_BASE ".HistoryPointMinDist"
#define TUNING_CVAR_MIN_SPAWN_DIST TUNING_CVAR_BASE ".MinSpawnDist"
#define TUNING_CVAR_DISTANT_SPAWN_DIST TUNING_CVAR_BASE ".DistantSpawnDist"
#define TUNING_CVAR_MAX_NEARBY_DIST TUNING_CVAR_BASE ".MaxNearbyDist"
#define TUNING_CVAR_MIN_REPOSITION_DIST TUNING_CVAR_BASE ".MinRepositionDist"
#define TUNING_CVAR_MOVE_THRESHOLD_DIST TUNING_CVAR_BASE ".MoveThresholdDist"
#define TUNING_CVAR_CLOSE_EFFECT_DIST TUNING_CVAR_BASE ".CloseEffectDist"
#define TUNING_CVAR_FALLBACK_STALK_DIST TUNING_CVAR_BASE ".FallbackStalkDist"
#define TUNING_CVAR_PROXIMITY_RUMBLE_DIST TUNING_CVAR_BASE ".ProximityRumbleDist"

// --- Visibility ---
#define VISIBILITY_HEIGHT 40.0f
#define VISIBILITY_TOP_HEIGHT 80.0f
#define VISIBILITY_SIDE_OFFSET 20.0f
#define FLOOR_RAYCAST_HEIGHT 60.0f
#define WATCH_MARGIN 1.35f

// --- Cooldown durations ---
#define DEFAULT_MOVE_COOLDOWN_FRAMES 3600
#define EFFECT_COOLDOWN_FRAMES 30
#define JUMPSCARE_ZOOM_FRAMES 20
#define JUMPSCARE_COOLDOWN_FRAMES 90
#define DEFAULT_RESPAWN_COOLDOWN_FRAMES 1800
#define DEFAULT_DIALOGUE_COOLDOWN_FRAMES 600

// --- Disappearance ---
#define DEFAULT_DISAPPEAR_CHANCE 0.3f

// --- Laugh SFX ---
#define DEFAULT_LAUGH_BASE_FRAMES 300
#define DEFAULT_LAUGH_RANDOM_FRAMES 300
#define DEFAULT_LAUGH_MIN_PITCH 0.9f
#define DEFAULT_LAUGH_MAX_PITCH 1.1f

// --- Color distortion ---
#define DEFAULT_COLOR_DISTORT_BASE_FRAMES 240
#define DEFAULT_COLOR_DISTORT_RANDOM_FRAMES 360
#define DEFAULT_COLOR_DISTORT_DURATION 12
#define COLOR_DISTORT_INTENSITY 160

// --- Proximity rumble ---
#define PROXIMITY_RUMBLE_MIN_STRENGTH 70
#define PROXIMITY_RUMBLE_MAX_STRENGTH 255
#define PROXIMITY_RUMBLE_MIN_DECAY 2
#define PROXIMITY_RUMBLE_MAX_DECAY 18
#define PROXIMITY_RUMBLE_STEP 8

// --- Arrival effects ---
#define DEFAULT_CLOSE_EFFECT_DIST 110.0f
#define DUST_DRAW_FLAGS DUST_DRAWFLAG_RAND_COLOR_OFFSET
#define DUST_SCALE 120
#define DUST_SCALE_STEP (-8)
#define DUST_LIFE 6

// --- Visibility jumpscare ---
#define JUMPSCARE_TARGET_DIST 60.0f
#define JUMPSCARE_TARGET_FOV 40.0f
#define JUMPSCARE_DIST_STEP_SCALE 0.35f
#define JUMPSCARE_DIST_MIN_DIFF 0.5f
#define JUMPSCARE_FOV_MIN_DIFF 0.1f

// --- Dialogue corruption ---
#define DIALOGUE_SEARCH_WINDOW 96
#define DIALOGUE_LATE_START_NUMERATOR 1
#define DIALOGUE_LATE_START_DENOMINATOR 4
#define DIALOGUE_TRIGGER_BASE 8
#define DIALOGUE_TRIGGER_RANGE 8
#define DIALOGUE_CHANCE 0.5f
#define TEXTBOX_LINE_BREAK 0x11
#define MESSAGE_TERMINATOR 0xBF

// --- Debug overlay ---
#define DEBUG_OVERLAY_CVAR "gDeveloperTools.BenDrowned.DebugOverlay"
#define DEBUG_HISTORY_MARKER_SCALE 0.12f
#define DEBUG_TARGET_MARKER_SCALE 0.18f
#define DEBUG_STATUE_MARKER_SCALE 0.22f
#define DEBUG_MARKER_HEIGHT 12.0f
#define DEBUG_HISTORY_MARKER_TYPE 2
#define DEBUG_TARGET_MARKER_TYPE 1
#define DEBUG_STATUE_MARKER_TYPE 3

// --- Fallback yaw offsets for off-camera stalking positions ---
#define FALLBACK_YAW_SIDE_NEAR 0x5000
#define FALLBACK_YAW_SIDE_FAR 0x7000
#define FALLBACK_YAW_BEHIND ((s16)0x8000)
#define HISTORY_ZONE_CACHE_SIZE 64

static const s16 sFallbackYawOffsets[] = {
    FALLBACK_YAW_SIDE_NEAR,      (s16)-FALLBACK_YAW_SIDE_NEAR, FALLBACK_YAW_SIDE_FAR,
    (s16)-FALLBACK_YAW_SIDE_FAR, FALLBACK_YAW_BEHIND,
};

struct ColorDistortion {
    u16 colorFlag;
    u16 intensity;
};

static const std::string_view sDialogueMessages[] = {
    "turn arround",
    "god please help",
    "help me",
    "don't look behind you",
    "it is right behind you",
    "you were not supposed to see me",
    "it followed you here",
    "this is not your save file",
    "it is still watching you",
    "please wake up",
    "you need to leave",
    "it knows your name",
    "your god has left you to die",
};

static const ColorDistortion sColorDistortions[] = {
    { COLORFILTER_COLORFLAG_BLUE, COLOR_DISTORT_INTENSITY },
    { COLORFILTER_COLORFLAG_RED, COLOR_DISTORT_INTENSITY },
    { COLORFILTER_COLORFLAG_GRAY, (u16)(COLORFILTER_INTENSITY_FLAG | COLOR_DISTORT_INTENSITY) },
};

// Consolidated mutable module state.
static struct {
    std::array<Vec3f, HISTORY_SIZE> history;
    size_t historyCount;
    size_t historyWriteIndex;
    s32 recordTimer;
    s32 moveCooldown;
    s32 respawnCooldown;
    s32 colorDistortCooldown;
    s32 effectCooldown;
    s32 jumpscareCooldown;
    s32 jumpscareTimer;
    s32 laughCooldown;
    s32 dialogueCooldown;
    bool jumpscareCameraActive;
    bool disappearAfterObserved;
    bool statueObserved;
    bool statueWasVisible;
    PlayState* lastPlayState;
    EnTorch2* ownedStatue;
    bool spawnedStatue;
    u32 ignoreStatueInterpolationUntilFrame;
    f32 jumpscareOriginalDist;
    f32 jumpscareOriginalFov;
    s16 currentSceneId;
    s32 currentSceneLayer;
} sState;

struct ZoneHistoryCacheEntry {
    bool valid;
    s16 sceneId;
    s32 sceneLayer;
    std::array<Vec3f, HISTORY_SIZE> history;
    size_t historyCount;
    size_t historyWriteIndex;
};

static ZoneHistoryCacheEntry sZoneHistoryCache[HISTORY_ZONE_CACHE_SIZE];
static size_t sNextZoneHistoryCacheReplacementIndex = 0;

// Runtime-adjustable tuning parameters (exposed via debug menu).
static BenDrowned::TuningParams sTuning = {
    DEFAULT_MOVE_COOLDOWN_FRAMES,
    DEFAULT_RESPAWN_COOLDOWN_FRAMES,
    DEFAULT_DIALOGUE_COOLDOWN_FRAMES,
    DEFAULT_LAUGH_BASE_FRAMES,
    DEFAULT_LAUGH_RANDOM_FRAMES,
    DEFAULT_COLOR_DISTORT_BASE_FRAMES,
    DEFAULT_COLOR_DISTORT_RANDOM_FRAMES,
    DEFAULT_COLOR_DISTORT_DURATION,
    DEFAULT_DISAPPEAR_CHANCE,
    DIALOGUE_CHANCE,
    DEFAULT_LAUGH_MIN_PITCH,
    DEFAULT_LAUGH_MAX_PITCH,
    DEFAULT_HISTORY_POINT_MIN_DIST,
    DEFAULT_MIN_SPAWN_DIST,
    DEFAULT_DISTANT_SPAWN_DIST,
    DEFAULT_MAX_NEARBY_DIST,
    DEFAULT_MIN_REPOSITION_DISTANCE,
    DEFAULT_MOVE_THRESHOLD_DIST,
    DEFAULT_CLOSE_EFFECT_DIST,
    DEFAULT_FALLBACK_STALK_DISTANCE,
    DEFAULT_PROXIMITY_RUMBLE_DIST,
};

// Constant effect parameters passed by address to C functions.
static Vec3f sZeroVelocity = { 0.0f, 0.0f, 0.0f };
static Vec3f sDustAccel = { 0.0f, 0.08f, 0.0f };
static Color_RGBA8 sDustPrimColor = { 170, 130, 90, 160 };
static Color_RGBA8 sDustEnvColor = { 100, 60, 20, 110 };

static void ResetLaughCooldown() {
    sState.laughCooldown = sTuning.laughBaseFrames + (s32)(Rand_ZeroOne() * sTuning.laughRandomFrames);
}

static void ResetColorDistortCooldown() {
    sState.colorDistortCooldown =
        sTuning.colorDistortBaseFrames + (s32)(Rand_ZeroOne() * sTuning.colorDistortRandomFrames);
}

static void NormalizeTuning() {
    sTuning.moveCooldownFrames = std::max(sTuning.moveCooldownFrames, 0);
    sTuning.respawnCooldownFrames = std::max(sTuning.respawnCooldownFrames, 0);
    sTuning.dialogueCooldownFrames = std::max(sTuning.dialogueCooldownFrames, 0);
    sTuning.laughBaseFrames = std::max(sTuning.laughBaseFrames, 0);
    sTuning.laughRandomFrames = std::max(sTuning.laughRandomFrames, 0);
    sTuning.colorDistortBaseFrames = std::max(sTuning.colorDistortBaseFrames, 0);
    sTuning.colorDistortRandomFrames = std::max(sTuning.colorDistortRandomFrames, 0);
    sTuning.colorDistortDuration = std::max(sTuning.colorDistortDuration, 1);
    sTuning.disappearChance = std::clamp(sTuning.disappearChance, 0.0f, 1.0f);
    sTuning.dialogueChance = std::clamp(sTuning.dialogueChance, 0.0f, 1.0f);
    sTuning.historyPointMinDist = std::max(sTuning.historyPointMinDist, TUNING_MIN_DISTANCE);
    sTuning.minSpawnDist = std::max(sTuning.minSpawnDist, TUNING_MIN_DISTANCE);
    sTuning.distantSpawnDist = std::max(sTuning.distantSpawnDist, TUNING_MIN_DISTANCE);
    sTuning.maxNearbyDist = std::max(sTuning.maxNearbyDist, TUNING_MIN_DISTANCE);
    sTuning.minRepositionDist = std::max(sTuning.minRepositionDist, TUNING_MIN_DISTANCE);
    sTuning.moveThresholdDist = std::max(sTuning.moveThresholdDist, TUNING_MIN_DISTANCE);
    sTuning.closeEffectDist = std::max(sTuning.closeEffectDist, TUNING_MIN_DISTANCE);
    sTuning.fallbackStalkDist = std::max(sTuning.fallbackStalkDist, TUNING_MIN_DISTANCE);
    sTuning.proximityRumbleDist = std::max(sTuning.proximityRumbleDist, TUNING_MIN_DISTANCE);
    sTuning.laughMinPitch = std::clamp(sTuning.laughMinPitch, BenDrowned::MIN_TUNING_PITCH, BenDrowned::MAX_TUNING_PITCH);
    sTuning.laughMaxPitch = std::clamp(sTuning.laughMaxPitch, BenDrowned::MIN_TUNING_PITCH, BenDrowned::MAX_TUNING_PITCH);

    if (sTuning.laughMinPitch > sTuning.laughMaxPitch) {
        std::swap(sTuning.laughMinPitch, sTuning.laughMaxPitch);
    }
}

static size_t HistoryIndexFromEnd(size_t i) {
    return (sState.historyWriteIndex + HISTORY_SIZE - i - 1) % HISTORY_SIZE;
}

static void DecrementCooldown(s32* cooldown) {
    if (*cooldown > 0) {
        (*cooldown)--;
    }
}

static void ResetHistoryBuffer() {
    sState.historyCount = 0;
    sState.historyWriteIndex = 0;
    sState.recordTimer = 0;
}

static void ResetZoneRuntimeState() {
    sState.moveCooldown = 0;
    sState.respawnCooldown = 0;
    sState.colorDistortCooldown = 0;
    sState.effectCooldown = 0;
    sState.jumpscareCooldown = 0;
    sState.jumpscareTimer = 0;
    sState.dialogueCooldown = 0;
    sState.jumpscareCameraActive = false;
    sState.disappearAfterObserved = false;
    sState.statueObserved = false;
    sState.statueWasVisible = false;
    sState.ignoreStatueInterpolationUntilFrame = 0;
    sState.jumpscareOriginalDist = 0.0f;
    sState.jumpscareOriginalFov = 0.0f;
    ResetLaughCooldown();
}

static void ResetHistory() {
    ResetHistoryBuffer();
    ResetZoneRuntimeState();
}

static void ClearStatueTracking() {
    sState.ownedStatue = nullptr;
    sState.spawnedStatue = false;
    sState.disappearAfterObserved = false;
    sState.statueObserved = false;
    sState.statueWasVisible = false;
    sState.ignoreStatueInterpolationUntilFrame = 0;
}

static void CleanupOwnedStatue() {
    if (sState.spawnedStatue && (sState.ownedStatue != nullptr) && (sState.ownedStatue->actor.update != NULL)) {
        Actor_Kill(&sState.ownedStatue->actor);
    }

    ClearStatueTracking();
}

static s16 GetCurrentSceneId(PlayState* play) {
    return (play != nullptr) ? Play_GetOriginalSceneId(play->sceneId) : -1;
}

static s32 GetCurrentSceneLayer() {
    return gSaveContext.sceneLayer;
}

static ZoneHistoryCacheEntry* FindZoneHistoryCacheEntry(s16 sceneId, s32 sceneLayer) {
    for (auto& entry : sZoneHistoryCache) {
        if (entry.valid && (entry.sceneId == sceneId) && (entry.sceneLayer == sceneLayer)) {
            return &entry;
        }
    }

    return nullptr;
}

static ZoneHistoryCacheEntry* GetOrCreateZoneHistoryCacheEntry(s16 sceneId, s32 sceneLayer) {
    if (auto* entry = FindZoneHistoryCacheEntry(sceneId, sceneLayer); entry != nullptr) {
        return entry;
    }

    for (auto& entry : sZoneHistoryCache) {
        if (!entry.valid) {
            entry.valid = true;
            entry.sceneId = sceneId;
            entry.sceneLayer = sceneLayer;
            entry.historyCount = 0;
            entry.historyWriteIndex = 0;
            return &entry;
        }
    }

    auto* entry = &sZoneHistoryCache[sNextZoneHistoryCacheReplacementIndex];
    sNextZoneHistoryCacheReplacementIndex = (sNextZoneHistoryCacheReplacementIndex + 1) % HISTORY_ZONE_CACHE_SIZE;
    entry->valid = true;
    entry->sceneId = sceneId;
    entry->sceneLayer = sceneLayer;
    entry->historyCount = 0;
    entry->historyWriteIndex = 0;
    return entry;
}

static void SaveCurrentZoneHistory() {
    if (sState.currentSceneId < 0) {
        return;
    }

    auto* entry = GetOrCreateZoneHistoryCacheEntry(sState.currentSceneId, sState.currentSceneLayer);
    entry->history = sState.history;
    entry->historyCount = sState.historyCount;
    entry->historyWriteIndex = sState.historyWriteIndex;
}

static void LoadZoneHistory(s16 sceneId, s32 sceneLayer) {
    if (auto* entry = FindZoneHistoryCacheEntry(sceneId, sceneLayer); entry != nullptr) {
        sState.history = entry->history;
        sState.historyCount = entry->historyCount;
        sState.historyWriteIndex = entry->historyWriteIndex;
        sState.recordTimer = 0;
        return;
    }

    ResetHistoryBuffer();
}

static void HandleZoneChange(PlayState* play) {
    s16 sceneId = GetCurrentSceneId(play);
    s32 sceneLayer = GetCurrentSceneLayer();

    if ((sceneId == sState.currentSceneId) && (sceneLayer == sState.currentSceneLayer)) {
        return;
    }

    SaveCurrentZoneHistory();
    sState.currentSceneId = sceneId;
    sState.currentSceneLayer = sceneLayer;
    LoadZoneHistory(sceneId, sceneLayer);
    ResetZoneRuntimeState();
    ClearStatueTracking();
}

static void LoadTuning() {
    sTuning.moveCooldownFrames = CVarGetInteger(TUNING_CVAR_MOVE_COOLDOWN, DEFAULT_MOVE_COOLDOWN_FRAMES);
    sTuning.respawnCooldownFrames = CVarGetInteger(TUNING_CVAR_RESPAWN_COOLDOWN, DEFAULT_RESPAWN_COOLDOWN_FRAMES);
    sTuning.dialogueCooldownFrames = CVarGetInteger(TUNING_CVAR_DIALOGUE_COOLDOWN, DEFAULT_DIALOGUE_COOLDOWN_FRAMES);
    sTuning.laughBaseFrames = CVarGetInteger(TUNING_CVAR_LAUGH_BASE, DEFAULT_LAUGH_BASE_FRAMES);
    sTuning.laughRandomFrames = CVarGetInteger(TUNING_CVAR_LAUGH_RANDOM, DEFAULT_LAUGH_RANDOM_FRAMES);
    sTuning.colorDistortBaseFrames =
        CVarGetInteger(TUNING_CVAR_COLOR_DISTORT_BASE, DEFAULT_COLOR_DISTORT_BASE_FRAMES);
    sTuning.colorDistortRandomFrames =
        CVarGetInteger(TUNING_CVAR_COLOR_DISTORT_RANDOM, DEFAULT_COLOR_DISTORT_RANDOM_FRAMES);
    sTuning.colorDistortDuration =
        CVarGetInteger(TUNING_CVAR_COLOR_DISTORT_DURATION, DEFAULT_COLOR_DISTORT_DURATION);
    sTuning.disappearChance = CVarGetFloat(TUNING_CVAR_DISAPPEAR_CHANCE, DEFAULT_DISAPPEAR_CHANCE);
    sTuning.dialogueChance = CVarGetFloat(TUNING_CVAR_DIALOGUE_CHANCE, DIALOGUE_CHANCE);
    sTuning.laughMinPitch = CVarGetFloat(TUNING_CVAR_LAUGH_MIN_PITCH, DEFAULT_LAUGH_MIN_PITCH);
    sTuning.laughMaxPitch = CVarGetFloat(TUNING_CVAR_LAUGH_MAX_PITCH, DEFAULT_LAUGH_MAX_PITCH);
    sTuning.historyPointMinDist = CVarGetFloat(TUNING_CVAR_HISTORY_POINT_MIN_DIST, DEFAULT_HISTORY_POINT_MIN_DIST);
    sTuning.minSpawnDist = CVarGetFloat(TUNING_CVAR_MIN_SPAWN_DIST, DEFAULT_MIN_SPAWN_DIST);
    sTuning.distantSpawnDist = CVarGetFloat(TUNING_CVAR_DISTANT_SPAWN_DIST, DEFAULT_DISTANT_SPAWN_DIST);
    sTuning.maxNearbyDist = CVarGetFloat(TUNING_CVAR_MAX_NEARBY_DIST, DEFAULT_MAX_NEARBY_DIST);
    sTuning.minRepositionDist = CVarGetFloat(TUNING_CVAR_MIN_REPOSITION_DIST, DEFAULT_MIN_REPOSITION_DISTANCE);
    sTuning.moveThresholdDist = CVarGetFloat(TUNING_CVAR_MOVE_THRESHOLD_DIST, DEFAULT_MOVE_THRESHOLD_DIST);
    sTuning.closeEffectDist = CVarGetFloat(TUNING_CVAR_CLOSE_EFFECT_DIST, DEFAULT_CLOSE_EFFECT_DIST);
    sTuning.fallbackStalkDist = CVarGetFloat(TUNING_CVAR_FALLBACK_STALK_DIST, DEFAULT_FALLBACK_STALK_DISTANCE);
    sTuning.proximityRumbleDist = CVarGetFloat(TUNING_CVAR_PROXIMITY_RUMBLE_DIST, DEFAULT_PROXIMITY_RUMBLE_DIST);
    NormalizeTuning();
}

static void HandlePlayStateChange(PlayState* play) {
    if (play != sState.lastPlayState) {
        SaveCurrentZoneHistory();
        sState.lastPlayState = play;
        sState.currentSceneId = -1;
        sState.currentSceneLayer = -1;
        ResetHistory();
        ClearStatueTracking();
    }
}

static bool IsNormalGameplayState(PlayState* play) {
    return (play->pauseCtx.state == PAUSE_STATE_OFF) && (play->pauseCtx.debugEditor == DEBUG_EDITOR_NONE) &&
           (play->msgCtx.msgMode == MSGMODE_NONE) && (play->transitionTrigger == TRANS_TRIGGER_OFF) &&
           (play->transitionMode == TRANS_MODE_OFF) && !Play_InCsMode(play);
}

static bool IsPlayerGroundedAndDry(Player* player) {
    return (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) && !(player->actor.bgCheckFlags & BGCHECKFLAG_WATER);
}

static void RecordHistoryPoint(Player* player) {
    Vec3f playerPos = player->actor.world.pos;
    size_t distinctCheckCount;

    if (sState.recordTimer > 0) {
        sState.recordTimer--;
        return;
    }

    distinctCheckCount = std::min(sState.historyCount, (size_t)HISTORY_DISTINCT_CHECK_WINDOW);
    for (size_t i = 0; i < distinctCheckCount; i++) {
        size_t idx = HistoryIndexFromEnd(i);

        if (Math3D_Vec3fDistSq(&sState.history[idx], &playerPos) < SQ(sTuning.historyPointMinDist)) {
            sState.recordTimer = RECORD_INTERVAL_FRAMES;
            return;
        }
    }

    sState.history[sState.historyWriteIndex] = playerPos;
    sState.historyWriteIndex = (sState.historyWriteIndex + 1) % HISTORY_SIZE;
    if (sState.historyCount < HISTORY_SIZE) {
        sState.historyCount++;
    }
    sState.recordTimer = RECORD_INTERVAL_FRAMES;
}

static bool IsPointOnScreen(PlayState* play, const Vec3f& worldPos) {
    Vec3f projectedPos;
    f32 projectedW;
    Vec3f projectedSource = worldPos;

    SkinMatrix_Vec3fMtxFMultXYZW(&play->viewProjectionMtxF, &projectedSource, &projectedPos, &projectedW);

    return (projectedW > 1.0f) && (projectedPos.z > 0.0f) && (fabsf(projectedPos.x) < (projectedW * WATCH_MARGIN)) &&
           (fabsf(projectedPos.y) < (projectedW * WATCH_MARGIN));
}

static bool CanCameraSeePoint(PlayState* play, const Vec3f& point) {
    Camera* camera = GET_ACTIVE_CAM(play);
    Vec3f cameraForward;
    Vec3f cameraRight = { 1.0f, 0.0f, 0.0f };
    Vec3f hitPos;
    Vec3f watchPoint;
    CollisionPoly* hitPoly = nullptr;
    f32 cameraForwardDistSq;
    auto isVisibleSample = [&](f32 x, f32 y, f32 z) {
        watchPoint.x = x;
        watchPoint.y = y;
        watchPoint.z = z;

        return IsPointOnScreen(play, watchPoint) &&
               !BgCheck_AnyLineTest1(&play->colCtx, &camera->eye, &watchPoint, &hitPos, &hitPoly, false);
    };

    if (camera == nullptr) {
        return false;
    }

    cameraForward.x = camera->at.x - camera->eye.x;
    cameraForward.y = 0.0f;
    cameraForward.z = camera->at.z - camera->eye.z;
    cameraForwardDistSq = SQ(cameraForward.x) + SQ(cameraForward.z);
    if (cameraForwardDistSq > 0.001f) {
        f32 invCameraForwardDist = 1.0f / sqrtf(cameraForwardDistSq);

        cameraRight.x = cameraForward.z * invCameraForwardDist;
        cameraRight.z = -cameraForward.x * invCameraForwardDist;
    }

    return isVisibleSample(point.x, point.y + VISIBILITY_HEIGHT, point.z) ||
           isVisibleSample(point.x, point.y + VISIBILITY_TOP_HEIGHT, point.z) ||
           isVisibleSample(point.x + (cameraRight.x * VISIBILITY_SIDE_OFFSET), point.y + VISIBILITY_HEIGHT,
                           point.z + (cameraRight.z * VISIBILITY_SIDE_OFFSET)) ||
           isVisibleSample(point.x - (cameraRight.x * VISIBILITY_SIDE_OFFSET), point.y + VISIBILITY_HEIGHT,
                           point.z - (cameraRight.z * VISIBILITY_SIDE_OFFSET));
}

static bool FindHiddenHistoryPoint(PlayState* play, Player* player, f32 maxDistSq, Vec3f* hiddenPoint) {
    for (size_t i = 0; i < sState.historyCount; i++) {
        size_t idx = HistoryIndexFromEnd(i);
        Vec3f candidatePoint = sState.history[idx];
        f32 playerDistSq = Math3D_Vec3fDistSq(&candidatePoint, &player->actor.world.pos);

        if (playerDistSq < SQ(sTuning.minSpawnDist)) {
            continue;
        }

        if ((maxDistSq > 0.0f) && (playerDistSq > maxDistSq)) {
            continue;
        }

        if (!CanCameraSeePoint(play, candidatePoint)) {
            *hiddenPoint = candidatePoint;
            return true;
        }
    }

    return false;
}

static bool FindDistantSpawnPoint(PlayState* play, Player* player, Vec3f* hiddenPoint) {
    Vec3f bestDistantPoint = player->actor.world.pos;
    Vec3f bestFallbackPoint = player->actor.world.pos;
    bool foundDistantPoint = false;
    bool foundFallbackPoint = false;
    f32 bestDistantDistSq = 0.0f;
    f32 bestFallbackDistSq = 0.0f;

    for (size_t i = 0; i < sState.historyCount; i++) {
        size_t idx = HistoryIndexFromEnd(i);
        Vec3f candidatePoint = sState.history[idx];
        f32 playerDistSq = Math3D_Vec3fDistSq(&candidatePoint, &player->actor.world.pos);

        if (playerDistSq < SQ(sTuning.minSpawnDist)) {
            continue;
        }

        if (!CanCameraSeePoint(play, candidatePoint)) {
            if (playerDistSq >= SQ(sTuning.distantSpawnDist)) {
                if (!foundDistantPoint || (playerDistSq > bestDistantDistSq)) {
                    bestDistantPoint = candidatePoint;
                    bestDistantDistSq = playerDistSq;
                    foundDistantPoint = true;
                }
            } else if (!foundFallbackPoint || (playerDistSq > bestFallbackDistSq)) {
                bestFallbackPoint = candidatePoint;
                bestFallbackDistSq = playerDistSq;
                foundFallbackPoint = true;
            }
        }
    }

    if (foundDistantPoint) {
        *hiddenPoint = bestDistantPoint;
        return true;
    }

    if (foundFallbackPoint) {
        *hiddenPoint = bestFallbackPoint;
        return true;
    }

    return false;
}

static bool SnapPointToFloor(PlayState* play, Vec3f* point) {
    Vec3f raycastPos = *point;
    CollisionPoly* floorPoly = nullptr;
    s32 bgId;
    f32 floorHeight;

    raycastPos.y += FLOOR_RAYCAST_HEIGHT;
    floorHeight = BgCheck_EntityRaycastFloor3(&play->colCtx, &floorPoly, &bgId, &raycastPos);

    if (floorHeight != BGCHECK_Y_MIN) {
        point->y = floorHeight;
        return true;
    }

    return false;
}

static bool IsStatueAlive(EnTorch2* statue) {
    return (statue != nullptr) && (statue->actor.update != NULL);
}

static EnTorch2* GetStatue(PlayState* play) {
    EnTorch2* statue = play->actorCtx.elegyShells[TORCH2_PARAM_HUMAN];

    if (!IsStatueAlive(sState.ownedStatue)) {
        ClearStatueTracking();
    }

    if (!IsStatueAlive(statue)) {
        statue = nullptr;
        play->actorCtx.elegyShells[TORCH2_PARAM_HUMAN] = nullptr;
    }

    if (statue != nullptr) {
        sState.ownedStatue = statue;
    }

    return statue;
}

static EnTorch2* SpawnStatue(PlayState* play, const Vec3f& spawnPos) {
    EnTorch2* statue = (EnTorch2*)Actor_Spawn(&play->actorCtx, play, ACTOR_EN_TORCH2, spawnPos.x, spawnPos.y,
                                              spawnPos.z, 0, 0, 0, TORCH2_PARAM_HUMAN);

    if (statue != nullptr) {
        play->actorCtx.elegyShells[TORCH2_PARAM_HUMAN] = statue;
        sState.ownedStatue = statue;
        sState.spawnedStatue = true;
    }

    return statue;
}

static void DismissStatue(PlayState* play, EnTorch2* statue) {
    if ((statue != nullptr) && (play->actorCtx.elegyShells[TORCH2_PARAM_HUMAN] == statue)) {
        play->actorCtx.elegyShells[TORCH2_PARAM_HUMAN] = nullptr;
    }

    if ((statue != nullptr) && (statue->actor.update != NULL)) {
        Actor_Kill(&statue->actor);
    }

    ClearStatueTracking();
}

static void SetStatueRotation(EnTorch2* statue, Player* player) {
    s16 targetYaw = Math_Vec3f_Yaw(&statue->actor.world.pos, &player->actor.world.pos);

    statue->actor.world.rot.y = targetYaw;
    statue->actor.shape.rot.y = targetYaw;
    statue->actor.home.rot.y = targetYaw;
}

static size_t RandomIndex(size_t count) {
    return static_cast<size_t>(fminf(Rand_ZeroOne() * count, static_cast<f32>(count - 1)));
}

static u8 LerpU8(u8 min, u8 max, f32 t) {
    return static_cast<u8>(std::clamp<f32>(min + (t * (max - min)), min, max));
}

static bool IsCorruptibleChar(char ch) {
    return (ch >= ' ') && (ch <= '~');
}

static bool IsGlyphChar(char ch) {
    return IsCorruptibleChar(ch) && (ch != ' ');
}

static bool CanReplaceWholeDialogueMessage(const std::string& msg) {
    bool hasGlyph = false;

    for (unsigned char ch : msg) {
        if ((ch == MESSAGE_TERMINATOR) || (ch == TEXTBOX_LINE_BREAK)) {
            continue;
        }

        if (!IsCorruptibleChar(ch)) {
            return false;
        }

        if (!hasGlyph && IsGlyphChar(ch)) {
            hasGlyph = true;
        }
    }

    return hasGlyph;
}

static bool TryCorruptMessage(CustomMessage::Entry* entry) {
    if ((entry == nullptr) || !CanReplaceWholeDialogueMessage(entry->msg)) {
        return false;
    }

    entry->msg = sDialogueMessages[RandomIndex(std::size(sDialogueMessages))];
    entry->autoFormat = true;
    return true;
}

static bool ShouldCorruptOpenText(PlayState* play, u16 textId) {
    // Allow the haunt to hit readable non-actor text too so it can still show up in ordinary gameplay.
    return (play != nullptr) && (textId != 0) && (textId != CUSTOM_MESSAGE_ID) && (sState.dialogueCooldown <= 0) &&
           (Rand_ZeroOne() < sTuning.dialogueChance);
}

static bool IsPointFarEnoughFromReference(const Vec3f* referencePoint, f32 minReferenceDistSq, const Vec3f& point) {
    Vec3f pointCopy = point;
    Vec3f referencePointCopy = {};

    if ((referencePoint == nullptr) || (minReferenceDistSq <= 0.0f)) {
        return true;
    }

    // Math3D_Vec3fDistSq still takes mutable Vec3f pointers.
    referencePointCopy = *referencePoint;
    return Math3D_Vec3fDistSq(&referencePointCopy, &pointCopy) >= minReferenceDistSq;
}

static bool FindFallbackPoint(PlayState* play, Player* player, const Vec3f* referencePoint, f32 minReferenceDistSq,
                              Vec3f* hiddenPoint) {
    Camera* camera = GET_ACTIVE_CAM(play);

    if (camera == nullptr) {
        return false;
    }

    for (s16 yawOffset : sFallbackYawOffsets) {
        Vec3f candidatePoint = player->actor.world.pos;
        s16 stalkYaw = Math_Vec3f_Yaw(&camera->eye, &camera->at) + yawOffset;

        candidatePoint.x += Math_SinS(stalkYaw) * sTuning.fallbackStalkDist;
        candidatePoint.z += Math_CosS(stalkYaw) * sTuning.fallbackStalkDist;

        if (!SnapPointToFloor(play, &candidatePoint)) {
            continue;
        }

        if (!IsPointFarEnoughFromReference(referencePoint, minReferenceDistSq, candidatePoint)) {
            continue;
        }

        if (!CanCameraSeePoint(play, candidatePoint)) {
            *hiddenPoint = candidatePoint;
            return true;
        }
    }

    return false;
}

static bool FindTargetPoint(PlayState* play, Player* player, Vec3f* hiddenPoint) {
    return FindHiddenHistoryPoint(play, player, SQ(sTuning.maxNearbyDist), hiddenPoint) ||
           FindFallbackPoint(play, player, nullptr, 0.0f, hiddenPoint) ||
           FindHiddenHistoryPoint(play, player, 0.0f, hiddenPoint);
}

static bool FindTargetPointFarFromCurrent(PlayState* play, Player* player, const Vec3f& currentPoint,
                                          Vec3f* hiddenPoint) {
    Vec3f bestHistoryPoint = {};
    // Math3D_Vec3fDistSq still takes mutable Vec3f pointers.
    Vec3f currentPointCopy = currentPoint;
    bool foundHistoryPoint = false;
    f32 bestHistoryDistSq = 0.0f;
    f32 minMoveDistSq = SQ(std::max(sTuning.minRepositionDist, sTuning.minSpawnDist));

    for (size_t i = 0; i < sState.historyCount; i++) {
        size_t idx = HistoryIndexFromEnd(i);
        Vec3f candidatePoint = sState.history[idx];
        f32 playerDistSq = Math3D_Vec3fDistSq(&candidatePoint, &player->actor.world.pos);
        f32 statueDistSq;

        if (playerDistSq < SQ(sTuning.minSpawnDist)) {
            continue;
        }

        if (playerDistSq > SQ(sTuning.maxNearbyDist)) {
            continue;
        }

        if (CanCameraSeePoint(play, candidatePoint)) {
            continue;
        }

        statueDistSq = Math3D_Vec3fDistSq(&candidatePoint, &currentPointCopy);
        if (statueDistSq < minMoveDistSq) {
            continue;
        }

        if (!foundHistoryPoint || (statueDistSq > bestHistoryDistSq)) {
            bestHistoryPoint = candidatePoint;
            bestHistoryDistSq = statueDistSq;
            foundHistoryPoint = true;
        }
    }

    if (foundHistoryPoint) {
        *hiddenPoint = bestHistoryPoint;
        return true;
    }

    return FindFallbackPoint(play, player, &currentPoint, minMoveDistSq, hiddenPoint);
}

static void MoveStatue(PlayState* play, Player* player, EnTorch2* statue, const Vec3f& targetPoint) {
    Vec3f snappedPoint = targetPoint;

    SnapPointToFloor(play, &snappedPoint);
    Math_Vec3f_Copy(&statue->actor.world.pos, &snappedPoint);
    Math_Vec3f_Copy(&statue->actor.home.pos, &snappedPoint);
    Math_Vec3f_Copy(&statue->actor.prevPos, &snappedPoint);
    Math_Vec3f_Copy(&statue->actor.focus.pos, &snappedPoint);

    Math_Vec3f_Copy(&statue->actor.velocity, &sZeroVelocity);
    statue->actor.speed = 0.0f;
    statue->actor.floorHeight = snappedPoint.y;
    statue->actor.bgCheckFlags |= BGCHECKFLAG_GROUND;
    statue->state = TORCH2_STATE_SOLID;
    statue->framesUntilNextState = 0;
    statue->alpha = 255;

    SetStatueRotation(statue, player);
    sState.ignoreStatueInterpolationUntilFrame = play->gameplayFrames;
}

static void TriggerArrivalEffects(PlayState* play, Player* player, EnTorch2* statue) {
    f32 distSq;

    if (sState.effectCooldown > 0) {
        return;
    }

    distSq = Math3D_Dist2DSq(player->actor.world.pos.x, player->actor.world.pos.z, statue->actor.world.pos.x,
                             statue->actor.world.pos.z);
    if (distSq > SQ(sTuning.closeEffectDist)) {
        return;
    }

    EffectSsDust_Spawn(play, DUST_DRAW_FLAGS, &statue->actor.world.pos, &sZeroVelocity, &sDustAccel, &sDustPrimColor,
                       &sDustEnvColor, DUST_SCALE, DUST_SCALE_STEP, DUST_LIFE, DUST_UPDATE_NORMAL);

    sState.effectCooldown = EFFECT_COOLDOWN_FRAMES;
}

static void TriggerVisibilityJumpscare(PlayState* play, Player* player, EnTorch2* statue) {
    Camera* camera;
    f32 distSq;

    if ((play == nullptr) || (player == nullptr) || (statue == nullptr) || (sState.jumpscareCooldown > 0)) {
        return;
    }

    distSq = Math3D_Dist2DSq(player->actor.world.pos.x, player->actor.world.pos.z, statue->actor.world.pos.x,
                             statue->actor.world.pos.z);
    if (distSq > SQ(sTuning.closeEffectDist)) {
        return;
    }

    camera = GET_ACTIVE_CAM(play);
    if (camera == nullptr) {
        return;
    }

    sState.jumpscareOriginalDist = camera->dist;
    sState.jumpscareOriginalFov = camera->fov;
    sState.jumpscareCameraActive = true;
    sState.jumpscareTimer = JUMPSCARE_ZOOM_FRAMES;
    sState.jumpscareCooldown = JUMPSCARE_COOLDOWN_FRAMES;

    Audio_PlaySfx(NA_SE_SY_CAMERA_ZOOM_UP_2);
    Audio_PlaySfx(NA_SE_OC_OCARINA);
    Audio_PlaySfx(NA_SE_EV_OCARINA_BOUND_0);
    Audio_PlaySfx(NA_SE_EV_OCARINA_BOUND_1);
}

static void UpdateVisibilityJumpscareCamera(PlayState* play) {
    Camera* camera;

    if (play == nullptr) {
        return;
    }

    camera = GET_ACTIVE_CAM(play);
    if (camera == nullptr) {
        return;
    }

    if (sState.jumpscareTimer > 0) {
        camera->dist = Camera_ScaledStepToCeilF(JUMPSCARE_TARGET_DIST, camera->dist, JUMPSCARE_DIST_STEP_SCALE, 1.0f);
        camera->fov = Camera_ScaledStepToCeilF(JUMPSCARE_TARGET_FOV, camera->fov, camera->fovUpdateRate,
                                               JUMPSCARE_FOV_MIN_DIFF);
        DecrementCooldown(&sState.jumpscareTimer);
        return;
    }

    if (!sState.jumpscareCameraActive) {
        return;
    }

    camera->dist =
        Camera_ScaledStepToCeilF(sState.jumpscareOriginalDist, camera->dist, JUMPSCARE_DIST_STEP_SCALE, 1.0f);
    camera->fov = Camera_ScaledStepToCeilF(sState.jumpscareOriginalFov, camera->fov, camera->fovUpdateRate,
                                           JUMPSCARE_FOV_MIN_DIFF);

    if (fabsf(camera->dist - sState.jumpscareOriginalDist) < JUMPSCARE_DIST_MIN_DIFF &&
        fabsf(camera->fov - sState.jumpscareOriginalFov) < JUMPSCARE_FOV_MIN_DIFF) {
        sState.jumpscareCameraActive = false;
        sState.jumpscareOriginalDist = 0.0f;
        sState.jumpscareOriginalFov = 0.0f;
    }
}

static void UpdateStatueLaugh(EnTorch2* statue) {
    if ((statue == nullptr) || (sState.laughCooldown > 0)) {
        return;
    }

    f32 laughPitch = sTuning.laughMinPitch + (Rand_ZeroOne() * (sTuning.laughMaxPitch - sTuning.laughMinPitch));
    Audio_PlaySfx_AtPosWithFreq(&statue->actor.projectedPos, NA_SE_VO_OMVO00, laughPitch);
    ResetLaughCooldown();
}

static void UpdateStatueProximityRumble(Player* player, EnTorch2* statue) {
    if ((player == nullptr) || (statue == nullptr)) {
        return;
    }

    f32 distSq = Math3D_Dist2DSq(player->actor.world.pos.x, player->actor.world.pos.z, statue->actor.world.pos.x,
                                 statue->actor.world.pos.z);
    if (distSq > SQ(sTuning.proximityRumbleDist)) {
        return;
    }

    f32 dist = sqrtf(distSq);
    f32 proximity = std::clamp(1.0f - (dist / sTuning.proximityRumbleDist), 0.0f, 1.0f);
    u8 strength = LerpU8(PROXIMITY_RUMBLE_MIN_STRENGTH, PROXIMITY_RUMBLE_MAX_STRENGTH, proximity);
    u8 decayTimer = LerpU8(PROXIMITY_RUMBLE_MIN_DECAY, PROXIMITY_RUMBLE_MAX_DECAY, proximity);

    Rumble_Override(0.0f, strength, decayTimer, PROXIMITY_RUMBLE_STEP);
}

static void UpdateStatueColorDistortion(EnTorch2* statue) {
    if (statue == nullptr) {
        return;
    }

    if ((sState.colorDistortCooldown > 0) || (statue->actor.colorFilterTimer != 0)) {
        return;
    }

    const ColorDistortion& distortion = sColorDistortions[RandomIndex(std::size(sColorDistortions))];
    Actor_SetColorFilter(&statue->actor, distortion.colorFlag, distortion.intensity, COLORFILTER_BUFFLAG_OPA,
                         sTuning.colorDistortDuration);
    ResetColorDistortCooldown();
}

static void PopulateDebugHistoryEntry(PlayState* play, Player* player, const Vec3f& point,
                                      BenDrowned::DebugHistoryEntry* entry) {
    Vec3f pointCopy = point;

    entry->pos = point;
    entry->playerDistSq = Math3D_Vec3fDistSq(&pointCopy, &player->actor.world.pos);
    entry->playerDist = sqrtf(entry->playerDistSq);
    entry->visible = CanCameraSeePoint(play, point);
    entry->spawnEligible = !entry->visible && (entry->playerDistSq >= SQ(sTuning.minSpawnDist));
    entry->distantEligible = entry->spawnEligible && (entry->playerDistSq >= SQ(sTuning.distantSpawnDist));
}

static void AddDebugObject(PlayState* play, const Vec3f& pos, f32 scale, u8 red, u8 green, u8 blue, u8 alpha,
                           s16 type) {
    DebugDisplay_AddObject(pos.x, pos.y + DEBUG_MARKER_HEIGHT, pos.z, 0, 0, 0, scale, scale, scale, red, green, blue,
                           alpha, type, play->state.gfxCtx);
}

static void AddDebugHistoryObject(PlayState* play, const BenDrowned::DebugHistoryEntry& entry) {
    if (entry.distantEligible) {
        AddDebugObject(play, entry.pos, DEBUG_HISTORY_MARKER_SCALE, 80, 180, 255, 220, DEBUG_HISTORY_MARKER_TYPE);
    } else if (entry.spawnEligible) {
        AddDebugObject(play, entry.pos, DEBUG_HISTORY_MARKER_SCALE, 80, 255, 120, 200, DEBUG_HISTORY_MARKER_TYPE);
    } else if (entry.visible) {
        AddDebugObject(play, entry.pos, DEBUG_HISTORY_MARKER_SCALE, 255, 90, 90, 180, DEBUG_HISTORY_MARKER_TYPE);
    } else {
        AddDebugObject(play, entry.pos, DEBUG_HISTORY_MARKER_SCALE, 255, 190, 70, 160, DEBUG_HISTORY_MARKER_TYPE);
    }
}

static void DrawDebugOverlay() {
    PlayState* play = gPlayState;
    Player* player = nullptr;
    EnTorch2* statue = nullptr;
    Vec3f spawnPoint = { 0.0f, 0.0f, 0.0f };
    Vec3f targetPoint = { 0.0f, 0.0f, 0.0f };

    if ((play == nullptr) || !CVarGetInteger(DEBUG_OVERLAY_CVAR, 0)) {
        return;
    }

    // The engine's normal DebugDisplay pass has already run before OnPlayDrawWorldEnd.
    DebugDisplay_Init();

    player = GET_PLAYER(play);
    if ((player == nullptr) || (player->actor.update == NULL)) {
        player = nullptr;
    }

    statue = play->actorCtx.elegyShells[TORCH2_PARAM_HUMAN];
    if ((statue != nullptr) && (statue->actor.update == NULL)) {
        statue = nullptr;
    }

    for (size_t i = 0; i < sState.historyCount; i++) {
        size_t idx = HistoryIndexFromEnd(i);
        BenDrowned::DebugHistoryEntry entry = {};

        entry.pos = sState.history[idx];
        if (player != nullptr) {
            PopulateDebugHistoryEntry(play, player, entry.pos, &entry);
        }
        AddDebugHistoryObject(play, entry);
    }

    if ((player != nullptr) && FindDistantSpawnPoint(play, player, &spawnPoint)) {
        AddDebugObject(play, spawnPoint, DEBUG_TARGET_MARKER_SCALE, 80, 220, 255, 255, DEBUG_TARGET_MARKER_TYPE);
    }

    if ((player != nullptr) && FindTargetPoint(play, player, &targetPoint)) {
        AddDebugObject(play, targetPoint, DEBUG_TARGET_MARKER_SCALE, 255, 220, 80, 255, DEBUG_TARGET_MARKER_TYPE);
    }

    if (statue != nullptr) {
        AddDebugObject(play, statue->actor.world.pos, DEBUG_STATUE_MARKER_SCALE, 220, 80, 255, 255,
                       DEBUG_STATUE_MARKER_TYPE);
    }

    DebugDisplay_DrawObjects(play);
}

namespace BenDrowned {

DebugSnapshot GetDebugSnapshot() {
    DebugSnapshot snapshot = {};
    PlayState* play = gPlayState;
    EnTorch2* statue;
    Player* player;

    snapshot.enabled = CVAR != 0;
    snapshot.hasPlayState = play != nullptr;
    if (play == nullptr) {
        return snapshot;
    }

    player = GET_PLAYER(play);
    statue = GetStatue(play);

    snapshot.normalGameplayState = IsNormalGameplayState(play);
    snapshot.playerValid = (player != nullptr) && (player->actor.update != NULL);
    snapshot.playerGroundedAndDry = snapshot.playerValid && IsPlayerGroundedAndDry(player);
    snapshot.statueAlive = statue != nullptr;
    snapshot.statueManaged = sState.spawnedStatue;
    snapshot.statueObserved = sState.statueObserved;
    snapshot.statueWasVisible = sState.statueWasVisible;
    snapshot.disappearAfterObserved = sState.disappearAfterObserved;
    snapshot.respawnReady = sState.respawnCooldown <= 0;
    snapshot.moveReady = sState.moveCooldown <= 0;
    snapshot.historyCount = static_cast<s32>(sState.historyCount);
    snapshot.recordTimer = sState.recordTimer;
    snapshot.moveCooldown = sState.moveCooldown;
    snapshot.respawnCooldown = sState.respawnCooldown;
    snapshot.colorDistortCooldown = sState.colorDistortCooldown;
    snapshot.effectCooldown = sState.effectCooldown;
    snapshot.laughCooldown = sState.laughCooldown;
    snapshot.dialogueCooldown = sState.dialogueCooldown;

    if (snapshot.playerValid) {
        snapshot.playerPos = player->actor.world.pos;
    }

    if (snapshot.statueAlive) {
        snapshot.statuePos = statue->actor.world.pos;
        snapshot.statueVisible = CanCameraSeePoint(play, statue->actor.world.pos);
    }

    if (snapshot.playerValid) {
        snapshot.hasDistantSpawnPoint = FindDistantSpawnPoint(play, player, &snapshot.distantSpawnPoint);
        snapshot.hasTargetPoint = FindTargetPoint(play, player, &snapshot.targetPoint);
    }

    for (size_t i = 0; i < sState.historyCount; i++) {
        size_t idx = HistoryIndexFromEnd(i);
        DebugHistoryEntry& entry = snapshot.history[i];
        entry.pos = sState.history[idx];
        if (snapshot.playerValid) {
            PopulateDebugHistoryEntry(play, player, entry.pos, &entry);
        }
    }

    return snapshot;
}

TuningParams& GetTuning() {
    return sTuning;
}

void SaveTuning() {
    NormalizeTuning();
    CVarSetInteger(TUNING_CVAR_MOVE_COOLDOWN, sTuning.moveCooldownFrames);
    CVarSetInteger(TUNING_CVAR_RESPAWN_COOLDOWN, sTuning.respawnCooldownFrames);
    CVarSetInteger(TUNING_CVAR_DIALOGUE_COOLDOWN, sTuning.dialogueCooldownFrames);
    CVarSetInteger(TUNING_CVAR_LAUGH_BASE, sTuning.laughBaseFrames);
    CVarSetInteger(TUNING_CVAR_LAUGH_RANDOM, sTuning.laughRandomFrames);
    CVarSetInteger(TUNING_CVAR_COLOR_DISTORT_BASE, sTuning.colorDistortBaseFrames);
    CVarSetInteger(TUNING_CVAR_COLOR_DISTORT_RANDOM, sTuning.colorDistortRandomFrames);
    CVarSetInteger(TUNING_CVAR_COLOR_DISTORT_DURATION, sTuning.colorDistortDuration);
    CVarSetFloat(TUNING_CVAR_DISAPPEAR_CHANCE, sTuning.disappearChance);
    CVarSetFloat(TUNING_CVAR_DIALOGUE_CHANCE, sTuning.dialogueChance);
    CVarSetFloat(TUNING_CVAR_LAUGH_MIN_PITCH, sTuning.laughMinPitch);
    CVarSetFloat(TUNING_CVAR_LAUGH_MAX_PITCH, sTuning.laughMaxPitch);
    CVarSetFloat(TUNING_CVAR_HISTORY_POINT_MIN_DIST, sTuning.historyPointMinDist);
    CVarSetFloat(TUNING_CVAR_MIN_SPAWN_DIST, sTuning.minSpawnDist);
    CVarSetFloat(TUNING_CVAR_DISTANT_SPAWN_DIST, sTuning.distantSpawnDist);
    CVarSetFloat(TUNING_CVAR_MAX_NEARBY_DIST, sTuning.maxNearbyDist);
    CVarSetFloat(TUNING_CVAR_MIN_REPOSITION_DIST, sTuning.minRepositionDist);
    CVarSetFloat(TUNING_CVAR_MOVE_THRESHOLD_DIST, sTuning.moveThresholdDist);
    CVarSetFloat(TUNING_CVAR_CLOSE_EFFECT_DIST, sTuning.closeEffectDist);
    CVarSetFloat(TUNING_CVAR_FALLBACK_STALK_DIST, sTuning.fallbackStalkDist);
    CVarSetFloat(TUNING_CVAR_PROXIMITY_RUMBLE_DIST, sTuning.proximityRumbleDist);
    CVarSave();
}

} // namespace BenDrowned

void RegisterBenDrowned() {
    LoadTuning();

    if (!CVAR && (gPlayState != nullptr)) {
        CleanupOwnedStatue();
        ResetHistory();
    }

    COND_ID_HOOK(OnActorUpdate, ACTOR_PLAYER, CVAR, [](Actor*) {
        PlayState* play = gPlayState;
        Vec3f hiddenPoint;
        EnTorch2* statue;
        Player* player;
        bool spawnedThisFrame = false;
        bool dismissedThisFrame = false;
        bool statueVisible = false;

        if (play == nullptr) {
            return;
        }

        player = GET_PLAYER(play);
        if ((player == nullptr) || (player->actor.update == NULL)) {
            return;
        }

        HandlePlayStateChange(play);
        HandleZoneChange(play);
        DecrementCooldown(&sState.dialogueCooldown);

        if (!IsNormalGameplayState(play)) {
            return;
        }

        if (IsPlayerGroundedAndDry(player)) {
            RecordHistoryPoint(player);
        }
        DecrementCooldown(&sState.effectCooldown);
        DecrementCooldown(&sState.respawnCooldown);
        DecrementCooldown(&sState.colorDistortCooldown);
        DecrementCooldown(&sState.laughCooldown);
        DecrementCooldown(&sState.moveCooldown);
        DecrementCooldown(&sState.jumpscareCooldown);
        UpdateVisibilityJumpscareCamera(play);

        statue = GetStatue(play);
        if (statue != nullptr) {
            UpdateStatueLaugh(statue);
            UpdateStatueProximityRumble(player, statue);
            statueVisible = CanCameraSeePoint(play, statue->actor.world.pos);
            if (statueVisible) {
                UpdateStatueColorDistortion(statue);
                if (!sState.statueWasVisible) {
                    TriggerVisibilityJumpscare(play, player, statue);
                    sState.disappearAfterObserved = Rand_ZeroOne() < sTuning.disappearChance;
                }
                sState.statueObserved = true;
                sState.statueWasVisible = true;
                return;
            }

            if (sState.statueWasVisible) {
                sState.statueWasVisible = false;
                if (sState.statueObserved) {
                    if (sState.disappearAfterObserved) {
                        DismissStatue(play, statue);
                        statue = nullptr;
                        dismissedThisFrame = true;
                        sState.respawnCooldown = sTuning.respawnCooldownFrames;
                    } else {
                        Vec3f repositionPoint;
                        if (FindTargetPointFarFromCurrent(play, player, statue->actor.world.pos, &repositionPoint)) {
                            MoveStatue(play, player, statue, repositionPoint);
                            sState.statueObserved = false;
                            sState.moveCooldown = sTuning.moveCooldownFrames;
                        }
                    }
                }
            }
        }

        if (dismissedThisFrame) {
            return;
        }

        if ((statue == nullptr) && (sState.respawnCooldown <= 0) && FindDistantSpawnPoint(play, player, &hiddenPoint)) {
            SnapPointToFloor(play, &hiddenPoint);
            statue = SpawnStatue(play, hiddenPoint);
            spawnedThisFrame = statue != nullptr;
            if (spawnedThisFrame) {
                ResetColorDistortCooldown();
                sState.moveCooldown = sTuning.moveCooldownFrames;
            }
        }

        if ((statue != nullptr) && FindTargetPointFarFromCurrent(play, player, statue->actor.world.pos, &hiddenPoint)) {
            if (!spawnedThisFrame && (sState.moveCooldown <= 0) &&
                (Math3D_Vec3fDistSq(&statue->actor.world.pos, &hiddenPoint) > SQ(sTuning.moveThresholdDist))) {
                MoveStatue(play, player, statue, hiddenPoint);
                TriggerArrivalEffects(play, player, statue);
                sState.moveCooldown = sTuning.moveCooldownFrames;
            }
        }

        if (statue != nullptr) {
            SetStatueRotation(statue, player);
        }
    });

    COND_HOOK(OnOpenText, CVAR, [](u16* textId, bool* loadFromMessageTable) {
        PlayState* play = gPlayState;

        if (!ShouldCorruptOpenText(play, *textId)) {
            return;
        }

        CustomMessage::Entry entry = CustomMessage::LoadVanillaMessageTableEntry(*textId);
        if (entry.msg.empty()) {
            return;
        }
        if (!TryCorruptMessage(&entry)) {
            return;
        }
        CustomMessage::LoadCustomMessageIntoFont(entry);
        *loadFromMessageTable = false;
        sState.dialogueCooldown = sTuning.dialogueCooldownFrames;
    });

    COND_ID_HOOK(ShouldActorDraw, ACTOR_EN_TORCH2, CVAR, [](Actor* actor, bool*) {
        if ((sState.ignoreStatueInterpolationUntilFrame != 0) && (actor == (Actor*)sState.ownedStatue) &&
            (gPlayState != nullptr) && (gPlayState->gameplayFrames == sState.ignoreStatueInterpolationUntilFrame)) {
            FrameInterpolation_IgnoreActorMtx();
        }
    });

    COND_HOOK(OnPlayDrawWorldEnd, CVAR, []() { DrawDebugOverlay(); });
}

static RegisterShipInitFunc initFunc(RegisterBenDrowned, {
                                                             CVAR_NAME,
                                                             DEBUG_OVERLAY_CVAR,
                                                             TUNING_CVAR_MOVE_COOLDOWN,
                                                             TUNING_CVAR_RESPAWN_COOLDOWN,
                                                             TUNING_CVAR_DIALOGUE_COOLDOWN,
                                                             TUNING_CVAR_LAUGH_BASE,
                                                             TUNING_CVAR_LAUGH_RANDOM,
                                                             TUNING_CVAR_COLOR_DISTORT_BASE,
                                                             TUNING_CVAR_COLOR_DISTORT_RANDOM,
                                                             TUNING_CVAR_COLOR_DISTORT_DURATION,
                                                             TUNING_CVAR_DISAPPEAR_CHANCE,
                                                             TUNING_CVAR_DIALOGUE_CHANCE,
                                                             TUNING_CVAR_LAUGH_MIN_PITCH,
                                                             TUNING_CVAR_LAUGH_MAX_PITCH,
                                                             TUNING_CVAR_HISTORY_POINT_MIN_DIST,
                                                             TUNING_CVAR_MIN_SPAWN_DIST,
                                                             TUNING_CVAR_DISTANT_SPAWN_DIST,
                                                             TUNING_CVAR_MAX_NEARBY_DIST,
                                                             TUNING_CVAR_MIN_REPOSITION_DIST,
                                                             TUNING_CVAR_MOVE_THRESHOLD_DIST,
                                                             TUNING_CVAR_CLOSE_EFFECT_DIST,
                                                             TUNING_CVAR_FALLBACK_STALK_DIST,
                                                             TUNING_CVAR_PROXIMITY_RUMBLE_DIST,
                                                         });
