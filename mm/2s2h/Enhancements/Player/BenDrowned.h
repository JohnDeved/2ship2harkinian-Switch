#pragma once

#include <array>
#include <cstddef>

extern "C" {
#include "ultra64.h"
#include "z64math.h"
}

namespace BenDrowned {

constexpr std::size_t DEBUG_HISTORY_SIZE = 48;

struct DebugHistoryEntry {
    Vec3f pos;
    f32 playerDist;
    f32 playerDistSq;
    bool visible;
    bool spawnEligible;
    bool distantEligible;
};

struct TuningParams {
    s32 moveCooldownFrames;
    s32 respawnCooldownFrames;
    s32 dialogueCooldownFrames;
    s32 laughBaseFrames;
    s32 laughRandomFrames;
    s32 colorDistortBaseFrames;
    s32 colorDistortRandomFrames;
    s32 colorDistortDuration;
    f32 disappearChance;
    f32 dialogueChance;
    f32 laughMinPitch;
    f32 laughMaxPitch;
    f32 historyPointMinDist;
    f32 minSpawnDist;
    f32 distantSpawnDist;
    f32 maxNearbyDist;
    f32 minRepositionDist;
    f32 moveThresholdDist;
    f32 closeEffectDist;
    f32 fallbackStalkDist;
    f32 proximityRumbleDist;
};

struct DebugSnapshot {
    bool enabled;
    bool hasPlayState;
    bool normalGameplayState;
    bool playerValid;
    bool playerGroundedAndDry;
    bool statueAlive;
    bool statueManaged;
    bool statueObserved;
    bool statueVisible;
    bool statueWasVisible;
    bool disappearAfterObserved;
    bool respawnReady;
    bool moveReady;
    bool hasDistantSpawnPoint;
    bool hasTargetPoint;
    s32 historyCount;
    s32 recordTimer;
    s32 moveCooldown;
    s32 respawnCooldown;
    s32 colorDistortCooldown;
    s32 effectCooldown;
    s32 laughCooldown;
    s32 dialogueCooldown;
    Vec3f playerPos;
    Vec3f statuePos;
    Vec3f distantSpawnPoint;
    Vec3f targetPoint;
    std::array<DebugHistoryEntry, DEBUG_HISTORY_SIZE> history;
};

DebugSnapshot GetDebugSnapshot();
TuningParams& GetTuning();
void SaveTuning();

} // namespace BenDrowned
