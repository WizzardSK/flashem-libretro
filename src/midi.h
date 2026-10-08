#pragma once
#include <stdint.h>

/* Zevio sound register interface. Observed BIOS CDDA streaming and embedded
 * WAV one-shot and observed game PCM soundfont configurations. */
#define MIDI_VOICES 64
#define MIDI_REGISTER_BYTES 0x1300
typedef struct {
    uint32_t regs[MIDI_REGISTER_BYTES / 4];
    uint64_t requested, unsupported_starts;
    uint64_t dry_voices, dry_samples, dry_ends; /* diagnostic: one-shot PCM voices ended, not yet restarted */
    uint64_t active;
    uint16_t master_level[2]; /* current stereo level, exposed at1248 */
    uint32_t cursor[MIDI_VOICES];
    uint8_t half_phase[MIDI_VOICES];
    uint16_t phase[MIDI_VOICES]; /* PCM/IMA4 soundfont fractional sample, /4096 */
    int16_t ima_pcm[MIDI_VOICES][64];
    int32_t ima_predictor[MIDI_VOICES];
    uint8_t ima_index[MIDI_VOICES], ima_pos[MIDI_VOICES], ima_valid[MIDI_VOICES];
    void *memory_ctx;
    int (*read_sample)(void *ctx, uint32_t addr, int16_t *sample);
    void *trace_ctx;
    void (*trace)(void *ctx, unsigned voice, const uint32_t *regs,
                  uint32_t pc, int key_on);
} Midi;

void midi_reset(Midi *m);
/* Offsets relative to B0000000. PC is the actual instruction address. */
uint32_t midi_read(const Midi *m, uint32_t offset);
void midi_write(Midi *m, uint32_t offset, uint32_t value, uint32_t pc);
void midi_set_trace(Midi *m, void *ctx,
                    void (*trace)(void *, unsigned, const uint32_t *, uint32_t, int));
void midi_set_memory(Midi *m, void *ctx,
                     int (*read_sample)(void *, uint32_t, int16_t *));
/* Produce frames at 44100 Hz; stereo interleaved. Invalid memory ends a voice.
 * Hardware callers must run this on emulated time, including idle skips. */
void midi_render(Midi *m, int16_t *stereo, unsigned frames);
