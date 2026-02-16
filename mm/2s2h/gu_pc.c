#include <math.h>
#include "z64.h"

#if defined(__ARM_NEON) && defined(__aarch64__)
#include <arm_neon.h>
#endif

void guMtxF2L(float mf[4][4], Mtx* m) {
#if defined(__ARM_NEON) && defined(__aarch64__)
    /* Convert 4×4 float matrix to N64 fixed-point 16.16 format.
     * Each pair of floats (a, b) is packed into:
     *   m1 word = (int(a*65536) & 0xffff0000) | ((int(b*65536) >> 16) & 0xffff)  [integer parts]
     *   m2 word = ((int(a*65536) << 16) & 0xffff0000) | (int(b*65536) & 0xffff)  [fractional parts]
     */
    float32x4_t scale = vdupq_n_f32(65536.0f);
    s32* m1 = &m->m[0][0];
    s32* m2 = &m->m[2][0];

    for (unsigned int r = 0; r < 4; r++) {
        float32x4_t row = vld1q_f32(mf[r]);
        int32x4_t fixed = vcvtq_s32_f32(vmulq_f32(row, scale));

        /* Extract pairs: (fixed[0], fixed[1]) and (fixed[2], fixed[3]) */
        s32 f0 = vgetq_lane_s32(fixed, 0);
        s32 f1 = vgetq_lane_s32(fixed, 1);
        s32 f2 = vgetq_lane_s32(fixed, 2);
        s32 f3 = vgetq_lane_s32(fixed, 3);

        *m1++ = (f0 & 0xffff0000) | ((u32)(f1) >> 16);
        *m1++ = (f2 & 0xffff0000) | ((u32)(f3) >> 16);
        *m2++ = ((u32)(f0) << 16) | (f1 & 0xffff);
        *m2++ = ((u32)(f2) << 16) | (f3 & 0xffff);
    }
#else
    unsigned int r, c;
    s32 tmp1;
    s32 tmp2;
    s32* m1 = &m->m[0][0];
    s32* m2 = &m->m[2][0];
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 2; c++) {
            tmp1 = mf[r][2 * c] * 65536.0f;
            tmp2 = mf[r][2 * c + 1] * 65536.0f;
            *m1++ = (tmp1 & 0xffff0000) | ((tmp2 >> 0x10) & 0xffff);
            *m2++ = ((tmp1 << 0x10) & 0xffff0000) | (tmp2 & 0xffff);
        }
    }
#endif
}

void guMtxL2F(float mf[4][4], Mtx* m) {
    unsigned int r, c;
    u32 tmp1;
    u32 tmp2;
    u32* m1;
    u32* m2;
    s32 stmp1, stmp2;
    m1 = (u32*)&m->m[0][0];
    m2 = (u32*)&m->m[2][0];
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 2; c++) {
            tmp1 = (*m1 & 0xffff0000) | ((*m2 >> 0x10) & 0xffff);
            tmp2 = ((*m1++ << 0x10) & 0xffff0000) | (*m2++ & 0xffff);
            stmp1 = *(s32*)&tmp1;
            stmp2 = *(s32*)&tmp2;
            mf[r][c * 2 + 0] = stmp1 / 65536.0f;
            mf[r][c * 2 + 1] = stmp2 / 65536.0f;
        }
    }
}

void guMtxIdentF(f32 mf[4][4]) {
    unsigned int r, c;
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            if (r == c) {
                mf[r][c] = 1.0f;
            } else {
                mf[r][c] = 0.0f;
            }
        }
    }
}

void guMtxIdent(Mtx* m) {
    guMtxIdentF(m->m);
}

void guTranslateF(float m[4][4], float x, float y, float z) {
    guMtxIdentF(m);
    m[3][0] = x;
    m[3][1] = y;
    m[3][2] = z;
}
void guTranslate(Mtx* m, float x, float y, float z) {
    float mf[4][4];
    guTranslateF(mf, x, y, z);
    guMtxF2L(mf, m);
}

void guScaleF(float mf[4][4], float x, float y, float z) {
    guMtxIdentF(mf);
    mf[0][0] = x;
    mf[1][1] = y;
    mf[2][2] = z;
    mf[3][3] = 1.0;
}
void guScale(Mtx* m, float x, float y, float z) {
    float mf[4][4];
    guScaleF(mf, x, y, z);
    guMtxF2L(mf, m);
}

void guNormalize(f32* x, f32* y, f32* z) {
    f32 tmp = 1.0f / sqrtf(*x * *x + *y * *y + *z * *z);
    *x = *x * tmp;
    *y = *y * tmp;
    *z = *z * tmp;
}
