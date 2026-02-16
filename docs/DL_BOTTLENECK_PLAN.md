# Display List Processing Bottleneck: Analysis & Fix Plan

## Executive Summary

Profiler data confirms the **display list interpreter** (`DrawAndRunGraphicsCommands` in libultraship's Fast3D backend) consumes **48.7ms (84%) of each 57.8ms frame** on Switch. GPU utilization is only ~30%, meaning the CPU cannot feed GL commands fast enough. This is the dominant bottleneck preventing 60 FPS.

---

## 1. What We Know

### Measured Data (from Frame Profiler)

| Metric | Value |
|--------|-------|
| Total frame time | 57.8 ms (17.3 FPS) |
| DL Process time | 48.7 ms (84.2% of frame) |
| Game logic total | ~5.6 ms (9.7% of frame) |
| Frame interpolation | 1.9 ms (3.4%) |
| DL iterations/frame | 1.0 |
| GPU utilization | ~30% |
| Core 0 active | 53.6 ms |
| Core 1 active | 0.01 ms |

### What DL Process Does

`DrawAndRunGraphicsCommands` in libultraship performs:

1. **GBI Command Walking** — Parse each 8-byte N64 display list command
2. **Vertex Processing** — Transform vertices through the RSP pipeline (lighting, clip testing)
3. **Triangle Assembly** — Build triangles from vertex indices
4. **Texture State** — Load/decode textures, manage TMEM simulation
5. **Shader Compilation** — Map N64 RDP combiner modes to GLSL shaders (compile on first use, then cache)
6. **Draw Call Submission** — Flush vertex batches to OpenGL as `glDrawArrays`
7. **State Changes** — `glBindTexture`, `glUseProgram`, `glUniform*`, blend/depth/cull state
8. **Buffer Management** — VBO uploads, framebuffer operations

### What We Don't Know Yet

We can currently measure the **total** DL processing time and count top-level GBI commands, but we cannot yet break down **where inside** the 48.7ms the time is spent. Specifically:

- How much time is in GBI command dispatch vs. GL API calls?
- How many GL draw calls per frame? (not GBI TRI commands — the batched GL calls)
- How many shader compilations per frame? (should be 0 after warmup)
- How much time in texture decode/upload?
- What is the average batch size (triangles per draw call)?
- Is the GL driver stalling (implicit sync)?

---

## 2. Detection Plan (Profiling Inside the Bottleneck)

### 2a. Game-Side DL Statistics ✅ IMPLEMENTED

The Frame Profiler now counts GBI commands by type via `FrameProfiler_ScanDisplayList()`:

- Top-level command count (total GBI commands in the root display list)
- Triangle count (`G_TRI1` + 2×`G_TRI2`)
- Vertex count (from `G_VTX` command fields)
- Texture loads (`G_SETTIMG` — texture source changes)
- Matrix loads (`G_MTX` — transform changes)
- Pipe syncs (`G_RDPPIPESYNC` — state barriers, force draw call flush)
- DL subcalls (`G_DL` — nested display list references)
- SetCombine (`G_SETCOMBINE` — shader/combiner mode changes)
- Cost per triangle and per command (us/tri, us/cmd)

**Usage**: Open Dev Tools → Frame Profiler and export a snapshot to see all stats.

### 2b. libultraship Fast3D Profiling (REQUIRES SUBMODULE CHANGES)

To break down the 48.7ms further, we need instrumentation **inside** the Fast3D interpreter. These changes would go in the `libultraship` submodule:

#### Proposed Counters (add to interpreter state)

```cpp
struct Fast3DStats {
    // Counts
    uint32_t glDrawCalls;        // actual glDrawArrays/glDrawElements calls
    uint32_t glTextureBinds;     // glBindTexture calls
    uint32_t glShaderSwitches;   // glUseProgram calls
    uint32_t shaderCompilations; // new shader variants compiled this frame
    uint32_t verticesSubmitted;  // total vertices sent to GL
    uint32_t trianglesSubmitted; // total triangles sent to GL
    uint32_t batchFlushes;       // number of vertex buffer flushes

    // Timing (nanoseconds)
    uint64_t timeGbiDispatch;    // time in GBI command parsing
    uint64_t timeVertexProcess;  // time in vertex transformation
    uint64_t timeTextureSetup;   // time in texture load/decode/upload
    uint64_t timeShaderSetup;    // time in shader compilation/binding
    uint64_t timeDrawSubmit;     // time in glDraw* calls
    uint64_t timeStateChanges;   // time in other GL state changes
};
```

#### Where to Instrument (libultraship paths)

| Component | File | What to Measure |
|-----------|------|-----------------|
| GBI dispatch | `src/fast/interpreter.cpp` | Time per command category |
| Vertex transform | `src/fast/interpreter.cpp` (G_VTX handler) | Per-vertex cost |
| Triangle assembly | `src/fast/interpreter.cpp` (G_TRI1/G_TRI2) | Batch size at flush |
| Texture setup | `src/fast/interpreter.cpp` (G_SETTIMG/G_LOADBLOCK) | Texture decode+upload time |
| Shader compile | `src/fast/backends/gfx_opengl.cpp` (create_program) | Compilation count + time |
| Draw call | `src/fast/backends/gfx_opengl.cpp` (draw_triangles) | Per-draw overhead |
| State changes | `src/fast/backends/gfx_opengl.cpp` | GL bind/uniform calls |

#### Integration with Frame Profiler

The Fast3D stats struct would be exposed via a getter method on `Fast3dWindow`:

```cpp
// In libultraship
const Fast3DStats& Fast3dWindow::GetFrameStats() const;
void Fast3dWindow::ResetFrameStats();

// In game code (BenPort.cpp)
auto& stats = wnd->GetFrameStats();
FrameProfiler_AddCounter(PROFILE_COUNTER_GL_DRAW_CALLS, stats.glDrawCalls);
// ... etc
wnd->ResetFrameStats();
```

---

## 3. Optimization Strategies

Based on what we know about N64 rendering characteristics and the Fast3D architecture:

### Strategy A: Reduce Draw Call Count (HIGH IMPACT)

**Problem**: N64 games submit many small triangle batches. Each `G_RDPPIPESYNC` forces a draw call flush. MM has complex scenes with many actors, each drawing a few triangles.

**Solution**: Batch aggressively in the interpreter. Defer `glDrawArrays` until a state change actually requires it (not on every PipeSync).

**Expected impact**: Reducing draw calls from hundreds to tens would significantly cut GL driver overhead.

**Where**: `libultraship/src/fast/interpreter.cpp` (flush logic), `gfx_opengl.cpp` (draw_triangles)

### Strategy B: Reduce Shader Switches (HIGH IMPACT)

**Problem**: Each unique N64 combiner mode (`G_SETCOMBINE`) maps to a different GLSL shader. Switching shaders (`glUseProgram`) is expensive.

**Solution**:
1. Sort draw calls by shader (deferred rendering)
2. Use uber-shader with uniform-based mode selection instead of separate shader programs
3. Pre-warm all shader variants at load time

**Where**: `libultraship/src/fast/backends/gfx_opengl.cpp` (shader cache)

### Strategy C: Reduce Texture Binds (MEDIUM IMPACT)

**Problem**: Each `G_SETTIMG` may trigger a texture upload or rebind.

**Solution**:
1. Texture atlas — combine small textures into larger atlas textures
2. Texture cache with LRU — avoid redundant uploads
3. Persistent texture objects — don't recreate GL textures each frame

**Where**: `libultraship/src/fast/interpreter.cpp` (texture handling), `gfx_opengl.cpp` (texture management)

### Strategy D: Render Thread Offload (HIGH IMPACT — architectural)

**Problem**: DL processing (49ms) runs on Core 0, blocking game logic for the next frame.

**Solution**: Move `DrawAndRunGraphicsCommands` to a dedicated render thread on Core 1. Core 0 generates frame N+1's display list while Core 1 renders frame N.

```
Frame N:    [Core 0: Game Logic 9ms][    Core 0: idle    ]
            [Core 1: DL Process --------49ms---------]

Frame N+1:  [Core 0: Game Logic 9ms][    Core 0: idle    ]
                                    [Core 1: DL Process -49ms-]
```

The double-buffer infrastructure already exists (`gGfxPools[0]`/`gGfxPools[1]`).

**Expected impact**: Frame time drops from 58ms to ~49ms (9ms game logic runs in parallel). That's 17.3 FPS → ~20.4 FPS. The real win is when combined with Strategy A-C reducing DL time below 16.6ms.

**Where**: `mm/2s2h/BenPort.cpp` (Graph_ProcessGfxCommands), new render thread infrastructure

### Strategy E: Display List Caching (MEDIUM IMPACT)

**Problem**: Static scene geometry (rooms, floors, walls) generates identical display lists every frame, but the interpreter re-walks them.

**Solution**: Cache the GL command sequence for static DL segments. On subsequent frames, replay the cached GL commands directly without re-interpreting GBI.

**Where**: `libultraship/src/fast/interpreter.cpp` (add caching layer)

### Strategy F: DL Complexity Reduction — Game Side (LOW IMPACT but easy)

**Problem**: Some scenes have high triangle counts that overwhelm the interpreter.

**Solution**: Add CVars to control:
- Draw distance (cull distant actors before they enter the DL)
- LOD bias (force lower detail models)
- Effect particle limits

**Where**: `mm/src/code/z_actor.c` (actor culling), various enhancement files

---

## 4. Implementation Priority

```
IMMEDIATE (game-side, this repo):
  ✅ DL command counting and cost metrics
  → Strategy F: Draw distance CVar
  → Strategy D: Render thread prototype

REQUIRES LIBULTRASHIP CHANGES:
  → Strategy A: Draw call batching in Fast3D interpreter
  → Strategy B: Uber-shader or shader sorting
  → Strategy C: Texture bind optimization
  → Strategy E: DL caching for static geometry
  → Fast3D internal profiling (Section 2b counters)
```

---

## 5. Success Criteria

| Metric | Current | Target (Phase 1) | Target (Phase 2) |
|--------|---------|-------------------|-------------------|
| Frame time | 57.8 ms | 40-50 ms | <16.6 ms |
| FPS | 17.3 | 20-25 | 60 |
| DL Process | 48.7 ms | 30-40 ms | <10 ms |
| Core 1 utilization | 0% | 50%+ | 50%+ |
| GPU utilization | 30% | 50%+ | 80%+ |

**Phase 1** = render thread + basic batching improvements
**Phase 2** = full interpreter optimization (uber-shader, DL caching, aggressive batching)
