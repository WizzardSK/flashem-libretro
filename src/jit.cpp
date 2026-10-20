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
 * a 64-byte line the JIT read code from is marked, and a write to it - by the CPU,
 * or DMA and program loading through jit_note_write (by physical address,
 * the kernel mapping RAM one to one) - drops what was translated from it.
 * A change of the MMU's table, domains, process ID or control register
 * (by MCR or a reset) drops everything. */
#include "jit.h"

extern "C" {
#include "arm9.h"
}

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <ctime>
#include <thread>
#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

namespace {

constexpr auto HALT_FALLBACK = Dynarmic::HaltReason::UserDefined1;
constexpr auto HALT_IRQ = Dynarmic::HaltReason::UserDefined2;

struct Bridge final : Dynarmic::A32::UserCallbacks {
    ARM9 *cpu = nullptr;
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    std::uint64_t budget = 0, used = 0;
    // An instruction for the interpreter, the JIT halted before it
    bool fallback = false, svc = false, running = false;
    // the MMU state the code cache was translated under
    CP15 mmu{};
    std::uint32_t fallback_pc = 0;
    std::size_t fallback_count = 0;
    // 64-byte lines code was translated from: games write data right next
    // to their code, and a page at a time retranslated a loop on every store
    static constexpr int LINE = 6;
    std::vector<bool> code_line = std::vector<bool>(1u << (32 - LINE));
    // VFLASH_JITSTATS: runs, instructions given back, code words read
    // (translation), lines invalidated, full clears
    std::uint64_t n_runs = 0, n_back = 0, n_code = 0, n_inval = 0, n_clear = 0;
    std::uint64_t n_mem = 0, n_ticks = 0, ns_run = 0, ns_back = 0;

    void *ctx() const { return cpu->mem_ctx; }

    void mark(std::uint32_t va) { code_line[va >> LINE] = true; }
    void wrote(std::uint32_t va, std::size_t len) {
        for (std::uint32_t l = va >> LINE, e = (std::uint32_t)((va + len - 1) >> LINE); l <= e; l++)
            if (code_line[l]) {
                code_line[l] = false;
                n_inval++;
                jit->InvalidateCacheRange(l << LINE, 1u << LINE);
            }
    }
    void forget_code() { code_line.assign(code_line.size(), false); }

    // The CPU's data accesses, as the interpreter makes them: ARMv5 ignores
    // the low address bits of halfword and word accesses (dynarmic passes
    // them on), and an unaligned word load reads the aligned word, rotated.
    // With VFLASH_JITCHECK they are also recorded for the lockstep check.
    struct Access { std::uint32_t va; std::uint32_t v; std::uint8_t size; bool write; };
    bool rec = false;
    std::vector<Access> accesses;

    std::uint32_t rd(std::uint32_t va, std::uint8_t size) {
        n_mem++;
        std::uint32_t v = size == 4 ? cpu->mem_read32(ctx(), va) : size == 2 ? cpu->mem_read16(ctx(), va) : cpu->mem_read8(ctx(), va);
        if (rec) accesses.push_back({va, v, size, false});
        return v;
    }
    void wr(std::uint32_t va, std::uint32_t v, std::uint8_t size) {
        n_mem++;
        if (rec) accesses.push_back({va, v, size, true});
        if (size == 4) cpu->mem_write32(ctx(), va, v);
        else if (size == 2) cpu->mem_write16(ctx(), va, (std::uint16_t)v);
        else cpu->mem_write8(ctx(), va, (std::uint8_t)v);
        wrote(va, size);
    }

    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t va) override {
        mark(va);
        n_code++;
        return cpu->mem_read32(ctx(), va & ~3u);
    }
    std::uint8_t MemoryRead8(std::uint32_t va) override { return (std::uint8_t)rd(va, 1); }
    std::uint16_t MemoryRead16(std::uint32_t va) override { return (std::uint16_t)rd(va & ~1u, 2); }
    std::uint32_t MemoryRead32(std::uint32_t va) override {
        std::uint32_t v = rd(va & ~3u, 4);
        unsigned rot = (va & 3) * 8;
        return rot ? (v >> rot) | (v << (32 - rot)) : v;
    }
    std::uint64_t MemoryRead64(std::uint32_t va) override {
        std::uint64_t lo = rd(va & ~3u, 4);
        return lo | (std::uint64_t)rd((va & ~3u) + 4, 4) << 32;
    }
    void MemoryWrite8(std::uint32_t va, std::uint8_t v) override { wr(va, v, 1); }
    void MemoryWrite16(std::uint32_t va, std::uint16_t v) override { wr(va & ~1u, v, 2); }
    void MemoryWrite32(std::uint32_t va, std::uint32_t v) override { wr(va & ~3u, v, 4); }
    void MemoryWrite64(std::uint32_t va, std::uint64_t v) override {
        wr(va & ~3u, (std::uint32_t)v, 4);
        wr((va & ~3u) + 4, (std::uint32_t)(v >> 32), 4);
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
    // a device write that raises an interrupt stops the code right there
    config.check_halt_on_memory_access = true;
    // CP15 is the interpreter's: tools/dynarmic.patch gives every
    // coprocessor instruction back
    // VFLASH_JITOPT=<hex> (debugging): dynarmic's optimisation flags
    if (getenv("VFLASH_JITOPT"))
        config.optimizations = static_cast<Dynarmic::OptimizationFlag>(strtoul(getenv("VFLASH_JITOPT"), nullptr, 16));
    b->jit = std::make_unique<Dynarmic::A32::Jit>(config);
    g_bridge = b;
    // VFLASH_JITSTATS also line-buffers the log and has a thread report
    // every 5 s where the CPU is, which shows a hang inside a frame
    if (getenv("VFLASH_JITSTATS")) {
        setvbuf(stdout, nullptr, _IOLBF, 0);
        std::thread([cpu] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                Bridge *b = g_bridge;
                if (!b) return;
                printf("[JIT] watchdog: pc %08X cpsr %08X cycles %llu runs %llu ticks %llu\n", cpu->r[15], cpu->cpsr,
                       (unsigned long long)cpu->cycles, (unsigned long long)b->n_runs, (unsigned long long)b->n_ticks);
            }
        }).detach();
    }
    return 1;
}

extern "C" void jit_shutdown(void) {
    delete g_bridge;
    g_bridge = nullptr;
}

extern "C" int jit_active(void) { return g_bridge != nullptr; }

namespace {

// One run of dynarmic - or with step, one instruction - and the
// instructions it gave back; true if it gave any back
bool run_once(Bridge &b, ARM9 *cpu, std::uint64_t stop, bool step) {
    // An MMU change - by MCR, or a reset through a device - leaves the code
    // cache translated by stale mappings
    if (cpu->cp15.ttb != b.mmu.ttb || cpu->cp15.control != b.mmu.control
        || cpu->cp15.domain != b.mmu.domain || cpu->cp15.pid != b.mmu.pid) {
        b.mmu = cpu->cp15;
        b.n_clear++;
        b.jit->ClearCache();
        b.forget_code();
    }
    b.budget = stop > cpu->cycles ? stop - cpu->cycles : 1;
    // VFLASH_JITBUDGET=<n> (debugging): runs of at most n cycles
    static const std::uint64_t cap = getenv("VFLASH_JITBUDGET") ? strtoull(getenv("VFLASH_JITBUDGET"), nullptr, 0) : 0;
    if (cap && b.budget > cap) b.budget = cap;
    b.used = 0;
    b.fallback = b.svc = false;
    load_state(b);
    b.jit->ClearHalt(HALT_FALLBACK | HALT_IRQ);
    b.running = true;
    b.n_runs++;
    static const bool timed = getenv("VFLASH_JITSTATS") != nullptr;
    timespec t0, t1;
    if (timed) clock_gettime(CLOCK_MONOTONIC, &t0);
    const auto why = step ? b.jit->Step() : b.jit->Run();
    b.running = false;
    if (timed) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        b.ns_run += (t1.tv_sec - t0.tv_sec) * 1000000000ull + t1.tv_nsec - t0.tv_nsec;
    }
    b.n_ticks += b.used;
    if (!b.used && !b.fallback && !Dynarmic::Has(why, HALT_IRQ)) {
        // dynarmic stopped without running anything: let the interpreter
        // take the instruction, so that time moves on
        static int told;
        if (told++ < 10)
            printf("[JIT] no progress at %08X (halt %08X), interpreted\n", b.jit->Regs()[15], (unsigned)why);
        store_state(b);
        arm9_step(cpu);
        return true;
    }
    store_state(b);
    cpu->cycles += b.used;
    if (!b.fallback) return false;
    b.n_back += b.fallback_count;
    // the instructions dynarmic gave back, by the interpreter
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
    return true;
}

/* VFLASH_JITCHECK=<n> (debugging): dynarmic runs one instruction at a time,
 * each also run by the interpreter on a copy of the state before it, with
 * its loads answered from what dynarmic read; the first n differences in
 * registers, flags or memory accesses are printed. */
struct Replay {
    Bridge *b;
    ARM9 *real;
    std::size_t next;
    std::uint32_t fetch_pc;
    bool fetched, bad;
};
Replay g_replay;

std::uint32_t replay_rd(void *ctx, std::uint32_t va, std::uint8_t size) {
    Replay &r = *(Replay *)ctx;
    if (!r.fetched && va == (r.fetch_pc & (size == 4 ? ~3u : ~1u))) {   // the instruction fetch
        r.fetched = true;
        return size == 4 ? r.real->mem_read32(r.real->mem_ctx, va) : r.real->mem_read16(r.real->mem_ctx, va);
    }
    auto &acc = r.b->accesses;
    if (r.next >= acc.size() || acc[r.next].write || acc[r.next].va != va || acc[r.next].size != size) {
        if (!r.bad) printf("[JITCHECK]   interpreter reads %u at %08X, dynarmic's access #%zu %s\n", size, va, r.next,
                           r.next < acc.size() ? "differs" : "missing");
        r.bad = true;
        return r.next < acc.size() ? acc[r.next++].v : 0;
    }
    return acc[r.next++].v;
}
void replay_wr(void *ctx, std::uint32_t va, std::uint32_t v, std::uint8_t size) {
    Replay &r = *(Replay *)ctx;
    auto &acc = r.b->accesses;
    if (r.next >= acc.size() || !acc[r.next].write || acc[r.next].va != va || acc[r.next].size != size || acc[r.next].v != v) {
        if (!r.bad) printf("[JITCHECK]   interpreter writes %u at %08X = %08X, dynarmic's access #%zu %s\n", size, va, v, r.next,
                           r.next < acc.size() ? "differs" : "missing");
        r.bad = true;
    }
    r.next++;
}
std::uint32_t replay_r32(void *c, std::uint32_t a) { return replay_rd(c, a, 4); }
std::uint16_t replay_r16(void *c, std::uint32_t a) { return (std::uint16_t)replay_rd(c, a, 2); }
std::uint8_t replay_r8(void *c, std::uint32_t a) { return (std::uint8_t)replay_rd(c, a, 1); }
void replay_w32(void *c, std::uint32_t a, std::uint32_t v) { replay_wr(c, a, v, 4); }
void replay_w16(void *c, std::uint32_t a, std::uint16_t v) { replay_wr(c, a, v, 2); }
void replay_w8(void *c, std::uint32_t a, std::uint8_t v) { replay_wr(c, a, v, 1); }

void check_step(Bridge &b, ARM9 *cpu, std::uint64_t stop, long &left) {
    ARM9 pre = *cpu;
    b.rec = true;
    b.accesses.clear();
    bool gave_back = run_once(b, cpu, stop, true);
    b.rec = false;
    if (gave_back) return;
    ARM9 sh = pre;
    Replay &r = g_replay;
    r = Replay{&b, cpu, 0, pre.r[15], false, false};
    sh.mem_ctx = &r;
    sh.mem_read32 = replay_r32; sh.mem_read16 = replay_r16; sh.mem_read8 = replay_r8;
    sh.mem_write32 = replay_w32; sh.mem_write16 = replay_w16; sh.mem_write8 = replay_w8;
    sh.mem_page = nullptr; sh.fetch_ptr = nullptr;
    std::uint32_t insn = pre.thumb ? cpu->mem_read16(cpu->mem_ctx, pre.r[15] & ~1u) : cpu->mem_read32(cpu->mem_ctx, pre.r[15] & ~3u);
    arm9_step(&sh);
    // Thumb BL/BLX is two instructions to the interpreter, one to dynarmic
    if (pre.thumb && (insn & 0xF800) == 0xF000 && sh.r[15] == pre.r[15] + 2) {
        r.fetch_pc = sh.r[15];
        r.fetched = false;
        arm9_step(&sh);
    }
    bool diff = r.bad || r.next != b.accesses.size() || sh.cpsr != cpu->cpsr
                || std::memcmp(sh.r, cpu->r, sizeof sh.r) || std::memcmp(sh.vfp_s, cpu->vfp_s, sizeof sh.vfp_s);
    if (!diff) return;
    left--;
    printf("[JITCHECK] %08X %0*X cpsr %08X cyc %llu: %zu accesses by dynarmic, interpreter used %zu\n",
           pre.r[15], pre.thumb ? 4 : 8, insn, pre.cpsr, (unsigned long long)pre.cycles, b.accesses.size(), r.next);
    for (int i = 0; i < 16; i++)
        if (sh.r[i] != cpu->r[i] || pre.r[i] != cpu->r[i])
            printf("[JITCHECK]   r%-2d %08X -> dynarmic %08X interpreter %08X%s\n", i, pre.r[i], cpu->r[i], sh.r[i],
                   sh.r[i] != cpu->r[i] ? "  <--" : "");
    if (sh.cpsr != cpu->cpsr) printf("[JITCHECK]   cpsr -> dynarmic %08X interpreter %08X\n", cpu->cpsr, sh.cpsr);
    for (std::size_t i = 0; i < b.accesses.size(); i++)
        printf("[JITCHECK]   #%zu %s%u %08X %08X\n", i, b.accesses[i].write ? "w" : "r", b.accesses[i].size,
               b.accesses[i].va, b.accesses[i].v);
    fflush(stdout);
}

}  // namespace

extern "C" void jit_run(ARM9 *cpu, std::uint64_t stop) {
    Bridge &b = *g_bridge;
    if (cpu->cycles >= stop) return;
    // VFLASH_JITFLUSH (debugging): 1 drops all translated code before every
    // run, 2 whenever the TLB was flushed or a page table written
    static const int flush = getenv("VFLASH_JITFLUSH") ? atoi(getenv("VFLASH_JITFLUSH")) : 0;
    static std::uint32_t flush_gen;
    if (flush == 1 || (flush == 2 && cpu->cp15.tlb_gen != flush_gen)) {
        flush_gen = cpu->cp15.tlb_gen;
        b.jit->ClearCache();
        b.forget_code();
    }
    static long check_left = getenv("VFLASH_JITCHECK") ? atol(getenv("VFLASH_JITCHECK")) : 0;
    // VFLASH_JITCHECK_FROM=<cycle>: start checking there
    static const std::uint64_t check_from = getenv("VFLASH_JITCHECK_FROM") ? strtoull(getenv("VFLASH_JITCHECK_FROM"), nullptr, 0) : 0;
    if (check_left > 0 && cpu->cycles >= check_from) {
        while (cpu->cycles < stop && check_left > 0) check_step(b, cpu, stop, check_left);
        return;
    }
    run_once(b, cpu, stop, false);
}

extern "C" void jit_stats(void) {
    static const bool on = getenv("VFLASH_JITSTATS") != nullptr;
    if (!g_bridge || !on) return;
    Bridge &b = *g_bridge;
    printf("[JIT] runs %llu, given back %llu, code words read %llu, lines invalidated %llu, clears %llu, "
           "memory accesses %llu, cycles %llu, %llu ms in Run\n",
           (unsigned long long)b.n_runs, (unsigned long long)b.n_back, (unsigned long long)b.n_code,
           (unsigned long long)b.n_inval, (unsigned long long)b.n_clear, (unsigned long long)b.n_mem,
           (unsigned long long)b.n_ticks, (unsigned long long)(b.ns_run / 1000000));
    b.n_runs = b.n_back = b.n_code = b.n_inval = b.n_clear = b.n_mem = b.n_ticks = b.ns_run = 0;
}

extern "C" void jit_interrupt(void) {
    if (g_bridge && g_bridge->running) g_bridge->jit->HaltExecution(HALT_IRQ);
}

extern "C" void jit_note_write(std::uint32_t pa, std::size_t len) {
    if (g_bridge && len) g_bridge->wrote(pa, len);
}
