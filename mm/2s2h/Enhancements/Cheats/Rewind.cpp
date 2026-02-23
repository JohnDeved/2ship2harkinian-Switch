#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"
#include "2s2h/BenPort.h"

extern "C" {
#include "z64.h"
#include "variables.h"
}

#define CVAR_NAME "gCheats.Rewind"
#define CVAR CVarGetInteger(CVAR_NAME, 0)

// 30 seconds of history at 20 fps game logic rate
static constexpr int REWIND_BUFFER_SIZE = 30 * 20;

// Upper bound on joint/morph table entries per skeleton.
// SkelAnime.limbCount is u8, but MM skeletons rarely exceed ~30 limbs.
// 76 provides a safe margin for any actor without wasting excessive memory.
static constexpr int MAX_LIMBS = 76;

// Hint for vector::reserve to avoid reallocation in typical scenes
static constexpr int EXPECTED_MAX_ACTORS = 256;

// Per-actor snapshot of visual/physics state.
// ActorShape is POD (Vec3s, scalars, function pointer) so a shallow copy is safe.
struct ActorSnapshot {
    Actor* ptr;
    s16 id;
    u8 category;
    PosRot world;
    PosRot focus;
    Vec3f prevPos;
    Vec3f scale;
    ActorShape shape;
    Vec3f velocity;
    f32 speed;
    f32 gravity;
    u32 flags;
    u8 health;
    u16 colorFilterParams;
    u8 colorFilterTimer;
    u16 freezeTimer;
};

// SkelAnime scalar fields + joint/morph table data for animation replay
struct SkelAnimeSnapshot {
    void* animation;
    f32 startFrame;
    f32 endFrame;
    f32 animLength;
    f32 curFrame;
    f32 playSpeed;
    f32 morphWeight;
    f32 morphRate;
    u8 mode;
    u8 movementFlags;
    s16 prevYaw;
    Vec3s prevTransl;
    Vec3s baseTransl;
    u8 limbCount;
    Vec3s jointTable[MAX_LIMBS];
    Vec3s morphTable[MAX_LIMBS];
};

// Player-specific animation state (main body + upper body blend)
struct PlayerSnapshot {
    bool valid = false;
    SkelAnimeSnapshot skelAnime;
    SkelAnimeSnapshot skelAnimeUpper;
    u32 stateFlags1;
    u32 stateFlags2;
    u32 stateFlags3;
    u8 transformation;
    u8 currentMask;
};

// Player resource state (health, magic, rupees)
struct PlayerResourceSnapshot {
    s16 health;
    s8 magic;
    s16 rupees;
    s16 magicState;
};

// Camera state for smooth rewind visuals
struct CameraSnapshot {
    Vec3f eye;
    Vec3f at;
    Vec3f up;
    Vec3f eyeNext;
    f32 fov;
    f32 dist;
};

// Scene-level flags (switches, chests, collectibles, cleared rooms)
struct SceneFlagsSnapshot {
    ActorContextSceneFlags flags;
};

// NPC animation entry — stores one SkelAnime snapshot for a non-Player actor
struct NpcAnimEntry {
    Actor* ptr;
    SkelAnimeSnapshot anim;
};

// One complete frame of game state
struct FrameSnapshot {
    bool valid = false;
    u32 gameplayFrames = 0;
    std::vector<ActorSnapshot> actors;
    PlayerSnapshot player;
    PlayerResourceSnapshot resources;
    SceneFlagsSnapshot sceneFlags;
    CameraSnapshot camera;
    std::vector<NpcAnimEntry> npcAnims;
};

static std::vector<FrameSnapshot> sRewindBuffer(REWIND_BUFFER_SIZE);
static int sBufferHead = 0;
static int sBufferCount = 0;
static bool sIsRewinding = false;

// Reusable containers to avoid per-frame heap allocation
static std::unordered_map<Actor*, size_t> sSnapshotLookup;
static std::unordered_set<Actor*> sHiddenActors;
static std::unordered_map<Actor*, size_t> sNpcAnimLookup;

// Cache: actor ID -> byte offset of first SkelAnime within actor struct, -1 = none
static std::unordered_map<s16, int> sSkelAnimeOffsetCache;

static void ClearBuffer() {
    for (auto& frame : sRewindBuffer) {
        frame.valid = false;
        frame.actors.clear();
        frame.actors.reserve(EXPECTED_MAX_ACTORS);
        frame.player.valid = false;
        frame.npcAnims.clear();
    }
    sBufferHead = 0;
    sBufferCount = 0;
    sIsRewinding = false;
    sHiddenActors.clear();
    sSkelAnimeOffsetCache.clear();
}

static void CaptureSkelAnime(SkelAnimeSnapshot& out, const SkelAnime* src) {
    out.animation = src->animation;
    out.startFrame = src->startFrame;
    out.endFrame = src->endFrame;
    out.animLength = src->animLength;
    out.curFrame = src->curFrame;
    out.playSpeed = src->playSpeed;
    out.morphWeight = src->morphWeight;
    out.morphRate = src->morphRate;
    out.mode = src->mode;
    out.movementFlags = src->movementFlags;
    out.prevYaw = src->prevYaw;
    out.prevTransl = src->prevTransl;
    out.baseTransl = src->baseTransl;

    // SkelAnime.limbCount is already u8, so no truncation risk
    int count = src->limbCount;
    out.limbCount = src->limbCount;
    if (count > MAX_LIMBS) {
        count = MAX_LIMBS;
    }
    if (src->jointTable != NULL && count > 0) {
        memcpy(out.jointTable, src->jointTable, count * sizeof(Vec3s));
    }
    if (src->morphTable != NULL && count > 0) {
        memcpy(out.morphTable, src->morphTable, count * sizeof(Vec3s));
    }
}

static void RestoreSkelAnime(SkelAnime* dst, const SkelAnimeSnapshot& src) {
    dst->animation = src.animation;
    dst->startFrame = src.startFrame;
    dst->endFrame = src.endFrame;
    dst->animLength = src.animLength;
    dst->curFrame = src.curFrame;
    dst->playSpeed = src.playSpeed;
    dst->morphWeight = src.morphWeight;
    dst->morphRate = src.morphRate;
    dst->mode = src.mode;
    dst->movementFlags = src.movementFlags;
    dst->prevYaw = src.prevYaw;
    dst->prevTransl = src.prevTransl;
    dst->baseTransl = src.baseTransl;

    int count = src.limbCount;
    if (count > MAX_LIMBS) {
        count = MAX_LIMBS;
    }
    if (dst->jointTable != NULL && count > 0) {
        memcpy(dst->jointTable, src.jointTable, count * sizeof(Vec3s));
    }
    if (dst->morphTable != NULL && count > 0) {
        memcpy(dst->morphTable, src.morphTable, count * sizeof(Vec3s));
    }
}

// Heuristic check: does memory at this location look like a valid SkelAnime?
static bool IsLikelySkelAnime(const SkelAnime* sa, int remaining) {
    if (remaining < (int)sizeof(SkelAnime)) {
        return false;
    }
    if (sa->limbCount == 0 || sa->limbCount > MAX_LIMBS) {
        return false;
    }
    // mode: 0=loop, 1=loop+interp, 2=once, 3=once+interp, 4=partial, 5=partial+interp
    if (sa->mode > 5) {
        return false;
    }
    if (sa->skeleton == NULL || ((uintptr_t)sa->skeleton & 3) != 0) {
        return false;
    }
    if (sa->jointTable == NULL || ((uintptr_t)sa->jointTable & 1) != 0) {
        return false;
    }
    // Zero or negative animLength would cause divide-by-zero in the animation engine
    if (sa->animLength <= 0.0f) {
        return false;
    }
    return true;
}

// Return a pointer to the SkelAnime at the given byte offset within an actor.
static SkelAnime* GetActorSkelAnime(Actor* actor, int offset) {
    return reinterpret_cast<SkelAnime*>(reinterpret_cast<u8*>(actor) + offset);
}

// Find the byte offset of the first SkelAnime within an actor's extended struct.
// Result is cached by actor ID since all instances of the same actor type share
// the same struct layout. Returns -1 if no SkelAnime is found.
static int FindSkelAnimeOffset(Actor* actor) {
    auto cached = sSkelAnimeOffsetCache.find(actor->id);
    if (cached != sSkelAnimeOffsetCache.end()) {
        return cached->second;
    }

    int result = -1;
    if (actor->overlayEntry != NULL && actor->overlayEntry->profile != NULL) {
        u32 instSize = actor->overlayEntry->profile->instanceSize;
        if (instSize >= sizeof(Actor) + sizeof(SkelAnime)) {
            const u8* base = reinterpret_cast<const u8*>(actor);
            int end = (int)instSize - (int)sizeof(SkelAnime);
            // Scan at pointer-aligned (4-byte) offsets since struct fields are aligned
            for (int off = (int)sizeof(Actor); off <= end; off += 4) {
                const SkelAnime* sa = reinterpret_cast<const SkelAnime*>(base + off);
                if (IsLikelySkelAnime(sa, (int)instSize - off)) {
                    result = off;
                    break;
                }
            }
        }
    }

    sSkelAnimeOffsetCache[actor->id] = result;
    return result;
}

static void CaptureFrame() {
    if (gPlayState == NULL) {
        return;
    }

    FrameSnapshot& snapshot = sRewindBuffer[sBufferHead];
    snapshot.valid = true;
    snapshot.gameplayFrames = gPlayState->gameplayFrames;

    snapshot.actors.clear();

    for (int i = 0; i < ACTORCAT_MAX; i++) {
        Actor* actor = gPlayState->actorCtx.actorLists[i].first;
        while (actor != NULL) {
            ActorSnapshot as;
            as.ptr = actor;
            as.id = actor->id;
            as.category = actor->category;
            as.world = actor->world;
            as.focus = actor->focus;
            as.prevPos = actor->prevPos;
            as.scale = actor->scale;
            as.shape = actor->shape;
            as.velocity = actor->velocity;
            as.speed = actor->speed;
            as.gravity = actor->gravity;
            as.flags = actor->flags;
            as.health = actor->colChkInfo.health;
            as.colorFilterParams = actor->colorFilterParams;
            as.colorFilterTimer = actor->colorFilterTimer;
            as.freezeTimer = actor->freezeTimer;
            snapshot.actors.push_back(as);
            actor = actor->next;
        }
    }

    // Capture Player animation state (main body + upper body blend)
    Player* player = GET_PLAYER(gPlayState);
    if (player != NULL) {
        snapshot.player.valid = true;
        CaptureSkelAnime(snapshot.player.skelAnime, &player->skelAnime);
        CaptureSkelAnime(snapshot.player.skelAnimeUpper, &player->skelAnimeUpper);
        snapshot.player.stateFlags1 = player->stateFlags1;
        snapshot.player.stateFlags2 = player->stateFlags2;
        snapshot.player.stateFlags3 = player->stateFlags3;
        snapshot.player.transformation = player->transformation;
        snapshot.player.currentMask = player->currentMask;
    } else {
        snapshot.player.valid = false;
    }

    // Capture player resources (health, magic, rupees)
    snapshot.resources.health = gSaveContext.save.saveInfo.playerData.health;
    snapshot.resources.magic = gSaveContext.save.saveInfo.playerData.magic;
    snapshot.resources.rupees = gSaveContext.save.saveInfo.playerData.rupees;
    snapshot.resources.magicState = gSaveContext.magicState;

    // Capture scene flags (switches, chests, collectibles, cleared rooms)
    snapshot.sceneFlags.flags = gPlayState->actorCtx.sceneFlags;

    // Capture camera state for smooth rewind visuals
    Camera* mainCam = GET_ACTIVE_CAM(gPlayState);
    if (mainCam != NULL) {
        snapshot.camera.eye = mainCam->eye;
        snapshot.camera.at = mainCam->at;
        snapshot.camera.up = mainCam->up;
        snapshot.camera.eyeNext = mainCam->eyeNext;
        snapshot.camera.fov = mainCam->fov;
        snapshot.camera.dist = mainCam->dist;
    }

    // Capture NPC animation state by scanning for SkelAnime in each actor's struct
    snapshot.npcAnims.clear();
    Actor* playerActor = (player != NULL) ? &player->actor : NULL;
    for (int i = 0; i < ACTORCAT_MAX; i++) {
        Actor* actor = gPlayState->actorCtx.actorLists[i].first;
        while (actor != NULL) {
            if (actor != playerActor) {
                int offset = FindSkelAnimeOffset(actor);
                if (offset >= 0) {
                    NpcAnimEntry entry;
                    entry.ptr = actor;
                    const SkelAnime* sa = GetActorSkelAnime(actor, offset);
                    CaptureSkelAnime(entry.anim, sa);
                    snapshot.npcAnims.push_back(entry);
                }
            }
            actor = actor->next;
        }
    }

    sBufferHead = (sBufferHead + 1) % REWIND_BUFFER_SIZE;
    if (sBufferCount < REWIND_BUFFER_SIZE) {
        sBufferCount++;
    }
}

static void RestoreFrame() {
    if (gPlayState == NULL || sBufferCount <= 0) {
        return;
    }

    // Step back one frame in the ring buffer
    sBufferHead = (sBufferHead - 1 + REWIND_BUFFER_SIZE) % REWIND_BUFFER_SIZE;
    sBufferCount--;

    FrameSnapshot& snapshot = sRewindBuffer[sBufferHead];
    if (!snapshot.valid) {
        return;
    }

    // Build lookup table (reuse static map to avoid heap allocation)
    sSnapshotLookup.clear();
    sSnapshotLookup.reserve(snapshot.actors.size());
    for (size_t idx = 0; idx < snapshot.actors.size(); idx++) {
        sSnapshotLookup[snapshot.actors[idx].ptr] = idx;
    }

    // Build NPC animation lookup
    sNpcAnimLookup.clear();
    sNpcAnimLookup.reserve(snapshot.npcAnims.size());
    for (size_t idx = 0; idx < snapshot.npcAnims.size(); idx++) {
        sNpcAnimLookup[snapshot.npcAnims[idx].ptr] = idx;
    }

    // Restore actors that exist in the snapshot; hide actors spawned after this frame
    sHiddenActors.clear();
    for (int i = 0; i < ACTORCAT_MAX; i++) {
        Actor* actor = gPlayState->actorCtx.actorLists[i].first;
        while (actor != NULL) {
            auto it = sSnapshotLookup.find(actor);
            if (it != sSnapshotLookup.end()) {
                const ActorSnapshot& as = snapshot.actors[it->second];
                // Guard against pointer reuse: a killed actor's address may be
                // reused by a newly spawned actor of a different type.
                if (as.id == actor->id && as.category == actor->category) {
                    actor->world = as.world;
                    actor->focus = as.focus;
                    actor->prevPos = as.prevPos;
                    actor->scale = as.scale;
                    actor->shape = as.shape;
                    actor->velocity = as.velocity;
                    actor->speed = as.speed;
                    actor->gravity = as.gravity;
                    actor->flags = as.flags;
                    actor->colChkInfo.health = as.health;
                    actor->colorFilterParams = as.colorFilterParams;
                    actor->colorFilterTimer = as.colorFilterTimer;
                    actor->freezeTimer = as.freezeTimer;

                    // Restore NPC animation if captured
                    auto animIt = sNpcAnimLookup.find(actor);
                    if (animIt != sNpcAnimLookup.end()) {
                        int offset = FindSkelAnimeOffset(actor);
                        if (offset >= 0) {
                            RestoreSkelAnime(GetActorSkelAnime(actor, offset), snapshot.npcAnims[animIt->second].anim);
                        }
                    }
                }
            } else {
                // Actor was spawned after this snapshot frame — hide during rewind
                sHiddenActors.insert(actor);
            }
            actor = actor->next;
        }
    }

    // Restore Player animation state
    Player* player = GET_PLAYER(gPlayState);
    if (player != NULL && snapshot.player.valid) {
        RestoreSkelAnime(&player->skelAnime, snapshot.player.skelAnime);
        RestoreSkelAnime(&player->skelAnimeUpper, snapshot.player.skelAnimeUpper);
        player->stateFlags1 = snapshot.player.stateFlags1;
        player->stateFlags2 = snapshot.player.stateFlags2;
        player->stateFlags3 = snapshot.player.stateFlags3;
        player->transformation = snapshot.player.transformation;
        player->currentMask = snapshot.player.currentMask;
    }

    // Restore player resources (health, magic, rupees)
    gSaveContext.save.saveInfo.playerData.health = snapshot.resources.health;
    gSaveContext.save.saveInfo.playerData.magic = snapshot.resources.magic;
    gSaveContext.save.saveInfo.playerData.rupees = snapshot.resources.rupees;
    gSaveContext.magicState = snapshot.resources.magicState;

    // Restore scene flags (switches, chests, collectibles, cleared rooms)
    gPlayState->actorCtx.sceneFlags = snapshot.sceneFlags.flags;

    // Restore camera state for smooth rewind visuals
    Camera* mainCam = GET_ACTIVE_CAM(gPlayState);
    if (mainCam != NULL) {
        mainCam->eye = snapshot.camera.eye;
        mainCam->at = snapshot.camera.at;
        mainCam->up = snapshot.camera.up;
        mainCam->eyeNext = snapshot.camera.eyeNext;
        mainCam->fov = snapshot.camera.fov;
        mainCam->dist = snapshot.camera.dist;
    }

    gPlayState->gameplayFrames = snapshot.gameplayFrames;
}

void RegisterRewind() {
    // At the start of each frame: check input and restore state if rewinding.
    // Restoring here (before Play_Main) means the camera update inside Play_Main
    // will naturally track the restored player position.
    COND_HOOK(OnGameStateMainStart, CVAR, []() {
        if (gPlayState == NULL) {
            return;
        }

        Input* input = CONTROLLER1(&gPlayState->state);
        bool rewindPressed = CHECK_BTN_ALL(input->cur.button, BTN_CUSTOM_MODIFIER1);

        // Don't allow rewind during pause, cutscenes, or message dialogs
        if (gPlayState->pauseCtx.state != PAUSE_STATE_OFF || gPlayState->csCtx.state != CS_STATE_IDLE ||
            gPlayState->msgCtx.msgMode != MSGMODE_NONE) {
            rewindPressed = false;
        }

        bool wasRewinding = sIsRewinding;
        sIsRewinding = rewindPressed && sBufferCount > 0;

        if (sIsRewinding) {
            RestoreFrame();
        } else if (wasRewinding) {
            // Rewind just ended — kill actors that were spawned after
            // the point we rewound to so they don't persist in the new timeline.
            for (int i = 0; i < ACTORCAT_MAX; i++) {
                Actor* actor = gPlayState->actorCtx.actorLists[i].first;
                while (actor != NULL) {
                    Actor* next = actor->next;
                    if (sHiddenActors.count(actor) > 0) {
                        Actor_Kill(actor);
                    }
                    actor = next;
                }
            }
            sHiddenActors.clear();
        }
    });

    // At the end of each frame: capture state when not rewinding.
    // This runs after all actor and camera updates have completed.
    COND_HOOK(OnGameStateMainFinish, CVAR, []() {
        if (!sIsRewinding) {
            CaptureFrame();
        }
    });

    // Prevent normal actor updates while rewinding so the restored
    // positions are not immediately overwritten by game logic.
    COND_HOOK(ShouldActorUpdate, CVAR, [](Actor* actor, bool* should) {
        if (sIsRewinding) {
            *should = false;
        }
    });

    // Hide actors that were spawned after the current rewind point.
    COND_HOOK(ShouldActorDraw, CVAR, [](Actor* actor, bool* should) {
        if (sIsRewinding && sHiddenActors.count(actor) > 0) {
            *should = false;
        }
    });

    // Clear the rewind buffer on scene changes since actor pointers
    // become invalid when a new scene is loaded.
    COND_HOOK(OnSceneInit, CVAR, [](s8 sceneId, s8 spawnNum) { ClearBuffer(); });
}

static RegisterShipInitFunc initFunc(RegisterRewind, { CVAR_NAME });
