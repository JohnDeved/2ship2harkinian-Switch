#include <array>
#include <cmath>
#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "functions.h"
#include "variables.h"
#include "overlays/actors/ovl_En_Torch2/z_en_torch2.h"
}

#define CVAR_NAME "gEnhancements.Player.BenDrowned"
#define CVAR CVarGetInteger(CVAR_NAME, 0)

namespace {
constexpr size_t BEN_DROWNED_HISTORY_SIZE = 24;
constexpr s32 BEN_DROWNED_RECORD_INTERVAL_FRAMES = 20;
constexpr f32 BEN_DROWNED_MIN_SPAWN_DIST_SQ = 100.0f * 100.0f;
constexpr f32 BEN_DROWNED_MOVE_DIST_SQ = 30.0f * 30.0f;
constexpr f32 BEN_DROWNED_VISIBILITY_HEIGHT = 40.0f;
constexpr f32 BEN_DROWNED_FLOOR_RAYCAST_HEIGHT = 60.0f;

struct BenDrownedHistoryEntry {
    Vec3f pos;
};

std::array<BenDrownedHistoryEntry, BEN_DROWNED_HISTORY_SIZE> sBenDrownedHistory;
size_t sBenDrownedHistoryCount = 0;
size_t sBenDrownedHistoryWriteIndex = 0;
s32 sBenDrownedRecordTimer = 0;
PlayState* sLastPlayState = nullptr;
EnTorch2* sOwnedBenDrownedStatue = nullptr;
bool sSpawnedBenDrownedStatue = false;

void ResetBenDrownedHistory() {
    sBenDrownedHistoryCount = 0;
    sBenDrownedHistoryWriteIndex = 0;
    sBenDrownedRecordTimer = 0;
}

void ClearBenDrownedStatueTracking() {
    sOwnedBenDrownedStatue = nullptr;
    sSpawnedBenDrownedStatue = false;
}

void CleanupOwnedBenDrownedStatue() {
    if (sSpawnedBenDrownedStatue && (sOwnedBenDrownedStatue != nullptr) && (sOwnedBenDrownedStatue->actor.update != NULL)) {
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

    return (projectedW > 1.0f) && (projectedPos.z > 0.0f) && (fabsf(projectedPos.x) < projectedW) &&
           (fabsf(projectedPos.y) < projectedW);
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

bool FindHiddenBenDrownedPoint(PlayState* play, Player* player, Vec3f* hiddenPoint) {
    for (size_t i = 0; i < sBenDrownedHistoryCount; i++) {
        size_t historyIndex =
            (sBenDrownedHistoryWriteIndex + BEN_DROWNED_HISTORY_SIZE - i - 1) % BEN_DROWNED_HISTORY_SIZE;
        Vec3f candidatePoint = sBenDrownedHistory[historyIndex].pos;

        if (Math3D_Vec3fDistSq(&candidatePoint, &player->actor.world.pos) < BEN_DROWNED_MIN_SPAWN_DIST_SQ) {
            continue;
        }

        if (!CanCameraSeePoint(play, candidatePoint)) {
            *hiddenPoint = candidatePoint;
            return true;
        }
    }

    return false;
}

void SnapBenDrownedPointToFloor(PlayState* play, Vec3f* point) {
    Vec3f raycastPos = *point;
    CollisionPoly* floorPoly = nullptr;
    s32 bgId;
    f32 floorHeight;

    raycastPos.y += BEN_DROWNED_FLOOR_RAYCAST_HEIGHT;
    floorHeight = BgCheck_EntityRaycastFloor3(&play->colCtx, &floorPoly, &bgId, &raycastPos);

    if (floorHeight != BGCHECK_Y_MIN) {
        point->y = floorHeight;
    }
}

EnTorch2* GetBenDrownedStatue(PlayState* play) {
    EnTorch2* statue = play->actorCtx.elegyShells[TORCH2_PARAM_HUMAN];

    if ((sOwnedBenDrownedStatue != nullptr) && (sOwnedBenDrownedStatue->actor.update == NULL)) {
        ClearBenDrownedStatueTracking();
    }

    if ((statue != nullptr) && (statue->actor.update == NULL)) {
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

void MoveBenDrownedStatue(PlayState* play, Player* player, EnTorch2* statue, const Vec3f& targetPoint) {
    Vec3f snappedPoint = targetPoint;

    SnapBenDrownedPointToFloor(play, &snappedPoint);
    Math_Vec3f_Copy(&statue->actor.world.pos, &snappedPoint);
    Math_Vec3f_Copy(&statue->actor.home.pos, &snappedPoint);
    Math_Vec3f_Copy(&statue->actor.prevPos, &snappedPoint);
    Math_Vec3f_Copy(&statue->actor.focus.pos, &snappedPoint);

    statue->actor.velocity.x = 0.0f;
    statue->actor.velocity.y = 0.0f;
    statue->actor.velocity.z = 0.0f;
    statue->actor.speed = 0.0f;
    statue->actor.floorHeight = snappedPoint.y;
    statue->actor.bgCheckFlags |= BGCHECKFLAG_GROUND;
    statue->state = TORCH2_STATE_SOLID;
    statue->framesUntilNextState = 0;
    statue->alpha = 255;

    SetBenDrownedStatueRotation(statue, player);
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

        if ((player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) && !(player->actor.bgCheckFlags & BGCHECKFLAG_WATER)) {
            RecordBenDrownedHistoryPoint(player);
        }

        statue = GetBenDrownedStatue(play);
        if ((statue != nullptr) && CanCameraSeePoint(play, statue->actor.world.pos)) {
            return;
        }

        if (FindHiddenBenDrownedPoint(play, player, &hiddenPoint)) {
            if (statue == nullptr) {
                SnapBenDrownedPointToFloor(play, &hiddenPoint);
                statue = SpawnBenDrownedStatue(play, hiddenPoint);
                spawnedStatueThisFrame = statue != nullptr;
            }

            if ((statue != nullptr) &&
                (spawnedStatueThisFrame ||
                 (Math3D_Vec3fDistSq(&statue->actor.world.pos, &hiddenPoint) > BEN_DROWNED_MOVE_DIST_SQ))) {
                MoveBenDrownedStatue(play, player, statue, hiddenPoint);
            }
        }

        if (statue != nullptr) {
            SetBenDrownedStatueRotation(statue, player);
        }
    });
}

static RegisterShipInitFunc initFunc(RegisterBenDrowned, { CVAR_NAME });
