@prism(type='fragment', name='Fast3D Fragment Shader', version='1.0.0', description='Ported shader to prism', author='Emill & Prism Team')

@{GLSL_VERSION}

@if(core_opengl || opengles)
out vec4 vOutColor;
@end

@for(i in 0..2)
    @if(o_textures[i])
        @{attr} vec2 vTexCoord@{i};
        @for(j in 0..2)
            @if(o_clamp[i][j])
                @if(j == 0)
                    @{attr} float vTexClampS@{i};
                @else
                    @{attr} float vTexClampT@{i};
                @end
            @end
        @end
    @end
@end

@if(o_fog) @{attr} vec4 vFog;
@if(o_grayscale) @{attr} vec4 vGrayscaleColor;

@for(i in 0..o_inputs)
    @if(o_alpha)
        @{attr} vec4 vInput@{i + 1};
    @else
        @{attr} vec3 vInput@{i + 1};
    @end
@end

@if(o_textures[0]) uniform sampler2D uTex0;
@if(o_textures[1]) uniform sampler2D uTex1;

@if(o_masks[0]) uniform sampler2D uTexMask0;
@if(o_masks[1]) uniform sampler2D uTexMask1;

@if(o_blend[0]) uniform sampler2D uTexBlend0;
@if(o_blend[1]) uniform sampler2D uTexBlend1;

uniform int frame_count;
uniform float noise_scale;

uniform int texture_width[2];
uniform int texture_height[2];
uniform int texture_filtering[2];

uniform int shader_cel_enabled;
uniform int shader_cel_bands;
uniform float shader_cel_softness;
uniform int shader_tonemapping_enabled;
uniform float shader_color_temp;
uniform float shader_brightness;
uniform float shader_contrast;
uniform float shader_film_grain;
uniform float shader_shadow_tint_intensity;
uniform float shader_shadow_tint_mix;
uniform float shader_rim_intensity;
uniform float shader_bloom_threshold;
uniform float shader_bloom_intensity;
uniform float shader_outline_intensity;
uniform float shader_vignette;
uniform float shader_specular_intensity;
uniform float shader_subsurface_intensity;
uniform float shader_micronormal_intensity;
uniform float shader_sharpening;
uniform float shader_hemi_ambient_intensity;
uniform float shader_hemi_ambient_sky_r;
uniform float shader_hemi_ambient_sky_g;
uniform float shader_hemi_ambient_sky_b;
uniform float shader_hemi_ambient_ground_r;
uniform float shader_hemi_ambient_ground_g;
uniform float shader_hemi_ambient_ground_b;
uniform float shader_saturation;
uniform float shader_viewport_width;
uniform float shader_viewport_height;

#define TEX_OFFSET(off) @{texture}(tex, texCoord - off / texSize)
#define WRAP(x, low, high) mod((x)-(low), (high)-(low)) + (low)

float random(in vec3 value) {
    float random = dot(sin(value), vec3(12.9898, 78.233, 37.719));
    return fract(sin(random) * 143758.5453);
}

vec4 fromLinear(vec4 linearRGB){
    bvec3 cutoff = lessThan(linearRGB.rgb, vec3(0.0031308));
    vec3 higher = vec3(1.055)*pow(linearRGB.rgb, vec3(1.0/2.4)) - vec3(0.055);
    vec3 lower = linearRGB.rgb * vec3(12.92);
    return vec4(mix(higher, lower, cutoff), linearRGB.a);
}

vec4 filter3point(in sampler2D tex, in vec2 texCoord, in vec2 texSize) {
    vec2 offset = fract(texCoord*texSize - vec2(0.5));
    offset -= step(1.0, offset.x + offset.y);
    vec4 c0 = TEX_OFFSET(offset);
    vec4 c1 = TEX_OFFSET(vec2(offset.x - sign(offset.x), offset.y));
    vec4 c2 = TEX_OFFSET(vec2(offset.x, offset.y - sign(offset.y)));
    return c0 + abs(offset.x)*(c1-c0) + abs(offset.y)*(c2-c0);
}

vec4 hookTexture2D(in int id, sampler2D tex, in vec2 uv, in vec2 texSize) {
@if(o_three_point_filtering)
    if(texture_filtering[id] == @{FILTER_THREE_POINT}) {
        return filter3point(tex, uv, texSize);
    }
@end
    return @{texture}(tex, uv);
}

#define TEX_SIZE(tex) vec2(texture_width[tex], texture_height[tex])

void main() {
    @for(i in 0..2)
        @if(o_textures[i])
            @{s = o_clamp[i][0]}
            @{t = o_clamp[i][1]}

            vec2 texSize@{i} = TEX_SIZE(@{i});

            @if(!s && !t)
                vec2 vTexCoordAdj@{i} = vTexCoord@{i};
            @else
                @if(s && t)
                    vec2 vTexCoordAdj@{i} = clamp(vTexCoord@{i}, 0.5 / texSize@{i}, vec2(vTexClampS@{i}, vTexClampT@{i}));
                @elseif(s)
                    vec2 vTexCoordAdj@{i} = vec2(clamp(vTexCoord@{i}.s, 0.5 / texSize@{i}.s, vTexClampS@{i}), vTexCoord@{i}.t);
                @else
                    vec2 vTexCoordAdj@{i} = vec2(vTexCoord@{i}.s, clamp(vTexCoord@{i}.t, 0.5 / texSize@{i}.t, vTexClampT@{i}));
                @end
            @end

            vec4 texVal@{i} = hookTexture2D(@{i}, uTex@{i}, vTexCoordAdj@{i}, texSize@{i});

            @if(o_masks[i])
                @if(opengles) 
                    vec2 maskSize@{i} = vec2(textureSize(uTexMask@{i}, 0));
                @else 
                    vec2 maskSize@{i} = textureSize(uTexMask@{i}, 0);
                @end

                vec4 maskVal@{i} = hookTexture2D(@{i}, uTexMask@{i}, vTexCoordAdj@{i}, maskSize@{i});

                @if(o_blend[i])
                    vec4 blendVal@{i} = hookTexture2D(@{i}, uTexBlend@{i}, vTexCoordAdj@{i}, texSize@{i});
                @else
                    vec4 blendVal@{i} = vec4(0, 0, 0, 0);
                @end

                texVal@{i} = mix(texVal@{i}, blendVal@{i}, maskVal@{i}.a);
            @end
        @end
    @end

    @if(o_alpha) 
        vec4 texel;
    @else 
        vec3 texel;
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
                    texel.a = WRAP(texel.a, -1.01, 1.01);
                @else
                    texel.a = WRAP(texel.a, -0.51, 1.51);
                @end
            @end

            @if(o_c[c][0][2] == SHADER_COMBINED)
                texel.rgb = WRAP(texel.rgb, -1.01, 1.01);
            @else
                texel.rgb = WRAP(texel.rgb, -0.51, 1.51);
            @end
        @end

        @if(!o_color_alpha_same[c] && o_alpha)
            texel = vec4(@{
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

    texel = WRAP(texel, -0.51, 1.51);
    texel = clamp(texel, 0.0, 1.0);
    // TODO discard if alpha is 0?
    @if(o_fog)
        @if(o_alpha)
            texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);
        @else
            texel = mix(texel, vFog.rgb, vFog.a);
        @end
    @end

    @if(o_texture_edge && o_alpha)
        if (texel.a > 0.19) texel.a = 1.0; else discard;
    @end

    @if(o_alpha && o_noise)
        texel.a *= floor(clamp(random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + texel.a, 0.0, 1.0));
    @end

    @if(o_grayscale)
        float intensity = (texel.r + texel.g + texel.b) / 3.0;
        vec3 new_texel = vGrayscaleColor.rgb * intensity;
        texel.rgb = mix(texel.rgb, new_texel, vGrayscaleColor.a);
    @end

    // Shader Effects: Cel Shading (§3.1 — toon ramp with soft transitions)
    if (shader_cel_enabled != 0) {
        float celLum = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
        if (celLum > 0.001) {
            float numBands = float(shader_cel_bands);
            float scaled = celLum * numBands;
            float bandIndex = floor(scaled);
            float t = fract(scaled);
            float halfSoft = shader_cel_softness * 0.5;
            float edge = smoothstep(0.5 - halfSoft, 0.5 + halfSoft, t);
            float bandedLum = (bandIndex + edge) / numBands;
            texel.rgb *= bandedLum / celLum;
        }
    }

    // Shader Effects: Shadow Colorization (§3.2 — cool/warm tinted shadows)
    if (shader_shadow_tint_intensity > 0.0) {
        float shadowLum = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
        vec3 coolTint = vec3(0.7, 0.8, 1.0);
        vec3 warmTint = vec3(1.0, 0.85, 0.7);
        vec3 tint = mix(coolTint, warmTint, shader_shadow_tint_mix);
        float shadowFactor = 1.0 - smoothstep(0.15, 0.5, shadowLum);
        texel.rgb = mix(texel.rgb, texel.rgb * tint, shadowFactor * shader_shadow_tint_intensity);
    }

    // Shader Effects: Rim Light (§3.4 — screen-space edge glow)
    if (shader_rim_intensity > 0.0) {
        float rimLum = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
        float dx = dFdx(rimLum);
        float dy = dFdy(rimLum);
        float edgeMag = length(vec2(dx, dy));
        float rim = smoothstep(0.02, 0.15, edgeMag);
        vec3 rimColor = vec3(0.85, 0.9, 1.0);
        texel.rgb += rim * rimColor * shader_rim_intensity * 0.4;
        texel.rgb = clamp(texel.rgb, 0.0, 1.0);
    }

    // Shader Effects: Color Temperature (§11 — day/night mood)
    if (shader_color_temp != 0.0) {
        float tempScale = shader_color_temp * 0.1;
        texel.rgb = clamp(texel.rgb + vec3(tempScale, 0.0, -tempScale) * texel.rgb, 0.0, 1.0);
    }

    // Shader Effects: Tone Mapping — ACES approximation (§10.1)
    if (shader_tonemapping_enabled != 0) {
        vec3 x = texel.rgb;
        texel.rgb = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
    }

    // Shader Effects: Bloom/Glow (§10.3 — per-pixel bright glow)
    if (shader_bloom_intensity > 0.0) {
        float bloomLum = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
        float bloom = max(0.0, bloomLum - shader_bloom_threshold);
        texel.rgb += texel.rgb * bloom * shader_bloom_intensity;
        texel.rgb = clamp(texel.rgb, 0.0, 1.0);
    }

    // Shader Effects: Edge Outlines (§6.1 — screen-space color-edge detection)
    if (shader_outline_intensity > 0.0) {
        float outLum = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
        float odx = dFdx(outLum);
        float ody = dFdy(outLum);
        float outEdge = length(vec2(odx, ody));
        float outline = smoothstep(0.05, 0.12, outEdge);
        vec3 outlineColor = vec3(0.15, 0.12, 0.1);
        texel.rgb = mix(texel.rgb, outlineColor, outline * shader_outline_intensity);
    }

    // Shader Effects: Specular Highlight (§3.3 — stylized anime highlight)
    if (shader_specular_intensity > 0.0) {
        float specLum = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
        float sdx = dFdx(specLum);
        float sdy = dFdy(specLum);
        // Use luminance gradient magnitude as pseudo-normal curvature for specular
        float curvature = length(vec2(sdx, sdy));
        float spec = pow(clamp(1.0 - curvature * 8.0, 0.0, 1.0), 64.0);
        float toonSpec = smoothstep(0.4, 0.6, spec);
        // Apply specular only to brighter areas (metals, wet surfaces)
        float specMask = smoothstep(0.3, 0.7, specLum);
        texel.rgb += vec3(1.0) * toonSpec * specMask * shader_specular_intensity * 0.15;
        texel.rgb = clamp(texel.rgb, 0.0, 1.0);
    }

    // Shader Effects: Subsurface Terminator Softness (§3.5 — warm tint at shadow edge)
    if (shader_subsurface_intensity > 0.0) {
        float subLum = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
        // Terminator region: where luminance is at the shadow/light boundary
        float terminator = smoothstep(0.2, 0.4, subLum) * (1.0 - smoothstep(0.4, 0.6, subLum));
        vec3 warmShift = vec3(0.08, 0.03, -0.02);
        texel.rgb += warmShift * terminator * shader_subsurface_intensity;
        texel.rgb = clamp(texel.rgb, 0.0, 1.0);
    }

    // Shader Effects: Procedural Micro-Normal (§8.2 — specular breakup from luminance gradient)
    if (shader_micronormal_intensity > 0.0) {
        float mnLum = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
        float mnDx = dFdx(mnLum);
        float mnDy = dFdy(mnLum);
        // Derive tiny specular perturbation from albedo luminance gradient
        float microSpec = abs(mnDx * mnDy) * 500.0;
        microSpec = clamp(microSpec, 0.0, 1.0);
        float microHighlight = pow(microSpec, 4.0) * smoothstep(0.4, 0.8, mnLum);
        texel.rgb += vec3(microHighlight * shader_micronormal_intensity * 0.1);
        texel.rgb = clamp(texel.rgb, 0.0, 1.0);
    }

    // Shader Effects: Sharpening (§10.4 — CAS-like per-fragment sharpening)
    if (shader_sharpening > 0.0) {
        float sharpLum = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
        float sharpDx = dFdx(sharpLum);
        float sharpDy = dFdy(sharpLum);
        float laplacian = abs(sharpDx) + abs(sharpDy);
        vec3 sharpened = texel.rgb + (texel.rgb - vec3(sharpLum)) * laplacian * shader_sharpening * 4.0;
        texel.rgb = clamp(sharpened, 0.0, 1.0);
    }

    // Shader Effects: Hemisphere Ambient (§5.1 — sky/ground ambient light)
    if (shader_hemi_ambient_intensity > 0.0 && shader_viewport_height > 0.0) {
        float screenY = gl_FragCoord.y / shader_viewport_height;
        vec3 skyColor = vec3(shader_hemi_ambient_sky_r, shader_hemi_ambient_sky_g, shader_hemi_ambient_sky_b);
        vec3 groundColor = vec3(shader_hemi_ambient_ground_r, shader_hemi_ambient_ground_g, shader_hemi_ambient_ground_b);
        vec3 ambientColor = mix(groundColor, skyColor, screenY);
        float ambientLum = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
        float ambientFactor = 1.0 - smoothstep(0.0, 0.5, ambientLum);
        texel.rgb = mix(texel.rgb, texel.rgb * ambientColor, ambientFactor * shader_hemi_ambient_intensity * 0.5);
        texel.rgb = clamp(texel.rgb, 0.0, 1.0);
    }

    // Shader Effects: Saturation (§10.2 — color grading)
    if (shader_saturation != 0.0) {
        float satLum = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
        texel.rgb = mix(vec3(satLum), texel.rgb, 1.0 + shader_saturation);
        texel.rgb = clamp(texel.rgb, 0.0, 1.0);
    }

    // Shader Effects: Brightness/Contrast (§10 — color grading)
    if (shader_brightness != 0.0 || shader_contrast != 0.0) {
        texel.rgb = clamp((texel.rgb - 0.5) * (1.0 + shader_contrast) + 0.5, 0.0, 1.0);
        texel.rgb = clamp(texel.rgb + shader_brightness, 0.0, 1.0);
    }

    // Shader Effects: Film Grain (§10.5 — subtle noise overlay)
    if (shader_film_grain > 0.0) {
        float grain = random(vec3(gl_FragCoord.xy, float(frame_count))) * 2.0 - 1.0;
        // 0.15 scales the grain to a subtle perceptual range at full strength
        texel.rgb = clamp(texel.rgb + vec3(grain * shader_film_grain * 0.15), 0.0, 1.0);
    }

    // Shader Effects: Vignette (§10.5 — screen edge darkening)
    if (shader_vignette > 0.0 && shader_viewport_width > 0.0 && shader_viewport_height > 0.0) {
        vec2 uv = gl_FragCoord.xy / vec2(shader_viewport_width, shader_viewport_height);
        vec2 vigCoord = uv * 2.0 - 1.0;
        float vigDist = dot(vigCoord, vigCoord);
        float vigFactor = 1.0 - vigDist * shader_vignette * 0.5;
        texel.rgb *= clamp(vigFactor, 0.0, 1.0);
    }

    @if(o_alpha)
        @if(o_alpha_threshold)
            if (texel.a < 8.0 / 256.0) discard;
        @end
        @if(o_invisible)
            texel.a = 0.0;
        @end
        @{vOutColor} = texel;
    @else
        @{vOutColor} = vec4(texel, 1.0);
    @end

    @if(srgb_mode)
        @{vOutColor} = fromLinear(@{vOutColor});
    @end
}