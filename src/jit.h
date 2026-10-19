#pragma once
/* The ARM9's JIT: dynarmic (external/dynarmic, built with JIT=1) runs the
 * guest's code; the interpreter (arm9.c) remains for what dynarmic does not
 * model and for builds without it. */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the core was built with dynarmic */
int jit_available(void);
/* M0: makes a dynarmic A32 instance and drops it again; a description of
 * the result for the log */
const char *jit_probe(void);

#ifdef __cplusplus
}
#endif
