#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "variables.h"
}

#define CVAR_NAME "gEnhancements.Player.ModernZTargeting"
#define CVAR_RSTICK_NAME "gEnhancements.Player.ModernZTargeting.RightStickSwitch"
#define CVAR CVarGetInteger(CVAR_NAME, 0)
#define CVAR_RSTICK CVarGetInteger(CVAR_RSTICK_NAME, 1)

// Threshold for right stick X to trigger a target switch (raw stick range is roughly -128..127)
#define STICK_THRESHOLD 20
// Stick must return below this before another switch is allowed
#define STICK_RELEASE_THRESHOLD 10
// Minimum frames between switches as a safety net
#define SWITCH_COOLDOWN 8

static s32 sSwitchCooldown = 0;
static bool sStickReleased = true;

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

void RegisterModernZTargeting() {
    COND_HOOK(OnGameStateUpdate, (CVAR && CVAR_RSTICK), []() {
        if (gPlayState == nullptr) {
            return;
        }

        PlayState* play = gPlayState;
        Player* player = GET_PLAYER(play);

        // Not active during cutscenes or special states
        if ((play->csCtx.state != CS_STATE_IDLE) || (player->csAction != PLAYER_CSACTION_NONE) ||
            (player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_20000000))) {
            sSwitchCooldown = 0;
            sStickReleased = true;
            return;
        }

        // Only active when the player has a lock-on target
        if (player->focusActor == NULL) {
            sSwitchCooldown = 0;
            sStickReleased = true;
            return;
        }

        s32 rightStickX = play->state.input[0].cur.right_stick_x;

        if (sSwitchCooldown > 0) {
            sSwitchCooldown--;
        }

        // Require stick to return to neutral before another switch
        if (rightStickX > -STICK_RELEASE_THRESHOLD && rightStickX < STICK_RELEASE_THRESHOLD) {
            sStickReleased = true;
        }

        if (!sStickReleased || sSwitchCooldown > 0 ||
            (rightStickX > -STICK_THRESHOLD && rightStickX < STICK_THRESHOLD)) {
            return;
        }

        bool switchRight = rightStickX > 0;

        // Use camera yaw to determine screen-space left/right
        Camera* cam = play->cameraPtrs[play->activeCamId];
        s16 cameraYaw = Math_Vec3f_Yaw(&cam->eye, &cam->at);

        // Current target angle relative to camera
        s16 currentYaw = Math_Vec3f_Yaw(&player->actor.world.pos, &player->focusActor->focus.pos);
        s16 currentRel = (s16)(currentYaw - cameraYaw);

        Actor* bestActor = NULL;
        s16 bestDiff = 0;
        bool foundDirect = false;

        // Wrap-around candidate: furthest target in the opposite direction
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

                if (switchRight) {
                    if (yawDiff > 0) {
                        if (!foundDirect || yawDiff < bestDiff) {
                            bestActor = actor;
                            bestDiff = yawDiff;
                            foundDirect = true;
                        }
                    } else if (yawDiff < 0) {
                        // Track the furthest-left target for wrap-around
                        if (!foundWrap || yawDiff < wrapDiff) {
                            wrapActor = actor;
                            wrapDiff = yawDiff;
                            foundWrap = true;
                        }
                    }
                } else {
                    if (yawDiff < 0) {
                        if (!foundDirect || yawDiff > bestDiff) {
                            bestActor = actor;
                            bestDiff = yawDiff;
                            foundDirect = true;
                        }
                    } else if (yawDiff > 0) {
                        // Track the furthest-right target for wrap-around
                        if (!foundWrap || yawDiff > wrapDiff) {
                            wrapActor = actor;
                            wrapDiff = yawDiff;
                            foundWrap = true;
                        }
                    }
                }
            }
        }

        // Fall back to wrap-around if no direct candidate
        if (!foundDirect && foundWrap) {
            bestActor = wrapActor;
        }

        if (bestActor != NULL) {
            // Replicate the vanilla target-switch logic from Player_UpdateZTargeting:
            // Clear refindable so the new target is treated as a fresh lock-on.
            bestActor->flags &= ~ACTOR_FLAG_FOCUS_ACTOR_REFINDABLE;
            player->focusActor = bestActor;
            // 15 frames gives the reticle time to settle onto the new target
            // (vanilla counts down from 15 to 5, ignoring leash distance during that window).
            player->zTargetActiveTimer = 15;
            player->stateFlags2 &= ~(PLAYER_STATE2_CAN_ACCEPT_TALK_OFFER | PLAYER_STATE2_200000);

            sStickReleased = false;
            sSwitchCooldown = SWITCH_COOLDOWN;
        }
    });
}

static RegisterShipInitFunc initFunc(RegisterModernZTargeting, { CVAR_NAME, CVAR_RSTICK_NAME });
