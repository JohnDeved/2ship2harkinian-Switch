#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "variables.h"
}

#define CVAR_NAME "gEnhancements.Camera.AimingFirstPersonCamera"

void RegisterAimingCameraFix() {
    // When the camera transitions into an aiming mode (bow/slingshot/Deku nuts),
    // align the focal actor's rotation to the current camera look direction so
    // Camera_Subject1 starts from where the player was already looking instead
    // of snapping to the player's body facing direction.
    COND_HOOK(OnCameraChangeModeFlags, CVarGetInteger(CVAR_NAME, 0), [](Camera* camera) {
        if (camera->mode != CAM_MODE_SLINGSHOT && camera->mode != CAM_MODE_BOWARROW &&
            camera->mode != CAM_MODE_DEKUSHOOT) {
            return;
        }

        if (gPlayState == nullptr || camera != Play_GetCamera(gPlayState, CAM_ID_MAIN)) {
            return;
        }

        Actor* focalActor = camera->focalActor;
        if (focalActor == nullptr) {
            return;
        }

        // Set focus/shape/world yaw to the current camera look direction.
        // Camera_Subject1 uses BINANG_ROT180(focus.rot.y) as its target eye yaw,
        // so matching it to the current camera yaw eliminates the snap transition.
        // shape.rot.y must also match so the aim clamp in Ship_HandleFirstPersonAiming
        // stays centred on the camera direction rather than the old body facing.
        s16 cameraYaw = Math_Vec3f_Yaw(&camera->eye, &camera->at);
        focalActor->focus.rot.y = cameraYaw;
        focalActor->shape.rot.y = cameraYaw;
        focalActor->world.rot.y = cameraYaw;
    });
}

static RegisterShipInitFunc initFunc(RegisterAimingCameraFix, { CVAR_NAME });
