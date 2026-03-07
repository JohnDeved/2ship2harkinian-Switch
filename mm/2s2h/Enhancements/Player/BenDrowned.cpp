#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/Enhancements/Player/BenDrowned.h"
#include "2s2h/CustomMessage/CustomMessage.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "functions.h"
#include "sfx.h"
#include "variables.h"
#include "z64debug_display.h"
#include "z64quake.h"
#include "overlays/actors/ovl_En_Torch2/z_en_torch2.h"
#include "overlays/effects/ovl_Effect_Ss_Dust/z_eff_ss_dust.h"
}

#define CVAR_NAME "gEnhancements.Player.BenDrowned"
#define CVAR CVarGetInteger(CVAR_NAME, 0)

// --- History recording ---
#define HISTORY_SIZE 48
#define HISTORY_DISTINCT_CHECK_WINDOW 12
#define RECORD_INTERVAL_FRAMES 20
#define MIN_HISTORY_POINT_DIST_SQ (20.0f * 20.0f)

// --- Spawn / movement distances (defaults) ---
#define DEFAULT_MIN_SPAWN_DIST 50.0f
#define DEFAULT_DISTANT_SPAWN_DIST 140.0f
#define DEFAULT_MAX_NEARBY_DIST 225.0f
#define MOVE_DIST_SQ (15.0f * 15.0f)
#define DEFAULT_FALLBACK_STALK_DISTANCE 80.0f
#define DEFAULT_PROXIMITY_RUMBLE_DIST 100.0f

// --- Visibility ---
#define VISIBILITY_HEIGHT 40.0f
#define VISIBILITY_TOP_HEIGHT 80.0f
#define VISIBILITY_SIDE_OFFSET 20.0f
#define FLOOR_RAYCAST_HEIGHT 60.0f
#define WATCH_MARGIN 1.35f

// --- Cooldown durations ---
#define DEFAULT_MOVE_COOLDOWN_FRAMES 3600
#define EFFECT_COOLDOWN_FRAMES 30
#define DEFAULT_RESPAWN_COOLDOWN_FRAMES 1800
#define DEFAULT_DIALOGUE_COOLDOWN_FRAMES 600

// --- Disappearance ---
#define DEFAULT_DISAPPEAR_CHANCE 0.3f

// --- Laugh SFX ---
#define DEFAULT_LAUGH_BASE_FRAMES 300
#define DEFAULT_LAUGH_RANDOM_FRAMES 300
#define LAUGH_MIN_PITCH 0.9f
#define LAUGH_MAX_PITCH 1.1f

// --- Color distortion ---
#define COLOR_DISTORT_BASE_FRAMES 240
#define COLOR_DISTORT_RANDOM_FRAMES 360
#define COLOR_DISTORT_DURATION 12
#define COLOR_DISTORT_INTENSITY 160

// --- Proximity rumble ---
#define PROXIMITY_RUMBLE_MIN_STRENGTH 70
#define PROXIMITY_RUMBLE_MAX_STRENGTH 255
#define PROXIMITY_RUMBLE_MIN_DECAY 2
#define PROXIMITY_RUMBLE_MAX_DECAY 18
#define PROXIMITY_RUMBLE_STEP 8

// --- Arrival effects ---
#define CLOSE_EFFECT_DIST_SQ (110.0f * 110.0f)
#define JUMPSCARE_DIST_SQ (60.0f * 60.0f)
#define DUST_DRAW_FLAGS DUST_DRAWFLAG_RAND_COLOR_OFFSET
#define DUST_SCALE 120
#define DUST_SCALE_STEP (-8)
#define DUST_LIFE 6
#define QUAKE_SPEED 17232
#define QUAKE_PERTURB_X 2
#define QUAKE_PERTURB_Y 0
#define QUAKE_PERTURB_Z 0
#define QUAKE_PERTURB_W 0
#define QUAKE_DURATION 4
#define RUMBLE_STRENGTH 180
#define RUMBLE_DECAY 10
#define RUMBLE_DURATION 70

// --- Dialogue corruption ---
#define DIALOGUE_SEARCH_WINDOW 96
#define DIALOGUE_LATE_START_NUMERATOR 1
#define DIALOGUE_LATE_START_DENOMINATOR 4
#define DIALOGUE_TRIGGER_BASE 8
#define DIALOGUE_TRIGGER_RANGE 8
#define DIALOGUE_CHANCE 0.25f

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

static const s16 sFallbackYawOffsets[] = {
    FALLBACK_YAW_SIDE_NEAR,      (s16)-FALLBACK_YAW_SIDE_NEAR, FALLBACK_YAW_SIDE_FAR,
    (s16)-FALLBACK_YAW_SIDE_FAR, FALLBACK_YAW_BEHIND,
};

struct ColorDistortion {
    u16 colorFlag;
    u16 intensity;
};

static const std::string_view sDialoguePhrases[] = {
    "help me", "look behind", "don't turn", "you saw me", "it sees you", "watching you",
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
    s32 laughCooldown;
    s32 dialogueCooldown;
    bool disappearAfterObserved;
    bool statueObserved;
    bool statueWasVisible;
    PlayState* lastPlayState;
    EnTorch2* ownedStatue;
    bool spawnedStatue;
} sState;

// Runtime-adjustable tuning parameters (exposed via debug menu).
static BenDrowned::TuningParams sTuning = {
    DEFAULT_MOVE_COOLDOWN_FRAMES,
    DEFAULT_RESPAWN_COOLDOWN_FRAMES,
    DEFAULT_DIALOGUE_COOLDOWN_FRAMES,
    DEFAULT_LAUGH_BASE_FRAMES,
    DEFAULT_LAUGH_RANDOM_FRAMES,
    DEFAULT_DISAPPEAR_CHANCE,
    DIALOGUE_CHANCE,
    DEFAULT_MIN_SPAWN_DIST,
    DEFAULT_DISTANT_SPAWN_DIST,
    DEFAULT_MAX_NEARBY_DIST,
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
    sState.colorDistortCooldown = COLOR_DISTORT_BASE_FRAMES + (s32)(Rand_ZeroOne() * COLOR_DISTORT_RANDOM_FRAMES);
}

static size_t HistoryIndexFromEnd(size_t i) {
    return (sState.historyWriteIndex + HISTORY_SIZE - i - 1) % HISTORY_SIZE;
}

static void DecrementCooldown(s32* cooldown) {
    if (*cooldown > 0) {
        (*cooldown)--;
    }
}

static void ResetHistory() {
    sState.historyCount = 0;
    sState.historyWriteIndex = 0;
    sState.recordTimer = 0;
    sState.moveCooldown = 0;
    sState.respawnCooldown = 0;
    sState.colorDistortCooldown = 0;
    sState.effectCooldown = 0;
    sState.dialogueCooldown = 0;
    sState.disappearAfterObserved = false;
    sState.statueObserved = false;
    sState.statueWasVisible = false;
    ResetLaughCooldown();
}

static void ClearStatueTracking() {
    sState.ownedStatue = nullptr;
    sState.spawnedStatue = false;
    sState.disappearAfterObserved = false;
    sState.statueObserved = false;
    sState.statueWasVisible = false;
}

static void CleanupOwnedStatue() {
    if (sState.spawnedStatue && (sState.ownedStatue != nullptr) && (sState.ownedStatue->actor.update != NULL)) {
        Actor_Kill(&sState.ownedStatue->actor);
    }

    ClearStatueTracking();
}

static void HandlePlayStateChange(PlayState* play) {
    if (play != sState.lastPlayState) {
        sState.lastPlayState = play;
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

        if (Math3D_Vec3fDistSq(&sState.history[idx], &playerPos) < MIN_HISTORY_POINT_DIST_SQ) {
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

static bool TryReplaceDialoguePhrase(std::string* msg, std::string_view phrase, size_t searchStart, size_t searchEnd) {
    if ((searchStart >= msg->size()) || ((msg->size() - searchStart) < phrase.size())) {
        return false;
    }

    if (searchEnd > msg->size()) {
        searchEnd = msg->size();
    }

    for (size_t startPos = searchStart; (startPos + phrase.size()) <= searchEnd; startPos++) {
        bool fits = true;

        for (size_t i = 0; i < phrase.size(); i++) {
            char originalChar = (*msg)[startPos + i];

            if (!IsCorruptibleChar(originalChar)) {
                fits = false;
                break;
            }

            if (IsGlyphChar(phrase[i]) != IsGlyphChar(originalChar)) {
                fits = false;
                break;
            }
        }

        if (fits) {
            msg->replace(startPos, phrase.size(), phrase.data(), phrase.size());
            return true;
        }
    }

    return false;
}

static bool TryCorruptMessage(std::string* msg) {
    size_t phraseCount = std::size(sDialoguePhrases);
    size_t startIndex = RandomIndex(phraseCount);

    if (msg->empty()) {
        return false;
    }

    size_t lateStart = (msg->size() / DIALOGUE_LATE_START_DENOMINATOR) * DIALOGUE_LATE_START_NUMERATOR;
    s32 triggerStart = DIALOGUE_TRIGGER_BASE + (s32)(Rand_ZeroOne() * DIALOGUE_TRIGGER_RANGE);
    size_t searchStart = std::max(lateStart, static_cast<size_t>(triggerStart));
    size_t searchEnd = std::min(msg->size(), searchStart + (size_t)DIALOGUE_SEARCH_WINDOW);

    for (size_t offset = 0; offset < phraseCount; offset++) {
        if (TryReplaceDialoguePhrase(msg, sDialoguePhrases[(startIndex + offset) % phraseCount], searchStart,
                                     searchEnd)) {
            return true;
        }
    }

    return false;
}

static bool ShouldCorruptOpenText(PlayState* play, u16 textId) {
    return (play != nullptr) && (play->msgCtx.talkActor != nullptr) && (textId != 0) &&
           (textId != CUSTOM_MESSAGE_ID) && (sState.dialogueCooldown <= 0) &&
           (Rand_ZeroOne() < sTuning.dialogueChance);
}

static bool FindFallbackPoint(PlayState* play, Player* player, Vec3f* hiddenPoint) {
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

        if (!CanCameraSeePoint(play, candidatePoint)) {
            *hiddenPoint = candidatePoint;
            return true;
        }
    }

    return false;
}

static bool FindTargetPoint(PlayState* play, Player* player, Vec3f* hiddenPoint) {
    return FindHiddenHistoryPoint(play, player, SQ(sTuning.maxNearbyDist), hiddenPoint) ||
           FindFallbackPoint(play, player, hiddenPoint) || FindHiddenHistoryPoint(play, player, 0.0f, hiddenPoint);
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
}

static void TriggerArrivalEffects(PlayState* play, Player* player, EnTorch2* statue) {
    f32 distSq;
    s16 quakeIndex;

    if (sState.effectCooldown > 0) {
        return;
    }

    distSq = Math3D_Dist2DSq(player->actor.world.pos.x, player->actor.world.pos.z, statue->actor.world.pos.x,
                             statue->actor.world.pos.z);
    if (distSq > CLOSE_EFFECT_DIST_SQ) {
        return;
    }

    EffectSsDust_Spawn(play, DUST_DRAW_FLAGS, &statue->actor.world.pos, &sZeroVelocity, &sDustAccel, &sDustPrimColor,
                       &sDustEnvColor, DUST_SCALE, DUST_SCALE_STEP, DUST_LIFE, DUST_UPDATE_NORMAL);

    if (distSq < JUMPSCARE_DIST_SQ) {
        quakeIndex = Quake_Request(GET_ACTIVE_CAM(play), QUAKE_TYPE_3);

        if (quakeIndex >= 0) {
            Quake_SetSpeed(quakeIndex, QUAKE_SPEED);
            Quake_SetPerturbations(quakeIndex, QUAKE_PERTURB_X, QUAKE_PERTURB_Y, QUAKE_PERTURB_Z, QUAKE_PERTURB_W);
            Quake_SetDuration(quakeIndex, QUAKE_DURATION);
        }

        Rumble_Request(0.0f, RUMBLE_STRENGTH, RUMBLE_DECAY, RUMBLE_DURATION);
    }

    sState.effectCooldown = EFFECT_COOLDOWN_FRAMES;
}

static void UpdateStatueLaugh(EnTorch2* statue) {
    if ((statue == nullptr) || (sState.laughCooldown > 0)) {
        return;
    }

    f32 laughPitch = LAUGH_MIN_PITCH + (Rand_ZeroOne() * (LAUGH_MAX_PITCH - LAUGH_MIN_PITCH));
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
                         COLOR_DISTORT_DURATION);
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

} // namespace BenDrowned

void RegisterBenDrowned() {
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

        statue = GetStatue(play);
        if (statue != nullptr) {
            UpdateStatueLaugh(statue);
            UpdateStatueProximityRumble(player, statue);
            statueVisible = CanCameraSeePoint(play, statue->actor.world.pos);
            if (statueVisible) {
                UpdateStatueColorDistortion(statue);
                if (!sState.statueWasVisible) {
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
                        if (FindTargetPoint(play, player, &repositionPoint)) {
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

        if ((statue != nullptr) && FindTargetPoint(play, player, &hiddenPoint)) {
            if (!spawnedThisFrame && (sState.moveCooldown <= 0) &&
                (Math3D_Vec3fDistSq(&statue->actor.world.pos, &hiddenPoint) > MOVE_DIST_SQ)) {
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
        if (!TryCorruptMessage(&entry.msg)) {
            return;
        }

        entry.autoFormat = false;
        CustomMessage::LoadCustomMessageIntoFont(entry);
        *loadFromMessageTable = false;
        sState.dialogueCooldown = sTuning.dialogueCooldownFrames;
    });

    COND_HOOK(OnPlayDrawWorldEnd, CVAR, []() { DrawDebugOverlay(); });
}

static RegisterShipInitFunc initFunc(RegisterBenDrowned, { CVAR_NAME, DEBUG_OVERLAY_CVAR });
