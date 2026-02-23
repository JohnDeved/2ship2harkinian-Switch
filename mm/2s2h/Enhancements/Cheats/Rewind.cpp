#include <unordered_map>
#include <vector>

#include <libultraship/bridge/consolevariablebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

extern "C" {
#include "z64.h"
#include "variables.h"
}

#define CVAR_NAME "gCheats.Rewind"
#define CVAR CVarGetInteger(CVAR_NAME, 0)

// 30 seconds of history at 20 fps game logic rate
static constexpr int REWIND_BUFFER_SIZE = 30 * 20;

// Per-actor snapshot of visual/physics state.
// ActorShape is POD (Vec3s, scalars, function pointer) so a shallow copy is safe.
struct ActorSnapshot {
    Actor* ptr;
    s16 id;
    u8 category;
    PosRot world;
    Vec3f prevPos;
    ActorShape shape;
    Vec3f velocity;
    f32 speed;
    f32 gravity;
};

// One complete frame of game state
struct FrameSnapshot {
    bool valid = false;
    u32 gameplayFrames = 0;
    std::vector<ActorSnapshot> actors;
};

static std::vector<FrameSnapshot> sRewindBuffer(REWIND_BUFFER_SIZE);
static int sBufferHead = 0;
static int sBufferCount = 0;
static bool sIsRewinding = false;

static void ClearBuffer() {
    for (auto& frame : sRewindBuffer) {
        frame.valid = false;
        frame.actors.clear();
    }
    sBufferHead = 0;
    sBufferCount = 0;
    sIsRewinding = false;
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
            as.prevPos = actor->prevPos;
            as.shape = actor->shape;
            as.velocity = actor->velocity;
            as.speed = actor->speed;
            as.gravity = actor->gravity;
            snapshot.actors.push_back(as);
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

    // Build a lookup table from actor pointer to snapshot index for O(1) matching.
    std::unordered_map<Actor*, size_t> snapshotLookup;
    snapshotLookup.reserve(snapshot.actors.size());
    for (size_t idx = 0; idx < snapshot.actors.size(); idx++) {
        snapshotLookup[snapshot.actors[idx].ptr] = idx;
    }

    // Restore actor states by matching live actors against the snapshot.
    // We iterate the live actor list (safe pointers) and look up the snapshot
    // entry. This avoids dereferencing potentially stale pointers.
    for (int i = 0; i < ACTORCAT_MAX; i++) {
        Actor* actor = gPlayState->actorCtx.actorLists[i].first;
        while (actor != NULL) {
            auto it = snapshotLookup.find(actor);
            if (it != snapshotLookup.end()) {
                const ActorSnapshot& as = snapshot.actors[it->second];
                if (as.id == actor->id && as.category == actor->category) {
                    actor->world = as.world;
                    actor->prevPos = as.prevPos;
                    actor->shape = as.shape;
                    actor->velocity = as.velocity;
                    actor->speed = as.speed;
                    actor->gravity = as.gravity;
                }
            }
            actor = actor->next;
        }
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
        bool rewindPressed = CHECK_BTN_ALL(input->cur.button, BTN_DLEFT);

        sIsRewinding = rewindPressed && sBufferCount > 0;

        if (sIsRewinding) {
            RestoreFrame();
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

    // Clear the rewind buffer on scene changes since actor pointers
    // become invalid when a new scene is loaded.
    COND_HOOK(OnSceneInit, CVAR, [](s8 sceneId, s8 spawnNum) { ClearBuffer(); });
}

static RegisterShipInitFunc initFunc(RegisterRewind, { CVAR_NAME });
