#include "StimulusEmitterProbe.h"

#include <atomic>

#include <Windows.h>

namespace spatch::stimulus {
namespace {

std::atomic<std::uint32_t> g_probe_count{0};
std::atomic<std::uint32_t> g_control_resolved{0};
std::atomic<std::uint32_t> g_census_count{0};
std::atomic<std::uint32_t> g_sibling_count{0};
std::atomic<std::uint32_t> g_census_saw_receiver{0};
std::atomic<std::uint32_t> g_first_sibling_uid{0};
std::atomic<std::uintptr_t> g_first_sibling_vftable{0};

}  // namespace

void RecordEmitterControl(bool resolved) noexcept {
    // Count the ATTEMPT here, and it doubles as the budget gate. An earlier
    // version counted only inside the (now-deleted) resolver-recording call,
    // which is reached only when the control succeeds -- so a control failing on
    // every call left the counter at 0, the budget never exhausted, and the
    // SEH-guarded work ran on every stimulus forever. Worst case paying most.
    g_probe_count.fetch_add(1, std::memory_order_relaxed);
    if (resolved) {
        g_control_resolved.fetch_add(1, std::memory_order_relaxed);
    }
}

// Component array layout, already proven by the push-voice-line component scan
// in Hooks.cpp: [sim+0x60] = count, [sim+0x68] = array of 16-byte {ptr, uid}.
constexpr std::uintptr_t kComponentCountOffset = 0x60;
constexpr std::uintptr_t kComponentArrayOffset = 0x68;
constexpr std::uintptr_t kComponentEntryStride = 0x10;
constexpr std::uintptr_t kComponentUidInEntry = 0x08;
constexpr std::uint32_t kMaxComponentScan = 512;

void CensusStimulusComponents(std::uintptr_t sim_object,
                              const char* side) noexcept {
    (void)side;
    if (sim_object == 0) {
        return;
    }
    g_census_count.fetch_add(1, std::memory_order_relaxed);

    std::uint32_t count = 0;
    std::uintptr_t array = 0;
    __try {
        count = *reinterpret_cast<const std::uint32_t*>(
            sim_object + kComponentCountOffset);
        array = *reinterpret_cast<const std::uintptr_t*>(
            sim_object + kComponentArrayOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (array == 0 || count == 0 || count > kMaxComponentScan) {
        return;
    }
    // The array walk is its OWN control: it must find the KNOWN receiver UID
    // among the entries, otherwise the walk itself is unreliable and a zero
    // sibling count says nothing. Counting "saw the receiver" proves the same
    // loop that produced the sibling count actually traversed real entries.
    bool saw_receiver = false;
    std::uint32_t siblings = 0;
    // Capture the FIRST sibling's UID and vtable. A count alone says a component
    // exists but not which, and identifying it is the whole point -- it is the
    // concrete target for the next step. Recording it here means ONE session
    // answers "does an emitter exist and what is it", instead of a positive
    // result needing another build/deploy cycle to find out.
    std::uint32_t first_sibling_uid = 0;
    std::uintptr_t first_sibling_vftable = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t uid = 0;
        std::uintptr_t component = 0;
        __try {
            component = *reinterpret_cast<const std::uintptr_t*>(
                array + index * kComponentEntryStride);
            uid = *reinterpret_cast<const std::uint32_t*>(
                array + index * kComponentEntryStride + kComponentUidInEntry);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }
        if (uid == kStimulusReceiverTypeUid) {
            saw_receiver = true;
            continue;
        }
        if ((uid & kComponentFamilyMask) ==
            (kStimulusReceiverTypeUid & kComponentFamilyMask)) {
            if (siblings == 0) {
                first_sibling_uid = uid;
                __try {
                    first_sibling_vftable = *reinterpret_cast<std::uintptr_t*>(
                        component);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    first_sibling_vftable = 0;
                }
            }
            ++siblings;
        }
    }
    // Only a walk that PROVED itself may contribute. A walk that never saw the
    // receiver has not shown it was reading the right array, so its siblings
    // are meaningless -- and adding them anyway produces a confidently-wrong
    // number with nothing on the log to say so. This is the same
    // uninterpretable-negative shape the candidate loop had.
    if (!saw_receiver) {
        return;
    }
    g_census_saw_receiver.fetch_add(1, std::memory_order_relaxed);
    g_sibling_count.fetch_add(siblings, std::memory_order_relaxed);
    // Monotonic first-wins, so a later walk cannot overwrite the first real hit
    // and two different objects cannot race into an ambiguous answer.
    if (first_sibling_uid != 0) {
        std::uint32_t expected = 0;
        g_first_sibling_uid.compare_exchange_strong(
            expected, first_sibling_uid, std::memory_order_relaxed);
        g_first_sibling_vftable.store(first_sibling_vftable,
                                      std::memory_order_relaxed);
    }
}

std::uint32_t EmitterProbeCount() noexcept {
    return g_probe_count.load(std::memory_order_relaxed);
}


std::uint32_t EmitterControlResolved() noexcept {
    return g_control_resolved.load(std::memory_order_relaxed);
}

std::uint32_t ComponentCensusCount() noexcept {
    return g_census_count.load(std::memory_order_relaxed);
}

std::uint32_t SiblingComponentCount() noexcept {
    return g_sibling_count.load(std::memory_order_relaxed);
}

std::uint32_t CensusSawReceiver() noexcept {
    return g_census_saw_receiver.load(std::memory_order_relaxed);
}

std::uint32_t FirstSiblingUid() noexcept {
    return g_first_sibling_uid.load(std::memory_order_relaxed);
}

std::uintptr_t FirstSiblingVftable() noexcept {
    return g_first_sibling_vftable.load(std::memory_order_relaxed);
}

}  // namespace spatch::stimulus
