/* The ARM9's JIT on dynarmic. M0: the library builds, links and makes an
 * instance; nothing runs on it yet. */
#include "jit.h"

#include <cstdint>
#include <memory>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

namespace {

struct ProbeCallbacks final : Dynarmic::A32::UserCallbacks {
    std::uint8_t MemoryRead8(std::uint32_t) override { return 0; }
    std::uint16_t MemoryRead16(std::uint32_t) override { return 0; }
    std::uint32_t MemoryRead32(std::uint32_t) override { return 0; }
    std::uint64_t MemoryRead64(std::uint32_t) override { return 0; }
    void MemoryWrite8(std::uint32_t, std::uint8_t) override {}
    void MemoryWrite16(std::uint32_t, std::uint16_t) override {}
    void MemoryWrite32(std::uint32_t, std::uint32_t) override {}
    void MemoryWrite64(std::uint32_t, std::uint64_t) override {}
    void InterpreterFallback(std::uint32_t, std::size_t) override {}
    void CallSVC(std::uint32_t) override {}
    void ExceptionRaised(std::uint32_t, Dynarmic::A32::Exception) override {}
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 0; }
};

}  // namespace

extern "C" int jit_available(void) { return 1; }

extern "C" const char *jit_probe(void) {
    ProbeCallbacks callbacks;
    Dynarmic::A32::UserConfig config;
    config.callbacks = &callbacks;
    config.arch_version = Dynarmic::A32::ArchVersion::v5TE;
    auto jit = std::make_unique<Dynarmic::A32::Jit>(config);
    return jit ? "dynarmic A32 (ARMv5TE) ready" : "dynarmic failed";
}
