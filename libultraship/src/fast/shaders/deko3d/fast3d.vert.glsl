// Fast3D deko3d uber-shader: vertex stage
// Compiled to DKSH via uam at build time.
//
// The vertex layout is dynamic — the backend configures which attributes
// are active via DkVtxAttribState each draw call. Inactive attributes
// read as (0,0,0,1). The shader always declares the full superset.
//
// Attribute slot assignment (must match ConfigureVertexState order):
//   0: aVtxPos        vec4  (always)
//   1: aTexCoord0     vec2  (if texture 0 used)
//   2: aTexClampS0    float (if texture 0 clamp S)
//   3: aTexClampT0    float (if texture 0 clamp T)
//   4: aTexCoord1     vec2  (if texture 1 used)
//   5: aTexClampS1    float (if texture 1 clamp S)
//   6: aTexClampT1    float (if texture 1 clamp T)
//   7: aFog           vec4  (if fog)
//   8: aGrayscaleColor vec4 (if grayscale)
//   9..12: aInput1..4  vec4 (color inputs; vec3 when no alpha, padded to vec4)
//
// Because the backend packs attributes tightly and sets attrib count
// dynamically, we declare them all and let unused ones default to zero.

#version 460

layout(location = 0) in vec4 aVtxPos;
layout(location = 1) in vec2 aTexCoord0;
layout(location = 2) in float aTexClampS0;
layout(location = 3) in float aTexClampT0;
layout(location = 4) in vec2 aTexCoord1;
layout(location = 5) in float aTexClampS1;
layout(location = 6) in float aTexClampT1;
layout(location = 7) in vec4 aFog;
layout(location = 8) in vec4 aGrayscaleColor;
layout(location = 9) in vec4 aInput1;
layout(location = 10) in vec4 aInput2;
layout(location = 11) in vec4 aInput3;
layout(location = 12) in vec4 aInput4;

layout(location = 0) out vec2 vTexCoord0;
layout(location = 1) out float vTexClampS0;
layout(location = 2) out float vTexClampT0;
layout(location = 3) out vec2 vTexCoord1;
layout(location = 4) out float vTexClampS1;
layout(location = 5) out float vTexClampT1;
layout(location = 6) out vec4 vFog;
layout(location = 7) out vec4 vGrayscaleColor;
layout(location = 8) out vec4 vInput1;
layout(location = 9) out vec4 vInput2;
layout(location = 10) out vec4 vInput3;
layout(location = 11) out vec4 vInput4;

void main() {
    vTexCoord0 = aTexCoord0;
    vTexClampS0 = aTexClampS0;
    vTexClampT0 = aTexClampT0;
    vTexCoord1 = aTexCoord1;
    vTexClampS1 = aTexClampS1;
    vTexClampT1 = aTexClampT1;
    vFog = aFog;
    vGrayscaleColor = aGrayscaleColor;
    vInput1 = aInput1;
    vInput2 = aInput2;
    vInput3 = aInput3;
    vInput4 = aInput4;
    gl_Position = aVtxPos;
}
