#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"
#include "QuickBar.h"

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "z64ocarina.h"
#include "z64save.h"
void Player_UseItem(PlayState* play, Player* thisx, ItemId item);
void Interface_LoadItemIconImpl(PlayState* play, u8 btn);
void Interface_Dpad_LoadItemIconImpl(PlayState* play, u8 btn);
OcarinaStaff* AudioOcarina_GetPlayingStaff(void);
}

#define CVAR_NAME "gEnhancements.Equipment.QuickBar"
#define CVAR CVarGetInteger(CVAR_NAME, 0)

// How many frames a D-pad button must be held before the QuickBar opens
static const int HOLD_THRESHOLD = 12;

// Stick threshold for QuickBar navigation
static const int STICK_THRESHOLD = 40;
static const int STICK_RELEASE_THRESHOLD = 20;

static QuickBarState sState = {};
static bool sStickReleased = true;

// Saved right stick values (captured in PreMain before zeroing for camera)
static s8 sSavedRightStickX = 0;
static s8 sSavedRightStickY = 0;

// Deferred song recognition (needs frames for the ocarina state machine to initialize)
struct DeferredSong {
    bool pending;
    int songIndex;    // OCARINA_SONG_* index to force-recognize
    int delayFrames;  // Frames to wait before executing
};
static DeferredSong sDeferredSong = {};

QuickBarState& GetQuickBarState() {
    return sState;
}

bool IsQuickBarEnabled() {
    return CVAR != 0 && !CVarGetInteger("gEnhancements.Dpad.DpadEquips", 0);
}

// ─── Item name tables ───────────────────────────────────────────────────────

static const char* GetItemName(int itemId) {
    switch (itemId) {
        case ITEM_BOW:               return "Hero's Bow";
        case ITEM_ARROW_FIRE:        return "Fire Arrows";
        case ITEM_ARROW_ICE:         return "Ice Arrows";
        case ITEM_ARROW_LIGHT:       return "Light Arrows";
        case ITEM_HOOKSHOT:          return "Hookshot";
        case ITEM_BOMB:              return "Bombs";
        case ITEM_BOMBCHU:           return "Bombchus";
        case ITEM_DEKU_STICK:        return "Deku Stick";
        case ITEM_DEKU_NUT:          return "Deku Nuts";
        case ITEM_MAGIC_BEANS:       return "Magic Beans";
        case ITEM_POWDER_KEG:        return "Powder Keg";
        case ITEM_LENS_OF_TRUTH:     return "Lens of Truth";
        case ITEM_PICTOGRAPH_BOX:    return "Pictograph Box";
        case ITEM_SWORD_GREAT_FAIRY: return "Great Fairy's Sword";

        case ITEM_MASK_DEKU:          return "Deku Mask";
        case ITEM_MASK_GORON:         return "Goron Mask";
        case ITEM_MASK_ZORA:          return "Zora Mask";
        case ITEM_MASK_FIERCE_DEITY:  return "Fierce Deity's Mask";
        case ITEM_MASK_TRUTH:         return "Mask of Truth";
        case ITEM_MASK_KAFEIS_MASK:   return "Kafei's Mask";
        case ITEM_MASK_ALL_NIGHT:     return "All-Night Mask";
        case ITEM_MASK_BUNNY:         return "Bunny Hood";
        case ITEM_MASK_KEATON:        return "Keaton Mask";
        case ITEM_MASK_GARO:          return "Garo's Mask";
        case ITEM_MASK_ROMANI:        return "Romani's Mask";
        case ITEM_MASK_CIRCUS_LEADER: return "Circus Leader's Mask";
        case ITEM_MASK_POSTMAN:       return "Postman's Hat";
        case ITEM_MASK_COUPLE:        return "Couple's Mask";
        case ITEM_MASK_GREAT_FAIRY:   return "Great Fairy's Mask";
        case ITEM_MASK_GIBDO:         return "Gibdo Mask";
        case ITEM_MASK_DON_GERO:      return "Don Gero's Mask";
        case ITEM_MASK_KAMARO:        return "Kamaro's Mask";
        case ITEM_MASK_CAPTAIN:       return "Captain's Hat";
        case ITEM_MASK_STONE:         return "Stone Mask";
        case ITEM_MASK_BREMEN:        return "Bremen Mask";
        case ITEM_MASK_BLAST:         return "Blast Mask";
        case ITEM_MASK_SCENTS:        return "Mask of Scents";
        case ITEM_MASK_GIANT:         return "Giant's Mask";

        case ITEM_BOTTLE:             return "Empty Bottle";
        case ITEM_POTION_RED:         return "Red Potion";
        case ITEM_POTION_GREEN:       return "Green Potion";
        case ITEM_POTION_BLUE:        return "Blue Potion";
        case ITEM_FAIRY:              return "Fairy";
        case ITEM_DEKU_PRINCESS:      return "Deku Princess";
        case ITEM_MILK_BOTTLE:        return "Milk";
        case ITEM_MILK_HALF:          return "Milk (Half)";
        case ITEM_FISH:               return "Fish";
        case ITEM_BUG:                return "Bug";
        case ITEM_POE:                return "Poe Soul";
        case ITEM_BIG_POE:            return "Big Poe Soul";
        case ITEM_SPRING_WATER:       return "Spring Water";
        case ITEM_HOT_SPRING_WATER:   return "Hot Spring Water";
        case ITEM_ZORA_EGG:           return "Zora Egg";
        case ITEM_GOLD_DUST:          return "Gold Dust";
        case ITEM_SEAHORSE:           return "Seahorse";
        case ITEM_CHATEAU:            return "Chateau Romani";
        case ITEM_MUSHROOM:           return "Mystery Milk";
        case ITEM_BLUE_FIRE:          return "Blue Fire";
        case ITEM_HYLIAN_LOACH:       return "Hylian Loach";
        case ITEM_OBABA_DRINK:        return "Obaba's Drink";
        default:                      return "???";
    }
}

const char* QuickBar_GetItemName(int itemId) {
    return GetItemName(itemId);
}

// Song names by quest bit index
static const char* GetSongName(int questBit) {
    switch (questBit) {
        case QUEST_SONG_TIME:          return "Song of Time";
        case QUEST_SONG_HEALING:       return "Song of Healing";
        case QUEST_SONG_SOARING:       return "Song of Soaring";
        case QUEST_SONG_SONATA:        return "Sonata of Awakening";
        case QUEST_SONG_LULLABY:       return "Goron Lullaby";
        case QUEST_SONG_BOSSA_NOVA:    return "New Wave Bossa Nova";
        case QUEST_SONG_ELEGY:         return "Elegy of Emptiness";
        case QUEST_SONG_OATH:          return "Oath to Order";
        case QUEST_SONG_STORMS:        return "Song of Storms";
        case QUEST_SONG_EPONA:         return "Epona's Song";
        case QUEST_SONG_SUN:           return "Song of Double Time";
        case QUEST_SONG_SARIA:         return "Inverted Song of Time";
        case QUEST_SONG_LULLABY_INTRO: return "Goron Lullaby (Intro)";
        default:                       return "???";
    }
}

// ─── Build item lists (owned only) ─────────────────────────────────────────

// Tools the player can select as Active Tool
static const int sToolItems[] = {
    ITEM_BOW, ITEM_ARROW_FIRE, ITEM_ARROW_ICE, ITEM_ARROW_LIGHT,
    ITEM_HOOKSHOT, ITEM_BOMB, ITEM_BOMBCHU, ITEM_DEKU_STICK,
    ITEM_DEKU_NUT, ITEM_MAGIC_BEANS, ITEM_POWDER_KEG,
    ITEM_LENS_OF_TRUTH, ITEM_PICTOGRAPH_BOX, ITEM_SWORD_GREAT_FAIRY
};

// All masks (transformation masks first)
static const int sMaskItems[] = {
    ITEM_MASK_DEKU, ITEM_MASK_GORON, ITEM_MASK_ZORA, ITEM_MASK_FIERCE_DEITY,
    ITEM_MASK_GREAT_FAIRY, ITEM_MASK_KAFEIS_MASK, ITEM_MASK_BREMEN,
    ITEM_MASK_KAMARO, ITEM_MASK_BLAST, ITEM_MASK_BUNNY, ITEM_MASK_KEATON,
    ITEM_MASK_POSTMAN, ITEM_MASK_TRUTH, ITEM_MASK_SCENTS,
    ITEM_MASK_DON_GERO, ITEM_MASK_ROMANI, ITEM_MASK_GARO,
    ITEM_MASK_CAPTAIN, ITEM_MASK_STONE, ITEM_MASK_CIRCUS_LEADER,
    ITEM_MASK_ALL_NIGHT, ITEM_MASK_GIBDO, ITEM_MASK_COUPLE, ITEM_MASK_GIANT
};

// Song quest bits for songs the player can learn
static const int sSongQuests[] = {
    QUEST_SONG_TIME, QUEST_SONG_HEALING, QUEST_SONG_SOARING,
    QUEST_SONG_SONATA, QUEST_SONG_LULLABY, QUEST_SONG_BOSSA_NOVA,
    QUEST_SONG_ELEGY, QUEST_SONG_OATH, QUEST_SONG_STORMS,
    QUEST_SONG_SARIA,   // Inverted Song of Time
    QUEST_SONG_SUN,     // Song of Double Time
    QUEST_SONG_EPONA
};

static int GetSlotForItem(int itemId) {
    switch (itemId) {
        case ITEM_BOW: return SLOT_BOW;
        case ITEM_ARROW_FIRE: return SLOT_ARROW_FIRE;
        case ITEM_ARROW_ICE: return SLOT_ARROW_ICE;
        case ITEM_ARROW_LIGHT: return SLOT_ARROW_LIGHT;
        case ITEM_HOOKSHOT: return SLOT_HOOKSHOT;
        case ITEM_BOMB: return SLOT_BOMB;
        case ITEM_BOMBCHU: return SLOT_BOMBCHU;
        case ITEM_DEKU_STICK: return SLOT_DEKU_STICK;
        case ITEM_DEKU_NUT: return SLOT_DEKU_NUT;
        case ITEM_MAGIC_BEANS: return SLOT_MAGIC_BEANS;
        case ITEM_POWDER_KEG: return SLOT_POWDER_KEG;
        case ITEM_LENS_OF_TRUTH: return SLOT_LENS_OF_TRUTH;
        case ITEM_PICTOGRAPH_BOX: return SLOT_PICTOGRAPH_BOX;
        case ITEM_SWORD_GREAT_FAIRY: return SLOT_SWORD_GREAT_FAIRY;
        default: return SLOT_NONE;
    }
}

static int GetSlotForAnyItem(int itemId) {
    // Tools
    int slot = GetSlotForItem(itemId);
    if (slot != SLOT_NONE) return slot;
    // Ocarina
    if (itemId == ITEM_OCARINA_OF_TIME) return SLOT_OCARINA;
    // Masks
    if (itemId >= ITEM_MASK_DEKU && itemId <= ITEM_MASK_GIANT) {
        for (int s = SLOT_MASK_POSTMAN; s <= SLOT_MASK_FIERCE_DEITY; s++) {
            if (gSaveContext.save.saveInfo.inventory.items[s] == (u8)itemId) return s;
        }
    }
    // Bottles
    for (int s = SLOT_BOTTLE_1; s <= SLOT_BOTTLE_6; s++) {
        if (gSaveContext.save.saveInfo.inventory.items[s] == (u8)itemId) return s;
    }
    return SLOT_NONE;
}

// Sync DPAD equip slots with QuickBar last-used items so the native
// Dpad HUD rendering shows QuickBar items instead of DpadEquips items
static void SyncDpadFromQuickBar() {
    if (gPlayState == nullptr) return;
    struct { QuickBarCategory cat; int dpadSlot; int defaultItem; } mapping[4] = {
        { QB_CAT_MASKS,   EQUIP_SLOT_D_UP,    -1 },
        { QB_CAT_TOOLS,   EQUIP_SLOT_D_RIGHT,  -1 },
        { QB_CAT_BOTTLES, EQUIP_SLOT_D_DOWN,   -1 },
        { QB_CAT_SONGS,   EQUIP_SLOT_D_LEFT,   ITEM_OCARINA_OF_TIME },
    };
    for (int i = 0; i < 4; i++) {
        int itemId = mapping[i].defaultItem >= 0 ? mapping[i].defaultItem : sState.lastUsed[mapping[i].cat];
        int dSlot = mapping[i].dpadSlot;
        if (itemId >= 0) {
            DPAD_SET_CUR_FORM_BTN_ITEM(dSlot, itemId);
            int invSlot = GetSlotForAnyItem(itemId);
            if (invSlot != SLOT_NONE) {
                DPAD_SET_CUR_FORM_BTN_SLOT(dSlot, invSlot);
            }
            gSaveContext.shipSaveContext.dpad.status[dSlot] = BTN_ENABLED;
            Interface_Dpad_LoadItemIconImpl(gPlayState, dSlot);
        } else {
            DPAD_SET_CUR_FORM_BTN_ITEM(dSlot, ITEM_NONE);
            gSaveContext.shipSaveContext.dpad.status[dSlot] = BTN_DISABLED;
        }
    }
}

static bool PlayerHasItem(int itemId) {
    int slot = GetSlotForItem(itemId);
    if (slot != SLOT_NONE) {
        return gSaveContext.save.saveInfo.inventory.items[slot] == (u8)itemId;
    }
    // Masks
    if (itemId >= ITEM_MASK_DEKU && itemId <= ITEM_MASK_GIANT) {
        for (int s = SLOT_MASK_POSTMAN; s <= SLOT_MASK_FIERCE_DEITY; s++) {
            if (gSaveContext.save.saveInfo.inventory.items[s] == (u8)itemId) {
                return true;
            }
        }
        return false;
    }
    return false;
}

static void BuildToolList(std::vector<int>& items, std::vector<const char*>& names) {
    items.clear();
    names.clear();
    for (int i = 0; i < (int)ARRAY_COUNT(sToolItems); i++) {
        int item = sToolItems[i];
        if (PlayerHasItem(item)) {
            items.push_back(item);
            names.push_back(GetItemName(item));
        }
    }
}

static void BuildMaskList(std::vector<int>& items, std::vector<const char*>& names) {
    items.clear();
    names.clear();
    for (int i = 0; i < (int)ARRAY_COUNT(sMaskItems); i++) {
        int item = sMaskItems[i];
        if (PlayerHasItem(item)) {
            items.push_back(item);
            names.push_back(GetItemName(item));
        }
    }
}

static void BuildSongList(std::vector<int>& items, std::vector<const char*>& names) {
    items.clear();
    names.clear();
    for (int i = 0; i < (int)ARRAY_COUNT(sSongQuests); i++) {
        int q = sSongQuests[i];
        if (CHECK_QUEST_ITEM(q)) {
            items.push_back(q);
            names.push_back(GetSongName(q));
        }
    }
    // Include intro version only if player hasn't learned the full Goron Lullaby
    if (CHECK_QUEST_ITEM(QUEST_SONG_LULLABY_INTRO) && !CHECK_QUEST_ITEM(QUEST_SONG_LULLABY)) {
        items.push_back(QUEST_SONG_LULLABY_INTRO);
        names.push_back(GetSongName(QUEST_SONG_LULLABY_INTRO));
    }
}

static void BuildBottleList(std::vector<int>& items, std::vector<const char*>& names) {
    items.clear();
    names.clear();
    for (int s = SLOT_BOTTLE_1; s <= SLOT_BOTTLE_6; s++) {
        u8 content = gSaveContext.save.saveInfo.inventory.items[s];
        if (content != ITEM_NONE) {
            items.push_back(content);
            names.push_back(GetItemName(content));
        }
    }
}

static void BuildCategoryList(QuickBarCategory cat) {
    switch (cat) {
        case QB_CAT_TOOLS:
            BuildToolList(sState.currentItems, sState.currentNames);
            break;
        case QB_CAT_MASKS:
            BuildMaskList(sState.currentItems, sState.currentNames);
            break;
        case QB_CAT_SONGS:
            BuildSongList(sState.currentItems, sState.currentNames);
            break;
        case QB_CAT_BOTTLES:
            BuildBottleList(sState.currentItems, sState.currentNames);
            break;
        default:
            break;
    }
}

// ─── Actions ────────────────────────────────────────────────────────────────

static void SetActiveTool(int itemId) {
    sState.activeToolItem = itemId;
}

static void UseItemOnPlayer(int itemId) {
    if (gPlayState == nullptr) return;
    Player* player = GET_PLAYER(gPlayState);
    // Set heldItemButton so the game knows which "button" this came from.
    // Required for non-mask items to work properly through Player_UseItem.
    player->heldItemButton = EQUIP_SLOT_C_LEFT;
    Player_UseItem(gPlayState, player, (ItemId)itemId);
}

// Map song quest bit to OcarinaSongId for playback
static int QuestBitToSongId(int questBit) {
    switch (questBit) {
        case QUEST_SONG_SONATA:        return OCARINA_SONG_SONATA;
        case QUEST_SONG_LULLABY:       return OCARINA_SONG_GORON_LULLABY;
        case QUEST_SONG_BOSSA_NOVA:    return OCARINA_SONG_NEW_WAVE;
        case QUEST_SONG_ELEGY:         return OCARINA_SONG_ELEGY;
        case QUEST_SONG_OATH:          return OCARINA_SONG_OATH;
        case QUEST_SONG_SARIA:         return OCARINA_SONG_SARIAS;
        case QUEST_SONG_TIME:          return OCARINA_SONG_TIME;
        case QUEST_SONG_HEALING:       return OCARINA_SONG_HEALING;
        case QUEST_SONG_EPONA:         return OCARINA_SONG_EPONAS;
        case QUEST_SONG_SOARING:       return OCARINA_SONG_SOARING;
        case QUEST_SONG_STORMS:        return OCARINA_SONG_STORMS;
        case QUEST_SONG_SUN:           return OCARINA_SONG_SUNS;
        case QUEST_SONG_LULLABY_INTRO: return OCARINA_SONG_GORON_LULLABY_INTRO;
        default: return -1;
    }
}

// Actor freeze is handled via the ShouldActorUpdate hook in RegisterQuickBar.
// When QuickBar is open, non-player actors are frozen (same visual effect as ocarina).

// ─── Open / close QuickBar ──────────────────────────────────────────────────

static void OpenQuickBar(QuickBarCategory cat) {
    BuildCategoryList(cat);
    if (sState.currentItems.empty()) return;

    sState.isOpen = true;
    sState.openCategory = cat;

    // Try to select the last-used item
    sState.selectionIndex = 0;
    if (sState.lastUsed[cat] >= 0) {
        for (int i = 0; i < (int)sState.currentItems.size(); i++) {
            if (sState.currentItems[i] == sState.lastUsed[cat]) {
                sState.selectionIndex = i;
                break;
            }
        }
    }

    sStickReleased = true;
    Audio_PlaySfx(NA_SE_SY_WIN_OPEN);
}

static void CloseQuickBar(bool confirm) {
    if (!sState.isOpen) return;

    if (confirm && !sState.currentItems.empty()) {
        int selectedItem = sState.currentItems[sState.selectionIndex];
        sState.lastUsed[sState.openCategory] = selectedItem;

        switch (sState.openCategory) {
            case QB_CAT_TOOLS: {
                SetActiveTool(selectedItem);
                // Equip the tool to C-Left button slot so the player can use it
                int slot = GetSlotForItem(selectedItem);
                if (slot != SLOT_NONE) {
                    SET_CUR_FORM_BTN_ITEM(EQUIP_SLOT_C_LEFT, selectedItem);
                    SET_CUR_FORM_BTN_SLOT(EQUIP_SLOT_C_LEFT, slot);
                    Interface_LoadItemIconImpl(gPlayState, EQUIP_SLOT_C_LEFT);
                }
                break;
            }
            case QB_CAT_MASKS: {
                // Equip mask to C-Left and directly toggle the player's currentMask.
                // We set currentMask directly (same as z_player.c:4706-4713) because
                // Player_UseItem has complex state conditions that can fail.
                Player* player = GET_PLAYER(gPlayState);
                int maskSlot = GetSlotForAnyItem(selectedItem);
                if (maskSlot != SLOT_NONE) {
                    SET_CUR_FORM_BTN_ITEM(EQUIP_SLOT_C_LEFT, selectedItem);
                    SET_CUR_FORM_BTN_SLOT(EQUIP_SLOT_C_LEFT, maskSlot);
                    Interface_LoadItemIconImpl(gPlayState, EQUIP_SLOT_C_LEFT);
                }
                // For transform masks (Deku, Goron, Zora, Fierce Deity), use Player_UseItem
                // which triggers the transformation cutscene
                if (selectedItem == ITEM_MASK_DEKU || selectedItem == ITEM_MASK_GORON ||
                    selectedItem == ITEM_MASK_ZORA || selectedItem == ITEM_MASK_FIERCE_DEITY) {
                    UseItemOnPlayer(selectedItem);
                } else {
                    // Non-transform masks: directly toggle currentMask
                    PlayerItemAction itemAction = (PlayerItemAction)(selectedItem - ITEM_MASK_DEKU + PLAYER_IA_MASK_DEKU);
                    PlayerMask maskId = GET_MASK_FROM_IA(itemAction);
                    player->prevMask = player->currentMask;
                    if (maskId == player->currentMask) {
                        player->currentMask = PLAYER_MASK_NONE;
                    } else {
                        player->currentMask = maskId;
                    }
                    gSaveContext.save.equippedMask = player->currentMask;
                }
                break;
            }
            case QB_CAT_SONGS:
                if (CVarGetInteger("gEnhancements.Equipment.QuickBar.AutoPlaySongs", 1)) {
                    // Pull out ocarina, then defer song recognition by 30 frames.
                    // We set the playing staff's state to the song index, which the
                    // message system recognizes as a completed song (same as if the
                    // player entered the notes manually).
                    UseItemOnPlayer(ITEM_OCARINA_OF_TIME);
                    int songIndex = QuestBitToSongId(selectedItem);
                    if (songIndex >= 0) {
                        sDeferredSong = { true, songIndex, 30 };
                    }
                }
                break;
            case QB_CAT_BOTTLES: {
                SetActiveTool(selectedItem);
                // Equip the bottle to C-Left button slot
                // Find the bottle slot containing this item
                for (int s = SLOT_BOTTLE_1; s <= SLOT_BOTTLE_6; s++) {
                    if (gSaveContext.save.saveInfo.inventory.items[s] == (u8)selectedItem) {
                        SET_CUR_FORM_BTN_ITEM(EQUIP_SLOT_C_LEFT, selectedItem);
                        SET_CUR_FORM_BTN_SLOT(EQUIP_SLOT_C_LEFT, s);
                        Interface_LoadItemIconImpl(gPlayState, EQUIP_SLOT_C_LEFT);
                        break;
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    sState.isOpen = false;
    sState.currentItems.clear();
    sState.currentNames.clear();
}

// ─── Tap recall ─────────────────────────────────────────────────────────────

static void TapRecall(QuickBarCategory cat) {
    int lastItem = sState.lastUsed[cat];
    if (lastItem < 0 && cat != QB_CAT_SONGS) return;
    if (gPlayState == nullptr) return;

    switch (cat) {
        case QB_CAT_TOOLS: {
            // Tap RIGHT: equip tool to C-Left so it appears on the button
            SetActiveTool(lastItem);
            int slot = GetSlotForItem(lastItem);
            if (slot != SLOT_NONE) {
                SET_CUR_FORM_BTN_ITEM(EQUIP_SLOT_C_LEFT, lastItem);
                SET_CUR_FORM_BTN_SLOT(EQUIP_SLOT_C_LEFT, slot);
                Interface_LoadItemIconImpl(gPlayState, EQUIP_SLOT_C_LEFT);
            }
            break;
        }
        case QB_CAT_BOTTLES: {
            // Tap DOWN: equip last bottle to C-Left
            for (int s = SLOT_BOTTLE_1; s <= SLOT_BOTTLE_6; s++) {
                if (gSaveContext.save.saveInfo.inventory.items[s] == (u8)lastItem) {
                    SET_CUR_FORM_BTN_ITEM(EQUIP_SLOT_C_LEFT, lastItem);
                    SET_CUR_FORM_BTN_SLOT(EQUIP_SLOT_C_LEFT, s);
                    Interface_LoadItemIconImpl(gPlayState, EQUIP_SLOT_C_LEFT);
                    break;
                }
            }
            break;
        }
        case QB_CAT_MASKS: {
            // Tap UP: toggle last mask
            Player* player = GET_PLAYER(gPlayState);
            int maskSlot = GetSlotForAnyItem(lastItem);
            if (maskSlot != SLOT_NONE) {
                SET_CUR_FORM_BTN_ITEM(EQUIP_SLOT_C_LEFT, lastItem);
                SET_CUR_FORM_BTN_SLOT(EQUIP_SLOT_C_LEFT, maskSlot);
                Interface_LoadItemIconImpl(gPlayState, EQUIP_SLOT_C_LEFT);
            }
            if (lastItem == ITEM_MASK_DEKU || lastItem == ITEM_MASK_GORON ||
                lastItem == ITEM_MASK_ZORA || lastItem == ITEM_MASK_FIERCE_DEITY) {
                UseItemOnPlayer(lastItem);
            } else {
                PlayerItemAction itemAction = (PlayerItemAction)(lastItem - ITEM_MASK_DEKU + PLAYER_IA_MASK_DEKU);
                PlayerMask maskId = GET_MASK_FROM_IA(itemAction);
                player->prevMask = player->currentMask;
                if (maskId == player->currentMask) {
                    player->currentMask = PLAYER_MASK_NONE;
                } else {
                    player->currentMask = maskId;
                }
                gSaveContext.save.equippedMask = player->currentMask;
            }
            break;
        }
        case QB_CAT_SONGS:
            // Tap LEFT: pull out ocarina
            UseItemOnPlayer(ITEM_OCARINA_OF_TIME);
            break;
        default:
            break;
    }
}

// ─── Pre-main hook (consumes input before camera reads it) ──────────────────

static void QuickBarPreMain() {
    if (gPlayState == nullptr) return;

    Input* input = CONTROLLER1(&gPlayState->state);

    if (sState.isOpen) {
        // Save right stick values BEFORE zeroing so QuickBarUpdate can use them
        sSavedRightStickX = input->cur.right_stick_x;
        sSavedRightStickY = input->cur.right_stick_y;

        // Consume right stick BEFORE the camera/FreeLook reads it
        input->cur.right_stick_x = 0;
        input->cur.right_stick_y = 0;

        // Also zero left stick to freeze player movement
        input->cur.stick_x = 0;
        input->cur.stick_y = 0;
        input->rel.stick_x = 0;
        input->rel.stick_y = 0;
    } else {
        sSavedRightStickX = 0;
        sSavedRightStickY = 0;
    }
}

// ─── Per-frame update ───────────────────────────────────────────────────────

static void QuickBarUpdate() {
    if (gPlayState == nullptr) return;

    // Process deferred song recognition
    // After pulling out the ocarina, the message system enters MSGMODE_OCARINA_PLAYING.
    // We set the playing staff's state to the song index, which the message system
    // picks up and processes through the normal song recognition flow (same as if
    // the player entered the correct notes manually).
    if (sDeferredSong.pending) {
        if (sDeferredSong.delayFrames > 0) {
            sDeferredSong.delayFrames--;
        } else {
            sDeferredSong.pending = false;
            OcarinaStaff* staff = AudioOcarina_GetPlayingStaff();
            if (staff != NULL) {
                staff->state = sDeferredSong.songIndex;
            }
        }
    }

    if (Play_InCsMode(gPlayState)) return;

    // Don't process if Dpad Equips is active (both features use D-pad)
    if (CVarGetInteger("gEnhancements.Dpad.DpadEquips", 0)) return;

    Input* input = CONTROLLER1(&gPlayState->state);
    int curButtons = input->cur.button;

    // D-pad directions
    static const int dpadBtns[] = { BTN_DUP, BTN_DRIGHT, BTN_DLEFT, BTN_DDOWN };

    for (int i = 0; i < QB_CAT_COUNT; i++) {
        int btn = dpadBtns[i];
        QuickBarCategory cat = (QuickBarCategory)i;

        if (curButtons & btn) {
            // Button is held
            sState.dpadHoldFrames[i]++;

            if (!sState.dpadWasHeld[i] && sState.dpadHoldFrames[i] >= HOLD_THRESHOLD) {
                // Threshold reached → open QuickBar
                sState.dpadWasHeld[i] = true;
                OpenQuickBar(cat);
            }
        } else if (sState.dpadHoldFrames[i] > 0) {
            // Button was just released
            if (!sState.dpadWasHeld[i]) {
                // Was a tap → recall (no sound effect)
                TapRecall(cat);
            } else if (sState.isOpen && sState.openCategory == cat) {
                // Was a hold → confirm selection
                CloseQuickBar(true);
            }
            sState.dpadHoldFrames[i] = 0;
            sState.dpadWasHeld[i] = false;
        }
    }

    // QuickBar navigation with right stick (using saved values from PreMain)
    if (sState.isOpen && !sState.currentItems.empty()) {
        s32 stickX = sSavedRightStickX;

        if (stickX > -STICK_RELEASE_THRESHOLD && stickX < STICK_RELEASE_THRESHOLD) {
            sStickReleased = true;
        }

        if (sStickReleased) {
            if (stickX > STICK_THRESHOLD) {
                sState.selectionIndex++;
                if (sState.selectionIndex >= (int)sState.currentItems.size()) {
                    sState.selectionIndex = 0;
                }
                sStickReleased = false;
                Audio_PlaySfx(NA_SE_SY_CURSOR);
            } else if (stickX < -STICK_THRESHOLD) {
                sState.selectionIndex--;
                if (sState.selectionIndex < 0) {
                    sState.selectionIndex = (int)sState.currentItems.size() - 1;
                }
                sStickReleased = false;
                Audio_PlaySfx(NA_SE_SY_CURSOR);
            }
        }

        // Consume D-pad input so other systems don't process it
        input->press.button &= ~(BTN_DUP | BTN_DDOWN | BTN_DLEFT | BTN_DRIGHT);
        input->cur.button   &= ~(BTN_DUP | BTN_DDOWN | BTN_DLEFT | BTN_DRIGHT);
        // Right stick already consumed in PreMain
    }

    // Sync DPAD equip slots so native rendering shows QuickBar items
    SyncDpadFromQuickBar();
}

// ─── Registration ───────────────────────────────────────────────────────────

void RegisterQuickBar() {
    // Initialize state
    sState.isOpen = false;
    sState.activeToolItem = -1;
    for (int i = 0; i < QB_CAT_COUNT; i++) {
        sState.lastUsed[i] = -1;
        sState.dpadHoldFrames[i] = 0;
        sState.dpadWasHeld[i] = false;
    }
    sState.selectionIndex = 0;

    // Pre-main hook: consume right stick before camera reads it
    COND_HOOK(OnGameStateMainStart, CVAR, []() {
        QuickBarPreMain();
    });

    COND_HOOK(OnGameStateUpdate, CVAR, []() {
        QuickBarUpdate();
    });

    // Freeze non-player actors while QuickBar is open (same visual effect as ocarina)
    COND_HOOK(ShouldActorUpdate, CVAR, [](Actor* actor, bool* should) {
        if (sState.isOpen && actor->category != ACTORCAT_PLAYER) {
            *should = false;
        }
    });
}

static RegisterShipInitFunc initFunc(RegisterQuickBar, { CVAR_NAME });
