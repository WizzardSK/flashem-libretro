/* The ARM9's JIT on dynarmic (external/dynarmic, built with JIT=1).
 *
 * dynarmic runs the guest's code in user-mode terms: it keeps the mode bits
 * of the CPSR but no banked registers, SPSR or exception entry. Those stay
 * with the interpreter (arm9.c), which holds the whole ARM9 state (ARM9)
 * between runs: jit_run hands dynarmic the current registers, CPSR and VFP
 * state, runs it for a slice of cycles and takes them back. What dynarmic
 * gives back - instructions it does not translate (LDM/STM ^, MSR of the
 * control field via tools/dynarmic.patch, MRS/MSR of the SPSR, SUBS PC,
 * MCR/MRC, undefined ones) and SWI - halts it at that instruction, which the
 * interpreter then executes; interrupts are taken between runs by hw.c.
 *
 * Memory goes through the same callbacks as the interpreter's (hw.c), with
 * ARMv5's word load rotation done here. Code is cached by virtual address;
 * a page the JIT read code from is marked, and a write to it - by the CPU,
 * or DMA and program loading through jit_note_write (by physical address,
 * the kernel mapping RAM one to one) - drops what was translated from it.
 * A change of the MMU's table or control register drops everything. */
#include "jit.h"

extern "C" {
#include "arm9.h"
}

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

namespace {

constexpr auto HALT_FALLBACK = Dynarmic::HaltReason::UserDefined1;

struct Bridge final : Dynarmic::A32::UserCallbacks {
    ARM9 *cpu = nullptr;
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    std::uint64_t budget = 0, used = 0;
    // An instruction for the interpreter, the JIT halted before it
    bool fallback = false, svc = false;
    std::uint32_t fallback_pc = 0;
    std::size_t fallback_count = 0;
    // 4 KB pages code was translated from
    std::vector<std::uint8_t> code_page = std::vector<std::uint8_t>(1u << 20);

    void *ctx() const { return cpu->mem_ctx; }

    void mark(std::uint32_t va) { code_page[va >> 12] = 1; }
    void wrote(std::uint32_t va, std::size_t len) {
        for (std::uint32_t p = va >> 12, e = (std::uint32_t)((va + len - 1) >> 12); p <= e; p++)
            if (code_page[p]) {
                code_page[p] = 0;
                jit->InvalidateCacheRange(p << 12, 4096);
            }
    }

    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t va) override {
        mark(va);
        return cpu->mem_read32(ctx(), va);
    }
    std::uint8_t MemoryRead8(std::uint32_t va) override { return cpu->mem_read8(ctx(), va); }
    std::uint16_t MemoryRead16(std::uint32_t va) override { return cpu->mem_read16(ctx(), va); }
    // ARMv5: an unaligned word load reads the aligned word, rotated
    std::uint32_t MemoryRead32(std::uint32_t va) override {
        std::uint32_t v = cpu->mem_read32(ctx(), va);
        unsigned rot = (va & 3) * 8;
        return rot ? (v >> rot) | (v << (32 - rot)) : v;
    }
    std::uint64_t MemoryRead64(std::uint32_t va) override {
        return cpu->mem_read32(ctx(), va) | (std::uint64_t)cpu->mem_read32(ctx(), va + 4) << 32;
    }
    void MemoryWrite8(std::uint32_t va, std::uint8_t v) override { cpu->mem_write8(ctx(), va, v); wrote(va, 1); }
    void MemoryWrite16(std::uint32_t va, std::uint16_t v) override { cpu->mem_write16(ctx(), va, v); wrote(va, 2); }
    void MemoryWrite32(std::uint32_t va, std::uint32_t v) override { cpu->mem_write32(ctx(), va, v); wrote(va, 4); }
    void MemoryWrite64(std::uint32_t va, std::uint64_t v) override {
        cpu->mem_write32(ctx(), va, (std::uint32_t)v);
        cpu->mem_write32(ctx(), va + 4, (std::uint32_t)(v >> 32));
        wrote(va, 8);
    }

    void give_back(std::uint32_t pc, std::size_t count) {
        fallback = true;
        fallback_pc = pc;
        fallback_count = count;
        jit->HaltExecution(HALT_FALLBACK);
    }
    void InterpreterFallback(std::uint32_t pc, std::size_t n) override { give_back(pc, n); }
    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception) override { give_back(pc, 1); }
    // dynarmic is past the SWI when it calls this: jit_run steps back to it
    // and has the interpreter execute it, entering the exception as the
    // hardware would
    void CallSVC(std::uint32_t) override {
        svc = true;
        give_back(0, 1);
    }

    void AddTicks(std::uint64_t ticks) override { used += ticks; }
    std::uint64_t GetTicksRemaining() override { return used < budget ? budget - used : 0; }
    // the interpreter's own cycle counts, so that time runs as it did
    std::uint64_t GetTicksForCode(bool thumb, std::uint32_t, std::uint32_t insn) override {
        return (std::uint64_t)arm9_insn_cycles(insn, thumb);
    }
};

Bridge *g_bridge;

void load_state(Bridge &b) {
    ARM9 *c = b.cpu;
    auto &regs = b.jit->Regs();
    for (int i = 0; i < 16; i++) regs[i] = c->r[i];
    b.jit->SetCpsr(c->cpsr);
    auto &ext = b.jit->ExtRegs();
    std::memcpy(ext.data(), c->vfp_s, sizeof c->vfp_s);
    b.jit->SetFpscr(c->vfp_fpscr);
}

void store_state(Bridge &b) {
    ARM9 *c = b.cpu;
    const auto &regs = b.jit->Regs();
    for (int i = 0; i < 16; i++) c->r[i] = regs[i];
    c->cpsr = b.jit->Cpsr();
    c->thumb = (c->cpsr & ARM9_FLAG_T) != 0;
    const auto &ext = b.jit->ExtRegs();
    std::memcpy(c->vfp_s, ext.data(), sizeof c->vfp_s);
    c->vfp_fpscr = b.jit->Fpscr();
}

}  // namespace

extern "C" int jit_available(void) { return 1; }

extern "C" const char *jit_probe(void) { return "dynarmic A32 (ARMv5TE)"; }

extern "C" int jit_init(ARM9 *cpu) {
    if (g_bridge) return 1;
    auto *b = new Bridge;
    b->cpu = cpu;
    Dynarmic::A32::UserConfig config;
    config.callbacks = b;
    config.arch_version = Dynarmic::A32::ArchVersion::v5TE;
    // CP15 is the interpreter's: tools/dynarmic.patch gives every
    // coprocessor instruction back
    b->jit = std::make_unique<Dynarmic::A32::Jit>(config);
    g_bridge = b;
    return 1;
}

extern "C" void jit_shutdown(void) {
    delete g_bridge;
    g_bridge = nullptr;
}

extern "C" int jit_active(void) { return g_bridge != nullptr; }

extern "C" void jit_run(ARM9 *cpu, std::uint64_t stop) {
    Bridge &b = *g_bridge;
    if (cpu->cycles >= stop) return;
    b.budget = stop - cpu->cycles;
    b.used = 0;
    b.fallback = b.svc = false;
    load_state(b);
    b.jit->ClearHalt(HALT_FALLBACK);
    b.jit->Run();
    store_state(b);
    cpu->cycles += b.used;
    if (!b.fallback) return;
    // the instructions dynarmic gave back, by the interpreter; an MMU change
    // among them means the code cache is translated by stale mappings
    const CP15 mmu = cpu->cp15;
    cpu->r[15] = b.svc ? cpu->r[15] - (cpu->thumb ? 2 : 4) : b.fallback_pc;
    for (std::size_t i = 0; i < b.fallback_count; i++) {
        // VFLASH_JITLOG=<n>: the first n instructions given back, for debugging
        static long log_left = getenv("VFLASH_JITLOG") ? atol(getenv("VFLASH_JITLOG")) : 0;
        if (log_left > 0) {
            log_left--;
            std::uint32_t at = cpu->r[15], cpsr = cpu->cpsr;
            std::uint32_t insn = cpu->thumb ? cpu->mem_read16(cpu->mem_ctx, at) : cpu->mem_read32(cpu->mem_ctx, at);
            arm9_step(cpu);
            printf("[JIT] %s %08X %08X cpsr %08X -> pc %08X cpsr %08X sp %08X lr %08X cyc %llu\n",
                   b.svc ? "svc" : "int", at, insn, cpsr, cpu->r[15], cpu->cpsr, cpu->r[13], cpu->r[14],
                   (unsigned long long)cpu->cycles);
            continue;
        }
        arm9_step(cpu);
    }
    if (cpu->cp15.ttb != mmu.ttb || cpu->cp15.control != mmu.control
        || cpu->cp15.domain != mmu.domain || cpu->cp15.pid != mmu.pid) {
        b.jit->ClearCache();
        std::fill(b.code_page.begin(), b.code_page.end(), 0);
    }
}

extern "C" void jit_note_write(std::uint32_t pa, std::size_t len) {
    if (g_bridge && len) g_bridge->wrote(pa, len);
}
