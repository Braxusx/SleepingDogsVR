#pragma once

#include <cstdint>

#include "StimulusTelemetry.h"

namespace spatch::stimulus {

// --- Emitter presence probe (added 2026-10-01, after static RE was exhausted)
// ---------------------------------------------------------------------------
// Four static strategies were tried and all four premises were refuted by control
// groups (see artifacts/.../sd-stimulus-hook-surface.md "PHASE E"):
//   1. Lua co-location   -- stimuli that fire 40,080 times are absent from the
//                           shipped scripts entirely, so emission is native.
//   2. Stimulus::Init    -- one caller (PURCHASE); SPRINTING_PERSON, FACE_ME and
//                           WATCHED also have none while arriving. Constructor.
//   3. Name-string xref  -- 0 of 5 CONTROL names have a code reference, so a
//                           negative for the weapons carries no information.
//   4. Immediate scan    -- emission is name-driven, not immediate-driven.
//
// So stop guessing statically and ask the engine directly. StimulusReceiverComponent
// is _TypeUID 0x54000001 and is confirmed live (guard_rejected=0 across 241,693
// samples), and ResolveSimObjectComponent already works behind SEH. Use that same
// proven path to fetch the EMITTER component on the very same SimObject.
//
// If a candidate resolves, its TypeUID is the concrete target and the emitter
// hunt becomes a vtable walk. If none resolve AND the control resolved, the
// honest statement is only "these candidate UIDs do not exist on this object" --
// not "emission originates elsewhere". That stronger claim is available only
// from a control-validated negative; a zero control means the resolve path is
// not working here and the whole probe is INVALID.

// Count the probe ATTEMPTS. This is also the budget gate, so it must advance
// whether or not the control succeeds.
[[nodiscard]] std::uint32_t EmitterProbeCount() noexcept;

// How many times the KNOWN receiver UID (0x54000001) resolved on the same
// sim_object the census ran against. This is the probe's own control: it must
// be non-zero for the sibling count to mean anything. If the control is zero the
// resolve path is not working here and the census says nothing whatsoever --
// the same class of uninterpretable negative that invalidated four earlier
// static premises.
[[nodiscard]] std::uint32_t EmitterControlResolved() noexcept;

// The SDK tags components with `uid >> 25`, so the top SEVEN bits are the
// class. 0xFF000000 would also fold in bit 24 and pull in a neighbouring class.
inline constexpr std::uint32_t kComponentFamilyMask = 0xFE000000u;


// Record whether the KNOWN receiver UID resolved on the sim_object the
// candidates were tried against.
void RecordEmitterControl(bool resolved) noexcept;

// --- Direct component census ------------------------------------------------
// Replaces the speculative candidate-UID list entirely. A SimObject carries a
// component array at [sim+0x68] of 16-byte {ptr, uid} entries with a count at
// [sim+0x60] -- already proven by the push-voice-line component scan. Reading it
// enumerates the REAL UIDs instead of guessing a family, so "no emitter" becomes
// a definite answer rather than an uninterpretable negative.
//
// Scoped to the PRODUCER side deliberately: Stimulus::Init takes the emitter as
// producer and the target separately, so a ped is the RECIPIENT while the thing
// that draws and fires is the player. Both sides are reported separately so a
// hit on the wrong one cannot be mistaken for the right one.
void CensusStimulusComponents(std::uintptr_t sim_object,
                              const char* side) noexcept;

[[nodiscard]] std::uint32_t ComponentCensusCount() noexcept;
// How many censuses actually SAW the known receiver UID in the walked array.
// This is the walk's own control: a sibling count is only meaningful when the
// same loop demonstrably found the component we know is there.
[[nodiscard]] std::uint32_t CensusSawReceiver() noexcept;

// The FIRST sibling component's TypeUID and vtable, first-wins and monotonic.
// A count alone says a component exists but not WHICH, and identifying it is
// the whole point -- it is the concrete target for the vtable walk. Capturing
// it here means one session answers both questions, instead of a positive
// result needing another build/deploy cycle to find out what it found.
[[nodiscard]] std::uint32_t FirstSiblingUid() noexcept;
[[nodiscard]] std::uintptr_t FirstSiblingVftable() noexcept;

[[nodiscard]] std::uint32_t SiblingComponentCount() noexcept;

// Cap on probe attempts. The resolve walks a component list and the detour runs
// per ped per stimulus, so this must not become a hot-path cost. Attempts are
// counted in RecordEmitterControl so a FAILING control still exhausts the
// budget -- otherwise the worst case runs the probe forever.
inline constexpr std::uint32_t kEmitterProbeBudget = 64;

}  // namespace spatch::stimulus
