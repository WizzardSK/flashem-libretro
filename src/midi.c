#include "midi.h"
#include <string.h>
#include <stdio.h>

/* Real BIOS writes captured by tools/midi_bios_probe.c. The buffers contain
 * ten CDDA sectors per planar channel; end is the final 16-bit sample address.
 * Gain FF/FF00 routes a mono stream left/right. Broader mode/gain/envelope
 * behavior is not inferred from these two configurations. */
static int stream_supported(const uint32_t *r) {
    int streaming = r[3] == 0x10 && r[7] == 0xffffffff &&
                    (r[8] == 0xff || r[8] == 0xff00);
    /* BIOS UI records100B2EB4/EE0/F0 point directly into mono signed16LE
     * RIFF/WAVE payloads whose headers specify22050Hz, proving this tuple. */
    int ui = r[3] == 0x1e4 &&
             ((r[7] == 0xffffffff && r[8] == 0xffff) ||
              (r[7] == 0x0000ffff && r[8] == 0xff) ||
              (r[7] == 0xffff0000 && r[8] == 0xff00));
    return (streaming || ui) && r[4] == 0x1000 && r[5] == 0xffffffff &&
           r[6] == 0xffff0000 && r[9] == 0 &&
           !(r[0] & 1) && !(r[1] & 1) && !(r[2] & 1) &&
           r[0] <= r[1] && r[2] <= r[1];
}

/* Dingo's real soundfont driver (BIOS equivalent1008BDD8) writes this tuple.
 * It strips descriptor sample-rate bits0C and computes a /4096 pitch from
 * the octave table. Signed16 data and root-note frequency agree independently.
 * BIOS1008BF2C clears mode bit10 on note release: sustain loops then run
 * through the inclusive end once. Envelope/interpolation remain approximate. */
static int music_supported(const uint32_t *r) {
    return (r[3]==0 || r[3]==0x10 || r[3]==2) && r[4]<=0x1000 && r[5]==0xffffffff &&
           r[6]==0xf6ff0000 && r[7]==0xffffffff &&
           (r[8]&0xffff0000)==0xffff0000 && r[9]==0xffffff00 &&
           !(r[0]&1) && !(r[1]&1) && !(r[2]&1) &&
           r[0]<=r[1] && r[2]<=r[1] && (r[3]!=0x10 || r[2]>=r[0]) &&
           /* Soundfont compressed end denotes the final16-bit word, unlike
            * fixed-rate effect descriptors' final-byte endpoint. */
           (r[3]!=2 || ((uint64_t)r[1]-r[0]+2)%34==0);
}

static int ima_supported(const uint32_t *r) {
    if(r[3]==2) return music_supported(r);
    return (r[3]==0x12 || r[3]==0x16 || r[3]==0x1e6 || r[3]==0x1ea ||
            r[3]==0x1f6 || r[3]==0x1fa) && r[4]==0x1000 &&
           r[5]==0xffffffff && r[6]==0xffff0000 && r[7]==0xffffffff &&
           (r[8]==0xff || r[8]==0xff00 || r[8]==0xffff) && r[9]==0 &&
           !(r[0]&1) && r[0]<=r[1] && r[2]>=r[0] && r[2]<=r[1] &&
           /* Streaming constructors use either final-byte or final-word
            * endpoints (BIOS1003A308 vs1003A384). Wacky uses the latter
            * for a500-block IMA4 ring. Both must end on a complete block. */
           (((uint64_t)r[1]-r[0]+1)%34==0 ||
            ((r[3]==0x12 || r[3]==0x16) && ((uint64_t)r[1]-r[0]+2)%34==0)) &&
           (r[2]-r[0])%34==0;
}

/* Plain PCM at a fixed rate, the family the observed tuples above belong to,
 * read from the mode bits instead of a whole tuple: 0x1E0 with bit 0 for 8-bit
 * unsigned samples (else 16-bit signed), bit 2 for 22050 Hz or bit 3 for
 * 11025 Hz, bit 4 to loop from +0x08; bit 1 (IMA4) is ima_supported's. The
 * pitch at +0x10 scales the rate, /4096: vtech-lib's fctest plays 22 kHz
 * 8-bit music as mode 0x1E9 at pitch 0x1FFF (mrdudz, issue #3). The bits come
 * from homebrew written against the hardware, not from a capture. */
static int pcm_supported(const uint32_t *r) {
    unsigned rate = r[3] & 0xc;
    return (r[3] & ~0x1du) == 0x1e0 && (rate == 4 || rate == 8) &&
           r[4] > 0 && r[4] <= 0x1fff && r[0] <= r[1] &&
           (!(r[3] & 0x10) || (r[2] >= r[0] && r[2] <= r[1])) &&
           ((r[3] & 1) || (!(r[0] & 1) && !(r[1] & 1)));
}

/* Apple IMA4: big-endian packed predictor/index, then32low-first nibble pairs.
 * Verified against FFmpeg adpcm_ima_qt on original MJP and SF010 assets.
 * Retain predictor low bits between consistent headers as QuickTime does. */
static int ima_block(Midi *m,unsigned v) {
    static const int step[89]={7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
    static const int change[8]={-1,-1,-1,-1,2,4,6,8};
    int16_t word;unsigned h,i;int pred,index;
    if(!m->read_sample || !m->read_sample(m->memory_ctx,m->cursor[v],&word))return 0;
    h=((uint16_t)word>>8)|(((uint16_t)word&255)<<8);
    index=h&127;pred=(int16_t)(h&0xff80);
    if(index>88)return 0;
    if(m->ima_valid[v] && index==m->ima_index[v] &&
       pred-m->ima_predictor[v]<=127 && pred-m->ima_predictor[v]>=-127)
        pred=m->ima_predictor[v];
    for(i=0;i<64;i++) {
        unsigned byte,n;int s,d;
        if((i&3)==0 && !m->read_sample(m->memory_ctx,m->cursor[v]+2+i/2,&word))return 0;
        byte=((uint16_t)word>>((i&2)?8:0))&255;n=(byte>>((i&1)*4))&15;
        s=step[index];d=(s>>3)+((n&1)?s>>2:0)+((n&2)?s>>1:0)+((n&4)?s:0);
        pred+=(n&8)?-d:d;if(pred>32767)pred=32767;if(pred< -32768)pred=-32768;
        index+=change[n&7];if(index<0)index=0;if(index>88)index=88;
        m->ima_pcm[v][i]=(int16_t)pred;
    }
    m->ima_predictor[v]=pred;m->ima_index[v]=(uint8_t)index;m->ima_valid[v]=1;
    m->ima_pos[v]=0;m->cursor[v]+=2;return 1;
}

void midi_reset(Midi *m) {
    memset(m, 0, sizeof(*m));
    /* Unity until firmware configures the master: compatibility default,
     * not a measured silicon reset value. Normal BIOS sets this explicitly. */
    m->master_level[0] = m->master_level[1] = 0x7fff;
}

static void master_update(Midi *m) {
    unsigned c;
    for (c = 0; c < 2; c++) {
        unsigned shift = c * 16;
        /* BIOS volume setters10013D90/DD8/E58 always useFFFF. The fade
         * state machine uses it to establish a starting level before a ramp.
         * Other ramp encodings/timing remain unknown: retain current level. */
        if (((m->regs[0x1184 / 4] >> shift) & 0xffff) == 0xffff)
            m->master_level[c] = (m->regs[0x1188 / 4] >> shift) & 0x7fff;
    }
}

void midi_set_trace(Midi *m, void *ctx,
                    void (*trace)(void *, unsigned, const uint32_t *, uint32_t, int)) {
    m->trace_ctx = ctx;
    m->trace = trace;
}

uint32_t midi_read(const Midi *m, uint32_t o) {
    if ((o & 3) || o >= MIDI_REGISTER_BYTES) return 0;
    if (o == 0x1248)
        return m->master_level[0] | ((uint32_t)m->master_level[1] << 16);
    if (o >= 0x1010 && o <= 0x101c) return 0; /* channel command strobes */
    if (o < 0x1000 && (o & 0x3f) == 0x28) return m->cursor[o / 64];
    if (o < 0x1000 && (o & 0x3f) == 0x34)
        /* Firmware tests zero/nonzero only; exact active substate unverified. */
        return (m->regs[o / 4] & ~0x70000u) |
               ((m->active & (UINT64_C(1) << (o / 64))) ? 0x10000 : 0);
    return m->regs[o / 4];
}

void midi_write(Midi *m, uint32_t o, uint32_t value, uint32_t pc) {
    unsigned n;
    if ((o & 3) || o >= MIDI_REGISTER_BYTES) return;
    if (o == 0x1004) {
        /* BIOS1008A8E4 acknowledges one of six pending sources by writing
         * its bit while preserving upper control fields. Event timing and
         * source generation remain unimplemented; writes cannot raise IRQs. */
        m->regs[o / 4] = (value & ~0x3fu) |
                         (m->regs[o / 4] & 0x3fu & ~value);
        return;
    }
    if (o >= 0x1010 && o <= 0x101c) {
        unsigned base = (o & 4) ? 32 : 0;
        int on = o < 0x1018;
        for (n = 0; n < 32; n++) if (value & (1u << n)) {
            unsigned v = base + n;
            uint64_t bit = UINT64_C(1) << v;
            if (on) {
                m->dry_voices &= ~bit;
                m->requested |= bit;
                m->cursor[v] = m->regs[v * 16];
                m->half_phase[v] = 0;
                m->phase[v] = 0;
                m->ima_pos[v]=64;m->ima_valid[v]=0;
                if (stream_supported(&m->regs[v * 16]) || music_supported(&m->regs[v * 16]) || ima_supported(&m->regs[v*16]) || pcm_supported(&m->regs[v * 16])) m->active |= bit;
                else { m->active &= ~bit; m->unsupported_starts++; }
            } else { m->requested &= ~bit; m->active &= ~bit; }
            if (m->trace) m->trace(m->trace_ctx, v, &m->regs[v * 16], pc, on);
        }
        return;
    }
    /* Parameters remain readable for firmware RMW, including unknown fields.
     * Firmware proves pitch setter width, not the hardware masking semantics;
     * preserve full writes until those semantics are established. */
    m->regs[o / 4] = value;
    if (o == 0x1184 || o == 0x1188) master_update(m);
}

void midi_set_memory(Midi *m, void *ctx,
                     int (*read_sample)(void *, uint32_t, int16_t *)) {
    m->memory_ctx = ctx; m->read_sample = read_sample;
}

static int16_t clip(int32_t v) {
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

void midi_render(Midi *m, int16_t *stereo, unsigned frames) {
    unsigned i, v;
    for (i = 0; i < frames; i++) {
        int32_t left = 0, right = 0;
        for (v = 0; v < MIDI_VOICES; v++) {
            uint64_t bit = UINT64_C(1) << v;
            uint32_t *r = &m->regs[v * 16];
            int16_t sample;
            int music,ima;
            if (!(m->active & bit)) continue;
            music=music_supported(r);
            ima=ima_supported(r);
            if (!music && !ima && !stream_supported(r)) {
                if (!pcm_supported(r)) {
                    m->active &= ~bit; m->unsupported_starts++; continue;
                }
                /* Generic PCM: base rate * pitch/4096 against the 44100 Hz
                 * output, as a /4096 phase step */
                unsigned base = (r[3] & 4) ? 22050 : 11025;
                unsigned step = (unsigned)((uint64_t)base * r[4] / 44100);
                int eight = r[3] & 1;
                if (!m->read_sample || !m->read_sample(m->memory_ctx, m->cursor[v], &sample)) {
                    m->active &= ~bit; continue;
                }
                if (eight) sample = (int16_t)(((sample & 0xff) - 128) << 8);
                left += (int32_t)sample * (int32_t)(r[8] & 255) / 255;
                right += (int32_t)sample * (int32_t)((r[8] >> 8) & 255) / 255;
                {
                    unsigned phase = m->phase[v] + step;
                    m->phase[v] = (uint16_t)(phase & 4095);
                    for (; phase >= 4096; phase -= 4096) {
                        if (m->cursor[v] + (eight ? 1u : 2u) > r[1]) {
                            /* One-shot voices stop on their last sample, the
                             * cursor left there: vtech-lib waits for +0x28 to
                             * reach the end before starting the next pass */
                            if (r[3] & 0x10) m->cursor[v] = r[2];
                            else { m->cursor[v] = r[1]; m->active &= ~bit; m->dry_voices |= bit; m->dry_ends++; break; }
                        } else m->cursor[v] += eight ? 1 : 2;
                    }
                }
                continue;
            }
            if(ima) {
                if(m->ima_pos[v]==64 && !ima_block(m,v)) {m->active&=~bit;continue;}
                sample=m->ima_pcm[v][m->ima_pos[v]];
            } else if (!m->read_sample || !m->read_sample(m->memory_ctx, m->cursor[v], &sample)) {
                m->active &= ~bit; continue;
            }
            if (music) {
                unsigned phase=m->phase[v]+r[4];
                /* Firmware packs independent eight-bit channel gains here.
                 * Linear amplitude is an approximation pending a capture. */
                left += (int32_t)sample*(int32_t)(r[8]&255)/255;
                right += (int32_t)sample*(int32_t)((r[8]>>8)&255)/255;
                m->phase[v]=(uint16_t)(phase&4095);
                if (phase<4096) continue;
            } else {
                if (r[8] & 0xff) left += sample;
                if (r[8] & 0xff00) right += sample;
            }
            if (r[3] == 0x1e4 || (ima && !music)) {
                /* BIOS1003A1F8 maps ratebits4 to22050Hz,8 to11025Hz.
                 * Soundfont mode2 instead uses the pitch accumulator above. */
                unsigned divisor=r[3]==0x12?1:(r[3]==0x1ea || r[3]==0x1fa)?4:2;
                m->half_phase[v]=(uint8_t)((m->half_phase[v]+1)%divisor);
                if (m->half_phase[v]) continue;
            }
            if(ima) {
                m->ima_pos[v]++;
                if(!(m->ima_pos[v]&1))m->cursor[v]++;
                if(m->ima_pos[v]==64 && m->cursor[v]>r[1]) {
                    if(r[3]&0x10) m->cursor[v]=r[2];
                    else m->active&=~bit;
                }
                continue;
            }
            if (m->cursor[v] >= r[1]) {
                if (r[3] == 0x10) m->cursor[v] = r[2];
                else m->active &= ~bit;
            }
            else m->cursor[v] += 2;
        }
        if (m->dry_voices) m->dry_samples++;
        /* Linear gain before final saturation is an approximation; exact
         * hardware rounding and mixer headroom still require a capture.
         * Use64-bit products:64 full-scale voices exceed32-bit here. */
        left = (int32_t)((int64_t)left * m->master_level[0] / 0x7fff);
        right = (int32_t)((int64_t)right * m->master_level[1] / 0x7fff);
        stereo[2 * i] = clip(left); stereo[2 * i + 1] = clip(right);
    }
}
