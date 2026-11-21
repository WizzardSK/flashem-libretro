/* Native decoding for the DSP's MIAV (.mjp) movie player (#7).
 *
 * The movie files: a 64-byte "MIAV" header, then chunks "00dc" (a JPEG
 * frame), "01wb"/"02wb" (IMA4 audio), each tag + u32 frame number + u32 size
 * + payload. The JPEG data has every 16-bit word byte-swapped, only frame 0
 * carries the tables (SOI..DHT) and every frame starts at SOS; the entropy
 * data is not byte-stuffed and has no EOI. Baseline, 4:2:0 (Y 2x2, Cb, Cr).
 * Format worked out by TobiasAR (mjp2mp4.py) and checked against the DSP's
 * own output (within one level). */
#include "mjp_hle.h"
#include <string.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2 0.70710678118654752440
#endif

typedef struct {
    uint8_t  bits[17];
    uint8_t  vals[256];
    int      mincode[17], maxcode[18], valptr[17];
    int      ok;
} Huff;

typedef struct {
    uint16_t q[4][64];
    Huff     dc[4], ac[4];
    int      w, h;
    int      comp_id[3], comp_h[3], comp_v[3], comp_q[3];
    int      ncomp;
} Tables;

static const uint8_t zigzag[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63 };

static void huff_build(Huff *t) {
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        t->valptr[l] = k;
        t->mincode[l] = code;
        code += t->bits[l];
        k += t->bits[l];
        t->maxcode[l] = t->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    t->maxcode[17] = 0x7fffffff;
    t->ok = 1;
}

/* Header segments: DQT, SOF0, DHT (DRI/APPn skipped). 0 on success. */
static int parse_tables(Tables *t, const uint8_t *p, size_t n) {
    size_t i = 0;
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xD8) i = 2;
    while (i + 4 <= n) {
        if (p[i] != 0xFF) return -1;
        unsigned m = p[i + 1], len = (unsigned)p[i + 2] << 8 | p[i + 3];
        const uint8_t *s = p + i + 4;
        if (i + 2 + len > n) return -1;
        if (m == 0xDB) {
            for (unsigned o = 0; o + 65 <= len - 2; ) {
                unsigned pq = s[o] >> 4, tq = s[o] & 3;
                o++;
                for (int k = 0; k < 64; k++) {
                    t->q[tq][zigzag[k]] = pq ? (uint16_t)(s[o + 2 * k] << 8 | s[o + 2 * k + 1]) : s[o + k];
                }
                o += pq ? 128 : 64;
            }
        } else if (m == 0xC0 || m == 0xC1) {
            t->h = s[1] << 8 | s[2];
            t->w = s[3] << 8 | s[4];
            t->ncomp = s[5] > 3 ? 3 : s[5];
            for (int c = 0; c < t->ncomp; c++) {
                t->comp_id[c] = s[6 + 3 * c];
                t->comp_h[c] = s[7 + 3 * c] >> 4;
                t->comp_v[c] = s[7 + 3 * c] & 15;
                t->comp_q[c] = s[8 + 3 * c] & 3;
            }
        } else if (m == 0xC4) {
            unsigned o = 0;
            while (o < len - 2) {
                unsigned tc = s[o] >> 4, th = s[o] & 3, total = 0;
                Huff *h = tc ? &t->ac[th] : &t->dc[th];
                h->bits[0] = 0;
                for (int l = 1; l <= 16; l++) { h->bits[l] = s[o + l]; total += s[o + l]; }
                if (total > 256 || o + 17 + total > len - 2) return -1;
                memcpy(h->vals, s + o + 17, total);
                huff_build(h);
                o += 17 + total;
            }
        } else if (m == 0xDA) {
            break;
        }
        i += 2 + len;
    }
    return t->w > 0 && t->h > 0 && t->ncomp == 3 ? 0 : -1;
}

typedef struct { const uint8_t *p, *end; uint32_t buf; int n; } Bits;

static inline unsigned bits_get(Bits *b, int k) {
    while (b->n < k) {
        b->buf = b->buf << 8 | (b->p < b->end ? *b->p++ : 0);
        b->n += 8;
    }
    unsigned v = (b->buf >> (b->n - k)) & ((1u << k) - 1);
    b->n -= k;
    return v;
}

static int huff_decode(Bits *b, const Huff *h) {
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = code << 1 | (int)bits_get(b, 1);
        if (code <= h->maxcode[l] && h->bits[l]) {
            return h->vals[h->valptr[l] + code - h->mincode[l]];
        }
    }
    return 0;
}

static inline int extend(int v, int s) { return s && v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }

/* Separable float IDCT, then +128 and clamp: about as the DSP rounds */
static void idct(const int in[64], uint8_t *out, int stride) {
    static float c[8][8];
    static int init;
    if (!init) {
        for (int x = 0; x < 8; x++)
            for (int u = 0; u < 8; u++)
                c[x][u] = (float)((u ? 1.0 : M_SQRT1_2) * cos((2 * x + 1) * u * M_PI / 16) / 2);
        init = 1;
    }
    float tmp[64];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            float s = 0;
            for (int u = 0; u < 8; u++) s += c[x][u] * in[y * 8 + u];
            tmp[y * 8 + x] = s;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            float s = 0;
            for (int v = 0; v < 8; v++) s += c[y][v] * tmp[v * 8 + x];
            int px = (int)lrintf(s) + 128;
            out[y * stride + x] = (uint8_t)(px < 0 ? 0 : px > 255 ? 255 : px);
        }
}

int mjp_decode(const uint8_t *hdr, size_t hdr_len, const uint8_t *scan, size_t scan_len,
               uint8_t *y, uint8_t *cb, uint8_t *cr, int *wp, int *hp, int max_w, int max_h) {
    static Tables t;
    memset(&t, 0, sizeof t);
    if (parse_tables(&t, hdr, hdr_len)) return -1;
    if (t.w > max_w || t.h > max_h || (t.w & 15) || (t.h & 15)) return -2;
    if (scan_len < 4 || scan[0] != 0xFF || scan[1] != 0xDA) return -3;
    unsigned slen = (unsigned)scan[2] << 8 | scan[3];
    if (slen + 2 > scan_len) return -3;
    int ns = scan[4], dct[3] = {0}, act[3] = {0}, ord[3] = {0, 1, 2};
    for (int i = 0; i < ns && i < 3; i++) {
        int id = scan[5 + 2 * i];
        for (int c = 0; c < 3; c++) if (t.comp_id[c] == id) {
            ord[i] = c; dct[c] = scan[6 + 2 * i] >> 4; act[c] = scan[6 + 2 * i] & 3;
        }
    }
    Bits b = { scan + 2 + slen, scan + scan_len, 0, 0 };
    int pred[3] = {0}, w = t.w, h = t.h, cw = w / 2;
    for (int my = 0; my < h / 16; my++)
        for (int mx = 0; mx < w / 16; mx++)
            for (int i = 0; i < 3; i++) {
                int c = ord[i], nb = t.comp_h[c] * t.comp_v[c];
                for (int k = 0; k < nb; k++) {
                    int blk[64] = {0};
                    int s = huff_decode(&b, &t.dc[dct[c]]);
                    pred[c] += extend((int)bits_get(&b, s), s);
                    blk[0] = pred[c] * t.q[t.comp_q[c]][0];
                    for (int z = 1; z < 64; ) {
                        int rs = huff_decode(&b, &t.ac[act[c]]), r = rs >> 4;
                        s = rs & 15;
                        if (!s) { if (r != 15) break; z += 16; continue; }
                        z += r;
                        if (z > 63) break;
                        blk[zigzag[z]] = extend((int)bits_get(&b, s), s) * t.q[t.comp_q[c]][zigzag[z]];
                        z++;
                    }
                    if (c == 0) {
                        int bx = mx * 16 + (k & 1) * 8, by = my * 16 + (k >> 1) * 8;
                        idct(blk, y + by * w + bx, w);
                    } else {
                        idct(blk, (c == 1 ? cb : cr) + my * 8 * cw + mx * 8, cw);
                    }
                }
            }
    *wp = w; *hp = h;
    return 0;
}
