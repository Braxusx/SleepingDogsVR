#pragma once

// Classification for the Stimulus::Init emitter-capture probe.
//
// MECHANISM. Stimulus::Init (SDK ai/stimulus.hh:25) is
//   void Init(StimulusEmitterComponent* emitter, SimObject* stimulus_target)
// and the receiver lives in RCX on x64, so RDX is the EMITTER at the hook
// entry. The bytes read from the shipped sdhdship.exe at RVA 0x366490 confirm
// that arity directly: `48 8B D9` (mov rbx, rcx) saves arg1, `48 8B 4A 28`
// (mov rcx, [rdx+28h]) dereferences arg2, and `49 8B F0` (mov rsi, r8)
// saves arg3.
//
// WHY NOT A COMPONENT CENSUS. sym-cop-crowd.txt shows the receiver is
// qNode<StimulusReceiverComponent, StimulusReceiverComponent> -- a self-node,
// so it lives on the ped's own SimObject and a component-array walk finds it
// every time. The emitter is qNode<StimulusEmitterComponent,
// StimulusManagerUpdateList>: its SECOND template argument is a different
// class, so emitters register with the CENTRAL StimulusManager update list and
// never with a ped or with the player. No SimObject component-array census can
// reach it, which is why every census attempt here returned zero while the
// receiver control succeeded. Capturing RDX at the call site sidesteps the
// registration question entirely.
//
// PURE BY DESIGN. Every function here takes its inputs as parameters and reads
// no globals, exactly like stimulus::ResolveOptInVerdict. The accumulator
// behind the hook is monotonic, so a resolver that read it could never be
// driven back into an earlier arm once a later one had fired -- that arm would
// ship untested. Taking captures/readable/uid as arguments forces the whole
// evidence into the signature, where it cannot be quietly omitted.

#include <cstdint>

namespace spatch::stimulus {

// Exactly five reportable states. A sixth arm would mean a reading this probe
// cannot support, and the log contract below names only these five.
enum class InitEmitterState {
    // The detour never ran. Says nothing about the emitter either way: it is
    // the "the instrument did not fire" state, not a negative measurement.
    kNotSeen,
    // The detour ran but the emitter was never read: either every captured
    // pointer was null, or every guarded read faulted. The probe measured
    // nothing, so no conclusion about the engine may be drawn from it.
    kUnreadable,
    // A read completed but the vtable did NOT match the known emitter vftable,
    // so the pointer was some other object and its +0x18 is not a uid at all.
    // This is the instrument's own control failing, which is what separates a
    // proven reading from a merely plausible one.
    kInvalid,
    // The read succeeded, PASSED the vtable control, and the engine's own
    // "not assigned yet" sentinel came back. Reported as its OWN state so it
    // can never be mistaken for a uid.
    kUninitialised,
    // The vtable control passed and the uid is not the sentinel. The only arm
    // that reports a uid as a uid, and the only one reachable with the control
    // satisfied.
    kValid,
};

// The engine's unassigned tag. StimulusEmitterComponent inherits SimComponent,
// whose m_TypeUID is at +0x18; before the engine assigns the real tag the slot
// holds this sentinel. It is NOT a uid, so it must never be logged or classified
// as one -- doing so is how a probe manufactures a plausible-looking finding
// out of an uninitialised object.
inline constexpr std::uint32_t kUnassignedTypeUid = 0xFFFFFFFFu;

// SimComponent::m_TypeUID offset within the component. StimulusTelemetry.h
// documents the layout: vptr(0x00) + qList(0x08..0x17) => base size 0x18, then
// m_TypeUID(0x18), m_NameUID(0x1C), m_Flags(0x20), m_pSimObject(0x28).
inline constexpr std::uintptr_t kEmitterTypeUidOffset = 0x18;

// How many emitter reads one session may perform before the probe stops paying.
// Stimulus::Init is an engine entry, so an uncapped read would put a guarded
// dereference on a path the game calls on its own schedule.
inline constexpr std::uint32_t kInitEmitterProbeBudget = 64;

// Classify from the four measured quantities.
//
//   captures      every entry to the detour, including null-argument entries
//   control_ok    reads whose vtable matched the known emitter vftable
//   readable      entries whose guarded emitter read actually completed
//   uid           the first successfully read uid, or 0 when nothing was read
//
// `control_ok` is the load-bearing term and it is checked BEFORE uid. Without
// it, a successful dereference of ANY object -- a Stimulus, a SimObject, a
// freed pointer -- would satisfy the ladder and yield VALID off a non-emitter's
// +0x18, which is precisely how this campaign produced four plausible readings
// that a control then refuted. The uid is only the emitter's uid when the
// vtable already proved the object was one.
//
// Ladder is strictly monotone and never skips: zero captures is NOT_SEEN
// regardless of the others; a positive capture count with no successful read is
// UNREADABLE; a read that fails the vtable control is INVALID; a controlled read
// of the sentinel is UNINITIALISED; only a controlled read of a non-sentinel uid
// is VALID. Neither the sentinel arm nor VALID is reachable without the control.
[[nodiscard]] constexpr InitEmitterState ClassifyInitEmitter(
    std::uint32_t captures,
    std::uint32_t readable,
    std::uint32_t control_ok,
    std::uint32_t uid) noexcept {
    if (captures == 0) {
        return InitEmitterState::kNotSeen;
    }
    if (readable == 0) {
        return InitEmitterState::kUnreadable;
    }
    if (control_ok == 0) {
        return InitEmitterState::kInvalid;
    }
    if (uid == kUnassignedTypeUid) {
        return InitEmitterState::kUninitialised;
    }
    return InitEmitterState::kValid;
}

// The log spelling. Exactly four distinct strings, so a reader can key off the
// state column without parsing the uid.
[[nodiscard]] const char* NameOf(InitEmitterState state) noexcept;

}  // namespace spatch::stimulus