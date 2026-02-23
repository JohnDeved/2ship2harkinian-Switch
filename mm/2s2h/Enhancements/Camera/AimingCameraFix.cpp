#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "variables.h"
}

#define CVAR_NAME "gEnhancements.Camera.AimingFirstPersonCamera"

// Aim camera modes that use Camera_Subject1 in a non-Z-targeting context.
// When the player draws their bow/slingshot without a lock-on, the camera
// snaps to the player's body facing direction. This enhancement makes the
// aiming camera instead start from the current camera look direction.
static bool IsAimCameraMode(s16 mode) {
    return (mode == CAM_MODE_SLINGSHOT) || (mode == CAM_MODE_BOWARROW) || (mode == CAM_MODE_DEKUSHOOT);
}

void RegisterAimingCameraFix() {
    COND_HOOK(OnCameraChangeModeFlags, CVarGetInteger(CVAR_NAME, 0), [](Camera* camera) {
        if (!IsAimCameraMode(camera->mode)) {
            return;
        }

        if (gPlayState == nullptr || camera != Play_GetCamera(gPlayState, CAM_ID_MAIN)) {
            return;
        }

        Actor* focalActor = camera->focalActor;
        if (focalActor == nullptr) {
            return;
        }

        // Align the focal actor's focus rotation with the current camera look
        // direction. Camera_Subject1 reads focalActor->focus.rot.y and computes
        // sp7C.yaw = BINANG_ROT180(focus.rot.y) as the target eye yaw. Setting
        // focus.rot.y = Math_Vec3f_Yaw(eye, at) makes sp7C.yaw equal to the
        // current camera yaw (from at to eye), so no snapping transition occurs.
        s16 cameraYaw = Math_Vec3f_Yaw(&camera->eye, &camera->at);
        focalActor->focus.rot.y = cameraYaw;
        // Also update world.rot.y so that the clamp in Ship_HandleFirstPersonAiming
        // (focus.rot.y is clamped to ±0x4AAA relative to shape.rot.y) does not
        // immediately force the aim back toward the original body direction.
        focalActor->world.rot.y = cameraYaw;
        focalActor->shape.rot.y = cameraYaw;
    });
}

static RegisterShipInitFunc initFunc(RegisterAimingCameraFix, { CVAR_NAME });
