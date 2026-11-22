/* CD servo DSP and disc model: see cdsp.h. Command and SENS semantics are
 * from the Sony CXD2545Q data sheet (§1-4 "Description of SENS signals",
 * §2-1 "$4X commands", §4 "Auto sequence"). */
#include "cdsp.h"
#include "cdrom.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Disc geometry, mm. */
#define R_PROGRAM   25.0          /* absolute 00:00:00 (LBA -150) */
#define R_LEADIN    23.0          /* first recorded frame */
#define R_STOP      22.5          /* sled at the innermost switch (SSTP) */
#define R_PARK      28.5          /* where the pickup rests at power-on */
#define PITCH       0.0016
#define SLED_MM_S   20.0
#define LEADOUT_SECTORS 6750      /* 90 s of lead-out after the last sector */
/* Area swept per sector: v * pitch / 75 (v = 1.3 m/s; VFLASH_CDCLV=<m/s>
 * overrides it, for debugging the driver's seek arithmetic). */
static double k_sector(void) {
    static double k;
    if (!k) {
        const char *e = getenv("VFLASH_CDCLV");
        double v = e && atof(e) > 0.5 && atof(e) < 2.0 ? atof(e) : 1.3;
        k = v * 1000.0 * PITCH / 75.0 / M_PI;
    }
    return k;
}
#define K_SECTOR    (k_sector())

/* VFLASH_CDLOG=<frame>: the machine turns this on from that frame. */
int cdsp_log;
static int dlog(void) { return cdsp_log; }

static double lba_at(double r)    { return (r * r - R_PROGRAM * R_PROGRAM) / K_SECTOR - 150.0; }
static double radius_at(double l) {
    double a = R_PROGRAM * R_PROGRAM + K_SECTOR * (l + 150.0);
    return a > 0 ? sqrt(a) : 0;
}

static int32_t leadout(CDSP *d) { return d->disc ? (int32_t)d->disc->sector_count : 0; }

void cdsp_reset(CDSP *d, CDROM *disc) {
    memset(d, 0, sizeof *d);
    d->disc = disc && disc->is_open ? disc : NULL;
    d->radius = R_PARK;
    d->lba = lba_at(d->radius);
}

static int tracking(CDSP *d) { return (d->tmode & 0xC) == 0x4; }

double cdsp_speed(CDSP *d) {
    double rate = (d->func & 4) ? 2.0 : 1.0;
    /* Sony CXD3068Q, $DX/$EX and section 3-3: CAV-W uses VP7..0
     * and VPCTL1..0, not DSPB alone. LPWR does not change the rate.
     * The retail driver sends D0C00040 / E6670000 with DSPB set:
     * (256 - 0xC0) / 32 * 2 = 4x. External-PWM mode is not modelled. */
    if ((d->spindle_mode & ~2u) == 0x65) {
        rate *= (256.0 - d->velocity) * (d->velocity_mul + 1) / 32.0;
        /* The newer retail HAL uses AE001140 / D1E00040 instead of
         * AE0011D5 / D0C00040. Model its CAV reference at twice the rate.
         * This divider interpretation is inferred from those SDK clock
         * profiles; the complete CXD3059 extended clock map is unknown. */
        if ((d->clock_config & 0xC0) == 0x40) rate *= 2.0;
    }
    /* VFLASH_CDSPEED=<factor> (debugging): scale the disc's rate, to try
     * the servo driver's timing against a slower or faster spindle */
    {
        static double scale = -1;
        if (scale < 0) {
            const char *e = getenv("VFLASH_CDSPEED");
            scale = e && atof(e) > 0 ? atof(e) : 1.0;
        }
        rate *= scale;
    }
    return rate;
}

/* Pickup motion since the last look. With the tracking servo on it follows
 * the spiral outwards at the disc's data rate; the sled ($2X bits 1-0:
 * 2 = forward/outwards, 3 = reverse) carries it at a fixed speed until the
 * innermost switch stops it. */
void cdsp_advance(CDSP *d, uint64_t now) {
    double dt = (double)(int64_t)(now - d->last_us) * 1e-6;
    d->last_us = now;
    if (dt <= 0) return;
    int sled = d->tmode & 3;
    if (sled == 2 || sled == 3) {
        double r0 = d->radius;
        d->radius += (sled == 2 ? 1 : -1) * SLED_MM_S * dt;
        if (d->radius < R_STOP) d->radius = R_STOP;
        d->lba = lba_at(d->radius);
        /* The traverse monitor counts the tracks a sled move crosses too:
         * game kernels kick the sled and wait for COMP instead of jumping. */
        d->traversed += fabs(d->radius - r0) / PITCH;
        if (d->bcount && !d->comp_fall && d->traversed >= d->bcount)
            d->comp_fall = now;
    } else if (tracking(d) && d->focused) {
        d->lba += 75.0 * cdsp_speed(d) * dt;
        d->radius = radius_at(d->lba);
    }
    if (d->busy && now >= d->busy_until) d->busy = 0;
}

int cdsp_reading(CDSP *d) {
    return d->disc && d->focused && d->radius >= R_LEADIN &&
           d->lba < leadout(d) + LEADOUT_SECTORS;
}

int32_t cdsp_lba(CDSP *d) { return (int32_t)floor(d->lba); }

static void jump(CDSP *d, int dir, uint32_t tracks) {
    d->radius += dir * PITCH * tracks;
    if (d->radius < R_STOP) d->radius = R_STOP;
    d->lba = lba_at(d->radius);
}

void cdsp_command(CDSP *d, uint64_t now, uint32_t bits, uint64_t data) {
    cdsp_advance(d, now);
    if (bits < 8 || bits > 64) return;
    uint8_t first = (uint8_t)(data >> (bits - 8));
    uint8_t addr = first >> 4, d1 = first & 15;
    d->sel = first;
    d->cmds++;
    switch (addr) {
    case 0x0:   /* focus control: FS4 (bit 3) = focus servo on */
        d->focused = (d1 & 8) != 0;
        break;
    case 0x2:
        if (dlog() && d->tmode != d1)
            printf("[CDSP] tracking mode %X -> %X (r=%.3f mm, lba %d)\n",
                   d->tmode, d1, d->radius, cdsp_lba(d));
        d->tmode = d1;
        break;
    case 0x4: { /* auto sequence: AS3-0, AS0 = reverse */
        int dir = (d1 & 1) ? -1 : 1;
        uint32_t t = 1000, tracks = 0;
        switch (d1 >> 1) {
        case 0: t = 0; break;                                 /* cancel */
        case 2: jump(d, dir, tracks = d->n); t = 2000 + d->n / 4; break; /* fine search */
        case 3: d->focused = 1; break;                        /* focus-on */
        case 4: jump(d, dir, tracks = 1); break;
        case 5: jump(d, dir, tracks = 10); t = 2000; break;
        case 6: jump(d, dir, tracks = 2 * d->n); t = 2000 + d->n / 2; break; /* 2N-track: the
                                  * ROM writes half the distance (0x10056F80) */
        case 7: jump(d, dir, tracks = d->n); t = 2000 + d->n / 4;      /* M-track move */
            d->tmode &= ~0xF; break;
        }
        /* COMP falls once the traverse has counted register B's tracks */
        if (tracks && d->bcount <= tracks && !d->comp_fall)
            d->comp_fall = now + (uint64_t)t * d->bcount / tracks + 1;
        d->busy = t != 0;
        d->busy_until = now + t;
        if (dlog())
            printf("[CDSP] %llu us auto sequence %X N=%u -> r=%.3f mm, lba %d\n",
                   (unsigned long long)now, d1, d->n, d->radius, cdsp_lba(d));
        break;
    }
    case 0x7:
        if (bits < 20) break;
        d->n = (uint32_t)(data >> (bits - 20)) & 0xFFFF;   /* D19-D4 */
        break;
    case 0xB:   /* traverse monitor count (D19-D4); COMP goes high. A lone
                 * command byte only selects SENS. */
        if (bits < 20) break;
        d->bcount = (uint32_t)(data >> (bits - 20)) & 0xFFFF;
        d->comp_fall = 0;
        d->traversed = 0;
        if (dlog()) printf("[CDSP] traverse count %u (%u bits %llX)\n", d->bcount, bits, (unsigned long long)data);
        break;
    case 0x9:
        if (dlog() && d->func != d1) printf("[CDSP] function %X -> %X\n", d->func, d1);
        d->func = d1;
        break;
    case 0xD:
        if (bits >= 20) {
            uint32_t params = (uint32_t)(data >> (bits - 20));
            d->velocity = (params >> 4) & 0xFF;
            d->velocity_mul = (params >> 2) & 3;
        }
        break;
    case 0xA:
        if (bits >= 32 && (data >> (bits - 24)) == 0xAE0011)
            d->clock_config = (uint8_t)(data >> (bits - 32));
        break;
    case 0xE:
        if (bits >= 16) d->spindle_mode = (uint8_t)(data >> (bits - 16));
        break;
    }
}

int cdsp_sens(CDSP *d, uint64_t now) {
    cdsp_advance(d, now);
    uint8_t a = d->sel >> 4;
    int v = 0;
    switch (a) {
    case 0x0: case 0x2: case 0xC:     /* FZC, TZC, COUT: edges */
        d->toggle ^= 1; v = d->toggle; break;
    case 0x1: v = 0; break;           /* AS */
    case 0x3:
        if (d->sel == 0x38) v = 1;    /* AGOK */
        else v = d->radius <= R_STOP; /* SSTP: innermost track switch */
        break;
    case 0x4: v = !d->busy; break;    /* XBUSY */
    case 0x5: v = d->focused && d->disc; break;   /* FOK */
    case 0xA: v = cdsp_reading(d); break;         /* GFS */
    case 0xB: v = !(d->comp_fall && now >= d->comp_fall); break;   /* COMP */
    case 0xE: v = 1; break;           /* OV64 (active low) */
    }
    if (dlog()) {
        static uint8_t last_sel = 0xFF; static int last_v = -1; static int n;
        if ((d->sel != last_sel || v != last_v) && n++ < 200000)
            printf("[CDSP] SENS %02X -> %d\n", d->sel, v);
        last_sel = d->sel; last_v = v;
    }
    return v;
}

/* Serial readout after a command. $39xx selects a measurement register
 * (CXD2545 §1-4: $3904 TE average, $3908 FE average, $390C VC average,
 * $391C TRVSC, $391D FB, $391F RFDC average). Offsets read as centred (0).
 * RFDC is the RF level: the ROM's gain calibration (0x1005B52C) steps the RF
 * gain until it reads, as a signed byte, within [-98, -94] from its table at
 * 0x1000D2C5/0x1000D2CA, so a present disc answers from the middle of that. */
uint32_t cdsp_readback(CDSP *d, uint64_t now, uint32_t bits, uint64_t data, uint32_t rbits) {
    cdsp_advance(d, now);
    uint32_t v = 0;
    uint8_t first = bits >= 8 ? (uint8_t)(data >> (bits - 8)) : 0;
    uint8_t sub = bits >= 16 ? (uint8_t)(data >> (bits - 16)) : 0;
    if (first == 0x39 && (sub & 0x7F) == 0x1F && d->disc) v = 0xA0;
    if (dlog()) {
        static int n;
        if (n++ < 40000)
            printf("[CDSP] read %u bits after %0*llX -> %X\n", rbits, (int)(bits + 3) / 4,
                   (unsigned long long)data, v);
    }
    return v;
}

/* ---------------------------------------------------------------- subcode */

static uint8_t bcd(unsigned v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

static void msf(uint8_t *p, int32_t frames) {
    if (frames < 0) frames += 100 * 60 * 75;
    p[0] = bcd((unsigned)frames / 4500 % 100);
    p[1] = bcd((unsigned)frames / 75 % 60);
    p[2] = bcd((unsigned)frames % 75);
}

void cdsp_subq(CDSP *d, int32_t lba, uint8_t q[10]) {
    CDROM *c = d->disc;
    memset(q, 0, 10);
    if (!c) return;
    int nt = c->ntracks;
    int32_t lo = leadout(d);
    int32_t leadin_start = (int32_t)floor(lba_at(R_LEADIN));
    if (lba < -150) {
        /* Lead-in: the TOC, each point three times over. */
        int32_t k = (lba - leadin_start) / 3 % (nt + 3);
        uint8_t ctrl = c->track[0].ctrl;
        q[1] = 0;
        msf(q + 3, lba - leadin_start);
        if (k < nt) {
            ctrl = c->track[k].ctrl;
            q[2] = bcd(c->track[k].num);
            msf(q + 7, (int32_t)c->track[k].start + 150);
        } else if (k == nt) {
            q[2] = 0xA0; q[7] = bcd(c->track[0].num); q[8] = 0x00;
        } else if (k == nt + 1) {
            ctrl = c->track[nt - 1].ctrl;
            q[2] = 0xA1; q[7] = bcd(c->track[nt - 1].num);
        } else {
            ctrl = c->track[nt - 1].ctrl;
            q[2] = 0xA2; msf(q + 7, lo + 150);
        }
        q[0] = (uint8_t)(ctrl << 4 | 1);
        return;
    }
    msf(q + 7, lba + 150);
    if (lba >= lo) {
        q[0] = (uint8_t)(c->track[nt - 1].ctrl << 4 | 1);
        q[1] = 0xAA; q[2] = 0x01;
        msf(q + 3, lba - lo);
        return;
    }
    int t = 0;
    while (t + 1 < nt && (int32_t)c->track[t + 1].start <= lba) t++;
    /* The gap before a track (index 00) counts down to its start. */
    if (t + 1 < nt && lba >= (int32_t)c->track[t + 1].start - 150 && t + 1 < nt &&
        lba < (int32_t)c->track[t + 1].start)
        t++;
    int32_t rel = lba - (int32_t)c->track[t].start;
    q[0] = (uint8_t)(c->track[t].ctrl << 4 | 1);
    q[1] = bcd(c->track[t].num);
    q[2] = rel < 0 ? 0x00 : 0x01;
    msf(q + 3, rel < 0 ? -rel : rel);
}
