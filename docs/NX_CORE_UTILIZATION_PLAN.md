# Nintendo Switch CPU Core Utilization: Debugging & Optimization Plan

## Problem Statement

Core 0 is maxed out running the main game loop while cores 1–2 sit mostly idle. The GPU is only at ~30% utilization, confirming a **CPU-bound bottleneck on core 0**. We need instrumentation to identify exactly where time is spent, and a plan to redistribute work across all available cores.

---

## 1. Current Core Allocation

The Switch has 4 ARM Cortex-A57 cores. Current thread-to-core mapping:

| Core | Thread | Workload | Estimated Load |
|------|--------|----------|----------------|
| **0** | Main thread | Game logic, actor updates, collision AT, BgCheck raycasts, display list generation, rendering submission, frame interpolation | **~100%** (bottleneck) |
| **1** | Collision worker | `CollisionCheck_OC` (O(n²) pairwise), then effects updates | **~15–25%** (idle most of frame) |
| **2** | Audio thread | `AudioMgr_CreateNextAudioBuffer` + playback | **~5–10%** (very light) |
| **3** | *(unused)* | — | **0%** |

> **Note on core 3**: The Switch OS may reserve core 3 for system tasks depending on the application's resource configuration. Availability of core 3 should be verified on target hardware using `svcGetInfo` or by attempting `svcSetThreadCoreMask` with core 3. If core 3 is unavailable, the optimization plan still applies but with a 2-core worker pool (cores 1 + shared time on core 2) instead of 3.

### Core 0 Frame Breakdown (What Runs on Main Thread)

A single frame on core 0 executes this pipeline in `Play_UpdateMain` / `Play_DrawMain`:

```
┌─────────────────────────────────────────────────────────────┐
│  GAME UPDATE PHASE (~60-70% of frame)                       │
│                                                             │
│  1. Submit OC to worker (core 1)                            │
│  2. CollisionCheck_AT()              ← main thread          │
│  3. Wait for OC worker               ← STALL if slow       │
│  4. CollisionCheck_Damage()          ← main thread          │
│  5. Actor_UpdateAll()                ← EXPENSIVE, main only │
│     ├─ per actor: actor->update()                           │
│     ├─ per actor: Actor_UpdateBgCheckInfo() ← BgCheck calls │
│     └─ per actor: physics/position updates                  │
│  6. Submit effects to worker (core 1)                       │
│  7. Cutscene / Room / Skybox / Message / Interface updates  │
│  8. Wait for effects worker           ← STALL if slow      │
├─────────────────────────────────────────────────────────────┤
│  DRAW PHASE (~30-40% of frame)                              │
│                                                             │
│  9. Play_DrawMain()                                         │
│     ├─ Actor_DrawAll()               ← EXPENSIVE            │
│     │   ├─ per actor: matrix setup (sys_matrix)             │
│     │   ├─ per actor: SkelAnime (skeleton animation)        │
│     │   └─ per actor: display list generation               │
│     └─ Environment / sky / effects drawing                  │
│ 10. Graph_ProcessGfxCommands()                              │
│     ├─ Frame interpolation replay    ← moderate cost        │
│     └─ GPU command submission        ← GPU only at 30%      │
└─────────────────────────────────────────────────────────────┘
```

### Why Cores 1–2 Are Idle

- **Core 1** only runs two small tasks per frame: `CollisionCheck_OC` (submitted/waited early) and `EffectsTask` (submitted/waited later). Between and after these, it sleeps on a condition variable.
- **Core 2** audio synthesis takes a fraction of the frame budget. It sleeps most of the time.
- **Core 3** is completely unused.

---

## 2. Instrumentation Plan (Phase 1: Measure)

Before optimizing, we need to **measure where time is actually spent** on core 0. Without data, any optimization is guesswork.

### 2a. Lightweight Frame Profiler

Add a minimal profiler that measures wall-clock time of each major phase per frame.

**Location**: New file `mm/2s2h/DeveloperTools/FrameProfiler.cpp` / `.h`

**Implementation**:

```cpp
// Use clock_gettime(CLOCK_MONOTONIC) for timing.
// Note: ARMv8 hardware cycle counters (PMCCNTR_EL0) require privileged
// access and are NOT available in user-space on Switch.
// clock_gettime is the recommended portable timing mechanism.
// Store per-phase timings in a ring buffer (last N frames)

struct FrameProfile {
    float collisionAT_ms;
    float collisionOC_ms;     // measured on worker thread
    float actorUpdateAll_ms;
    float bgCheckTotal_ms;    // accumulated across all actors
    float effectsUpdate_ms;   // measured on worker thread
    float actorDrawAll_ms;
    float frameInterp_ms;
    float gfxCommands_ms;
    float totalFrame_ms;
    float workerIdle_ms;      // time core 1 spent sleeping
};
```

**Instrumentation points** (wrap with timing calls):

| Phase | File | Function/Location |
|-------|------|-------------------|
| Collision AT | `z_play.c:~1071` | Around `CollisionCheck_AT()` |
| Collision OC | `z_play.c:~1069` | Inside `OcTask()` on worker |
| Actor Update | `z_play.c:~1078` | Around `Actor_UpdateAll()` |
| BgCheck calls | `z_bgcheck.c` | Accumulate inside `BgCheck_EntityRaycastFloor`, `BgCheck_SphVsWall`, etc. |
| Effects | `z_play.c:~1083` | Inside `EffectsTask()` on worker |
| Actor Draw | `z_play.c` draw section | Around the actor drawing loop |
| Frame Interp | `BenPort.cpp:~980` | Around frame interpolation in `Graph_ProcessGfxCommands` |
| GFX Submit | `BenPort.cpp:~990` | Around `RunCommands()` / `DrawAndRunGraphicsCommands()` |
| Worker idle | `collision_worker.cpp` | Track time spent in condition_variable wait |

### 2b. On-Screen Overlay

Display profiling data as an overlay (similar to existing DeveloperTools):

```
Frame: 18.2ms (54 FPS)  Target: 16.6ms
──────────────────────────────
Core 0: ActorUpdate  6.3ms ████████████░░░
Core 0: ActorDraw    4.1ms ████████░░░░░░░
Core 0: CollisionAT  1.8ms ████░░░░░░░░░░░
Core 0: BgCheck      3.2ms ██████░░░░░░░░░
Core 0: FrameInterp  1.4ms ███░░░░░░░░░░░░
Core 0: GfxSubmit    0.9ms ██░░░░░░░░░░░░░
Core 1: CollisionOC  2.1ms ████░░░░░░░░░░░
Core 1: Effects      0.8ms ██░░░░░░░░░░░░░
Core 1: Idle        14.0ms (77% idle)
Core 2: Audio        0.9ms ██░░░░░░░░░░░░░
Core 2: Idle        16.8ms (95% idle)
```

### 2c. Per-Actor Cost Tracking

Track the top-N most expensive actors by update + draw time:

```
Top Actors by CPU Cost:
  1. Player (ACTOR_PLAYER)      1.8ms update + 0.9ms draw
  2. En_Horse (ACTOR_EN_HORSE)  0.4ms update + 0.3ms draw
  3. En_Npc (ACTOR_EN_NPC)      0.3ms update + 0.2ms draw
  ...
```

This uses the existing `GameInteractor` hook system:
- `HOOK(OnActorUpdate)` → record start/end time per actor ID
- `HOOK(OnActorDraw)` → record start/end time per actor ID

---

## 3. Optimization Plan (Phase 2: Redistribute Work)

Based on the architecture analysis, here are the concrete changes ordered by **impact** and **safety**.

### 3a. HIGH IMPACT — Parallel Actor BgCheck (Background Collision Raycasts)

**Problem**: Every actor calls `Actor_UpdateBgCheckInfo()` which does multiple raycasts (floor, wall, ceiling) against the scene collision. These are **read-only queries** against static scene geometry and are fully independent per actor.

**Change**: After computing positions but before running actor-specific updates, batch all actor BgCheck queries and dispatch them across cores 1 and 3 (or a thread pool).

**Files to modify**:
- `mm/src/code/z_actor.c` — Split `Actor_UpdateAll` into: (a) collect actors needing BgCheck, (b) parallel BgCheck dispatch, (c) sequential actor updates
- `mm/2s2h/collision_worker.cpp` — Extend to support a thread pool (2+ worker threads) instead of a single worker, or add a second `TaskWorker` instance pinned to core 3

**Safety**: BgCheck reads are stateless lookups against immutable-per-frame collision data. No writes to shared state. Safe to parallelize.

**Estimated savings**: 2–4ms per frame (BgCheck is one of the heaviest per-actor costs).

### 3b. HIGH IMPACT — Move Actor_DrawAll Skeleton/Matrix Work Off Core 0

**Problem**: `Actor_DrawAll` spends significant time computing skeletal animation matrices (`SkelAnime_Update`, `Matrix_*` operations) before generating display lists. The matrix computation is independent per actor.

**Change**: Pre-compute actor matrices/skeleton poses on worker threads before the draw phase begins. The draw phase then only generates display list commands using precomputed transforms.

**Files to modify**:
- `mm/src/code/z_skelanime.c` — Add a "prepare" step that can run off main thread
- `mm/src/code/z_actor.c` — Insert parallel skeleton prepare between update and draw phases
- `mm/2s2h/collision_worker.cpp` — Reuse worker pool

**Safety**: Matrix computations are per-actor with no shared mutable state. Display list generation must remain serial (shared command buffer).

**Estimated savings**: 1–3ms per frame.

### 3c. MEDIUM IMPACT — Use Core 3 (Currently Unused)

**Problem**: Core 3 is completely idle.

**Change**: Convert the single `TaskWorker` into a `TaskWorkerPool` with workers on cores 1 and 3 (if available; see core 3 note above). Core 2 remains dedicated to audio. If core 3 is reserved by the OS, the pool can still use core 1 with multiple logical workers sharing that core, or time-share with core 2 using audio-priority scheduling.

**Files to modify**:
- `mm/2s2h/collision_worker.cpp` / `.h` — Add pool support:
  ```cpp
  // New API
  void TaskWorkerPool_Init(int numWorkers, const int* coreAffinities);
  void TaskWorkerPool_SubmitBatch(TaskFunc* tasks, void** args, int count);
  void TaskWorkerPool_WaitAll();
  ```
- Pin second worker to core 3: `svcSetThreadCoreMask(handle, 3, (1U << 3))`

**Safety**: No impact on audio thread. Core 3 is fully available for application use on Switch.

**Estimated savings**: Doubles parallel throughput for batched work.

### 3d. MEDIUM IMPACT — Overlap Collision Phases

**Problem**: Currently `CollisionCheck_AT` and `CollisionCheck_OC` run concurrently (good), but `CollisionCheck_Damage` waits for both to complete before running serially on core 0.

**Change**: With a worker pool, run AT on core 1 and OC on core 3 simultaneously, or pipeline damage resolution to start as soon as per-actor results are available.

**Files to modify**:
- `mm/src/code/z_play.c` — Restructure collision submission to use pool

**Safety**: AT writes `atFlags/acFlags`, OC writes `ocFlags1/ocFlags2` — separate field sets per collider. Already verified safe for concurrent execution.

**Estimated savings**: 0.5–1.5ms per frame (scene-dependent).

### 3e. LOW IMPACT — Async Frame Interpolation

**Problem**: Frame interpolation replay in `Graph_ProcessGfxCommands` processes all recorded matrix operations sequentially on core 0.

**Change**: Split the interpolation tree into segments and process on multiple cores. The replay is read-only against the recorded operations and writes to independent output matrices.

**Files to modify**:
- `mm/2s2h/Enhancements/FrameInterpolation/FrameInterpolation.cpp` — Add parallel segment processing

**Safety**: Each matrix interpolation is independent. Output matrices are per-actor, no conflicts.

**Estimated savings**: 0.5–1ms per frame.

### 3f. LOW IMPACT — Audio Thread Work Stealing

**Problem**: Core 2 is 90%+ idle since audio synthesis is lightweight.

**Change**: Allow core 2's audio thread to pick up general work items from the pool when it has no audio work pending. Use a priority system where audio tasks preempt general work.

**Safety**: Requires careful priority handling. Audio latency must not increase. This is the riskiest change and should only be done after the above items.

---

## 4. Profiler Results (Actual Measurements)

Real-world profiler data from Switch hardware (60-frame average):

```
Total Frame:    57.8 ms (17.3 FPS)
DL Process:     48.7 ms (84.2%)  ← DOMINANT BOTTLENECK
Play Draw:       5.1 ms ( 8.8%)
Frame Interp:    1.9 ms ( 3.4%)
Play Update:     0.5 ms ( 0.9%)
Audio Wait:      0.0 ms ( 0.0%)
Unaccounted:     1.6 ms ( 2.7%)

Core 0 Active:  53.6 ms
Core 1 Active:   0.01 ms (essentially idle)
DL Iterations:   1.0 per frame
```

### Key Finding

**The bottleneck is NOT game logic.** The display list interpreter (`DrawAndRunGraphicsCommands` in libultraship's Fast3D backend) consumes 84% of frame time with a single call per frame. Game logic (actors, collision, effects) totals only ~5.6ms — well within the 16.6ms budget.

GPU utilization is ~30%, confirming the CPU can't feed display list commands to the GPU fast enough. The ARM Cortex-A57 at ~1GHz spends 49ms translating N64 display lists to OpenGL ES commands.

### Revised Priorities

The original plan focused on parallelizing game logic (Phases 3-5). While those optimizations are still valid, they target only ~9ms of a 58ms frame. **The rendering backend must be addressed first.**

---

## 5. Implementation Order

```
Phase 1: Instrumentation ✅ IMPLEMENTED
  ├─ [1] ✅ Add FrameProfiler with timing points (FrameProfiler.cpp/h)
  ├─ [2] ✅ Add on-screen overlay display (ImGui window in Dev Tools menu)
  ├─ [3] ✅ Automated bottleneck analysis with percentage breakdown
  ├─ [4] ✅ Counter tracking (DL iterations per frame)
  ├─ [5] ✅ Snapshot export with analysis section
  └─ [6] Per-actor cost tracking via GameInteractor hooks (future)

Phase 2: Core 3 Activation ✅ IMPLEMENTED
  └─ [7] ✅ TaskWorkerPool with 2 threads on cores 1+3

Phase 2b: Parallel Collision ✅ IMPLEMENTED
  └─ [8] ✅ AT on main thread, OC on worker thread

Phase 3: Fast3D Interpreter Optimization (HIGH PRIORITY — 84% of frame)
  ├─ [9] Profile inside DrawAndRunGraphicsCommands (requires libultraship changes)
  │      Break down: DL walking, texture binds, shader switches, draw calls, buffer swap
  ├─ [10] Batch small draw calls — reduce GL driver overhead
  ├─ [11] Texture atlas / bind reduction — minimize state changes
  ├─ [12] Vertex batching — aggregate small triangles into fewer draw calls
  └─ [13] Display list caching — skip re-interpretation for static scene geometry

Phase 4: Render Thread Offload (HIGH PRIORITY — unlocks parallelism)
  ├─ [14] Move DrawAndRunGraphicsCommands to a dedicated render thread (Core 1)
  │      Overlap: Core 0 generates frame N+1's DL while Core 1 renders frame N
  ├─ [15] Double-buffer display lists (already have gGfxPools[0]/[1])
  └─ [16] Validate frame pacing and synchronization
  Expected: 58ms → ~49ms (9ms game logic runs in parallel with 49ms render)

Phase 5: Game Logic Optimization (LOWER PRIORITY — only 9ms total)
  ├─ [17] Batch BgCheck queries in Actor_UpdateAll
  ├─ [18] Dispatch batches to worker pool
  ├─ [19] Separate skeleton/matrix prep from DL generation
  ├─ [20] Parallelize matrix prep on worker pool
  └─ [21] Validate with profiler

Phase 6: Additional Optimizations (future)
  ├─ [22] Async frame interpolation segments
  ├─ [23] DL complexity reduction (draw distance CVars, LOD)
  └─ [24] (Optional) Audio thread work stealing
```

---

## 5. Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| Race conditions in parallel BgCheck | BgCheck queries are read-only against per-frame snapshot of collision data. Add debug assertions to verify no writes during parallel phase. |
| Actor update order dependencies | Keep actor `update()` calls sequential on core 0. Only parallelize stateless computations (BgCheck, matrix math). |
| Display list corruption | Display list generation stays single-threaded. Only pre-computation moves off core 0. |
| Audio latency increase | Audio stays on dedicated core 2. Never share core 2 with heavy compute unless using strict priority preemption. |
| Profiling overhead | Use `clock_gettime(CLOCK_MONOTONIC)` for timing. Typical overhead is tens of nanoseconds per call on ARM, but actual cost on Switch should be measured on target hardware since it may involve a syscall. Compile out with `#ifdef ENABLE_PROFILER`. With ~20 timing points per frame, worst-case overhead is well under 0.1ms. |
| Dynamic actor counts | Worker pool batch sizes adapt to actor count. Small scenes may not benefit but won't regress. |

---

## 6. Expected Outcome

**Current state**: Core 0 at 100%, cores 1-2 at ~15%, core 3 at 0%. GPU at 30%. **CPU-bottlenecked.**

**Target state**: Core 0 at ~60%, cores 1+3 at ~40-50%, core 2 at ~10%. GPU utilization rises as CPU feeds it faster.

**Expected frame time improvement**: 4–8ms reduction on core 0, which would move from ~18ms/frame (55 FPS) to ~10-14ms/frame (70+ FPS at the CPU level), letting the GPU become the limiting factor instead.

---

## 7. Files Summary

| File | Changes |
|------|---------|
| `mm/2s2h/DeveloperTools/FrameProfiler.cpp` (new) | Frame timing infrastructure |
| `mm/2s2h/DeveloperTools/FrameProfiler.h` (new) | Profiler API |
| `mm/2s2h/collision_worker.cpp` | Expand to thread pool, add idle tracking |
| `mm/2s2h/collision_worker.h` | Pool API additions |
| `mm/src/code/z_play.c` | Restructure update/draw to use pool |
| `mm/src/code/z_actor.c` | Batch BgCheck, parallel dispatch |
| `mm/src/code/z_bgcheck.c` | Thread-safety assertions |
| `mm/src/code/z_skelanime.c` | Separate prepare from draw |
| `mm/2s2h/Enhancements/FrameInterpolation/FrameInterpolation.cpp` | Parallel segment replay |
| `mm/2s2h/BenPort.cpp` | Profiler hooks in Graph_ProcessGfxCommands |
