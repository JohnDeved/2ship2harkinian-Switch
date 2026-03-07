#include <array>
#include <cmath>
#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "functions.h"
#include "variables.h"
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
constexpr f32 BEN_DROWNED_VISIBILITY_HEIGHT = 40.0f;
constexpr f32 BEN_DROWNED_FLOOR_RAYCAST_HEIGHT = 60.0f;
constexpr f32 BEN_DROWNED_WATCH_MARGIN = 1.35f;
constexpr f32 BEN_DROWNED_FALLBACK_STALK_DISTANCE = 160.0f;
constexpr s32 BEN_DROWNED_MOVE_COOLDOWN_FRAMES = 3600;
constexpr s32 BEN_DROWNED_EFFECT_COOLDOWN_FRAMES = 30;
constexpr s32 BEN_DROWNED_DIALOGUE_COOLDOWN_FRAMES = 900;
constexpr size_t BEN_DROWNED_DIALOGUE_SEARCH_WINDOW = 48;
constexpr s32 BEN_DROWNED_DIALOGUE_TRIGGER_BASE = 8;
constexpr s32 BEN_DROWNED_DIALOGUE_TRIGGER_RANGE = 8;
constexpr f32 BEN_DROWNED_DIALOGUE_CHANCE = 0.12f;
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
struct BenDrownedDialoguePhrase {
    const char* text;
    size_t length;
};

constexpr std::array<BenDrownedDialoguePhrase, 6> BEN_DROWNED_DIALOGUE_PHRASES = {
    BenDrownedDialoguePhrase{ "help me", sizeof("help me") - 1 },
    BenDrownedDialoguePhrase{ "look behind", sizeof("look behind") - 1 },
    BenDrownedDialoguePhrase{ "don't turn", sizeof("don't turn") - 1 },
    BenDrownedDialoguePhrase{ "you saw me", sizeof("you saw me") - 1 },
    BenDrownedDialoguePhrase{ "it sees you", sizeof("it sees you") - 1 },
    BenDrownedDialoguePhrase{ "watching you", sizeof("watching you") - 1 },
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
s32 sBenDrownedMoveCooldown = 0;
s32 sBenDrownedEffectCooldown = 0;
s32 sBenDrownedDialogueCooldown = 0;
s16 sBenDrownedDialogueTriggerPos = -1;
u16 sBenDrownedDialogueTextId = 0;
bool sBenDrownedDialogueInjected = false;
PlayState* sLastPlayState = nullptr;
EnTorch2* sOwnedBenDrownedStatue = nullptr;
bool sSpawnedBenDrownedStatue = false;

void ResetBenDrownedHistory() {
    sBenDrownedHistoryCount = 0;
    sBenDrownedHistoryWriteIndex = 0;
    sBenDrownedRecordTimer = 0;
    sBenDrownedMoveCooldown = 0;
    sBenDrownedEffectCooldown = 0;
    sBenDrownedDialogueCooldown = 0;
    sBenDrownedDialogueTriggerPos = -1;
    sBenDrownedDialogueTextId = 0;
    sBenDrownedDialogueInjected = false;
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

template <size_t N> size_t GetBenDrownedRandomIndex() {
    return static_cast<size_t>(fminf(Rand_ZeroOne() * N, static_cast<f32>(N - 1)));
}

void ResetBenDrownedDialogueState() {
    sBenDrownedDialogueTriggerPos = -1;
    sBenDrownedDialogueTextId = 0;
    sBenDrownedDialogueInjected = false;
}

bool IsBenDrownedDialogueMsgMode(MessageContext* msgCtx) {
    return (msgCtx->msgMode == MSGMODE_TEXT_STARTING) || (msgCtx->msgMode == MSGMODE_TEXT_NEXT_MSG) ||
           (msgCtx->msgMode == MSGMODE_TEXT_CONTINUING) || (msgCtx->msgMode == MSGMODE_TEXT_DISPLAYING) ||
           (msgCtx->msgMode == MSGMODE_TEXT_AWAIT_INPUT);
}

bool IsBenDrownedCorruptibleChar(char ch) {
    return (ch >= ' ') && (ch <= '~');
}

bool TryInjectBenDrownedDialoguePhrase(PlayState* play, MessageContext* msgCtx, const BenDrownedDialoguePhrase& phrase) {
    size_t searchStart = msgCtx->textDrawPos + 1;
    size_t searchEnd;
    size_t startPos;
    size_t i;

    if ((searchStart >= msgCtx->decodedTextLen) || ((msgCtx->decodedTextLen - searchStart) < phrase.length)) {
        return false;
    }

    searchEnd = searchStart + BEN_DROWNED_DIALOGUE_SEARCH_WINDOW;
    if (searchEnd > msgCtx->decodedTextLen) {
        searchEnd = msgCtx->decodedTextLen;
    }

    for (startPos = searchStart; (startPos + phrase.length) <= searchEnd; startPos++) {
        bool fits = true;

        for (i = 0; i < phrase.length; i++) {
            if (!IsBenDrownedCorruptibleChar(msgCtx->decodedBuffer.schar[startPos + i])) {
                fits = false;
                break;
            }
        }

        if (!fits) {
            continue;
        }

        for (i = 0; i < phrase.length; i++) {
            if (msgCtx->decodedBuffer.schar[startPos + i] != phrase.text[i]) {
                msgCtx->decodedBuffer.schar[startPos + i] = phrase.text[i];
                // Font character slots are packed in 0x80-byte increments.
                Font_LoadCharNES(play, phrase.text[i], (startPos + i) << 7);
            }
        }
        return true;
    }

    return false;
}

void UpdateBenDrownedDialogueHaunting(PlayState* play) {
    MessageContext* msgCtx = &play->msgCtx;

    if (sBenDrownedDialogueCooldown > 0) {
        sBenDrownedDialogueCooldown--;
    }

    if ((gSaveContext.options.language == LANGUAGE_JPN) || (msgCtx->talkActor == nullptr) ||
        (msgCtx->currentTextId == 0) || // no active textbox
        !IsBenDrownedDialogueMsgMode(msgCtx)) {
        ResetBenDrownedDialogueState();
        return;
    }

    if (msgCtx->currentTextId != sBenDrownedDialogueTextId) {
        sBenDrownedDialogueTextId = msgCtx->currentTextId;
        sBenDrownedDialogueInjected = false;
        sBenDrownedDialogueTriggerPos = -1;

        if ((sBenDrownedDialogueCooldown <= 0) && (Rand_ZeroOne() < BEN_DROWNED_DIALOGUE_CHANCE)) {
            sBenDrownedDialogueTriggerPos =
                BEN_DROWNED_DIALOGUE_TRIGGER_BASE + (s32)(Rand_ZeroOne() * BEN_DROWNED_DIALOGUE_TRIGGER_RANGE);
        }
    }

    if (!sBenDrownedDialogueInjected && (sBenDrownedDialogueTriggerPos >= 0) &&
        ((msgCtx->msgMode == MSGMODE_TEXT_DISPLAYING) || (msgCtx->msgMode == MSGMODE_TEXT_AWAIT_INPUT)) &&
        (msgCtx->textDrawPos >= sBenDrownedDialogueTriggerPos)) {
        size_t startIndex = GetBenDrownedRandomIndex<BEN_DROWNED_DIALOGUE_PHRASES.size()>();

        sBenDrownedDialogueInjected = true;
        for (size_t offset = 0; offset < BEN_DROWNED_DIALOGUE_PHRASES.size(); offset++) {
            if (TryInjectBenDrownedDialoguePhrase(
                    play, msgCtx,
                    BEN_DROWNED_DIALOGUE_PHRASES[(startIndex + offset) % BEN_DROWNED_DIALOGUE_PHRASES.size()])) {
                sBenDrownedDialogueCooldown = BEN_DROWNED_DIALOGUE_COOLDOWN_FRAMES;
                break;
            }
        }
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
        UpdateBenDrownedDialogueHaunting(play);

        if (!IsNormalGameplayState(play)) {
            return;
        }

        if (IsPlayerGroundedAndDry(player)) {
            RecordBenDrownedHistoryPoint(player);
        }
        if (sBenDrownedEffectCooldown > 0) {
            sBenDrownedEffectCooldown--;
        }
        if (sBenDrownedMoveCooldown > 0) {
            sBenDrownedMoveCooldown--;
        }

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
                 ((sBenDrownedMoveCooldown <= 0) &&
                  (Math3D_Vec3fDistSq(&statue->actor.world.pos, &hiddenPoint) > BEN_DROWNED_MOVE_DIST_SQ)))) {
                MoveBenDrownedStatue(play, player, statue, hiddenPoint);
                TriggerBenDrownedArrivalEffects(play, player, statue);
                sBenDrownedMoveCooldown = BEN_DROWNED_MOVE_COOLDOWN_FRAMES;
            }
        }

        if (statue != nullptr) {
            // Keep the statue re-facing Link whenever it is off-camera, even if it is not ready to move again yet.
            SetBenDrownedStatueRotation(statue, player);
        }
    });
}

static RegisterShipInitFunc initFunc(RegisterBenDrowned, { CVAR_NAME });
