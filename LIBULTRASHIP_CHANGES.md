# libultraship Submodule — Pending Changes

> **These changes need to be applied to the
> [JohnDeved/libultraship](https://github.com/JohnDeved/libultraship) fork
> (`perf/nx-high-impact` branch) on top of commit `f16f4ee`.**
>
> The submodule pointer in this repo has been reverted to `f16f4ee` (the last
> reachable commit on `perf/nx-high-impact`).  Once these changes are applied
> to the fork, update the submodule ref to the new commit hash.

---

## Overview

Move **fog color**, **fog factor computation**, and **grayscale color** from
per-vertex VBO attributes to GPU uniforms.  This removes **8 floats per vertex**
(24 floats per triangle) from the VBO and eliminates per-vertex CPU math
(1/w division, `fabsf`, clamp, 7 byte-to-float divisions).

### Impact per triangle

| Data                | Before (per-vertex attribute) | After (uniform)                  |
|---------------------|-------------------------------|----------------------------------|
| Fog RGB (3 floats)  | 9 floats/tri + 9 divisions    | 1 `glUniform3fv` per draw change |
| Fog factor (1 float)| 3 floats/tri + 3× `1/w` div   | Computed in vertex shader        |
| Grayscale (4 floats)| 12 floats/tri + 12 divisions  | 1 `glUniform4fv` per draw change |
| **Total saved**     | **24 floats/tri = 96 bytes**  |                                  |

---

## Changes by File

### 1. `include/fast/backends/gfx_opengl.h`

Add fog/grayscale uniform locations and dirty-tracking state to `ShaderProgram`
and `GfxRenderingAPIOGL`.

```diff
 struct ShaderProgram {
     GLuint openglProgramId;
     uint8_t numInputs;
     bool usedTextures[SHADER_MAX_TEXTURES];
     uint8_t numFloats;
     GLint attribLocations[16];
     uint8_t attribSizes[16];
     uint8_t numAttribs;
     GLint frameCountLocation;
     GLint noiseScaleLocation;
     GLint texture_width_location;
     GLint texture_height_location;
     GLint texture_filtering_location;
+    GLint fog_color_location;
+    GLint fog_mul_location;
+    GLint fog_offset_location;
+    GLint grayscale_color_location;
+    bool hasFog;
+    bool hasGrayscale;
 };
```

Add dirty-tracking fields to `GfxRenderingAPIOGL`:

```diff
     // Dirty-flag tracking for per-draw uniforms to skip redundant glUniform calls.
     GLint mLastPerDrawFiltering[2] = { -1, -1 };
     GLint mLastPerDrawWidth[2] = { -1, -1 };
     GLint mLastPerDrawHeight[2] = { -1, -1 };
+
+    // Fog/grayscale uniform dirty tracking
+    float mLastFogColor[3] = { -1.0f, -1.0f, -1.0f };
+    float mLastFogMul = -99999.0f;
+    float mLastFogOffset = -99999.0f;
+    float mLastGrayscaleColor[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
 };
```

---

### 2. `include/fast/backends/gfx_rendering_api.h`

Add virtual methods for setting fog/grayscale parameters from the interpreter:

```diff
     virtual void SetUseAlpha(bool useAlpha) = 0;
     virtual void DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) = 0;
+    virtual void SetFogParams(float r, float g, float b, float mul, float offset) = 0;
+    virtual void SetGrayscaleColor(float r, float g, float b, float a) = 0;
     virtual void Init() = 0;
```

---

### 3. `src/fast/backends/gfx_opengl.cpp`

#### 3a. `CreateAndLoadNewShader` — query new uniform locations

After the existing `texture_filtering_location` query, add:

```diff
     prg->texture_filtering_location = glGetUniformLocation(shader_program, "texture_filtering");

+    prg->fog_color_location = glGetUniformLocation(shader_program, "uFogColor");
+    prg->fog_mul_location = glGetUniformLocation(shader_program, "uFogMul");
+    prg->fog_offset_location = glGetUniformLocation(shader_program, "uFogOffset");
+    prg->grayscale_color_location = glGetUniformLocation(shader_program, "uGrayscaleColor");
+    prg->hasFog = cc_features.opt_fog;
+    prg->hasGrayscale = cc_features.opt_grayscale;

     LoadShader(prg);
```

#### 3b. `CreateAndLoadNewShader` — remove fog/grayscale vertex attributes

Remove the fog and grayscale attribute registration from the vertex attribute
setup loop:

```diff
-    if (cc_features.opt_fog) {
-        prg->attribLocations[cnt] = glGetAttribLocation(shader_program, "aFog");
-        prg->attribSizes[cnt] = 4;
-        ++cnt;
-    }
-
-    if (cc_features.opt_grayscale) {
-        prg->attribLocations[cnt] = glGetAttribLocation(shader_program, "aGrayscaleColor");
-        prg->attribSizes[cnt] = 4;
-        ++cnt;
-    }
```

#### 3c. `LoadShader` — invalidate fog/grayscale dirty tracking

In `LoadShader`, add invalidation alongside the existing per-draw cache reset:

```diff
     // Invalidate per-draw uniform cache when switching shaders
     mLastPerDrawFiltering[0] = mLastPerDrawFiltering[1] = -1;
     mLastPerDrawWidth[0] = mLastPerDrawWidth[1] = -1;
     mLastPerDrawHeight[0] = mLastPerDrawHeight[1] = -1;
+    mLastFogColor[0] = mLastFogColor[1] = mLastFogColor[2] = -1.0f;
+    mLastFogMul = -99999.0f;
+    mLastFogOffset = -99999.0f;
+    mLastGrayscaleColor[0] = mLastGrayscaleColor[1] = mLastGrayscaleColor[2] = mLastGrayscaleColor[3] = -1.0f;
```

#### 3d. Add `SetFogParams` and `SetGrayscaleColor` implementations

```cpp
void GfxRenderingAPIOGL::SetFogParams(float r, float g, float b, float mul, float offset) {
    if (mCurrentShaderProgram && mCurrentShaderProgram->hasFog) {
        if (r != mLastFogColor[0] || g != mLastFogColor[1] || b != mLastFogColor[2]) {
            float color[3] = { r, g, b };
            glUniform3fv(mCurrentShaderProgram->fog_color_location, 1, color);
            mLastFogColor[0] = r;
            mLastFogColor[1] = g;
            mLastFogColor[2] = b;
        }
        if (mul != mLastFogMul) {
            glUniform1f(mCurrentShaderProgram->fog_mul_location, mul);
            mLastFogMul = mul;
        }
        if (offset != mLastFogOffset) {
            glUniform1f(mCurrentShaderProgram->fog_offset_location, offset);
            mLastFogOffset = offset;
        }
    }
}

void GfxRenderingAPIOGL::SetGrayscaleColor(float r, float g, float b, float a) {
    if (mCurrentShaderProgram && mCurrentShaderProgram->hasGrayscale) {
        if (r != mLastGrayscaleColor[0] || g != mLastGrayscaleColor[1] ||
            b != mLastGrayscaleColor[2] || a != mLastGrayscaleColor[3]) {
            float color[4] = { r, g, b, a };
            glUniform4fv(mCurrentShaderProgram->grayscale_color_location, 1, color);
            mLastGrayscaleColor[0] = r;
            mLastGrayscaleColor[1] = g;
            mLastGrayscaleColor[2] = b;
            mLastGrayscaleColor[3] = a;
        }
    }
}
```

---

### 4. `src/fast/interpreter.cpp`

#### 4a. Remove fog factor CPU computation in `GfxSpVertex`

The fog factor (`z * winv * fog_mul + fog_offset`) is no longer computed on the
CPU.  The vertex shader will compute it from `aVtxPos.z / aVtxPos.w`.

```diff
-            float winv = 1.0f / w;
-            if (winv < 0.0f) {
-                winv = std::numeric_limits<int16_t>::max();
-            }
-
-            float fog_z = z * winv * mRsp->fog_mul + mRsp->fog_offset;
-            fog_z = Ship::Math::clamp(fog_z, 0.0f, 255.0f);
-            d->color.a = fog_z; // Use alpha variable to store fog factor
+            // Fog factor is now computed in the vertex shader from z/w + uniforms
+            d->color.a = v->cn[3];
```

Note: `d->color.a` must still be set to `v->cn[3]` (the original vertex alpha)
since it's used elsewhere.  The `if (mRsp->geometry_mode & G_FOG)` branch
should fall through to the `else` path (or be removed entirely).

#### 4b. Remove fog/grayscale VBO packing in `GfxSpTri1`

Remove the per-vertex fog color and grayscale color writes from the VBO packing
loop:

```diff
-        if (use_fog) {
-            mBufVbo[mBufVboLen++] = mRdp->fog_color.r / 255.0f;
-            mBufVbo[mBufVboLen++] = mRdp->fog_color.g / 255.0f;
-            mBufVbo[mBufVboLen++] = mRdp->fog_color.b / 255.0f;
-            mBufVbo[mBufVboLen++] = v_arr[i]->color.a / 255.0f;
-        }
-
-        if (use_grayscale) {
-            mBufVbo[mBufVboLen++] = mRdp->grayscale_color.r / 255.0f;
-            mBufVbo[mBufVboLen++] = mRdp->grayscale_color.g / 255.0f;
-            mBufVbo[mBufVboLen++] = mRdp->grayscale_color.b / 255.0f;
-            mBufVbo[mBufVboLen++] = mRdp->grayscale_color.a / 255.0f;
-        }
```

#### 4c. Set fog/grayscale uniforms before DrawTriangles

In `GfxSpTri1`, before the call to `mRenderingApi->DrawTriangles(...)`, add
uniform upload calls:

```diff
+    if (use_fog) {
+        mRenderingApi->SetFogParams(
+            mRdp->fog_color.r / 255.0f,
+            mRdp->fog_color.g / 255.0f,
+            mRdp->fog_color.b / 255.0f,
+            (float)mRsp->fog_mul,
+            (float)mRsp->fog_offset
+        );
+    }
+    if (use_grayscale) {
+        mRenderingApi->SetGrayscaleColor(
+            mRdp->grayscale_color.r / 255.0f,
+            mRdp->grayscale_color.g / 255.0f,
+            mRdp->grayscale_color.b / 255.0f,
+            mRdp->grayscale_color.a / 255.0f
+        );
+    }
     mRenderingApi->DrawTriangles(mBufVbo, mBufVboLen, numTris);
```

#### 4d. Remove fog `cc_options` numFloats accounting

In the shader feature calculation, fog and grayscale no longer contribute to
`numFloats` (they're uniforms now, not attributes).  The `update_floats`
directives are removed from the vertex shader template, so the C++ side must
not count them either.  Search for where `numFloats` is incremented for fog
and grayscale in `GfxSpTri1` and remove those increments.

---

### 5. `src/fast/shaders/opengl/default.shader.vs`

Replace per-vertex fog/grayscale attributes with uniforms and GPU-side fog
computation:

```diff
 @if(o_fog)
-    @{attr} vec4 aFog;
-    @{out} vec4 vFog;
-    @{update_floats(4)}
-@end
-
-@if(o_grayscale)
-    @{attr} vec4 aGrayscaleColor;
-    @{out} vec4 vGrayscaleColor;
-    @{update_floats(4)}
+    @{out} float vFogFactor;
+    uniform float uFogMul;
+    uniform float uFogOffset;
 @end
```

In `main()`:

```diff
     @if(o_fog)
-        vFog = aFog;
-    @end
-    @if(o_grayscale)
-        vGrayscaleColor = aGrayscaleColor;
+        if (aVtxPos.w > 0.001) {
+            vFogFactor = clamp(aVtxPos.z / aVtxPos.w * uFogMul + uFogOffset, 0.0, 255.0) / 255.0;
+        } else {
+            vFogFactor = 1.0;
+        }
     @end
```

---

### 6. `src/fast/shaders/opengl/default.shader.fs`

Replace per-vertex fog/grayscale varyings with uniforms:

```diff
-@if(o_fog) @{attr} vec4 vFog;
-@if(o_grayscale) @{attr} vec4 vGrayscaleColor;
+@if(o_fog) @{attr} float vFogFactor;
+@if(o_fog) uniform vec3 uFogColor;
+@if(o_grayscale) uniform vec4 uGrayscaleColor;
```

In `main()` fog mixing:

```diff
     @if(o_fog)
         @if(o_alpha)
-            texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);
+            texel = vec4(mix(texel.rgb, uFogColor, vFogFactor), texel.a);
         @else
-            texel = mix(texel, vFog.rgb, vFog.a);
+            texel = mix(texel, uFogColor, vFogFactor);
         @end
     @end
```

In `main()` grayscale mixing:

```diff
     @if(o_grayscale)
         float intensity = (texel.r + texel.g + texel.b) / 3.0;
-        vec3 new_texel = vGrayscaleColor.rgb * intensity;
-        texel.rgb = mix(texel.rgb, new_texel, vGrayscaleColor.a);
+        vec3 new_texel = uGrayscaleColor.rgb * intensity;
+        texel.rgb = mix(texel.rgb, new_texel, uGrayscaleColor.a);
     @end
```

---

## Verification Checklist

After applying these changes:

- [ ] Fog renders correctly (Termina Field, Southern Swamp fog areas)
- [ ] Fog color transitions are smooth (no per-vertex banding)
- [ ] Grayscale effect works (Song of Healing, Stone Tower)
- [ ] No visual regressions in scenes without fog/grayscale
- [ ] Shader compilation succeeds (all combiner variations)
- [ ] No OpenGL errors logged on Switch (check `glGetError`)
- [ ] VBO size per draw call is measurably smaller (8 floats/vertex less when fog+grayscale active)

---

## Notes

- The fog factor range is `[0, 255]` in integer N64 convention, divided by 255
  in the shader to get `[0.0, 1.0]`.  `uFogMul` and `uFogOffset` are passed
  as raw `int16_t`-origin floats (cast from `mRsp->fog_mul` and
  `mRsp->fog_offset`).
- The `aVtxPos.w > 0.001` guard in the vertex shader prevents division by zero
  for degenerate vertices; the fallback `1.0` means "fully fogged" which is
  the safe default for behind-camera geometry.
- The game's custom shaders (`mm/assets/custom/shaders/opengl/`) have already
  been updated in this repo to match the new uniform-based approach.
