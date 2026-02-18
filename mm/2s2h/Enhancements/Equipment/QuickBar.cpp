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

// Saved R_UPDATE_RATE before QuickBar opened (for restoring game speed)
static s32 sSavedUpdateRate = 0;

// Deferred song playback (needs many frames for the ocarina state machine to initialize)
struct DeferredSong {
    bool pending;
    int songId;       // OcarinaSongId for AudioOcarina_SetPlaybackSong
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

// ─── Time-stop control (uses R_UPDATE_RATE for visible slow-motion) ─────────

static void EnterTimeStop() {
    if (gPlayState == nullptr) return;
    // Save the current game speed divisor so we can restore it later
    sSavedUpdateRate = R_UPDATE_RATE;
    // Double the rate to halve the game speed (visible slow motion)
    R_UPDATE_RATE = sSavedUpdateRate * 2;
    // Also stop the day/night clock (same as TimeStop cheat)
    R_TIME_SPEED = 0;
}

static void ExitTimeStop() {
    if (gPlayState == nullptr) return;
    // Restore original game speed
    R_UPDATE_RATE = sSavedUpdateRate;
}

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
    EnterTimeStop();
    Audio_PlaySfx(NA_SE_SY_WIN_OPEN);
}

static void CloseQuickBar(bool confirm) {
    if (!sState.isOpen) return;

    ExitTimeStop();

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
            case QB_CAT_MASKS:
                // Masks: use Player_UseItem which toggles equip on/off
                UseItemOnPlayer(selectedItem);
                break;
            case QB_CAT_SONGS:
                if (CVarGetInteger("gEnhancements.Equipment.QuickBar.AutoPlaySongs", 1)) {
                    // Pull out ocarina, then defer song auto-play by 30 frames
                    // to give the ocarina state machine time to fully initialize
                    UseItemOnPlayer(ITEM_OCARINA_OF_TIME);
                    int songId = QuestBitToSongId(selectedItem);
                    if (songId >= 0) {
                        sDeferredSong = { true, songId, 30 };
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

    switch (cat) {
        case QB_CAT_TOOLS:
            // Tap RIGHT: just set the active tool (shown on D-pad HUD)
            if (lastItem >= 0) {
                SetActiveTool(lastItem);
            }
            break;
        case QB_CAT_BOTTLES:
            // Tap DOWN: just set last bottle (shown on D-pad HUD)
            // Actual use via RB click (future)
            break;
        case QB_CAT_MASKS:
            // Tap UP: equip last mask (masks are toggle-equip, works fine)
            if (lastItem >= 0) {
                UseItemOnPlayer(lastItem);
            }
            break;
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
        input->cur.x = 0;
        input->cur.y = 0;
        input->rel.x = 0;
        input->rel.y = 0;

        // Enforce slow-motion every frame (game may try to reset R_UPDATE_RATE)
        if (sSavedUpdateRate > 0) {
            R_UPDATE_RATE = sSavedUpdateRate * 2;
        }
        R_TIME_SPEED = 0;
    } else {
        sSavedRightStickX = 0;
        sSavedRightStickY = 0;
    }
}

// ─── Per-frame update ───────────────────────────────────────────────────────

static void QuickBarUpdate() {
    if (gPlayState == nullptr) return;

    // Process deferred song playback (needs ocarina state machine to initialize)
    if (sDeferredSong.pending) {
        if (sDeferredSong.delayFrames > 0) {
            sDeferredSong.delayFrames--;
        } else {
            sDeferredSong.pending = false;
            AudioOcarina_SetInstrument(OCARINA_INSTRUMENT_DEFAULT);
            AudioOcarina_SetPlaybackSong(sDeferredSong.songId + 1, 1);
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
}

static RegisterShipInitFunc initFunc(RegisterQuickBar, { CVAR_NAME });
