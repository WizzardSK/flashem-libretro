#pragma once
/* The ARM9's JIT: dynarmic (external/dynarmic, built with JIT=1) runs the
 * guest's code; the interpreter (arm9.c) remains for what dynarmic does not
 * model and for builds without it. */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

struct ARM9;

/* 1 when the core was built with dynarmic */
int jit_available(void);
/* What the JIT is, for the log */
const char *jit_probe(void);

/* Sets the JIT up for the CPU (one at a time); 1 if it runs the code now */
int  jit_init(struct ARM9 *cpu);
void jit_shutdown(void);
int  jit_active(void);
/* Runs the CPU until its cycle count reaches stop (or a little past) */
void jit_run(struct ARM9 *cpu, uint64_t stop);
/* Memory written behind the CPU's back (DMA, program loading): code
 * translated from it is dropped */
void jit_note_write(uint32_t addr, size_t len);
/* With VFLASH_JITSTATS, prints and resets the JIT's counters */
void jit_stats(void);

#ifdef __cplusplus
}
#endif
