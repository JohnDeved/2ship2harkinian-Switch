#include <array>
#include <cmath>
#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/BenGui/Notification.h"
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "functions.h"
#include "variables.h"
#include "z64effect_ss.h"
#include "z64quake.h"
#include "overlays/actors/ovl_En_Torch2/z_en_torch2.h"
#include "overlays/effects/ovl_Effect_Ss_Dust/z_eff_ss_dust.h"
}

#define CVAR_NAME "gEnhancements.Player.BenDrowned"
#define CVAR CVarGetInteger(CVAR_NAME, 0)

namespace {
constexpr size_t BEN_DROWNED_HISTORY_SIZE = 24;
constexpr s32 BEN_DROWNED_RECORD_INTERVAL_FRAMES = 20;
constexpr f32 BEN_DROWNED_MIN_SPAWN_DIST_SQ = 100.0f * 100.0f;
constexpr f32 BEN_DROWNED_MAX_NEARBY_DIST_SQ = 450.0f * 450.0f;
constexpr f32 BEN_DROWNED_MOVE_DIST_SQ = 30.0f * 30.0f;
constexpr f32 BEN_DROWNED_CLOSE_EFFECT_DIST_SQ = 220.0f * 220.0f;
constexpr f32 BEN_DROWNED_JUMPSCARE_DIST_SQ = 120.0f * 120.0f;
constexpr f32 BEN_DROWNED_PLAYER_STILLNESS_THRESHOLD = 0.1f;
constexpr f32 BEN_DROWNED_VISIBILITY_HEIGHT = 40.0f;
constexpr f32 BEN_DROWNED_FLOOR_RAYCAST_HEIGHT = 60.0f;
constexpr f32 BEN_DROWNED_WATCH_MARGIN = 1.15f;
constexpr f32 BEN_DROWNED_FALLBACK_STALK_DISTANCE = 160.0f;
constexpr s32 BEN_DROWNED_EFFECT_COOLDOWN_FRAMES = 30;
constexpr s32 BEN_DROWNED_HAUNT_IDLE_TRIGGER_FRAMES = 120;
constexpr s32 BEN_DROWNED_HAUNT_FIRST_PERSON_TRIGGER_FRAMES = 70;
constexpr s32 BEN_DROWNED_HAUNT_COOLDOWN_MIN_FRAMES = 180;
constexpr s32 BEN_DROWNED_HAUNT_COOLDOWN_RANGE_FRAMES = 120;
constexpr s32 BEN_DROWNED_HAUNT_MESSAGE_COOLDOWN_FRAMES = 600;
constexpr s32 BEN_DROWNED_HAUNT_FIRST_PERSON_COOLDOWN_FRAMES = 450;
constexpr f32 BEN_DROWNED_HAUNT_OFFSET_DIST = 70.0f;
constexpr f32 BEN_DROWNED_HAUNT_HEIGHT_OFFSET = 25.0f;
constexpr f32 BEN_DROWNED_HAUNT_DRIP_HEIGHT_OFFSET = 45.0f;
constexpr f32 BEN_DROWNED_HAUNT_RIPPLE_HEIGHT_OFFSET = 2.0f;
constexpr f32 BEN_DROWNED_HAUNT_SMOKE_SCALE = 150.0f;
constexpr s16 BEN_DROWNED_HAUNT_RIPPLE_RADIUS = 120;
constexpr s16 BEN_DROWNED_HAUNT_RIPPLE_RADIUS_MAX = 360;
constexpr s16 BEN_DROWNED_HAUNT_RIPPLE_LIFE = 0;
constexpr s8 BEN_DROWNED_HAUNT_REVERB = 0x20;
constexpr f32 BEN_DROWNED_HAUNT_NOTIFICATION_DURATION = 4.0f;
constexpr s16 BEN_DROWNED_FIRST_PERSON_QUAKE_SPEED = 21536;
constexpr s16 BEN_DROWNED_FIRST_PERSON_QUAKE_X = 3;
constexpr s16 BEN_DROWNED_FIRST_PERSON_QUAKE_Y = 0;
constexpr s16 BEN_DROWNED_FIRST_PERSON_QUAKE_Z = 0;
constexpr s16 BEN_DROWNED_FIRST_PERSON_QUAKE_W = 0;
constexpr s16 BEN_DROWNED_FIRST_PERSON_QUAKE_DURATION = 6;
constexpr u8 BEN_DROWNED_FIRST_PERSON_RUMBLE_STRENGTH = 120;
constexpr u8 BEN_DROWNED_FIRST_PERSON_RUMBLE_DECAY = 20;
constexpr u8 BEN_DROWNED_FIRST_PERSON_RUMBLE_DURATION = 20;
constexpr u16 BEN_DROWNED_DUST_DRAW_FLAGS = DUST_DRAWFLAG_RAND_COLOR_OFFSET;
constexpr s16 BEN_DROWNED_DUST_SCALE = 120;
constexpr s16 BEN_DROWNED_DUST_SCALE_STEP = -8;
constexpr s16 BEN_DROWNED_DUST_LIFE = 6;
constexpr s16 BEN_DROWNED_QUAKE_SPEED = 17232;
constexpr s16 BEN_DROWNED_QUAKE_PERTURB_X = 2;
constexpr s16 BEN_DROWNED_QUAKE_PERTURB_Y = 0;
constexpr s16 BEN_DROWNED_QUAKE_PERTURB_Z = 0;
constexpr s16 BEN_DROWNED_QUAKE_PERTURB_W = 0;
constexpr s16 BEN_DROWNED_QUAKE_DURATION = 4;
constexpr u8 BEN_DROWNED_RUMBLE_STRENGTH = 180;
constexpr u8 BEN_DROWNED_RUMBLE_DECAY = 10;
constexpr u8 BEN_DROWNED_RUMBLE_DURATION = 70;
constexpr s16 BEN_DROWNED_FALLBACK_YAW_SIDE_NEAR = 0x5000;
constexpr s16 BEN_DROWNED_FALLBACK_YAW_SIDE_FAR = 0x7000;
constexpr s16 BEN_DROWNED_FALLBACK_YAW_BEHIND = (s16)0x8000;
constexpr std::array<s16, 5> BEN_DROWNED_FALLBACK_YAW_OFFSETS = { BEN_DROWNED_FALLBACK_YAW_SIDE_NEAR,
                                                                   (s16)-BEN_DROWNED_FALLBACK_YAW_SIDE_NEAR,
                                                                   BEN_DROWNED_FALLBACK_YAW_SIDE_FAR,
                                                                   (s16)-BEN_DROWNED_FALLBACK_YAW_SIDE_FAR,
                                                                   BEN_DROWNED_FALLBACK_YAW_BEHIND };
constexpr std::array<s16, 3> BEN_DROWNED_HAUNT_YAW_OFFSETS = { 0x7000, -0x7000, (s16)0x8000 };
constexpr std::array<const char*, 5> BEN_DROWNED_CREEPY_MESSAGES = { "Did you hear that?", "Don't look away.",
                                                                     "It followed you here.", "The water remembers.",
                                                                     "You are not alone." };

enum BenDrownedHauntEffect {
    BEN_DROWNED_HAUNT_EFFECT_DRIP,
    BEN_DROWNED_HAUNT_EFFECT_RIPPLE,
    BEN_DROWNED_HAUNT_EFFECT_COLD_BREATH,
    BEN_DROWNED_HAUNT_EFFECT_GHOST_AUDIO,
    BEN_DROWNED_HAUNT_EFFECT_NOTIFICATION,
    BEN_DROWNED_HAUNT_EFFECT_COUNT,
};

struct BenDrownedHistoryEntry {
    Vec3f pos;
};

std::array<BenDrownedHistoryEntry, BEN_DROWNED_HISTORY_SIZE> sBenDrownedHistory;
Vec3f sBenDrownedZeroVelocity = { 0.0f, 0.0f, 0.0f };
Vec3f sBenDrownedDustAccel = { 0.0f, 0.08f, 0.0f };
Color_RGBA8 sBenDrownedDustPrimColor = { 170, 130, 90, 160 };
Color_RGBA8 sBenDrownedDustEnvColor = { 100, 60, 20, 110 };
size_t sBenDrownedHistoryCount = 0;
size_t sBenDrownedHistoryWriteIndex = 0;
s32 sBenDrownedRecordTimer = 0;
s32 sBenDrownedEffectCooldown = 0;
s32 sBenDrownedHauntCooldown = 0;
s32 sBenDrownedHauntMessageCooldown = 0;
s32 sBenDrownedFirstPersonCooldown = 0;
s32 sBenDrownedIdleFrames = 0;
s32 sBenDrownedFirstPersonFrames = 0;
PlayState* sLastPlayState = nullptr;
EnTorch2* sOwnedBenDrownedStatue = nullptr;
bool sSpawnedBenDrownedStatue = false;

void ResetBenDrownedHistory() {
    sBenDrownedHistoryCount = 0;
    sBenDrownedHistoryWriteIndex = 0;
    sBenDrownedRecordTimer = 0;
    sBenDrownedEffectCooldown = 0;
    sBenDrownedHauntCooldown = 0;
    sBenDrownedHauntMessageCooldown = 0;
    sBenDrownedFirstPersonCooldown = 0;
    sBenDrownedIdleFrames = 0;
    sBenDrownedFirstPersonFrames = 0;
}

void ClearBenDrownedStatueTracking() {
    sOwnedBenDrownedStatue = nullptr;
    sSpawnedBenDrownedStatue = false;
}

void CleanupOwnedBenDrownedStatue() {
    if (sSpawnedBenDrownedStatue && (sOwnedBenDrownedStatue != nullptr) &&
        (sOwnedBenDrownedStatue->actor.update != NULL)) {
        Actor_Kill(&sOwnedBenDrownedStatue->actor);
    }

    ClearBenDrownedStatueTracking();
}

void HandlePlayStateChange(PlayState* play) {
    if (play != sLastPlayState) {
        sLastPlayState = play;
        ResetBenDrownedHistory();
        ClearBenDrownedStatueTracking();
    }
}

bool IsNormalGameplayState(PlayState* play) {
    return (play->pauseCtx.state == PAUSE_STATE_OFF) && (play->pauseCtx.debugEditor == DEBUG_EDITOR_NONE) &&
           (play->msgCtx.msgMode == MSGMODE_NONE) && (play->transitionTrigger == TRANS_TRIGGER_OFF) &&
           (play->transitionMode == TRANS_MODE_OFF) && !Play_InCsMode(play);
}

bool IsPlayerGroundedAndDry(Player* player) {
    return (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) && !(player->actor.bgCheckFlags & BGCHECKFLAG_WATER);
}

bool IsPlayerStandingStill(Player* player) {
    return fabsf(player->actor.speed) < BEN_DROWNED_PLAYER_STILLNESS_THRESHOLD;
}

void RecordBenDrownedHistoryPoint(Player* player) {
    if (sBenDrownedRecordTimer > 0) {
        sBenDrownedRecordTimer--;
        return;
    }

    sBenDrownedHistory[sBenDrownedHistoryWriteIndex].pos = player->actor.world.pos;
    sBenDrownedHistoryWriteIndex = (sBenDrownedHistoryWriteIndex + 1) % BEN_DROWNED_HISTORY_SIZE;
    if (sBenDrownedHistoryCount < BEN_DROWNED_HISTORY_SIZE) {
        sBenDrownedHistoryCount++;
    }
    sBenDrownedRecordTimer = BEN_DROWNED_RECORD_INTERVAL_FRAMES;
}

bool IsPointOnScreen(PlayState* play, const Vec3f& worldPos) {
    Vec3f projectedPos;
    f32 projectedW;
    Vec3f projectedSource = worldPos;

    SkinMatrix_Vec3fMtxFMultXYZW(&play->viewProjectionMtxF, &projectedSource, &projectedPos, &projectedW);

    return (projectedW > 1.0f) && (projectedPos.z > 0.0f) &&
           (fabsf(projectedPos.x) < (projectedW * BEN_DROWNED_WATCH_MARGIN)) &&
           (fabsf(projectedPos.y) < (projectedW * BEN_DROWNED_WATCH_MARGIN));
}

bool CanCameraSeePoint(PlayState* play, const Vec3f& point) {
    Camera* camera = GET_ACTIVE_CAM(play);
    Vec3f watchPoint = point;
    Vec3f hitPos;
    CollisionPoly* hitPoly = nullptr;

    if (camera == nullptr) {
        return false;
    }

    watchPoint.y += BEN_DROWNED_VISIBILITY_HEIGHT;
    if (!IsPointOnScreen(play, watchPoint)) {
        return false;
    }

    return !BgCheck_AnyLineTest1(&play->colCtx, &camera->eye, &watchPoint, &hitPos, &hitPoly, false);
}

bool FindHiddenBenDrownedHistoryPoint(PlayState* play, Player* player, f32 maxDistSq, Vec3f* hiddenPoint) {
    for (size_t i = 0; i < sBenDrownedHistoryCount; i++) {
        size_t historyIndex =
            (sBenDrownedHistoryWriteIndex + BEN_DROWNED_HISTORY_SIZE - i - 1) % BEN_DROWNED_HISTORY_SIZE;
        Vec3f candidatePoint = sBenDrownedHistory[historyIndex].pos;
        f32 playerDistSq = Math3D_Vec3fDistSq(&candidatePoint, &player->actor.world.pos);

        if (playerDistSq < BEN_DROWNED_MIN_SPAWN_DIST_SQ) {
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

bool SnapBenDrownedPointToFloor(PlayState* play, Vec3f* point) {
    Vec3f raycastPos = *point;
    CollisionPoly* floorPoly = nullptr;
    s32 bgId;
    f32 floorHeight;

    raycastPos.y += BEN_DROWNED_FLOOR_RAYCAST_HEIGHT;
    floorHeight = BgCheck_EntityRaycastFloor3(&play->colCtx, &floorPoly, &bgId, &raycastPos);

    if (floorHeight != BGCHECK_Y_MIN) {
        point->y = floorHeight;
        return true;
    }

    return false;
}

bool IsBenDrownedStatueAlive(EnTorch2* statue) {
    return (statue != nullptr) && (statue->actor.update != NULL);
}

EnTorch2* GetBenDrownedStatue(PlayState* play) {
    EnTorch2* statue = play->actorCtx.elegyShells[TORCH2_PARAM_HUMAN];

    if (!IsBenDrownedStatueAlive(sOwnedBenDrownedStatue)) {
        ClearBenDrownedStatueTracking();
    }

    if (!IsBenDrownedStatueAlive(statue)) {
        statue = nullptr;
        play->actorCtx.elegyShells[TORCH2_PARAM_HUMAN] = nullptr;
    }

    if (statue != nullptr) {
        sOwnedBenDrownedStatue = statue;
    }

    return statue;
}

EnTorch2* SpawnBenDrownedStatue(PlayState* play, const Vec3f& spawnPos) {
    EnTorch2* statue = (EnTorch2*)Actor_Spawn(&play->actorCtx, play, ACTOR_EN_TORCH2, spawnPos.x, spawnPos.y, spawnPos.z,
                                              0, 0, 0, TORCH2_PARAM_HUMAN);

    if (statue != nullptr) {
        play->actorCtx.elegyShells[TORCH2_PARAM_HUMAN] = statue;
        sOwnedBenDrownedStatue = statue;
        sSpawnedBenDrownedStatue = true;
    }

    return statue;
}

void SetBenDrownedStatueRotation(EnTorch2* statue, Player* player) {
    s16 targetYaw = Math_Vec3f_Yaw(&statue->actor.world.pos, &player->actor.world.pos);

    statue->actor.world.rot.y = targetYaw;
    statue->actor.shape.rot.y = targetYaw;
    statue->actor.home.rot.y = targetYaw;
}

Vec3f GetBenDrownedHauntPoint(Player* player, s16 yawOffset, f32 dist, f32 yOffset) {
    Vec3f point = player->actor.world.pos;
    s16 yaw = player->actor.shape.rot.y + yawOffset;

    point.x += Math_SinS(yaw) * dist;
    point.z += Math_CosS(yaw) * dist;
    point.y += yOffset;
    return point;
}

template <size_t N> size_t GetBenDrownedRandomIndex() {
    return static_cast<size_t>(fminf(Rand_ZeroOne() * N, static_cast<f32>(N - 1)));
}

void DecrementCooldown(s32& cooldown) {
    if (cooldown > 0) {
        cooldown--;
    }
}

s16 GetBenDrownedRandomYawOffset() {
    return BEN_DROWNED_HAUNT_YAW_OFFSETS[GetBenDrownedRandomIndex<BEN_DROWNED_HAUNT_YAW_OFFSETS.size()>()];
}

void TriggerBenDrownedPhantomDrip(Player* player) {
    Vec3f soundPos = GetBenDrownedHauntPoint(player, GetBenDrownedRandomYawOffset(), BEN_DROWNED_HAUNT_OFFSET_DIST,
                                            BEN_DROWNED_HAUNT_DRIP_HEIGHT_OFFSET);

    Audio_PlaySfx_AtPosWithReverb(&soundPos, NA_SE_EV_WATERDROP_GRD, BEN_DROWNED_HAUNT_REVERB);
}

void TriggerBenDrownedDryRipple(PlayState* play, Player* player) {
    Vec3f ripplePos = player->actor.world.pos;

    ripplePos.y = player->actor.floorHeight + BEN_DROWNED_HAUNT_RIPPLE_HEIGHT_OFFSET;
    EffectSsGRipple_Spawn(play, &ripplePos, BEN_DROWNED_HAUNT_RIPPLE_RADIUS, BEN_DROWNED_HAUNT_RIPPLE_RADIUS_MAX,
                          BEN_DROWNED_HAUNT_RIPPLE_LIFE);
    Audio_PlaySfx_AtPosWithReverb(&ripplePos, NA_SE_EV_WATERDROP_GRD, BEN_DROWNED_HAUNT_REVERB);
}

void TriggerBenDrownedColdBreath(PlayState* play, Player* player) {
    Vec3f smokePos = GetBenDrownedHauntPoint(player, 0, 0.0f, BEN_DROWNED_HAUNT_HEIGHT_OFFSET);
    Vec3f smokeVelocity;

    smokeVelocity.x = Rand_CenteredFloat(0.15f);
    smokeVelocity.y = 0.2f;
    smokeVelocity.z = Rand_CenteredFloat(0.15f);

    EffectSsIceSmoke_Spawn(play, &smokePos, &smokeVelocity, &gZeroVec3f, BEN_DROWNED_HAUNT_SMOKE_SCALE);
    Audio_PlaySfx_AtPosWithReverb(&smokePos, NA_SE_EV_UNDER_WATER, BEN_DROWNED_HAUNT_REVERB);
}

void TriggerBenDrownedGhostAudio(Player* player) {
    Vec3f soundPos = GetBenDrownedHauntPoint(player, GetBenDrownedRandomYawOffset(), BEN_DROWNED_HAUNT_OFFSET_DIST,
                                            BEN_DROWNED_HAUNT_HEIGHT_OFFSET);
    u16 sound = (Rand_ZeroOne() < 0.5f) ? NA_SE_EN_PO_LAUGH : NA_SE_EN_STALKIDS_FLOAT;

    Audio_PlaySfx_AtPosWithReverb(&soundPos, sound, BEN_DROWNED_HAUNT_REVERB);
}

void TriggerBenDrownedCreepyNotification() {
    if (sBenDrownedHauntMessageCooldown > 0) {
        return;
    }

    Notification::Emit({
        .message = BEN_DROWNED_CREEPY_MESSAGES[GetBenDrownedRandomIndex<BEN_DROWNED_CREEPY_MESSAGES.size()>()],
        .remainingTime = BEN_DROWNED_HAUNT_NOTIFICATION_DURATION,
        .mute = true,
    });

    sBenDrownedHauntMessageCooldown = BEN_DROWNED_HAUNT_MESSAGE_COOLDOWN_FRAMES;
}

void TriggerBenDrownedFirstPersonScare(PlayState* play, Player* player) {
    s16 quakeIndex;

    if (sBenDrownedFirstPersonCooldown > 0) {
        return;
    }

    Audio_PlaySfx_2(NA_SE_SY_STALKIDS_PSYCHO);
    Audio_PlaySfx_AtPosWithReverb(&player->actor.world.pos, NA_SE_EN_PO_DISAPPEAR, BEN_DROWNED_HAUNT_REVERB);

    quakeIndex = Quake_Request(GET_ACTIVE_CAM(play), QUAKE_TYPE_3);
    if (quakeIndex >= 0) {
        Quake_SetSpeed(quakeIndex, BEN_DROWNED_FIRST_PERSON_QUAKE_SPEED);
        Quake_SetPerturbations(quakeIndex, BEN_DROWNED_FIRST_PERSON_QUAKE_X, BEN_DROWNED_FIRST_PERSON_QUAKE_Y,
                               BEN_DROWNED_FIRST_PERSON_QUAKE_Z, BEN_DROWNED_FIRST_PERSON_QUAKE_W);
        Quake_SetDuration(quakeIndex, BEN_DROWNED_FIRST_PERSON_QUAKE_DURATION);
    }

    Rumble_Request(0.0f, BEN_DROWNED_FIRST_PERSON_RUMBLE_STRENGTH, BEN_DROWNED_FIRST_PERSON_RUMBLE_DECAY,
                   BEN_DROWNED_FIRST_PERSON_RUMBLE_DURATION);
    TriggerBenDrownedCreepyNotification();
    sBenDrownedFirstPersonCooldown = BEN_DROWNED_HAUNT_FIRST_PERSON_COOLDOWN_FRAMES;
}

void TriggerRandomBenDrownedHaunt(PlayState* play, Player* player) {
    switch (GetBenDrownedRandomIndex<BEN_DROWNED_HAUNT_EFFECT_COUNT>()) {
        case BEN_DROWNED_HAUNT_EFFECT_DRIP:
            TriggerBenDrownedPhantomDrip(player);
            break;
        case BEN_DROWNED_HAUNT_EFFECT_RIPPLE:
            TriggerBenDrownedDryRipple(play, player);
            break;
        case BEN_DROWNED_HAUNT_EFFECT_COLD_BREATH:
            TriggerBenDrownedColdBreath(play, player);
            break;
        case BEN_DROWNED_HAUNT_EFFECT_GHOST_AUDIO:
            TriggerBenDrownedGhostAudio(player);
            break;
        default:
            TriggerBenDrownedCreepyNotification();
            break;
    }

    sBenDrownedHauntCooldown = BEN_DROWNED_HAUNT_COOLDOWN_MIN_FRAMES +
                               (s32)(Rand_ZeroOne() * BEN_DROWNED_HAUNT_COOLDOWN_RANGE_FRAMES);
}

void UpdateBenDrownedHaunting(PlayState* play, Player* player) {
    DecrementCooldown(sBenDrownedEffectCooldown);
    DecrementCooldown(sBenDrownedHauntCooldown);
    DecrementCooldown(sBenDrownedHauntMessageCooldown);
    DecrementCooldown(sBenDrownedFirstPersonCooldown);

    if (IsPlayerGroundedAndDry(player) && IsPlayerStandingStill(player)) {
        sBenDrownedIdleFrames++;
    } else {
        sBenDrownedIdleFrames = 0;
    }

    if (player->unk_AA5 == PLAYER_UNKAA5_3) {
        sBenDrownedFirstPersonFrames++;
        if (sBenDrownedFirstPersonFrames == BEN_DROWNED_HAUNT_FIRST_PERSON_TRIGGER_FRAMES) {
            TriggerBenDrownedFirstPersonScare(play, player);
        }
    } else {
        sBenDrownedFirstPersonFrames = 0;
    }

    if ((sBenDrownedIdleFrames >= BEN_DROWNED_HAUNT_IDLE_TRIGGER_FRAMES) && (sBenDrownedHauntCooldown <= 0)) {
        TriggerRandomBenDrownedHaunt(play, player);
    }
}

bool FindBenDrownedFallbackPoint(PlayState* play, Player* player, Vec3f* hiddenPoint) {
    Camera* camera = GET_ACTIVE_CAM(play);

    if (camera == nullptr) {
        return false;
    }

    for (s16 yawOffset : BEN_DROWNED_FALLBACK_YAW_OFFSETS) {
        Vec3f candidatePoint = player->actor.world.pos;
        s16 stalkYaw = Math_Vec3f_Yaw(&camera->eye, &camera->at) + yawOffset;

        candidatePoint.x += Math_SinS(stalkYaw) * BEN_DROWNED_FALLBACK_STALK_DISTANCE;
        candidatePoint.z += Math_CosS(stalkYaw) * BEN_DROWNED_FALLBACK_STALK_DISTANCE;

        if (!SnapBenDrownedPointToFloor(play, &candidatePoint)) {
            continue;
        }

        if (!CanCameraSeePoint(play, candidatePoint)) {
            *hiddenPoint = candidatePoint;
            return true;
        }
    }

    return false;
}

bool FindBenDrownedTargetPoint(PlayState* play, Player* player, Vec3f* hiddenPoint) {
    return FindHiddenBenDrownedHistoryPoint(play, player, BEN_DROWNED_MAX_NEARBY_DIST_SQ, hiddenPoint) ||
           FindBenDrownedFallbackPoint(play, player, hiddenPoint) ||
           FindHiddenBenDrownedHistoryPoint(play, player, 0.0f, hiddenPoint);
}

void MoveBenDrownedStatue(PlayState* play, Player* player, EnTorch2* statue, const Vec3f& targetPoint) {
    Vec3f snappedPoint = targetPoint;

    SnapBenDrownedPointToFloor(play, &snappedPoint);
    Math_Vec3f_Copy(&statue->actor.world.pos, &snappedPoint);
    Math_Vec3f_Copy(&statue->actor.home.pos, &snappedPoint);
    Math_Vec3f_Copy(&statue->actor.prevPos, &snappedPoint);
    Math_Vec3f_Copy(&statue->actor.focus.pos, &snappedPoint);

    Math_Vec3f_Copy(&statue->actor.velocity, &sBenDrownedZeroVelocity);
    statue->actor.speed = 0.0f;
    statue->actor.floorHeight = snappedPoint.y;
    statue->actor.bgCheckFlags |= BGCHECKFLAG_GROUND;
    statue->state = TORCH2_STATE_SOLID;
    statue->framesUntilNextState = 0;
    statue->alpha = 255;

    SetBenDrownedStatueRotation(statue, player);
}

void TriggerBenDrownedArrivalEffects(PlayState* play, Player* player, EnTorch2* statue) {
    f32 distSq;
    f32 dist;
    s16 quakeIndex;

    if (sBenDrownedEffectCooldown > 0) {
        return;
    }

    distSq = Math3D_Dist2DSq(player->actor.world.pos.x, player->actor.world.pos.z, statue->actor.world.pos.x,
                             statue->actor.world.pos.z);
    if (distSq > BEN_DROWNED_CLOSE_EFFECT_DIST_SQ) {
        return;
    }

    EffectSsDust_Spawn(play, BEN_DROWNED_DUST_DRAW_FLAGS, &statue->actor.world.pos,
                       &sBenDrownedZeroVelocity, &sBenDrownedDustAccel, &sBenDrownedDustPrimColor,
                       &sBenDrownedDustEnvColor, BEN_DROWNED_DUST_SCALE, BEN_DROWNED_DUST_SCALE_STEP,
                       BEN_DROWNED_DUST_LIFE, DUST_UPDATE_NORMAL);

    Actor_PlaySfx(&statue->actor, (distSq < BEN_DROWNED_JUMPSCARE_DIST_SQ) ? NA_SE_EV_STONEDOOR_STOP
                                                                            : NA_SE_EV_STONE_STATUE_OPEN);

    if (distSq < BEN_DROWNED_JUMPSCARE_DIST_SQ) {
        dist = sqrtf(distSq);
        quakeIndex = Quake_Request(GET_ACTIVE_CAM(play), QUAKE_TYPE_3);

        if (quakeIndex >= 0) {
            Quake_SetSpeed(quakeIndex, BEN_DROWNED_QUAKE_SPEED);
            Quake_SetPerturbations(quakeIndex, BEN_DROWNED_QUAKE_PERTURB_X, BEN_DROWNED_QUAKE_PERTURB_Y,
                                   BEN_DROWNED_QUAKE_PERTURB_Z, BEN_DROWNED_QUAKE_PERTURB_W);
            Quake_SetDuration(quakeIndex, BEN_DROWNED_QUAKE_DURATION);
        }

        Rumble_Request(dist, BEN_DROWNED_RUMBLE_STRENGTH, BEN_DROWNED_RUMBLE_DECAY, BEN_DROWNED_RUMBLE_DURATION);
    }

    sBenDrownedEffectCooldown = BEN_DROWNED_EFFECT_COOLDOWN_FRAMES;
}
} // namespace

void RegisterBenDrowned() {
    if (!CVAR && (gPlayState != nullptr)) {
        CleanupOwnedBenDrownedStatue();
        ResetBenDrownedHistory();
    }

    COND_ID_HOOK(OnActorUpdate, ACTOR_PLAYER, CVAR, [](Actor*) {
        PlayState* play = gPlayState;
        Vec3f hiddenPoint;
        EnTorch2* statue;
        Player* player;
        bool spawnedStatueThisFrame = false;

        if (play == nullptr) {
            return;
        }

        player = GET_PLAYER(play);
        if ((player == nullptr) || (player->actor.update == NULL)) {
            return;
        }

        HandlePlayStateChange(play);

        if (!IsNormalGameplayState(play)) {
            return;
        }

        if (IsPlayerGroundedAndDry(player)) {
            RecordBenDrownedHistoryPoint(player);
        }
        UpdateBenDrownedHaunting(play, player);

        statue = GetBenDrownedStatue(play);
        if ((statue != nullptr) && CanCameraSeePoint(play, statue->actor.world.pos)) {
            return;
        }

        if (FindBenDrownedTargetPoint(play, player, &hiddenPoint)) {
            if (statue == nullptr) {
                SnapBenDrownedPointToFloor(play, &hiddenPoint);
                statue = SpawnBenDrownedStatue(play, hiddenPoint);
                spawnedStatueThisFrame = statue != nullptr;
            }

            if ((statue != nullptr) &&
                (spawnedStatueThisFrame ||
                 (Math3D_Vec3fDistSq(&statue->actor.world.pos, &hiddenPoint) > BEN_DROWNED_MOVE_DIST_SQ))) {
                MoveBenDrownedStatue(play, player, statue, hiddenPoint);
                TriggerBenDrownedArrivalEffects(play, player, statue);
            }
        }

        if (statue != nullptr) {
            SetBenDrownedStatueRotation(statue, player);
        }
    });
}

static RegisterShipInitFunc initFunc(RegisterBenDrowned, { CVAR_NAME });
