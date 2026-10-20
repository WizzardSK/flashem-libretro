/* Graphics engine command lists: see ge.h.
 *
 * A list is a run of 32-bit words; the top byte is the opcode. Worked out
 * so far from the lists a BOOT.BIN kernel builds (its list builders sit at
 * 0x10A35xxx in Dingo Rallye's kernel):
 *   0x08000000 a b   a chunk: its commands run from a (the next word) up to
 *                    b, where the next chunk's header is
 *   0x1400FD01       end of list (reported to the CPU as status 0xFD08)
 *   0x20RRnnnn v...  set n 16-bit registers from RR, two to a word, low first
 *   0x81yyyxxx       drawing origin (12-bit y, 12-bit x)
 *   0x82yyyxxx       clip rectangle top-left
 *   0x83yyyxxx       clip rectangle bottom-right
 *   0xD3000003 p s c fill a rectangle at p = y << 16 | x, size s = h << 16 | w,
 *                    with colour c (BGR555)
 * The surface is 1024 pixels wide, 16 bits a pixel, stored in 8x8 tiles of
 * 128 bytes (16 KB per row of tiles) - so the video engine's three display
 * buffers at (0,480), (512,480) and (512,240) sit at 0x100C0000, 0x100C2000
 * and 0x1004A000. A8 +0x98 is the address of row +0x90 >> 16 (the kernel
 * sets row 240 = 0x10048000, a game row 128 = 0x10010000: the same layout);
 * g->surface is where row 0 would be. That row is the surface's top edge:
 * the kernel's first list fills (0,0) 512x240, which must not reach the
 * RAM below 0x10048000. */
#include "ge.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#define RAM_BASE 0x10000000u

static uint32_t rd(GE *g, uint32_t a) {
    uint32_t o = a - RAM_BASE, v = 0;
    if (o < g->ram_size - 3) memcpy(&v, g->ram + o, 4);
    return v;
}

static void wr(GE *g, uint32_t a, uint32_t v) {
    uint32_t o = a - RAM_BASE;
    if (o < g->ram_size - 3) memcpy(g->ram + o, &v, 4);
}

static uint16_t *px(GE *g, int x, int y) {
    if (x < 0 || x > 1023 || y < g->top) return NULL;
    uint32_t a = g->surface + (uint32_t)(y >> 3) * 16384 + (uint32_t)(x >> 5) * 512 +
                 (uint32_t)(y & 7) * 64 + (uint32_t)(x & 31) * 2;
    uint32_t o = a - RAM_BASE;
    return o < g->ram_size - 1 ? (uint16_t *)(g->ram + o) : NULL;
}

/* VFLASH_GEWATCH=x,y: report every write to that surface pixel. */
static int watch_x = -1, watch_y = -1;
static const char *watch_what;
static uint32_t watch_pc;
/* for the unknown-op log: the list being run and the previous command word */
static uint32_t cur_list, prev_w, prev_pc;

static void watch(GE *g, int x, int y, uint16_t c) {
    if (x == watch_x && y == watch_y)
        printf("[GEW] (%d,%d) = %04X by %s at %08X\n", x, y, c, watch_what, watch_pc);
    (void)g;
}

/* Depth of what the 3D path last drew at each surface pixel;
 * a fill clears it. Where the engine keeps its own depth is not known. */
static float zbuf[1024 * 1024];
static uint8_t abuf[1024 * 1024];   /* destination alpha, see c555() */
static uint8_t abuf[1024 * 1024]; /* Destination alpha; fills clear it below. */
/* Sprite depth. A sprite's attribute word (the f16 after its header) is its
 * depth as 1/z - bigger is nearer - in the same buffer as the 3D: every sprite
 * pixel writes z = 1/attr there, and with depth testing on (0x1E bit 15) is only
 * drawn if that is not farther than what is there. Found 2026-09-27:
 *   Multisports' menu draws its labels (512, 170.5) and then glass bars (170.5,
 *   palette alpha 0xE) over them - the bars now stay behind;
 *   Cars' timer (25.1 -> z 0.04) is drawn with depth off before a checkered
 *   banner at z 0.98, which the hardware video shows behind the timer;
 *   the Cars help panel (0x4738) no longer covers its text (0x474D), and Bratz's
 *   "Confirm" button (hardware video) appears.
 * Dingo's title backgrounds (512/1024) do not hide its 3D characters (a depth
 * clear follows them). VFLASH_SPRD=0 turns sprite depth off. */
static int sprd = -1;
/* 0x88000000 | m: m = 2 turns blending on; 0x8D000000 | k: the blend factor
 * per channel (0xFFFFFF opaque). Cars fades its logos with a full-screen
 * white sprite under 88000002 + 8D000000..8DFFFFFF; Disney Princess draws its
 * text with 8DFCFCFC. The texture's own alpha (4444 formats) applies on top. */
static uint32_t blend, blend_k = 0xFFFFFF;

/* Write a texel/colour: c = BGR555 | alpha << 16, alpha 1-16 (16 = opaque). */
static uint16_t blend_px(uint16_t dst, uint32_t c) {
    int al = (int)(c >> 16 & 31);
    if (al == 0 || al > 16) al = 16;
    int on = (blend & 3) == 2;
    if (al == 16 && (!on || blend_k == 0xFFFFFF)) return (uint16_t)c;
    int r = 0;
    for (int k = 0; k < 3; k++) {
        int f = al * 255 * 16;                          /* weight, of 16*255*16 */
        if (on) f = al * (int)(blend_k >> (8 * k) & 255) * 16;
        int a = dst >> (5 * k) & 31, b = (int)(c >> (5 * k)) & 31;
        r |= ((b * f + a * (16 * 255 * 16 - f)) / (16 * 255 * 16)) << (5 * k);
    }
    return (uint16_t)r;
}

/* 0x1E000000 | m: bit 15 turns depth testing (and writing) on. Dingo
 * Rallye's race draws its sky dome round the camera with it off (0x4230)
 * and the world with it on (0xE230); the menus' 3D uses 0x8030. */
static int zenable = 1;
/* 0x1C: render mode (EXPERIMENT, VFLASH_CULL) */
static uint32_t reg1c = 0x6760;
/* 0x85 ... 0x96 around a 0xD3 fill: the fill clears the depth buffer over
 * the viewport instead (its rectangle is relative to the clip's top-left).
 * Both Dingo Rallye's menus and race do 85078100, D3 (0,0) 512x240 with
 * 0x3A00/0x38E8 (a far depth), 96000000 once a frame. */
static int zfill;

/* Clipped like everything else: Dingo Rallye's menu frame fills (0,0) 512x240
 * with 0x38E8 under a (512,480)-(1023,719) clip, and once it moves the
 * surface's top up to row 128 (+0x98 = 0x10010000) that fill would land on
 * the ROM kernel's RAM. */
static void fill(GE *g, int x0, int y0, int w, int h, uint16_t c) {
    int x1 = x0 + w, y1 = y0 + h;
    if (g->clip_x1 || g->clip_y1) {
        if (x0 < g->clip_x0) x0 = g->clip_x0;
        if (y0 < g->clip_y0) y0 = g->clip_y0;
        if (x1 > g->clip_x1 + 1) x1 = g->clip_x1 + 1;
        if (y1 > g->clip_y1 + 1) y1 = g->clip_y1 + 1;
    }
    /* what px() would refuse anyway, so that a row is a run of stores */
    if (x0 < 0) x0 = 0;
    if (x1 > 1024) x1 = 1024;
    if (y0 < g->top) y0 = g->top;
    for (int y = y0; y < y1; y++) {
        uint32_t o = g->surface + (uint32_t)(y >> 3) * 16384 + (uint32_t)(y & 7) * 64 - RAM_BASE;
        if ((uint64_t)o + 16384 > g->ram_size) {   /* a row partly outside RAM */
            for (int x = x0; x < x1; x++) {
                uint16_t *p = px(g, x, y);
                if (p) { *p = c; if (y < 1024) { zbuf[y * 1024 + x] = 1e30f; abuf[y * 1024 + x] = 0; } watch(g, x, y, c); }
            }
            continue;
        }
        uint8_t *row = g->ram + o;
        for (int x = x0; x < x1; x++) ((uint16_t *)(row + (x >> 5) * 512))[x & 31] = c;
        if (y < 1024) {
            for (int x = x0; x < x1; x++) zbuf[y * 1024 + x] = 1e30f;
            if (x1 > x0) memset(abuf + y * 1024 + x0, 0, (size_t)(x1 - x0));
        }
        if (watch_x >= 0)
            for (int x = x0; x < x1; x++) watch(g, x, y, c);
    }
}

/* A texel of the current texture as a BGR555 colour; returns 0 for a
 * transparent one, else colour | 0x10000. Transparent is bit 15 set (as for
 * the video engine's palette) or 0x0000: Dingo Rallye's menu icons sit in
 * 0xFFFF, its banner's outside is index 0 = 0x968D, while other palettes
 * hold real colours at index 0. A texture is a
 * region of the (tiled) surface itself, from the 0x87 position: 16 bpp
 * texels are surface pixels, 8 bpp pack two and 4 bpp four to a pixel (low
 * bits first) - so a 512-wide 8 bpp texture spans 256 surface pixels. Its
 * size is 0x86 bits 2-0 / 5-3 (log2(size / 8)); coordinates wrap. The
 * palette is a row of surface pixels at the last 0x8F-0x94 position. */
/* A BGR555 texel: 0 and bit 15 are transparent. */
static uint32_t c555(uint16_t p) {
    return p && !(p & 0x8000) ? p | 0x100000u : 0;
}

/* Destination alpha. Every sprite / triangle pixel leaves the alpha of what it
 * drew (1-16) when that came from a 4444 texture, 0 otherwise; fills clear it.
 * Under 0x88 = 0x42 (blending + bit 6) a sprite with a non-4444 texture takes
 * its alpha from there instead of the texel: Bratz's menu draws white 4444
 * swirls, then a grey gradient "glint" over the same 128x128 - on hardware the
 * glint only shows inside the swirls, tinted by its colour word (gold -> purple,
 * animating; video). Kept apart from the pixels because bit 15 of the surface is
 * the video engine's transparency. VFLASH_DSTA=0 disables. */

static uint32_t opaque(uint16_t *c) {
    return c ? c555(*c) : 0;
}

/* Texture format 0x86 bit 8: palette entries (or 16 bpp texels) are 4444
 * with alpha in the top nibble (Disney Princess's text: white with an alpha
 * ramp; Dingo Rallye's blob shadow: black with one) instead of BGR555 with
 * bit 15 = transparent. Returns BGR555 | alpha (1-16) << 16, 0 = invisible. */
static uint32_t argb4444(uint16_t v) {
    static int order = -1;
    if (order < 0) order = getenv("VFLASH_4444") ? atoi(getenv("VFLASH_4444")) : 0;
    int a = v >> 12;
    if (!a) return 0;
    /* Not inverted under 0x88 bit 6: that bit is set on nearly every draw (0x40 is
     * the normal state, 0x42 = blending on) - reading the nibble as transparency
     * there made Bratz's parchment menu and clothes display see-through. Open:
     * Multisports' menu bars (4 bpp, palette 0FFF..EFFF, mostly E, 0x42) cover
     * their labels, which are drawn before them. */
    int c0 = v & 15, c1 = v >> 4 & 15, c2 = v >> 8 & 15;   /* low nibble first */
    int r = order ? c2 : c0, b = order ? c0 : c2;
    uint32_t bgr = (uint32_t)((r << 1 | r >> 3) | (c1 << 1 | c1 >> 3) << 5 | (b << 1 | b >> 3) << 10);
    return bgr | (uint32_t)(a + 1) << 16;
}

static uint32_t tex_colour(GE *g, uint16_t *p) {
    if (!p) return 0;
    if (g->tex_fmt & 0x100) return argb4444(*p);
    return opaque(p);
}

static uint32_t pal_colour(GE *g, int i) {
    return tex_colour(g, px(g, g->pal_x + i, g->pal_y));
}

static uint32_t texel(GE *g, int u, int v) {
    int w = 8 << (g->tex_fmt & 7), h = 8 << (g->tex_fmt >> 3 & 7);
    u &= w - 1; v &= h - 1;
    uint16_t *p;
    switch (g->tex_bpp) {
    case 4:
        p = px(g, g->tex_x + (u >> 2), g->tex_y + v);
        return p ? pal_colour(g, *p >> ((u & 3) * 4) & 15) : 0;
    case 8:
        p = px(g, g->tex_x + (u >> 1), g->tex_y + v);
        return p ? pal_colour(g, *p >> ((u & 1) * 8) & 255) : 0;
    default:
        return tex_colour(g, px(g, g->tex_x + u, g->tex_y + v));
    }
}


/* 0xC8ffnnnn: a sprite. f bit 0 = texture coordinates follow, bit 2 = a
 * colour follows (0x10A34A10). Words: attributes, y << 16 | x (signed,
 * from the origin), h << 16 | w, [v << 16 | u], [colour]. */
static void tex_setup(GE *g);
static void tex_off(void);
static uint32_t tex_get(GE *g, int u, int v);

static void sprite(GE *g, uint32_t hdr, uint32_t pc) {
    g->sprites++;
    uint32_t f = hdr >> 16 & 0xFF;
    uint32_t pos = rd(g, pc + 8), size = rd(g, pc + 12);
    uint32_t uv = (f & 1) ? rd(g, pc + 16) : 0;
    int x0 = g->org_x + (int16_t)(pos & 0xFFFF), y0 = g->org_y + (int16_t)(pos >> 16);
    int w = (int)(size & 0xFFFF), h = (int)(size >> 16);
    int u0 = (int)(uv & 0xFFFF), v0 = (int)(uv >> 16);
    if (g->trace)
        printf("[GET]   sprite at (%d,%d) %dx%d uv (%d,%d) tex (%d,%d) %d bpp fmt %03X pal (%d,%d) clip (%d,%d)-(%d,%d)\n",
               x0, y0, w, h, u0, v0, g->tex_x, g->tex_y, g->tex_bpp, g->tex_fmt & 0xFFF, g->pal_x, g->pal_y,
               g->clip_x0, g->clip_y0, g->clip_x1, g->clip_y1);
    /* only the part inside the clip rectangle */
    int xs = g->clip_x0 > x0 ? g->clip_x0 - x0 : 0, ys = g->clip_y0 > y0 ? g->clip_y0 - y0 : 0;
    if (x0 + w - 1 > g->clip_x1) w = g->clip_x1 - x0 + 1;
    if (y0 + h - 1 > g->clip_y1) h = g->clip_y1 - y0 + 1;
    if (f & 1) {
        if ((w - xs) * (h - ys) >= 256) tex_setup(g);
        else tex_off();
    }
    if (sprd < 0) sprd = !getenv("VFLASH_SPRD") || atoi(getenv("VFLASH_SPRD")) != 0;
    float sa = (float)ge_f16_to((uint16_t)rd(g, pc + 4)), sz = sa > 0 ? 1.0f / sa : 1e30f;
    int sdepth = sprd && !g->nodepth;
    /* colour word (flag bit 2), 00BBGGRR: tints the texels; normally 00FFFFFF */
    static int dsta = -1;
    if (dsta < 0) dsta = !getenv("VFLASH_DSTA") || atoi(getenv("VFLASH_DSTA"));
    uint32_t cw = (f & 4) ? rd(g, pc + ((f & 1) ? 20 : 16)) : 0xFFFFFFu;
    int tint = (f & 1) && (cw & 0xFFFFFF) != 0xFFFFFF;
    int t4444 = (f & 1) && (g->tex_fmt & 0x100);
    int da = dsta && (f & 1) && !t4444 && (blend & 0x43) == 0x42;
    const uint32_t flat = (f & 1) ? 0 : (rd(g, pc + 16) & 0xFFFF) | 0x100000u;
    for (int y = ys; y < h; y++)
        for (int x = xs; x < w; x++) {
            uint32_t c = (f & 1) ? tex_get(g, u0 + x, v0 + y) : flat;
            uint16_t *p;
            if (c && (p = px(g, x0 + x, y0 + y))) {
                int yy = y0 + y, xx = x0 + x;
                int in = yy >= 0 && yy < 1024 && xx >= 0 && xx < 1024;
                if (sdepth && in) {
                    float *zp = &zbuf[yy * 1024 + xx];
                    if (zenable && sz > *zp) continue;
                    *zp = sz;
                }
                if (da) {
                    int a = in ? abuf[yy * 1024 + xx] : 0;
                    if (!a) continue;
                    c = (c & 0x7FFFu) | (uint32_t)a << 16;
                }
                if (tint) {
                    uint32_t o = c & ~0x7FFFu;
                    for (int k = 0; k < 3; k++)
                        o |= ((c >> (5 * k) & 31) * (cw >> (8 * k) & 255) / 255) << (5 * k);
                    c = o;
                }
                if (in && !da) abuf[yy * 1024 + xx] = t4444 ? (uint8_t)(c >> 16 & 31) : 0;
                *p = blend_px(*p, c); g->pixels++;
                watch(g, x0 + x, y0 + y, *p);
            }
        }
}

/* ---- 3D models (first pass) ----
 *
 * 0x09000000 a calls the model at a, a list of:
 *   0x0A000000 b      vertex buffer at b
 *   0x40ffnnnn v...   n vertices inline, a triangle strip
 *   0x44ffnnnn i...   n indices, two to a word (low first): word offsets of
 *                     vertices in the buffer, a triangle list
 *   0x04000000        return
 * A vertex is five words: halves x y z, three more (a normal?), u v, then a
 * colour word (BGR555 in the low half). Transform, from the registers:
 *   0x3C-0x3E scale, 0x40-0x48 3x3 matrix, 0x4C-0x4E translation (4.12),
 *   0x50-0x58 a second 3x3 matrix and 0x5C-0x5E its translation (the view),
 *   0x78 / 0x79 x / y focal lengths in pixels (halves), about the origin
 *   set by 0x81. */
static int use_half(void) {
    static int on = -1;
    if (on < 0) on = getenv("VFLASH_HALF") && atoi(getenv("VFLASH_HALF"));
    return on;
}

double ge_f16_to(uint16_t h) {
    if (!(h & 0x7FFF)) return 0;
    /* (1 + m/512) * 2^(e-31) put together as a double's bits - the same
     * value ldexp gave, without a libm call per vertex coordinate */
    uint64_t bits = (uint64_t)(h & 0x8000) << 48 | (uint64_t)((h >> 9 & 63) - 31 + 1023) << 52 |
                    (uint64_t)(h & 511) << 43;
    double v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

uint16_t ge_f16_from(double v) {
    uint16_t s = v < 0 ? 0x8000 : 0;
    v = fabs(v);
    if (!(v > 0)) return s;
    int e;
    double m = frexp(v, &e);                  /* v = m * 2^e, 0.5 <= m < 1 */
    long mm = lround((m * 2 - 1) * 512);      /* 9 mantissa bits, rounded */
    e += 30;                                  /* biased exponent of 1.m */
    if (mm == 512) { mm = 0; e++; }
    if (e < 0) return s;
    if (e > 63) return (uint16_t)(s | 0x7FFF);
    return (uint16_t)(s | e << 9 | mm);
}

static float half(uint16_t h) {
    if (!use_half()) return (float)ge_f16_to(h);
    int s = h >> 15, e = h >> 10 & 31, m = h & 1023;
    float v;
    if (e == 0) v = (float)m / 1024.0f / 16384.0f;
    else if (e == 31) v = 65504.0f;
    else { v = 1.0f + (float)m / 1024.0f; int k = e - 15; while (k > 0) { v *= 2; k--; } while (k < 0) { v /= 2; k++; } }
    return s ? -v : v;
}

/* Matrix and translation registers: 16-bit floats made by the math unit's
 * 20.12 converter (4.12 fixed under VFLASH_HALF). */
static float fx(uint16_t r) { return use_half() ? (float)(int16_t)r / 4096.0f : (float)ge_f16_to(r); }

typedef struct { float x, y, z, u, v; uint16_t c; int ok; float vx, vy; float l[3]; } V3;

/* Render state from the game kernel's renderer (Dingo Rallye 0x10A7A6xx-0x10A7AB6C,
 * setters in class order; emitters 0x10A34C58-0x10A34E58):
 *   0x84 f   flags, default 0x17: bits 0|1 lighting (a material flag; bit 1 off
 *            when the kernel's global light switch is), bits 2/4 a two-boolean
 *            setter (meaning unknown), bit 3 fog on, bit 5 fog mode.
 *   0x8A c   fog colour; 0x8B / 0x8C fog start / end (engine floats).
 *   Registers 0x60/0x64/0x68: light 0-2 direction (x y z floats), 0x6C-0x6E
 *   their colours (BGR555), 0x70 ambient colour. No retail frame seen so far
 *   turns fog on (0x8A-0x8C are all zero); Dingo's race lights with ambient only. */
static uint32_t rflags = 0x17, fog_col, fog_start, fog_end;
static int up_x, up_y, up_w, up_h;   /* 0xD0 upload rectangle */

static void bgr_f(uint32_t c, float o[3]) {
    for (int k = 0; k < 3; k++) o[k] = (float)(c >> (5 * k) & 31) / 31.0f;
}

static void light_vertex(GE *g, V3 *o, float nx, float ny, float nz) {
    static int off = -1;
    if (off < 0) off = getenv("VFLASH_NOLIGHT") != NULL;
    o->l[0] = o->l[1] = o->l[2] = 1.0f;
    if (off || !(rflags & 1)) return;
    float m[9];
    for (int i = 0; i < 9; i++) m[i] = fx((uint16_t)g->reg[0x40 + i]);
    float wx = m[0] * nx + m[1] * ny + m[2] * nz, wy = m[3] * nx + m[4] * ny + m[5] * nz,
          wz = m[6] * nx + m[7] * ny + m[8] * nz;
    float len = sqrtf(wx * wx + wy * wy + wz * wz);
    if (len > 0) { wx /= len; wy /= len; wz /= len; }
    float acc[3];
    bgr_f(g->reg[0x70], acc);
    for (int i = 0; i < 3; i++) {
        float lx = half((uint16_t)g->reg[0x60 + 4 * i]), ly = half((uint16_t)g->reg[0x61 + 4 * i]),
              lz = half((uint16_t)g->reg[0x62 + 4 * i]);
        float ll = sqrtf(lx * lx + ly * ly + lz * lz);
        if (ll == 0) continue;
        float d = (wx * lx + wy * ly + wz * lz) / ll;
        if (d <= 0) continue;
        float col[3];
        bgr_f(g->reg[0x6C + i], col);
        for (int k = 0; k < 3; k++) acc[k] += d * col[k];
    }
    for (int k = 0; k < 3; k++) o->l[k] = acc[k] > 1 ? 1 : acc[k];
}

/* Light and fog one pixel's colour (BGR555 | alpha << 16). */
static uint32_t shade(uint32_t col, const float l[3], float z) {
    float f = 0;
    if ((rflags & 8) && fog_end != fog_start) {
        float fs = half((uint16_t)fog_start), fe = half((uint16_t)fog_end);
        f = (z - fs) / (fe - fs);
        f = f < 0 ? 0 : f > 1 ? 1 : f;
    }
    if (l[0] >= 1 && l[1] >= 1 && l[2] >= 1 && f == 0) return col;
    uint32_t out = col & ~0x7FFFu;
    for (int k = 0; k < 3; k++) {
        float c = (float)(col >> (5 * k) & 31) * l[k];
        float fc = fog_col > 0xFFFF ? (float)(fog_col >> (8 * k) & 255) / 255.0f * 31 : (float)(fog_col >> (5 * k) & 31);
        c = c * (1 - f) + fc * f;
        out |= (uint32_t)(c + 0.5f) << (5 * k);
    }
    return out;
}

/* A vertex's words, by the record's flags byte f: x|y, z (and the normal's
 * first half), then bit 3: the rest of the normal, bit 4: u|v, bit 5: a
 * colour. Dingo Rallye's 0x38 is five words, Bratz's 0x10 quads three. */
static uint32_t vtx_words(uint32_t f) { return 2 + (f >> 3 & 1) + (f >> 4 & 1) + (f >> 5 & 1); }

/* The near plane: register 0x74 (0x3200 = 1/64 in Dingo Rallye; 0x75 looks
 * like the far plane). Triangles crossing it are clipped, not dropped. */
static float near_z(GE *g) {
    float n = half((uint16_t)g->reg[0x74]);
    return n > 0 ? n : 0.01f;
}

static void project(GE *g, V3 *o) {
    float f_x = half((uint16_t)g->reg[0x78]), f_y = half((uint16_t)g->reg[0x79]);
    o->ok = o->z >= near_z(g) * 0.999f;
    o->x = o->ok ? (float)g->org_x + o->vx * f_x / o->z : 0;
    o->y = o->ok ? (float)g->org_y + o->vy * f_y / o->z : 0;
}

static V3 xform(GE *g, uint32_t a, uint32_t f) {
    V3 o;
    /* x|y, z|nx, then ny|nz (flags bit 5), u|v (bit 4), colour (bit 3) */
    uint32_t uvo = 2 + (f >> 5 & 1);
    uint32_t w0 = rd(g, a), w1 = rd(g, a + 4);
    uint32_t w3 = (f & 0x10) ? rd(g, a + 4 * uvo) : 0;
    uint32_t w4 = (f & 0x08) ? rd(g, a + 4 * (uvo + (f >> 4 & 1))) : 0x7FFF;
    float x = half((uint16_t)w0), y = half((uint16_t)(w0 >> 16)), z = half((uint16_t)w1);
    uint16_t *R = (uint16_t *)g->reg;
    (void)R;
    x *= fx((uint16_t)g->reg[0x3C]); y *= fx((uint16_t)g->reg[0x3D]); z *= fx((uint16_t)g->reg[0x3E]);
    float m[9], v[9];
    for (int i = 0; i < 9; i++) { m[i] = fx((uint16_t)g->reg[0x40 + i]); v[i] = fx((uint16_t)g->reg[0x50 + i]); }
    float wx = m[0] * x + m[1] * y + m[2] * z + fx((uint16_t)g->reg[0x4C]);
    float wy = m[3] * x + m[4] * y + m[5] * z + fx((uint16_t)g->reg[0x4D]);
    float wz = m[6] * x + m[7] * y + m[8] * z + fx((uint16_t)g->reg[0x4E]);
    float vx = v[0] * wx + v[1] * wy + v[2] * wz + fx((uint16_t)g->reg[0x5C]);
    float vy = v[3] * wx + v[4] * wy + v[5] * wz + fx((uint16_t)g->reg[0x5D]);
    float vz = v[6] * wx + v[7] * wy + v[8] * wz + fx((uint16_t)g->reg[0x5E]);
    o.vx = vx; o.vy = vy; o.z = vz;
    project(g, &o);
    o.u = half((uint16_t)w3); o.v = half((uint16_t)(w3 >> 16));
    o.c = (uint16_t)w4;
    if (f & 0x20) light_vertex(g, &o, half((uint16_t)(w1 >> 16)), half((uint16_t)rd(g, a + 8)), half((uint16_t)(rd(g, a + 8) >> 16)));
    else light_vertex(g, &o, 0, 0, 0), o.l[0] = o.l[1] = o.l[2] = 1.0f;
    return o;
}

/* The triangle rasteriser's texture, set up once per triangle (tex_setup):
 * the palette decoded, and the region checked to lie inside RAM and the
 * surface, so a texel is an address, a load and a table lookup - the same
 * result as texel(), which it falls back to otherwise. Textured pixels
 * dominate (SpongeBob's loading screen: ~770k a frame, 4 texels each). */
static struct {
    int ok, bpp, wm, hm, tx, ty, a4444;
    int32_t soff;             /* surface row 0 relative to RAM_BASE (may be < 0) */
    const uint8_t *ram;
    uint32_t pal[256];
} tc;

static void tex_setup(GE *g) {
    int w = 8 << (g->tex_fmt & 7), h = 8 << (g->tex_fmt >> 3 & 7);
    int bpp = g->tex_bpp == 4 || g->tex_bpp == 8 ? g->tex_bpp : 16;
    int sw = bpp == 4 ? w >> 2 : bpp == 8 ? w >> 1 : w;   /* surface pixels wide */
    tc.ok = 0;
    if (g->tex_x < 0 || g->tex_x + sw > 1024 || g->tex_y < g->top || g->tex_y + h > 4096) return;
    int64_t first = (int64_t)(int32_t)(g->surface - RAM_BASE) + (int64_t)(g->tex_y >> 3) * 16384;
    int64_t last = (int64_t)(int32_t)(g->surface - RAM_BASE) + (int64_t)((g->tex_y + h - 1) >> 3) * 16384 + 16384;
    if (first < 0 || last > (int64_t)g->ram_size) return;
    tc.bpp = bpp; tc.wm = w - 1; tc.hm = h - 1; tc.tx = g->tex_x; tc.ty = g->tex_y;
    tc.a4444 = (g->tex_fmt & 0x100) != 0;
    tc.soff = (int32_t)(g->surface - RAM_BASE);
    tc.ram = g->ram;
    int n = bpp == 4 ? 16 : bpp == 8 ? 256 : 0;
    /* The palette row, decoded again only when its words or format changed:
     * every textured triangle sets the texture up, mostly with the palette
     * of the one before (SpongeBob's beach: ~10 % of the time went here) */
    static uint16_t raw[256], last_raw[256];
    static int last_n = -1, last_4444;
    int direct = n && g->pal_x >= 0 && g->pal_x + n <= 1024 && g->pal_y >= g->top;
    if (direct) {
        uint32_t o = g->surface + (uint32_t)(g->pal_y >> 3) * 16384 + (uint32_t)(g->pal_y & 7) * 64 - RAM_BASE;
        direct = (uint64_t)o + 16384 <= g->ram_size;
        if (direct)
            for (int i = 0; i < n; i++) {
                int x = g->pal_x + i;
                memcpy(&raw[i], g->ram + o + (x >> 5) * 512 + (x & 31) * 2, 2);
            }
    }
    if (direct && n == last_n && tc.a4444 == last_4444 && !memcmp(raw, last_raw, (size_t)n * 2))
        ;   /* tc.pal still holds it */
    else {
        for (int i = 0; i < n; i++) tc.pal[i] = pal_colour(g, i);
        last_n = direct ? n : -1;
        last_4444 = tc.a4444;
        if (direct) memcpy(last_raw, raw, (size_t)n * 2);
    }
    tc.ok = 1;
}

static inline uint32_t tfetch(int u, int v) {
    u &= tc.wm; v &= tc.hm;
    int y = tc.ty + v, x = tc.tx + (tc.bpp == 4 ? u >> 2 : tc.bpp == 8 ? u >> 1 : u);
    uint16_t p;
    memcpy(&p, tc.ram + tc.soff + (y >> 3) * 16384 + (x >> 5) * 512 + (y & 7) * 64 + (x & 31) * 2, 2);
    if (tc.bpp == 4) return tc.pal[p >> ((u & 3) * 4) & 15];
    if (tc.bpp == 8) return tc.pal[p >> ((u & 1) * 8) & 255];
    if (tc.a4444) return argb4444(p);
    return c555(p);
}

#define TEXEL(g, u, v) (tc.ok ? tfetch(u, v) : texel(g, u, v))

/* Sprites use the context too when big enough to pay for the palette decode
 * (per-texel pal_colour was ~930M calls in a Dingo boot + menus + race). */
static void tex_off(void) { tc.ok = 0; }
static uint32_t tex_get(GE *g, int u, int v) { return TEXEL(g, u, v); }

/* floor to int without a libm call (plain x86-64 has no SSE4.1 round) */
static inline int ifloor(float f) { int i = (int)f; return (float)i > f ? i - 1 : i; }

/* The 2x2 texels at (iu, iv)..(iu+1, iv+1) for the bilinear filter: two row
 * and two column addresses instead of four full lookups, one depth switch. */
static inline uint32_t tsurf(int x) { return (uint32_t)((x >> 5) * 512 + (x & 31) * 2); }
static inline uint16_t tload(int32_t o) { uint16_t p; memcpy(&p, tc.ram + o, 2); return p; }
static inline uint32_t tdirect(uint16_t p) {
    if (tc.a4444) return argb4444(p);
    return c555(p);
}

static inline void tquad(int iu, int iv, uint32_t c[4]) {
    int u0 = iu & tc.wm, u1 = (iu + 1) & tc.wm;
    int y0 = tc.ty + (iv & tc.hm), y1 = tc.ty + ((iv + 1) & tc.hm);
    int32_t r0 = tc.soff + (y0 >> 3) * 16384 + (y0 & 7) * 64, r1 = tc.soff + (y1 >> 3) * 16384 + (y1 & 7) * 64;
    switch (tc.bpp) {
    case 4: {
        uint32_t a = tsurf(tc.tx + (u0 >> 2)), b = tsurf(tc.tx + (u1 >> 2));
        int s0 = (u0 & 3) * 4, s1 = (u1 & 3) * 4;
        c[0] = tc.pal[tload(r0 + a) >> s0 & 15]; c[1] = tc.pal[tload(r0 + b) >> s1 & 15];
        c[2] = tc.pal[tload(r1 + a) >> s0 & 15]; c[3] = tc.pal[tload(r1 + b) >> s1 & 15];
        break;
    }
    case 8: {
        uint32_t a = tsurf(tc.tx + (u0 >> 1)), b = tsurf(tc.tx + (u1 >> 1));
        int s0 = (u0 & 1) * 8, s1 = (u1 & 1) * 8;
        c[0] = tc.pal[tload(r0 + a) >> s0 & 255]; c[1] = tc.pal[tload(r0 + b) >> s1 & 255];
        c[2] = tc.pal[tload(r1 + a) >> s0 & 255]; c[3] = tc.pal[tload(r1 + b) >> s1 & 255];
        break;
    }
    default: {
        uint32_t a = tsurf(tc.tx + u0), b = tsurf(tc.tx + u1);
        c[0] = tdirect(tload(r0 + a)); c[1] = tdirect(tload(r0 + b));
        c[2] = tdirect(tload(r1 + a)); c[3] = tdirect(tload(r1 + b));
    }
    }
}

/* Bilinear texture lookup for triangles (VFLASH_FILTER=0 turns it off):
 * point sampling left detailed textures, like Dingo Rallye's race ground,
 * as speckle. Falls back to the nearest texel next to transparent ones. */
static uint32_t filtered(GE *g, float fu, float fv) {
    static int on = -1;
    if (on < 0) on = !getenv("VFLASH_FILTER") || atoi(getenv("VFLASH_FILTER"));
    if (!on) return TEXEL(g, (int)fu, (int)fv);
    fu -= 0.5f; fv -= 0.5f;
    int iu = ifloor(fu), iv = ifloor(fv);
    float du = fu - (float)iu, dv = fv - (float)iv;
    uint32_t c[4];
    if (tc.ok) tquad(iu, iv, c);
    else {
        c[0] = texel(g, iu, iv); c[1] = texel(g, iu + 1, iv);
        c[2] = texel(g, iu, iv + 1); c[3] = texel(g, iu + 1, iv + 1);
    }
    if (!c[0] || !c[1] || !c[2] || !c[3])
        return c[(du >= 0.5f) + 2 * (dv >= 0.5f)];
    float wt[4] = { (1 - du) * (1 - dv), du * (1 - dv), (1 - du) * dv, du * dv };
    uint32_t out = 0;
    float al = 0;
    for (int k = 0; k < 3; k++) {
        float acc = 0;
        for (int i = 0; i < 4; i++) acc += wt[i] * (float)(c[i] >> (5 * k) & 31);
        out |= (uint32_t)(acc + 0.5f) << (5 * k);
    }
    for (int i = 0; i < 4; i++) al += wt[i] * (float)(c[i] >> 16 & 31);
    return out | (uint32_t)(al + 0.5f) << 16;
}

static float edge(float ax, float ay, float bx, float by, float cx, float cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static void raster(GE *g, V3 a, V3 b, V3 c) {
    if (!a.ok || !b.ok || !c.ok || g->no3d) return;
    float area = edge(a.x, a.y, b.x, b.y, c.x, c.y);
    {
        static int cull = -1;
        if (cull < 0) cull = getenv("VFLASH_CULL") ? atoi(getenv("VFLASH_CULL")) : 0;
        if (cull && (reg1c & (1u << (cull >> 4 ? cull >> 4 : 6)))) {
            if ((cull & 15) == 1 && area < 0) return;
            if ((cull & 15) == 2 && area > 0) return;
        }
    }
    if (area == 0) return;
    int x0 = (int)fminf(a.x, fminf(b.x, c.x)), x1 = (int)fmaxf(a.x, fmaxf(b.x, c.x)) + 1;
    int y0 = (int)fminf(a.y, fminf(b.y, c.y)), y1 = (int)fmaxf(a.y, fmaxf(b.y, c.y)) + 1;
    if (x0 < g->clip_x0) x0 = g->clip_x0;
    if (y0 < g->clip_y0) y0 = g->clip_y0;
    if (x1 > g->clip_x1) x1 = g->clip_x1;
    if (y1 > g->clip_y1) y1 = g->clip_y1;
    if (x1 - x0 > 1024 || y1 - y0 > 1024) return;
    if (y0 < 0) y0 = 0;
    if (y1 > 1023) y1 = 1023;
    int tw = 8 << (g->tex_fmt & 7), th = 8 << (g->tex_fmt >> 3 & 7);
    g->tris++;
    /* Per triangle: each attribute over z at the vertices (perspective-correct
     * interpolation), and the edge functions as w = e * x + f(y) for the spans. */
    float ra = 1.0f / a.z, rb = 1.0f / b.z, rc = 1.0f / c.z;
    float ua = a.u * ra * (float)tw, ub = b.u * rb * (float)tw, uc = c.u * rc * (float)tw;
    float va = a.v * ra * (float)th, vb = b.v * rb * (float)th, vc = c.v * rc * (float)th;
    float la[3], lb[3], lc[3];
    int lit = 0;
    for (int k = 0; k < 3; k++) {
        la[k] = a.l[k] * ra; lb[k] = b.l[k] * rb; lc[k] = c.l[k] * rc;
        lit |= a.l[k] < 1 || b.l[k] < 1 || c.l[k] < 1;
    }
    int fog = (rflags & 8) && fog_end != fog_start;
    if (g->tex_bpp) tex_setup(g);
    float rarea = 1.0f / area;
    /* w0 = ((c.x - b.x) (py - b.y) - (c.y - b.y) (px - b.x)) / area, likewise w1:
     * linear in px with slopes -(c.y - b.y) / area and -(a.y - c.y) / area */
    float s0 = -(c.y - b.y) * rarea, s1 = -(a.y - c.y) * rarea;
    int ztest = zenable && !g->nodepth;
    for (int y = y0; y <= y1; y++) {
        float py_ = (float)y + 0.5f;
        /* the span where w0, w1, w2 >= 0 on this row (one pixel of slack each
         * side; the exact test below still decides) */
        float w0x = ((c.x - b.x) * (py_ - b.y) + (c.y - b.y) * b.x) * rarea;   /* w0 at px = 0 */
        float w1x = ((a.x - c.x) * (py_ - c.y) + (a.y - c.y) * c.x) * rarea;
        float lo = (float)x0, hi = (float)x1 + 1;
        /* w = s * px + w_at_0 >= 0 */
        float ss[3] = { s0, s1, -s0 - s1 }, ww[3] = { w0x, w1x, 1.0f - w0x - w1x };
        for (int i = 0; i < 3; i++) {
            if (ss[i] > 0) { float t = -ww[i] / ss[i]; if (t > lo) lo = t; }
            else if (ss[i] < 0) { float t = -ww[i] / ss[i]; if (t < hi) hi = t; }
            else if (ww[i] < -1e-4f) { lo = hi + 1; break; }
        }
        int xs = (int)floorf(lo - 0.5f) - 1, xe = (int)ceilf(hi - 0.5f) + 1;
        if (xs < x0) xs = x0;
        if (xe > x1) xe = x1;
        for (int x = xs; x <= xe; x++) {
            float px_ = (float)x + 0.5f;
            float w0 = edge(b.x, b.y, c.x, c.y, px_, py_) * rarea;
            float w1 = edge(c.x, c.y, a.x, a.y, px_, py_) * rarea;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float z = 1.0f / (w0 * ra + w1 * rb + w2 * rc);
            float *zp = &zbuf[y * 1024 + x];
            if (ztest && z >= *zp) continue;
            uint32_t col;
            if (g->tex_bpp) {
                col = filtered(g, (w0 * ua + w1 * ub + w2 * uc) * z, (w0 * va + w1 * vb + w2 * vc) * z);
                if (!col) continue;
            } else col = a.c | 0x100000u;
            if (lit || fog) {
                float l[3];
                for (int k = 0; k < 3; k++) l[k] = (w0 * la[k] + w1 * lb[k] + w2 * lc[k]) * z;
                col = shade(col, l, z);
            }
            uint16_t *p = px(g, x, y);
            if (p) {
                *p = blend_px(*p, col); if (zenable) *zp = z; g->pixels++; watch(g, x, y, *p);
                abuf[y * 1024 + x] = g->tex_bpp && (g->tex_fmt & 0x100) ? (uint8_t)(col >> 16 & 31) : 0;
            }
        }
    }
}

/* Near plane: triangles entirely in front are drawn; crossing ones are
 * rejected, or with VFLASH_CLIP=1 clipped in view space (Sutherland-Hodgman)
 * and rasterised as a fan. */
static V3 lerp_v(GE *g, V3 p, V3 q, float nz) {
    float t = (nz - p.z) / (q.z - p.z);
    V3 o = p;
    o.vx = p.vx + (q.vx - p.vx) * t; o.vy = p.vy + (q.vy - p.vy) * t; o.z = nz;
    o.u = p.u + (q.u - p.u) * t; o.v = p.v + (q.v - p.v) * t;
    for (int k = 0; k < 3; k++) o.l[k] = p.l[k] + (q.l[k] - p.l[k]) * t;
    project(g, &o);
    return o;
}

/* Per model call, for the trace: triangles submitted, all behind the near plane,
 * crossing it, and the screen box of the ones in front. */
static int st_sub, st_behind, st_cross;
static float st_x0, st_y0, st_x1, st_y1, st_z0, st_z1;

static void tri(GE *g, V3 a, V3 b, V3 c) {
    if (g->trace) {
        st_sub++;
        if (!a.ok && !b.ok && !c.ok) st_behind++;
        else if (!a.ok || !b.ok || !c.ok) st_cross++;
        else {
            st_x0 = fminf(st_x0, fminf(a.x, fminf(b.x, c.x))); st_x1 = fmaxf(st_x1, fmaxf(a.x, fmaxf(b.x, c.x)));
            st_y0 = fminf(st_y0, fminf(a.y, fminf(b.y, c.y))); st_y1 = fmaxf(st_y1, fmaxf(a.y, fmaxf(b.y, c.y)));
            st_z0 = fminf(st_z0, fminf(a.z, fminf(b.z, c.z))); st_z1 = fmaxf(st_z1, fmaxf(a.z, fmaxf(b.z, c.z)));
        }
    }
    if (a.ok && b.ok && c.ok) { raster(g, a, b, c); return; }
    /* Triangles crossing the near plane are clipped (VFLASH_CLIP=0 rejects them
     * instead). Rejecting lost SpongeBob's loading-screen counter top (the lower
     * fifth of the screen black) and the last road rows in Cars' race. It was the
     * default until 2026-09-27 because clipping drew junk - Bratz stripes, a
     * Dingo menu wedge, Shrek title particles - but the stripes were the then
     * unwritten skinning buffer at 0x10D44060, and none of the junk reproduced
     * with the skinning / vertex-layout fixes in. */
    static int clip = -1;
    if (clip < 0) clip = !getenv("VFLASH_CLIP") || atoi(getenv("VFLASH_CLIP"));
    if ((!a.ok && !b.ok && !c.ok) || !clip) return;
    float nz = near_z(g);
    static int cliplog = -1;
    if (cliplog < 0) cliplog = getenv("VFLASH_CLIPLOG") != NULL;
    if (cliplog) {
        static int n;
        if (n++ < 40) printf("[CLIP] z %.3f %.3f %.3f  vx %.2f %.2f %.2f  vy %.2f %.2f %.2f near %.4f c %04X\n",
                             a.z, b.z, c.z, a.vx, b.vx, c.vx, a.vy, b.vy, c.vy, nz, a.c);
    }
    V3 in[3] = { a, b, c }, out[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        V3 p = in[i], q = in[(i + 1) % 3];
        if (p.ok) out[n++] = p;
        if (p.ok != q.ok) out[n++] = lerp_v(g, p, q, nz);
    }
    for (int i = 2; i < n; i++) raster(g, out[0], out[i - 1], out[i]);
}

/* Vertex records, which the command interpreter runs like any command:
 *   0x0A000000 b      vertex buffer at b
 *   0x40ffnnnn v...   n vertices inline, a triangle strip
 *   0x44ffnnnn ...    n vertices, a triangle list
 *   0x67ffnnnn v...   n vertices inline, a skinning store (below)
 *   0x6Fffnnnn v...   n vertices inline, a polygon (fan)
 * Flags ff bit 6 (0x78 rather than 0x38/0xB8): the record holds n 16-bit
 * indices into the buffer instead of the vertices (Spider-Man's 0x4438 has
 * inline vertices, every other 0x44 seen so far indices).
 * Returns the address after the record, or 0 if w is not one. */
static uint32_t vtx_at(GE *g, uint32_t a, uint32_t w, uint32_t i) {
    if (w >> 16 & 0x40)
        return g->vb + 4 * (rd(g, a + 4 + 4 * (i >> 1)) >> (16 * (i & 1)) & 0xFFFF);
    return a + 4 + 4 * vtx_words(w >> 16 & 0xFF) * i;
}

/* 0x67 records and flags bit 7 (every 0x6F seen is 0x6FB8): the engine's
 * skinning. Multisports stores its figures with 0x67380006 (6 vertices per
 * bone, 0x0C destinations 0x78 apart); drawing those as triangles put the
 * figures' bones behind the camera and, clipped, smeared them over the
 * screen. Instead of drawing,
 * each vertex is put through the model stage (scale 0x3C-0x3E, matrix 0x40-0x48,
 * translation 0x4C-0x4E; the normal through the matrix) and stored as a 5-word
 * 0x38-layout vertex at the vertex buffer (0x0C000000 a), the i-th at a + 20 i.
 * The game then draws the whole body with indexed 0x4078 records over that
 * buffer and identity model registers. Spider-Man's player: one 0x0C + store per
 * bone into 0x10D44060...; nothing else writes that buffer. */
static void store_vtx(GE *g, uint32_t src, uint32_t f, uint32_t dst) {
    uint32_t uvo = 2 + (f >> 5 & 1);
    uint32_t w0 = rd(g, src), w1 = rd(g, src + 4);
    uint32_t uv = (f & 0x10) ? rd(g, src + 4 * uvo) : 0;
    uint32_t col = (f & 0x08) ? rd(g, src + 4 * (uvo + (f >> 4 & 1))) : 0x7FFF;
    float x = half((uint16_t)w0) * fx((uint16_t)g->reg[0x3C]);
    float y = half((uint16_t)(w0 >> 16)) * fx((uint16_t)g->reg[0x3D]);
    float z = half((uint16_t)w1) * fx((uint16_t)g->reg[0x3E]);
    float n[3] = { 0, 0, 0 };
    if (f & 0x20) {
        uint32_t w2 = rd(g, src + 8);
        n[0] = half((uint16_t)(w1 >> 16)); n[1] = half((uint16_t)w2); n[2] = half((uint16_t)(w2 >> 16));
    }
    float m[9];
    for (int i = 0; i < 9; i++) m[i] = fx((uint16_t)g->reg[0x40 + i]);
    float o[3], on[3];
    for (int r = 0; r < 3; r++) {
        o[r] = m[3 * r] * x + m[3 * r + 1] * y + m[3 * r + 2] * z + fx((uint16_t)g->reg[0x4C + r]);
        on[r] = m[3 * r] * n[0] + m[3 * r + 1] * n[1] + m[3 * r + 2] * n[2];
    }
    if (g->trace && dst == g->vb)
        printf("[GET]   store %08X: (%.3f %.3f %.3f) -> (%.3f %.3f %.3f)\n", dst, x, y, z, o[0], o[1], o[2]);
    wr(g, dst, ge_f16_from(o[0]) | (uint32_t)ge_f16_from(o[1]) << 16);
    wr(g, dst + 4, ge_f16_from(o[2]) | (uint32_t)ge_f16_from(on[0]) << 16);
    wr(g, dst + 8, ge_f16_from(on[1]) | (uint32_t)ge_f16_from(on[2]) << 16);
    wr(g, dst + 12, uv);
    wr(g, dst + 16, col);
}

static uint32_t prim(GE *g, uint32_t a, uint32_t w) {
    uint32_t op = w >> 24, n = w & 0xFFFF;
    if (op == 0x0A) { g->vb = rd(g, a + 4); return a + 8; }
    if ((w >> 16 & 0x80) || op == 0x67) {
        uint32_t f = w >> 16 & 0xFF;
        for (uint32_t i = 0; i < n; i++) store_vtx(g, vtx_at(g, a, w, i), f, g->vb + 20 * i);
        return w >> 16 & 0x40 ? a + 4 + 4 * ((n + 1) >> 1) : a + 4 + 4 * vtx_words(w >> 16 & 0xFF) * n;
    }
    /* Triangle lists, every 3 vertices one triangle: 0x44 (indexed with flags bit 6 as in
     * every capture but Spider-Man's 0x4438 player model, inline without). */
    if (op == 0x44) {
        uint32_t f = w >> 16 & 0xFF;
        for (uint32_t i = 0; i + 2 < n; i += 3)
            tri(g, xform(g, vtx_at(g, a, w, i), f), xform(g, vtx_at(g, a, w, i + 1), f),
                xform(g, vtx_at(g, a, w, i + 2), f));
        return w >> 16 & 0x40 ? a + 4 + 4 * ((n + 1) >> 1) : a + 4 + 4 * vtx_words(w >> 16 & 0xFF) * n;
    }
    if (op == 0x40 || op == 0x6F) {
        uint32_t f = w >> 16 & 0xFF;
        V3 p0 = xform(g, vtx_at(g, a, w, 0), f), p1 = xform(g, vtx_at(g, a, w, 1), f);
        for (uint32_t i = 2; i < n; i++) {
            V3 p2 = xform(g, vtx_at(g, a, w, i), f);
            /* strips alternate winding: every second triangle is flipped back */
            if (op == 0x40 && (i & 1)) tri(g, p1, p0, p2);
            else tri(g, p0, p1, p2);
            if (op == 0x40) p0 = p1;
            p1 = p2;
        }
        return w >> 16 & 0x40 ? a + 4 + 4 * ((n + 1) >> 1) : a + 4 + 4 * vtx_words(w >> 16 & 0xFF) * n;
    }
    return 0;
}

static void run(GE *g, uint32_t pc, uint32_t end, int depth) {
    for (int steps = 0; steps < 100000; steps++) {
        if (end && pc >= end) return;
        /* A list that leaves RAM ends there. Bratz's particle lists end with
         * 0x08 a 0 - run a, then continue at 0 - i.e. nothing follows; running
         * on through 100000 zero words cost 72 runaways per audit run. */
        if (pc - RAM_BASE >= g->ram_size) {
            if (g->log) printf("[GE] list %08X left RAM at %08X (after %08X at %08X)\n", cur_list, pc, prev_w, prev_pc);
            return;
        }
        uint32_t w = rd(g, pc), op = w >> 24;
        if (g->trace) {
            printf("[GET] %08X %08X", pc, w);
            if (op >= 0xC0 && op <= 0xDF)
                for (uint32_t k = 1; k <= (w & 0xFFFF) && k <= 8; k++) printf(" %08X", rd(g, pc + 4 * k));
            if (op == 0x08) printf(" %08X %08X", rd(g, pc + 4), rd(g, pc + 8));
            if (op == 0x0C || op == 0x0A || op == 0x09) printf(" %08X (vb %08X)", rd(g, pc + 4), g->vb);
            if (op == 0x40 || op == 0x44 || op == 0x67 || op == 0x6F)
                for (uint32_t k = 1; k <= 5 * (w & 0xFFFF) + 3 && k <= 48; k++) printf(" %08X", rd(g, pc + 4 * k));
            if (op == 0x20)
                for (uint32_t k = 1; k <= ((w & 0xFFFF) + 1) / 2 && k <= 8; k++) printf(" %08X", rd(g, pc + 4 * k));
            printf("\n");
        }
        pc += 4;
        uint32_t pw = prev_w, ppc = prev_pc;
        prev_w = w; prev_pc = pc - 4;
        switch (op) {
        case 0x08: {
            uint32_t a = rd(g, pc), b = rd(g, pc + 4);
            pc += 8;
            if (depth < 8) run(g, a, b, depth + 1);
            pc = b;
            break;
        }
        case 0x09:   /* call the list at the next word; it ends with 0x04 */
            g->calls++;
            if (g->trace) {
                printf("[GET]   regs:");
                for (int r = 0; r < 256; r++) if (g->reg[r]) printf(" %02X=%04X", r, g->reg[r]);
                printf("\n");
            }
            if (g->trace) { st_sub = st_behind = st_cross = 0; st_x0 = st_y0 = st_z0 = 1e9f; st_x1 = st_y1 = st_z1 = -1e9f; }
            /* a call outside RAM (Bratz calls address 0 while loading) would run
             * 100000 zero words; the ROM there is not a model either */
            if (depth < 8 && rd(g, pc) - RAM_BASE < g->ram_size) run(g, rd(g, pc), 0, depth + 1);
            if (g->trace)
                printf("[GET]   call %08X: %d tris, %d behind, %d crossing; box (%.0f,%.0f)-(%.0f,%.0f) z %.3f..%.3f\n",
                       rd(g, pc), st_sub, st_behind, st_cross, st_x0, st_y0, st_x1, st_y1, st_z0, st_z1);
            pc += 4;
            break;
        case 0x04:   /* return from a called list */
            return;
        case 0x0A: case 0x40: case 0x44: case 0x67: case 0x6F:
            watch_what = "triangle"; watch_pc = pc - 4;
            pc = prim(g, pc - 4, w);
            break;
        case 0x00:   /* padding in the USA-revision kernels' lists (millions a run) */
            break;
        case 0x12:   /* 12000000 opens some models' strip records (Spider-Man's sea,
                      * Dingo's race track) - which render right without it; no effect
                      * modelled (a vertex-cache reset / strip start?) */
            break;
        case 0x88: blend = w & 0xFFFFFF; break;
        case 0x84: rflags = w & 0xFFFFFF; break;
        case 0x8A: fog_col = w & 0xFFFFFF; break;
        case 0x8B: fog_start = w & 0xFFFF; break;
        case 0x8C: fog_end = w & 0xFFFF; break;
        case 0x95:   /* sync: the sprite builders end each sprite (and each upload
                      * strip) with it; nothing to do when drawing is immediate */
            break;
        case 0xD0:   /* upload destination: D0000002 y<<16|x h<<16|w */
            up_x = (int)(rd(g, pc) & 0xFFFF); up_y = (int)(rd(g, pc) >> 16);
            up_w = (int)(rd(g, pc + 4) & 0xFFFF); up_h = (int)(rd(g, pc + 4) >> 16);
            pc += 4 * (w & 0xFFFF);
            break;
        case 0x0E: { /* 0E00nnnn src: copy n words of 16-bit pixels from src into the
                      * D0 rectangle, row by row (0x10A333B4 splits big images) */
            uint32_t src = rd(g, pc), n = w & 0xFFFF;
            pc += 4;
            for (uint32_t i = 0; i < 2 * n && up_w > 0; i++) {
                uint16_t *d = px(g, up_x + (int)(i % (uint32_t)up_w), up_y + (int)(i / (uint32_t)up_w));
                uint32_t v = rd(g, src + 4 * (i >> 1));
                if (d) *d = (uint16_t)(v >> (16 * (i & 1)));
            }
            break;
        }
        case 0xD1: { /* D1000002 y<<16|x h<<16|w: read a surface rectangle back into
                      * RAM at the 0x0C address, row by row, 2 pixels a word - the
                      * inverse of D0/0E. Bratz renders a 128x128 sprite off-screen,
                      * reads it back (D1 to 0x10E39A2C) and uploads it (D0/0E from
                      * there) as a texture: render-to-texture through RAM. */
            int rx = (int)(rd(g, pc) & 0xFFFF), ry = (int)(rd(g, pc) >> 16);
            int rw = (int)(rd(g, pc + 4) & 0xFFFF), rh = (int)(rd(g, pc + 4) >> 16);
            for (int i = 0; i < rw * rh; i += 2) {
                uint16_t *s0 = px(g, rx + i % rw, ry + i / rw), *s1 = px(g, rx + (i + 1) % rw, ry + (i + 1) / rw);
                wr(g, g->vb + 2 * (uint32_t)i, (uint32_t)(s0 ? *s0 : 0) | (uint32_t)(s1 ? *s1 : 0) << 16);
            }
            pc += 4 * (w & 0xFFFF);
            break;
        }
        case 0xD2: { /* D2000003 src y<<16|x  h<<16|w  dst y<<16|x: copy a surface
                      * rectangle. Bratz rotates a 512-wide strip at row 992 with two
                      * of these a frame (a scrolling texture). */
            int sx = (int)(rd(g, pc) & 0xFFFF), sy = (int)(rd(g, pc) >> 16);
            int cw_ = (int)(rd(g, pc + 4) & 0xFFFF), ch = (int)(rd(g, pc + 4) >> 16);
            int dx = (int)(rd(g, pc + 8) & 0xFFFF), dy = (int)(rd(g, pc + 8) >> 16);
            static uint16_t tmp[1024 * 64];
            if (cw_ > 0 && ch > 0 && cw_ * ch <= 1024 * 64) {
                for (int y = 0; y < ch; y++)
                    for (int x = 0; x < cw_; x++) { uint16_t *s = px(g, sx + x, sy + y); tmp[y * cw_ + x] = s ? *s : 0; }
                for (int y = 0; y < ch; y++)
                    for (int x = 0; x < cw_; x++) { uint16_t *d = px(g, dx + x, dy + y); if (d) *d = tmp[y * cw_ + x]; }
            }
            pc += 4 * (w & 0xFFFF);
            break;
        }
        case 0x89:   /* alone after a D0/0E upload (Bratz): texture-cache sync */
            break;
        case 0x8D: blend_k = w & 0xFFFFFF; break;
        case 0x85: zfill = 1; break;
        case 0x96: zfill = 0; break;
        /* 0x1C: render mode, default 0x006760. Bits 15-23 a per-object counter
         * (Cars / Bratz / SpongeBob world models, with 1E bit 2 = 0xE234), low bits
         * flags: bit 3 with that counter, bit 6 clear on SpongeBob's blender glass,
         * bits 6+9 clear on Dingo's title sky. Bit 6 is not plain backface culling
         * (VFLASH_CULL tried it: it deleted Spider-Man's and Dingo's skies, drawn
         * under 0x6760). No effect modelled yet. */
        case 0x1C: reg1c = w & 0xFFFFFF; break;
        /* 0x10000000 a: once a frame, alone in a list after the depth clear, with a
         * RAM address (Spider-Man 0x10B40128: zeroed at boot, never read by the CPU)
         * - a readback / fence destination. Two words; the address used to be run
         * as a command (the "unknown op 10" at 10Bxxxxx). */
        case 0x10: pc += 4; break;
        case 0x1E:
            zenable = (w & 0x8000) != 0;
            break;
        case 0x0C:   /* 0x0C000000 a: the vertex buffer for the model called next
                      * (like 0x0A inside a model; Bratz's indexed models rely on it) */
            g->vb = rd(g, pc);
            pc += 4;
            break;
        case 0x14:
            if ((w & 0xFF00) == 0xFD00) return;
            g->unknown[op]++;
            break;
        case 0x20: {
            uint32_t r = w >> 16 & 0xFF, n = w & 0xFFFF;
            /* 16-bit registers, two to a word, low half first */
            for (uint32_t i = 0; i < n && i < 256; i++)
                g->reg[(r + i) & 0xFF] = rd(g, pc + 4 * (i >> 1)) >> (16 * (i & 1)) & 0xFFFF;
            pc += 4 * ((n + 1) >> 1);
            break;
        }
        case 0x81: g->org_y = w >> 12 & 0xFFF; g->org_x = w & 0xFFF; break;
        case 0x82: g->clip_y0 = w >> 12 & 0xFFF; g->clip_x0 = w & 0xFFF; break;
        case 0x83: g->clip_y1 = w >> 12 & 0xFFF; g->clip_x1 = w & 0xFFF; break;
        case 0x86: g->tex_fmt = w & 0xFFFFFF; break;
        case 0x87: g->tex_y = w >> 12 & 0xFFF; g->tex_x = w & 0xFFF;
                   g->tex_bpp = g->pal_pending ? g->pal_pending : 16; g->pal_pending = 0; break;
        case 0x8F: case 0x90: case 0x91:   /* 8 bpp palette position */
        case 0x92: case 0x93: case 0x94:   /* 4 bpp palette position */
            g->pal_y = w >> 12 & 0xFFF; g->pal_x = w & 0xFFF;
            g->pal_pending = op >= 0x92 ? 4 : 8;
            break;
        case 0xC8:
            watch_what = "sprite"; watch_pc = pc - 4;
            sprite(g, w, pc - 4);
            pc += 4 * (w & 0xFFFF);
            break;
        case 0xD3: {
            uint32_t p = rd(g, pc), s = rd(g, pc + 4), c = rd(g, pc + 8);
            pc += 12;
            g->fills++;
            watch_what = "fill"; watch_pc = pc - 16;
            /* SDK depth rectangles use viewport-local coordinates, while
             * colour fills address the absolute tiled surface. SpongeBob
             * clears (96,35) 320x160 to inverse depth 256 after making the
             * corresponding colour rectangle transparent. Ignoring that
             * fill lets its farther background sprite cover the movie.
             * Only route a local rectangle when it fits the viewport and
             * is disjoint from the colour clip; ordinary colour fills keep
             * their existing clipping and surface bounds. */
            int px0 = (int)(p & 0xFFFF), py0 = (int)(p >> 16);
            int fw = (int)(s & 0xFFFF), fh = (int)(s >> 16);
            int cw = g->clip_x1 - g->clip_x0 + 1, ch = g->clip_y1 - g->clip_y0 + 1;
            int local_depth = px0 + fw <= cw && py0 + fh <= ch &&
                (px0 + fw <= g->clip_x0 || py0 + fh <= g->clip_y0);
            if (zfill || local_depth) {
                double inverse = ge_f16_to((uint16_t)c);
                float clear_depth = inverse > 0 ? (float)(1.0 / inverse) : 1e30f;
                int x0 = g->clip_x0 + (int)(p & 0xFFFF), y0 = g->clip_y0 + (int)(p >> 16);
                int x1 = x0 + (int)(s & 0xFFFF), y1 = y0 + (int)(s >> 16);
                if (x0 < 0) x0 = 0;
                if (y0 < 0) y0 = 0;
                if (x1 > 1024) x1 = 1024;
                if (y1 > 1024) y1 = 1024;
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++) zbuf[y * 1024 + x] = clear_depth;
                break;
            }
            fill(g, (int)(p & 0xFFFF), (int)(p >> 16), (int)(s & 0xFFFF), (int)(s >> 16), (uint16_t)c);
            break;
        }
        default:
            if (op >= 0xC0 && op <= 0xDF) {   /* commands with a word count */
                if (g->log && !g->unknown[op])
                    printf("[GE] unknown op %08X at %08X (list %08X, after %08X at %08X)\n", w, pc - 4, cur_list, pw, ppc);
                g->unknown[op]++;
                pc += 4 * (w & 0xFFFF);
                break;
            }
            if (g->log && !g->unknown[op])
                printf("[GE] unknown op %08X at %08X (list %08X, after %08X at %08X)\n", w, pc - 4, cur_list, pw, ppc);
            g->unknown[op]++;
            break;
        }
    }
    /* 100000 commands without a return: the engine is running through data (the audit
     * greps this; a misparsed record length is the usual cause) */
    if (g->log) printf("[GE] runaway list %08X (depth %d, now at %08X)\n", cur_list, depth, pc);
}

void ge_run(GE *g, uint32_t list) {
    static int init;
    if (!init) {
        init = 1;
        if (getenv("VFLASH_GEWATCH")) sscanf(getenv("VFLASH_GEWATCH"), "%d,%d", &watch_x, &watch_y);
        /* depth starts far, not 0: until a game's first depth clear reaches a
         * region, a zeroed buffer failed every test there (replays starting
         * mid-frame lost whole models, e.g. the Bratz room's girl) */
        for (int i = 0; i < 1024 * 1024; i++) zbuf[i] = 1e30f;
    }
    g->lists++;
    cur_list = list; prev_w = prev_pc = 0;
    if (list - RAM_BASE >= g->ram_size) return;   /* the ROM kicks list 0 at boot */
    run(g, list, 0, 0);
}
