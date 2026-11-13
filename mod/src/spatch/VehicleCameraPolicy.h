#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace spatch::vehicle_camera {

struct AddressProfile {
    std::uintptr_t selector_rva = 0;
    std::uintptr_t parameter_setter_rva = 0;

    bool operator==(const AddressProfile&) const = default;
};

struct DynamicAddressProfile {
    std::uintptr_t update_rva = 0;
    std::uintptr_t desired_eye_look_up_rva = 0;
    std::uintptr_t angular_approach_rva = 0;
    std::uintptr_t follow_yaw_return_rva = 0;
    std::uintptr_t manual_yaw_recenter_return_rva = 0;

    bool operator==(const DynamicAddressProfile&) const = default;
};

inline constexpr std::uintptr_t kLegacySelectorRva = 0x003C18B0;
inline constexpr std::uintptr_t kLegacyChaseParameterSetterRva = 0x003C98C0;
inline constexpr std::uintptr_t kLatestSteamSelectorRva = 0x003C1760;
inline constexpr std::uintptr_t kLatestSteamChaseParameterSetterRva = 0x003C9760;
inline constexpr std::uintptr_t kLegacyChaseUpdateRva = 0x003D0000;
inline constexpr std::uintptr_t kLegacyDesiredEyeLookUpRva = 0x003CB1C0;
inline constexpr std::uintptr_t kLegacyAngularApproachRva = 0x00181A50;
inline constexpr std::uintptr_t kLegacyNeedConstrainYawRva = 0x003C7340;
inline constexpr std::uintptr_t kLegacyFollowYawReturnRva = 0x003D0919;
inline constexpr std::uintptr_t kLegacyOnFootSnapGateRva = 0x003DC971;
inline constexpr std::uintptr_t kLegacyManualYawRecenterReturnRva = 0x003D286D;
inline constexpr std::uintptr_t kLatestSteamChaseUpdateRva = 0x003CFEA0;
inline constexpr std::uintptr_t kLatestSteamDesiredEyeLookUpRva = 0x003CB060;
inline constexpr std::uintptr_t kLatestSteamAngularApproachRva = 0x00181AE0;
inline constexpr std::uintptr_t kLatestSteamNeedConstrainYawRva = 0x003C71E0;
inline constexpr std::array<std::uint8_t, 16> kNeedConstrainYawSignature{
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x50,0x80};
inline constexpr std::uintptr_t kLatestSteamFollowYawReturnRva = 0x003D07B9;
inline constexpr std::uintptr_t kLatestSteamManualYawRecenterReturnRva =
    0x003D270D;
inline constexpr std::uintptr_t kLatestSteamOnFootSnapGateRva = 0x003DC811;

inline constexpr AddressProfile kLegacyResearchedAddresses{
    kLegacySelectorRva,
    kLegacyChaseParameterSetterRva,
};
inline constexpr AddressProfile kLatestSteamAddresses{
    kLatestSteamSelectorRva,
    kLatestSteamChaseParameterSetterRva,
};
inline constexpr DynamicAddressProfile kLegacyDynamicAddresses{
    kLegacyChaseUpdateRva,
    kLegacyDesiredEyeLookUpRva,
    kLegacyAngularApproachRva,
    kLegacyFollowYawReturnRva,
    kLegacyManualYawRecenterReturnRva,
};
inline constexpr DynamicAddressProfile kLatestSteamDynamicAddresses{
    kLatestSteamChaseUpdateRva,
    kLatestSteamDesiredEyeLookUpRva,
    kLatestSteamAngularApproachRva,
    kLatestSteamFollowYawReturnRva,
    kLatestSteamManualYawRecenterReturnRva,
};

[[nodiscard]] constexpr AddressProfile SelectAddresses(
    bool latest_steam_layout) noexcept {
    return latest_steam_layout ? kLatestSteamAddresses
                               : kLegacyResearchedAddresses;
}

[[nodiscard]] constexpr DynamicAddressProfile SelectDynamicAddresses(
    bool latest_steam_layout) noexcept {
    return latest_steam_layout ? kLatestSteamDynamicAddresses
                               : kLegacyDynamicAddresses;
}

// The paired Ghidra disassembly and decompilation show the same Win64 ABI and
// prologue in both supported executables: RCX is ChaseCameraComponent, RDX is
// the selected (or transition-blended) ChaseCameraParameters block.
inline constexpr std::array<std::uint8_t, 31> kParameterSetterSignature{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x70,
};

// ChaseCameraComponent::Update(this, dt). The verified GotoAngleSnap path can
// invoke it recursively with dt=0, so any stateful detour must be nesting-safe
// and must not integrate time in the nested snap update.
inline constexpr std::array<std::uint8_t, 18> kChaseUpdateSignature{
    0x40, 0x55, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xA8, 0xFD,
    0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x58, 0x03, 0x00, 0x00,
};

// GameCameraComponent::SetDesiredEyeLookUp(this, dt, eye, look, up,
// collide, out_eye, out_look). Chase Update reaches this once after composing
// its desired pose and before the helper performs the stock collision path.
inline constexpr std::array<std::uint8_t, 23>
    kDesiredEyeLookUpSignature{
        0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x48, 0x20, 0x48,
        0x89, 0x48, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41,
        0x54, 0x41, 0x56, 0x48, 0x8D, 0x68, 0xB8,
    };

// Shortest-path angular approach ABI: RCX=float*, XMM1=target,
// XMM2=radians/second, XMM3=delta seconds. The helper is shared, so the live
// detour additionally requires one of the two exact Chase return addresses and
// the matching component-field pointer.
inline constexpr std::array<std::uint8_t, 23>
    kLegacyAngularApproachSignature{
        0x48, 0x83, 0xEC, 0x18, 0xF3, 0x0F, 0x10, 0x2D,
        0xB0, 0xBB, 0x4E, 0x01, 0x0F, 0x28, 0xE1, 0x0F,
        0x29, 0x34, 0x24, 0xF3, 0x0F, 0x5C, 0x21,
    };
inline constexpr std::array<std::uint8_t, 23>
    kLatestSteamAngularApproachSignature{
        0x48, 0x83, 0xEC, 0x18, 0xF3, 0x0F, 0x10, 0x2D,
        0x88, 0xBB, 0x4E, 0x01, 0x0F, 0x28, 0xE1, 0x0F,
        0x29, 0x34, 0x24, 0xF3, 0x0F, 0x5C, 0x21,
    };

[[nodiscard]] constexpr std::array<std::uint8_t, 23>
SelectAngularApproachSignature(bool latest_steam_layout) noexcept {
    return latest_steam_layout ? kLatestSteamAngularApproachSignature
                               : kLegacyAngularApproachSignature;
}

inline constexpr std::size_t kSubjectOffset = 0xB30;
inline constexpr std::size_t kPhysicsMoverOffset = 0x1A8;
// The paired selector disassembly/decompilation proves these subject fields.
// They replay the exact Flee/Race/Drive selection even when property sets
// alias Race, Drive, and HijackFront to the same parameter block.
inline constexpr std::size_t kSelectorFleeStateOffset = 0x344;
inline constexpr std::size_t kSelectorDriveObjectOffset = 0x1D8;
inline constexpr std::size_t kSelectorDriveObjectStateOffset = 0x68;
inline constexpr std::size_t kSelectorHijackObjectOffset = 0x148;
inline constexpr std::size_t kSelectorHijackObjectModeOffset = 0x348;
inline constexpr std::size_t kSelectorRaceObjectOffset = 0x178;
inline constexpr std::size_t kSelectorRaceObjectStateOffset = 0x688;
inline constexpr std::size_t kTargetParametersOffset = 0xB50;
inline constexpr std::size_t kUpdateEyeOffsetFlagOffset = 0xB59;
inline constexpr std::size_t kAimOrFocusFlagOffset = 0xB5C;
inline constexpr std::size_t kLookingBackFlagOffset = 0x6D0;
inline constexpr std::size_t kIsLookingAroundFlagOffset = 0x4F0;

// FollowCameraComponent state consumed by NeedConstrainYaw. These are not
// ChaseCameraComponent offsets: the native helper selects context limits from
// this range, or explicit limits from the second range.
inline constexpr std::size_t kFollowContextYawRestrictOffset = 0x1F24;
inline constexpr std::size_t kFollowContextYawMinOffset = 0x1F2C;
inline constexpr std::size_t kFollowContextYawRateOffset = 0x1F28;
inline constexpr std::size_t kFollowContextYawMaxOffset = 0x1F30;
inline constexpr std::size_t kFollowExplicitYawRestrictOffset = 0x2030;
inline constexpr std::size_t kFollowExplicitYawMinOffset = 0x2034;
inline constexpr std::size_t kFollowExplicitYawMaxOffset = 0x2038;
inline constexpr std::size_t kFollowExplicitYawRateOffset = 0x203C;

inline constexpr std::size_t kLookaroundJoyInputOffset = 0x4F4;
inline constexpr std::size_t kLookupJoyInputOffset = 0x4F8;
inline constexpr std::size_t kLookupMouseOffset = 0x4FC;
inline constexpr std::size_t kAlternateLookFlagOffset = 0x380;
inline constexpr std::size_t kLookaroundCenterTimerOffset = 0x500;
inline constexpr std::size_t kLookaroundAngleOffset = 0x504;
inline constexpr std::size_t kLookaroundAngleDesiredOffset = 0x508;
inline constexpr std::size_t kCenteringSpeedMinComponentOffset = 0x514;
inline constexpr std::size_t kCenteringSpeedMaxComponentOffset = 0x518;
inline constexpr std::size_t kReverseSpeedComponentOffset = 0x51C;
inline constexpr std::size_t kOrbitSpeedComponentOffset = 0x520;
inline constexpr std::size_t kYawAngVelTimespanComponentOffset = 0x524;
inline constexpr std::size_t kLookOffsetMaxComponentOffset = 0x528;
inline constexpr std::size_t kLookOffsetAngVelMinComponentOffset = 0x52C;
inline constexpr std::size_t kLookOffsetAngVelMaxComponentOffset = 0x530;
inline constexpr std::size_t kPitchOffsetMaxComponentOffset = 0x534;
inline constexpr std::size_t kPitchOffsetLookFactorUpComponentOffset = 0x538;
inline constexpr std::size_t kPitchOffsetLookFactorDownComponentOffset = 0x53C;
inline constexpr std::size_t kPitchOffsetEyeFactorUpComponentOffset = 0x540;
inline constexpr std::size_t kPitchOffsetEyeFactorDownComponentOffset = 0x544;
inline constexpr std::size_t kForwardAngleOffset = 0x958;
inline constexpr std::size_t kForwardAngleDesiredOffset = 0x95C;
inline constexpr std::size_t kTargetPitchPositionOffset = 0x9A8;
inline constexpr std::size_t kTargetPitchTargetOffset = 0x9AC;
inline constexpr std::size_t kYawAngVelRunningSumOffset = 0xA64;
inline constexpr std::size_t kYawAngVelRunningTimeOffset = 0xA68;
inline constexpr std::size_t kEyeLockFlagOffset = 0xB6C;
inline constexpr std::size_t kLookLockFlagOffset = 0xB6D;
inline constexpr std::size_t kPhysicsMoverForwardSpeedOffset = 0x2B4;
inline constexpr std::size_t kPhysicsMoverSteeringInputOffset = 0x58;
inline constexpr std::size_t kPhysicsMoverInputFlagsOffset = 0x68;
inline constexpr std::uint32_t kPhysicsMoverInReverseFlag = 1u << 0;
inline constexpr std::uint32_t kPhysicsMoverHandbrakeFlag = 1u << 2;

[[nodiscard]] constexpr bool IsPhysicsMoverReversing(
    std::uint32_t input_flags) noexcept {
    return (input_flags & kPhysicsMoverInReverseFlag) != 0;
}
// The selector copies the outgoing parameter state here, then overwrites its
// eye/target ranges with the live Homer-spring values before blending. D28 is
// the remaining source weight: 1 at transition start and 0 at completion.
inline constexpr std::size_t kTransitionSourceParametersOffset = 0xBE8;
inline constexpr std::size_t kTransitionSourceWeightOffset = 0xD28;

inline constexpr std::size_t kParameterNamePointerOffset = 0x0;
inline constexpr std::size_t kParameterNameSymbolOffset = 0x8;
inline constexpr std::size_t kParameterContextOffset = 0xC;
inline constexpr std::size_t kParameterYawAngVelTimespanOffset = 0x40;
inline constexpr std::size_t kParameterLookOffsetMaxOffset = 0x44;
inline constexpr std::size_t kParameterLookOffsetAngVelMinOffset = 0x48;
inline constexpr std::size_t kParameterLookOffsetAngVelMaxOffset = 0x4C;
inline constexpr std::size_t kParameterPitchOffsetMaxOffset = 0x50;
inline constexpr std::size_t kParameterPitchOffsetLookFactorUpOffset = 0x54;
inline constexpr std::size_t kParameterPitchOffsetLookFactorDownOffset = 0x58;
inline constexpr std::size_t kParameterPitchOffsetEyeFactorUpOffset = 0x5C;
inline constexpr std::size_t kParameterPitchOffsetEyeFactorDownOffset = 0x60;
inline constexpr std::size_t kParameterCenteringSpeedMinOffset = 0xF0;
inline constexpr std::size_t kParameterCenteringSpeedMaxOffset = 0xF4;
inline constexpr std::size_t kParameterReverseSpeedOffset = 0xF8;
inline constexpr std::size_t kParameterOrbitSpeedOffset = 0x108;
inline constexpr std::uint32_t kFleeContext = 1;
inline constexpr std::uint32_t kDriveContext = 3;

// VehicleSubjectComponent::OnAttach resolves the complete ChaseCameraContext
// array into these pointer fields. The SDK enum order is confirmed by the
// selector's paired disassembly/decompilation. Normal player driving selects
// +0x388 at runtime; +0x3B0 is HijackFront, not the default road camera.
inline constexpr std::size_t kAimParametersSlotOffset = 0x370;
inline constexpr std::size_t kFleeParametersSlotOffset = 0x378;
inline constexpr std::size_t kRaceParametersSlotOffset = 0x380;
inline constexpr std::size_t kDriveParametersSlotOffset = 0x388;
inline constexpr std::size_t kBurnoutParametersSlotOffset = 0x390;
inline constexpr std::size_t kPassengerParametersSlotOffset = 0x398;
inline constexpr std::size_t kHijackBackParametersSlotOffset = 0x3A0;
inline constexpr std::size_t kHijackSideParametersSlotOffset = 0x3A8;
inline constexpr std::size_t kHijackFrontParametersSlotOffset = 0x3B0;
inline constexpr std::size_t kHijackTopParametersSlotOffset = 0x3B8;
inline constexpr std::size_t kLookBackParametersSlotOffset = 0x3C0;
inline constexpr std::size_t kLookSideParametersSlotOffset = 0x3C8;
inline constexpr std::size_t kLookUpParametersSlotOffset = 0x3D0;
inline constexpr std::array<std::size_t, 13> kParameterSlotOffsets{
    0x370,
    0x378,
    0x380,
    0x388,
    0x390,
    0x398,
    0x3A0,
    0x3A8,
    0x3B0,
    0x3B8,
    0x3C0,
    0x3C8,
    0x3D0,
};
inline constexpr std::size_t kAimParametersSlotIndex = 0;
inline constexpr std::size_t kFleeParametersSlotIndex = 1;
inline constexpr std::size_t kRaceParametersSlotIndex = 2;
inline constexpr std::size_t kDriveParametersSlotIndex = 3;
inline constexpr std::size_t kBurnoutParametersSlotIndex = 4;
inline constexpr std::size_t kPassengerParametersSlotIndex = 5;
inline constexpr std::size_t kHijackBackParametersSlotIndex = 6;
inline constexpr std::size_t kHijackSideParametersSlotIndex = 7;
inline constexpr std::size_t kHijackFrontParametersSlotIndex = 8;
inline constexpr std::size_t kHijackTopParametersSlotIndex = 9;
inline constexpr std::size_t kLookBackParametersSlotIndex = 10;
inline constexpr std::size_t kLookSideParametersSlotIndex = 11;
inline constexpr std::size_t kLookUpParametersSlotIndex = 12;

using ParameterSlotMask = std::uint16_t;

[[nodiscard]] constexpr ParameterSlotMask ParameterSlotMaskForIndex(
    std::size_t index) noexcept {
    return index < kParameterSlotOffsets.size()
               ? static_cast<ParameterSlotMask>(
                     ParameterSlotMask{1} << static_cast<unsigned int>(index))
               : ParameterSlotMask{0};
}

inline constexpr ParameterSlotMask kAimParametersSlotMask =
    ParameterSlotMaskForIndex(kAimParametersSlotIndex);
inline constexpr ParameterSlotMask kFleeParametersSlotMask =
    ParameterSlotMaskForIndex(kFleeParametersSlotIndex);
inline constexpr ParameterSlotMask kRaceParametersSlotMask =
    ParameterSlotMaskForIndex(kRaceParametersSlotIndex);
inline constexpr ParameterSlotMask kDriveParametersSlotMask =
    ParameterSlotMaskForIndex(kDriveParametersSlotIndex);
inline constexpr ParameterSlotMask kBurnoutParametersSlotMask =
    ParameterSlotMaskForIndex(kBurnoutParametersSlotIndex);
inline constexpr ParameterSlotMask kPassengerParametersSlotMask =
    ParameterSlotMaskForIndex(kPassengerParametersSlotIndex);
inline constexpr ParameterSlotMask kHijackBackParametersSlotMask =
    ParameterSlotMaskForIndex(kHijackBackParametersSlotIndex);
inline constexpr ParameterSlotMask kHijackSideParametersSlotMask =
    ParameterSlotMaskForIndex(kHijackSideParametersSlotIndex);
inline constexpr ParameterSlotMask kHijackFrontParametersSlotMask =
    ParameterSlotMaskForIndex(kHijackFrontParametersSlotIndex);
inline constexpr ParameterSlotMask kHijackTopParametersSlotMask =
    ParameterSlotMaskForIndex(kHijackTopParametersSlotIndex);
inline constexpr ParameterSlotMask kLookBackParametersSlotMask =
    ParameterSlotMaskForIndex(kLookBackParametersSlotIndex);
inline constexpr ParameterSlotMask kLookSideParametersSlotMask =
    ParameterSlotMaskForIndex(kLookSideParametersSlotIndex);
inline constexpr ParameterSlotMask kLookUpParametersSlotMask =
    ParameterSlotMaskForIndex(kLookUpParametersSlotIndex);
inline constexpr ParameterSlotMask kDriveBlockAllowedSlotMask =
    kRaceParametersSlotMask | kDriveParametersSlotMask |
    kHijackFrontParametersSlotMask;
// The police-minigame operating trace proves only ExoticCar's exact
// Race/Drive alias. Other Race alias shapes stay native until observed.
inline constexpr ParameterSlotMask kCapturedRoadRaceSlotMask =
    kRaceParametersSlotMask | kDriveParametersSlotMask;
inline constexpr ParameterSlotMask kMotorcycleDriveBlockAllowedSlotMask =
    kDriveBlockAllowedSlotMask;

struct BaseDriveBranchInputs {
    bool flee_active = false;
    bool drive_object_present = false;
    bool drive_object_state_present = false;
    bool hijack_object_present = false;
    std::uint32_t hijack_object_mode = 0;
    bool race_object_present = false;
    bool race_object_state_present = false;
};

enum class BaseSelectorBranch : std::uint8_t {
    Unknown,
    Flee,
    Race,
    Drive,
    HijackFront,
};

// Base selector order is Flee, Race, then Drive/HijackFront. When the Drive
// predicate D is true, Race requires the +178/+688 pair. When D is false, the
// Race helper instead accepts +148 with mode +348 equal to 6. This exact replay
// is necessary because property sets can reuse one context-3 block for Race,
// Drive, and HijackFront.
[[nodiscard]] constexpr BaseSelectorBranch ResolveBaseSelectorBranch(
    const BaseDriveBranchInputs& inputs) noexcept {
    if (inputs.flee_active) {
        return BaseSelectorBranch::Flee;
    }
    const bool drive_predicate =
        inputs.drive_object_present && inputs.drive_object_state_present &&
        !inputs.hijack_object_present;
    const bool race_predicate = drive_predicate
                                    ? inputs.race_object_present &&
                                          inputs.race_object_state_present
                                    : inputs.hijack_object_present &&
                                          inputs.hijack_object_mode == 6;
    return race_predicate ? BaseSelectorBranch::Race
                          : (drive_predicate
                                 ? BaseSelectorBranch::Drive
                                 : BaseSelectorBranch::HijackFront);
}

[[nodiscard]] constexpr bool IsBaseDriveBranchSelected(
    const BaseDriveBranchInputs& inputs) noexcept {
    return ResolveBaseSelectorBranch(inputs) == BaseSelectorBranch::Drive;
}

[[nodiscard]] constexpr const char* BaseSelectorBranchName(
    BaseSelectorBranch branch) noexcept {
    switch (branch) {
        case BaseSelectorBranch::Flee:
            return "flee";
        case BaseSelectorBranch::Race:
            return "race";
        case BaseSelectorBranch::Drive:
            return "drive";
        case BaseSelectorBranch::HijackFront:
            return "hijack_front";
        default:
            return "unknown";
    }
}

struct ParameterIdentity {
    std::uintptr_t name_pointer = 0;
    std::uint32_t name_symbol = 0;
    std::uint32_t context = 0;

    bool operator==(const ParameterIdentity&) const = default;
};

[[nodiscard]] constexpr ParameterSlotMask ProfileSlotMatchMask(
    std::uintptr_t parameters,
    const std::array<std::uintptr_t, kParameterSlotOffsets.size()>&
        slots) noexcept {
    if (parameters == 0) {
        return 0;
    }
    ParameterSlotMask mask = 0;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (slots[index] == parameters) {
            mask = static_cast<ParameterSlotMask>(
                mask | ParameterSlotMaskForIndex(index));
        }
    }
    return mask;
}

// SetChaseParameters copies three low/medium/high eye and target ranges into
// the component, then resolves current target and (when +0xB59 is nonzero)
// current eye. These are the second qVector3 components identified as lateral.
// Paired CameraSubject vtable and chase-update evidence maps matrix basis v1 to
// GetLeft and consumes it without negation. The user gameplay trace proved the
// Drive-slot and road-car eligibility path before activation.
inline constexpr std::array<std::size_t, 4> kEyeLateralComponentOffsets{
    0x54C,
    0x578,
    0x584,
    0x590,
};
inline constexpr std::array<std::size_t, 4> kTargetLateralComponentOffsets{
    0x558,
    0x5A8,
    0x5B4,
    0x5C0,
};

inline constexpr std::size_t kParameterBlockSize = 0x140;
inline constexpr std::array<std::size_t, 6>
    kDriverSideLateralParameterOffsets{
        0x84,
        0x94,
        0xA4,
        0xB4,
        0xC4,
    0xD4,
};

// The CameraSubject vtable and chase-update matrix multiply prove that the
// parameter block's positive second component follows vehicle-local Left.
// Wei sits on the right, so his driver-side offset is negative. Magnitude is a
// first-pass value for the user's visual gameplay A/B.
inline constexpr float kDriverSideLateralOffsetMeters = -0.35f;

using ParameterBlock = std::array<std::byte, kParameterBlockSize>;

enum class ParameterOffsetResult : std::uint8_t {
    Applied,
    InvalidDelta,
    InvalidParameter,
};

[[nodiscard]] constexpr std::uint32_t ReadParameterU32(
    const ParameterBlock& block,
    std::size_t offset) noexcept {
    if (offset > block.size() - sizeof(std::uint32_t)) {
        return 0;
    }
    return std::to_integer<std::uint32_t>(block[offset]) |
           (std::to_integer<std::uint32_t>(block[offset + 1]) << 8) |
           (std::to_integer<std::uint32_t>(block[offset + 2]) << 16) |
           (std::to_integer<std::uint32_t>(block[offset + 3]) << 24);
}

[[nodiscard]] constexpr float ReadParameterFloat(
    const ParameterBlock& block,
    std::size_t offset) noexcept {
    return std::bit_cast<float>(ReadParameterU32(block, offset));
}

constexpr void WriteParameterFloat(ParameterBlock& block,
                                   std::size_t offset,
                                   float value) noexcept {
    if (offset > block.size() - sizeof(std::uint32_t)) {
        return;
    }
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    block[offset] = static_cast<std::byte>(bits & 0xFFu);
    block[offset + 1] = static_cast<std::byte>((bits >> 8) & 0xFFu);
    block[offset + 2] = static_cast<std::byte>((bits >> 16) & 0xFFu);
    block[offset + 3] = static_cast<std::byte>((bits >> 24) & 0xFFu);
}

[[nodiscard]] constexpr bool IsFiniteParameterFloat(float value) noexcept {
    return (std::bit_cast<std::uint32_t>(value) & 0x7F800000u) !=
           0x7F800000u;
}

// First runtime candidate, derived from the captured legacy Chase path and the
// GTA IV/FusionFix behavior contract. These values are deliberately centralized
// so the user's real-game A/B can tune the feel without changing hook logic.
inline constexpr float kGtaIvFollowRateScale = 0.50f;
inline constexpr float kGtaIvHandbrakeFollowRateScale = 0.20f;
inline constexpr float kGtaIvManualYawRecenterDelaySeconds = 0.75f;
inline constexpr float kGtaIvManualYawRecenterRateScale = 0.65f;
inline constexpr float kGtaIvManualLookInputDeadzone = 0.01f;
inline constexpr float kGtaIvManualPitchInputRateRadiansPerSecond = 1.80f;
inline constexpr float kGtaIvMousePitchRadiansPerEyeMeter = 0.45f;
inline constexpr float kGtaIvManualPitchMinimumRadians = -0.30f;
inline constexpr float kGtaIvManualPitchMaximumRadians = 0.35f;
inline constexpr float kGtaIvMinimumEyeOrbitPitchRadians = -0.05f;
inline constexpr float kGtaIvMaximumEyeOrbitPitchRadians = 0.75f;
inline constexpr float kGtaIvManualPitchRecenterDelaySeconds = 0.75f;
inline constexpr float kGtaIvManualPitchRecenterRateRadiansPerSecond = 0.45f;
inline constexpr float kGtaIvMaximumDynamicsDeltaSeconds = 0.05f;

[[nodiscard]] constexpr float CameraAbs(float value) noexcept {
    return value < 0.0f ? -value : value;
}

[[nodiscard]] constexpr float ClampCameraValue(float value,
                                                float minimum,
                                                float maximum) noexcept {
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

[[nodiscard]] constexpr float ResolveGtaIvFollowRate(float stock_rate,
                                                     bool handbrake) noexcept {
    if (!IsFiniteParameterFloat(stock_rate) || stock_rate <= 0.0f) {
        return stock_rate;
    }
    return stock_rate * (handbrake ? kGtaIvHandbrakeFollowRateScale
                                  : kGtaIvFollowRateScale);
}

[[nodiscard]] constexpr bool ShouldHoldGtaIvManualYaw(
    float horizontal_input,
    float center_timer_seconds,
    bool reversing,
    bool disable_autocenter = false) noexcept {
    if (reversing || !IsFiniteParameterFloat(horizontal_input) ||
        !IsFiniteParameterFloat(center_timer_seconds)) {
        return false;
    }
    if (disable_autocenter) {
        return true;
    }
    return CameraAbs(horizontal_input) >= kGtaIvManualLookInputDeadzone ||
           center_timer_seconds < kGtaIvManualYawRecenterDelaySeconds;
}

[[nodiscard]] constexpr float ResolveGtaIvManualYawRecenterRate(
    float stock_rate) noexcept {
    if (!IsFiniteParameterFloat(stock_rate) || stock_rate <= 0.0f) {
        return stock_rate;
    }
    return stock_rate * kGtaIvManualYawRecenterRateScale;
}

struct ManualPitchState {
    float angle_radians = 0.0f;
    float idle_seconds = 0.0f;
    bool initialized = false;

    bool operator==(const ManualPitchState&) const = default;
};

struct ManualPitchFrameInput {
    float delta_seconds = 0.0f;
    float vertical_input = 0.0f;
    bool disable_recenter = false;
};

// Stock consumes +0x4FC as an absolute eye-Z offset before it acquires the
// current frame's mouse input. Map that absolute state to an angle instead of
// differentiating it: engine-driven decay then recenters pitch instead of
// masquerading as opposite mouse input.
[[nodiscard]] constexpr float ResolveGtaIvManualPitchOffset(
    float controller_pitch_radians,
    float stock_mouse_eye_offset) noexcept {
    if (!IsFiniteParameterFloat(controller_pitch_radians) ||
        !IsFiniteParameterFloat(stock_mouse_eye_offset)) {
        return 0.0f;
    }
    return ClampCameraValue(
        controller_pitch_radians +
            stock_mouse_eye_offset * kGtaIvMousePitchRadiansPerEyeMeter,
        kGtaIvManualPitchMinimumRadians,
        kGtaIvManualPitchMaximumRadians);
}

[[nodiscard]] constexpr bool ShouldResetGtaIvManualPitchForUpdate(
    unsigned int depth,
    float delta_seconds) noexcept {
    return depth == 1 && IsFiniteParameterFloat(delta_seconds) &&
           delta_seconds <= 0.0f;
}

[[nodiscard]] constexpr ManualPitchState StepGtaIvManualPitch(
    ManualPitchState state,
    const ManualPitchFrameInput& input) noexcept {
    if (!IsFiniteParameterFloat(input.delta_seconds) ||
        input.delta_seconds <= 0.0f ||
        !IsFiniteParameterFloat(input.vertical_input)) {
        return state;
    }

    const float delta_seconds = ClampCameraValue(
        input.delta_seconds, 0.0f, kGtaIvMaximumDynamicsDeltaSeconds);
    if (!state.initialized ||
        !IsFiniteParameterFloat(state.angle_radians) ||
        !IsFiniteParameterFloat(state.idle_seconds)) {
        state = {};
        state.initialized = true;
    }

    const bool has_manual_input =
        CameraAbs(input.vertical_input) >= kGtaIvManualLookInputDeadzone;

    if (has_manual_input) {
        state.angle_radians +=
            -input.vertical_input *
            kGtaIvManualPitchInputRateRadiansPerSecond * delta_seconds;
    }
    state.angle_radians = ClampCameraValue(
        state.angle_radians,
        kGtaIvManualPitchMinimumRadians,
        kGtaIvManualPitchMaximumRadians);

    if (has_manual_input) {
        state.idle_seconds = 0.0f;
    } else {
        state.idle_seconds += delta_seconds;
        if (!input.disable_recenter &&
            state.idle_seconds >= kGtaIvManualPitchRecenterDelaySeconds) {
            const float step =
                kGtaIvManualPitchRecenterRateRadiansPerSecond * delta_seconds;
            if (CameraAbs(state.angle_radians) <= step) {
                state.angle_radians = 0.0f;
            } else if (state.angle_radians > 0.0f) {
                state.angle_radians -= step;
            } else {
                state.angle_radians += step;
            }
        }
    }
    return state;
}

[[nodiscard]] constexpr bool IsValidTransitionSourceWeight(
    float value) noexcept {
    return IsFiniteParameterFloat(value) && value >= 0.0f && value <= 1.0f;
}

[[nodiscard]] constexpr ParameterOffsetResult
OffsetDriverSideLateralParameters(ParameterBlock& block,
                                  float lateral_offset_meters) noexcept {
    if (!IsFiniteParameterFloat(lateral_offset_meters)) {
        return ParameterOffsetResult::InvalidDelta;
    }

    std::array<float, kDriverSideLateralParameterOffsets.size()> adjusted{};
    for (std::size_t index = 0; index < adjusted.size(); ++index) {
        const float current = ReadParameterFloat(
            block, kDriverSideLateralParameterOffsets[index]);
        if (!IsFiniteParameterFloat(current)) {
            return ParameterOffsetResult::InvalidParameter;
        }
        const float next = current + lateral_offset_meters;
        if (!IsFiniteParameterFloat(next)) {
            return ParameterOffsetResult::InvalidParameter;
        }
        adjusted[index] = next;
    }

    // Commit only after all six values pass validation so a rejected block is
    // byte-for-byte unchanged.
    for (std::size_t index = 0; index < adjusted.size(); ++index) {
        WriteParameterFloat(block,
                            kDriverSideLateralParameterOffsets[index],
                            adjusted[index]);
    }
    return ParameterOffsetResult::Applied;
}

template <typename Setter>
constexpr void InvokeSelectedParameterBlockOnce(
    Setter&& setter,
    const void* original_parameters,
    const ParameterBlock& adjusted_parameters,
    bool use_adjusted_parameters) noexcept {
    setter(use_adjusted_parameters
               ? static_cast<const void*>(adjusted_parameters.data())
               : original_parameters);
}

[[nodiscard]] constexpr bool ShouldAttemptOuterDynamicProbeCapture(
    unsigned int depth,
    bool transaction_ready,
    bool logging_enabled,
    bool feature_enabled) noexcept {
    return depth == 0 && transaction_ready && logging_enabled &&
           feature_enabled;
}

// The behavior scope is independent of diagnostics: yaw/pitch hooks need an
// active Chase frame even when Logging=0. Nested zero-delta updates inherit an
// already-active outer scope so they can be identified without integrating a
// second frame of mod-owned state.
[[nodiscard]] constexpr bool ShouldEnterDynamicCameraScope(
    unsigned int depth,
    bool transaction_ready,
    bool feature_enabled,
    bool active_subject) noexcept {
    return depth != 0 ||
           (transaction_ready && feature_enabled && active_subject);
}

struct DynamicProbeCallCounts {
    unsigned int setter_calls = 0;
    unsigned int desired_pose_calls = 0;
    unsigned int nested_updates = 0;
    unsigned int nested_setter_calls = 0;
    unsigned int nested_desired_pose_calls = 0;
};

constexpr void MergeNestedDynamicProbeCounts(
    DynamicProbeCallCounts& parent,
    const DynamicProbeCallCounts& child) noexcept {
    ++parent.nested_updates;
    parent.nested_updates += child.nested_updates;
    parent.nested_setter_calls +=
        child.setter_calls + child.nested_setter_calls;
    parent.nested_desired_pose_calls +=
        child.desired_pose_calls + child.nested_desired_pose_calls;
}

template <typename Update>
constexpr void InvokeChaseCameraUpdateOnce(Update&& update,
                                           void* component,
                                           float delta_seconds) {
    update(component, delta_seconds);
}

template <typename Submit, typename Vector>
constexpr void InvokeDesiredEyeLookUpOnce(
    Submit&& submit,
    void* component,
    float delta_seconds,
    const Vector* desired_eye,
    const Vector* desired_look,
    const Vector* desired_up,
    bool collide,
    Vector* new_eye,
    Vector* new_look) {
    submit(component,
           delta_seconds,
           desired_eye,
           desired_look,
           desired_up,
           collide,
           new_eye,
           new_look);
}

enum class VehicleClass : std::uint8_t {
    Unknown,
    CarPhysicsMover,
    Motorcycle,
    Boat,
};

enum class DriverSideTargetProfile : std::uint8_t {
    None,
    RoadDrive,
    RoadRace,
    RoadFlee,
    MotorcycleDriveBlock,
    MotorcycleFlee,
};

[[nodiscard]] constexpr bool IsValidParameterIdentityForContext(
    const ParameterIdentity& identity,
    std::uint32_t expected_context) noexcept {
    return identity.name_pointer != 0 && identity.name_symbol != 0 &&
           identity.context == expected_context;
}

// Paired disassembly/decompilation proves the selector order Flee, Race, then
// Drive/HijackFront in both executable layouts. Runtime evidence additionally
// proves that Truck Drive reuses one context-3 block in the Race, Drive, and
// HijackFront slots. The replayed base-branch predicate disambiguates normal
// Drive from those aliases; blocks aliased to any other specialized slot remain
// rejected.
[[nodiscard]] constexpr DriverSideTargetProfile
ClassifyDriverSideTargetProfileFromMatchMask(
    VehicleClass vehicle_class,
    std::uintptr_t target_parameters,
    ParameterSlotMask match_mask,
    const std::array<std::uintptr_t, kParameterSlotOffsets.size()>& slots,
    const ParameterIdentity& target_identity,
    BaseSelectorBranch base_selector_branch) noexcept {
    if (target_parameters == 0) {
        return DriverSideTargetProfile::None;
    }

    if (vehicle_class == VehicleClass::CarPhysicsMover) {
        if (base_selector_branch == BaseSelectorBranch::Drive &&
            target_parameters == slots[kDriveParametersSlotIndex] &&
            (match_mask & kDriveParametersSlotMask) != 0 &&
            (match_mask & static_cast<ParameterSlotMask>(
                              ~kDriveBlockAllowedSlotMask)) == 0 &&
            IsValidParameterIdentityForContext(target_identity,
                                               kDriveContext)) {
            return DriverSideTargetProfile::RoadDrive;
        }
        if (base_selector_branch == BaseSelectorBranch::Race &&
            target_parameters == slots[kRaceParametersSlotIndex] &&
            match_mask == kCapturedRoadRaceSlotMask &&
            IsValidParameterIdentityForContext(target_identity,
                                               kDriveContext)) {
            return DriverSideTargetProfile::RoadRace;
        }
        if (target_parameters == slots[kFleeParametersSlotIndex] &&
            match_mask == kFleeParametersSlotMask &&
            IsValidParameterIdentityForContext(target_identity,
                                               kFleeContext)) {
            return DriverSideTargetProfile::RoadFlee;
        }
        return DriverSideTargetProfile::None;
    }

    if (vehicle_class == VehicleClass::Motorcycle) {
        // Paired selector evidence and a real Bike pursuit capture prove this
        // class uses the canonical +0x378 Flee target. Keep the discriminator
        // stricter than the known Bike Drive alias block: only the unique
        // context-1 target can enter motorcycle pursuit tuning.
        if (target_parameters == slots[kFleeParametersSlotIndex] &&
            match_mask == kFleeParametersSlotMask &&
            IsValidParameterIdentityForContext(target_identity,
                                               kFleeContext)) {
            return DriverSideTargetProfile::MotorcycleFlee;
        }
        if (base_selector_branch == BaseSelectorBranch::Drive &&
            target_parameters == slots[kDriveParametersSlotIndex] &&
            (match_mask & kDriveParametersSlotMask) != 0 &&
            (match_mask & static_cast<ParameterSlotMask>(
                              ~kMotorcycleDriveBlockAllowedSlotMask)) == 0 &&
            IsValidParameterIdentityForContext(target_identity,
                                               kDriveContext)) {
            return DriverSideTargetProfile::MotorcycleDriveBlock;
        }
    }
    return DriverSideTargetProfile::None;
}

[[nodiscard]] constexpr DriverSideTargetProfile
ClassifyDriverSideTargetProfile(
    VehicleClass vehicle_class,
    std::uintptr_t target_parameters,
    const std::array<std::uintptr_t, kParameterSlotOffsets.size()>& slots,
    const ParameterIdentity& target_identity,
    BaseSelectorBranch base_selector_branch) noexcept {
    return ClassifyDriverSideTargetProfileFromMatchMask(
        vehicle_class,
        target_parameters,
        ProfileSlotMatchMask(target_parameters, slots),
        slots,
        target_identity,
        base_selector_branch);
}

[[nodiscard]] constexpr const char* DriverSideTargetProfileName(
    DriverSideTargetProfile profile) noexcept {
    switch (profile) {
        case DriverSideTargetProfile::RoadDrive:
            return "road_drive";
        case DriverSideTargetProfile::RoadRace:
            return "road_race";
        case DriverSideTargetProfile::RoadFlee:
            return "road_flee";
        case DriverSideTargetProfile::MotorcycleDriveBlock:
            return "motorcycle_drive_block";
        case DriverSideTargetProfile::MotorcycleFlee:
            return "motorcycle_flee";
        default:
            return "none";
    }
}

struct CameraFeatureRequests {
    bool gta_iv_car_camera = false;
    bool gta_iv_bike_camera = false;
    bool gta_v_speed_shake = false;
    bool disable_controller_camera_autocenter = false;
    bool disable_onfoot_camera_autocenter = false;
    bool driving_swear_lines = false;
    bool speed_rumble = false;
    bool disable_pursuit_camera = false;
    // FixChaseAimCamera rides the desired-pose detour (the distance floor in
    // DetourGameCameraSetDesiredEyeLookUp). It must OR into
    // NeedsVehicleCameraCoreHooks or the pose detour never installs for a
    // user who sets no other camera key - the exact dropped-field class this
    // struct's install-site comment documents.
    bool fix_chase_aim_camera = false;
    // ModernBrakeReverse rides the chase camera scope for the player's raw
    // reverse-key state and the mover->drivetrain calibration.
    bool modern_brake_reverse = false;
    // NoWheelTurnBack rides the same scope for the driving-freshness publish.
    bool no_wheel_turn_back = false;
};

// Parameter selection, Chase Update scoping, and the desired-pose detour are
// shared infrastructure.  The angular-approach interception is needed only by
// the GTA IV follow/recenter behavior.
[[nodiscard]] constexpr bool NeedsVehicleCameraCoreHooks(
    const CameraFeatureRequests& requests) noexcept {
    return requests.gta_iv_car_camera || requests.gta_iv_bike_camera ||
           requests.gta_v_speed_shake || requests.speed_rumble ||
           requests.disable_pursuit_camera ||
           requests.fix_chase_aim_camera ||
           requests.disable_controller_camera_autocenter ||
           requests.disable_onfoot_camera_autocenter ||
           requests.driving_swear_lines || requests.modern_brake_reverse ||
           requests.no_wheel_turn_back;
}

[[nodiscard]] constexpr bool NeedsGtaIvAngularApproachHook(
    const CameraFeatureRequests& requests) noexcept {
    return requests.gta_iv_car_camera || requests.gta_iv_bike_camera ||
           requests.disable_controller_camera_autocenter ||
           requests.disable_onfoot_camera_autocenter;
}

struct GtaVSpeedShakeEligibility {
    bool feature_enabled = false;
    bool transaction_ready = false;
    bool camera_state_readable = false;
    VehicleClass vehicle_class = VehicleClass::Unknown;
    DriverSideTargetProfile target_profile = DriverSideTargetProfile::None;
    bool aim_or_focus = false;
    bool eye_locked = false;
    bool look_locked = false;
};

[[nodiscard]] constexpr bool IsGtaVSpeedShakeTargetProfile(
    VehicleClass vehicle_class,
    DriverSideTargetProfile target_profile) noexcept {
    switch (target_profile) {
        case DriverSideTargetProfile::RoadDrive:
        case DriverSideTargetProfile::RoadFlee:
            return vehicle_class == VehicleClass::CarPhysicsMover;
        case DriverSideTargetProfile::MotorcycleDriveBlock:
        case DriverSideTargetProfile::MotorcycleFlee:
            return vehicle_class == VehicleClass::Motorcycle;
        default:
            return false;
    }
}

[[nodiscard]] constexpr bool IsGtaVSpeedShakeEligible(
    const GtaVSpeedShakeEligibility& eligibility) noexcept {
    // GTA V's ordinary follow camera keeps the shaker through normal orbit,
    // look-behind, and police-chase modes.  Aim/drive-by uses a different
    // camera class; Sleeping Dogs' explicit eye/look locks likewise remain
    // native because this detour cannot safely post-compose those overrides.
    return eligibility.feature_enabled && eligibility.transaction_ready &&
           eligibility.camera_state_readable &&
           !eligibility.aim_or_focus && !eligibility.eye_locked &&
           !eligibility.look_locked &&
           IsGtaVSpeedShakeTargetProfile(eligibility.vehicle_class,
                                         eligibility.target_profile);
}

struct DriverSideEligibility {
    bool car_feature_enabled = false;
    bool bike_feature_enabled = false;
    bool transaction_ready = false;
    VehicleClass vehicle_class = VehicleClass::Unknown;
    DriverSideTargetProfile target_profile = DriverSideTargetProfile::None;
    bool looking_back = false;
    bool aim_or_focus = false;
    std::uintptr_t selected_parameters = 0;
    std::uintptr_t target_parameters = 0;
    float transition_source_weight = 0.0f;
};

[[nodiscard]] constexpr bool IsDriverSideTargetProfileEnabled(
    const DriverSideEligibility& eligibility) noexcept {
    switch (eligibility.target_profile) {
        case DriverSideTargetProfile::RoadDrive:
        case DriverSideTargetProfile::RoadRace:
        case DriverSideTargetProfile::RoadFlee:
            return eligibility.car_feature_enabled &&
                   eligibility.vehicle_class == VehicleClass::CarPhysicsMover;
        case DriverSideTargetProfile::MotorcycleDriveBlock:
        case DriverSideTargetProfile::MotorcycleFlee:
            return eligibility.bike_feature_enabled &&
                   eligibility.vehicle_class == VehicleClass::Motorcycle;
        default:
            return false;
    }
}

[[nodiscard]] constexpr float ResolveDriverSideBlendFactor(
    const DriverSideEligibility& eligibility) noexcept {
    if (!eligibility.transaction_ready ||
        !IsDriverSideTargetProfileEnabled(eligibility) ||
        eligibility.looking_back || eligibility.aim_or_focus ||
        eligibility.selected_parameters == 0 ||
        eligibility.target_parameters == 0 ||
        !IsValidTransitionSourceWeight(
            eligibility.transition_source_weight)) {
        return 0.0f;
    }

    if (eligibility.transition_source_weight > 0.0f) {
        // The selector's transition-local block intentionally leaves its
        // 16-byte identity header uninitialized. Canonical B50 is the target;
        // D28 is the source weight. The source snapshot already contains the
        // live, previously offset camera ranges, so add only the eligible
        // target's (1 - source) share and let outgoing transitions fade.
        if (eligibility.selected_parameters == eligibility.target_parameters) {
            return 0.0f;
        }
        return 1.0f - eligibility.transition_source_weight;
    }

    // With no blend, the selector passes the canonical target block directly.
    return eligibility.selected_parameters == eligibility.target_parameters
               ? 1.0f
               : 0.0f;
}

[[nodiscard]] constexpr bool ShouldApplyDriverSideOffset(
    const DriverSideEligibility& eligibility) noexcept {
    return ResolveDriverSideBlendFactor(eligibility) > 0.0f;
}

struct PhysicsMoverVtableProfile {
    std::uintptr_t car_physics_mover_rva = 0;
    std::uintptr_t motorcycle_rva = 0;
    std::uintptr_t boat_rva = 0;

    bool operator==(const PhysicsMoverVtableProfile&) const = default;
};

// VehicleSubjectComponent::OnAttach binds the concrete PhysicsMoverInterface
// at subject +0x1A8. Paired constructor evidence establishes these primary
// vtables for the only three concrete mover classes present in both images.
inline constexpr PhysicsMoverVtableProfile kLegacyPhysicsMoverVtables{
    0x017F7388,
    0x017F60A8,
    0x017F7218,
};
inline constexpr PhysicsMoverVtableProfile kLatestSteamPhysicsMoverVtables{
    0x017F7408,
    0x017F6128,
    0x017F7298,
};

[[nodiscard]] constexpr PhysicsMoverVtableProfile SelectPhysicsMoverVtables(
    bool latest_steam_layout) noexcept {
    return latest_steam_layout ? kLatestSteamPhysicsMoverVtables
                               : kLegacyPhysicsMoverVtables;
}

[[nodiscard]] constexpr VehicleClass ClassifyVehicle(
    std::uintptr_t physics_mover_vtable_rva,
    bool latest_steam_layout) noexcept {
    if (physics_mover_vtable_rva == 0) {
        return VehicleClass::Unknown;
    }
    const PhysicsMoverVtableProfile vtables =
        SelectPhysicsMoverVtables(latest_steam_layout);
    if (physics_mover_vtable_rva == vtables.car_physics_mover_rva) {
        return VehicleClass::CarPhysicsMover;
    }
    if (physics_mover_vtable_rva == vtables.motorcycle_rva) {
        return VehicleClass::Motorcycle;
    }
    if (physics_mover_vtable_rva == vtables.boat_rva) {
        return VehicleClass::Boat;
    }
    return VehicleClass::Unknown;
}

[[nodiscard]] constexpr const char* VehicleClassName(
    VehicleClass vehicle_class) noexcept {
    switch (vehicle_class) {
        case VehicleClass::CarPhysicsMover:
            return "car_physics_mover";
        case VehicleClass::Motorcycle:
            return "motorcycle";
        case VehicleClass::Boat:
            return "boat";
        default:
            return "unknown";
    }
}

[[nodiscard]] constexpr std::size_t ProfileSlotOffsetFor(
    std::uintptr_t parameters,
    const std::array<std::uintptr_t, kParameterSlotOffsets.size()>&
        slot_values) noexcept {
    if (parameters == 0) {
        return 0;
    }
    for (std::size_t index = 0; index < slot_values.size(); ++index) {
        if (slot_values[index] == parameters) {
            return kParameterSlotOffsets[index];
        }
    }
    return 0;
}

// On-foot controller auto-centering (disable_controller_camera_autocenter).
//
// RE EVIDENCE (2026-09-03, static scan of both supported executables):
// - The shared angular-approach helper is signature-verified exactly once per
//   layout: legacy 0x181A50 matches kLegacyAngularApproachSignature,
//   latest-steam 0x181AE0 matches kLatestSteamAngularApproachSignature.
// - Each layout has exactly three direct (E8 rel32) callers: the two
//   documented Chase sites (follow-yaw, manual-recenter) plus one on-foot
//   site in a camera update that reads component yaw fields (legacy
//   rdi+0x1934/0x1938/0x193C, 0x19AC/0x19B0/0x19B4, 0x1FF8, 0x200C/0x2018),
//   resolves components through SimObjectGetComponent (0x190AD0), and shows
//   the identical pre/post shape on both layouts (lea rcx,[rbp+0x80] scratch,
//   xmm1=target, xmm2=rate, post-call r14 handling plus an r12b-selected snap
//   `movss [rbp+0x80],xmm11` that bypasses the approach).
// - There is only ONE angular site on-foot (no separate manual-recenter
//   site): legacy call @0x3DC93E (return 0x3DC943), steam call @0x3DC7DE
//   (return 0x3DC7E3). The Chase return RVAs (0x3D0919/0x3D286D and steam
//   equivalents) are Chase-only and MUST NOT be reused here.
// LIMITS (why the detour stays native for now, see TODO there):
// - `current` is stack scratch ([rbp+0x80]), so the Chase back-derivation
//   (component = current - field_offset) is INVALID on this path.
//   Frame-slot recovery (proven 2026-09-05): the current slot is the
//   updater's r8 home slot; [current-0x18] is its return-address slot and
//   [current-0x38] its pushed rdi (= the camera component, sole rdi write
//   `mov rdi,rcx` at the head). See the snap-gate block below.
// - The r12b snap selector is threaded in by the caller; its producer (stick
//   deflection vs aim/lock state) is not proven, so holding (return false)
//   could freeze stick-driven yaw. Do NOT hold until live telemetry proves
//   manual stick motion bypasses this approach site.
// VERDICT 2026-09-03 (OnFootStaticRE): UNRESOLVED. The hold ships as the
// EXPERIMENTAL opt-in disable_onfoot_camera_autocenter (INI
// DisableOnFootCameraAutocenter, default 0); with the flag off the detour
// stays native (fail closed). Revert condition: stick yaw freeze in game =>
// set the flag back to 0.
// Full-function normalized diff: legacy [0x3DC1B0,0x3DCC13) == steam
// [0x3DC050,0x3DCAB3) up to RIP/call-target bytes (82+68+411 insns, 0 mismatches).
// - bl (stick-active) comes from the 0x3C7340 poll of raw axes
//   [rdi+0x1A60/0x1A64], gated by [rdi+0x1F24]/[rdi+0x2030]; al=1 means the
//   stick yaw sits outside the deadband. bl=1 (legacy 0x3DC714/0x3DC720,
//   steam +same-shape): xmm1(target)=[rsp+0x50] stick yaw plus the 0x3DC736
//   atan2 add, xmm2(rate)=[rsp+0x58] stick rate, shaping block skipped.
//   bl=0: target=[rdi+0x201C] (stick-accumulated while [rbp+0x70]!=0),
//   rate=[rdi+0x2018], dwell timer [rdi+0x2020]+=delta vs [rdi+0x2024].
//   Both paths funnel into the ONE site below; the only bypass is the r12b
//   snap overwrite (legacy 0x3DC973 / steam 0x3DC813). [rbp+0x80] has no other
//   writer and is the sole yaw channel downstream (0x3DCAB2/0x3DCB0F).
// - r12b=1 needs [rdi+0x18B1]!=0 AND 0x52A9C0/0x52A990/0x52A8D0 all false
//   (one-shot pending-event consumers on [rdi+0x5D0]+0x328/+0x338/+0x348).
//   Their producers live outside this function ([rdi+0x18B1] has exactly 1
//   ref in all of .text), so r12b==0 with bl==1 is statically reachable:
//   suppressing would freeze stick yaw on those frames (and force r14=0,
//   keeping [rdi+0x200C]/[rdi+0x2014] uncleared at 0x3DCB98). Missing link:
//   whether the input system can set those pending flags while the stick is
//   deflected beyond the 0x3C7340 deadband.
// Cheapest probe: diag_onfoot_yaw_probe_events + steady on-foot right-stick
// yaw; per hit log {stick_rx, pre_current, post_current, target, rate, delta}.
// post==target while rate*delta << |target-pre| on ~all deflected hits =>
// HOLD-SAFE; rate-limited advances => HOLD-UNSAFE.
inline constexpr std::uintptr_t kLegacyOnFootYawApproachReturnRva = 0x003DC943;
inline constexpr std::uintptr_t kLatestSteamOnFootYawApproachReturnRva =
    0x003DC7E3;

[[nodiscard]] constexpr std::uintptr_t SelectOnFootYawApproachReturnRva(
    bool latest_steam_layout) noexcept {
    return latest_steam_layout ? kLatestSteamOnFootYawApproachReturnRva
                               : kLegacyOnFootYawApproachReturnRva;
}
// On-foot snap gate (RE verdict 2026-09-05 round-3, Q3 option 1). Block-3
// of the shared on-foot camera updater stores the full blend-chase
// integrator target into the A50 current slot via `movss [rbp+0x80],
// xmm11` (legacy 0x3DC973 / steam 0x3DC813) whenever the r12b snap
// selector is set, bypassing the shipped angular-approach hold entirely.
// The gate is the 2-byte conditional jump `je +0x0c` (74 0c) at legacy
// 0x3DC971 / steam 0x3DC811; rewriting it to `jmp +0x0c` (eb 0c, same
// rel8) unconditionally skips only the instant snap store. r14b stays 0
// so the postlude takes the not-snapped blend path and recomputes from
// [rbp+0x80]; the blend-init pull remains as the minimal native motion. A
// byte patch cannot read stick state, so the skip is unconditional in
// scope - per the verdict this is acceptable: only the instant snap dies,
// and weight-limited smooth motion survives. The gate block is
// byte-identical and unique in .text per layout
// (round3/verify_patch_anchor.py, q26_out.txt).
//
// The signature spans the 3-byte `test r12b,r12b` immediately above the
// je plus the je, snap store, and `mov r14b,1` (17 bytes total), so the
// patch address is signature_start + kOnFootSnapGateSignatureOffset.
inline constexpr std::array<std::uint8_t, 17> kOnFootSnapGateSignature{
    0x45, 0x84, 0xE4, 0x74, 0x0C, 0xF3, 0x44, 0x0F, 0x11,
    0x9D, 0x80, 0x00, 0x00, 0x00, 0x41, 0xB6, 0x01,
};
inline constexpr std::size_t kOnFootSnapGateSignatureOffset = 3;
inline constexpr std::array<std::uint8_t, 2> kOnFootSnapGateOriginalBytes{
    0x74,
    0x0C,
};
inline constexpr std::array<std::uint8_t, 2> kOnFootSnapGatePatchedBytes{
    0xEB,
    0x0C,
};

[[nodiscard]] constexpr std::uintptr_t SelectOnFootSnapGateRva(
    bool latest_steam_layout) noexcept {
    return latest_steam_layout ? kLatestSteamOnFootSnapGateRva
                               : kLegacyOnFootSnapGateRva;
}

// Caller-frame validation for the on-foot probe's component recovery. The
// A50 current slot ([rbp+0x80] in the updater = the r8 home slot in its
// prologue) sits 0x18 above the updater's return-address slot and 0x38
// above its pushed-rdi slot (rdi = the camera component throughout, sole
// write `mov rdi,rcx` at the head). The expected return addresses are the
// instruction after the outer function's sole call into the updater.
inline constexpr std::uintptr_t kLegacyOnFootBlock3CallerReturnRva = 0x003DA685;
inline constexpr std::uintptr_t kLatestSteamOnFootBlock3CallerReturnRva =
    0x003DA525;

[[nodiscard]] constexpr std::uintptr_t SelectOnFootBlock3CallerReturnRva(
    bool latest_steam_layout) noexcept {
    return latest_steam_layout ? kLatestSteamOnFootBlock3CallerReturnRva
                               : kLegacyOnFootBlock3CallerReturnRva;
}

// On-foot desired-pose finalizer caller census (rounds 76-78, scrapped
// GTAIVOnFootCamera feature): the on-foot finalizer callers return to
// 0x3CFDC9 (consumer ~0x3CFB38) and 0x3D559B; the FollowCamera updater at
// 0x3DC1B0 never calls the finalizer.
// No gate consumes these identities anymore; re-add a predicate only with
// a new feature that needs it.

// Camera-component fields logged by the extended on-foot yaw probe (all
// offsets off the recovered component pointer; RE verdict round-3 Q1/Q2).
inline constexpr std::size_t kOnFootSnapFlagOffset = 0x18B1;
inline constexpr std::size_t kOnFootBlendIntegratorOffset = 0x201C;
inline constexpr std::size_t kOnFootAnchorYawOffset = 0x19B4;
inline constexpr std::size_t kOnFootCamYawOffset = 0x193C;
inline constexpr std::size_t kOnFootBlendWeightOffset = 0x200C;
inline constexpr std::size_t kOnFootSpeedOffset = 0x1A2C;
// Live yaw source re-committed into the cam pose by the dc1b0 block-3
// postlude every frame ([0x193c] = [0x19ac] + D*xmm13, xmm13 proven 0).
inline constexpr std::size_t kOnFootLiveYawOffset = 0x19AC;

// Blend-chase hold (RE verdict round-4 + live-session adjudication). The
// on-foot recenter after the A50 hold + snap-gate patch is the blend-chase
// function (0x3c9fa0 legacy / 0x3c9e40 steam): its per-frame call sites in
// the on-foot outer fn advance the look-target triple
// comp+0x19ac/0x19b0/0x19b4 toward the raw stick direction with a
// dwell-ramped weight, and the dc1b0 postlude re-commits that triple into
// the camera pose every frame. With the controller resting at a ~1%
// deflection the game treats the stick as active, so the camera keeps
// easing toward the rest direction even with the A50 approach held.
// Live-session evidence (2026-09-06): the active sites are the
// movement-mode selector (legacy call 0x3da4b7, modes 0/1) and the
// entity-gated site (legacy 0x3da44c, mode 11); the mode-7 fallback never
// ran. The detour skips those three sites when the OS right stick sits
// inside the deadband. A left-stick ("standing still") gate was tried and
// removed: XInput reported the left stick near full forward while the
// character stood still, blocking every skip. Walking follow does not
// depend on this gate - the outer fn's speed-gated W2 writer handles it.
// Manual look (>deadband) and aim/lock (mode 4) stay native. Fields on
// blend-chase's own component are NOT the stick axes/speed (live probe:
// comp+0x1a60 magnitude constant 1.0, comp+0x1a2c constant 0 - different
// component layout than the dc1b0 updater), so the decision uses OS
// XInput state.
inline constexpr std::uintptr_t kLegacyBlendChaseRva = 0x003C9FA0;
inline constexpr std::uintptr_t kLatestSteamBlendChaseRva = 0x003C9E40;
// Return addresses (call site + 5) of the three gated call sites.
inline constexpr std::uintptr_t kLegacyOnFootBlendChaseMode7ReturnRva =
    0x003D9F7D;
inline constexpr std::uintptr_t kLatestSteamOnFootBlendChaseMode7ReturnRva =
    0x003D9E1D;
inline constexpr std::uintptr_t kLegacyOnFootBlendChaseSelectorReturnRva =
    0x003DA4BC;
inline constexpr std::uintptr_t
    kLatestSteamOnFootBlendChaseSelectorReturnRva = 0x003DA35C;
inline constexpr std::uintptr_t kLegacyOnFootBlendChaseEntityReturnRva =
    0x003DA451;
inline constexpr std::uintptr_t kLatestSteamOnFootBlendChaseEntityReturnRva =
    0x003DA2F1;

// Blend-chase prologue, byte-identical in both layouts and unique in .text
// (verified round-4: legacy 0x3c9fa0, steam 0x3c9e40).
inline constexpr std::array<std::uint8_t, 16> kBlendChaseHeadSignature{
    0x48, 0x8B, 0xC4, 0x44, 0x88, 0x40, 0x18, 0x53,
    0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41,
};

// OS XInput deadband (raw int16 units, full scale 32768). Controller rest
// drift observed in live captures is 60-830 raw (~0.3-2.5%); 1500 (~4.6%)
// stays above it and keeps the manual-look onset delay small, so only
// genuinely idle sticks suppress the chase.
inline constexpr float kOnFootBlendChaseStickDeadband = 1500.0f;

// Speed-follow writer (W2, round-2 verdict "cam W2"): inline code in the
// on-foot outer fn that adds the speed-proportional yaw step
// (cam_yaw += k*dt*clamp(speed)) - the canonical walking/running
// auto-center. Live adjudication round-5: block-3 and the chase A50 sites
// do not run while moving (probe: zero hits), and blend-chase is skipped on
// neutral sticks, yet the walking recenter persisted - W2 is the remaining
// speed-driven yaw writer. The patch NOPs the 4-byte `addss xmm1, xmm0`
// step (the write-back then stores the unchanged yaw). W1 (position
// follow) and all other pose elements are untouched. The 24-byte signature
// is byte-identical and unique in .text per layout.
inline constexpr std::uintptr_t kLegacyOnFootSpeedFollowPatchRva = 0x003DA564;
inline constexpr std::uintptr_t kLatestSteamOnFootSpeedFollowPatchRva =
    0x003DA404;
inline constexpr std::array<std::uint8_t, 24> kOnFootSpeedFollowSignature{
    0xF3, 0x0F, 0x10, 0x8F, 0x3C, 0x19, 0x00, 0x00,
    0xF3, 0x0F, 0x58, 0xC8,
    0xF3, 0x41, 0x0F, 0x11, 0x5F, 0x54,
    0xF3, 0x41, 0x0F, 0x11, 0x57, 0x58,
};
inline constexpr std::size_t kOnFootSpeedFollowSignatureOffset = 8;
inline constexpr std::array<std::uint8_t, 4> kOnFootSpeedFollowOriginalBytes{
    0xF3, 0x0F, 0x58, 0xC8,
};
inline constexpr std::array<std::uint8_t, 4> kOnFootSpeedFollowPatchedBytes{
    0x0F, 0x1F, 0x40, 0x00,
};

[[nodiscard]] constexpr std::uintptr_t SelectOnFootSpeedFollowPatchRva(
    bool latest_steam_layout) noexcept {
    return latest_steam_layout ? kLatestSteamOnFootSpeedFollowPatchRva
                               : kLegacyOnFootSpeedFollowPatchRva;
}

// Anchor-advance writer (round-2 verdict E4): inline code in the on-foot
// outer fn that adds a speed-driven step to the look-target triple's third
// element (comp+0x19b4) every frame - the follow-anchor drift that drags
// the camera behind the player while moving. Live adjudication round-5:
// with W2 patched the walking recenter persisted, leaving this as the only
// remaining speed-driven writer. The patch NOPs the 4-byte
// `addss xmm1, xmm0` step; the store then rewrites the unchanged value.
// The 46-byte signature is byte-identical and unique in .text per layout.
inline constexpr std::uintptr_t kLegacyOnFootAnchorAdvancePatchRva =
    0x003DA62B;
inline constexpr std::uintptr_t kLatestSteamOnFootAnchorAdvancePatchRva =
    0x003DA4CB;
inline constexpr std::array<std::uint8_t, 46> kOnFootAnchorAdvanceSignature{
    0xF3, 0x0F, 0x10, 0x97, 0xAC, 0x19, 0x00, 0x00,
    0xF3, 0x0F, 0x10, 0x9F, 0xB0, 0x19, 0x00, 0x00,
    0xF3, 0x0F, 0x10, 0x8F, 0xB4, 0x19, 0x00, 0x00,
    0xF3, 0x0F, 0x58, 0xC8,
    0xF3, 0x0F, 0x10, 0x87, 0x98, 0x1A, 0x00, 0x00,
    0x0F, 0x2F, 0xC1,
    0x77, 0x03,
    0x0F, 0x28, 0xC1,
    0xF3, 0x0F,
};
inline constexpr std::size_t kOnFootAnchorAdvanceSignatureOffset = 0x18;
inline constexpr std::array<std::uint8_t, 4>
    kOnFootAnchorAdvanceOriginalBytes{0xF3, 0x0F, 0x58, 0xC8};
inline constexpr std::array<std::uint8_t, 4>
    kOnFootAnchorAdvancePatchedBytes{0x0F, 0x1F, 0x40, 0x00};

[[nodiscard]] constexpr std::uintptr_t SelectOnFootAnchorAdvancePatchRva(
    bool latest_steam_layout) noexcept {
    return latest_steam_layout ? kLatestSteamOnFootAnchorAdvancePatchRva
                               : kLegacyOnFootAnchorAdvancePatchRva;
}

// W1 yaw-delta writer (round-2 census "cam W1", live-adjudicated round-5):
// while moving, the on-foot outer fn recomputes the follow point every
// frame and blends the camera pose toward it; the yaw element (comp+0x193c)
// receives that delta at the `addss xmm0, xmm7` below - the residual
// walking auto-center after W2 and the anchor advance were patched (live
// 10 Hz probe: cam drifting -0.4/s while walking with the stick at rest,
// exactly the user-perceived rate). The patch NOPs that 4-byte add; the
// other two pose elements (position follow) still update. The 28-byte
// signature is byte-identical and unique in .text per layout.
inline constexpr std::uintptr_t kLegacyOnFootW1YawPatchRva = 0x003D948A;
inline constexpr std::uintptr_t kLatestSteamOnFootW1YawPatchRva = 0x003D932A;
inline constexpr std::array<std::uint8_t, 28> kOnFootW1YawSignature{
    0xF3, 0x0F, 0x10, 0x97, 0x34, 0x19, 0x00, 0x00,
    0xF3, 0x0F, 0x10, 0x8F, 0x38, 0x19, 0x00, 0x00,
    0xF3, 0x0F, 0x10, 0x87, 0x3C, 0x19, 0x00, 0x00,
    0xF3, 0x0F, 0x58, 0xC7,
};
inline constexpr std::size_t kOnFootW1YawSignatureOffset = 0x18;
inline constexpr std::array<std::uint8_t, 4> kOnFootW1YawOriginalBytes{
    0xF3, 0x0F, 0x58, 0xC7,
};
inline constexpr std::array<std::uint8_t, 4> kOnFootW1YawPatchedBytes{
    0x0F, 0x1F, 0x40, 0x00,
};

[[nodiscard]] constexpr std::uintptr_t SelectOnFootW1YawPatchRva(
    bool latest_steam_layout) noexcept {
    return latest_steam_layout ? kLatestSteamOnFootW1YawPatchRva
                               : kLegacyOnFootW1YawPatchRva;
}

// Auto-follow pose-set store (round-5 setter-probe adjudication): inside the
// on-foot outer fn, the cam pose triple is set directly from a
// character-attached point (r13+0x244..24c) behind a dwell timer
// ([comp+0x1f90] accumulates while moving) - the rendered-camera walking
// auto-center that survived the W1/W2/anchor-advance patches. The patch
// NOPs only the third store (the yaw element, comp+0x193c <- [r13+0x24c]);
// the first two stores (position elements) still update, so position
// follow is preserved. The 22-byte signature is byte-identical and unique
// in .text per layout.
inline constexpr std::uintptr_t kLegacyOnFootAutoFollowStoreRva = 0x003DA1B4;
inline constexpr std::uintptr_t kLatestSteamOnFootAutoFollowStoreRva =
    0x003DA054;
inline constexpr std::array<std::uint8_t, 22> kOnFootAutoFollowStoreSignature{
    0xF3, 0x45, 0x0F, 0x11, 0x57, 0x54,
    0xF3, 0x45, 0x0F, 0x11, 0x5F, 0x58,
    0xF3, 0x45, 0x0F, 0x11, 0x67, 0x5C,
    0x49, 0x8B, 0xCF, 0xE8,
};
inline constexpr std::size_t kOnFootAutoFollowStoreSignatureOffset = 12;
inline constexpr std::array<std::uint8_t, 6>
    kOnFootAutoFollowStoreOriginalBytes{0xF3, 0x45, 0x0F, 0x11, 0x67, 0x5C};
inline constexpr std::array<std::uint8_t, 6>
    kOnFootAutoFollowStorePatchedBytes{0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};

[[nodiscard]] constexpr std::uintptr_t SelectOnFootAutoFollowStoreRva(
    bool latest_steam_layout) noexcept {
    return latest_steam_layout ? kLatestSteamOnFootAutoFollowStoreRva
                               : kLegacyOnFootAutoFollowStoreRva;
}

[[nodiscard]] constexpr std::uintptr_t SelectBlendChaseRva(
    bool latest_steam_layout) noexcept {
    return latest_steam_layout ? kLatestSteamBlendChaseRva
                               : kLegacyBlendChaseRva;
}

[[nodiscard]] constexpr bool IsOnFootBlendChaseGatedReturn(
    std::uintptr_t return_rva, bool latest_steam_layout) noexcept {
    return return_rva == (latest_steam_layout
                              ? kLatestSteamOnFootBlendChaseMode7ReturnRva
                              : kLegacyOnFootBlendChaseMode7ReturnRva) ||
           return_rva == (latest_steam_layout
                              ? kLatestSteamOnFootBlendChaseSelectorReturnRva
                              : kLegacyOnFootBlendChaseSelectorReturnRva) ||
           return_rva == (latest_steam_layout
                              ? kLatestSteamOnFootBlendChaseEntityReturnRva
                              : kLegacyOnFootBlendChaseEntityReturnRva);
}

}  // namespace spatch::vehicle_camera
