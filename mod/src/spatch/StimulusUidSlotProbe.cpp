#include "StimulusUidSlotProbe.h"

#include <Windows.h>

#include <atomic>

namespace spatch::stimulus {
namespace {

std::atomic<std::uint32_t> g_reads{0};
std::atomic<std::uint32_t> g_control_ok{0};
std::atomic<std::uint32_t> g_emitter_resolved{0};
std::atomic<std::uint32_t> g_unaddressable{0};

// The receiver's UID is a known fact (SDK ai/stimulusreceivercomponent.hh:29,
// confirmed live by the component census). It is the control value, spelled here
// rather than imported so this probe cannot be satisfied by a bad resolver that
// also feeds the thing it is checked against.
constexpr std::uint32_t kKnownReceiverUid = 0x54000001u;

// Resolves an RVA to an address, or 0 if it is not inside the image. Supplied by
// the host (Hooks.cpp) so this translation unit does not own a second, divergent
// notion of the image base.
std::uintptr_t (*g_resolve)(std::uintptr_t) = nullptr;

std::uint32_t ReadDword(std::uintptr_t address) noexcept {
    if (address == 0) {
        return kUidSlotUninitialised;
    }
    // The slots live in the module image, which is mapped and writable for the
    // process lifetime, so this cannot fault in practice. SEH anyway: a probe
    // that crashes the game is worse than one that returns nothing, and the
    // unaddressable counter records it rather than hiding it.
    __try {
        return *reinterpret_cast<const std::uint32_t*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kUidSlotUninitialised;
    }
}

}  // namespace

void SetUidSlotResolver(std::uintptr_t (*resolve)(std::uintptr_t)) noexcept {
    g_resolve = resolve;
}

UidSlotReading ReadUidSlots() noexcept {
    UidSlotReading reading;
    g_reads.fetch_add(1, std::memory_order_relaxed);
    if (g_resolve == nullptr) {
        g_unaddressable.fetch_add(1, std::memory_order_relaxed);
        return reading;
    }

    const std::uintptr_t receiver_address = g_resolve(kReceiverUidSlotRva);
    const std::uintptr_t emitter_address = g_resolve(kEmitterUidSlotRva);
    if (receiver_address == 0 || emitter_address == 0) {
        // ResolveAddress already bounds-checks against the image, so a zero here
        // means the RVAs do not belong to this build. Report it as its own
        // verdict rather than as a failed control: "wrong build" and "slot not
        // filled yet" call for different follow-ups.
        g_unaddressable.fetch_add(1, std::memory_order_relaxed);
        return reading;
    }
    // Deliberately NOT latched. Component registration fills these slots during
    // startup, so a read taken before that point sees 0xFFFFFFFF and reports a
    // false INVALID for the rest of the session -- the same "a failure latched
    // as final" shape that invalidated earlier work. Every call re-reads; the
    // caller decides when to stop. A correct probe that runs early must recover
    // on a later tick, not stay broken.
    reading.slots_addressable = true;
    reading.receiver_uid = ReadDword(receiver_address);
    reading.emitter_uid = ReadDword(emitter_address);
    reading.control_ok = (reading.receiver_uid == kKnownReceiverUid);
    if (reading.control_ok) {
        g_control_ok.fetch_add(1, std::memory_order_relaxed);
        // Only meaningful behind a passing control. A UID of 0xFFFFFFFF is
        // excluded explicitly so "uninitialised" can never be reported as a
        // recovered emitter.
        if (reading.emitter_uid != 0u && reading.emitter_uid != kUidSlotUninitialised) {
            g_emitter_resolved.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return reading;
}

std::uint32_t UidSlotProbeReads() noexcept {
    return g_reads.load(std::memory_order_relaxed);
}

std::uint32_t UidSlotProbeControlOk() noexcept {
    return g_control_ok.load(std::memory_order_relaxed);
}

std::uint32_t UidSlotProbeEmitterResolved() noexcept {
    return g_emitter_resolved.load(std::memory_order_relaxed);
}

std::uint32_t UidSlotProbeUnaddressable() noexcept {
    return g_unaddressable.load(std::memory_order_relaxed);
}

}  // namespace spatch::stimulus
