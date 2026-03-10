// Fast3D deko3d uber-shader: fragment stage
// Compiled to DKSH via uam at build time.
//
// Implements the N64 RDP color combiner via uniform-driven branching.
// A single shader pair handles all combiner configurations; the backend
// uploads per-draw UberUniforms to select the active code paths.
//
// Layout must match the UberUniforms struct in gfx_deko3d.h.

#version 460

layout(location = 0) in vec2 vTexCoord0;
layout(location = 1) in float vTexClampS0;
layout(location = 2) in float vTexClampT0;
layout(location = 3) in vec2 vTexCoord1;
layout(location = 4) in float vTexClampS1;
layout(location = 5) in float vTexClampT1;
layout(location = 6) in vec4 vFog;
layout(location = 7) in vec4 vGrayscaleColor;
layout(location = 8) in vec4 vInput1;
layout(location = 9) in vec4 vInput2;
layout(location = 10) in vec4 vInput3;
layout(location = 11) in vec4 vInput4;

layout(location = 0) out vec4 outColor;

// Texture samplers — bound per draw call
layout(binding = 0) uniform sampler2D uTex0;
layout(binding = 1) uniform sampler2D uTex1;
layout(binding = 2) uniform sampler2D uTexMask0;
layout(binding = 3) uniform sampler2D uTexMask1;
layout(binding = 4) uniform sampler2D uTexBlend0;
layout(binding = 5) uniform sampler2D uTexBlend1;

// Uniform buffer — must match UberUniforms struct layout
layout(std140, binding = 0) uniform UberBlock {
    ivec4 cc_0_0;       // c[0][0] (cycle 0, RGB terms a,b,c,d)
    ivec4 cc_0_1;       // c[0][1] (cycle 0, Alpha terms)
    ivec4 cc_1_0;       // c[1][0] (cycle 1, RGB terms)
    ivec4 cc_1_1;       // c[1][1] (cycle 1, Alpha terms)

    ivec2 useTexture;   // useTexture[0], useTexture[1]
    ivec2 useMask;      // useMask[0], useMask[1]
    ivec2 useBlend;     // useBlend[0], useBlend[1]

    // useClamp[tex][s/t] — packed as ivec4: [0].s, [0].t, [1].s, [1].t
    ivec4 useClamp;

    int optAlpha;
    int optFog;
    int optNoise;
    int opt2Cyc;
    int optTextureEdge;
    int optAlphaThreshold;
    int optInvisible;
    int optGrayscale;

    // doSingle/doMultiply/doMix packed as ivec4: [cyc0_rgb, cyc0_a, cyc1_rgb, cyc1_a]
    ivec4 doSingle;
    ivec4 doMultiply;
    ivec4 doMix;

    ivec2 colorAlphaSame;

    ivec2 textureFiltering;
    ivec2 textureWidth;
    ivec2 textureHeight;

    int frameCount;
    float noiseScale;
    int srgbMode;
    int pad0;
};

// --- Noise generation ---
float random(vec3 value) {
    float dotVal = dot(value, vec3(12.9898, 78.233, 37.719));
    float noise = fract(sin(dotVal) * 143758.5453);
    return noise;
}

float randNoise() {
    return floor(random(vec3(gl_FragCoord.xy, float(frameCount))) + 0.5);
}

vec4 fromLinear(vec4 linearRGB){
    bvec3 cutoff = lessThan(linearRGB.rgb, vec3(0.0031308));
    vec3 higher = vec3(1.055) * pow(linearRGB.rgb, vec3(1.0 / 2.4)) - vec3(0.055);
    vec3 lower = linearRGB.rgb * vec3(12.92);
    return vec4(mix(higher, lower, cutoff), linearRGB.a);
}

vec4 sampleByUnit(int unit, vec2 uv) {
    if (unit == 0) return texture(uTex0, uv);
    if (unit == 1) return texture(uTex1, uv);
    if (unit == 2) return texture(uTexMask0, uv);
    if (unit == 3) return texture(uTexMask1, uv);
    if (unit == 4) return texture(uTexBlend0, uv);
    if (unit == 5) return texture(uTexBlend1, uv);
    return vec4(0.0);
}

vec4 filter3point(int unit, vec2 texCoord, vec2 texSize) {
    vec2 offset = fract(texCoord * texSize - vec2(0.5));
    offset -= step(1.0, offset.x + offset.y);
    vec4 c0 = sampleByUnit(unit, texCoord - offset / texSize);
    vec4 c1 = sampleByUnit(unit, texCoord - vec2(offset.x - sign(offset.x), offset.y) / texSize);
    vec4 c2 = sampleByUnit(unit, texCoord - vec2(offset.x, offset.y - sign(offset.y)) / texSize);
    return c0 + abs(offset.x) * (c1 - c0) + abs(offset.y) * (c2 - c0);
}

vec4 hookTexture2D(int id, int unit, vec2 uv, vec2 texSize) {
    if (id >= 0 && id < 2 && textureFiltering[id] == 0) { // FILTER_THREE_POINT
        return filter3point(unit, uv, texSize);
    }
    return sampleByUnit(unit, uv);
}

// --- Color combiner input lookup ---
// Maps a SHADER_* constant to a vec4 value.
// with_alpha: whether to return vec4 (true) or vec3 padded to vec4 (false)
// only_alpha: whether to return only the alpha channel as a scalar in .x
// first_cycle: swaps texel0/texel1 for second cycle
vec4 getCCInput(int item, bool with_alpha, bool only_alpha, bool first_cycle,
                vec4 texVal0, vec4 texVal1, vec4 texel) {
    // SHADER_0 = 0
    if (item == 0) {
        return vec4(0.0);
    }
    // SHADER_INPUT_1..4 = 1..4
    if (item == 1) return only_alpha ? vec4(vInput1.a) : (with_alpha ? vInput1 : vec4(vInput1.rgb, 1.0));
    if (item == 2) return only_alpha ? vec4(vInput2.a) : (with_alpha ? vInput2 : vec4(vInput2.rgb, 1.0));
    if (item == 3) return only_alpha ? vec4(vInput3.a) : (with_alpha ? vInput3 : vec4(vInput3.rgb, 1.0));
    if (item == 4) return only_alpha ? vec4(vInput4.a) : (with_alpha ? vInput4 : vec4(vInput4.rgb, 1.0));
    // SHADER_INPUT_5..7 = 5..7 (unused, treat as zero)
    if (item >= 5 && item <= 7) return vec4(0.0);
    // SHADER_TEXEL0 = 8
    if (item == 8) {
        vec4 t = first_cycle ? texVal0 : texVal1;
        return only_alpha ? vec4(t.a) : (with_alpha ? t : vec4(t.rgb, 1.0));
    }
    // SHADER_TEXEL0A = 9
    if (item == 9) {
        float a = first_cycle ? texVal0.a : texVal1.a;
        return only_alpha ? vec4(a) : vec4(a, a, a, a);
    }
    // SHADER_TEXEL1 = 10
    if (item == 10) {
        vec4 t = first_cycle ? texVal1 : texVal0;
        return only_alpha ? vec4(t.a) : (with_alpha ? t : vec4(t.rgb, 1.0));
    }
    // SHADER_TEXEL1A = 11
    if (item == 11) {
        float a = first_cycle ? texVal1.a : texVal0.a;
        return only_alpha ? vec4(a) : vec4(a, a, a, a);
    }
    // SHADER_1 = 12
    if (item == 12) return vec4(1.0);
    // SHADER_COMBINED = 13
    if (item == 13) {
        return only_alpha ? vec4(texel.a) : (with_alpha ? texel : vec4(texel.rgb, 1.0));
    }
    // SHADER_NOISE = 14
    if (item == 14) {
        float n = randNoise();
        return vec4(n, n, n, n);
    }

    return vec4(0.0);
}

// Compute one cycle of the color combiner for RGB
vec3 combineRGB(ivec4 cc, bool doS, bool doMul, bool doMx,
                bool withAlpha, bool firstCycle,
                vec4 texVal0, vec4 texVal1, vec4 texel) {
    vec4 a = getCCInput(cc.x, withAlpha, false, firstCycle, texVal0, texVal1, texel);
    vec4 b = getCCInput(cc.y, withAlpha, false, firstCycle, texVal0, texVal1, texel);
    vec4 c = getCCInput(cc.z, withAlpha, false, firstCycle, texVal0, texVal1, texel);
    vec4 d = getCCInput(cc.w, withAlpha, false, firstCycle, texVal0, texVal1, texel);

    if (doS) {
        return d.rgb;
    } else if (doMul) {
        return a.rgb * c.rgb;
    } else if (doMx) {
        return mix(b.rgb, a.rgb, c.rgb);
    } else {
        return (a.rgb - b.rgb) * c.rgb + d.rgb;
    }
}

// Compute one cycle of the color combiner for Alpha
float combineAlpha(ivec4 cc, bool doS, bool doMul, bool doMx,
                   bool firstCycle,
                   vec4 texVal0, vec4 texVal1, vec4 texel) {
    vec4 a = getCCInput(cc.x, true, true, firstCycle, texVal0, texVal1, texel);
    vec4 b = getCCInput(cc.y, true, true, firstCycle, texVal0, texVal1, texel);
    vec4 c = getCCInput(cc.z, true, true, firstCycle, texVal0, texVal1, texel);
    vec4 d = getCCInput(cc.w, true, true, firstCycle, texVal0, texVal1, texel);

    if (doS) {
        return d.x;
    } else if (doMul) {
        return a.x * c.x;
    } else if (doMx) {
        return mix(b.x, a.x, c.x);
    } else {
        return (a.x - b.x) * c.x + d.x;
    }
}

// Wrap helper matching OpenGL backend's WRAP macro
vec3 wrapRGB(vec3 v, float lo, float hi) {
    float range = hi - lo;
    vec3 x = v - vec3(lo);
    return x - floor(x / vec3(range)) * vec3(range) + vec3(lo);
}

float wrapA(float v, float lo, float hi) {
    float range = hi - lo;
    float x = v - lo;
    return x - floor(x / range) * range + lo;
}

vec4 wrap4(vec4 v, float lo, float hi) {
    float range = hi - lo;
    vec4 x = v - vec4(lo);
    return x - floor(x / vec4(range)) * vec4(range) + vec4(lo);
}

void main() {
    // --- Sample textures ---
    vec4 texVal0 = vec4(0.0);
    vec4 texVal1 = vec4(0.0);
    vec2 texSize0 = vec2(max(textureWidth.x, 1), max(textureHeight.x, 1));
    vec2 texSize1 = vec2(max(textureWidth.y, 1), max(textureHeight.y, 1));
    vec2 tc0 = vTexCoord0;
    vec2 tc1 = vTexCoord1;

    if (useClamp.x != 0) tc0.x = clamp(tc0.x, 0.5 / texSize0.x, vTexClampS0);
    if (useClamp.y != 0) tc0.y = clamp(tc0.y, 0.5 / texSize0.y, vTexClampT0);
    if (useClamp.z != 0) tc1.x = clamp(tc1.x, 0.5 / texSize1.x, vTexClampS1);
    if (useClamp.w != 0) tc1.y = clamp(tc1.y, 0.5 / texSize1.y, vTexClampT1);

    if (useTexture.x != 0) {
        texVal0 = hookTexture2D(0, 0, tc0, texSize0);
        if (useMask.x != 0) {
            vec4 maskVal0 = hookTexture2D(0, 2, tc0, texSize0);
            vec4 blendVal0 = useBlend.x != 0 ? hookTexture2D(0, 4, tc0, texSize0) : vec4(0.0);
            texVal0 = mix(texVal0, blendVal0, maskVal0.a);
        }
    }
    if (useTexture.y != 0) {
        texVal1 = hookTexture2D(1, 1, tc1, texSize1);
        if (useMask.y != 0) {
            vec4 maskVal1 = hookTexture2D(1, 3, tc1, texSize1);
            vec4 blendVal1 = useBlend.y != 0 ? hookTexture2D(1, 5, tc1, texSize1) : vec4(0.0);
            texVal1 = mix(texVal1, blendVal1, maskVal1.a);
        }
    }

    // --- Color combiner ---
    vec4 texel = vec4(0.0, 0.0, 0.0, 1.0);

    // Cycle 0 (always active)
    {
        bool firstCycle = true;
        bool doS_rgb = doSingle.x != 0;
        bool doM_rgb = doMultiply.x != 0;
        bool doX_rgb = doMix.x != 0;
        bool doS_a = doSingle.y != 0;
        bool doM_a = doMultiply.y != 0;
        bool doX_a = doMix.y != 0;

        if (colorAlphaSame.x != 0) {
            // RGB and Alpha use the same formula
            vec3 rgb = combineRGB(cc_0_0, doS_rgb, doM_rgb, doX_rgb,
                                  optAlpha != 0, firstCycle, texVal0, texVal1, texel);
            if (optAlpha != 0) {
                float a = combineAlpha(cc_0_0, doS_rgb, doM_rgb, doX_rgb,
                                       firstCycle, texVal0, texVal1, texel);
                texel = vec4(rgb, a);
            } else {
                texel = vec4(rgb, 1.0);
            }
        } else {
            vec3 rgb = combineRGB(cc_0_0, doS_rgb, doM_rgb, doX_rgb,
                                  false, firstCycle, texVal0, texVal1, texel);
            float a = 1.0;
            if (optAlpha != 0) {
                a = combineAlpha(cc_0_1, doS_a, doM_a, doX_a,
                                 firstCycle, texVal0, texVal1, texel);
            }
            texel = vec4(rgb, a);
        }
    }

    // Cycle 1 (if 2-cycle mode)
    if (opt2Cyc != 0) {
        // Wrap texel values between cycles (matching OpenGL backend behavior)
        if (optAlpha != 0) {
            if (cc_1_1.z == 13) { // SHADER_COMBINED
                texel.a = wrapA(texel.a, -1.01, 1.01);
            } else {
                texel.a = wrapA(texel.a, -0.51, 1.51);
            }
        }
        if (cc_1_0.z == 13) { // SHADER_COMBINED
            texel.rgb = wrapRGB(texel.rgb, -1.01, 1.01);
        } else {
            texel.rgb = wrapRGB(texel.rgb, -0.51, 1.51);
        }

        bool firstCycle = false;
        bool doS_rgb = doSingle.z != 0;
        bool doM_rgb = doMultiply.z != 0;
        bool doX_rgb = doMix.z != 0;
        bool doS_a = doSingle.w != 0;
        bool doM_a = doMultiply.w != 0;
        bool doX_a = doMix.w != 0;

        if (colorAlphaSame.y != 0) {
            vec3 rgb = combineRGB(cc_1_0, doS_rgb, doM_rgb, doX_rgb,
                                  optAlpha != 0, firstCycle, texVal0, texVal1, texel);
            if (optAlpha != 0) {
                float a = combineAlpha(cc_1_0, doS_rgb, doM_rgb, doX_rgb,
                                       firstCycle, texVal0, texVal1, texel);
                texel = vec4(rgb, a);
            } else {
                texel = vec4(rgb, 1.0);
            }
        } else {
            vec3 rgb = combineRGB(cc_1_0, doS_rgb, doM_rgb, doX_rgb,
                                  false, firstCycle, texVal0, texVal1, texel);
            float a = 1.0;
            if (optAlpha != 0) {
                a = combineAlpha(cc_1_1, doS_a, doM_a, doX_a,
                                 firstCycle, texVal0, texVal1, texel);
            }
            texel = vec4(rgb, a);
        }
    }

    texel = wrap4(texel, -0.51, 1.51);
    texel = clamp(texel, 0.0, 1.0);

    // --- Fog ---
    if (optFog != 0) {
        texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);
    }

    // --- Grayscale ---
    if (optGrayscale != 0) {
        float intensity = (texel.r + texel.g + texel.b) / 3.0;
        vec3 gray = vGrayscaleColor.rgb * intensity;
        texel.rgb = mix(texel.rgb, gray, vGrayscaleColor.a);
    }

    // --- Noise ---
    if (optNoise != 0) {
        texel.a *= floor(clamp(random(vec3(floor(gl_FragCoord.xy * noiseScale), float(frameCount))) + texel.a, 0.0, 1.0));
    }

    // --- Texture edge ---
    if (optTextureEdge != 0 && optAlpha != 0) {
        if (texel.a > 0.19) {
            texel.a = 1.0;
        } else {
            discard;
        }
    }

    // --- Alpha threshold ---
    if (optAlphaThreshold != 0) {
        if (texel.a < 8.0 / 256.0) {
            discard;
        }
    }

    // --- Invisible geometry ---
    if (optInvisible != 0) {
        texel.a = 0.0;
    }

    // --- sRGB conversion ---
    if (srgbMode != 0) {
        texel = fromLinear(texel);
    }

    // Final output
    texel = clamp(texel, 0.0, 1.0);
    outColor = texel;
}
