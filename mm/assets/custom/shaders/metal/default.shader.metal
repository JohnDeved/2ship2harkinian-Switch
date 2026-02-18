@prism(type='metal', name='Fast3D Metal Shader', version='1.0.0', description='Ported shader to prism', author='Emill & Prism Team')

#include <metal_stdlib>
using namespace metal;

// BEGIN VERTEX SHADER
struct FrameUniforms {
    int frameCount;
    float noiseScale;
    int celEnabled;
    int celBands;
    float celSoftness;
    int tonemappingEnabled;
    float colorTemp;
    float brightness;
    float contrast;
    float filmGrain;
    float shadowTintIntensity;
    float shadowTintMix;
    float rimIntensity;
    float bloomThreshold;
    float bloomIntensity;
    float outlineIntensity;
    float vignette;
    float specularIntensity;
    float subsurfaceIntensity;
    float micronormalIntensity;
    float sharpening;
    float hemiAmbientIntensity;
    float hemiAmbientSkyR;
    float hemiAmbientSkyG;
    float hemiAmbientSkyB;
    float hemiAmbientGroundR;
    float hemiAmbientGroundG;
    float hemiAmbientGroundB;
    float saturation;
    float viewportWidth;
    float viewportHeight;
};

struct Vertex {
    float4 position [[attribute(@{get_vertex_index()})]];
    @{update_floats(4)}
    @for(i in 0..2)
        @if(o_textures[i])
            float2 texCoord@{i} [[attribute(@{get_vertex_index()})]];
            @{update_floats(2)}
            @for(j in 0..2)
                @if(o_clamp[i][j])
                    @if(j == 0)
                        float texClampS@{i} [[attribute(@{get_vertex_index()})]];
                    @else
                        float texClampT@{i} [[attribute(@{get_vertex_index()})]];
                    @end
                    @{update_floats(1)}
                @end
            @end
        @end
    @end
    @if(o_fog)
        float4 fog [[attribute(@{get_vertex_index()})]];
        @{update_floats(4)}
    @end
    @if(o_grayscale)
        float4 grayscale [[attribute(@{get_vertex_index()})]];
        @{update_floats(4)}
    @end
    @for(i in 0..o_inputs)
        @if(o_alpha)
            float4 input@{i + 1} [[attribute(@{get_vertex_index()})]];
            @{update_floats(4)}
        @else
            float3 input@{i + 1} [[attribute(@{get_vertex_index()})]];
            @{update_floats(3)}
        @end
    @end
};

struct ProjectedVertex {
    @for(i in 0..2)
        @if(o_textures[i])
            float2 texCoord@{i};
            @for(j in 0..2)
                @if(o_clamp[i][j])
                    @if(j == 0)
                        float texClampS@{i};
                    @else
                        float texClampT@{i};
                    @end
                @end
            @end
        @end
    @end
    @if(o_fog)
        float4 fog;
    @end
    @if(o_grayscale)
        float4 grayscale;
    @end
    @for(i in 0..o_inputs)
        @if(o_alpha)
            float4 input@{i + 1};
        @else
            float3 input@{i + 1};
        @end
    @end
    float4 position [[position]];
};

vertex ProjectedVertex vertexShader(Vertex in [[stage_in]]) {
    ProjectedVertex out;
    @for(i in 0..2)
        @if(o_textures[i])
            out.texCoord@{i} = in.texCoord@{i};
            @for(j in 0..2)
                @if(o_clamp[i][j])
                    @if(j == 0)
                        out.texClampS@{i} = in.texClampS@{i};
                    @else
                        out.texClampT@{i} = in.texClampT@{i};
                    @end
                @end
            @end
        @end
    @end
    @if(o_fog)
        out.fog = in.fog;
    @end
    @if(o_grayscale)
        out.grayscale = in.grayscale;
    @end
    @for(i in 0..o_inputs)
         out.input@{i + 1} = in.input@{i + 1};
    @end
    out.position = in.position;
    return out;
}
// END - BEGIN FRAGMENT SHADER

float mod(float x, float y) {
    return float(x - y * floor(x / y));
}

float3 mod(float3 a, float3 b) {
    return float3(a.x - b.x * floor(a.x / b.x), a.y - b.y * floor(a.y / b.y), a.z - b.z * floor(a.z / b.z));
}

float4 mod(float4 a, float4 b) {
    return float4(a.x - b.x * floor(a.x / b.x), a.y - b.y * floor(a.y / b.y), a.z - b.z * floor(a.z / b.z), a.w - b.w * floor(a.w / b.w));
}

#define WRAP(x, low, high) mod((x)-(low), (high)-(low)) + (low)
#define TEX_OFFSET(tex, texSmplr, texCoord, off, texSize) tex.sample(texSmplr, texCoord - off / texSize)

@if(o_three_point_filtering)
    float4 filter3point(thread const texture2d<float> tex, thread const sampler texSmplr, thread const float2& texCoord, thread const float2& texSize) {
        float2 offset = fract((texCoord * texSize) - float2(0.5));
        offset -= float2(step(1.0, offset.x + offset.y));
        float4 c0 = TEX_OFFSET(tex, texSmplr, texCoord, offset, texSize);
        float4 c1 = TEX_OFFSET(tex, texSmplr, texCoord, float2(offset.x - sign(offset.x), offset.y), texSize);
        float4 c2 = TEX_OFFSET(tex, texSmplr, texCoord, float2(offset.x, offset.y - sign(offset.y)), texSize);
        return c0 + abs(offset.x) * (c1 - c0) + abs(offset.y) * (c2 - c0);
    }

    float4 hookTexture2D(thread const texture2d<float> tex, thread const sampler texSmplr, thread const float2& uv, thread const float2& texSize) {
        return filter3point(tex, texSmplr, uv, texSize);
    }
@else
    float4 hookTexture2D(thread const texture2d<float> tex, thread const sampler texSmplr, thread const float2& uv, thread const float2& texSize) {
        return tex.sample(texSmplr, uv);
    }
@end

float random(float3 value) {
    float random = dot(sin(value), float3(12.9898, 78.233, 37.719));
    return fract(sin(random) * 143758.5453);
}

fragment float4 fragmentShader(ProjectedVertex in [[stage_in]], constant FrameUniforms &frameUniforms [[buffer(0)]]
@if(o_textures[0])
    , texture2d<float> uTex0 [[texture(0)]], sampler uTex0Smplr [[sampler(0)]]
@end
@if(o_textures[1])
    , texture2d<float> uTex1 [[texture(1)]], sampler uTex1Smplr [[sampler(1)]]
@end
@if(o_masks[0])
    , texture2d<float> uTexMask0 [[texture(2)]]
@end
@if(o_masks[1])
    , texture2d<float> uTexMask1 [[texture(3)]]
@end
@if(o_blend[0])
    , texture2d<float> uTexBlend0 [[texture(4)]]
@end
@if(o_blend[1])
    , texture2d<float> uTexBlend1 [[texture(5)]]
@end
) {
    @for(i in 0..2)
        @if(o_textures[i])
            @{s = o_clamp[i][0]}
            @{t = o_clamp[i][1]}
            float2 texSize@{i} = float2(uTex@{i}.get_width(), uTex@{i}.get_height());
            @if(!s && !t)
                float2 vTexCoordAdj@{i} = in.texCoord@{i};
            @else
                @if(s && t)
                    float2 vTexCoordAdj@{i} = fast::clamp(in.texCoord@{i}, float2(0.5) / texSize@{i}, float2(in.texClampS@{i}, in.texClampT@{i}));
                @elseif(s)
                    float2 vTexCoordAdj@{i} = float2(fast::clamp(in.texCoord@{i}.x, 0.5 / texSize@{i}.x, in.texClampS@{i}), in.texCoord@{i}.y);
                @else
                    float2 vTexCoordAdj@{i} = float2(in.texCoord@{i}.x, fast::clamp(in.texCoord@{i}.y, 0.5 / texSize@{i}.y, in.texClampT@{i}));
                @end
            @end

            float4 texVal@{i} = hookTexture2D(uTex@{i}, uTex@{i}Smplr, vTexCoordAdj@{i}, texSize@{i});

            @if(o_masks[i])
                float2 maskSize@{i} = float2(uTexMask@{i}.get_width(), uTexMask@{i}.get_height());
                float4 maskVal@{i} = hookTexture2D(uTexMask@{i}, uTex@{i}Smplr, vTexCoordAdj@{i}, maskSize@{i});
                @if(o_blend[i])
                    float4 blendVal@{i} = hookTexture2D(uTexBlend@{i}, uTex@{i}Smplr, vTexCoordAdj@{i}, texSize@{i});
                @else
                    float4 blendVal@{i} = float4(0, 0, 0, 0);
                @end

                texVal@{i} = mix(texVal@{i}, blendVal@{i}, maskVal@{i}.w);
            @end
        @end
    @end
    
    @if(o_alpha)
        float4 texel;
    @else
        float3 texel;
    @end

    @if(o_2cyc)
        @{f_range = 2}
    @else
        @{f_range = 1}
    @end

    @for(c in 0..f_range)
        @if(c == 1)
            @if(o_alpha)
                @if(o_c[c][1][2] == SHADER_COMBINED)
                    texel.w = WRAP(texel.w, -1.01, 1.01);
                @else
                    texel.w = WRAP(texel.w, -0.51, 1.51);
                @end
            @end

            @if(o_c[c][0][2] == SHADER_COMBINED)
                texel.xyz = WRAP(texel.xyz, -1.01, 1.01);
            @else
                texel.xyz = WRAP(texel.xyz, -0.51, 1.51);
            @end
        @end

        @if(!o_color_alpha_same[c] && o_alpha)
            texel = float4(@{
            append_formula(o_c[c], o_do_single[c][0],
                           o_do_multiply[c][0], o_do_mix[c][0], false, false, true, c == 0)
            }, @{append_formula(o_c[c], o_do_single[c][1],
                           o_do_multiply[c][1], o_do_mix[c][1], true, true, true, c == 0)
            });
        @else
            texel = @{append_formula(o_c[c], o_do_single[c][0],
                           o_do_multiply[c][0], o_do_mix[c][0], o_alpha, false,
                           o_alpha, c == 0)};
        @end
    @end

    @if(o_texture_edge && o_alpha)
        if (texel.w > 0.19) texel.w = 1.0; else discard_fragment();
    @end

    texel = WRAP(texel, -0.51, 1.51);
    texel = clamp(texel, 0.0, 1.0);
    // TODO discard if alpha is 0?

    @if(o_fog)
        @if(o_alpha)
            texel = float4(mix(texel.xyz, in.fog.xyz, in.fog.w), texel.w);
        @else
            texel = mix(texel, in.fog.xyz, in.fog.w);
        @end
    @end

    @if(o_grayscale)
        float intensity = (texel.x + texel.y + texel.z) / 3.0;
        float3 new_texel = in.grayscale.xyz * intensity;
        texel.xyz = mix(texel.xyz, new_texel, in.grayscale.w);
    @end

    @if(o_alpha && o_noise)
        float2 coords = screenSpace.xy * noise_scale;
        texel.w *= round(saturate(random(float3(floor(coords), noise_frame)) + texel.w - 0.5));
    @end

    // Shader Effects: Cel Shading (§3.1 — toon ramp with soft transitions)
    if (frameUniforms.celEnabled != 0) {
        float celLum = dot(texel.xyz, float3(0.299, 0.587, 0.114));
        if (celLum > 0.001) {
            float numBands = float(frameUniforms.celBands);
            float scaled = celLum * numBands;
            float bandIndex = floor(scaled);
            float t = fract(scaled);
            float halfSoft = frameUniforms.celSoftness * 0.5;
            float edge = smoothstep(0.5 - halfSoft, 0.5 + halfSoft, t);
            float bandedLum = (bandIndex + edge) / numBands;
            texel.xyz *= bandedLum / celLum;
        }
    }

    // Shader Effects: Shadow Colorization (§3.2 — cool/warm tinted shadows)
    if (frameUniforms.shadowTintIntensity > 0.0) {
        float shadowLum = dot(texel.xyz, float3(0.299, 0.587, 0.114));
        float3 coolTint = float3(0.7, 0.8, 1.0);
        float3 warmTint = float3(1.0, 0.85, 0.7);
        float3 tint = mix(coolTint, warmTint, frameUniforms.shadowTintMix);
        float shadowFactor = 1.0 - smoothstep(0.15, 0.5, shadowLum);
        texel.xyz = mix(texel.xyz, texel.xyz * tint, shadowFactor * frameUniforms.shadowTintIntensity);
    }

    // Shader Effects: Rim Light (§3.4 — screen-space edge glow)
    if (frameUniforms.rimIntensity > 0.0) {
        float rimLum = dot(texel.xyz, float3(0.299, 0.587, 0.114));
        float dx = dfdx(rimLum);
        float dy = dfdy(rimLum);
        float edgeMag = length(float2(dx, dy));
        float rim = smoothstep(0.02, 0.15, edgeMag);
        float3 rimColor = float3(0.85, 0.9, 1.0);
        texel.xyz += rim * rimColor * frameUniforms.rimIntensity * 0.4;
        texel.xyz = clamp(texel.xyz, 0.0, 1.0);
    }

    // Shader Effects: Color Temperature (§11 — day/night mood)
    if (frameUniforms.colorTemp != 0.0) {
        float tempScale = frameUniforms.colorTemp * 0.1;
        texel.xyz = clamp(texel.xyz + float3(tempScale, 0.0, -tempScale) * texel.xyz, 0.0, 1.0);
    }

    // Shader Effects: Tone Mapping — ACES approximation (§10.1)
    if (frameUniforms.tonemappingEnabled != 0) {
        float3 x = texel.xyz;
        texel.xyz = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
    }

    // Shader Effects: Bloom/Glow (§10.3 — per-pixel bright glow)
    if (frameUniforms.bloomIntensity > 0.0) {
        float bloomLum = dot(texel.xyz, float3(0.299, 0.587, 0.114));
        float bloom = max(0.0, bloomLum - frameUniforms.bloomThreshold);
        texel.xyz += texel.xyz * bloom * frameUniforms.bloomIntensity;
        texel.xyz = clamp(texel.xyz, 0.0, 1.0);
    }

    // Shader Effects: Edge Outlines (§6.1 — screen-space color-edge detection)
    if (frameUniforms.outlineIntensity > 0.0) {
        float outLum = dot(texel.xyz, float3(0.299, 0.587, 0.114));
        float odx = dfdx(outLum);
        float ody = dfdy(outLum);
        float outEdge = length(float2(odx, ody));
        float outline = smoothstep(0.05, 0.12, outEdge);
        float3 outlineColor = float3(0.15, 0.12, 0.1);
        texel.xyz = mix(texel.xyz, outlineColor, outline * frameUniforms.outlineIntensity);
    }

    // Shader Effects: Specular Highlight (§3.3 — stylized anime highlight)
    if (frameUniforms.specularIntensity > 0.0) {
        float specLum = dot(texel.xyz, float3(0.299, 0.587, 0.114));
        float sdx = dfdx(specLum);
        float sdy = dfdy(specLum);
        float curvature = length(float2(sdx, sdy));
        float spec = pow(clamp(1.0 - curvature * 8.0, 0.0, 1.0), 64.0);
        float toonSpec = smoothstep(0.4, 0.6, spec);
        float specMask = smoothstep(0.3, 0.7, specLum);
        texel.xyz += float3(1.0) * toonSpec * specMask * frameUniforms.specularIntensity * 0.15;
        texel.xyz = clamp(texel.xyz, 0.0, 1.0);
    }

    // Shader Effects: Subsurface Terminator Softness (§3.5 — warm tint at shadow edge)
    if (frameUniforms.subsurfaceIntensity > 0.0) {
        float subLum = dot(texel.xyz, float3(0.299, 0.587, 0.114));
        float terminator = smoothstep(0.2, 0.4, subLum) * (1.0 - smoothstep(0.4, 0.6, subLum));
        float3 warmShift = float3(0.08, 0.03, -0.02);
        texel.xyz += warmShift * terminator * frameUniforms.subsurfaceIntensity;
        texel.xyz = clamp(texel.xyz, 0.0, 1.0);
    }

    // Shader Effects: Procedural Micro-Normal (§8.2 — specular breakup from luminance gradient)
    if (frameUniforms.micronormalIntensity > 0.0) {
        float mnLum = dot(texel.xyz, float3(0.299, 0.587, 0.114));
        float mnDx = dfdx(mnLum);
        float mnDy = dfdy(mnLum);
        float microSpec = abs(mnDx * mnDy) * 500.0;
        microSpec = clamp(microSpec, 0.0, 1.0);
        float microHighlight = pow(microSpec, 4.0) * smoothstep(0.4, 0.8, mnLum);
        texel.xyz += float3(microHighlight * frameUniforms.micronormalIntensity * 0.1);
        texel.xyz = clamp(texel.xyz, 0.0, 1.0);
    }

    // Shader Effects: Sharpening (§10.4 — CAS-like per-fragment sharpening)
    if (frameUniforms.sharpening > 0.0) {
        float sharpLum = dot(texel.xyz, float3(0.299, 0.587, 0.114));
        float sharpDx = dfdx(sharpLum);
        float sharpDy = dfdy(sharpLum);
        float laplacian = abs(sharpDx) + abs(sharpDy);
        float3 sharpened = texel.xyz + (texel.xyz - float3(sharpLum)) * laplacian * frameUniforms.sharpening * 4.0;
        texel.xyz = clamp(sharpened, 0.0, 1.0);
    }

    // Shader Effects: Hemisphere Ambient (§5.1 — sky/ground ambient light)
    if (frameUniforms.hemiAmbientIntensity > 0.0 && frameUniforms.viewportHeight > 0.0) {
        float screenY = in.position.y / frameUniforms.viewportHeight;
        float3 skyColor = float3(frameUniforms.hemiAmbientSkyR, frameUniforms.hemiAmbientSkyG, frameUniforms.hemiAmbientSkyB);
        float3 groundColor = float3(frameUniforms.hemiAmbientGroundR, frameUniforms.hemiAmbientGroundG, frameUniforms.hemiAmbientGroundB);
        float3 ambientColor = mix(groundColor, skyColor, screenY);
        float ambientLum = dot(texel.xyz, float3(0.299, 0.587, 0.114));
        float ambientFactor = 1.0 - smoothstep(0.0, 0.5, ambientLum);
        texel.xyz = mix(texel.xyz, texel.xyz * ambientColor, ambientFactor * frameUniforms.hemiAmbientIntensity * 0.5);
        texel.xyz = clamp(texel.xyz, 0.0, 1.0);
    }

    // Shader Effects: Saturation (§10.2 — color grading)
    if (frameUniforms.saturation != 0.0) {
        float satLum = dot(texel.xyz, float3(0.299, 0.587, 0.114));
        texel.xyz = mix(float3(satLum), texel.xyz, 1.0 + frameUniforms.saturation);
        texel.xyz = clamp(texel.xyz, 0.0, 1.0);
    }

    // Shader Effects: Brightness/Contrast (§10 — color grading)
    if (frameUniforms.brightness != 0.0 || frameUniforms.contrast != 0.0) {
        texel.xyz = clamp((texel.xyz - 0.5) * (1.0 + frameUniforms.contrast) + 0.5, 0.0, 1.0);
        texel.xyz = clamp(texel.xyz + frameUniforms.brightness, 0.0, 1.0);
    }

    // Shader Effects: Film Grain (§10.5 — subtle noise overlay)
    if (frameUniforms.filmGrain > 0.0) {
        float grain = random(float3(floor(in.position.xy), frameUniforms.frameCount)) * 2.0 - 1.0;
        // 0.15 scales the grain to a subtle perceptual range at full strength
        texel.xyz = clamp(texel.xyz + float3(grain, grain, grain) * frameUniforms.filmGrain * 0.15, 0.0, 1.0);
    }

    // Shader Effects: Vignette (§10.5 — screen edge darkening)
    if (frameUniforms.vignette > 0.0 && frameUniforms.viewportWidth > 0.0 && frameUniforms.viewportHeight > 0.0) {
        float2 uv = in.position.xy / float2(frameUniforms.viewportWidth, frameUniforms.viewportHeight);
        float2 vigCoord = uv * 2.0 - 1.0;
        float vigDist = dot(vigCoord, vigCoord);
        float vigFactor = 1.0 - vigDist * frameUniforms.vignette * 0.5;
        texel.xyz *= clamp(vigFactor, 0.0, 1.0);
    }

    @if(o_alpha)
        @if(o_alpha_threshold)
            if (texel.w < 8.0 / 256.0) discard_fragment();
        @end
        @if(o_invisible)
            texel.w = 0.0;
        @end
        return texel;
    @else
        return float4(texel, 1.0);
    @end
}