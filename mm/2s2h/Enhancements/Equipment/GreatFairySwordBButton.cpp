#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/BenPort.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "variables.h"
extern Input* sPlayerControlInput;
}

#define CVAR_NAME "gEnhancements.Equipment.GreatFairySwordBButton"
#define CVAR CVarGetInteger(CVAR_NAME, 0)
#define CVAR_M1_NAME "gEnhancements.Equipment.GreatFairySwordBButtonM1"
#define CVAR_M1 CVarGetInteger(CVAR_M1_NAME, 0)

void RegisterGreatFairySwordBButton() {
    COND_VB_SHOULD(VB_GET_ITEM_ON_BUTTON, CVAR || CVAR_M1, {
        Player* player = GET_PLAYER(gPlayState);
        EquipSlot slot = (EquipSlot)va_arg(args, int);
        ItemId* item = va_arg(args, ItemId*);

        if (slot == EQUIP_SLOT_B && player->transformation == PLAYER_FORM_HUMAN) {
            if (CVAR_M1 && sPlayerControlInput != nullptr && INV_CONTENT(ITEM_SWORD_GREAT_FAIRY) == ITEM_SWORD_GREAT_FAIRY &&
                CHECK_BTN_ALL(sPlayerControlInput->cur.button, BTN_CUSTOM_MODIFIER1)) {
                *item = ITEM_SWORD_GREAT_FAIRY;
            } else if (CVAR && player->heldItemId == ITEM_SWORD_GREAT_FAIRY) {
                *item = ITEM_SWORD_GREAT_FAIRY;
            }
        }
    });
}

static RegisterShipInitFunc initFunc(RegisterGreatFairySwordBButton, { CVAR_NAME, CVAR_M1_NAME });
