#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

#define CVAR_NAME "gEnhancements.Player.HookshotSpeed"
#define CVAR CVarGetInteger(CVAR_NAME, 1)

void RegisterHookshotSpeed() {
    COND_VB_SHOULD(VB_SET_HOOKSHOT_SPEED, CVAR > 1, {
        f32* speedMultiplier = va_arg(args, f32*);
        *speedMultiplier *= CVAR;
    });
}

static RegisterShipInitFunc initFunc(RegisterHookshotSpeed, { CVAR_NAME });
