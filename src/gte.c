#include "gte.h"
#include <string.h>
#include <stdio.h>

/*
 * GTE register layout (data regs d0..d31):
 *
 * d0  = VXY0 (V0.x[15:0] | V0.y[31:16])      d1  = VZ0  (V0.z[15:0])
 * d2  = VXY1                                    d3  = VZ1
 * d4  = VXY2                                    d5  = VZ2
 * d6  = RGBC (r,g,b,code)                       d7  = OTZ
 * d8  = IR0   d9  = IR1   d10 = IR2  d11 = IR3
 * d12 = SXY0  d13 = SXY1  d14 = SXY2 d15 = SXYP
 * d16 = SZ0   d17 = SZ1   d18 = SZ2  d19 = SZ3
 * d20 = RGB0  d21 = RGB1  d22 = RGB2
 * d23 = RES1  (prohibited)
 * d24 = MAC0  d25 = MAC1  d26 = MAC2  d27 = MAC3
 * d28 = IRGB (write: expand 5-bit to IR1/2/3)  d29 = ORGB (read: pack IR1/2/3)
 * d30 = LZCS  d31 = LZCR
 *
 * Control regs c0..c31:
 * c0..c4  = rotation matrix RT (3x3, 16-bit packed pairs + one word)
 * c5..c7  = translation vector TRX/TRY/TRZ (32-bit signed)
 * c8..c12 = light matrix L (3x3)
 * c13..c15= background color BK (32-bit fixed-point)
 * c16..c20= light-color matrix LCM (3x3)
 * c21..c23= far color FC (32-bit fixed-point)
 * c24     = OFX (screen offset X, 32-bit signed)
 * c25     = OFY (screen offset Y, 32-bit signed)
 * c26     = H (projection plane distance, 16-bit unsigned)
 * c27     = DQA (depth cue coeff, 16-bit signed)
 * c28     = DQB (depth cue offset, 32-bit)
 * c29     = ZSF3 (1/3 z-average factor, 16-bit signed)
 * c30     = ZSF4 (1/4 z-average factor, 16-bit signed)
 * c31     = FLAG (overflow flags, read-only aggregate)
 */

/* ---- Accessor macros for named GTE fields ---- */

/* Vectors (signed 16-bit packed) */
#define VX(n) ((int16_t)((gte->dr[(n) * 2]) & 0xFFFF))
#define VY(n) ((int16_t)((gte->dr[(n) * 2] >> 16) & 0xFFFF))
#define VZ(n) ((int16_t)(gte->dr[(n) * 2 + 1] & 0xFFFF))

/* Screen XY fifo */
#define SX(i) ((int16_t)(gte->dr[12 + (i)] & 0xFFFF))
#define SY(i) ((int16_t)(gte->dr[12 + (i)] >> 16))

/* Screen Z fifo */
#define SZ(i) ((uint16_t)(gte->dr[16 + (i)] & 0xFFFF))

/* IR accumulators */
#define IR0 ((int16_t)(gte->dr[8] & 0xFFFF))
#define IR1 ((int16_t)(gte->dr[9] & 0xFFFF))
#define IR2 ((int16_t)(gte->dr[10] & 0xFFFF))
#define IR3 ((int16_t)(gte->dr[11] & 0xFFFF))

/* MAC accumulators (32-bit signed) */
#define MAC0 ((int32_t)gte->dr[24])
#define MAC1 ((int32_t)gte->dr[25])
#define MAC2 ((int32_t)gte->dr[26])
#define MAC3 ((int32_t)gte->dr[27])

/* RGBC */
#define GTE_R ((uint8_t)(gte->dr[6]))
#define GTE_G ((uint8_t)(gte->dr[6] >> 8))
#define GTE_B ((uint8_t)(gte->dr[6] >> 16))
#define GTE_CD ((uint8_t)(gte->dr[6] >> 24))

/* OTZ */
#define OTZ ((uint16_t)(gte->dr[7] & 0xFFFF))

/* Rotation matrix (signed 16-bit) packed two per word */
#define RT(r, c) ((int16_t)(gte->cr[(r) * 2 + (c) / 2] >> (((c) & 1) * 16)))
/* Actually the RT layout in ctrl regs:
   c0 = RT11[15:0] | RT12[31:16]
   c1 = RT13[15:0] | RT21[31:16]
   c2 = RT22[15:0] | RT23[31:16]
   c3 = RT31[15:0] | RT32[31:16]
   c4 = RT33[15:0]
*/
static inline int16_t rt(const Gte *gte, int r, int c)
{
    /* r,c: 0-based (0..2) */
    int idx = r * 3 + c; /* 0..8 */
    uint32_t word = gte->cr[idx / 2];
    return (int16_t)((idx & 1) ? (word >> 16) : word);
}

/* Light matrix L (c8..c12) */
static inline int16_t mat_lm(const Gte *gte, int r, int c)
{
    int idx = r * 3 + c;
    uint32_t word = gte->cr[8 + idx / 2];
    return (int16_t)((idx & 1) ? (word >> 16) : word);
}

/* Light-color matrix LCM (c16..c20) */
static inline int16_t mat_lcm(const Gte *gte, int r, int c)
{
    int idx = r * 3 + c;
    uint32_t word = gte->cr[16 + idx / 2];
    return (int16_t)((idx & 1) ? (word >> 16) : word);
}

/* Translation vector (signed 32-bit) */
#define TRX ((int32_t)gte->cr[5])
#define TRY ((int32_t)gte->cr[6])
#define TRZ ((int32_t)gte->cr[7])

/* Background color (12-bit fixed-point in 32-bit words) */
#define RBK ((int32_t)gte->cr[13])
#define GBK ((int32_t)gte->cr[14])
#define BBK ((int32_t)gte->cr[15])

/* Far color */
#define RFC ((int32_t)gte->cr[21])
#define GFC ((int32_t)gte->cr[22])
#define BFC ((int32_t)gte->cr[23])

/* Screen offsets / projection */
#define OFX ((int32_t)gte->cr[24])
#define OFY ((int32_t)gte->cr[25])
#define H ((uint16_t)(gte->cr[26] & 0xFFFF))
#define DQA ((int16_t)(gte->cr[27] & 0xFFFF))
#define DQB ((int32_t)gte->cr[28])
#define ZSF3 ((int16_t)(gte->cr[29] & 0xFFFF))
#define ZSF4 ((int16_t)(gte->cr[30] & 0xFFFF))
#define FLAG (gte->cr[31])

/* ---- FLAGS bits ---- */
#define F_MAC1_POS (1u << 30)
#define F_MAC2_POS (1u << 29)
#define F_MAC3_POS (1u << 28)
#define F_MAC1_NEG (1u << 27)
#define F_MAC2_NEG (1u << 26)
#define F_MAC3_NEG (1u << 25)
#define F_IR1_SAT (1u << 24)
#define F_IR2_SAT (1u << 23)
#define F_IR3_SAT (1u << 22)
#define F_COLOR_R (1u << 21)
#define F_COLOR_G (1u << 20)
#define F_COLOR_B (1u << 19)
#define F_SZ2_OVF (1u << 18)
#define F_DIV_OVF (1u << 17)
#define F_MAC0_POS (1u << 16)
#define F_MAC0_NEG (1u << 15)
#define F_SX2_SAT (1u << 14)
#define F_SY2_SAT (1u << 13)
#define F_IR0_SAT (1u << 12)
#define F_ERROR (1u << 31) /* summary — set if any of bits 12..22 or 24..30 set */

/* ---- Saturation helpers ---- */

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
static int64_t clamp64(int64_t v, int64_t lo, int64_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Set MAC1/2/3 with 44-bit overflow check (shift right by sf*12 when sf=1) */
static int32_t set_mac(Gte *gte, int n, int64_t v, int sf)
{
    int64_t shifted = sf ? (v >> 12) : v;
    /* overflow flags on the unshifted value (44-bit range for MAC1-3) */
    int64_t limit = (int64_t)1 << 43;
    if (v > limit - 1)
        gte->cr[31] |= (n == 1 ? F_MAC1_POS : n == 2 ? F_MAC2_POS
                                                     : F_MAC3_POS);
    if (v < -limit)
        gte->cr[31] |= (n == 1 ? F_MAC1_NEG : n == 2 ? F_MAC2_NEG
                                                     : F_MAC3_NEG);
    int32_t r = (int32_t)clamp64(shifted, INT32_MIN, INT32_MAX);
    gte->dr[24 + n] = (uint32_t)r; /* MAC1=d25, MAC2=d26, MAC3=d27 (n=1,2,3) */
    return r;
}

static int32_t set_mac0(Gte *gte, int64_t v)
{
    if (v > (int64_t)0x7FFFFFFF)
        gte->cr[31] |= F_MAC0_POS;
    if (v < -(int64_t)0x80000000)
        gte->cr[31] |= F_MAC0_NEG;
    int32_t r = (int32_t)clamp64(v, INT32_MIN, INT32_MAX);
    gte->dr[24] = (uint32_t)r;
    return r;
}

static int16_t set_ir(Gte *gte, int n, int32_t v, bool lm_flag)
{
    int32_t lo = lm_flag ? 0 : -0x8000;
    int32_t hi = 0x7FFF;
    if (v < lo || v > hi)
    {
        gte->cr[31] |= (n == 0 ? F_IR0_SAT : n == 1 ? F_IR1_SAT
                                         : n == 2   ? F_IR2_SAT
                                                    : F_IR3_SAT);
    }
    int16_t r = (int16_t)clamp(v, lo, hi);
    gte->dr[8 + n] = (uint32_t)(uint16_t)r;
    return r;
}

static void set_ir0(Gte *gte, int32_t v)
{
    if (v < 0 || v > 0x1000)
        gte->cr[31] |= F_IR0_SAT;
    gte->dr[8] = (uint32_t)(uint16_t)(int16_t)clamp(v, 0, 0x1000);
}

static uint8_t sat_rgb(Gte *gte, int ch, int32_t v)
{
    if (v < 0 || v > 255)
    {
        gte->cr[31] |= (ch == 0 ? F_COLOR_R : ch == 1 ? F_COLOR_G
                                                      : F_COLOR_B);
    }
    return (uint8_t)clamp(v, 0, 255);
}

/* Push FIFO: SXY0←SXY1, SXY1←SXY2, SXY2←new; same for SZ */
static void push_sxy(Gte *gte, int32_t sx, int32_t sy)
{
    gte->dr[12] = gte->dr[13];
    gte->dr[13] = gte->dr[14];
    /* saturate sx/sy to 11-bit signed (-1024..+1023) */
    if (sx < -0x400 || sx > 0x3FF)
        gte->cr[31] |= F_SX2_SAT;
    if (sy < -0x400 || sy > 0x3FF)
        gte->cr[31] |= F_SY2_SAT;
    int32_t csx = clamp(sx, -0x400, 0x3FF);
    int32_t csy = clamp(sy, -0x400, 0x3FF);
    gte->dr[14] = (uint32_t)((uint16_t)(int16_t)csx) | ((uint32_t)((uint16_t)(int16_t)csy) << 16);
    gte->dr[15] = gte->dr[14]; /* SXYP mirrors SXY2 */
}

static void push_sz(Gte *gte, int32_t v)
{
    gte->dr[16] = gte->dr[17];
    gte->dr[17] = gte->dr[18];
    gte->dr[18] = gte->dr[19];
    if (v < 0)
    {
        gte->cr[31] |= F_SZ2_OVF;
        v = 0;
    }
    if (v > 0xFFFF)
    {
        gte->cr[31] |= F_SZ2_OVF;
        v = 0xFFFF;
    }
    gte->dr[19] = (uint32_t)(uint16_t)v;
}

static void push_rgb(Gte *gte, int32_t r, int32_t g, int32_t b)
{
    gte->dr[20] = gte->dr[21];
    gte->dr[21] = gte->dr[22];
    uint32_t code = (gte->dr[6] >> 24) & 0xFF;
    gte->dr[22] = (uint32_t)sat_rgb(gte, 0, r) | ((uint32_t)sat_rgb(gte, 1, g) << 8) | ((uint32_t)sat_rgb(gte, 2, b) << 16) | (code << 24);
}

/* ---- UNR division (hardware reciprocal approximation) ---- */
/* Compute floor(h * 0x20000 / sz) clamped to 0x1FFFF */
static uint32_t gte_divide(uint16_t h, uint16_t sz)
{
    /* Exact formula from psx-spx: use a lookup table for leading-zero
       based reciprocal. We implement the hardware algorithm. */
    static const uint8_t UNR_TABLE[257] = {
        0xFF, 0xFD, 0xFB, 0xF9, 0xF7, 0xF5, 0xF3, 0xF1,
        0xEF, 0xEE, 0xEC, 0xEA, 0xE8, 0xE6, 0xE4, 0xE3,
        0xE1, 0xDF, 0xDD, 0xDC, 0xDA, 0xD8, 0xD6, 0xD5,
        0xD3, 0xD1, 0xD0, 0xCE, 0xCC, 0xCB, 0xC9, 0xC7,
        0xC6, 0xC4, 0xC3, 0xC1, 0xC0, 0xBE, 0xBD, 0xBB,
        0xBA, 0xB8, 0xB7, 0xB5, 0xB4, 0xB2, 0xB1, 0xB0,
        0xAE, 0xAD, 0xAB, 0xAA, 0xA9, 0xA7, 0xA6, 0xA4,
        0xA3, 0xA2, 0xA0, 0x9F, 0x9E, 0x9C, 0x9B, 0x9A,
        0x98, 0x97, 0x96, 0x94, 0x93, 0x92, 0x91, 0x8F,
        0x8E, 0x8D, 0x8C, 0x8A, 0x89, 0x88, 0x87, 0x86,
        0x84, 0x83, 0x82, 0x81, 0x80, 0x7E, 0x7D, 0x7C,
        0x7B, 0x7A, 0x79, 0x77, 0x76, 0x75, 0x74, 0x73,
        0x72, 0x71, 0x70, 0x6F, 0x6E, 0x6C, 0x6B, 0x6A,
        0x69, 0x68, 0x67, 0x66, 0x65, 0x64, 0x63, 0x62,
        0x61, 0x60, 0x5F, 0x5E, 0x5D, 0x5C, 0x5B, 0x5A,
        0x59, 0x58, 0x57, 0x56, 0x55, 0x54, 0x53, 0x52,
        0x51, 0x50, 0x4F, 0x4E, 0x4D, 0x4C, 0x4B, 0x4A,
        0x49, 0x48, 0x47, 0x46, 0x45, 0x44, 0x43, 0x42,
        0x41, 0x40, 0x3F, 0x3E, 0x3D, 0x3C, 0x3B, 0x3B,
        0x3A, 0x39, 0x38, 0x37, 0x36, 0x35, 0x34, 0x33,
        0x33, 0x32, 0x31, 0x30, 0x2F, 0x2E, 0x2D, 0x2D,
        0x2C, 0x2B, 0x2A, 0x29, 0x28, 0x28, 0x27, 0x26,
        0x25, 0x24, 0x24, 0x23, 0x22, 0x21, 0x20, 0x20,
        0x1F, 0x1E, 0x1D, 0x1D, 0x1C, 0x1B, 0x1A, 0x1A,
        0x19, 0x18, 0x17, 0x17, 0x16, 0x15, 0x14, 0x14,
        0x13, 0x12, 0x11, 0x11, 0x10, 0x0F, 0x0F, 0x0E,
        0x0D, 0x0C, 0x0C, 0x0B, 0x0A, 0x0A, 0x09, 0x08,
        0x08, 0x07, 0x06, 0x06, 0x05, 0x04, 0x04, 0x03,
        0x02, 0x02, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF};

    if (sz == 0 || (h / sz) >= 2)
    {
        return 0x1FFFF; /* division overflow */
    }

    /* Leading zero count to normalise sz */
    int shift = 0;
    uint32_t tmp = sz;
    while (tmp < 0x8000)
    {
        tmp <<= 1;
        shift++;
    }
    /* tmp is now in range [0x8000, 0xFFFF] */
    uint32_t idx = ((tmp & 0x7FFF) + 0x40) >> 7; /* 0..256 */
    if (idx > 256)
        idx = 256;
    uint32_t recip = (uint32_t)UNR_TABLE[idx] + 0x101;
    /* d = floor((0x2000080 - sz_norm * recip) / 0x80) where sz_norm = sz<<shift */
    uint32_t sz_n = (uint32_t)sz << shift;
    uint32_t d = (uint32_t)(((uint64_t)0x2000080 - (uint64_t)sz_n * recip) >> 7);
    /* result = floor((h * d + 0x8000) >> 16) after scaling */
    uint64_t result = ((uint64_t)(h << shift) * d + 0x8000) >> 16;
    if (result > 0x1FFFF)
        result = 0x1FFFF;
    return (uint32_t)result;
}

/* ---- Matrix×Vector multiply helpers ---- */

/* Multiply 3x3 matrix (row-major int16 fn) by vector (vx,vy,vz int16),
   add translation (tx,ty,tz int64), shift right by sf*12.
   Stores result in MAC1,MAC2,MAC3 and IR1,IR2,IR3. */
typedef int16_t (*MatFn)(const Gte *, int, int);

static void mat_mul_vec(Gte *gte, MatFn mf,
                        int32_t tx, int32_t ty, int32_t tz,
                        int16_t vx, int16_t vy, int16_t vz,
                        int sf, bool lm)
{
    int64_t m1 = (int64_t)tx * 0x1000 + (int64_t)mf(gte, 0, 0) * vx + (int64_t)mf(gte, 0, 1) * vy + (int64_t)mf(gte, 0, 2) * vz;
    int64_t m2 = (int64_t)ty * 0x1000 + (int64_t)mf(gte, 1, 0) * vx + (int64_t)mf(gte, 1, 1) * vy + (int64_t)mf(gte, 1, 2) * vz;
    int64_t m3 = (int64_t)tz * 0x1000 + (int64_t)mf(gte, 2, 0) * vx + (int64_t)mf(gte, 2, 1) * vy + (int64_t)mf(gte, 2, 2) * vz;
    int32_t r1 = set_mac(gte, 1, m1, sf);
    int32_t r2 = set_mac(gte, 2, m2, sf);
    int32_t r3 = set_mac(gte, 3, m3, sf);
    set_ir(gte, 1, r1, lm);
    set_ir(gte, 2, r2, lm);
    set_ir(gte, 3, r3, lm);
}

/* ---- RTPS: perspective transform for one vertex ---- */
static void gte_rtps(Gte *gte, int v, int sf, bool lm)
{
    int16_t vx = VX(v), vy = VY(v), vz = VZ(v);
    mat_mul_vec(gte, rt, TRX, TRY, TRZ, vx, vy, vz, sf, lm);

    /* SZ push: use MAC3 (unshifted TRZ contribution is in MAC3) */
    int32_t mac3 = MAC3;
    push_sz(gte, mac3);

    /* Division: h / SZ3 */
    uint16_t sz3 = SZ(3);
    if (sz3 == 0 || H / sz3 >= 2)
        gte->cr[31] |= F_DIV_OVF;
    uint32_t div = gte_divide(H, sz3);

    /* SX2 = (MAC1 * div + OFX) >> 16 */
    int64_t sx64 = ((int64_t)(int16_t)IR1 * div + OFX + 0x8000) >> 16;
    /* SY2 = (MAC2 * div + OFY) >> 16 */
    int64_t sy64 = ((int64_t)(int16_t)IR2 * div + OFY + 0x8000) >> 16;

    push_sxy(gte, (int32_t)sx64, (int32_t)sy64);

    /* IR0 (depth cue interpolation factor) */
    int64_t ir0_64 = ((int64_t)(int16_t)DQA * div + (int64_t)DQB + 0x800) >> 12;
    set_ir0(gte, (int32_t)ir0_64);
    set_mac0(gte, ir0_64 * 0x1000);
}

/* ---- GTE commands ---- */

static void cmd_rtps(Gte *gte, uint32_t cmd)
{
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    gte->cr[31] = 0;
    gte_rtps(gte, 0, sf, lm);
}

static void cmd_rtpt(Gte *gte, uint32_t cmd)
{
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    gte->cr[31] = 0;
    gte_rtps(gte, 0, sf, lm);
    gte_rtps(gte, 1, sf, lm);
    gte_rtps(gte, 2, sf, lm);
}

static void cmd_nclip(Gte *gte, uint32_t cmd)
{
    (void)cmd;
    gte->cr[31] = 0;
    /* MAC0 = SX0*(SY1-SY2) + SX1*(SY2-SY0) + SX2*(SY0-SY1) */
    int64_t v = (int64_t)SX(0) * ((int32_t)SY(1) - (int32_t)SY(2)) + (int64_t)SX(1) * ((int32_t)SY(2) - (int32_t)SY(0)) + (int64_t)SX(2) * ((int32_t)SY(0) - (int32_t)SY(1));
    set_mac0(gte, v);
}

static void cmd_avsz3(Gte *gte, uint32_t cmd)
{
    (void)cmd;
    gte->cr[31] = 0;
    int64_t v = (int64_t)(int16_t)ZSF3 * ((int32_t)SZ(1) + (int32_t)SZ(2) + (int32_t)SZ(3));
    set_mac0(gte, v);
    int32_t otz = (int32_t)(v >> 12);
    if (otz < 0)
    {
        gte->cr[31] |= F_SZ2_OVF;
        otz = 0;
    }
    if (otz > 0xFFFF)
    {
        gte->cr[31] |= F_SZ2_OVF;
        otz = 0xFFFF;
    }
    gte->dr[7] = (uint32_t)(uint16_t)otz;
}

static void cmd_avsz4(Gte *gte, uint32_t cmd)
{
    (void)cmd;
    gte->cr[31] = 0;
    int64_t v = (int64_t)(int16_t)ZSF4 * ((int32_t)SZ(0) + (int32_t)SZ(1) + (int32_t)SZ(2) + (int32_t)SZ(3));
    set_mac0(gte, v);
    int32_t otz = (int32_t)(v >> 12);
    if (otz < 0)
    {
        gte->cr[31] |= F_SZ2_OVF;
        otz = 0;
    }
    if (otz > 0xFFFF)
    {
        gte->cr[31] |= F_SZ2_OVF;
        otz = 0xFFFF;
    }
    gte->dr[7] = (uint32_t)(uint16_t)otz;
}

static void cmd_op(Gte *gte, uint32_t cmd)
{
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    gte->cr[31] = 0;
    /* MAC1 = RT22*IR3 - RT33*IR2
       MAC2 = RT33*IR1 - RT11*IR3
       MAC3 = RT11*IR2 - RT22*IR1 */
    int64_t m1 = (int64_t)rt(gte, 1, 1) * (int32_t)IR3 - (int64_t)rt(gte, 2, 2) * (int32_t)IR2;
    int64_t m2 = (int64_t)rt(gte, 2, 2) * (int32_t)IR1 - (int64_t)rt(gte, 0, 0) * (int32_t)IR3;
    int64_t m3 = (int64_t)rt(gte, 0, 0) * (int32_t)IR2 - (int64_t)rt(gte, 1, 1) * (int32_t)IR1;
    set_ir(gte, 1, set_mac(gte, 1, m1, sf), lm);
    set_ir(gte, 2, set_mac(gte, 2, m2, sf), lm);
    set_ir(gte, 3, set_mac(gte, 3, m3, sf), lm);
}

static void cmd_sqr(Gte *gte, uint32_t cmd)
{
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    gte->cr[31] = 0;
    set_ir(gte, 1, set_mac(gte, 1, (int64_t)IR1 * IR1, sf), lm);
    set_ir(gte, 2, set_mac(gte, 2, (int64_t)IR2 * IR2, sf), lm);
    set_ir(gte, 3, set_mac(gte, 3, (int64_t)IR3 * IR3, sf), lm);
}

/* MVMVA: generic matrix × vector + translation */
static void cmd_mvmva(Gte *gte, uint32_t cmd)
{
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    int mx = (cmd >> 17) & 3; /* 0=RT, 1=L, 2=LCM, 3=invalid */
    int vn = (cmd >> 15) & 3; /* 0=V0, 1=V1, 2=V2, 3=IR */
    int tv = (cmd >> 13) & 3; /* 0=TR, 1=BK, 2=FC/0, 3=zero */

    gte->cr[31] = 0;

    /* Select matrix function */
    MatFn mf;
    switch (mx)
    {
    case 0:
        mf = rt;
        break;
    case 1:
        mf = mat_lm;
        break;
    case 2:
        mf = mat_lcm;
        break;
    default:
        /* Garbage matrix (doc says all -0x60 pattern) — we just zero out */
        mf = rt;
        break;
    }

    /* Select vector */
    int16_t vx, vy, vz;
    if (vn <= 2)
    {
        vx = VX(vn);
        vy = VY(vn);
        vz = VZ(vn);
    }
    else
    {
        vx = IR1;
        vy = IR2;
        vz = IR3;
    }

    /* Select translation */
    int64_t tx, ty, tz;
    switch (tv)
    {
    case 0:
        tx = TRX;
        ty = TRY;
        tz = TRZ;
        break;
    case 1:
        tx = RBK;
        ty = GBK;
        tz = BBK;
        break;
    case 2:
        tx = RFC;
        ty = GFC;
        tz = BFC;
        break;
    default:
        tx = 0;
        ty = 0;
        tz = 0;
        break;
    }

    mat_mul_vec(gte, mf, (int32_t)tx, (int32_t)ty, (int32_t)tz, vx, vy, vz, sf, lm);
}

/* Depth cue single: interpolate between far-color and IR color */
static void depth_cue(Gte *gte, int sf, bool lm)
{
    /* MAC1 = (RFC - IR1) * IR0 + IR1*0x1000 */
    int64_t m1 = ((int64_t)RFC << 12) + (int64_t)IR0 * ((int32_t)IR1 - (int32_t)(int16_t)GTE_R * 0x10);
    int64_t m2 = ((int64_t)GFC << 12) + (int64_t)IR0 * ((int32_t)IR2 - (int32_t)(int16_t)GTE_G * 0x10);
    int64_t m3 = ((int64_t)BFC << 12) + (int64_t)IR0 * ((int32_t)IR3 - (int32_t)(int16_t)GTE_B * 0x10);
    set_ir(gte, 1, set_mac(gte, 1, m1, sf), lm);
    set_ir(gte, 2, set_mac(gte, 2, m2, sf), lm);
    set_ir(gte, 3, set_mac(gte, 3, m3, sf), lm);
    push_rgb(gte, MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
}

static void cmd_dpcs(Gte *gte, uint32_t cmd)
{
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    gte->cr[31] = 0;
    depth_cue(gte, sf, lm);
}

static void cmd_dpct(Gte *gte, uint32_t cmd)
{
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    gte->cr[31] = 0;
    for (int i = 0; i < 3; i++)
        depth_cue(gte, sf, lm);
}

static void cmd_intpl(Gte *gte, uint32_t cmd)
{
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    gte->cr[31] = 0;
    int64_t m1 = ((int64_t)RFC << 12) + (int64_t)IR0 * ((int32_t)IR1 - (int64_t)RFC);
    int64_t m2 = ((int64_t)GFC << 12) + (int64_t)IR0 * ((int32_t)IR2 - (int64_t)GFC);
    int64_t m3 = ((int64_t)BFC << 12) + (int64_t)IR0 * ((int32_t)IR3 - (int64_t)BFC);
    set_ir(gte, 1, set_mac(gte, 1, m1, sf), lm);
    set_ir(gte, 2, set_mac(gte, 2, m2, sf), lm);
    set_ir(gte, 3, set_mac(gte, 3, m3, sf), lm);
    push_rgb(gte, MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
}

/* Normal × light + background */
static void normal_color(Gte *gte, int v, int sf, bool lm_flag)
{
    /* Step 1: L * Vn → IR1/2/3 */
    mat_mul_vec(gte, mat_lm, 0, 0, 0, VX(v), VY(v), VZ(v), sf, lm_flag);
    /* Step 2: LCM * IR → MAC+BK */
    mat_mul_vec(gte, mat_lcm, RBK, GBK, BBK, IR1, IR2, IR3, sf, lm_flag);
}

static void cmd_ncds(Gte *gte, uint32_t cmd)
{
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    gte->cr[31] = 0;
    normal_color(gte, 0, sf, lm);
    /* Step 3: IR = (FC - IR*RGBC) * IR0 + IR*RGBC */
    int64_t m1 = ((int64_t)RFC << 12) + (int64_t)IR0 * ((int32_t)IR1 - (int32_t)GTE_R * 0x10 * (int32_t)IR1 / 0x1000);
    int64_t m2 = ((int64_t)GFC << 12) + (int64_t)IR0 * ((int32_t)IR2 - (int32_t)GTE_G * 0x10 * (int32_t)IR2 / 0x1000);
    int64_t m3 = ((int64_t)BFC << 12) + (int64_t)IR0 * ((int32_t)IR3 - (int32_t)GTE_B * 0x10 * (int32_t)IR3 / 0x1000);
    set_ir(gte, 1, set_mac(gte, 1, m1, sf), lm);
    set_ir(gte, 2, set_mac(gte, 2, m2, sf), lm);
    set_ir(gte, 3, set_mac(gte, 3, m3, sf), lm);
    push_rgb(gte, MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
}

static void cmd_ncdt(Gte *gte, uint32_t cmd)
{
    /* NCDS repeated for V0,V1,V2 */
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    gte->cr[31] = 0;
    for (int v = 0; v < 3; v++)
    {
        normal_color(gte, v, sf, lm);
        int64_t m1 = ((int64_t)RFC << 12) + (int64_t)IR0 * ((int32_t)IR1 - (int32_t)GTE_R * 0x10 * (int32_t)IR1 / 0x1000);
        int64_t m2 = ((int64_t)GFC << 12) + (int64_t)IR0 * ((int32_t)IR2 - (int32_t)GTE_G * 0x10 * (int32_t)IR2 / 0x1000);
        int64_t m3 = ((int64_t)BFC << 12) + (int64_t)IR0 * ((int32_t)IR3 - (int32_t)GTE_B * 0x10 * (int32_t)IR3 / 0x1000);
        set_ir(gte, 1, set_mac(gte, 1, m1, sf), lm);
        set_ir(gte, 2, set_mac(gte, 2, m2, sf), lm);
        set_ir(gte, 3, set_mac(gte, 3, m3, sf), lm);
        push_rgb(gte, MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
    }
}

static void cmd_nccs(Gte *gte, uint32_t cmd)
{
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    gte->cr[31] = 0;
    normal_color(gte, 0, sf, lm);
    /* Color with RGBC modulation (no far-color) */
    int64_t m1 = (int64_t)GTE_R * 0x10 * IR1;
    int64_t m2 = (int64_t)GTE_G * 0x10 * IR2;
    int64_t m3 = (int64_t)GTE_B * 0x10 * IR3;
    set_ir(gte, 1, set_mac(gte, 1, m1, sf), lm);
    set_ir(gte, 2, set_mac(gte, 2, m2, sf), lm);
    set_ir(gte, 3, set_mac(gte, 3, m3, sf), lm);
    push_rgb(gte, MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
}

static void cmd_nct(Gte *gte, uint32_t cmd)
{
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    gte->cr[31] = 0;
    for (int v = 0; v < 3; v++)
    {
        normal_color(gte, v, sf, lm);
        int64_t m1 = (int64_t)GTE_R * 0x10 * IR1;
        int64_t m2 = (int64_t)GTE_G * 0x10 * IR2;
        int64_t m3 = (int64_t)GTE_B * 0x10 * IR3;
        set_ir(gte, 1, set_mac(gte, 1, m1, sf), lm);
        set_ir(gte, 2, set_mac(gte, 2, m2, sf), lm);
        set_ir(gte, 3, set_mac(gte, 3, m3, sf), lm);
        push_rgb(gte, MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
    }
}

static void cmd_cc(Gte *gte, uint32_t cmd)
{
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    gte->cr[31] = 0;
    /* LCM * IR → MAC+BK */
    mat_mul_vec(gte, mat_lcm, RBK, GBK, BBK, IR1, IR2, IR3, sf, lm);
    int64_t m1 = (int64_t)GTE_R * 0x10 * IR1;
    int64_t m2 = (int64_t)GTE_G * 0x10 * IR2;
    int64_t m3 = (int64_t)GTE_B * 0x10 * IR3;
    set_ir(gte, 1, set_mac(gte, 1, m1, sf), lm);
    set_ir(gte, 2, set_mac(gte, 2, m2, sf), lm);
    set_ir(gte, 3, set_mac(gte, 3, m3, sf), lm);
    push_rgb(gte, MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
}

static void cmd_ncs(Gte *gte, uint32_t cmd)
{
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    gte->cr[31] = 0;
    normal_color(gte, 0, sf, lm);
    push_rgb(gte, MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
}

static void cmd_dcpl(Gte *gte, uint32_t cmd)
{
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    gte->cr[31] = 0;
    /* Like NCDS step 3 but uses current IR directly */
    int64_t m1 = (int64_t)GTE_R * 0x10 * IR1 + (int64_t)IR0 * ((int64_t)RFC - (int64_t)GTE_R * 0x10 * IR1 / 0x1000);
    int64_t m2 = (int64_t)GTE_G * 0x10 * IR2 + (int64_t)IR0 * ((int64_t)GFC - (int64_t)GTE_G * 0x10 * IR2 / 0x1000);
    int64_t m3 = (int64_t)GTE_B * 0x10 * IR3 + (int64_t)IR0 * ((int64_t)BFC - (int64_t)GTE_B * 0x10 * IR3 / 0x1000);
    set_ir(gte, 1, set_mac(gte, 1, m1, sf), lm);
    set_ir(gte, 2, set_mac(gte, 2, m2, sf), lm);
    set_ir(gte, 3, set_mac(gte, 3, m3, sf), lm);
    push_rgb(gte, MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
}

static void cmd_gpf(Gte *gte, uint32_t cmd)
{
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    gte->cr[31] = 0;
    set_ir(gte, 1, set_mac(gte, 1, (int64_t)IR0 * IR1, sf), lm);
    set_ir(gte, 2, set_mac(gte, 2, (int64_t)IR0 * IR2, sf), lm);
    set_ir(gte, 3, set_mac(gte, 3, (int64_t)IR0 * IR3, sf), lm);
    push_rgb(gte, MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
}

static void cmd_gpl(Gte *gte, uint32_t cmd)
{
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    gte->cr[31] = 0;
    int shift = sf ? 12 : 0;
    set_ir(gte, 1, set_mac(gte, 1, ((int64_t)MAC1 << shift) + (int64_t)IR0 * IR1, sf), lm);
    set_ir(gte, 2, set_mac(gte, 2, ((int64_t)MAC2 << shift) + (int64_t)IR0 * IR2, sf), lm);
    set_ir(gte, 3, set_mac(gte, 3, ((int64_t)MAC3 << shift) + (int64_t)IR0 * IR3, sf), lm);
    push_rgb(gte, MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
}

static void cmd_ncct(Gte *gte, uint32_t cmd)
{
    int sf = (cmd >> 19) & 1;
    bool lm = (cmd >> 10) & 1;
    gte->cr[31] = 0;
    for (int v = 0; v < 3; v++)
    {
        normal_color(gte, v, sf, lm);
        int64_t m1 = (int64_t)GTE_R * 0x10 * IR1;
        int64_t m2 = (int64_t)GTE_G * 0x10 * IR2;
        int64_t m3 = (int64_t)GTE_B * 0x10 * IR3;
        set_ir(gte, 1, set_mac(gte, 1, m1, sf), lm);
        set_ir(gte, 2, set_mac(gte, 2, m2, sf), lm);
        set_ir(gte, 3, set_mac(gte, 3, m3, sf), lm);
        push_rgb(gte, MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
    }
}

/* ---- Public API ---- */

void gte_init(Gte *gte)
{
    memset(gte, 0, sizeof(*gte));
}

uint32_t gte_read_data(Gte *gte, uint32_t reg)
{
    switch (reg)
    {
    case 1:
    case 3:
    case 5:
    case 8:
    case 9:
    case 10:
    case 11:
        /* Sign-extend 16-bit fields */
        return (uint32_t)(int32_t)(int16_t)(gte->dr[reg] & 0xFFFF);
    case 7:
    case 16:
    case 17:
    case 18:
    case 19:
        /* Zero-extend 16-bit fields */
        return gte->dr[reg] & 0xFFFF;
    case 15:
        return gte->dr[14]; /* SXYP reads SXY2 */
    case 28:
    case 29:
    {
        /* ORGB: pack IR1/2/3 into 5-bit fields */
        uint8_t r5 = (uint8_t)clamp(IR1 >> 7, 0, 0x1F);
        uint8_t g5 = (uint8_t)clamp(IR2 >> 7, 0, 0x1F);
        uint8_t b5 = (uint8_t)clamp(IR3 >> 7, 0, 0x1F);
        return r5 | ((uint32_t)g5 << 5) | ((uint32_t)b5 << 10);
    }
    case 31:
        /* LZCR */
        {
            uint32_t v = gte->dr[30];
            if (v == 0)
                return 32;
            int c = 0;
            if (v & 0x80000000)
            {
                while (v & 0x80000000)
                {
                    c++;
                    v <<= 1;
                }
            }
            else
            {
                while (!(v & 0x80000000))
                {
                    c++;
                    v <<= 1;
                }
            }
            return (uint32_t)c;
        }
    default:
        return gte->dr[reg];
    }
}

void gte_write_data(Gte *gte, uint32_t reg, uint32_t val)
{
    switch (reg)
    {
    case 15:
        /* SXYP write: push into FIFO */
        gte->dr[12] = gte->dr[13];
        gte->dr[13] = gte->dr[14];
        gte->dr[14] = val;
        gte->dr[15] = val;
        break;
    case 28:
    {
        /* IRGB: 5-bit channels → IR1/2/3 (scaled by 0x80) */
        int16_t r = (int16_t)((val & 0x1F) << 7);
        int16_t g = (int16_t)(((val >> 5) & 0x1F) << 7);
        int16_t b = (int16_t)(((val >> 10) & 0x1F) << 7);
        gte->dr[9] = (uint32_t)(uint16_t)r;
        gte->dr[10] = (uint32_t)(uint16_t)g;
        gte->dr[11] = (uint32_t)(uint16_t)b;
        gte->dr[28] = val;
        break;
    }
    case 29:
        break; /* ORGB read-only */
    case 30:
        gte->dr[30] = val;
        /* LZCR will be computed on read */
        break;
    case 31:
        break; /* LZCR read-only */
    default:
        gte->dr[reg] = val;
        break;
    }
}

uint32_t gte_read_ctrl(Gte *gte, uint32_t reg)
{
    if (reg == 31)
    {
        /* FLAG: bit 31 = error summary (bits 12..22, 24..30) */
        uint32_t f = gte->cr[31];
        uint32_t err = f & 0x7F87E000u; /* bits 12..22 + 24..30 */
        if (err)
            f |= (1u << 31);
        return f;
    }
    if (reg == 4 || reg == 12 || reg == 20)
        return (uint32_t)(int32_t)(int16_t)(gte->cr[reg] & 0xFFFF);
    return gte->cr[reg];
}

void gte_write_ctrl(Gte *gte, uint32_t reg, uint32_t val)
{
    if (reg == 31)
    {
        gte->cr[31] = val & 0x7FFFF000u; /* bit31 and bits0-11 are read-only/zero */
        return;
    }
    gte->cr[reg] = val;
}

void gte_load(Gte *gte, uint32_t reg, uint32_t val)
{
    gte_write_data(gte, reg, val);
}

uint32_t gte_store(Gte *gte, uint32_t reg)
{
    return gte_read_data(gte, reg);
}

void gte_execute(Gte *gte, uint32_t cmd)
{
    uint32_t op = cmd & 0x3F; /* bits 5..0 */
    switch (op)
    {
    case 0x01:
        cmd_rtps(gte, cmd);
        break;
    case 0x06:
        cmd_nclip(gte, cmd);
        break;
    case 0x0C:
        cmd_op(gte, cmd);
        break;
    case 0x10:
        cmd_dpcs(gte, cmd);
        break;
    case 0x11:
        cmd_intpl(gte, cmd);
        break;
    case 0x12:
        cmd_mvmva(gte, cmd);
        break;
    case 0x13:
        cmd_ncds(gte, cmd);
        break;
    case 0x14:
        cmd_cc(gte, cmd);
        break;
    case 0x16:
        cmd_ncs(gte, cmd);
        break;
    case 0x1B:
        cmd_nccs(gte, cmd);
        break;
    case 0x1C:
        cmd_dcpl(gte, cmd);
        break;
    case 0x1E:
        cmd_nct(gte, cmd);
        break;
    case 0x20:
        cmd_sqr(gte, cmd);
        break;
    case 0x28:
        cmd_sqr(gte, cmd);
        break; /* SQR alias */
    case 0x29:
        cmd_dcpl(gte, cmd);
        break; /* DCPL alt */
    case 0x2A:
        cmd_dpct(gte, cmd);
        break;
    case 0x2D:
        cmd_avsz3(gte, cmd);
        break;
    case 0x2E:
        cmd_avsz4(gte, cmd);
        break;
    case 0x30:
        cmd_rtpt(gte, cmd);
        break;
    case 0x3D:
        cmd_gpf(gte, cmd);
        break;
    case 0x3E:
        cmd_gpl(gte, cmd);
        break;
    case 0x3F:
        cmd_ncct(gte, cmd);
        break;
    case 0x17:
        cmd_ncdt(gte, cmd);
        break;
    default:
        /* Unknown GTE command — silently ignore (don't abort) */
        fprintf(stderr, "GTE: unknown cmd 0x%02X (full=0x%08X)\n", op, cmd);
        break;
    }
}
