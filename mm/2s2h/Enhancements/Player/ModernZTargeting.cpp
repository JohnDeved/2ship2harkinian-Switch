#include <cstdlib>
#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "variables.h"
}

#define CVAR_NAME "gEnhancements.Player.ModernZTargeting.Enable"
#define CVAR_LEGACY_NAME "gEnhancements.Player.ModernZTargeting"
#define CVAR_CAMERA_NAME "gEnhancements.Player.ModernZTargeting.CameraBasedLock"
#define CVAR_RSTICK_NAME "gEnhancements.Player.ModernZTargeting.RightStickSwitch"
#define CVAR_LSHOULDER_NAME "gEnhancements.Player.ModernZTargeting.LeftShoulderSwitch"
#define CVAR_ZTOGGLE_NAME "gEnhancements.Player.ModernZTargeting.ZToggleRelease"
#define CVAR CVarGetInteger(CVAR_NAME, 0)
#define CVAR_RSTICK CVarGetInteger(CVAR_RSTICK_NAME, 1)
#define CVAR_LSHOULDER CVarGetInteger(CVAR_LSHOULDER_NAME, 0)

// Require a strong horizontal right stick deflection before arming a target switch.
#define STICK_FLICK_THRESHOLD 28
// Stick must return below this before another switch is allowed.
#define STICK_RELEASE_THRESHOLD 10
// Horizontal movement must clearly dominate vertical movement to avoid look-around input triggering a switch.
#define STICK_HORIZONTAL_MARGIN 10
// Stick must return to neutral quickly for the gesture to count as a flick.
#define STICK_FLICK_MAX_FRAMES 5
// Minimum frames between switches as a safety net.
#define SWITCH_COOLDOWN 8
#define SWITCH_DIRECTION_NONE 0
#define SWITCH_DIRECTION_LEFT -1
#define SWITCH_DIRECTION_RIGHT 1
#define SWITCH_DIRECTION_NEAREST 2

static s32 sSwitchCooldown = 0;
static bool sStickReleased = true;
static s32 sPendingSwitchDirection = SWITCH_DIRECTION_NONE;
static s32 sPendingFlickFrames = 0;

static void MigrateLegacyCVar() {
    if (CVarGet(CVAR_NAME) == nullptr && CVarGet(CVAR_LEGACY_NAME) != nullptr) {
        CVarSetInteger(CVAR_NAME, CVarGetInteger(CVAR_LEGACY_NAME, 0));
    }

    if (CVarGet(CVAR_LEGACY_NAME) != nullptr) {
        CVarClear(CVAR_LEGACY_NAME);
    }
}

static void ResetTargetSwitchState() {
    sSwitchCooldown = 0;
    sStickReleased = true;
    sPendingSwitchDirection = SWITCH_DIRECTION_NONE;
    sPendingFlickFrames = 0;
}

static bool IsActorTargetable(PlayState* play, Player* player, Actor* actor) {
    if (actor == NULL || actor->update == NULL) {
        return false;
    }
    if (actor == &player->actor) {
        return false;
    }
    if (!(actor->flags & ACTOR_FLAG_ATTENTION_ENABLED)) {
        return false;
    }
    if (actor->flags & ACTOR_FLAG_LOCK_ON_DISABLED) {
        return false;
    }

    // Must be within the actor's attention range
    if (actor->xyzDistToPlayerSq >= gAttentionRanges[actor->attentionRangeType].attentionRangeSq) {
        return false;
    }

    // Line-of-sight check: make sure there is no solid wall between player and actor
    CollisionPoly* poly;
    s32 bgId;
    Vec3f lineTestResultPos;

    if (BgCheck_CameraLineTest1(&play->colCtx, &player->actor.focus.pos, &actor->focus.pos, &lineTestResultPos, &poly,
                                true, true, true, true, &bgId)) {
        if (!SurfaceType_IsIgnoredByProjectiles(&play->colCtx, poly, bgId)) {
            return false;
        }
    }

    return true;
}

static Actor* FindTargetActor(PlayState* play, Player* player, s32 switchDirection) {
    Camera* cam = GET_ACTIVE_CAM(play);
    if (cam == NULL) {
        return NULL;
    }

    // Directional switching selects the nearest target on the chosen side of the current target,
    // then wraps to the furthest target on the opposite side if needed.
    // Left shoulder switching is non-directional, so it picks whichever adjacent target is closest on screen.
    // Use camera yaw to determine screen-space left/right.
    s16 cameraYaw = Math_Vec3f_Yaw(&cam->eye, &cam->at);

    // Current target angle relative to camera.
    s16 currentYaw = Math_Vec3f_Yaw(&player->actor.world.pos, &player->focusActor->focus.pos);
    s16 currentRel = (s16)(currentYaw - cameraYaw);

    Actor* bestActor = NULL;
    s16 bestDiff = 0;
    bool foundDirect = false;
    s32 bestAbsDiff = 0;

    // Wrap-around candidate: furthest target in the opposite direction.
    Actor* wrapActor = NULL;
    s16 wrapDiff = 0;
    bool foundWrap = false;

    for (s32 cat = 0; cat < ACTORCAT_MAX; cat++) {
        for (Actor* actor = play->actorCtx.actorLists[cat].first; actor != NULL; actor = actor->next) {
            if (actor == player->focusActor) {
                continue;
            }
            if (!IsActorTargetable(play, player, actor)) {
                continue;
            }

            s16 actorYaw = Math_Vec3f_Yaw(&player->actor.world.pos, &actor->focus.pos);
            s16 relYaw = (s16)(actorYaw - cameraYaw);
            s16 yawDiff = (s16)(relYaw - currentRel);
            s32 absYawDiff = std::abs(yawDiff);

            if (switchDirection == SWITCH_DIRECTION_NEAREST) {
                if ((absYawDiff != 0) && (!foundDirect || (absYawDiff < bestAbsDiff))) {
                    bestActor = actor;
                    bestAbsDiff = absYawDiff;
                    foundDirect = true;
                }
                continue;
            }

            if (switchDirection == SWITCH_DIRECTION_RIGHT) {
                if (yawDiff > 0) {
                    if (!foundDirect || (yawDiff < bestDiff)) {
                        bestActor = actor;
                        bestDiff = yawDiff;
                        foundDirect = true;
                    }
                } else if (yawDiff < 0) {
                    // Track the furthest-left target for wrap-around.
                    if (!foundWrap || (yawDiff < wrapDiff)) {
                        wrapActor = actor;
                        wrapDiff = yawDiff;
                        foundWrap = true;
                    }
                }
            } else if (switchDirection == SWITCH_DIRECTION_LEFT) {
                if (yawDiff < 0) {
                    if (!foundDirect || (yawDiff > bestDiff)) {
                        bestActor = actor;
                        bestDiff = yawDiff;
                        foundDirect = true;
                    }
                } else if (yawDiff > 0) {
                    // Track the furthest-right target for wrap-around.
                    if (!foundWrap || (yawDiff > wrapDiff)) {
                        wrapActor = actor;
                        wrapDiff = yawDiff;
                        foundWrap = true;
                    }
                }
            }
        }
    }

    // Fall back to wrap-around if no direct candidate.
    if (!foundDirect && foundWrap) {
        bestActor = wrapActor;
    }

    return bestActor;
}

static void SwitchTarget(Player* player, Actor* bestActor) {
    // Replicate the vanilla target-switch logic from Player_UpdateZTargeting:
    // Clear refindable so the new target is treated as a fresh lock-on.
    bestActor->flags &= ~ACTOR_FLAG_FOCUS_ACTOR_REFINDABLE;
    player->focusActor = bestActor;
    // Start at 15 so the reticle has time to settle onto the new target.
    // Vanilla ignores leash distance until the timer counts down to 5.
    player->zTargetActiveTimer = 15;
    player->stateFlags2 &= ~(PLAYER_STATE2_CAN_ACCEPT_TALK_OFFER | PLAYER_STATE2_200000);
    sSwitchCooldown = SWITCH_COOLDOWN;
}

static s32 GetRightStickSwitchDirection(Input* input, bool canTrigger) {
    s32 rightStickX = input->cur.right_stick_x;
    s32 rightStickY = input->cur.right_stick_y;
    s32 absRightStickX = std::abs(rightStickX);
    s32 absRightStickY = std::abs(rightStickY);
    bool stickInNeutral = (absRightStickX < STICK_RELEASE_THRESHOLD) && (absRightStickY < STICK_RELEASE_THRESHOLD);

    if (stickInNeutral) {
        s32 completedDirection = sPendingSwitchDirection;
        bool validFlick = canTrigger && (sPendingFlickFrames > 0) && (sPendingFlickFrames <= STICK_FLICK_MAX_FRAMES);

        sStickReleased = true;
        sPendingSwitchDirection = SWITCH_DIRECTION_NONE;
        sPendingFlickFrames = 0;

        if (validFlick) {
            return completedDirection;
        }
        return SWITCH_DIRECTION_NONE;
    }

    if (sPendingSwitchDirection != SWITCH_DIRECTION_NONE) {
        sPendingFlickFrames++;
        return SWITCH_DIRECTION_NONE;
    }

    if (!canTrigger || !sStickReleased) {
        return SWITCH_DIRECTION_NONE;
    }

    if ((absRightStickX >= STICK_FLICK_THRESHOLD) && (absRightStickX >= (absRightStickY + STICK_HORIZONTAL_MARGIN))) {
        if (rightStickX > 0) {
            sStickReleased = false;
            sPendingSwitchDirection = SWITCH_DIRECTION_RIGHT;
            sPendingFlickFrames = 1;
        } else if (rightStickX < 0) {
            sStickReleased = false;
            sPendingSwitchDirection = SWITCH_DIRECTION_LEFT;
            sPendingFlickFrames = 1;
        }
    }

    return SWITCH_DIRECTION_NONE;
}

void RegisterModernZTargeting() {
    MigrateLegacyCVar();

    COND_HOOK(OnGameStateUpdate, (CVAR && (CVAR_RSTICK || CVAR_LSHOULDER)), []() {
        if (gPlayState == nullptr) {
            return;
        }

        PlayState* play = gPlayState;
        Player* player = GET_PLAYER(play);

        // Not active during cutscenes or special states
        if ((play->csCtx.state != CS_STATE_IDLE) || (player->csAction != PLAYER_CSACTION_NONE) ||
            (player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_20000000))) {
            ResetTargetSwitchState();
            return;
        }

        // Only active when the player has a lock-on target
        if (player->focusActor == NULL) {
            ResetTargetSwitchState();
            return;
        }

        if (sSwitchCooldown > 0) {
            sSwitchCooldown--;
        }

        s32 switchDirection = SWITCH_DIRECTION_NONE;

        if (CVAR_RSTICK) {
            switchDirection = GetRightStickSwitchDirection(&play->state.input[0], sSwitchCooldown == 0);
        }

        if ((switchDirection == SWITCH_DIRECTION_NONE) && CVAR_LSHOULDER &&
            CHECK_BTN_ALL(play->state.input[0].press.button, BTN_L) && (sSwitchCooldown == 0)) {
            switchDirection = SWITCH_DIRECTION_NEAREST;
        }

        if (switchDirection == SWITCH_DIRECTION_NONE) {
            return;
        }

        Actor* bestActor = FindTargetActor(play, player, switchDirection);
        if (bestActor != NULL) {
            SwitchTarget(player, bestActor);
        }
    });
}

static RegisterShipInitFunc initFunc(RegisterModernZTargeting,
                                     { CVAR_NAME, CVAR_LEGACY_NAME, CVAR_CAMERA_NAME, CVAR_RSTICK_NAME,
                                       CVAR_LSHOULDER_NAME, CVAR_ZTOGGLE_NAME });
