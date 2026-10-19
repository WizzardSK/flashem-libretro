#include "arm9.h"
#include "cp15.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define PC   cpu->r[15]
#define SP   cpu->r[13]
#define LR   cpu->r[14]
#define CPSR cpu->cpsr

#define N_FLAG  ((CPSR >> 31) & 1)
#define Z_FLAG  ((CPSR >> 30) & 1)
#define C_FLAG  ((CPSR >> 29) & 1)
#define V_FLAG  ((CPSR >> 28) & 1)
#define T_FLAG  ((CPSR >>  5) & 1)

#define SET_N(v) do { CPSR = (CPSR & ~ARM9_FLAG_N) | ((v) ? ARM9_FLAG_N : 0); } while(0)
#define SET_Z(v) do { CPSR = (CPSR & ~ARM9_FLAG_Z) | ((v) ? ARM9_FLAG_Z : 0); } while(0)
#define SET_C(v) do { CPSR = (CPSR & ~ARM9_FLAG_C) | ((v) ? ARM9_FLAG_C : 0); } while(0)
#define SET_V(v) do { CPSR = (CPSR & ~ARM9_FLAG_V) | ((v) ? ARM9_FLAG_V : 0); } while(0)
#define SET_NZ(r)    do { SET_N((r)>>31); SET_Z((r)==0); } while(0)
#define SET_NZC(r,c) do { SET_NZ(r); SET_C(c); } while(0)

static inline uint32_t r32(ARM9 *c, uint32_t a){ return c->mem_read32(c->mem_ctx, a&~3u); }
static inline uint16_t r16(ARM9 *c, uint32_t a){ return c->mem_read16(c->mem_ctx, a&~1u); }
static inline uint8_t  r8 (ARM9 *c, uint32_t a){ return c->mem_read8 (c->mem_ctx, a);     }
static inline void w32(ARM9 *c, uint32_t a, uint32_t v){ c->mem_write32(c->mem_ctx,a&~3u,v); }
static inline void w16(ARM9 *c, uint32_t a, uint16_t v){ c->mem_write16(c->mem_ctx,a&~1u,v); }
static inline void w8 (ARM9 *c, uint32_t a, uint8_t  v){ c->mem_write8 (c->mem_ctx,a,v);     }

/* Bit NZCV of cond_tab[cond] says whether cond passes with those flags:
 * EQ NE CS CC MI PL VS VC HI LS GE LT GT LE AL, and never for 0xF. */
static const uint16_t cond_tab[16] = {
    0xF0F0, 0x0F0F, 0xCCCC, 0x3333, 0xFF00, 0x00FF, 0xAAAA, 0x5555,
    0x0C0C, 0xF3F3, 0xAA55, 0x55AA, 0x0A05, 0xF5FA, 0xFFFF, 0x0000 };
static inline int cond_ok(ARM9 *cpu, uint32_t cond) {
    return cond_tab[cond & 15] >> (CPSR >> 28) & 1;
}

/* Helpers that nearly every instruction goes through, put in place: as calls
 * they were a large part of the interpreter's time (do_sub alone ~550M calls
 * over SpongeBob's boot and loading screen) */
#if defined(__GNUC__) || defined(__clang__)
#define HOT_INLINE static inline __attribute__((always_inline))
#else
#define HOT_INLINE static inline
#endif

typedef struct { uint32_t v; int c; } SR;
HOT_INLINE SR bshift(uint32_t val, int type, int amt, int cin) {
    SR r = {val, cin};
    if (!amt) return r;
    switch(type) {
    case 0: r.c=(amt<=32)?(int)((val>>(32-amt))&1):0; r.v=(amt<32)?(val<<amt):0; break;
    case 1: r.c=(amt<=32)?(int)((val>>(amt-1))&1):0;  r.v=(amt<32)?(val>>amt):0; break;
    case 2: r.c=(int)(((int32_t)val>>(amt<32?amt-1:31))&1);
            r.v=(uint32_t)((int32_t)val>>(amt<32?amt:31)); break;
    case 3:
        amt&=31;
        if(amt){ r.v=(val>>amt)|(val<<(32-amt)); r.c=(int)((val>>(amt-1))&1); }
        else r.c=(int)(val>>31);
        break;
    }
    return r;
}

HOT_INLINE SR decode_shift(ARM9 *cpu, uint32_t insn) {
    int rm=insn&0xF, type=(insn>>5)&3, amt;
    uint32_t val=cpu->r[rm];
    /* ARM spec: Rm=PC reads as inst+8 (PC is already inst+8).
     * Exception: register-specified shift (bit4=1) adds +4 → inst+12. */
    if(rm==15 && (insn&(1<<4))) val+=4;
    if(insn&(1<<4)) { amt=cpu->r[(insn>>8)&0xF]&0xFF; }
    else { amt=(insn>>7)&0x1F;
        if(!amt && type==3){ SR r; r.c=val&1; r.v=(val>>1)|((uint32_t)C_FLAG<<31); return r; }
        if(!amt && (type==1 || type==2)) amt=32;   /* LSR #32 / ASR #32 */
    }
    return bshift(val,type,amt,C_FLAG);
}
HOT_INLINE uint32_t decode_imm(uint32_t insn){
    uint32_t i=insn&0xFF; int rot=((insn>>8)&0xF)*2;
    return rot?(i>>rot)|(i<<(32-rot)):i;
}

/* Banked registers. R8-R12 have one copy for FIQ and one for every other
 * mode; R13/R14 one per privileged mode plus one USR and SYS share. Leaving a
 * mode files its registers away, entering one brings its own back. */
static void save_bank(ARM9 *cpu, int m) {
    m &= 0x1F;
    if (m == ARM9_MODE_FIQ) {
        cpu->r8_fiq=cpu->r[8];cpu->r9_fiq=cpu->r[9];cpu->r10_fiq=cpu->r[10];
        cpu->r11_fiq=cpu->r[11];cpu->r12_fiq=cpu->r[12];
    } else {
        cpu->r8_usr=cpu->r[8];cpu->r9_usr=cpu->r[9];cpu->r10_usr=cpu->r[10];
        cpu->r11_usr=cpu->r[11];cpu->r12_usr=cpu->r[12];
    }
    switch(m){
    case ARM9_MODE_FIQ: cpu->r13_fiq=cpu->r[13];cpu->r14_fiq=cpu->r[14];cpu->spsr_fiq=cpu->spsr; break;
    case ARM9_MODE_IRQ: cpu->r13_irq=cpu->r[13];cpu->r14_irq=cpu->r[14];cpu->spsr_irq=cpu->spsr; break;
    case ARM9_MODE_SVC: cpu->r13_svc=cpu->r[13];cpu->r14_svc=cpu->r[14];cpu->spsr_svc=cpu->spsr; break;
    case ARM9_MODE_ABT: cpu->r13_abt=cpu->r[13];cpu->r14_abt=cpu->r[14];cpu->spsr_abt=cpu->spsr; break;
    case ARM9_MODE_UND: cpu->r13_und=cpu->r[13];cpu->r14_und=cpu->r[14];cpu->spsr_und=cpu->spsr; break;
    default:            cpu->r13_usr=cpu->r[13];cpu->r14_usr=cpu->r[14]; break;
    }
}
static void load_bank(ARM9 *cpu, int m) {
    m &= 0x1F;
    if (m == ARM9_MODE_FIQ) {
        cpu->r[8]=cpu->r8_fiq;cpu->r[9]=cpu->r9_fiq;cpu->r[10]=cpu->r10_fiq;
        cpu->r[11]=cpu->r11_fiq;cpu->r[12]=cpu->r12_fiq;
    } else {
        cpu->r[8]=cpu->r8_usr;cpu->r[9]=cpu->r9_usr;cpu->r[10]=cpu->r10_usr;
        cpu->r[11]=cpu->r11_usr;cpu->r[12]=cpu->r12_usr;
    }
    switch(m){
    case ARM9_MODE_FIQ: cpu->r[13]=cpu->r13_fiq;cpu->r[14]=cpu->r14_fiq;cpu->spsr=cpu->spsr_fiq; break;
    case ARM9_MODE_IRQ: cpu->r[13]=cpu->r13_irq;cpu->r[14]=cpu->r14_irq;cpu->spsr=cpu->spsr_irq; break;
    case ARM9_MODE_SVC: cpu->r[13]=cpu->r13_svc;cpu->r[14]=cpu->r14_svc;cpu->spsr=cpu->spsr_svc; break;
    case ARM9_MODE_ABT: cpu->r[13]=cpu->r13_abt;cpu->r[14]=cpu->r14_abt;cpu->spsr=cpu->spsr_abt; break;
    case ARM9_MODE_UND: cpu->r[13]=cpu->r13_und;cpu->r[14]=cpu->r14_und;cpu->spsr=cpu->spsr_und; break;
    default:            cpu->r[13]=cpu->r13_usr;cpu->r[14]=cpu->r14_usr; break;
    }
}
static void set_mode(ARM9 *cpu, uint32_t ncpsr){
    int om=CPSR&0x1F, nm=ncpsr&0x1F;
    if(om!=nm){ save_bank(cpu,om); CPSR=ncpsr; load_bank(cpu,nm); }
    else CPSR=ncpsr;
}

HOT_INLINE uint32_t do_add(ARM9 *cpu,uint32_t a,uint32_t b,int s){
    uint64_t r=(uint64_t)a+b; uint32_t res=(uint32_t)r;
    if(s){SET_N(res>>31);SET_Z(res==0);SET_C(r>>32);SET_V((!((a^b)>>31))&&((a^res)>>31));}
    return res;
}
HOT_INLINE uint32_t do_adc(ARM9 *cpu,uint32_t a,uint32_t b,int s){
    uint64_t r=(uint64_t)a+b+C_FLAG; uint32_t res=(uint32_t)r;
    if(s){SET_N(res>>31);SET_Z(res==0);SET_C(r>>32);SET_V((!((a^b)>>31))&&((a^res)>>31));}
    return res;
}
HOT_INLINE uint32_t do_sub(ARM9 *cpu,uint32_t a,uint32_t b,int s){
    uint64_t r=(uint64_t)a-b; uint32_t res=(uint32_t)r;
    if(s){SET_N(res>>31);SET_Z(res==0);SET_C(a>=b);SET_V(((a^b)>>31)&&((a^res)>>31));}
    return res;
}
HOT_INLINE uint32_t do_sbc(ARM9 *cpu,uint32_t a,uint32_t b,int s){
    uint64_t r=(uint64_t)a-b-(1-C_FLAG); uint32_t res=(uint32_t)r;
    if(s){SET_N(res>>31);SET_Z(res==0);SET_C((uint64_t)a>=(uint64_t)b+(1-C_FLAG));SET_V(((a^b)>>31)&&((a^res)>>31));}
    return res;
}

/* ===== VFP (CP10/CP11) Emulation ===== */

static inline float  vfp_s_get(ARM9 *cpu, int reg) { return cpu->vfp_s[reg & 31]; }
static inline void   vfp_s_set(ARM9 *cpu, int reg, float v) { cpu->vfp_s[reg & 31] = v; }
static inline double vfp_d_get(ARM9 *cpu, int reg) {
    union { float f[2]; double d; } u;
    u.f[0] = cpu->vfp_s[(reg & 15) * 2];
    u.f[1] = cpu->vfp_s[(reg & 15) * 2 + 1];
    return u.d;
}
static inline void   vfp_d_set(ARM9 *cpu, int reg, double v) {
    union { float f[2]; double d; } u;
    u.d = v;
    cpu->vfp_s[(reg & 15) * 2]     = u.f[0];
    cpu->vfp_s[(reg & 15) * 2 + 1] = u.f[1];
}
static inline uint32_t vfp_s_bits(ARM9 *cpu, int reg) {
    union { float f; uint32_t u; } t; t.f = cpu->vfp_s[reg & 31]; return t.u;
}
static inline void vfp_s_from_bits(ARM9 *cpu, int reg, uint32_t bits) {
    union { float f; uint32_t u; } t; t.u = bits; cpu->vfp_s[reg & 31] = t.f;
}

/* Execute VFP CDP (data processing) — returns 1 if handled */
static int vfp_cdp(ARM9 *cpu, uint32_t insn) {
    int cp = (insn >> 8) & 0xF;
    int is_double = (cp == 11);
    int p = (insn >> 23) & 1;
    int q = (insn >> 21) & 1;
    int r = (insn >> 20) & 1;
    int s = (insn >> 6) & 1;

    if (!is_double) {
        /* Single-precision CP10 */
        int d  = ((insn >> 12) & 0xF) * 2 + ((insn >> 22) & 1);  /* Sd = Vd:D */
        int n  = ((insn >> 16) & 0xF) * 2 + ((insn >> 7) & 1);   /* Sn = Vn:N */
        int m  = (insn & 0xF) * 2 + ((insn >> 5) & 1);            /* Sm = Vm:M */

        float fd = vfp_s_get(cpu, d);
        float fn = vfp_s_get(cpu, n);
        float fm = vfp_s_get(cpu, m);

        if (!p) {
            switch ((q << 1) | r) {
            case 0: /* VMLA */  vfp_s_set(cpu, d, fd + fn * fm); break;
            case 1: /* VMLS */  vfp_s_set(cpu, d, fd - fn * fm); break;
            case 2: /* VNMLS */ vfp_s_set(cpu, d, -(fd - fn * fm)); break;
            case 3: /* VNMLA */ vfp_s_set(cpu, d, -(fd + fn * fm)); break;
            }
            if (s) { /* with negate: swap sense */
                /* Already handled by cases above */
            }
        } else if (p && !q) {
            if (!r && !s)      vfp_s_set(cpu, d, fn * fm);     /* VMUL */
            else if (!r && s)  vfp_s_set(cpu, d, -(fn * fm));  /* VNMUL */
            else if (r && !s)  vfp_s_set(cpu, d, fn + fm);     /* VADD */
            else               vfp_s_set(cpu, d, fn - fm);     /* VSUB */
        } else if (p && q && !r) {
            vfp_s_set(cpu, d, fn / fm);  /* VDIV */
        } else if (p && q && r) {
            /* Extension: VMOV, VABS, VNEG, VSQRT, VCMP, VCVT */
            int opc2 = (insn >> 16) & 0xF;
            switch (opc2) {
            case 0: /* VMOV / VCPY */
                if (!s) vfp_s_set(cpu, d, fm);                 /* VMOV Sd, Sm */
                else    vfp_s_set(cpu, d, fabsf(fm));           /* VABS */
                break;
            case 1:
                if (!s) vfp_s_set(cpu, d, -fm);                /* VNEG */
                else    vfp_s_set(cpu, d, sqrtf(fm));           /* VSQRT */
                break;
            case 4: case 5: { /* VCMP / VCMPE */
                float a = fn;
                float b = (opc2 == 5 && !s) ? 0.0f : fm;
                uint32_t nzcv = 0;
                if (isnan(a) || isnan(b))      nzcv = 0x30000000u; /* C=1,V=1 = unordered */
                else if (a == b)               nzcv = 0x60000000u; /* Z=1,C=1 */
                else if (a < b)                nzcv = 0x80000000u; /* N=1 */
                else                           nzcv = 0x20000000u; /* C=1 */
                cpu->vfp_fpscr = (cpu->vfp_fpscr & 0x0FFFFFFFu) | nzcv;
                break;
            }
            case 7: { /* VCVT float↔int */
                if (!s) {
                    /* VCVT.F32.S32 or VCVT.F32.U32 */
                    uint32_t bits = vfp_s_bits(cpu, m);
                    if ((insn >> 7) & 1) /* unsigned */
                        vfp_s_set(cpu, d, (float)bits);
                    else
                        vfp_s_set(cpu, d, (float)(int32_t)bits);
                } else {
                    /* VCVT.U32.F32 or VCVT.S32.F32 (round toward zero) */
                    if ((insn >> 16) & 1) { /* unsigned (VCVT.U32) - opc2 bit0 */
                        uint32_t res = (fm >= 0 && fm < 4294967296.0f) ? (uint32_t)fm : 0;
                        vfp_s_from_bits(cpu, d, res);
                    } else { /* signed (VCVT.S32) */
                        int32_t res = (int32_t)fm;
                        vfp_s_from_bits(cpu, d, (uint32_t)res);
                    }
                }
                break;
            }
            case 8: { /* VCVT between single and double */
                if (is_double) {
                    /* double to single */
                    vfp_s_set(cpu, d, (float)vfp_d_get(cpu, m));
                } else {
                    /* single to double */
                    vfp_d_set(cpu, d / 2, (double)fm);
                }
                break;
            }
            default:
                return 0; /* unhandled */
            }
        } else {
            return 0;
        }
    } else {
        /* Double-precision CP11 */
        int d  = ((insn >> 12) & 0xF) | (((insn >> 22) & 1) << 4);  /* Dd */
        int n  = ((insn >> 16) & 0xF) | (((insn >> 7) & 1) << 4);   /* Dn */
        int m  = (insn & 0xF) | (((insn >> 5) & 1) << 4);            /* Dm */

        double fd = vfp_d_get(cpu, d);
        double fn = vfp_d_get(cpu, n);
        double fm = vfp_d_get(cpu, m);

        if (!p) {
            switch ((q << 1) | r) {
            case 0: vfp_d_set(cpu, d, fd + fn * fm); break;  /* VMLA */
            case 1: vfp_d_set(cpu, d, fd - fn * fm); break;  /* VMLS */
            case 2: vfp_d_set(cpu, d, -(fd - fn * fm)); break;
            case 3: vfp_d_set(cpu, d, -(fd + fn * fm)); break;
            }
        } else if (p && !q) {
            if (!r && !s)      vfp_d_set(cpu, d, fn * fm);
            else if (!r && s)  vfp_d_set(cpu, d, -(fn * fm));
            else if (r && !s)  vfp_d_set(cpu, d, fn + fm);
            else               vfp_d_set(cpu, d, fn - fm);
        } else if (p && q && !r) {
            vfp_d_set(cpu, d, fn / fm);
        } else if (p && q && r) {
            int opc2 = (insn >> 16) & 0xF;
            switch (opc2) {
            case 0:
                if (!s) vfp_d_set(cpu, d, fm);
                else    vfp_d_set(cpu, d, fabs(fm));
                break;
            case 1:
                if (!s) vfp_d_set(cpu, d, -fm);
                else    vfp_d_set(cpu, d, sqrt(fm));
                break;
            case 4: case 5: {
                double a = fn, b = (opc2 == 5 && !s) ? 0.0 : fm;
                uint32_t nzcv = 0;
                if (isnan(a) || isnan(b))  nzcv = 0x30000000u;
                else if (a == b)           nzcv = 0x60000000u;
                else if (a < b)            nzcv = 0x80000000u;
                else                       nzcv = 0x20000000u;
                cpu->vfp_fpscr = (cpu->vfp_fpscr & 0x0FFFFFFFu) | nzcv;
                break;
            }
            case 7: {
                if (!s) {
                    uint32_t bits = vfp_s_bits(cpu, m * 2);
                    if ((insn >> 7) & 1)
                        vfp_d_set(cpu, d, (double)bits);
                    else
                        vfp_d_set(cpu, d, (double)(int32_t)bits);
                } else {
                    if ((insn >> 16) & 1) {
                        uint32_t res = (fm >= 0 && fm < 4294967296.0) ? (uint32_t)fm : 0;
                        vfp_s_from_bits(cpu, d * 2, res);
                    } else {
                        vfp_s_from_bits(cpu, d * 2, (uint32_t)(int32_t)fm);
                    }
                }
                break;
            }
            case 8: {
                /* VCVT.F32.F64 or VCVT.F64.F32 */
                if (is_double) {
                    vfp_s_set(cpu, d, (float)vfp_d_get(cpu, m));
                } else {
                    vfp_d_set(cpu, d, (double)vfp_s_get(cpu, m * 2));
                }
                break;
            }
            default: return 0;
            }
        } else {
            return 0;
        }
    }
    return 1;
}

/* Execute VFP MCR/MRC (register transfer) — returns 1 if handled */
static int vfp_mcr_mrc(ARM9 *cpu, uint32_t insn) {
    int l = (insn >> 20) & 1;   /* MRC=1, MCR=0 */
    int cp = (insn >> 8) & 0xF;
    int opc1 = (insn >> 21) & 7;
    int rd = (insn >> 12) & 0xF;
    int crn = (insn >> 16) & 0xF;

    if (opc1 == 7) {
        /* VMRS / VMSR — VFP system register transfer */
        int reg = crn; /* specifies which system register */
        if (l) {
            /* VMRS: read VFP system reg to ARM reg */
            uint32_t val;
            switch (reg) {
            case 0: val = 0x41011090; break;  /* FPSID: VFPv2, ARM, rev 0 */
            case 1: val = cpu->vfp_fpscr; break;
            case 6: val = 0x00000000; break;  /* MVFR1 */
            case 7: val = 0x00000000; break;  /* MVFR0 */
            case 8: val = cpu->vfp_fpexc; break;
            default: val = 0; break;
            }
            if (rd == 15) {
                /* VMRS R15 (APSR_nzcv): copy FPSCR flags to CPSR */
                CPSR = (CPSR & 0x0FFFFFFFu) | (cpu->vfp_fpscr & 0xF0000000u);
            } else {
                cpu->r[rd] = val;
            }
        } else {
            /* VMSR: write ARM reg to VFP system reg */
            uint32_t val = cpu->r[rd];
            switch (reg) {
            case 1: cpu->vfp_fpscr = val; break;
            case 8: cpu->vfp_fpexc = val; break;
            default: break;
            }
        }
        return 1;
    }

    /* VMOV between ARM and single-precision VFP reg */
    if (opc1 == 0 && ((insn >> 5) & 7) == 0) {
        int sn = crn * 2 + ((insn >> 7) & 1); /* Sn */
        if (l) {
            /* VMOV Rd, Sn — VFP to ARM */
            cpu->r[rd] = vfp_s_bits(cpu, sn);
        } else {
            /* VMOV Sn, Rd — ARM to VFP */
            vfp_s_from_bits(cpu, sn, cpu->r[rd]);
        }
        return 1;
    }

    return 0; /* not handled */
}

/* Execute VFP LDC/STC (load/store) — returns 1 if handled */
static int vfp_ldc_stc(ARM9 *cpu, uint32_t insn) {
    int cp = (insn >> 8) & 0xF;
    int is_double = (cp == 11);
    int p = (insn >> 24) & 1;
    int u = (insn >> 23) & 1;
    int w = (insn >> 21) & 1;
    int l = (insn >> 20) & 1;
    int rn = (insn >> 16) & 0xF;
    int offset = (insn & 0xFF) * 4;
    uint32_t base = cpu->r[rn];
    if (rn == 15) base = PC - 4; /* PC-relative: inst+8 adjusted by pipeline, -4 for current */

    if (!is_double) {
        /* Single-precision: VLDR/VSTR or VLDM/VSTM */
        int sd = ((insn >> 12) & 0xF) * 2 + ((insn >> 22) & 1);

        if (p && !w) {
            /* VLDR / VSTR (immediate offset, no writeback) */
            uint32_t addr = u ? base + offset : base - offset;
            if (l) {
                vfp_s_from_bits(cpu, sd, r32(cpu, addr));
            } else {
                w32(cpu, addr, vfp_s_bits(cpu, sd));
            }
        } else {
            /* VLDM / VSTM (multiple) */
            int count = insn & 0xFF;
            uint32_t addr = base;
            if (!u) addr = base - count * 4;  /* decrement */
            if (p && u) addr += 4;             /* IB */
            for (int i = 0; i < count && (sd + i) < 32; i++) {
                if (l) {
                    vfp_s_from_bits(cpu, sd + i, r32(cpu, addr));
                } else {
                    w32(cpu, addr, vfp_s_bits(cpu, sd + i));
                }
                addr += 4;
            }
            if (w) {
                if (u) cpu->r[rn] = base + count * 4;
                else   cpu->r[rn] = base - count * 4;
            }
        }
    } else {
        /* Double-precision: VLDR/VSTR or VLDM/VSTM */
        int dd = ((insn >> 12) & 0xF) | (((insn >> 22) & 1) << 4);

        if (p && !w) {
            /* VLDR / VSTR (immediate offset) */
            uint32_t addr = u ? base + offset : base - offset;
            if (l) {
                vfp_s_from_bits(cpu, dd * 2, r32(cpu, addr));
                vfp_s_from_bits(cpu, dd * 2 + 1, r32(cpu, addr + 4));
            } else {
                w32(cpu, addr, vfp_s_bits(cpu, dd * 2));
                w32(cpu, addr + 4, vfp_s_bits(cpu, dd * 2 + 1));
            }
        } else {
            /* VLDM / VSTM (count = word count, 2 per double) */
            int wcount = insn & 0xFF;
            int dcount = wcount / 2;
            uint32_t addr = base;
            if (!u) addr = base - wcount * 4;
            if (p && u) addr += 4;
            for (int i = 0; i < dcount && (dd + i) * 2 + 1 < 32; i++) {
                if (l) {
                    vfp_s_from_bits(cpu, (dd + i) * 2, r32(cpu, addr));
                    vfp_s_from_bits(cpu, (dd + i) * 2 + 1, r32(cpu, addr + 4));
                } else {
                    w32(cpu, addr, vfp_s_bits(cpu, (dd + i) * 2));
                    w32(cpu, addr + 4, vfp_s_bits(cpu, (dd + i) * 2 + 1));
                }
                addr += 8;
            }
            if (w) {
                if (u) cpu->r[rn] = base + wcount * 4;
                else   cpu->r[rn] = base - wcount * 4;
            }
        }
    }
    return 1;
}

/* VMOV between two ARM registers and a double VFP register */
static int vfp_mrrc_mcrr(ARM9 *cpu, uint32_t insn) {
    int l = (insn >> 20) & 1;
    int rd2 = (insn >> 16) & 0xF;  /* Rt2 (high word) */
    int rd  = (insn >> 12) & 0xF;  /* Rt  (low word) */
    int m   = insn & 0xF;          /* Dm  */

    if (l) {
        /* VMOV Rd, Rd2, Dm — double to two ARM */
        cpu->r[rd]  = vfp_s_bits(cpu, m * 2);
        cpu->r[rd2] = vfp_s_bits(cpu, m * 2 + 1);
    } else {
        /* VMOV Dm, Rd, Rd2 — two ARM to double */
        vfp_s_from_bits(cpu, m * 2, cpu->r[rd]);
        vfp_s_from_bits(cpu, m * 2 + 1, cpu->r[rd2]);
    }
    return 1;
}

static void exec_arm(ARM9 *cpu, uint32_t insn) {
    if((insn>>28)==0xF){
        /* ARMv5 unconditional space: BLX <imm> calls Thumb code, the H bit
         * adding a halfword; PLD and the rest are hints or unpredictable. */
        if((insn&0x0E000000)==0x0A000000){
            int32_t off=((int32_t)(insn<<8)>>6)|(int32_t)((insn>>23)&2);
            LR=PC-4;
            PC=(uint32_t)((int32_t)PC+off);
            CPSR|=ARM9_FLAG_T;
        }
        return;
    }
    if(!cond_ok(cpu,insn>>28)) return;

    /* The common classes straight away by bits 27-25, where none of the
     * special encodings tested below in order can match but these three:
     * MSR immediate (in 001), the undefined space and PLD (in 01x) - the
     * same result as going down the list, without a dozen tests first
     * for most instructions. */
    switch((insn>>25)&7){
    case 1:
        if((insn&0x0FB00000)==0x03200000) break;   /* MSR immediate */
        goto data_processing;
    case 2: case 3:
        if((insn&0x0E000010)==0x06000010) break;   /* undefined */
        if((insn&0x0D70F000)==0x0550F000) return;  /* PLD */
        goto load_store;
    case 4:
        goto load_store_multiple;
    case 5: {   /* B / BL */
        int32_t off=(int32_t)(insn<<8)>>6;
        if(insn&(1<<24)) LR=PC-4;
        PC=(uint32_t)((int32_t)PC+off);
        return;
    }
    }

    /* BX Rm — Branch and Exchange (ARM→Thumb or Thumb→ARM) */
    if((insn&0x0FFFFFF0)==0x012FFF10){
        uint32_t a=cpu->r[insn&0xF];
        if(a&1){CPSR|=ARM9_FLAG_T; PC=a&~1u;}
        else   {CPSR&=~ARM9_FLAG_T; PC=a&~3u;}
        return;
    }
    /* BLX Rm — Branch with Link and Exchange.
     * LR = addr of next instruction = PC-4 (since PC=inst+8 here). */
    if((insn&0x0FFFFFF0)==0x012FFF30){
        uint32_t a=cpu->r[insn&0xF];
        LR = PC - 4;                          /* return address = inst+4 */
        if(a&1){CPSR|=ARM9_FLAG_T; PC=a&~1u;}
        else   {CPSR&=~ARM9_FLAG_T; PC=a&~3u;}
        return;
    }
    /* B / BL */
    if((insn&0x0E000000)==0x0A000000){
        int32_t off=(int32_t)(insn<<8)>>6;
        if(insn&(1<<24)) LR=PC-4;            /* BL: LR = inst+4 */
        PC=(uint32_t)((int32_t)PC+off);
        return;
    }
    /* SWI — Software Interrupt */
    if((insn&0x0F000000)==0x0F000000){ arm9_swi(cpu); return; }
    /* Undefined instruction — trigger UND exception */
    if((insn&0x0E000010)==0x06000010){ arm9_undef(cpu); return; }
    if((insn&0x0FBF0FFF)==0x010F0000){ cpu->r[(insn>>12)&0xF]=(insn&(1<<22))?cpu->spsr:CPSR; return; }
    if((insn&0x0FB00000)==0x03200000||(insn&0x0FB00FF0)==0x01200000){
        uint32_t val=(insn&(1<<25))?decode_imm(insn):cpu->r[insn&0xF];
        uint32_t mask=0; if(insn&(1<<19)) mask|=0xF0000000u; if(insn&(1<<16)) mask|=0xDFu; /* 0xDF not 0xFF: T bit (bit5) must not be changed by MSR */
        if(insn&(1<<22)){ if(insn&(1<<16)) mask|=0x20u; cpu->spsr=(cpu->spsr&~mask)|(val&mask); }
        else set_mode(cpu,(CPSR&~mask)|(val&mask));
        return;
    }
    if((insn&0x0FC000F0)==0x00000090){
        int rd=(insn>>16)&0xF,rn=(insn>>12)&0xF,rs=(insn>>8)&0xF,rm=insn&0xF;
        int s=(insn>>20)&1,a=(insn>>21)&1;
        uint32_t res=cpu->r[rm]*cpu->r[rs]; if(a) res+=cpu->r[rn];
        cpu->r[rd]=res; if(s){SET_N(res>>31);SET_Z(res==0);} return;
    }
    if((insn&0x0F8000F0)==0x00800090){
        int rdhi=(insn>>16)&0xF,rdlo=(insn>>12)&0xF,rs=(insn>>8)&0xF,rm=insn&0xF;
        int s=(insn>>20)&1,a=(insn>>21)&1,sgn=(insn>>22)&1;   /* U bit: 1 = SMULL/SMLAL */
        uint64_t res=sgn?((uint64_t)(int64_t)(int32_t)cpu->r[rm]*(int64_t)(int32_t)cpu->r[rs]):((uint64_t)cpu->r[rm]*cpu->r[rs]);
        if(a) res+=((uint64_t)cpu->r[rdhi]<<32)|cpu->r[rdlo];
        cpu->r[rdhi]=(uint32_t)(res>>32); cpu->r[rdlo]=(uint32_t)res;
        if(s){SET_N(res>>63);SET_Z(res==0);} return;
    }
    if((insn&0x0FB00FF0)==0x01000090){
        int rn=(insn>>16)&0xF,rd=(insn>>12)&0xF,rm=insn&0xF,b=(insn>>22)&1;
        uint32_t addr=cpu->r[rn];
        if(b){uint8_t t=r8(cpu,addr);w8(cpu,addr,(uint8_t)cpu->r[rm]);cpu->r[rd]=t;}
        else {uint32_t t=r32(cpu,addr);w32(cpu,addr,cpu->r[rm]);cpu->r[rd]=t;}
        return;
    }
    if((insn&0x0E000090)==0x00000090&&(insn&0x60)){
        int p=(insn>>24)&1,u=(insn>>23)&1,imm=(insn>>22)&1,w=(insn>>21)&1;
        int l=(insn>>20)&1,rn=(insn>>16)&0xF,rd=(insn>>12)&0xF,sh=(insn>>5)&3;
        uint32_t off=imm?(((insn>>4)&0xF0)|(insn&0xF)):cpu->r[insn&0xF];
        uint32_t base=cpu->r[rn], addr=p?(u?base+off:base-off):base;
        if(!l && sh>=2){
            /* LDRD (SH=2) / STRD (SH=3): Rd, Rd+1 at addr, addr+4 */
            if(sh==2){ cpu->r[rd]=r32(cpu,addr); cpu->r[rd+1]=r32(cpu,addr+4); }
            else     { w32(cpu,addr,cpu->r[rd]); w32(cpu,addr+4,cpu->r[rd+1]); }
            if(!p) addr=u?base+off:base-off;
            if(!p||w) cpu->r[rn]=addr;
            return;
        }
        if(l){ uint32_t v=0;
            if(sh==1) v=r16(cpu,addr);
            else if(sh==2) v=(uint32_t)(int32_t)(int8_t)r8(cpu,addr);
            else if(sh==3) v=(uint32_t)(int32_t)(int16_t)r16(cpu,addr);
            cpu->r[rd]=v;
        } else if(sh==1) w16(cpu,addr,(uint16_t)cpu->r[rd]);
        if(!p) addr=u?base+off:base-off;
        if(!p||w) cpu->r[rn]=addr;
        return;
    }
    /* CLZ — must be before data processing (both match 0x0C000000==0) */
    if((insn&0x0FFF0FF0)==0x016F0F10){
        int rd=(insn>>12)&0xF, rm=insn&0xF;
        cpu->r[rd] = cpu->r[rm] ? (uint32_t)__builtin_clz(cpu->r[rm]) : 32;
        return;
    }
    /* PLD — preload hint, NOP */
    if((insn&0x0D70F000)==0x0550F000) return;
    /* LDRD */
    if((insn&0x0E1000D0)==0x000000D0 && ((insn>>5)&3)==2){
        int p=(insn>>24)&1,u=(insn>>23)&1,imm=(insn>>22)&1,w=(insn>>21)&1;
        int rn=(insn>>16)&0xF,rd=(insn>>12)&0xF;
        uint32_t off=imm?(((insn>>4)&0xF0)|(insn&0xF)):cpu->r[insn&0xF];
        uint32_t base=cpu->r[rn], addr=p?(u?base+off:base-off):base;
        cpu->r[rd]   = r32(cpu, addr);
        cpu->r[rd+1] = r32(cpu, addr+4);
        if(!p) addr=u?base+off:base-off;
        if(!p||w) cpu->r[rn]=addr;
        return;
    }
    /* STRD */
    if((insn&0x0E1000F0)==0x000000F0){
        int p=(insn>>24)&1,u=(insn>>23)&1,imm=(insn>>22)&1,w=(insn>>21)&1;
        int rn=(insn>>16)&0xF,rd=(insn>>12)&0xF;
        uint32_t off=imm?(((insn>>4)&0xF0)|(insn&0xF)):cpu->r[insn&0xF];
        uint32_t base=cpu->r[rn], addr=p?(u?base+off:base-off):base;
        w32(cpu, addr,   cpu->r[rd]);
        w32(cpu, addr+4, cpu->r[rd+1]);
        if(!p) addr=u?base+off:base-off;
        if(!p||w) cpu->r[rn]=addr;
        return;
    }
    /* DSP multiply (SMLA/SMUL/SMLAW/SMLAL/SMULW) */
    if((insn&0x0F900090)==0x01000080){
        int op=(insn>>21)&3;
        int rd=(insn>>16)&0xF,rn=(insn>>12)&0xF,rs=(insn>>8)&0xF,rm=insn&0xF;
        int x=(insn>>5)&1, y=(insn>>6)&1;
        int16_t a=(int16_t)(x ? cpu->r[rm]>>16 : cpu->r[rm]);
        int16_t b=(int16_t)(y ? cpu->r[rs]>>16 : cpu->r[rs]);
        switch(op){
            case 0: cpu->r[rd]=(uint32_t)((int32_t)a*b + (int32_t)cpu->r[rn]); break;
            case 1: if(!x){ int32_t res=(int32_t)(((int64_t)(int32_t)cpu->r[rm]*b)>>16); cpu->r[rd]=(uint32_t)(res+(int32_t)cpu->r[rn]); }
                    else   { cpu->r[rd]=(uint32_t)(((int64_t)(int32_t)cpu->r[rm]*b)>>16); }
                    break;
            case 2: { int64_t acc=((int64_t)cpu->r[rd]<<32)|cpu->r[rn]; acc+=(int64_t)a*b; cpu->r[rd]=(uint32_t)(acc>>32); cpu->r[rn]=(uint32_t)acc; break; }
            case 3: cpu->r[rd]=(uint32_t)((int32_t)a*b); break;
        }
        return;
    }
    /* QADD/QSUB/QDADD/QDSUB */
    if((insn&0x0F900FF0)==0x01000050){
        int op=(insn>>21)&3;
        int rd=(insn>>12)&0xF,rn=(insn>>16)&0xF,rm=insn&0xF;
        int64_t a=(int32_t)cpu->r[rm], b=(int32_t)cpu->r[rn];
        if(op==1||op==3) b*=2;
        int64_t res=(op==0||op==2)?a+b:a-b;
        if(res>0x7FFFFFFF) res=0x7FFFFFFF;
        if(res<-0x80000000LL) res=-0x80000000LL;
        cpu->r[rd]=(uint32_t)(int32_t)res;
        return;
    }
data_processing:
    if((insn&0x0C000000)==0x00000000){
        int imm=(insn>>25)&1,op=(insn>>21)&0xF,s=(insn>>20)&1;
        int rn=(insn>>16)&0xF,rd=(insn>>12)&0xF;
        uint32_t a=cpu->r[rn]; if(rn==15) a=PC;
        uint32_t b; int shc=C_FLAG;
        if(imm){ b=decode_imm(insn); int rot=((insn>>8)&0xF)*2; if(rot) shc=(b>>31)&1; }
        else { SR sr=decode_shift(cpu,insn); b=sr.v; shc=sr.c; }
        uint32_t res=0; int wr=1;
        switch(op){
        case 0x0: res=a&b; if(s){SET_NZ(res);SET_C(shc);} break;
        case 0x1: res=a^b; if(s){SET_NZ(res);SET_C(shc);} break;
        case 0x2: res=do_sub(cpu,a,b,s); break;
        case 0x3: res=do_sub(cpu,b,a,s); break;
        case 0x4: res=do_add(cpu,a,b,s); break;
        case 0x5: res=do_adc(cpu,a,b,s); break;
        case 0x6: res=do_sbc(cpu,a,b,s); break;
        case 0x7: res=do_sbc(cpu,b,a,s); break;
        case 0x8: { uint32_t r=a&b; SET_NZ(r); SET_C(shc); wr=0; } break;
        case 0x9: { uint32_t r=a^b; SET_NZ(r); SET_C(shc); wr=0; } break;
        case 0xA: do_sub(cpu,a,b,1); wr=0; break;
        case 0xB: do_add(cpu,a,b,1); wr=0; break;
        case 0xC: res=a|b;  if(s){SET_NZ(res);SET_C(shc);} break;
        case 0xD: res=b;    if(s){SET_NZ(res);SET_C(shc);} break;
        case 0xE: res=a&~b; if(s){SET_NZ(res);SET_C(shc);} break;
        case 0xF: res=~b;   if(s){SET_NZ(res);SET_C(shc);} break;
        }
        if(wr){ cpu->r[rd]=res; if(rd==15){ if(s) set_mode(cpu,cpu->spsr); PC&=(CPSR&ARM9_FLAG_T)?~1u:~3u; } }
        return;
    }
load_store:
    if((insn&0x0C000000)==0x04000000){
        int i=(insn>>25)&1,p=(insn>>24)&1,u=(insn>>23)&1,b=(insn>>22)&1;
        int w=(insn>>21)&1,l=(insn>>20)&1,rn=(insn>>16)&0xF,rd=(insn>>12)&0xF;
        uint32_t base=cpu->r[rn]; if(rn==15) base=PC;
        uint32_t off=i?decode_shift(cpu,insn).v:(insn&0xFFF);
        uint32_t addr=p?(u?base+off:base-off):base;
        if(l){
            uint32_t v;
            if(b) v=r8(cpu,addr);
            else { int rot=(addr&3)*8; v=r32(cpu,addr); if(rot) v=(v>>rot)|(v<<(32-rot)); }
            cpu->r[rd]=v;
            /* LDR PC interworks on ARMv5: bit 0 selects Thumb. */
            if(rd==15){ if(v&1){ CPSR|=ARM9_FLAG_T; PC=v&~1u; } else PC=v&~3u; }
        } else {
            /* STR PC: stored value = current PC = inst+8 (already set) */
            uint32_t v = (rd==15) ? PC : cpu->r[rd];
            if(b) w8(cpu,addr,(uint8_t)v); else w32(cpu,addr,v);
        }
        if(!p) addr=u?base+off:base-off;
        if((!p||w)&&rn!=rd) cpu->r[rn]=addr;
        return;
    }
load_store_multiple:
    if((insn&0x0E000000)==0x08000000){
        int p=(insn>>24)&1,u=(insn>>23)&1,w=(insn>>21)&1,l=(insn>>20)&1,rn=(insn>>16)&0xF;
        uint16_t rlist=insn&0xFFFF;
        uint32_t base=cpu->r[rn];
        int cnt=__builtin_popcount(rlist);
        /* Start address for iteration (always increments addr by 4 per register):
         * IA (p=0,u=1): addr = base
         * IB (p=1,u=1): addr = base + 4
         * DA (p=0,u=0): addr = base - cnt*4 + 4
         * DB (p=1,u=0): addr = base - cnt*4        ← PUSH / STMDB */
        uint32_t addr = u ? (base + (uint32_t)(p?4:0))
                          : (base - (uint32_t)(cnt*4) + (uint32_t)(p?0:4));
        for(int i=0;i<16;i++){
            if(!(rlist&(1<<i))) continue;
            if(l){
                uint32_t v=r32(cpu,addr);
                cpu->r[i]=v;
                /* LDM with PC in list: loaded value IS the new PC.
                 * If S-bit (bit22) set and PC in list → restore CPSR from SPSR */
                if(i==15) {
                    if(insn & (1<<22)) {
                        set_mode(cpu, cpu->spsr);
                        PC = v & ((CPSR & ARM9_FLAG_T) ? ~1u : ~3u);
                    } else if(v & 1) { CPSR|=ARM9_FLAG_T; PC=v&~1u; }
                    else PC = v & ~3u;
                }
            } else {
                w32(cpu,addr,cpu->r[i]);
            }
            addr+=4;
        }
        if(w && !(l && (rlist>>rn&1))) cpu->r[rn]=u?base+(uint32_t)(cnt*4):base-(uint32_t)(cnt*4);
        return;
    }
    /* MCRR/MRRC — Coprocessor double register transfer (VFP: VMOV Dm, Rd, Rn) */
    if ((insn & 0x0FE00000) == 0x0C400000) {
        int cp = (insn >> 8) & 0xF;
        if (cp == 10 || cp == 11) { if (vfp_mrrc_mcrr(cpu, insn)) return; }
    }
    /* LDC/STC — Coprocessor data transfer (VFP: VLDR/VSTR/VLDM/VSTM) */
    if ((insn & 0x0E000000) == 0x0C000000 && !((insn & 0x0F000010) == 0x0E000010)) {
        int cp = (insn >> 8) & 0xF;
        if (cp == 10 || cp == 11) { if (vfp_ldc_stc(cpu, insn)) return; }
    }
    /* CDP — Coprocessor data processing (VFP: VADD/VMUL/etc.) */
    if ((insn & 0x0F000010) == 0x0E000000) {
        int cp = (insn >> 8) & 0xF;
        if (cp == 10 || cp == 11) { if (vfp_cdp(cpu, insn)) return; }
    }
    /* MCR/MRC — Coprocessor register transfer */
    if((insn&0x0F000010)==0x0E000010){
        int l=(insn>>20)&1, crn=(insn>>16)&0xF, rd=(insn>>12)&0xF;
        int cp=(insn>>8)&0xF, op2=(insn>>5)&7, crm=insn&0xF;
        if(cp==15){
            if(l) cpu->r[rd]=cp15_read(&cpu->cp15,(uint32_t)crn,(uint32_t)crm,(uint32_t)op2);
            else  cp15_write(&cpu->cp15,(uint32_t)crn,(uint32_t)crm,(uint32_t)op2,cpu->r[rd]);
        } else if (cp == 10 || cp == 11) {
            if (vfp_mcr_mrc(cpu, insn)) return;
            fprintf(stderr,"[VFP] Unhandled MCR/MRC: 0x%08X PC=0x%08X\n",insn,PC-8);
        } else {
            static int unk_cp_log = 0;
            if (unk_cp_log++ < 5)
                fprintf(stderr,"[ARM9] MCR/MRC unknown CP%d insn=0x%08X PC=0x%08X\n",cp,insn,PC-8);
        }
        return;
    }

    /* Unhandled — trigger Undefined Instruction exception */
    fprintf(stderr,"[ARM9] Undef→UND: 0x%08X PC=0x%08X\n",insn,PC-8);
    arm9_undef(cpu);
}

static void exec_thumb(ARM9 *cpu, uint16_t insn) {
    if((insn>>13)==0){ int op=(insn>>11)&3,sh=(insn>>6)&0x1F,rs=(insn>>3)&7,rd=insn&7; SR sr=bshift(cpu->r[rs],op,sh?sh:32,C_FLAG); cpu->r[rd]=sr.v; SET_NZ(sr.v); SET_C(sr.c); return; }
    if((insn>>11)==3){ int op=(insn>>9)&1,i=(insn>>10)&1,rn=(insn>>6)&7,rs=(insn>>3)&7,rd=insn&7; uint32_t b=i?(uint32_t)rn:cpu->r[rn]; cpu->r[rd]=op?do_sub(cpu,cpu->r[rs],b,1):do_add(cpu,cpu->r[rs],b,1); return; }
    if((insn>>13)==1){ int op=(insn>>11)&3,rd=(insn>>8)&7; uint32_t imm=insn&0xFF; switch(op){ case 0: cpu->r[rd]=imm; SET_NZ(imm); break; case 1: do_sub(cpu,cpu->r[rd],imm,1); break; case 2: cpu->r[rd]=do_add(cpu,cpu->r[rd],imm,1); break; case 3: cpu->r[rd]=do_sub(cpu,cpu->r[rd],imm,1); break; } return; }
    if((insn>>10)==0x10){ int op=(insn>>6)&0xF,rs=(insn>>3)&7,rd=insn&7; uint32_t a=cpu->r[rd],b=cpu->r[rs]; switch(op){ case 0: cpu->r[rd]=a&b;SET_NZ(a&b);break; case 1: cpu->r[rd]=a^b;SET_NZ(a^b);break; case 2:{SR s=bshift(a,0,b&0xFF,C_FLAG);cpu->r[rd]=s.v;SET_NZC(s.v,s.c);}break; case 3:{SR s=bshift(a,1,b&0xFF,C_FLAG);cpu->r[rd]=s.v;SET_NZC(s.v,s.c);}break; case 4:{SR s=bshift(a,2,b&0xFF,C_FLAG);cpu->r[rd]=s.v;SET_NZC(s.v,s.c);}break; case 5: cpu->r[rd]=do_adc(cpu,a,b,1);break; case 6: cpu->r[rd]=do_sbc(cpu,a,b,1);break; case 7:{SR s=bshift(a,3,b&0xFF,C_FLAG);cpu->r[rd]=s.v;SET_NZC(s.v,s.c);}break; case 8:{uint32_t r=a&b;SET_NZ(r);}break; case 9: cpu->r[rd]=do_sub(cpu,0,b,1);break; case 10: do_sub(cpu,a,b,1);break; case 11: do_add(cpu,a,b,1);break; case 12: cpu->r[rd]=a|b;SET_NZ(a|b);break; case 13: cpu->r[rd]=a*b;SET_NZ(a*b);break; case 14: cpu->r[rd]=a&~b;SET_NZ(a&~b);break; case 15: cpu->r[rd]=~b;SET_NZ(~b);break; } return; }
    if((insn>>10)==0x11){ int op=(insn>>8)&3,h1=(insn>>7)&1,h2=(insn>>6)&1; int rs=((insn>>3)&7)+(h2?8:0),rd=(insn&7)+(h1?8:0); switch(op){ case 0: cpu->r[rd]+=cpu->r[rs];break; case 1: do_sub(cpu,cpu->r[rd],cpu->r[rs],1);break; case 2: cpu->r[rd]=cpu->r[rs];break; case 3:{ uint32_t a=cpu->r[rs]; if(a&1){CPSR|=ARM9_FLAG_T;PC=a&~1u;}else{CPSR&=~ARM9_FLAG_T;PC=a&~3u;} }break; } return; }
    /* Thumb LDR PC-relative: LDR Rd,[PC,#imm8*4]
     * addr = (PC & ~2) + imm8*4  — PC is already inst+4 here */
    if((insn>>11)==0x9){ int rd=(insn>>8)&7; cpu->r[rd]=r32(cpu,(PC&~2u)+((insn&0xFF)<<2)); return; }
    /* Thumb SP-relative LDR/STR: encoding 0x9xxx
     * bit11=1(LDR)/0(STR), bits10:8=Rd, bits7:0=imm8 (word-scaled) */
    if((insn>>12)==0x9){
        int l=(insn>>11)&1, rd=(insn>>8)&7;
        uint32_t addr=SP+((uint32_t)(insn&0xFF)<<2);
        if(l) cpu->r[rd]=r32(cpu,addr);
        else  w32(cpu,addr,cpu->r[rd]);
        return;
    }
    /* ADR: ADD Rd,PC,#imm8*4  and  ADD Rd,SP,#imm8*4
     * bit11=0→PC-based, bit11=1→SP-based */
    if((insn>>12)==0xA){
        int sp=(insn>>11)&1, rd=(insn>>8)&7;
        uint32_t base = sp ? SP : (PC&~2u);
        cpu->r[rd] = base + ((uint32_t)(insn&0xFF)<<2);
        return;
    }
    if((insn>>12)==5&&!((insn>>9)&1)){ int l=(insn>>11)&1,b=(insn>>10)&1,ro=(insn>>6)&7,rb=(insn>>3)&7,rd=insn&7; uint32_t addr=cpu->r[rb]+cpu->r[ro]; if(l) cpu->r[rd]=b?r8(cpu,addr):r32(cpu,addr); else { if(b) w8(cpu,addr,(uint8_t)cpu->r[rd]); else w32(cpu,addr,cpu->r[rd]); } return; }
    if((insn>>13)==3){ int b=(insn>>12)&1,l=(insn>>11)&1,off=(insn>>6)&0x1F,rb=(insn>>3)&7,rd=insn&7; uint32_t addr=cpu->r[rb]+(b?(uint32_t)off:(uint32_t)(off<<2)); if(l) cpu->r[rd]=b?r8(cpu,addr):r32(cpu,addr); else { if(b) w8(cpu,addr,(uint8_t)cpu->r[rd]); else w32(cpu,addr,cpu->r[rd]); } return; }
    if((insn>>12)==0x8){ int l=(insn>>11)&1,off=((insn>>6)&0x1F)<<1,rb=(insn>>3)&7,rd=insn&7; uint32_t addr=cpu->r[rb]+(uint32_t)off; if(l) cpu->r[rd]=r16(cpu,addr); else w16(cpu,addr,(uint16_t)cpu->r[rd]); return; }
    if((insn>>8)==0xB0){ int s=(insn>>7)&1; uint32_t off=(uint32_t)((insn&0x7F)<<2); SP=s?SP-off:SP+off; return; }
    if((insn>>12)==0xB){ int l=(insn>>11)&1,r=(insn>>8)&1; uint8_t rlist=(uint8_t)(insn&0xFF); if(!l){ if(r){SP-=4;w32(cpu,SP,LR);} for(int i=7;i>=0;i--) if(rlist&(1<<i)){SP-=4;w32(cpu,SP,cpu->r[i]);} } else { for(int i=0;i<8;i++) if(rlist&(1<<i)){cpu->r[i]=r32(cpu,SP);SP+=4;} if(r){PC=r32(cpu,SP)&~1u;SP+=4;} } return; }
    if((insn>>12)==0xC){ int l=(insn>>11)&1,rb=(insn>>8)&7; uint8_t rlist=(uint8_t)(insn&0xFF); uint32_t addr=cpu->r[rb]; for(int i=0;i<8;i++){ if(!(rlist&(1<<i))) continue; if(l) cpu->r[i]=r32(cpu,addr); else w32(cpu,addr,cpu->r[i]); addr+=4; } cpu->r[rb]=addr; return; }
    if((insn>>12)==0xD){ int cond=(insn>>8)&0xF; if(cond==0xF){ arm9_swi(cpu); return; } if(cond_ok(cpu,(uint32_t)cond)){ PC=(uint32_t)((int32_t)PC+(int8_t)(insn&0xFF)*2); } return; }
    /* Thumb unconditional B: signed 11-bit offset in halfwords */
    if((insn>>11)==0x1C){ int32_t off=(int32_t)(((int16_t)((insn&0x7FF)<<5))>>4); PC=(uint32_t)((int32_t)PC+off); return; }
    if((insn>>12)==0xF){ int h=(insn>>11)&1; if(!h){ int32_t off=(int32_t)(((int16_t)(insn<<5))>>4)<<1; LR=(uint32_t)((int32_t)PC+off); } else { uint32_t addr=LR+((uint32_t)(insn&0x7FF)<<1); LR=(PC-2)|1; PC=addr; } return; }
    /* BKPT — treat as NOP in emulator context */
    if((insn>>8)==0xBE) return;
    /* Unhandled Thumb — trigger Undefined Instruction exception */
    fprintf(stderr,"[ARM9] Thumb undef→UND: 0x%04X PC=0x%08X\n",insn,PC-4);
    arm9_undef(cpu);
}

void arm9_reset(ARM9 *cpu) {
    memset(cpu->r,0,sizeof(cpu->r));
    CPSR=ARM9_MODE_SVC|ARM9_FLAG_I|ARM9_FLAG_F; cpu->spsr=0; PC=0; cpu->cycles=0;
    cp15_reset(&cpu->cp15);
    printf("[ARM9] Reset PC=0x%08X CPSR=0x%08X\n",PC,CPSR);
}

/* Vector base: 0x00000000 or 0xFFFF0000 depending on CP15 HIVEC bit */
static inline uint32_t vec_base(ARM9 *cpu) {
    return cpu->cp15.hivec ? 0xFFFF0000u : 0x00000000u;
}

/* Estimate cycles consumed by one instruction.
 * ARM926EJ-S pipeline: most ALU = 1 cycle, LDR = 2, LDM/STM = 1+N,
 * branch/BL = 3 (pipeline flush), MUL = 2-5.
 * We use a simple lookup on the top 4 bits of the ARM encoding. */
static int insn_cycles_arm(uint32_t insn) {
    uint32_t grp = (insn >> 25) & 7;
    switch (grp) {
        case 0: case 1: {           /* Data processing / misc */
            uint32_t op = (insn >> 21) & 0xF;
            /* MUL/MLA encoded in grp 0 with bits[7:4]=1001 */
            if ((insn & 0x0FC000F0) == 0x00000090) return 3;
            if ((insn & 0x0F8000F0) == 0x00800090) return 4; /* MULL */
            /* LDR/STR in grp 0: LDRH/STRH */
            if ((insn & 0x0E000090) == 0x00000090 && (insn & 0x60)) return 2;
            (void)op;
            return 1;
        }
        case 2: case 3:             /* LDR/STR */
            return 2;
        case 4:                     /* LDM/STM */
            return 1 + __builtin_popcount(insn & 0xFFFF);
        case 5:                     /* B/BL — pipeline flush */
            return 3;
        default:
            return 1;
    }
}

static int insn_cycles_thumb(uint16_t insn) {
    /* BL prefix/suffix, B, BX = 3; LDR/STR = 2; PUSH/POP = 1+N; else 1 */
    if ((insn >> 12) == 0xF)  return 3;  /* BL */
    if ((insn >> 11) == 0x1C) return 3;  /* B unconditional */
    if ((insn >> 12) == 0xD)  return 1;  /* B conditional (predicted not taken) */
    if ((insn >> 13) == 3)    return 2;  /* LDR/STR */
    if ((insn >> 12) == 0x8)  return 2;  /* LDRH/STRH */
    if ((insn >> 9)  == 0x16) return 2;  /* LDR SP-relative */
    if ((insn >> 12) == 0xB)  /* PUSH/POP */
        return 1 + __builtin_popcount(insn & 0x1FF);
    if ((insn >> 12) == 0xC)  /* LDMIA/STMIA */
        return 1 + __builtin_popcount(insn & 0xFF);
    return 1;
}

/* arm9_step — fetch, advance PC to simulate 3-stage pipeline, execute.
 *
 * ARM pipeline convention: during execution, PC = instruction_addr + 8.
 * We simulate this by setting PC = inst_addr + 8 before exec_arm so that
 * all PC-relative ops (LDR, ADR, B, BL) work correctly without any special
 * casing inside exec_arm.
 *
 * After exec_arm:
 *   - If no branch was taken: PC is still inst+8 → restore to inst+4
 *     (sequential execution, next instruction)
 *   - If a branch was taken: exec_arm set PC to target → leave it
 *
 * Thumb pipeline: PC = inst+4 during execution. Same logic, smaller delta.
 */
/* Instruction fetch through the cached host page when there is one. */
static inline const uint8_t *fetch_host(ARM9 *cpu, uint32_t a) {
    if (cpu->fetch_ptr && (a & ~0xFFFu) == cpu->fetch_page && cpu->fetch_gen == cpu->cp15.tlb_gen)
        return cpu->fetch_ptr + (a & 0xFFF);
    if (!cpu->mem_page) return NULL;
    const uint8_t *p = cpu->mem_page(cpu->mem_ctx, a);
    cpu->fetch_ptr = p;
    cpu->fetch_page = a & ~0xFFFu;
    cpu->fetch_gen = cpu->cp15.tlb_gen;   /* after the lookup, which may flush */
    return p ? p + (a & 0xFFF) : NULL;
}

int arm9_step(ARM9 *cpu) {
    uint32_t inst_addr = PC;
    int cyc;
    const uint8_t *h = fetch_host(cpu, inst_addr);
    if (T_FLAG) {
        uint16_t i;
        if (h) memcpy(&i, h, 2); else i = r16(cpu, inst_addr);
        PC = inst_addr + 4;
        exec_thumb(cpu, i);
        if (PC == inst_addr + 4) PC = inst_addr + 2;
        cyc = insn_cycles_thumb(i);
    } else {
        uint32_t i;
        if (h) memcpy(&i, h, 4); else i = r32(cpu, inst_addr);
        PC = inst_addr + 8;
        exec_arm(cpu, i);
        /* PC left at inst+8 means no branch - unless the instruction was a
         * taken branch to exactly there. */
        if (PC == inst_addr + 8 &&
            !((i & 0x0E000000) == 0x0A000000 && cond_ok(cpu, i >> 28)))
            PC = inst_addr + 4;
        cyc = insn_cycles_arm(i);
    }
    cpu->cycles += (uint64_t)cyc;
    return cyc;
}

/* Run until at least 'cycles' CPU cycles have elapsed */
void arm9_run(ARM9 *cpu, int cycles) {
    int done = 0;
    while (done < cycles)
        done += arm9_step(cpu);
}

/* IRQ: called between instructions.
 * PC = address of next instruction to execute.
 * LR_irq = address to return to after SUBS PC,LR,#4 = next instruction + 4.
 * (Standard ARM: LR_irq = interrupted_instruction + 8) */
void arm9_irq(ARM9 *cpu) {
    if(CPSR&ARM9_FLAG_I) return;
    save_bank(cpu,CPSR&0x1F);
    cpu->spsr_irq = CPSR;
    cpu->r14_irq  = PC + 4;   /* LR_irq so that SUBS PC,LR,#4 returns to PC */
    CPSR = (CPSR & ~0x3Fu) | ARM9_MODE_IRQ | ARM9_FLAG_I;
    load_bank(cpu, ARM9_MODE_IRQ);
    PC = vec_base(cpu) + 0x18;
}

void arm9_fiq(ARM9 *cpu) {
    if(CPSR&ARM9_FLAG_F) return;
    save_bank(cpu,CPSR&0x1F);
    cpu->spsr_fiq = CPSR;
    cpu->r14_fiq  = PC + 4;
    CPSR = (CPSR & ~0x3Fu) | ARM9_MODE_FIQ | ARM9_FLAG_I | ARM9_FLAG_F;
    load_bank(cpu, ARM9_MODE_FIQ);
    PC = vec_base(cpu) + 0x1C;
}

/* SWI: called from exec_arm when PC = inst+8.
 * LR_svc = address of instruction after SWI = inst+4 = PC-4. */
void arm9_swi(ARM9 *cpu) {
    save_bank(cpu,CPSR&0x1F);
    cpu->spsr_svc = CPSR;
    cpu->r14_svc  = PC - 4;   /* inst+4 = return address after SWI */
    CPSR = (CPSR & ~0x3Fu) | ARM9_MODE_SVC | ARM9_FLAG_I;
    load_bank(cpu, ARM9_MODE_SVC);
    PC = vec_base(cpu) + 0x08;
}

void arm9_undef(ARM9 *cpu) {
    save_bank(cpu,CPSR&0x1F);
    cpu->spsr_und = CPSR;
    cpu->r14_und  = PC - 4;
    CPSR = (CPSR & ~0x3Fu) | ARM9_MODE_UND | ARM9_FLAG_I;
    load_bank(cpu, ARM9_MODE_UND);
    PC = vec_base(cpu) + 0x04;
}

uint32_t arm9_get_pc(ARM9 *cpu) { return PC; }
