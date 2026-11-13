#include "StimulusTelemetry.h"

#include <atomic>
#include <cstdint>

namespace spatch::stimulus {
namespace {

// Per-type hit counters. Lock-free relaxed: this is written on a per-ped,
// per-stimulus path and read only when the census is emitted, so a contended
// counter would cost more than the telemetry is worth.
std::atomic<std::uint32_t> g_hits[kStimulusTypeCount];
std::atomic<std::uint64_t> g_total{0};
std::atomic<std::uint32_t> g_out_of_range{0};

// Opt-in accumulator. Monotonic OR across EVERY component observed, so the
// verdict cannot be flipped by one ped that happened not to opt in. A
// per-ped or last-ped reading would repeat this campaign's single-sample
// error: ped A opts in, ped B (last sampled) does not, and the report says
// "no ped opts in" -- sending the next session down the wrong branch on data
// that was already collected.
std::atomic<std::uint32_t> g_ever_opted_in[kOptInWords];
// SAMPLE count (one per call), not distinct components. Unthrottled on purpose:
// a per-component throttle would miss a ped that opts in after its first
// sample, which is the false negative this probe exists to prevent.
std::atomic<std::uint32_t> g_optin_samples{0};
// Components rejected by the m_TypeUID guard. Non-zero here while samples is
// zero means the guard never matched and the opt-in reading is INVALID -- not
// evidence of anything. Reported so a wrong offset can never masquerade as
// "no ped opted in".
std::atomic<std::uint32_t> g_optin_guard_rejections{0};

// The m_TypeUID actually found at the candidate offset, on the first rejection.
// 0 means "nothing recorded yet" and doubles as the recorded flag: a separate
// bool published before the value would let the reporter pair recorded=true
// with value 0, and an observed tag of 0 is a third wrong path. Duplicate
// stores are harmless on this cold path.
std::atomic<std::uint32_t> g_observed_type_uid{0};

// m_StimulusRegistered is a qStaticBitField<116> at StimulusReceiverComponent
// + 0x60 (verified live: type_matches=1 over all 116 records, and the member
// offsets sum to the SDK_ASSERT_SIZEOF of 0x428 with no slack).
constexpr std::uintptr_t kOptInOffset = 0x60;

// Guard the +0x60 read on the component's own type tag. The detour is installed
// on StimulusReceiverComponent::HandleStimulus, so `this` should always be one;
// but this is a raw dereference on a per-ped hot path, and if that assumption
// ever stops holding an unchecked read is a crash in-game and a wasted session.
//
// When the tag does not match, record what was actually there. A guard that only
// says "no" cannot be debugged; one that says "I expected X and saw Y" can.
bool IsStimulusReceiver(const void* component) noexcept {
    if (component == nullptr) {
        return false;
    }
    std::uint32_t type_uid = 0;
    std::memcpy(&type_uid,
                reinterpret_cast<const std::uint8_t*>(component) +
                    kSimComponentTypeUidOffset,
                sizeof(type_uid));
    // Record what was actually there. Race-free without a flag: a losing writer
    // simply overwrites with an equally valid observation.
    if (type_uid != kStimulusReceiverTypeUid &&
        g_observed_type_uid.load(std::memory_order_relaxed) == 0) {
        g_observed_type_uid.store(type_uid, std::memory_order_relaxed);
    }
    return type_uid == kStimulusReceiverTypeUid;
}

}  // namespace

bool OptedIn(const void* component, int type) noexcept {
    if (type < 0 || type >= kStimulusTypeCount || !IsStimulusReceiver(component)) {
        return false;
    }
    const auto* bits = reinterpret_cast<const std::uint8_t*>(component) +
                       kOptInOffset;
    const std::uint32_t word = *reinterpret_cast<const std::uint32_t*>(
        bits + static_cast<std::size_t>(type >> 5) * sizeof(std::uint32_t));
    return (word & (1u << (type & 31))) != 0;
}

void SampleOptIn(const void* component) noexcept {
    if (!IsStimulusReceiver(component)) {
        g_optin_guard_rejections.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_optin_samples.fetch_add(1, std::memory_order_relaxed);
    const auto* bits = reinterpret_cast<const std::uint8_t*>(component) +
                       kOptInOffset;
    for (int index = 0; index < kOptInWords; ++index) {
        std::uint32_t word = 0;
        std::memcpy(&word,
                    bits + static_cast<std::size_t>(index) * sizeof(word),
                    sizeof(word));
        // fetch_or is monotonic: once a bit is seen set it never clears.
        g_ever_opted_in[index].fetch_or(word, std::memory_order_relaxed);
    }
}

bool EverOptedIn(int type) noexcept {
    if (type < 0 || type >= kStimulusTypeCount) {
        return false;
    }
    const std::uint32_t word =
        g_ever_opted_in[type >> 5].load(std::memory_order_relaxed);
    return (word & (1u << (type & 31))) != 0;
}

std::uint32_t EverOptedInWeaponFearCount() noexcept {
    std::uint32_t count = 0;
    for (const int type : kWeaponFearTypes) {
        if (EverOptedIn(type)) {
            ++count;
        }
    }
    return count;
}

std::uint32_t OptInSamples() noexcept {
    return g_optin_samples.load(std::memory_order_relaxed);
}

std::uint32_t OptInGuardRejections() noexcept {
    return g_optin_guard_rejections.load(std::memory_order_relaxed);
}

std::uint32_t ObservedTypeUid() noexcept {
    return g_observed_type_uid.load(std::memory_order_relaxed);
}

bool ObservedTypeUidRecorded() noexcept {
    // No separate flag: the value itself is the flag. A flag published before
    // the store would let the reporter pair recorded=true with value 0, and an
    // observed tag of 0 is a third wrong path. Duplicate stores are harmless.
    return ObservedTypeUid() != 0;
}

const char* NameOf(OptInVerdict verdict) noexcept {
    switch (verdict) {
    case OptInVerdict::kInvalidGuardNoMatch:
        return "INVALID_guard_no_match";
    case OptInVerdict::kReactionGap:
        return "REACTION_gap";
    case OptInVerdict::kEmissionGap:
        return "EMISSION_gap";
    case OptInVerdict::kOptInGap:
        return "OPTIN_gap";
    }
    return "INVALID_guard_no_match";
}

void Record(int type) noexcept {
    if (type < 0 || type >= kStimulusTypeCount) {
        // Never indexed, only counted: a type outside the enum means the
        // detour's read of the stimulus is wrong, and the count is the evidence.
        g_out_of_range.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_hits[type].fetch_add(1, std::memory_order_relaxed);
    g_total.fetch_add(1, std::memory_order_relaxed);
}

std::uint32_t Hits(int type) noexcept {
    if (type < 0 || type >= kStimulusTypeCount) {
        return 0;
    }
    return g_hits[type].load(std::memory_order_relaxed);
}

std::uint64_t Total() noexcept {
    return g_total.load(std::memory_order_relaxed);
}

std::uint32_t OutOfRange() noexcept {
    return g_out_of_range.load(std::memory_order_relaxed);
}

std::uint32_t DistinctTypesSeen() noexcept {
    std::uint32_t seen = 0;
    for (int i = 0; i < kStimulusTypeCount; ++i) {
        if (g_hits[i].load(std::memory_order_relaxed) != 0) {
            ++seen;
        }
    }
    return seen;
}

std::uint64_t WeaponFearHits() noexcept {
    std::uint64_t total = 0;
    for (const int type : kWeaponFearTypes) {
        total += g_hits[type].load(std::memory_order_relaxed);
    }
    return total;
}

}  // namespace spatch::stimulus