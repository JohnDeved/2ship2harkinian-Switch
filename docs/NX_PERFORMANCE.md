# Nintendo Switch Performance Guide

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Hardware Specs](#hardware-specs)
3. [Profiler Data](#profiler-data)
4. [Core Allocation](#core-allocation)
5. [Implemented Performance Tooling](#implemented-performance-tooling)
6. [How to Use the Profiler](#how-to-use-the-profiler)
7. [How to Read Profiler Output](#how-to-read-profiler-output)
8. [Optimization Status](#optimization-status)
9. [Implementation Priority](#implementation-priority)
10. [NX-Specific Characteristics](#nx-specific-characteristics)
11. [Success Criteria](#success-criteria)

---

## Executive Summary

The dominant bottleneck on Switch is the **display list interpreter** (`DrawAndRunGraphicsCommands` in libultraship's Fast3D backend), consuming ~84% of each frame. GPU utilization sits at ~30% — the CPU cannot feed GL commands fast enough. Game logic totals only ~6ms, well within the 16.6ms budget for 60 FPS.

**Key finding**: Optimizing game logic (actors, collision, effects) alone cannot reach 60 FPS. The Fast3D rendering backend must be optimized first.

---

## Hardware Specs

| Component | Specification |
|-----------|--------------|
| **SoC** | NVIDIA Tegra X1 (Erista/Mariko) |
| **CPU** | 4× ARM Cortex-A57 @ 1020 MHz, 32 KB L1D/core, 2 MB shared L2 |
| **GPU** | Maxwell, 256 CUDA cores — Docked: 768 MHz, Handheld: 307/384 MHz |
| **Memory** | 4 GB LPDDR4 @ 1600 MHz (25.6 GB/s shared CPU+GPU) |
| **Graphics API** | OpenGL ES 3.2 via NVN compatibility layer |
| **Build** | AArch64, `-O3 -ffast-math`, devkitPro toolchain |

**Thermal**: Sustained CPU load may trigger throttling, reducing GPU clocks further. Distributing work across cores mitigates this.

---

## Profiler Data

Real-world measurements from Switch hardware (60-frame average):

| Metric | Value |
|--------|-------|
| Total frame time | 60.4 ms (16.5 FPS) |
| DL Process time | ~50.8 ms (~84% of frame) |
| Game logic total | ~6 ms (~10% of frame) |
| Frame interpolation | ~2 ms (~3%) |
| DL iterations/frame | 1.0 |
| GPU utilization | ~30% |
| Core 0 active | ~56 ms |
| Core 1 active | < 1 ms |

### Core 0 Frame Pipeline

```
┌─────────────────────────────────────────────────────────────┐
│  GAME UPDATE PHASE (~10% of frame)                          │
│                                                             │
│  1. Submit OC to worker (core 1)                            │
│  2. CollisionCheck_AT()              ← main thread          │
│  3. Wait for OC worker               ← STALL if slow       │
│  4. CollisionCheck_Damage()          ← main thread          │
│  5. Actor_UpdateAll()                ← main thread          │
│     ├─ per actor: actor->update()                           │
│     ├─ per actor: Actor_UpdateBgCheckInfo() (BgCheck calls) │
│     └─ per actor: physics/position updates                  │
│  6. Submit effects to worker (core 1)                       │
│  7. Cutscene / Room / Skybox / Message / Interface updates  │
│  8. Wait for effects worker                                 │
├─────────────────────────────────────────────────────────────┤
│  DRAW PHASE (~84% of frame — THE BOTTLENECK)                │
│                                                             │
│  9. Play_DrawMain()                                         │
│     ├─ Actor_DrawAll() — matrix setup, SkelAnime, DL gen    │
│     └─ Environment / sky / effects drawing                  │
│ 10. Graph_ProcessGfxCommands()                              │
│     ├─ Frame interpolation replay                           │
│     └─ DrawAndRunGraphicsCommands() ← 84% OF FRAME TIME    │
└─────────────────────────────────────────────────────────────┘
```

---

## Core Allocation

| Core | Thread | Workload | Load |
|------|--------|----------|------|
| **0** | Main thread | Game logic, actor updates, collision AT, DL generation, rendering submission, frame interpolation | ~100% (bottleneck) |
| **1** | Worker pool | `CollisionCheck_OC`, effects updates | ~15–25% |
| **2** | Audio thread | `AudioMgr_CreateNextAudioBuffer` + playback | ~5–10% |
| **3** | Worker pool | Available for parallel tasks | ~0–15% |

> **Core 3 note**: The Switch OS may reserve core 3 for system tasks. Verify availability with `svcGetInfo` or by attempting `svcSetThreadCoreMask`. If unavailable, the worker pool falls back to core 1 only.

---

## Implemented Performance Tooling

### FrameProfiler with Phase Timing

**Files**: `mm/2s2h/DeveloperTools/FrameProfiler.cpp` / `.h`

Measures wall-clock time of each major phase per frame using `clock_gettime(CLOCK_MONOTONIC)`:

- Collision AT, Collision OC (worker), Actor Update, Actor Draw
- Effects Update (worker), Frame Interpolation, DL Process
- Total frame time, core active time, worker idle time
- On-screen ImGui overlay with percentage breakdown
- Automated bottleneck analysis

### DL Buffer Scanning

**Called from**: `graph.c` after buffers are finalized via `FrameProfiler_ScanAllBuffers()`

Scans all 5 display list buffers directly:

| Buffer | Contents |
|--------|----------|
| **OPA** (opaque) | Scene geometry, opaque actors (typically largest) |
| **XLU** (translucent) | Transparency effects, water, particles |
| **Overlay** | HUD, UI elements |
| **Work** | Setup/initialization commands |
| **Debug** | Debug display (usually empty) |

**Per-buffer metrics**: Command count, triangle count, vertex count, texture loads (`G_SETTIMG`), matrix loads (`G_MTX`), pipe syncs (`G_RDPPIPESYNC`), SetCombine (shader changes), DL subcalls.

**Derived cost metrics**:
- Cost per triangle (µs/tri)
- Cost per command (µs/cmd)
- Cost per draw call (µs/draw)
- Estimated draw calls per frame (≈ pipe sync count)

### Fast3D Internal Profiling Counters

**Integration**: `libultraship` exposes `Fast3DStats` via `Fast3dWindow`, toggled at runtime with `SetProfilingEnabled()`. Game-side code in `BenPort.cpp` reads and pushes stats into FrameProfiler counters.

```cpp
struct Fast3DStats {
    // Counts
    uint32_t drawCalls;          // actual DrawTriangles calls
    uint32_t textureBinds;       // glBindTexture calls
    uint32_t shaderSwitches;     // glUseProgram calls
    uint32_t shaderCompilations; // new shader variants compiled this frame
    uint32_t verticesSubmitted;  // total vertices sent to GL
    uint32_t trianglesSubmitted; // total triangles sent to GL
    uint32_t batchFlushes;       // vertex buffer flushes
    uint32_t textureCacheMisses; // cache misses requiring upload/decode
    uint32_t stateChangeFlushes; // flushes triggered by state changes

    // Timing (nanoseconds)
    uint64_t timeTotal;          // total Interpreter::Run time
    uint64_t timeGbiDispatch;    // GBI command parsing
    uint64_t timeVertexLoad;     // vertex transformation/load
    uint64_t timeTriProcessing;  // triangle assembly (GfxSpTri1)
    uint64_t timeTextureSetup;   // texture load/decode/upload
    uint64_t timeShaderSetup;    // shader compilation/binding
    uint64_t timeDrawSubmit;     // glDraw* calls

    // Derived
    float avgBatchSize;
    float usPerTriangle;
    float usPerDrawCall;
};
```

**API**:
```cpp
const Fast3DStats& Fast3dWindow::GetFrameStats() const;
void Fast3dWindow::ResetFrameStats();
void Fast3dWindow::SetProfilingEnabled(bool enabled);
```

### Snapshot Export

Export a full profiler snapshot (phase timings + DL buffer breakdown + Fast3D stats) for offline analysis via the Dev Tools UI.

---

## How to Use the Profiler

1. **Enable**: Open Dev Tools menu → Frame Profiler
2. **View overlay**: The on-screen overlay shows per-phase timing bars and percentage breakdown
3. **Enable Fast3D profiling**: The profiler automatically toggles `Fast3dWindow::SetProfilingEnabled()` when enabled
4. **Export snapshot**: Click "Export Snapshot" in the Frame Profiler window for a full data dump
5. **Analyze**: Check the "Per-Buffer Breakdown" section to see where geometry concentrates; check Fast3D counters for GL-level bottlenecks

**Overhead**: ~20 timing points per frame using `clock_gettime(CLOCK_MONOTONIC)`. Worst-case overhead is well under 0.1ms. Compile out entirely with `#ifdef ENABLE_PROFILER`.

---

## How to Read Profiler Output

### Phase Breakdown

The top-level timing shows where the frame budget goes:

```
Frame: 60.4ms (16.5 FPS)  Target: 16.6ms
──────────────────────────────
DL Process:    50.8ms  ██████████████████████████████ 84%
Play Draw:      3.5ms  ████░░░░░░░░░░░░░░░░░░░░░░░░░  6%
Frame Interp:   2.0ms  ███░░░░░░░░░░░░░░░░░░░░░░░░░░  3%
Play Update:    0.5ms  █░░░░░░░░░░░░░░░░░░░░░░░░░░░░  1%
```

### Per-Buffer Breakdown

| Pattern | Meaning | Action |
|---------|---------|--------|
| **OPA dominates** | Scene geometry is the bottleneck | Focus on scene mesh complexity, static DL caching |
| **XLU dominates** | Transparency overdraw is expensive | Reduce transparent effects/particles |
| **Overlay high** | HUD/UI is complex | Investigate unnecessary UI redraws |

### Cost Metric Interpretation

| Metric | Value | Diagnosis |
|--------|-------|-----------|
| µs/tri > 10 | Draw call overhead dominates | Too many small draw calls. Batch aggressively, reduce PipeSync/SetCombine count |
| µs/tri 3–10 | Mixed overhead | Both draw call overhead and vertex count matter |
| µs/tri < 3 | Vertex throughput limited | Reduce triangle count (LOD, draw distance) |
| µs/draw > 200 | Per-draw cost is high | Likely shader compile or texture upload stalls. Pre-warm shaders, improve texture caching |

### Fast3D Counters to Watch

| Counter | Healthy | Problematic |
|---------|---------|-------------|
| `shaderCompilations` | 0 (after warmup) | > 0 means runtime compilation stalls |
| `stateChangeFlushes` | Low relative to total flushes | High means excessive state thrashing |
| `avgBatchSize` | > 10 triangles/batch | < 5 means batching is ineffective |
| `textureCacheMisses` | 0–2 per frame | > 10 means texture cache is too small |

### What to Include in Performance Reports

1. Total frame time and FPS
2. DL Process time and percentage
3. Per-buffer command/triangle counts
4. Cost per triangle and per draw call
5. Number of SetCombine (shader changes) per frame
6. Fast3D counters (draw calls, batch flushes, shader switches)
7. Scene/area where measurement was taken

---

## Optimization Status

### ✅ DONE

| Item | Details |
|------|---------|
| **Frame Profiler** | Phase timing, on-screen overlay, automated bottleneck analysis, snapshot export |
| **DL Buffer Scanning** | Per-buffer breakdown of all 5 DL buffers (OPA/XLU/Overlay/Work/Debug) |
| **Fast3D Profiling** | Internal counters and timing in interpreter + OpenGL backend, game-side integration |
| **Worker Pool on Cores 1+3** | `TaskWorkerPool` with 2 threads pinned to cores 1 and 3 |
| **Parallel Collision** | AT on main thread, OC dispatched to worker thread |

### 🔧 IN PROGRESS

| Item | Details |
|------|---------|
| **Pixel-depth quantization** | Reduce per-pixel depth precision to improve fill rate |
| **MAX_TRI_BUFFER increase** | Larger triangle buffers to reduce flush frequency |
| **VAO-per-shader caching** | Cache vertex array objects per shader to avoid rebinding |
| **Container pre-allocation** | Pre-allocate STL containers to reduce allocation overhead |
| **Core affinity fixes** | Correct thread-to-core pinning for worker pool |

### 📋 PLANNED

| Priority | Item | Expected Impact |
|----------|------|-----------------|
| HIGH | **Render thread offload** | Move `DrawAndRunGraphicsCommands` to Core 1. Core 0 generates frame N+1 while Core 1 renders frame N. ~10ms savings. |
| HIGH | **Draw call batching** | Defer `glDrawArrays` until a state change actually requires it. Reduce draw calls from hundreds to tens. |
| HIGH | **Uber-shader** | Single shader with uniform-based combiner mode selection, eliminating `glUseProgram` switches. |
| HIGH | **Shader prewarm** | Pre-compile all shader variants at load time. Eliminates runtime compilation stalls. |
| MEDIUM | **VBO streaming** | Persistent mapped buffers to eliminate per-frame VBO uploads. |
| MEDIUM | **NEON texture paths** | Use ARM NEON SIMD for texture decode/conversion in the Fast3D interpreter. |
| MEDIUM | **Texture bind reduction** | Texture atlas and improved LRU cache to minimize `glBindTexture` calls. |
| MEDIUM | **DL caching** | Cache GL command sequences for static scene geometry. Skip re-interpretation on subsequent frames. |
| LOW | **Draw distance CVar** | Cull distant actors before DL generation. |
| LOW | **Parallel BgCheck** | Batch actor BgCheck queries and dispatch across worker pool (read-only, safe to parallelize). |
| LOW | **Async frame interpolation** | Split interpolation tree into segments for parallel processing. |

---

## Implementation Priority

```
Phase 1: Instrumentation                              ✅ COMPLETE
  ├─ FrameProfiler with phase timing
  ├─ On-screen overlay (ImGui)
  ├─ DL buffer scanning (OPA/XLU/Overlay/Work/Debug)
  ├─ Fast3D internal profiling counters
  ├─ Snapshot export with analysis
  └─ Game-side Fast3D integration (BenPort.cpp)

Phase 2: Core Utilization                              ✅ COMPLETE
  ├─ TaskWorkerPool on cores 1+3
  └─ Parallel collision (AT main, OC worker)

Phase 3: Fast3D Interpreter Optimization               ← CURRENT FOCUS
  ├─ Draw call batching (reduce GL driver overhead)
  ├─ Shader prewarm (eliminate runtime compilation)
  ├─ Uber-shader (eliminate glUseProgram switches)
  ├─ Texture bind reduction (atlas, improved cache)
  └─ DL caching for static scene geometry

Phase 4: Render Thread Offload
  ├─ Move DrawAndRunGraphicsCommands to Core 1
  ├─ Double-buffer display lists (gGfxPools[0]/[1] already exist)
  ├─ GL context binding on render thread
  └─ Frame pacing and synchronization

Phase 5: Game Logic Optimization (low priority — only ~6ms)
  ├─ Batch BgCheck queries in Actor_UpdateAll
  ├─ Parallel skeleton/matrix prep on worker pool
  └─ DL complexity reduction (draw distance, LOD CVars)
```

---

## NX-Specific Characteristics

### ARM Cortex-A57

- **Branch prediction**: Modest predictor. The GBI command dispatch `switch()` may suffer mispredictions. Consider computed goto or function pointer table.
- **Cache pressure**: 32 KB L1D per core. Large DL buffers (OPA is ~209 KB) exceed L1. Streaming access helps but DL subcall jumps may thrash cache.
- **NEON**: 128-bit SIMD, already used for matrix ops. Could extend to vertex batch processing and texture decode.

### Maxwell GPU

- **Fill rate**: Not an issue at 720p with 30% utilization.
- **Draw call overhead**: NVN has lower overhead than desktop GL, but still significant at hundreds of calls/frame.
- **Shader compilation**: Very expensive on Maxwell. Must pre-warm all variants at load time.
- **Texture upload**: LPDDR4 bandwidth is shared CPU/GPU. Large uploads compete with CPU memory access.

### Thermal

- Sustained 100% load on Core 0 triggers throttling after minutes of gameplay.
- Distributing work across cores reduces per-core thermal load.

---

## Success Criteria

| Metric | Current | Phase 3 Target | Phase 4 Target | Final Target |
|--------|---------|----------------|----------------|-------------|
| Frame time | 60.4 ms | 40–50 ms | 30–40 ms | < 16.6 ms |
| FPS | 16.5 | 20–25 | 25–33 | 60 |
| DL Process | ~51 ms | 30–40 ms | 20–30 ms | < 10 ms |
| Core 1 util | ~0% | ~0% | 50%+ | 50%+ |
| GPU util | ~30% | 40%+ | 60%+ | 80%+ |
