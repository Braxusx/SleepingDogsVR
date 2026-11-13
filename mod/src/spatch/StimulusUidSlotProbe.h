#pragma once

#include <cstdint>

namespace spatch::stimulus {

// --- Component TypeUID slot reader (2026-10-01) --------------------------------
// Statically the emitter UID is UNRECOVERABLE, and we now know why. Component
// registration does not push a UID immediate; it loads one from an
// uninitialised .data slot that startup fills in. tools/uid_encoding_probe.py
// located both slots (its verdict: UPHELD - no encoding form carries the value):
//
//   receiver  name RVA 0x0175B450 consumed at 0x003323AA
//             mov edx, dword [rip+0x1D3A470]  @ RVA 0x003323AE
//             -> .data slot RVA 0x0206C824, on disk FF FF FF FF
//   emitter   name RVA 0x0175B868 consumed at 0x003321E2
//             mov edx, dword [rip+0x1D3A6E4]  @ RVA 0x003321E6
//             -> .data slot RVA 0x0206C8D0, on disk FF FF FF FF
//
// Reading those two slots at runtime is the first route in this campaign that can
// ship its OWN PASSING CONTROL. The receiver slot is a known value: if
// [0x0206C824] reads back as 0x54000001 then the RVA arithmetic and the image
// base are both proven correct, and the adjacent emitter slot is by construction
// the emitter's own UID. The receiver validates the emitter inside one
// measurement, with no assumption left to check afterwards.
//
// A control that does not equal 0x54000001 is a genuine INVALID: it means the
// slot is still uninitialised (0xFFFFFFFF, registration has not run yet) or the
// address is wrong. Reporting an emitter value in that state would reproduce the
// uninterpretable negative this campaign has already produced four times.
//
// The slots are world-load state filled at component registration, so this
// reading is independent of whether a weapon is ever drawn. It answers the
// emitter-UID question without a draw/fire protocol.

// Absolute RVA of the receiver's runtime-filled UID slot. Proven by the
// receiver's own registration site; reading it must yield kStimulusReceiverTypeUid.
inline constexpr std::uint32_t kReceiverUidSlotRva = 0x0206C824u;

// The emitter's slot, 0xAC bytes after the receiver's in the same .data block.
inline constexpr std::uint32_t kEmitterUidSlotRva = 0x0206C8D0u;

// The uninitialised on-disk value. Seeing it back means "not filled yet", which
// is a distinct verdict from "filled with something else" and must never be
// reported as a UID.
inline constexpr std::uint32_t kUidSlotUninitialised = 0xFFFFFFFFu;

// Raw reads only -- no interpretation, so a bad read cannot be laundered into a
// plausible-looking UID. `control_ok` is the gate on the emitter value.
struct UidSlotReading {
    std::uint32_t receiver_uid = kUidSlotUninitialised;
    std::uint32_t emitter_uid = kUidSlotUninitialised;
    bool control_ok = false;  // receiver read back as the known 0x54000001
    bool slots_addressable = false;  // both RVAs resolved inside the image
};


// Installs the host's RVA->address resolver. Injected rather than duplicated so
// there is exactly one translation of an RVA in this codebase; a second base
// calculation could disagree with the hooks and produce a plausible-looking
// wrong UID, which is precisely what the control would then be asked to excuse.
void SetUidSlotResolver(std::uintptr_t (*resolve)(std::uintptr_t)) noexcept;

// Resolves and reads both slots. Safe to call before world load: it reports
// slots_addressable=false rather than reading an unmapped page.
[[nodiscard]] UidSlotReading ReadUidSlots() noexcept;

// Monotonic counters, for the report line and the budget gate.
[[nodiscard]] std::uint32_t UidSlotProbeReads() noexcept;
[[nodiscard]] std::uint32_t UidSlotProbeControlOk() noexcept;
[[nodiscard]] std::uint32_t UidSlotProbeEmitterResolved() noexcept;
[[nodiscard]] std::uint32_t UidSlotProbeUnaddressable() noexcept;

}  // namespace spatch::stimulus
