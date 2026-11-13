#include "ArchiveEntryPolicy.h"
#include "AverageWindowPolicy.h"
#include "CharacterEyeFix.h"
#include "CharacterHookPolicy.h"
#include "CharacterSurfaceTelemetryPolicy.h"
#include "CharacterSweatPolicy.h"
#include "CharacterWetnessPolicy.h"
#include "ChunkStreamPolicy.h"
#include "CutContentProbe.h"
#include "CutsceneTiming.h"
#include "DisplaySettings.h"
#include "EngineFixes.h"
#include "FogRestorationPolicy.h"
#include "FogSlicingPolicy.h"
#include "GTAVSpeedShakePolicy.h"
#include "HooksSummary.h"
#include "HookTargetGuard.h"
#include "Logger.h"
#include "NisActorProbe.h"
#include "PedestrianTiming.h"
#include "PushVoiceLinePolicy.h"
#include "QFileIoPolicy.h"
#include "QcmpPolicy.h"
#include "RuntimePatch.h"
#include "WeiVoiceLinePolicy.h"
#include "AgxRuntime.h"
#include "SmaaRuntime.h"
#include "StimulusInitEmitterProbe.h"
#include "TextureFilteringPolicy.h"
#include "VehicleBeamLightPolicy.h"
#include "VehicleCameraPolicy.h"
#include "VehicleLightShadowPolicy.h"

#include <Windows.h>

#include <MinHook.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace spatch::hooks {
namespace {

// Final end-user builds intentionally disable summaries and verbose probes.
// Keeping hundreds of diagnostic snapshots as real atomics in that build
// still imposed locked read/modify/write instructions on render and NIS hot
// paths. Development builds retain the counters with relaxed ordering; final
// builds compile their operations away entirely.
template <typename T>
class DiagnosticAtomic {
public:
#if defined(SPATCH_FINAL_RELEASE)
    constexpr DiagnosticAtomic(T = {}) noexcept {}

    T fetch_add(T, std::memory_order = std::memory_order_relaxed) noexcept {
        return {};
    }
    T fetch_sub(T, std::memory_order = std::memory_order_relaxed) noexcept {
        return {};
    }
    [[nodiscard]] T load(std::memory_order = std::memory_order_relaxed) const noexcept {
        return {};
    }
    void store(T, std::memory_order = std::memory_order_relaxed) noexcept {}
    bool compare_exchange_strong(T& expected,
                                 T,
                                 std::memory_order = std::memory_order_relaxed) noexcept {
        expected = {};
        // Final builds intentionally have no diagnostic storage.  Report a
        // failed exchange rather than pretending a write happened; callers
        // use this primitive only for diagnostic one-shot gates, and the
        // false result preserves the safe no-op semantics of the other
        // operations in this specialization.
        return false;
    }
#else
    constexpr DiagnosticAtomic(T initial = {}) noexcept : value_(initial) {}

    T fetch_add(T operand, std::memory_order order = std::memory_order_relaxed) noexcept {
        return value_.fetch_add(operand, order);
    }
    T fetch_sub(T operand, std::memory_order order = std::memory_order_relaxed) noexcept {
        return value_.fetch_sub(operand, order);
    }
    [[nodiscard]] T load(std::memory_order order = std::memory_order_relaxed) const noexcept {
        return value_.load(order);
    }
    void store(T desired, std::memory_order order = std::memory_order_relaxed) noexcept {
        value_.store(desired, order);
    }
    bool compare_exchange_strong(T& expected,
                                 T desired,
                                 std::memory_order order = std::memory_order_relaxed) noexcept {
        return value_.compare_exchange_strong(expected, desired, order);
    }

private:
    std::atomic<T> value_;
#endif
};

constexpr std::uintptr_t kTaskReadyRva = 0x00A390C0;
constexpr std::uintptr_t kTaskDispatchRva = 0x00A37EC0;
constexpr std::uintptr_t kWaitHelperRva = 0x00A395C0;
constexpr std::uintptr_t kScaleformTimeRva = 0x0098DC90;
constexpr std::uintptr_t kScaleformInitRva = 0x009928B0;
constexpr std::uintptr_t kNisSetPlayTimeRva = 0x003E51B0;
constexpr std::uintptr_t kNisPlayRva = 0x003E3C40;
constexpr std::uintptr_t kNisBootstrapRva = 0x003E3B60;
constexpr std::uintptr_t kNisOwnerRva = 0x003E6CE0;
constexpr std::uintptr_t kNisActorSetupRva = 0x003E8050;
constexpr std::uintptr_t kNisActorRestoreRva = 0x003E7A80;
constexpr std::uintptr_t kTwitchStateTickRva = 0x000819D0;
constexpr std::uintptr_t kTwitchLoginCallbackRva = 0x00081CB0;
// Doc-295 combined probe targets (legacy_researched only).  The window
// procedure allocates one Main Pool input node per handled message and
// deduplicates mouse positions only after those allocations; the OS game-url
// probe performs a blocking WinINet request at init; the per-frame component
// update advances the doc-292 activity accumulator at +0x1E5C by a fixed
// 0.1 per call.
constexpr std::uintptr_t kPass8WndProcRva = 0x00A3BBC0;
constexpr std::uintptr_t kPass8OsGameUrlProbeRva = 0x00A37040;
constexpr std::uintptr_t kPass8ActivityTimerUpdateRva = 0x003DE0F0;
constexpr std::uintptr_t kAnalogDriverApplyRva = 0x004839A0;
constexpr std::uintptr_t kTransmissionShiftApplyRva = 0x004895E0;
constexpr std::uintptr_t kTransmissionRatioApplyRva = 0x004879D0;
constexpr std::uintptr_t kVehicleEngineApplyRva = 0x00483A60;
// Steering probe seams (SDHandlingIV T049, audited on this exe).
constexpr std::uintptr_t kSteeringCallbackRva = 0x00483DE0;
constexpr std::uintptr_t kSteeringInputRva = 0x004840C0;
// Latest-Steam drivetrain/steering remap (2026-10-31, needed by
// ModernBrakeReverse + NoWheelTurnBack): the whole TU shifted +0x1A0.
// shift/engine/callback/input byte-match at +0x1A0; ratio and analog driver
// come from the drivetrain vtable slots (13/14/19/23, legacy base 0x1799738
// -> latest 0x1799938, same relative slots; ratio's only text divergence is
// its rip lookup table moving with the same +0x1A0 shift).
constexpr std::uintptr_t kLatestSteamAnalogDriverApplyRva = 0x00483B40;
constexpr std::uintptr_t kLatestSteamTransmissionShiftApplyRva = 0x00489780;
constexpr std::uintptr_t kLatestSteamTransmissionRatioApplyRva = 0x00487B70;
constexpr std::uintptr_t kLatestSteamVehicleEngineApplyRva = 0x00483C00;
constexpr std::uintptr_t kLatestSteamSteeringCallbackRva = 0x00483F80;
constexpr std::uintptr_t kLatestSteamSteeringInputRva = 0x00484260;
constexpr std::uintptr_t kTimeDilateApplyRva = 0x00301340;
// Latest-Steam remap (2026-10-31): 40-instruction lockstep match at the new
// address; needed for DisableDrivebySlowMo / DisableStuntSlowMo there.
constexpr std::uintptr_t kLatestSteamTimeDilateApplyRva = 0x00300F80;
// Time-of-day CLOCK update (GTAVITimeClock), re-verified 2026-10-31: 45-instruction
// lockstep match at the latest address, all five rate constants identical
// in both builds. Not to be confused with kTimeOfDayAccessorRva - this is
// the per-frame advance, that is the read accessor.
constexpr std::uintptr_t kTimeOfDayClockUpdateRva = 0x006BCD0;
constexpr std::uintptr_t kLatestSteamTimeOfDayClockUpdateRva = 0x006BFD0;
// Prologue is rip-reference-free up to 0x6BCE7, identical in both builds.
constexpr std::array<std::uint8_t, 23> kTimeOfDayClockUpdateSignature{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
    0x89, 0x78, 0x18, 0x55, 0x48, 0x8D, 0x68, 0xA1,
    0x48, 0x81, 0xEC, 0xE0, 0x00, 0x00, 0x00};
// ModernBrakeReverse constants; the latch state and helpers live with the
// detours below (after SafeRead/ResolveAddress are declared).
// Signed mover speed above which a held reverse key counts as "braking from
// forward motion" (m/s).
constexpr float kBrakeReverseArmSpeedMps = 1.0f;
// Driver-output service brake above which the brake pedal counts as held.
constexpr float kBrakeReverseBrakeHeldThreshold = 0.05f;
constexpr std::uintptr_t kFrameFlowRva = 0x00590AD0;
constexpr std::uintptr_t kCutsceneFlowOwnerRva = 0x00412240;
constexpr std::uintptr_t kScaleformTimeProviderRva = 0x02441150;
constexpr std::uintptr_t kFogSlicingModeRva = 0x0006BA60;
constexpr std::uintptr_t kAntiAliasOwnerRva = 0x00053230;
constexpr std::uintptr_t kAntiAliasFxHandoffRva = 0x000554E0;
constexpr std::uintptr_t kAntiAliasAuxStateQueryRva = 0x00008A00;
constexpr std::uintptr_t kRenderSubmitMaterialRva = 0x0000E880;
constexpr std::uintptr_t kPresentBufferRva = 0x00053A30;
constexpr std::uintptr_t kRumbleApplyHelperRva = 0x0014C170;
constexpr std::uintptr_t kRumbleApplyObjectRva = 0x02175EB0;
constexpr std::uintptr_t kCharacterHandleWaterCollisionRva = 0x00535140;
constexpr std::uintptr_t kCharacterEffectsUpdateRva = 0x00559AF0;
// SDK-backed velocity/combat helpers. These remain legacy-only; the later
// Steam layout is fail-closed until both corresponding helpers are mapped.
constexpr std::uintptr_t kCharacterPhysicsGetVelocityRva = 0x00468150;
constexpr std::uintptr_t kUiIsPlayerInCombatRva = 0x005EE680;
// Latest-Steam remaps (wave 2, 2026-10-30 - site-evidence-wave2.md).
// Paired disassembly/decompilation proves this read-only occupant query on both
// supported layouts. It is signature-verified before velocity can count as
// running exertion.
constexpr std::uintptr_t kIsCharacterInVehicleRva = 0x00539A60;
constexpr std::uintptr_t kHealthApplyDamageRva = 0x00521F60;
// NOT a `bool()` function. See ReadSimTimePausedSafely() for the disassembly:
// the routine at this RVA takes an OUT pointer in RDX and performs an 8-byte
// store through it. It is only used here as a layout fingerprint.
constexpr std::uintptr_t kSimTimeIsPausedRva = 0x00510980;
// The two globals that routine branches on, read directly instead of calling it.
constexpr std::uintptr_t kSimTimePauseGateRva = 0x0203C95C;
constexpr std::uintptr_t kSimTimeScaleRva = 0x0203C970;
constexpr std::uintptr_t kUiIsGamePausedRva = 0x005EE1B0;
constexpr std::uintptr_t kCharacterPaintOwnerRva = 0x004CF1E0;
constexpr std::uintptr_t kCharacterPaintConsumerRva = 0x00509200;
constexpr std::uintptr_t kCharacterEffectDispatchOwnerRva = 0x00400930;
constexpr std::uintptr_t kCharacterEffectDispatchConsumeRva = 0x00402CB0;
constexpr std::uintptr_t kCharacterEffectQueueBuilderRva = 0x003FE840;
constexpr std::uintptr_t kCharacterAnimationApplyCharredEffectRva = 0x0057F270;
constexpr std::uintptr_t kCharacterAnimationApplyPaintEffectRva = 0x0057F280;
constexpr std::uintptr_t kCharacterAnimationCreateDamageRigRva = 0x00580570;
constexpr std::uintptr_t kDamageRigApplyCharredEffectRva = 0x003A1EA0;
constexpr std::uintptr_t kDamageRigApplyPaintEffectRva = 0x003A2430;
constexpr std::uintptr_t kCharacterDamageRigResetDamageRva = 0x003AE110;
constexpr std::uintptr_t kSimObjectGetComponentRva = 0x00190AD0;
constexpr std::uintptr_t kTimeOfDayAccessorRva = 0x0006AA70;
constexpr std::uintptr_t kWetSurfaceBlockCounterRva = 0x0242E578;
constexpr std::uintptr_t kSceneryPrepareRva = 0x0005B190;
constexpr std::uintptr_t kScenerySetupRva = 0x0005C400;
constexpr std::uintptr_t kRenderSceneryBuilderRva = 0x0005BC60;
constexpr std::uintptr_t kRasterizeBucketBuilderRva = 0x000262B0;
constexpr std::uintptr_t kRenderTaskManagerRva = 0x0235B160;
constexpr std::uintptr_t kRenderContextInstanceRva = 0x021299D8;
constexpr std::uintptr_t kSceneryCounter0Rva = 0x021363C0;
constexpr std::uintptr_t kSceneryCounter1Rva = 0x021363D0;
constexpr std::uintptr_t kSceneryCounter2Rva = 0x021363E0;
constexpr std::uintptr_t kSceneryCounter3Rva = 0x021363F0;
constexpr std::uintptr_t kD3DDeviceSlotRva = 0x02439AE0;
constexpr std::uintptr_t kD3DContextSlotRva = 0x02439AE8;
constexpr std::uintptr_t kDxgiSwapChainSlotRva = 0x02439B10;
constexpr std::uintptr_t kPresentRtvSlotRva = 0x02439B18;
constexpr std::uintptr_t kPedestrianFrameRateThrottleRva = 0x0040B960;
constexpr std::uintptr_t kPedestrianSpawnUpdateRva = 0x00418180;

// Stimulus::Init (SDK ai/stimulus.hh:25, SDK_CALL_FUNC target 0x366490).
// Legacy_researched only. Captured here because it is the one call site where
// the engine hands over the emitter directly: the emitter is a
// qNode<StimulusEmitterComponent, StimulusManagerUpdateList>, so it registers
// with the central manager update list and never appears on a SimObject.
constexpr std::uintptr_t kStimulusInitRva = 0x00366490;
// Push-bark restoration (doc 306 / round68): the DE still contains the full
// speech chain but never triggers it. The dispatcher is the shared entry for
// conversation elements; the speech wrapper queues the audible line; the
// name-to-id resolver and the player voice id global feed the invocation.
// Latest-Steam layout remapped 2026-10-30 (paired lockstep disassembly);
// resolved through RemapRvaForCurrentBuild below.
constexpr std::uintptr_t kConversationDispatcherRva = 0x005A6450;
constexpr std::uintptr_t kSpeechWrapperRva = 0x005A8E20;
constexpr std::uintptr_t kLatestSteamConversationDispatcherRva = 0x005A6690;
constexpr std::uintptr_t kLatestSteamSpeechWrapperRva = 0x005A9060;
// Speech-by-name entry (0x5A8930): resolves the voice name itself, so SPatch
// does not depend on the by-id argument order (ledger sec 36).
constexpr std::uintptr_t kSpeakByNameRva = 0x005A8930;
constexpr std::uintptr_t kLatestSteamSpeakByNameRva = 0x005A8B70;
// Wave-2 remaps (2026-10-30, site-evidence-wave2.md): HUD/radar, heat meter,
// scaleform, DOF, activity timer, sweat inputs, stock-AA hooks. The .data
// globals the radar/slow-mo paths touch (minimap visible, hud screen pointer,
// slow-mo scale/freeze/request) are UNMOVED in the latest layout, so they need
// no remap cases.
constexpr std::uintptr_t kHudUiApplyRva = 0x05E57D0;
constexpr std::uintptr_t kHeatMeterApplyRva = 0x614250;
constexpr std::uintptr_t kGfxSetVariableRva = 0x90E8B0;
constexpr std::uintptr_t kDofSetParametersRva = 0x009300;
constexpr std::uintptr_t kHudHealthBarSetterRva = 0x05E1A40;
// Both direct callers agree (0x614202/0x632168 -> same target).
constexpr std::uintptr_t kLatestSteamHudHealthBarSetterRva = 0x05E1BE0;
constexpr std::uintptr_t kLatestSteamHudUiApplyRva = 0x05E5970;
constexpr std::uintptr_t kLatestSteamHeatMeterApplyRva = 0x6143F0;
constexpr std::uintptr_t kLatestSteamGfxSetVariableRva = 0x90EB90;
constexpr std::uintptr_t kLatestSteamScaleformTimeRva = 0x0098E000;
constexpr std::uintptr_t kLatestSteamScaleformInitRva = 0x00992BF0;
constexpr std::uintptr_t kLatestSteamDofSetParametersRva = 0x009360;
constexpr std::uintptr_t kLatestSteamPass8ActivityTimerUpdateRva = 0x003DDF90;
constexpr std::uintptr_t kLatestSteamCharacterPhysicsGetVelocityRva =
    0x00468410;
constexpr std::uintptr_t kLatestSteamUiIsPlayerInCombatRva = 0x005EE820;
constexpr std::uintptr_t kLatestSteamAntiAliasOwnerRva = 0x00053540;
constexpr std::uintptr_t kLatestSteamAntiAliasFxHandoffRva = 0x000557F0;
constexpr std::array<std::uint8_t, 11> kLatestSteamUiIsPlayerInCombatSignature{
    0x48, 0x8B, 0x0D, 0x29, 0xB6, 0xDC, 0x01, 0x33, 0xD2, 0xE9, 0x32};
// ActorAudioComponent vftable, CONFIRMED AT RUNTIME (sec 38).
//
// The live component scan of the player SimObject logged every component's vftable
// RVA, and index 40 carried 0x17C9FD8. Earlier in this session 0x17C9FD8 was
// rejected as "ASCII text" - that was MY error: I read it at file-0xC00, the .text
// delta, when .rdata needs its own section mapping. Read correctly it is a vftable
// whose first entry is 0x140594DE0. Runtime evidence outranks the static misread.
//
// A SECOND of the reported vftables (0x17CA058) was NOT present on the player, so it
// is not used here.
constexpr std::uintptr_t kActorAudioComponentVftableRva = 0x17C9FD8;
// Latest-Steam layout: RUNTIME-PROVEN 2026-10-30 - the game's own by-name
// speech traffic (DetourSpeakByName vftable_rva logging, GoG boot 13:49)
// carries 0x17CA0E8 on every speak call (ANECDOTE/NON_COMMIT reactions),
// so that is the ActorAudioComponent vftable on the latest layout. A
// construction-site reading (0x17C9FB8) turned out to be a different
// audio-component class's vtables: paired static evidence lost to runtime
// ground truth here. See site-evidence-wave2.md.
constexpr std::uintptr_t kLatestSteamActorAudioComponentVftableRva =
    0x17CA0E8;

// There is deliberately NO ActorAudioComponent type uid constant here.
//
// MEASURED 2026-08-28 (live component_uid_scan of the player SimObject, 60 entries):
// the value 0xB1000003u (tag 0x58) DOES NOT EXIST among the player's components and
// was never anything but an inference. The scan is trustworthy because it does
// contain 0x3D (physics 0x7A000001), 0x54 (hitreact 0xA8000001), 0x63
// (anim 0xC6000003) and 0x66 (charlook 0xCC000001) - all four known-good uids.
// A separate static sweep found component type uids are assigned at RUNTIME and
// never stored as constants anywhere in the binary, so there is nothing to recover.
// Resolution uses the guard-based probe in ResolvePlayerAudioComponent instead.
constexpr std::uintptr_t kAudioNameToIdRva = 0x00143890;
constexpr std::uintptr_t kLatestSteamAudioNameToIdRva = 0x001436B0;
// CORRECTED 2026-08-28 (ledger sec 31): the ledger recorded 0x2359A6C, but that
// address has ZERO rip-relative code references in the DE binary, so it is not a
// game variable. The engine player-id accessor 0x181140 (lea rax,[rip+...]; ret)
// returns 0x235AA6C, which has three code references and is exactly the value the
// conversation dispatcher compares [element+0x18] against to recognise the player.
// Reading 0x2359A6C fed a garbage voice id into the speech wrapper.
constexpr std::uintptr_t kPlayerVoiceIdGlobalRva = 0x0235AA6C;

// Player SimObject singleton. HealthApplyDamage (0x521F60) compares its
// `attacker` argument against exactly this slot four instructions into the
// prologue (0x521FA9 call 0x360C20 -> mov rax,[rip+...] = *0x235C488;
// 0x521FAE cmp rdi,rax), so the game already performs the player-as-attacker
// test for us. 144 verified rip-relative readers in .text confirm it is the
// canonical player singleton (ledger sec 37). Reading it directly is more
// reliable than any per-object id field.
constexpr std::uintptr_t kPlayerSimObjectGlobalRva = 0x0235C488;
// Unmoved in the latest layout (rip-relative vote from the anchors,
// site-evidence-wave1.md).
constexpr std::uintptr_t kLatestSteamPlayerSimObjectGlobalRva = 0x0235C488;
// ScriptedConversationElement entry twin (vtable slot 3 of the derived class).
// The conversation system drives THIS virtual, not the base dispatcher: the
// twin forwards to kConversationDispatcherRva only when [element+0xa8]==0 and
// otherwise diverts to the subtitle wrapper, so a detector placed solely on
// the base dispatcher never observes scripted push elements (round68 sec 27).
constexpr std::uintptr_t kScriptedConversationEntryRva = 0x005A66D0;
constexpr std::uintptr_t kLatestSteamScriptedConversationEntryRva = 0x005A6910;
// ConversationElement field offsets (legacy layout, doc 306 / round68
// ledger sections 14-15; paired disasm evidence in the ledger).
constexpr std::uintptr_t kElementReactionTypeOffset = 0x98;
constexpr std::uintptr_t kElementActionNameOffset = 0x38;
constexpr std::uintptr_t kElementTargetComponentOffset = 0x68;
// Speech target component guard offsets (ledger section 15).
constexpr std::uintptr_t kSpeechGuardEnabledOffset = 0x168;
constexpr std::uintptr_t kSpeechGuardCountOffset = 0x1BC;
constexpr std::uintptr_t kSpeechGuardObjectOffset = 0x190;
constexpr std::uintptr_t kSpeechGuardThresholdOffset = 0x1A8;
// Speech-wrapper guard offsets (0x5A8E20 / 0x5A8930), reused for the signature probe.
constexpr std::uintptr_t kSpeechEnabledOffset = 0x168;
constexpr std::uintptr_t kSpeechVoiceCountOffset = 0x1BC;
constexpr std::uintptr_t kSpeechGuardFlagsOffset = 0x228;
constexpr std::uintptr_t kAverageWindowInitializeRva = 0x0017CFC0;
constexpr std::uintptr_t kMaterialOnLoadRva = 0x000934A0;
constexpr std::uintptr_t kPcFileReadRva = 0x00A38290;
constexpr std::uintptr_t kPcFileSeekRva = 0x00A382D0;
constexpr std::uintptr_t kPcFileTellRva = 0x00A38840;
constexpr std::uintptr_t kPcFileSizeRva = 0x00A38890;
constexpr std::uintptr_t kQFileReadAtRva = 0x001896B0;
constexpr std::uintptr_t kQFileWriteAtRva = 0x0018CE50;
constexpr std::uintptr_t kQFileReadyRva = 0x0018CA00;
constexpr std::uintptr_t kQcmpDecompressRva = 0x00183080;
constexpr std::uintptr_t kStreamFileOpenRva = 0x0022BBD0;
constexpr std::uintptr_t kStreamFileCloseRva = 0x00228530;
constexpr std::uintptr_t kResourceChunkDispatchRva = 0x00176B50;
constexpr unsigned long kWaitLogThresholdMs = 16;
constexpr unsigned long kWaitLongThresholdMs = 100;
constexpr unsigned long kWaitVeryLongThresholdMs = 1000;
constexpr std::uintptr_t kLatestSteamNisSetPlayTimeRva = 0x003E50C0;
constexpr std::uintptr_t kLatestSteamNisOwnerRva = 0x003E6BF0;
constexpr std::uintptr_t kLatestSteamFrameFlowRva = 0x00590D30;
constexpr std::uintptr_t kLatestSteamCutsceneFlowOwnerRva = 0x00412490;
constexpr std::uintptr_t kLatestSteamFogSlicingModeRva = 0x0006BD70;
constexpr std::uintptr_t kLatestSteamPresentBufferRva = 0x00053D40;
constexpr std::uintptr_t kLatestSteamHealthApplyDamageRva = 0x005221C0;
constexpr std::uintptr_t kLatestSteamSimObjectGetComponentRva = 0x00190A60;
constexpr std::uintptr_t kLatestSteamTimeOfDayAccessorRva = 0x0006AD80;
constexpr std::uintptr_t kLatestSteamCharacterHandleWaterCollisionRva = 0x005353A0;
constexpr std::uintptr_t kLatestSteamCharacterEffectsUpdateRva = 0x00559D50;
constexpr std::uintptr_t kLatestSteamCharacterEffectDispatchConsumeRva = 0x00402E30;
constexpr std::uintptr_t kLatestSteamCharacterEffectQueueBuilderRva = 0x003FE900;
constexpr std::uintptr_t kLatestSteamDamageRigApplyPaintEffectRva = 0x003A2220;
constexpr std::uintptr_t kLatestSteamCharacterDamageRigResetDamageRva = 0x003ADF30;
constexpr std::uintptr_t kLatestSteamTaskReadyRva = 0x00A39060;
constexpr std::uintptr_t kLatestSteamTaskDispatchRva = 0x00A37E60;
constexpr std::uintptr_t kLatestSteamPedestrianFrameRateThrottleRva = 0x0040BBB0;
constexpr std::uintptr_t kLatestSteamPedestrianSpawnUpdateRva = 0x00418400;
constexpr std::uintptr_t kLatestSteamAverageWindowInitializeRva = 0x0017D040;
constexpr std::uintptr_t kLatestSteamMaterialOnLoadRva = 0x00093890;
constexpr std::uintptr_t kLatestSteamUiIsGamePausedRva = 0x005EE350;
constexpr std::uintptr_t kLatestSteamIsCharacterInVehicleRva = 0x00539CC0;
constexpr std::uintptr_t kLatestSteamPcFileReadRva = 0x00A38230;
constexpr std::uintptr_t kLatestSteamPcFileSeekRva = 0x00A38270;
constexpr std::uintptr_t kLatestSteamPcFileTellRva = 0x00A387E0;
constexpr std::uintptr_t kLatestSteamPcFileSizeRva = 0x00A38830;
constexpr std::uintptr_t kLatestSteamQFileReadAtRva = 0x00189760;
constexpr std::uintptr_t kLatestSteamQFileWriteAtRva = 0x0018CF00;
constexpr std::uintptr_t kLatestSteamQFileReadyRva = 0x0018CAB0;
constexpr std::uintptr_t kLatestSteamQcmpDecompressRva = 0x00183110;
constexpr std::uintptr_t kLatestSteamStreamFileOpenRva = 0x0022BD20;
constexpr std::uintptr_t kLatestSteamStreamFileCloseRva = 0x002286F0;
constexpr std::uintptr_t kLatestSteamResourceChunkDispatchRva = 0x00176BF0;
constexpr std::array<std::uint8_t, 13> kPedestrianSpawnUpdateSignature{
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0xC7, 0x41, 0x20, 0x01, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 16> kConversationDispatcherSignature{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
    0x24, 0x18, 0x56, 0x41, 0x56, 0x41, 0x57, 0x48};
constexpr std::array<std::uint8_t, 16> kScriptedConversationEntrySignature{
    0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74,
    0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48};
constexpr std::array<std::uint8_t, 16> kSpeechWrapperSignature{
    0x48, 0x8B, 0xC4, 0x56, 0x57, 0x41, 0x54, 0x41,
    0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48};
constexpr std::array<std::uint8_t, 16> kSpeakByNameSignature{
    0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24};
constexpr std::array<std::uint8_t, 14> kLegacyPedestrianThrottleSignature{
    0x8B, 0x05, 0xA6, 0xF5, 0xC6, 0x01, 0xF3, 0x0F, 0x10, 0x15, 0x9A, 0xF9, 0xC6, 0x01};
constexpr std::array<std::uint8_t, 14> kLatestPedestrianThrottleSignature{
    0x8B, 0x05, 0x56, 0xF3, 0xC6, 0x01, 0xF3, 0x0F, 0x10, 0x15, 0x4A, 0xF7, 0xC6, 0x01};
constexpr std::array<std::uint8_t, 20> kAverageWindowInitializeSignature{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0x29, 0x74, 0x24,
    0x40, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x09, 0x0F, 0x29, 0x7C};
constexpr std::array<std::uint8_t, 16> kMaterialOnLoadSignature{
    0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56, 0x57,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30};
constexpr std::array<std::uint8_t, 6> kUiIsGamePausedSignature{
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x20};
// 48 85 D2 74 49 53 = test rdx,rdx / je +0x49 / push rbx
constexpr std::array<std::uint8_t, 6> kSimTimeIsPausedSignature{
    0x48, 0x85, 0xD2, 0x74, 0x49, 0x53};
constexpr std::array<std::uint8_t, 16> kCharacterPhysicsGetVelocitySignature{
    0x48, 0x8B, 0x81, 0xB0, 0x01, 0x00, 0x00, 0x48,
    0x8B, 0x48, 0x20, 0x48, 0x8B, 0xC2, 0x0F, 0x28};
constexpr std::array<std::uint8_t, 11> kUiIsPlayerInCombatSignature{
    0x48, 0x8B, 0x0D, 0xC9, 0xB7, 0xDC, 0x01, 0x33, 0xD2, 0xE9, 0x12};
constexpr std::array<std::uint8_t, 38> kLegacyIsCharacterInVehicleSignature{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x07, 0x32,
    0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3, 0x8B, 0x15, 0x66, 0xD6,
    0xB4, 0x01, 0xE8, 0x55, 0x70, 0xC5, 0xFF, 0x48, 0x85, 0xC0,
    0x0F, 0x95, 0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3};
constexpr std::array<std::uint8_t, 38> kLatestIsCharacterInVehicleSignature{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x85, 0xC9, 0x75, 0x07, 0x32,
    0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3, 0x8B, 0x15, 0x06, 0xD4,
    0xB4, 0x01, 0xE8, 0x85, 0x6D, 0xC5, 0xFF, 0x48, 0x85, 0xC0,
    0x0F, 0x95, 0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3};
constexpr std::array<std::uint8_t, 17> kPcFileReadSignature{0x48, 0x83, 0xEC, 0x38, 0x48, 0x8B,
                                                            0x4A, 0x50, 0x49, 0x8B, 0xC1, 0x4D,
                                                            0x8B, 0xD0, 0x48, 0x85, 0xC9};
constexpr std::array<std::uint8_t, 15> kPcFileSeekSignature{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x4A, 0x50, 0x4D, 0x8B, 0xD1, 0x48, 0x85, 0xC9, 0x75};
constexpr std::array<std::uint8_t, 13> kPcFileTellAndSizeSignature{
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x4A, 0x50, 0x48, 0x85, 0xC9, 0x75, 0x09};
constexpr std::array<std::uint8_t, 17> kQFileReadAtSignature{0x48, 0x89, 0x6C, 0x24, 0x18, 0x56,
                                                             0x41, 0x56, 0x41, 0x57, 0x48, 0x83,
                                                             0xEC, 0x20, 0x4D, 0x8B, 0xF9};
constexpr std::array<std::uint8_t, 17> kQFileWriteAtSignature{0x48, 0x89, 0x6C, 0x24, 0x18, 0x56,
                                                              0x41, 0x56, 0x41, 0x57, 0x48, 0x83,
                                                              0xEC, 0x40, 0x4D, 0x8B, 0xF9};
constexpr std::array<std::uint8_t, 16> kQcmpDecompressSignature{
    0x48, 0x89, 0x74, 0x24, 0x20, 0x41, 0x56, 0x48,
    0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00, 0x0F, 0x10};
constexpr std::array<std::uint8_t, 6> kQFileReadySignature{0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
constexpr std::array<std::uint8_t, 16> kStreamFileOpenSignature{
    0x48, 0x8B, 0xC4, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0xC7, 0x40, 0xD8};
constexpr std::array<std::uint8_t, 16> kStreamFileCloseSignature{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0x79, 0x20, 0x00, 0x48, 0x8B, 0xD9, 0x75, 0x11};
constexpr std::array<std::uint8_t, 16> kResourceChunkDispatchSignature{
    0x40, 0x57, 0x41, 0x54, 0x41, 0x56, 0x48, 0x83,
    0xEC, 0x30, 0x45, 0x8B, 0xE0, 0x48, 0x8B, 0xFA};
constexpr std::array<std::uint8_t, 16> kPass8WndProcSignature{
    0x4C, 0x89, 0x4C, 0x24, 0x20, 0x55, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr std::array<std::uint8_t, 17> kPass8OsGameUrlProbeSignature{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57,
    0x48, 0x81, 0xEC, 0xD0, 0x01, 0x00};
constexpr std::array<std::uint8_t, 16> kPass8ActivityTimerUpdateSignature{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
    0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x57};
// READ FROM THE BINARY, not transcribed: 32 bytes at RVA 0x366490 of
// sdhdship.exe (x64, imagebase 0x140000000), mapped through the .text section
// table (.text vaddr 0x1000, rawptr 0x400 -> file offset 0x365890).
//   48 89 5C 24 08    mov [rsp+08], rbx
//   48 89 74 24 10    mov [rsp+10], rsi
//   57                push rdi
//   48 83 EC 70       sub  rsp, 70h
//   48 8B D9          mov  rbx, rcx      <- `this` (the Stimulus)
// immediately followed by 48 8B 4A 28 (mov rcx,[rdx+28h]) and 49 8B F0
// (mov rsi, r8). SDK ai/stimulus.hh:25 declares
//   Init(StimulusEmitterComponent* emitter, SimObject* stimulus_target)
// so on x64 `emitter` is arg2 => RDX and `stimulus_target` is arg3 => R8; RCX is
// the Stimulus itself, NOT a receiver. Carrying the trailing `mov rbx, rcx` is
// deliberate: it is what pins the register->argument mapping the capture
// depends on, and an arity change then fails ValidateHookSignature instead of
// silently capturing the wrong register.
constexpr std::array<std::uint8_t, 18> kStimulusInitSignature{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24,
    0x10, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x8B, 0xD9};

// THE CONTROL. `readable` only proves the dereference did not fault; it says
// nothing about WHICH object was dereferenced. The emitter's vftable RVA is a
// known constant (sym-cop-crowd.txt:58, VA 0x141768BE8), so a captured pointer
// whose vtable equals it IS a StimulusEmitterComponent, and the +0x18 read is
// then provably the emitter's own TypeUID. A vtable mismatch means some other
// object was passed, and the uid read from it is meaningless no matter how
// plausible it looks -- so the state is INVALID regardless of the value read.
// Four earlier attempts in this campaign died on exactly that substitution.
constexpr std::uintptr_t kStimulusEmitterVftableRva = 0x01768BE8;

using TaskReadyFn = void (*)(void* task_manager_owner, void* task_header);
using TaskDispatchFn = void (*)(void* task_manager, unsigned int worker_index);
using WaitHelperFn = void (*)(void* task_manager_owner, void* task_header);
using ScaleformTimeFn = void (*)();
using ScaleformInitFn = void (*)(void* heap_desc, void* sys_alloc);
using NisSetPlayTimeFn = void (*)(void* nis_manager, float scene_time, bool sync_scene_time);
using NisPlayFn = void (*)(void* nis_manager);
using NisBootstrapFn = unsigned char (*)(void* nis_manager, void* scene_asset);
using NisOwnerFn = void (*)(void* nis_manager, float delta_seconds);
using NisActorSetupFn = void (*)(std::uintptr_t actor_state, std::uintptr_t actor_target);
using NisActorRestoreFn = void (*)(std::uintptr_t actor_state);
using TwitchStateTickFn = void (*)(std::uintptr_t owner);
using TwitchLoginCallbackFn = void (*)(int result, std::uintptr_t owner);
using Pass8WndProcFn = long long (*)(void* hwnd,
                                     unsigned int message,
                                     unsigned long long wparam,
                                     long long lparam);
using Pass8OsGameUrlProbeFn = int (*)(void* result_struct, const char* url);
using Pass8ActivityTimerUpdateFn = void (*)(void* component, float delta_seconds);
using FrameFlowFn = void (*)(float delta_seconds, void* callback);
using CutsceneFlowOwnerFn = void (*)(void* owner, float delta_seconds);
using VolumetricFogSetterFn = void (*)(void* script_context);
using FogSlicingModeFn = void (*)(void* time_of_day_manager, int update_interval);
using AntiAliasOwnerFn = void (*)(std::uintptr_t render_context,
                                  std::uintptr_t param_2,
                                  std::uintptr_t param_3,
                                  std::uintptr_t* param_4,
                                  std::uintptr_t* param_5);
using AntiAliasFxHandoffFn = void (*)(std::uintptr_t render_context,
                                      std::uintptr_t arg1,
                                      std::uintptr_t arg2,
                                      std::uintptr_t arg3);
using AntiAliasAuxStateQueryFn = char (*)(void* state);
using RenderSubmitMaterialFn = void (*)(void* render_cmd,
                                        void* submit_params,
                                        void* material,
                                        int flags);
using PresentBufferFn = void (*)(void* render_packet);
using PedestrianFrameRateThrottleFn = void (*)();
using PedestrianSpawnUpdateFn = void (*)(void* manager, float delta_seconds);
// Stimulus::Init (SDK ai/stimulus.hh:25). RCX is the receiver, RDX is the
// StimulusEmitterComponent* this probe captures, R8 is the SimObject target.
// The 18 signature bytes below are read from the shipped sdhdship.exe and
// include `mov rbx, rcx`, so a mismatch on arity fails the guard instead of
// silently capturing the wrong register.
using StimulusInitFn = void (*)(void* stimulus,
                                void* emitter,
                                void* stimulus_target);
// ConversationElement::Trigger-equivalent: shared virtual entry for ambient
// conversation dispatches. The element at rcx carries the reaction type at
// +0x98, flags at +0x9C, the action name at +0x38 and the target speech
// component at +0x68 (doc 306 / round68 ledger sections 14-15).
using ConversationDispatcherFn = bool (*)(void* element,
                                          unsigned int reaction_id,
                                          unsigned int arg3,
                                          unsigned char arg4);
// ScriptedConversationElement entry twin: identical argument contract to the
// base dispatcher (it tail-jumps to it), reached through the derived vtable.
using ScriptedConversationEntryFn = ConversationDispatcherFn;
// Speech-by-id wrapper (0x5A8E20).
// CORRECTED 2026-08-28 (ledger sec 36, InvocationAudit): the argument order
// below is the PREVIOUS belief and is WRONG. Disassembly proves arg2 is the
// NAME id and arg3 is the VOICE id:
//   0x5A8E9B  lea eax,[rbp+1]        ; rbp = arg2
//   0x5A8E9E  test eax,0xfffffffe
//   0x5A8EA3  cmove ebp,[global]     ; arg2 is defaulted when 0/-1 => NAME id
//   0x5A8E93  mov ecx, r15d          ; arg3 (r15d) passed through => VOICE id
// Passing voice_id in arg2 therefore made the game look up a name id that is
// really a voice id, which silently produces no line.
// Prefer SpeakByNameFn (below), which resolves the name itself.
using SpeechWrapperFn = bool (*)(void* target_component,
                                 int name_id,
                                 unsigned int voice_id,
                                 unsigned int zero,
                                 unsigned int reaction_id,
                                 unsigned char flag);
// Speech-by-NAME entry (0x5A8930). Takes the fully-qualified voice name in r8 and
// resolves it internally via 0x143890, then runs the same guards and the same inner
// implementation (0x5A7AE0) as the by-id wrapper. This is the route SPatch uses: it
// avoids depending on the by-id argument order entirely.
//
// SEVEN arguments, not six. The real caller (dispatcher 0x5A64E2) sets
// [rsp+0x20]=arg5, [rsp+0x28]=arg6 and [rsp+0x30]=arg7, and SpeakByName reads that
// seventh byte at 0x5A8DCF and forwards it to the inner speak 0x5A7AE0:
//   0x5A8DCF movzx eax, byte [rbp+0xc0]   ; rbp = R-0x90, so [rbp+0xc0] = R+0x30
//   0x5A8DD6 mov   byte [rsp+0x28], al    ; forwarded as the inner call's arg6
// Declaring only six args left that slot uninitialised, which is what raised the
// exception we observed live (ledger sec 38).
//
// Argument SEMANTICS (from dispatcher 0x5A6450 - note arg2/arg5 are easy to invert):
//   rcx = ActorAudioComponent of the SPEAKER (Wei)
//   edx = VOICE id (ARG2)          <- 0x5A648D mov edi,[rcx+0x18]; 0x5A64B3 mov edx,edi
//   r8  = name string,  r9 = 0
//   ARG5 [rsp+0x20] = REACTION id  <- 0x5A646E mov esi,r8d; 0x5A647D cmovne esi,0xA
//   ARG6 [rsp+0x28] = flag
//   ARG7 [rsp+0x30] = trailing flag
// Guard 4 thresholds ARG5 (the reaction id) against [component+0x1A8]:
//   0x5A8978 mov r12d,[rbp+0xb0]  ; rbp = R-0x90, so this is R+0x20 = ARG5
//   0x5A8994 cmp r12d,[rdi+0x1a8] ; jbe -> return false
using SpeakByNameFn = bool (*)(void* target_component,
                               unsigned int voice_id,
                               const char* name,
                               unsigned int zero,
                               unsigned int reaction_id,
                               unsigned char flag,
                               unsigned char trailing_flag);
using AudioNameToIdFn = unsigned int (*)(const char* name);
using AverageWindowInitializeFn = void (*)(void* window,
                                           float maximum_timespan,
                                           float assumed_sample_rate);
using MaterialOnLoadFn = void (*)(void* material);
using ChaseCameraSetParametersFn = void (*)(void* component,
                                             const void* parameters);
using ChaseCameraUpdateFn = void (*)(void* component, float delta_seconds);
using AngularApproachFn = bool (*)(float* current,
                                   float target,
                                   float rate,
                                   float delta_seconds);
struct CameraVector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};
using GameCameraSetDesiredEyeLookUpFn = void (*)(
    void* component,
    float delta_seconds,
    const CameraVector3* desired_eye,
    const CameraVector3* desired_look,
    const CameraVector3* desired_up,
    bool collide,
    CameraVector3* new_eye,
    CameraVector3* new_look);
using SamplerBuilderFn = void (*)(void* sampler_state, void* descriptor_source);
using AnisotropyWriterFn = void (*)(int exponent);
using PcFileReadFn = qfile_io::DeviceReadFn;
using PcFileSeekFn = qfile_io::DeviceSeekFn;
using PcFileTellFn = std::uint64_t (*)(void* device, void* file);
using PcFileSizeFn = std::uint64_t (*)(void* device, void* file);
using QFileReadAtFn = std::uint64_t (*)(void* file, void* buffer, std::uint64_t byte_count,
                                        std::int64_t offset, std::uint32_t origin);
using QFileWriteAtFn = std::uint64_t (*)(void* file, const void* buffer, std::uint64_t byte_count,
                                         std::int64_t offset, std::uint32_t origin,
                                         bool* disk_full);
using QcmpDecompressFn = std::uint64_t (*)(void* source,
                                           std::uint64_t source_size,
                                           void* destination,
                                           std::uint64_t destination_capacity,
                                           const char* label);
using StreamFileOpenFn = void* (*)(const char* path, std::uint32_t access, bool allow_loose_file,
                                   void* device, void* user_data);
using StreamFileCloseFn = void (*)(void* stream_file);
using ResourceChunkDispatchFn = void (*)(void* manager,
                                         void* buffer,
                                         std::uint32_t size);
using RumbleApplyHelperFn = void (*)(std::uintptr_t rumble_object, bool enabled);
using CharacterHandleWaterCollisionFn = void (*)(void* character_effects_component, const void* mat,
                                                 const void* character_velocity);
using CharacterEffectsUpdateFn = void (*)(void* character_effects_component, float delta_seconds);
using CharacterPhysicsGetVelocityFn = void* (*)(void* character_physics_component,
                                                float* output_velocity);
using UiIsPlayerInCombatFn = bool (*)();
using IsCharacterInVehicleFn = bool (*)(void* sim_object);
using HealthApplyDamageFn = bool (*)(void* health_component,
                                     int damage,
                                     void* attacker,
                                     void* hit_record,
                                     bool projectile_damage);
// There is deliberately no SimTimeIsPausedFn. The engine routine at
// kSimTimeIsPausedRva is not callable as `bool()`: it requires an out pointer in
// RDX and stores 8 bytes through it. See ReadSimTimePausedSafely().
using UiIsGamePausedFn = bool (*)();
using CharacterPaintOwnerFn = void (*)(std::uintptr_t owner, std::uintptr_t character_effects_component);
using CharacterPaintConsumerFn = void (*)(std::uintptr_t character_effects_component,
                                          std::uintptr_t paint_block_owner);
using CharacterEffectDispatchOwnerFn = void* (*)();
using CharacterEffectDispatchConsumeFn = void (*)(void* dispatch_owner,
                                                  void* character_effects_component);
using CharacterEffectQueueBuilderFn = void (*)(std::uintptr_t owner,
                                               std::uintptr_t character_effects_component,
                                               std::uint32_t mode);
using CharacterAnimationApplyCharredEffectFn = void (*)(void* character_animation_component,
                                                        float amount);
using CharacterAnimationApplyPaintEffectFn = void (*)(void* character_animation_component,
                                                      bool enable,
                                                      float r,
                                                      float g,
                                                      float b);
using CharacterAnimationCreateDamageRigFn = void (*)(void* character_animation_component);
using DamageRigApplyCharredEffectFn = void (*)(void* damage_rig, float amount);
using DamageRigApplyPaintEffectFn = void (*)(void* damage_rig,
                                             bool enable,
                                             float r,
                                             float g,
                                             float b);
using CharacterDamageRigResetDamageFn = void (*)(void* damage_rig);
using SimObjectGetComponentFn = void* (*)(void* sim_object, std::uint32_t type_uid);
using TimeOfDayAccessorFn = std::uintptr_t (*)();
using SceneryPrepareFn = void (*)(std::uintptr_t scenery_state);
using ScenerySetupFn = void (*)(std::uintptr_t query_context,
                                std::uint32_t query_kind,
                                void* param_3,
                                void* param_4,
                                float range_scale);
using RenderSceneryBuilderFn = std::uintptr_t (*)(std::uintptr_t param_1,
                                                  std::uintptr_t param_2,
                                                  void* param_3,
                                                  std::uint32_t param_4,
                                                  std::uint32_t param_5);
using RasterizeBucketBuilderFn = unsigned long long (*)(std::uintptr_t state,
                                                        int bucket_index,
                                                        void* dependency);
TaskReadyFn g_task_ready_original = nullptr;
TaskDispatchFn g_task_dispatch_original = nullptr;
WaitHelperFn g_wait_helper_original = nullptr;
ScaleformTimeFn g_scaleform_time_original = nullptr;
ScaleformInitFn g_scaleform_init_original = nullptr;
NisSetPlayTimeFn g_nis_set_play_time_original = nullptr;
NisPlayFn g_nis_play_original = nullptr;
NisBootstrapFn g_nis_bootstrap_original = nullptr;
NisOwnerFn g_nis_owner_original = nullptr;
NisActorSetupFn g_nis_actor_setup_original = nullptr;
NisActorRestoreFn g_nis_actor_restore_original = nullptr;
TwitchStateTickFn g_twitch_state_tick_original = nullptr;
TwitchLoginCallbackFn g_twitch_login_callback_original = nullptr;
Pass8WndProcFn g_pass8_wndproc_original = nullptr;
Pass8OsGameUrlProbeFn g_pass8_os_game_url_original = nullptr;
Pass8ActivityTimerUpdateFn g_pass8_activity_timer_original = nullptr;
// Pass-8 probe counters.  All observe-only; the summary cadence and the
// last-seen mouse position are owned by the window thread, everything else
// is a relaxed atomic tally.
std::atomic<unsigned long long> g_pass8_messages_total = 0;
std::atomic<unsigned long long> g_pass8_mousemove_total = 0;
std::atomic<unsigned long long> g_pass8_mousemove_same_position = 0;
std::atomic<unsigned long long> g_pass8_timer_calls = 0;
std::atomic<unsigned long long> g_pass8_timer_advanced = 0;
std::atomic<unsigned long long> g_pass8_timer_pulse_advance = 0;
std::atomic<unsigned long long> g_pass8_timer_verbose = 0;
std::atomic<unsigned long long> g_pass8_os_game_url_calls = 0;
Pass8ActivityTimerUpdateFn g_activity_timer_fix_original = nullptr;
std::atomic<unsigned long long> g_activity_timer_fix_corrections = 0;
std::atomic<unsigned long long> g_activity_timer_fix_verbose = 0;
unsigned long long g_pass8_summary_last_tick = 0;
int g_pass8_last_mouse_x = 0;
int g_pass8_last_mouse_y = 0;
bool g_pass8_have_mouse_position = false;
FrameFlowFn g_frame_flow_original = nullptr;
CutsceneFlowOwnerFn g_cutscene_flow_owner_original = nullptr;
VolumetricFogSetterFn g_volumetric_fog_setter_original = nullptr;
FogSlicingModeFn g_fog_slicing_mode_original = nullptr;
AntiAliasOwnerFn g_antialias_owner_original = nullptr;
AntiAliasFxHandoffFn g_antialias_fx_handoff_original = nullptr;
AntiAliasAuxStateQueryFn g_antialias_aux_state_query_original = nullptr;
RenderSubmitMaterialFn g_render_submit_material_original = nullptr;
PresentBufferFn g_present_buffer_original = nullptr;
PedestrianFrameRateThrottleFn g_pedestrian_frame_rate_throttle_original = nullptr;
PedestrianSpawnUpdateFn g_pedestrian_spawn_update_original = nullptr;
ConversationDispatcherFn g_conversation_dispatcher_original = nullptr;
ScriptedConversationEntryFn g_scripted_conversation_entry_original = nullptr;
SpeechWrapperFn g_speech_wrapper_original = nullptr;
SpeakByNameFn g_speak_by_name_original = nullptr;
AudioNameToIdFn g_audio_name_to_id = nullptr;
// Push-voice-line runtime state. The gate is reached from the conversation
// dispatchers AND from the HealthApplyDamage bump path, which the engine may drive
// from different threads, so Evaluate() is wrapped in a lock below rather than
// relying on a single call site.
// detour (game threads); the atomics keep the diagnostic counters and the
// name-id cache honest.
push_voice_line::PushVoiceLineGate g_push_voice_line_gate;
// Guards g_push_voice_line_gate. See EvaluatePushVoiceLineGate for why: two hook
// paths on possibly different engine threads mutate the gate's non-atomic state.
std::mutex g_push_voice_line_gate_mutex;
std::atomic<unsigned long> g_push_voice_line_diag_events = 0;
std::atomic<unsigned long> g_push_voice_line_wrapper_diag_events = 0;
std::atomic<unsigned long> g_push_voice_line_fired = 0;
std::atomic<unsigned long> g_push_voice_line_bump_probe_events = 0;
// The player is identified by the character-effects component pointer the engine
// hands to our own CharacterEffects::Update hook with its attached-to-player flag
// set - the same signal the shipped wetness/sweat restore relies on. Remember the
// SimObject derived from it so bump detection can ask "is a struck character the
// player?" without guessing an id (round68 ledger section 33).
std::atomic<std::uintptr_t> g_player_sim_object_from_effects = 0;
constexpr unsigned long kPushVoiceLineBumpProbeBudget = 400;
std::atomic<unsigned int> g_push_voice_line_name_id_cached{0};
std::atomic<bool> g_push_voice_line_name_id_resolved{false};
thread_local bool g_push_voice_line_invoking = false;
// Wei voice-line state (restore_combat_lines). A SEPARATE gate instance:
// these lines must not consume the push line's cooldown/budget (and the two
// features can be enabled independently). The gate type and its v1 constants
// are reused; combat taunts may need a tighter budget after a live session.
push_voice_line::PushVoiceLineGate g_wei_voice_line_gate;
std::mutex g_wei_voice_line_gate_mutex;
std::atomic<unsigned long> g_wei_voice_line_diag_events = 0;
std::atomic<unsigned long> g_wei_voice_line_fired = 0;
// Alternates combat_taunt_cold / combat_taunt_angry; advanced only when a
// taunt actually plays so the pick stays even across suppressed events.
std::atomic<unsigned long> g_wei_voice_line_taunt_counter = 0;
// Last fire time of a combat taunt (GetTickCount64 ms; 0 = never). Enforces
// wei_voice_line::kTauntCooldownMs ON TOP of the shared gate so taunts do not
// crowd out apologies/entry lines from the gate budget; checked before the
// gate so a gapped taunt consumes no slot (tuned 2026-08-29, user feedback).
std::atomic<std::uint64_t> g_wei_voice_line_last_taunt_ms = 0;
// Last observed player combat state, so the not-in-combat -> in-combat flip
// can be detected at damage events (the only place this path observes).
std::atomic<bool> g_wei_voice_line_combat_active = false;
// Combat-entry persistence fix (live session 2026-08-29: 48 accepted lines,
// zero combat_pre_posture). The flip is observed on ONE damage event; if the
// shared gate was inside its cooldown at that instant, v1 lost the entry
// line for the whole fight. The flip now sets this flag, which survives gate
// cooldowns until the entry line fires or combat ends; a pending entry
// outranks the same event's taunt per the documented priority.
std::atomic<bool> g_wei_voice_line_entry_pending = false;
// NPC voice-line state (restore_police_voice_lines /
// restore_encourage_voice_lines). Two SEPARATE gate instances - one per
// family group - so the police triggers and the encouragement trigger pace
// independently; the gate type and its v1 constants are reused from the
// shipped push/Wei stack. The diag counter is shared because the two
// features log the same suppressed-shape line.
push_voice_line::PushVoiceLineGate g_npc_police_voice_line_gate;
std::mutex g_npc_police_voice_line_gate_mutex;
push_voice_line::PushVoiceLineGate g_npc_encourage_voice_line_gate;

// Driving swear trigger (MaybeFireDrivingSwearLine, restored 2026-10-31 from
// the recovery snapshot - the arm was lost in the heat-meter-restore
// restructure, leaving DrivingSwearLines=1 with no trigger on any layout).
push_voice_line::PushVoiceLineGate g_driving_swear_line_gate;
std::mutex g_driving_swear_line_gate_mutex;
std::atomic<unsigned long> g_driving_swear_line_fired = 0;
std::atomic<unsigned long> g_driving_swear_observations = 0;
std::atomic<unsigned long> g_driving_swear_candidates = 0;
std::atomic<unsigned long> g_driving_swear_variant_cursor{0};
struct DrivingSwearTriggerState {
    void* component = nullptr;
    std::uintptr_t physics_mover = 0;
    bool has_prev_speed = false;
    float prev_speed = 0.0f;
};
thread_local DrivingSwearTriggerState g_driving_swear_trigger_state;


// ---------------------------------------------------------------------------
// Vehicle light shadows (round75). The deferred-light submit selector reads a
// per-light flag byte from the packed batch entry (arg3 + 2); bit 2 makes the
// stock selector choose the game's own spotlight shadow technique family
// (DRSPOTLIGHTSHADOWS). The detour below sets that bit for configured light
// types before the original runs; every variant decision stays stock.
using LightSubmitSelectorFn = void (*)(void* render_object,
                                       const void* light_record,
                                       void* packed_entry);
LightSubmitSelectorFn g_light_submit_selector_original = nullptr;
DiagnosticAtomic<unsigned long> g_light_submit_selector_calls{0};
DiagnosticAtomic<unsigned long> g_light_submit_shadows_forced{0};
DiagnosticAtomic<unsigned long> g_light_submit_shadows_already_set{0};
DiagnosticAtomic<unsigned long> g_light_submit_flag_write_faults{0};
// Per-type submit counters; slot 8 aggregates types >= 8. The session census
// needs whole-session visibility, not a capped first-come line stream: the
// first probe burned its 400-line budget on street lamps in 1.9 seconds.
constexpr std::size_t kLightSubmitTypeSlots = 9;
std::array<DiagnosticAtomic<unsigned long>, kLightSubmitTypeSlots>
    g_light_submit_type_counts{};
std::mutex g_light_submit_kind_mutex;
std::set<std::uint32_t> g_light_submit_kinds;
std::atomic<std::uint64_t> g_light_submit_last_summary_ms{0};

// ---------------------------------------------------------------------------
// Vehicle headlight beam light (round78). The light is a real engine
// Render::DynamicLightComponent spliced into the deferred light list, not a
// record injected into the light tables - the shadow-map pass never reads
// those tables (round78/notes.md), so only a component can own a shadow map.
//
// The per-frame record table builder FUN_14004e230 walks that list and is
// called synchronously before the collection job is queued, so the detour
// updates the component at its entry and the walk picks it up the same frame.
using RecordTableBuilderFn = int (*)(std::int64_t record_table,
                                     std::int64_t texture_table);
RecordTableBuilderFn g_record_table_builder_original = nullptr;
// Two components, one per headlight. A single central light reads as a
// spotlight bolted to the bonnet rather than a pair of headlights.
std::uint8_t* g_beam_components[vehicle_beam_light::kBeamLightCount] = {};
std::uint8_t* g_beam_records[vehicle_beam_light::kBeamLightCount] = {};

// True when `record` is one of our components' record blocks.
bool IsBeamRecord(const void* record) noexcept {
    for (int index = 0; index < vehicle_beam_light::kBeamLightCount; ++index) {
        if (vehicle_beam_light::IsOwnRecord(record, g_beam_components[index])) {
            return true;
        }
    }
    return false;
}
vehicle_beam_light::BeamTracker g_beam_tracker{};
DiagnosticAtomic<unsigned long> g_beam_frames_appended{0};  // per-frame updates
DiagnosticAtomic<unsigned long> g_beam_append_faults{0};
DiagnosticAtomic<unsigned long> g_beam_frames_seen{0};
DiagnosticAtomic<unsigned long> g_beam_hero_found{0};
DiagnosticAtomic<unsigned long> g_beam_lamp_found{0};
DiagnosticAtomic<unsigned long> g_beam_submits{0};
std::atomic<std::uint64_t> g_beam_last_summary_ms{0};
std::uint32_t g_beam_last_total = 0;
std::atomic<bool> g_beam_create_attempted[vehicle_beam_light::kBeamLightCount]{};
std::atomic<int> g_beam_template_type{-1};
std::atomic<bool> g_beam_probe_done{false};
std::atomic<bool> g_beam_gate_probe_done{false};
std::atomic<int> g_beam_in_table{0};
float g_beam_last_hero_pos[3] = {0.0f, 0.0f, 0.0f};
float g_beam_last_our_pos[3] = {0.0f, 0.0f, 0.0f};

// Deferred light collection job FUN_14007dd40(a, b, ctx). Hooked at its head
// because `ctx` carries the camera the collector gates every light against.
using LightCollectorFn = void (*)(std::int64_t arg1,
                                  std::int64_t arg2,
                                  std::uint32_t* context);
LightCollectorFn g_light_collector_original = nullptr;

// ---------------------------------------------------------------------------
// Vehicle anchor (round80).
//
// Round79 scanned the chase camera's subject for a world matrix and found one
// at +0x70 - but its translation was byte-identical two hours apart and then
// frozen for 75 s while the camera moved: a static prop near the spawn point,
// not the followed vehicle. That approach is removed rather than patched.
//
// The live source was already in the log. The desired-pose detour receives
// `desired_look` - the point the chase camera is looking at - and that point
// IS what the camera follows: the car while driving, the character on foot.
// `look - eye`, flattened to the ground plane, is its heading. Both are
// recomputed every frame, so nothing can go stale.
//
// Driving is detected from the camera component itself: the selected parameter
// block at +0xB50 equals the Drive slot at +0x388 exactly when the player is
// driving (VehicleCameraPolicy documents that normal driving selects +0x388).
// On foot the beam is parked out of frustum instead of lighting Wei, because a
// headlight on a pedestrian is not the feature.
float g_beam_anchor_pos[3] = {0.0f, 0.0f, 0.0f};
float g_beam_anchor_dir[3] = {0.0f, 0.0f, 0.0f};
std::atomic<bool> g_beam_anchor_valid{false};
// 0 = unknown (never treat as on-foot: that would hide a working feature),
// 1 = driving, 2 = confirmed not driving. Only 2 parks the lights.
std::atomic<int> g_beam_drive_state{0};
float g_beam_cached_camera[3] = {0.0f, 0.0f, 0.0f};
// The camera's own forward, cached so the "is the light in front of me" check
// can use an INDEPENDENT axis. Measuring it against the record's own normal was
// circular - that always reads positive - and it hid an inverted direction for
// two sessions.
float g_beam_cached_forward[3] = {0.0f, 0.0f, 0.0f};
std::atomic<bool> g_beam_cached_camera_valid{false};
// 1 while the lights are parked out of frustum because the player is on
// foot. Logged, so "the light vanished" is never a mystery.
std::atomic<int> g_beam_parked{0};
// The collector's own output: how many lights it admitted this frame, split
// into its four batches. Read after the original returns, because these are
// written at the END of FUN_14007dd40. They separate "the builder rejected the
// light" (in_table=0) from "the collector filtered it out" (in_table=1,
// groups all zero for the type-1 lanes).
// Distance the placement used from the camera out to the car. Logged because
// it is the one number that says whether the anchor was the car or the camera.
float g_beam_last_lead = 0.0f;
std::atomic<unsigned long> g_beam_group_a{0};
std::atomic<unsigned long> g_beam_group_b{0};
std::atomic<unsigned long> g_beam_group_c{0};
std::atomic<unsigned long> g_beam_group_d{0};
void LogBeamSummaryMaybe(std::uint32_t total,
                         std::uint32_t c0,
                         std::uint32_t d0,
                         std::uint32_t e0,
                         bool hero_found,
                         bool lamp_found) noexcept;
std::mutex g_npc_encourage_voice_line_gate_mutex;
std::atomic<unsigned long> g_npc_voice_line_diag_events = 0;
std::atomic<unsigned long> g_npc_voice_line_fired = 0;
AverageWindowInitializeFn g_average_window_initialize_original = nullptr;
MaterialOnLoadFn g_material_on_load_original = nullptr;
ChaseCameraSetParametersFn g_chase_camera_set_parameters_original = nullptr;
ChaseCameraUpdateFn g_chase_camera_update_original = nullptr;
AngularApproachFn g_angular_approach_original = nullptr;
GameCameraSetDesiredEyeLookUpFn g_game_camera_set_desired_eye_look_up_original =
    nullptr;
std::atomic<unsigned long long> g_gtaiv_camera_probe_sample_count = 0;
std::atomic<unsigned int> g_gtaiv_camera_probe_log_budget = 0;
std::atomic<unsigned long long> g_gtaiv_camera_probe_next_sample_tick = 0;
std::mutex g_gtaiv_camera_probe_state_mutex;
unsigned long long g_gtaiv_camera_probe_last_state =
    (std::numeric_limits<unsigned long long>::max)();
std::uintptr_t g_gtaiv_camera_probe_last_subject =
    (std::numeric_limits<std::uintptr_t>::max)();
std::atomic<unsigned long long> g_gtaiv_camera_dynamic_probe_sample_count = 0;
std::atomic<unsigned int> g_gtaiv_camera_dynamic_probe_log_budget = 0;
std::atomic<unsigned long long> g_gtaiv_camera_dynamic_probe_next_sample_tick =
    0;
SamplerBuilderFn g_sampler_builder_original = nullptr;
std::atomic<bool> g_sampler_builder_first_verified_invocation_logged = false;
std::atomic<bool> g_sampler_builder_failure_logged = false;
AnisotropyWriterFn g_anisotropy_writer_original = nullptr;
std::atomic<bool> g_anisotropy_writer_first_verified_invocation_logged = false;
PcFileReadFn g_pc_file_read_original = nullptr;
PcFileSeekFn g_pc_file_seek_original = nullptr;
PcFileTellFn g_pc_file_tell_original = nullptr;
PcFileSizeFn g_pc_file_size_original = nullptr;
QFileReadAtFn g_qfile_read_at_original = nullptr;
QFileWriteAtFn g_qfile_write_at_original = nullptr;
qfile_io::QFileReadyFn g_qfile_ready = nullptr;
QcmpDecompressFn g_qcmp_decompress_original = nullptr;
std::atomic<bool> g_qcmp_rejection_logged = false;
StreamFileOpenFn g_stream_file_open_original = nullptr;
StreamFileCloseFn g_stream_file_close = nullptr;
ResourceChunkDispatchFn g_resource_chunk_dispatch_original = nullptr;
std::atomic<bool> g_resource_chunk_rejection_logged = false;
constexpr std::size_t kArchiveSizeCacheCapacity = 32;
constexpr std::uintptr_t kArchiveSizeCacheClaimed = 1;
struct ArchiveSizeCacheEntry {
    std::atomic<std::uintptr_t> inventory{0};
    std::atomic<std::uintptr_t> file{0};
    std::atomic<std::uintptr_t> native_handle{0};
    std::atomic<std::uint32_t> resource_uid{0};
    std::atomic<std::uint64_t> size{qfile_io::kOperationFailure};
};
std::array<ArchiveSizeCacheEntry, kArchiveSizeCacheCapacity> g_archive_size_cache{};
std::array<std::atomic<std::uint64_t>, kArchiveSizeCacheCapacity> g_archive_rejection_keys{};
RumbleApplyHelperFn g_rumble_apply_helper = nullptr;
CharacterHandleWaterCollisionFn g_character_handle_water_collision_original = nullptr;
CharacterEffectsUpdateFn g_character_effects_update_original = nullptr;
CharacterPhysicsGetVelocityFn g_character_physics_get_velocity = nullptr;
UiIsPlayerInCombatFn g_ui_is_player_in_combat = nullptr;
IsCharacterInVehicleFn g_is_character_in_vehicle = nullptr;
HealthApplyDamageFn g_health_apply_damage_original = nullptr;
// Set at bootstrap when the simtime-pause globals are known to belong to this
// layout. Legacy-only: the data RVAs are not in the latest-layout remap table.
bool g_simtime_pause_supported = false;
UiIsGamePausedFn g_ui_is_game_paused = nullptr;
CharacterPaintOwnerFn g_character_paint_owner_original = nullptr;
CharacterPaintConsumerFn g_character_paint_consumer_original = nullptr;
CharacterEffectDispatchOwnerFn g_character_effect_dispatch_owner_original = nullptr;
CharacterEffectDispatchConsumeFn g_character_effect_dispatch_consume_original = nullptr;
CharacterEffectQueueBuilderFn g_character_effect_queue_builder_original = nullptr;
CharacterAnimationApplyCharredEffectFn g_character_animation_apply_charred_effect_original = nullptr;
CharacterAnimationApplyPaintEffectFn g_character_animation_apply_paint_effect_original = nullptr;
CharacterAnimationCreateDamageRigFn g_character_animation_create_damage_rig_original = nullptr;
DamageRigApplyCharredEffectFn g_damage_rig_apply_charred_effect_original = nullptr;
DamageRigApplyPaintEffectFn g_damage_rig_apply_paint_effect_original = nullptr;
CharacterDamageRigResetDamageFn g_character_damage_rig_reset_damage_original = nullptr;
SimObjectGetComponentFn g_simobject_get_component = nullptr;
SceneryPrepareFn g_scenery_prepare_original = nullptr;
ScenerySetupFn g_scenery_setup_original = nullptr;
RenderSceneryBuilderFn g_render_scenery_builder_original = nullptr;
RasterizeBucketBuilderFn g_rasterize_bucket_builder_original = nullptr;
TimeOfDayAccessorFn g_time_of_day_accessor = nullptr;

Config g_config{};
bool g_use_latest_steam_layout = false;
bool g_sweat_only_health_fast_path = false;
std::uintptr_t g_module_base = 0;
std::uintptr_t g_module_end = 0;
std::uintptr_t g_provider_slot = 0;
std::uintptr_t g_render_task_manager = 0;
std::uintptr_t g_render_context_instance_slot = 0;
std::uintptr_t g_scenery_counter0 = 0;
std::uintptr_t g_scenery_counter1 = 0;
std::uintptr_t g_scenery_counter2 = 0;
std::uintptr_t g_scenery_counter3 = 0;
std::uintptr_t g_d3d_device_slot = 0;
std::uintptr_t g_d3d_context_slot = 0;
std::uintptr_t g_dxgi_swapchain_slot = 0;
std::uintptr_t g_present_rtv_slot = 0;
std::uintptr_t g_rumble_apply_object = 0;
std::uintptr_t g_wet_surface_block_counter = 0;
std::uintptr_t g_volumetric_fog_intensity = 0;
std::filesystem::path g_config_path;
DiagnosticAtomic<unsigned long long> g_task_ready_count = 0;
DiagnosticAtomic<unsigned long long> g_task_dispatch_count = 0;
DiagnosticAtomic<unsigned long long> g_wait_helper_count = 0;
DiagnosticAtomic<unsigned long long> g_wait_helper_nonzero_task_count = 0;
DiagnosticAtomic<unsigned long long> g_wait_over_16ms_count = 0;
DiagnosticAtomic<unsigned long long> g_wait_over_100ms_count = 0;
DiagnosticAtomic<unsigned long long> g_wait_over_1000ms_count = 0;
DiagnosticAtomic<unsigned long long> g_wait_over_5000ms_count = 0;
DiagnosticAtomic<unsigned long long> g_pedestrian_throttle_frame_count = 0;
DiagnosticAtomic<unsigned long long> g_pedestrian_throttle_stock_call_count = 0;
DiagnosticAtomic<unsigned long long> g_pedestrian_throttle_clamped_frame_count = 0;
DiagnosticAtomic<unsigned long long> g_average_window_initialize_count = 0;
DiagnosticAtomic<unsigned long long> g_average_window_expanded_count = 0;
DiagnosticAtomic<bool> g_average_window_fallback_logged = false;
std::atomic<bool> g_original_fog_applied = false;
std::atomic<bool> g_original_fog_frame_initialized = false;
std::atomic<bool> g_original_fog_failure_logged = false;
std::atomic<bool> g_original_fog_setter_logged = false;
std::atomic<std::uint32_t> g_character_eye_restore_applied_mask = 0;
std::atomic<std::uint32_t> g_character_eye_restore_already_present_mask = 0;
std::atomic<std::uint32_t> g_character_eye_restore_failure_mask = 0;
DiagnosticAtomic<unsigned long long> g_scaleform_time_count = 0;
DiagnosticAtomic<unsigned long long> g_scaleform_provider_non_null_count = 0;
DiagnosticAtomic<unsigned long long> g_scaleform_init_count = 0;
DiagnosticAtomic<unsigned long long> g_rumble_override_apply_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_set_play_time_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_sync_scene_time_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_delta_zero_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_delta_30hz_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_delta_60hz_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_delta_other_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_scene_time_fix_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_play_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_play_advanced_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_play_repeat_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_play_multi_tick_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_bootstrap_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_bootstrap_state1_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_bootstrap_state2_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_bootstrap_fail_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_owner_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_owner_dt_zero_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_owner_dt_30hz_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_owner_dt_60hz_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_owner_dt_other_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_owner_advanced_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_owner_repeat_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_owner_multi_tick_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_actor_setup_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_actor_restore_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_actor_restore_untracked_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_actor_setup_duplicate_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_actor_restore_duplicate_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_actor_restore_never_seen_count = 0;
DiagnosticAtomic<unsigned long long> g_nis_actor_restore_suppressed_count = 0;
DiagnosticAtomic<unsigned long long> g_twitch_state_tick_count = 0;
DiagnosticAtomic<unsigned long long> g_twitch_login_callback_count = 0;
DiagnosticAtomic<unsigned long long> g_twitch_login_failure_count = 0;
DiagnosticAtomic<unsigned long long> g_frame_flow_count = 0;
DiagnosticAtomic<unsigned long long> g_frame_flow_dt_zero_count = 0;
DiagnosticAtomic<unsigned long long> g_frame_flow_dt_60hz_count = 0;
DiagnosticAtomic<unsigned long long> g_frame_flow_dt_other_count = 0;
DiagnosticAtomic<unsigned long long> g_frame_flow_from_cutscene_owner_count = 0;
DiagnosticAtomic<unsigned long long> g_cutscene_flow_owner_count = 0;
DiagnosticAtomic<unsigned long long> g_cutscene_flow_owner_dt_zero_count = 0;
DiagnosticAtomic<unsigned long long> g_cutscene_flow_owner_forwarded_zero_count = 0;
DiagnosticAtomic<unsigned long long> g_cutscene_flow_owner_forwarded_60hz_count = 0;
DiagnosticAtomic<unsigned long long> g_cutscene_flow_owner_forwarded_other_count = 0;
// Per-cutscene log budget for the director cadence samples; refilled each
// time a new cutscene scope begins (was process-lifetime, which left every
// scene after the first unlogged).
std::atomic<int> g_cutscene_owner_cadence_budget{32};
DiagnosticAtomic<unsigned long long> g_cutscene_zero_dt_override_count = 0;
DiagnosticAtomic<unsigned long long> g_cutscene_zero_dt_pause_override_count = 0;
DiagnosticAtomic<unsigned long long> g_fog_slicing_mode_count = 0;
DiagnosticAtomic<unsigned long long> g_fog_slicing_clamp_count = 0;
DiagnosticAtomic<unsigned long long> g_aa_owner_count = 0;
DiagnosticAtomic<unsigned long long> g_aa_skip_count = 0;
DiagnosticAtomic<unsigned long long> g_aa_main_count = 0;
DiagnosticAtomic<unsigned long long> g_aa_hair_blur_zero_count = 0;
DiagnosticAtomic<unsigned long long> g_aa_fx_handoff_count = 0;
DiagnosticAtomic<unsigned long long> g_aa_variant_apply_count = 0;
DiagnosticAtomic<unsigned long long> g_aa_aux_apply_count = 0;
DiagnosticAtomic<unsigned long long> g_post_material_submit_count = 0;
DiagnosticAtomic<unsigned long long> g_post_composite_lights_submit_count = 0;
DiagnosticAtomic<unsigned long long> g_post_composite_final_submit_count = 0;
DiagnosticAtomic<unsigned long long> g_post_bloom_threshold_submit_count = 0;
DiagnosticAtomic<unsigned long long> g_post_lightshaft_submit_count = 0;
DiagnosticAtomic<unsigned long long> g_post_shadow_collector_submit_count = 0;
DiagnosticAtomic<unsigned long long> g_post_composite_final_snapshot_change_count = 0;
DiagnosticAtomic<unsigned long long> g_character_water_collision_count = 0;
DiagnosticAtomic<unsigned long long> g_character_effects_update_count = 0;
DiagnosticAtomic<unsigned long long> g_character_effects_wet_surface_count = 0;
DiagnosticAtomic<unsigned long long> g_character_health_apply_damage_count = 0;
DiagnosticAtomic<unsigned long long> g_character_health_apply_projectile_count = 0;
DiagnosticAtomic<unsigned long long> g_character_health_apply_melee_count = 0;
DiagnosticAtomic<unsigned long long> g_character_health_anim_found_count = 0;
DiagnosticAtomic<unsigned long long> g_character_health_hitreact_found_count = 0;
DiagnosticAtomic<unsigned long long> g_character_paint_owner_count = 0;
DiagnosticAtomic<unsigned long long> g_character_paint_consumer_count = 0;
DiagnosticAtomic<unsigned long long> g_character_effect_dispatch_owner_count = 0;
DiagnosticAtomic<unsigned long long> g_character_effect_dispatch_consume_count = 0;
DiagnosticAtomic<unsigned long long> g_character_effect_queue_builder_count = 0;
DiagnosticAtomic<unsigned long long> g_character_effect_queue_builder_tracked_count = 0;
DiagnosticAtomic<unsigned long long> g_character_charred_anim_count = 0;
DiagnosticAtomic<unsigned long long> g_character_paint_anim_count = 0;
DiagnosticAtomic<unsigned long long> g_character_damage_rig_create_count = 0;
DiagnosticAtomic<unsigned long long> g_character_charred_rig_count = 0;
DiagnosticAtomic<unsigned long long> g_character_paint_rig_count = 0;
DiagnosticAtomic<unsigned long long> g_character_damage_rig_reset_count = 0;
DiagnosticAtomic<unsigned long long> g_character_wet_force_count = 0;
DiagnosticAtomic<unsigned long long> g_character_wet_force_verify_count = 0;
#if !defined(SPATCH_FINAL_RELEASE)
DiagnosticAtomic<unsigned long long> g_unique_callback_count = 0;
#endif
DiagnosticAtomic<unsigned long long> g_scenery_prepare_count = 0;
DiagnosticAtomic<unsigned long long> g_scenery_setup_count = 0;
DiagnosticAtomic<unsigned long long> g_render_scenery_builder_count = 0;
DiagnosticAtomic<unsigned long long> g_rasterize_bucket_builder_count = 0;
DiagnosticAtomic<unsigned long long> g_render_scenery_queue_delta_total = 0;
DiagnosticAtomic<unsigned long long> g_rasterize_bucket_queue_delta_total = 0;
DiagnosticAtomic<unsigned long long> g_scenery_setup_queue_delta_total = 0;
DiagnosticAtomic<unsigned long long> g_scenery_prepare_ready_total = 0;
DiagnosticAtomic<unsigned long long> g_scenery_setup_ready_total = 0;
DiagnosticAtomic<unsigned long long> g_render_scenery_ready_total = 0;
DiagnosticAtomic<unsigned long long> g_rasterize_bucket_ready_total = 0;
// A malformed fog interval is a useful diagnostic, but the hook can be
// reached once per render update.  Emit the warning once per process and keep
// the counter for the complete occurrence count instead of turning a bad
// engine value into synchronous logger I/O on every frame.
std::atomic<bool> g_fog_clamp_warning_emitted = false;
DiagnosticAtomic<unsigned int> g_last_scenery_counter0 = 0;
DiagnosticAtomic<unsigned int> g_last_scenery_counter1 = 0;
DiagnosticAtomic<unsigned int> g_last_scenery_counter2 = 0;
DiagnosticAtomic<unsigned int> g_last_scenery_counter3 = 0;
DiagnosticAtomic<std::uint32_t> g_last_nis_owner_dt_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_frame_flow_dt_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_cutscene_flow_input_dt_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_cutscene_flow_forwarded_dt_bits = 0;
DiagnosticAtomic<int> g_last_aa_state_gate = 0;
DiagnosticAtomic<int> g_last_aa_hair_blur_gate = 0;
DiagnosticAtomic<int> g_last_aa_variant_mode = 0;
DiagnosticAtomic<int> g_last_aa_aux_mode = 0;
DiagnosticAtomic<unsigned int> g_last_aa_shader_uid = 0;
DiagnosticAtomic<unsigned int> g_last_aa_raster_uid = 0;
DiagnosticAtomic<unsigned int> g_last_aa_aux_uid = 0;
DiagnosticAtomic<std::uintptr_t> g_last_aa_material = 0;
DiagnosticAtomic<std::uintptr_t> g_last_aa_target = 0;
DiagnosticAtomic<std::uintptr_t> g_last_aa_source_a = 0;
DiagnosticAtomic<std::uintptr_t> g_last_aa_source_b = 0;
DiagnosticAtomic<std::uintptr_t> g_last_aa_fx_arg1 = 0;
DiagnosticAtomic<std::uintptr_t> g_last_aa_fx_arg2 = 0;
DiagnosticAtomic<std::uintptr_t> g_last_aa_fx_arg3 = 0;
DiagnosticAtomic<std::uintptr_t> g_last_post_material = 0;
DiagnosticAtomic<int> g_last_post_final_flags = 0;
DiagnosticAtomic<std::uintptr_t> g_last_post_final_cmd = 0;
DiagnosticAtomic<std::uintptr_t> g_last_post_final_params = 0;
DiagnosticAtomic<std::uintptr_t> g_last_post_final_param0 = 0;
DiagnosticAtomic<std::uintptr_t> g_last_post_final_param1 = 0;
DiagnosticAtomic<std::uintptr_t> g_last_post_final_param2 = 0;
DiagnosticAtomic<std::uintptr_t> g_last_post_final_param3 = 0;
DiagnosticAtomic<int> g_last_rumble_override_value = -1;
DiagnosticAtomic<std::uintptr_t> g_last_character_wet_component = 0;
std::atomic<std::uintptr_t> g_character_sweat_active_component = 0;
std::atomic<std::uintptr_t> g_player_water_collision_component = 0;
std::atomic<unsigned long long> g_player_water_collision_generation = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_water_speed_bits = 0;
DiagnosticAtomic<unsigned int> g_last_character_active_surface_uid = 0;
DiagnosticAtomic<unsigned int> g_last_character_active_wet_surface_uid = 0;
DiagnosticAtomic<int> g_last_character_wet_gate_counter = 0;
DiagnosticAtomic<unsigned int> g_last_character_is_on_fire = 0;
DiagnosticAtomic<unsigned int> g_last_character_is_smoldering = 0;
DiagnosticAtomic<unsigned int> g_last_character_is_attached_to_player = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_fire_extinguish_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_smolder_extinguish_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_queued_health_damage_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_timeofday_weather_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_timeofday_override_bits = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_health_component = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_health_anim_component = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_health_hitreact_component = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_health_attacker = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_health_hit_record = 0;
DiagnosticAtomic<int> g_last_character_health_damage = 0;
DiagnosticAtomic<unsigned int> g_last_character_health_projectile = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_charred_amount_bits = 0;
DiagnosticAtomic<unsigned int> g_last_character_paint_enable = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_paint_r_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_paint_g_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_paint_b_bits = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_paint_consumer_component = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_paint_consumer_block = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_paint_owner_ptr = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_paint_owner_component = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_effect_dispatch_owner = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_effect_dispatch_component = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_effect_queue_builder_owner = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_effect_queue_builder_component = 0;
DiagnosticAtomic<std::uint32_t> g_last_character_effect_queue_builder_mode = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_damage_anim_component = 0;
DiagnosticAtomic<std::uintptr_t> g_last_character_damage_rig = 0;
std::atomic<int> g_aa_runtime_variant_mode = 0;
DiagnosticAtomic<unsigned long long> g_last_aa_variant_up_key_tick = 0;
DiagnosticAtomic<unsigned long long> g_last_aa_variant_down_key_tick = 0;
std::atomic<int> g_aa_runtime_aux_mode = 0;
DiagnosticAtomic<unsigned long long> g_last_aa_aux_up_key_tick = 0;
DiagnosticAtomic<unsigned long long> g_last_aa_aux_down_key_tick = 0;
DiagnosticAtomic<unsigned long long> g_last_smaa_toggle_key_tick = 0;
DiagnosticAtomic<long> g_character_trace_budget = 0;
// Stimulus::Init emitter-capture state. All monotonic and one-shot: the
// report latch is claimed with a CAS so concurrent engine threads cannot
// emit the summary line twice.
StimulusInitFn g_stimulus_init_original = nullptr;
std::atomic<long> g_stimulus_init_probe_budget{0};
std::atomic<std::uint32_t> g_stimulus_init_captures{0};
std::atomic<std::uint32_t> g_stimulus_init_non_null{0};
std::atomic<std::uint32_t> g_stimulus_init_readable{0};
std::atomic<std::uint32_t> g_stimulus_init_control_ok{0};
std::atomic<std::uint32_t> g_stimulus_init_control_miss{0};
std::atomic<std::uint32_t> g_stimulus_init_first_uid{0};
std::atomic<std::uintptr_t> g_stimulus_init_first_vftable{0};
std::atomic<bool> g_stimulus_init_uid_logged{false};
std::atomic<bool> g_stimulus_init_reported{false};
#if !defined(SPATCH_FINAL_RELEASE)
std::atomic<bool> g_character_surface_probe = false;
std::atomic<unsigned long long> g_character_surface_probe_last_log_tick = 0;
std::atomic<unsigned long long> g_character_surface_observed_water_generation = 0;
#endif

#if !defined(SPATCH_FINAL_RELEASE)
DiagnosticAtomic<unsigned long> g_task_ready_verbose = 0;
DiagnosticAtomic<unsigned long> g_task_dispatch_verbose = 0;
DiagnosticAtomic<unsigned long> g_wait_helper_verbose = 0;
DiagnosticAtomic<unsigned long> g_scaleform_time_verbose = 0;
DiagnosticAtomic<unsigned long> g_scaleform_init_verbose = 0;
DiagnosticAtomic<unsigned long> g_nis_set_play_time_verbose = 0;
DiagnosticAtomic<unsigned long> g_nis_play_verbose = 0;
DiagnosticAtomic<unsigned long> g_nis_bootstrap_verbose = 0;
DiagnosticAtomic<unsigned long> g_nis_owner_verbose = 0;
DiagnosticAtomic<unsigned long> g_nis_actor_state_verbose = 0;
DiagnosticAtomic<unsigned long> g_twitch_probe_verbose = 0;
DiagnosticAtomic<unsigned long> g_frame_flow_verbose = 0;
DiagnosticAtomic<unsigned long> g_cutscene_flow_owner_verbose = 0;
DiagnosticAtomic<unsigned long> g_fog_slicing_verbose = 0;
DiagnosticAtomic<unsigned long> g_aa_probe_verbose = 0;
DiagnosticAtomic<unsigned long> g_aa_fx_probe_verbose = 0;
DiagnosticAtomic<unsigned long> g_post_material_verbose = 0;
DiagnosticAtomic<unsigned long> g_scenery_prepare_verbose = 0;
DiagnosticAtomic<unsigned long> g_scenery_setup_verbose = 0;
DiagnosticAtomic<unsigned long> g_render_scenery_builder_verbose = 0;
DiagnosticAtomic<unsigned long> g_rasterize_bucket_builder_verbose = 0;
DiagnosticAtomic<unsigned long> g_character_regression_verbose = 0;

DiagnosticAtomic<unsigned long long> g_last_summary_tick = 0;
#endif
DiagnosticAtomic<std::uintptr_t> g_last_nis_manager = 0;
DiagnosticAtomic<std::uint32_t> g_last_nis_scene_time_bits = 0;
DiagnosticAtomic<std::uint32_t> g_last_nis_scene_delta_bits = 0;
std::atomic<bool> g_hooks_initialized = false;
std::atomic<bool> g_cleanup_pending = false;
// MinHook enables each detour as it is created and this fork cannot disable a
// target without freeing its original trampoline.  Keep every mutating detour
// transparent until the complete static/hook transaction commits; a late
// failure may retain code hooks for process lifetime, but not partial behavior.
std::atomic<bool> g_behavior_transaction_ready = false;
// The bundled MinHook fork exposes no disable-only operation and frees a
// trampoline from MH_RemoveHook.  Retain all created hooks for process
// lifetime; Main.cpp pins this module, so the OS reclaims the state at process
// exit without ever invalidating an in-flight original call.
std::atomic<bool> g_minhook_retained_process_lifetime = false;
std::vector<std::uintptr_t> g_created_hook_targets;
std::mutex g_hook_lifecycle_mutex;
bool g_hook_creation_transaction_open = false;
hook_guard::Guard g_hook_target_guard;
bool g_rumble_override_attempted = false;
#if !defined(SPATCH_FINAL_RELEASE)
std::mutex g_seen_callback_mutex;
std::vector<std::uintptr_t> g_seen_callbacks;
#endif
std::mutex g_nis_actor_state_mutex;
nisprobe::Tracker g_nis_actor_state_tracker;

nisprobe::SetupResult TrackNisActorSetupLocked(std::uintptr_t actor_state,
                                               std::uintptr_t actor_target) {
    std::lock_guard<std::mutex> lock(g_nis_actor_state_mutex);
    return nisprobe::TrackSetup(g_nis_actor_state_tracker, actor_state, actor_target);
}

nisprobe::RestoreResult TrackNisActorRestoreLocked(std::uintptr_t actor_state) {
    std::lock_guard<std::mutex> lock(g_nis_actor_state_mutex);
    return nisprobe::TrackRestore(g_nis_actor_state_tracker, actor_state);
}

void ResetNisActorTrackerLocked() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_nis_actor_state_mutex);
        nisprobe::ResetTracker(g_nis_actor_state_tracker);
    } catch (...) {
        // The tracker is optional fail-open state.  A process-lifetime mutex is
        // not expected to throw, but no cleanup exception may replace the
        // original engine exception that triggered this reset.
    }
}

struct QueueSnapshot {
    int queued_total = 0;
    int completed_total = 0;
};

struct SceneryCounters {
    unsigned int c0 = 0;
    unsigned int c1 = 0;
    unsigned int c2 = 0;
    unsigned int c3 = 0;
};

enum class BuilderScope : std::uint8_t {
    None = 0,
    SceneryPrepare,
    ScenerySetup,
    RenderScenery,
    RasterizeBucket,
};

thread_local BuilderScope g_current_builder_scope = BuilderScope::None;
thread_local unsigned int g_nis_play_scope_depth = 0;
thread_local unsigned int g_nis_play_scope_calls = 0;
thread_local unsigned int g_nis_play_scope_advanced_calls = 0;
thread_local unsigned int g_nis_play_scope_zero_delta_calls = 0;
thread_local unsigned int g_nis_owner_scope_depth = 0;
thread_local unsigned int g_nis_owner_scope_calls = 0;
thread_local unsigned int g_nis_owner_scope_advanced_calls = 0;
thread_local unsigned int g_nis_owner_scope_zero_delta_calls = 0;
thread_local std::uintptr_t g_current_nis_owner_manager = 0;
thread_local unsigned int g_cutscene_flow_owner_scope_depth = 0;
thread_local unsigned int g_antialias_owner_scope_depth = 0;
thread_local bool g_cutscene_flow_forwarded = false;
thread_local float g_cutscene_flow_forwarded_dt = 0.0f;
thread_local void* g_cutscene_flow_forwarded_callback = nullptr;
thread_local float g_cutscene_flow_input_dt = 0.0f;
thread_local float g_current_cutscene_dt = 0.0f;
thread_local float g_current_nis_owner_dt = 0.0f;
thread_local std::uintptr_t g_last_nis_manager_tls = 0;
thread_local std::uintptr_t g_last_nis_instance_tls = 0;
thread_local float g_last_nis_raw_scene_time_tls = 0.0f;
thread_local float g_last_nis_applied_scene_time_tls = 0.0f;
thread_local bool g_last_nis_scene_history_valid_tls = false;
thread_local float g_last_cutscene_flow_completed_dt_tls = 0.0f;
thread_local CutsceneCadenceTracker g_cutscene_cadence_tracker_tls{};
// Last observed pause flags on this thread, so the frame flow can report the
// delta of the frame that carried the transition. -1 means "not sampled yet".
thread_local int g_last_frame_flow_pause_bits_tls = -1;
thread_local pedestrian_timing::FixedRateScheduler g_pedestrian_throttle_scheduler_tls{};
thread_local bool g_pedestrian_update_delta_valid_tls = false;
thread_local float g_pedestrian_update_delta_tls = 0.0f;
thread_local void* g_pedestrian_manager_tls = nullptr;
thread_local std::uintptr_t g_last_cutscene_owner_tls = 0;
struct CharacterWetnessRuntimeState {
    character_wetness::State policy{};
    unsigned long long observed_water_generation = 0;
    std::uintptr_t effects_component = 0;
    std::uintptr_t sim_object = 0;
    std::uintptr_t character_look_component = 0;
    float baseline_amount = 0.0f;
    float last_written_amount = 0.0f;
    bool owns_amount = false;
};
CharacterWetnessRuntimeState g_character_wetness_state{};
std::mutex g_character_wetness_mutex;
struct CharacterSweatRuntimeState {
    character_sweat::State policy{};
    std::uintptr_t effects_component = 0;
    std::uintptr_t sim_object = 0;
    std::uintptr_t physics_component = 0;
    std::uintptr_t character_look_component = 0;
    float baseline_amount = 0.0f;
    float last_written_amount = 0.0f;
    bool owns_amount = false;
};
CharacterSweatRuntimeState g_character_sweat_state{};
std::mutex g_character_sweat_mutex;

struct CharacterSweatDecisionSnapshot {
    bool sampled = false;
    bool velocity_valid = false;
    float horizontal_speed = 0.0f;
    float run_speed_threshold = 0.0f;
    bool vehicle_state_valid = false;
    bool in_vehicle = false;
    bool combat_valid = false;
    bool in_combat = false;
    character_sweat::ExertionDecision decision{};
};

// CharacterEffects::Update runs for every visible character, not just the
// player. Keep per-component state so an NPC's wetness/sweat timer cannot
// overwrite another character's material or inherit Wei's timer. Entries are
// bounded by inactivity pruning; all pointer reads/writes remain guarded by
// SafeRead/SafeWrite before a stale pooled component can be touched.
struct NpcCharacterEffectsRuntimeState {
    std::uintptr_t effects_component = 0;
    std::uintptr_t sim_object = 0;
    std::uintptr_t physics_component = 0;
    std::uintptr_t character_look_component = 0;
    character_wetness::State wetness_policy{};
    std::uint32_t last_active_wet_surface_uid = 0;
    float wetness_baseline_amount = 0.0f;
    float wetness_last_written_amount = 0.0f;
    bool wetness_owns_amount = false;
    character_sweat::State sweat_policy{};
    float sweat_baseline_amount = 0.0f;
    float sweat_last_written_amount = 0.0f;
    bool sweat_owns_amount = false;
    bool water_contact_pending = false;
    unsigned long long last_seen_tick = 0;
};

constexpr unsigned long long kNpcCharacterStateKeepAliveMs = 10000;
constexpr unsigned long long kNpcCharacterStatePruneIntervalMs = 1000;
constexpr std::size_t kNpcCharacterStateMaximumEntries = 512;
std::unordered_map<std::uintptr_t, NpcCharacterEffectsRuntimeState>
    g_npc_character_effects_states;
std::mutex g_npc_character_effects_mutex;
unsigned long long g_npc_character_effects_last_prune_tick = 0;

std::unordered_map<std::uintptr_t, unsigned long long> g_character_combat_until_ticks;
std::mutex g_character_combat_activity_mutex;
struct CutsceneOwnerTimingState {
    float input_dt = 0.0f;
    float current_dt = 0.0f;
    bool forwarded = false;
    float forwarded_dt = 0.0f;
    void* forwarded_callback = nullptr;
};
thread_local std::array<CutsceneOwnerTimingState, 256> g_cutscene_owner_timing_stack{};

void ResetTrackedCutsceneCadence() {
    ResetCutsceneCadenceTracker(g_cutscene_cadence_tracker_tls);
}

void ResetTrackedCutsceneTimeline() {
    ResetTrackedCutsceneCadence();
    g_last_nis_scene_history_valid_tls = false;
}

float TrackCutsceneCadence(float live_delta) {
    return TrackCutsceneBaseDelta(
        g_cutscene_cadence_tracker_tls, live_delta, g_config.cutscene_fps);
}

template <typename T>
bool SafeRead(std::uintptr_t address, T& out_value) {
    if (address == 0) {
        out_value = T{};
        return false;
    }
    __try {
        out_value = *reinterpret_cast<const T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out_value = T{};
        return false;
    }
}

bool SafeCopyBytes(std::uintptr_t source_address,
                   void* destination,
                   std::size_t size) noexcept {
    if (source_address == 0 || destination == nullptr || size == 0) {
        return false;
    }
    __try {
        std::memcpy(destination,
                    reinterpret_cast<const void*>(source_address),
                    size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::memset(destination, 0, size);
        return false;
    }
}

template <typename T>
bool SafeWrite(std::uintptr_t address, const T& value) {
    if (address == 0) {
        return false;
    }
    __try {
        *reinterpret_cast<T*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void AppendCharacterTraceLine(const char* stage,
                              void* health_component,
                              void* anim_component,
                              void* hitreact_component,
                              int damage,
                              bool projectile_damage,
                              bool result) {
#if defined(SPATCH_FINAL_RELEASE)
    (void)stage;
    (void)health_component;
    (void)anim_component;
    (void)hitreact_component;
    (void)damage;
    (void)projectile_damage;
    (void)result;
    return;
#else
    if (!g_use_latest_steam_layout) {
        return;
    }

    if (g_character_trace_budget.load(std::memory_order_relaxed) <= 0) {
        return;
    }
    long budget_before = g_character_trace_budget.fetch_sub(1);
    if (budget_before <= 0) {
        g_character_trace_budget.fetch_add(1);
        return;
    }

    const std::filesystem::path trace_path =
        g_config_path.empty() ? std::filesystem::path(L"SPatch.chartrace.log")
                              : (g_config_path.parent_path() / L"SPatch.chartrace.log");

    char line[512]{};
    const int line_length = _snprintf_s(line,
                                        sizeof(line),
                                        _TRUNCATE,
                                        "%s hc=%p anim=%p hitreact=%p damage=%d projectile=%d result=%d\r\n",
                                        stage,
                                        health_component,
                                        anim_component,
                                        hitreact_component,
                                        damage,
                                        projectile_damage ? 1 : 0,
                                        result ? 1 : 0);
    if (line_length <= 0) {
        return;
    }

    HANDLE file = CreateFileW(trace_path.c_str(),
                              FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(line_length), &written, nullptr);
    CloseHandle(file);
#endif
}

#if defined(SPATCH_FINAL_RELEASE)
#define ShouldLogVerbose(counter) false
#else
bool ShouldLogVerbose(DiagnosticAtomic<unsigned long>& counter) noexcept {
    if (g_config.max_verbose_events == 0) {
        return false;
    }
    const unsigned long current = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    return current <= g_config.max_verbose_events;
}
#endif

std::uint32_t FloatToBits(float value);
#if defined(SPATCH_FINAL_RELEASE)
#define MaybeWriteSummary() ((void)0)
#else
void MaybeWriteSummary();
#endif

std::uintptr_t ReadSimObjectFromComponent(void* component) {
    if (component == nullptr) {
        return 0;
    }

    // SimComponent inherits qSafePointerNode:
    //   vptr (0x00) + qList (0x08..0x17) => base size 0x18
    // Then:
    //   m_TypeUID (0x18), m_NameUID (0x1C), m_Flags (0x20), m_SimObjIndex (0x22),
    //   padding (0x24), m_pSimObject (0x28)
    std::uintptr_t sim_object = 0;
    SafeRead(reinterpret_cast<std::uintptr_t>(component) + 0x28, sim_object);
    return sim_object;
}

std::uintptr_t ResolveSimObjectComponent(std::uintptr_t sim_object, std::uint32_t type_uid) {
    if (sim_object == 0 || g_simobject_get_component == nullptr) {
        return 0;
    }

    return reinterpret_cast<std::uintptr_t>(
        g_simobject_get_component(reinterpret_cast<void*>(sim_object), type_uid));
}

#if !defined(SPATCH_FINAL_RELEASE)
bool FieldProbeRequested(const std::filesystem::path& trigger_path,
                         const wchar_t* environment_name) noexcept {
    wchar_t value[16]{};
    const DWORD length = GetEnvironmentVariableW(
        environment_name, value, static_cast<DWORD>(std::size(value)));
    if (length != 0 && length < std::size(value)) {
        wchar_t* end = nullptr;
        if (std::wcstol(value, &end, 10) != 0 && end != value) {
            return true;
        }
    }

    std::error_code error;
    return !trigger_path.empty() && std::filesystem::is_regular_file(trigger_path, error);
}
#endif

void DetourCharacterAnimationApplyCharredEffect(void* character_animation_component, float amount) {
    g_character_charred_anim_count.fetch_add(1);
    g_last_character_damage_anim_component.store(
        reinterpret_cast<std::uintptr_t>(character_animation_component));
    g_last_character_charred_amount_bits.store(FloatToBits(amount));

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_charred_anim component=0x%p amount=%.3f",
                   character_animation_component,
                   amount);
    }

    g_character_animation_apply_charred_effect_original(character_animation_component, amount);
    MaybeWriteSummary();
}

void DetourCharacterAnimationApplyPaintEffect(void* character_animation_component,
                                              bool enable,
                                              float r,
                                              float g,
                                              float b) {
    g_character_paint_anim_count.fetch_add(1);
    g_last_character_damage_anim_component.store(
        reinterpret_cast<std::uintptr_t>(character_animation_component));
    g_last_character_paint_enable.store(enable ? 1u : 0u);
    g_last_character_paint_r_bits.store(FloatToBits(r));
    g_last_character_paint_g_bits.store(FloatToBits(g));
    g_last_character_paint_b_bits.store(FloatToBits(b));

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_paint_anim component=0x%p enable=%d rgb=%.3f/%.3f/%.3f",
                   character_animation_component,
                   enable ? 1 : 0,
                   r,
                   g,
                   b);
    }

    g_character_animation_apply_paint_effect_original(character_animation_component, enable, r, g, b);
    MaybeWriteSummary();
}

void DetourDamageRigApplyCharredEffect(void* damage_rig, float amount) {
    g_character_charred_rig_count.fetch_add(1);
    g_last_character_damage_rig.store(reinterpret_cast<std::uintptr_t>(damage_rig));
    g_last_character_charred_amount_bits.store(FloatToBits(amount));

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_charred_rig rig=0x%p amount=%.3f", damage_rig, amount);
    }

    g_damage_rig_apply_charred_effect_original(damage_rig, amount);
    MaybeWriteSummary();
}

void DetourDamageRigApplyPaintEffect(void* damage_rig, bool enable, float r, float g, float b) {
    g_character_paint_rig_count.fetch_add(1);
    g_last_character_damage_rig.store(reinterpret_cast<std::uintptr_t>(damage_rig));
    g_last_character_paint_enable.store(enable ? 1u : 0u);
    g_last_character_paint_r_bits.store(FloatToBits(r));
    g_last_character_paint_g_bits.store(FloatToBits(g));
    g_last_character_paint_b_bits.store(FloatToBits(b));

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_paint_rig rig=0x%p enable=%d rgb=%.3f/%.3f/%.3f",
                   damage_rig,
                   enable ? 1 : 0,
                   r,
                   g,
                   b);
    }

    g_damage_rig_apply_paint_effect_original(damage_rig, enable, r, g, b);
    MaybeWriteSummary();
}

void DetourCharacterPaintConsumer(std::uintptr_t character_effects_component,
                                  std::uintptr_t paint_block_owner) {
    g_character_paint_consumer_count.fetch_add(1);
    g_last_character_paint_consumer_component.store(character_effects_component);
    g_last_character_paint_consumer_block.store(paint_block_owner);

    const auto read_ptr = [](std::uintptr_t base, std::uintptr_t offset) -> std::uintptr_t {
        std::uintptr_t value = 0;
        if (base != 0) {
            SafeRead(base + offset, value);
        }
        return value;
    };

    bool enable = false;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;

    const std::uintptr_t paint_lanes = read_ptr(paint_block_owner, 0x60);
    if (paint_lanes != 0) {
        const std::uintptr_t lane0 = read_ptr(paint_lanes, 0x00);
        const std::uintptr_t lane1 = read_ptr(paint_lanes, 0x08);
        const std::uintptr_t lane2 = read_ptr(paint_lanes, 0x10);
        const std::uintptr_t lane3 = read_ptr(paint_lanes, 0x18);
        const std::uintptr_t lane0_node = read_ptr(lane0, 0x08);
        const std::uintptr_t lane1_node = read_ptr(lane1, 0x08);
        const std::uintptr_t lane2_node = read_ptr(lane2, 0x08);
        const std::uintptr_t lane3_node = read_ptr(lane3, 0x08);
        enable = read_ptr(lane0_node, 0x20) != 0;
        SafeRead(lane1_node + 0x20, r);
        SafeRead(lane2_node + 0x20, g);
        SafeRead(lane3_node + 0x20, b);
        g_last_character_paint_enable.store(enable ? 1u : 0u);
        g_last_character_paint_r_bits.store(FloatToBits(r));
        g_last_character_paint_g_bits.store(FloatToBits(g));
        g_last_character_paint_b_bits.store(FloatToBits(b));
    }

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF(
            "character_paint_consumer component=0x%p block=0x%p enable=%d rgb=%.3f/%.3f/%.3f",
            reinterpret_cast<void*>(character_effects_component),
            reinterpret_cast<void*>(paint_block_owner),
            enable ? 1 : 0,
            r,
            g,
            b);
    }

    g_character_paint_consumer_original(character_effects_component, paint_block_owner);
    MaybeWriteSummary();
}

void* DetourCharacterEffectDispatchOwner() {
    g_character_effect_dispatch_owner_count.fetch_add(1);
    void* dispatch_owner = g_character_effect_dispatch_owner_original();
    g_last_character_effect_dispatch_owner.store(reinterpret_cast<std::uintptr_t>(dispatch_owner));

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_effect_dispatch_owner owner=0x%p", dispatch_owner);
    }

    return dispatch_owner;
}

void DetourCharacterPaintOwner(std::uintptr_t owner, std::uintptr_t character_effects_component) {
    g_character_paint_owner_count.fetch_add(1);
    g_last_character_paint_owner_ptr.store(owner);
    g_last_character_paint_owner_component.store(character_effects_component);

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_paint_owner owner=0x%p component=0x%p",
                   reinterpret_cast<void*>(owner),
                   reinterpret_cast<void*>(character_effects_component));
    }

    g_character_paint_owner_original(owner, character_effects_component);
    MaybeWriteSummary();
}

void DetourCharacterEffectDispatchConsume(void* dispatch_owner, void* character_effects_component) {
    g_character_effect_dispatch_consume_count.fetch_add(1);
    g_last_character_effect_dispatch_owner.store(reinterpret_cast<std::uintptr_t>(dispatch_owner));
    g_last_character_effect_dispatch_component.store(
        reinterpret_cast<std::uintptr_t>(character_effects_component));

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_effect_dispatch_consume owner=0x%p component=0x%p",
                   dispatch_owner,
                   character_effects_component);
    }

    g_character_effect_dispatch_consume_original(dispatch_owner, character_effects_component);
    MaybeWriteSummary();
}

void DetourCharacterEffectQueueBuilder(std::uintptr_t owner,
                                       std::uintptr_t character_effects_component,
                                       std::uint32_t mode) {
    g_character_effect_queue_builder_count.fetch_add(1);
    g_last_character_effect_queue_builder_owner.store(owner);
    g_last_character_effect_queue_builder_component.store(character_effects_component);
    g_last_character_effect_queue_builder_mode.store(mode);

    const std::uintptr_t tracked_wet_component = g_last_character_wet_component.load();
    const bool is_tracked_wet_component =
        character_effects_component != 0 && tracked_wet_component != 0 &&
        character_effects_component == tracked_wet_component;
    if (is_tracked_wet_component) {
        g_character_effect_queue_builder_tracked_count.fetch_add(1);
    }

    if (ShouldLogVerbose(g_character_regression_verbose) &&
        (is_tracked_wet_component || g_character_effect_queue_builder_count.load() <= 4)) {
        log::InfoF("character_effect_queue_builder owner=0x%p component=0x%p mode=%u tracked=%u",
                   reinterpret_cast<void*>(owner),
                   reinterpret_cast<void*>(character_effects_component),
                   mode,
                   is_tracked_wet_component ? 1u : 0u);
    }

    g_character_effect_queue_builder_original(owner, character_effects_component, mode);
    MaybeWriteSummary();
}

std::uintptr_t RemapRvaForCurrentBuild(std::uintptr_t rva) {
    if (!g_use_latest_steam_layout) {
        return rva;
    }

    switch (rva) {
        case kTaskReadyRva:
            return kLatestSteamTaskReadyRva;
        case kTaskDispatchRva:
            return kLatestSteamTaskDispatchRva;
        // Voice chain (remap evidence: site-evidence-wave1.md).
        case kConversationDispatcherRva:
            return kLatestSteamConversationDispatcherRva;
        case kSpeechWrapperRva:
            return kLatestSteamSpeechWrapperRva;
        case kSpeakByNameRva:
            return kLatestSteamSpeakByNameRva;
        case kScriptedConversationEntryRva:
            return kLatestSteamScriptedConversationEntryRva;
        case kAudioNameToIdRva:
            return kLatestSteamAudioNameToIdRva;
        case kPlayerSimObjectGlobalRva:
            return kLatestSteamPlayerSimObjectGlobalRva;
        // Wave-2 remaps (site-evidence-wave2.md): HUD/radar, heat meter,
        // scaleform, DOF, activity timer, sweat inputs, stock-AA hooks.
        case kHudUiApplyRva:
            return kLatestSteamHudUiApplyRva;
        case kHudHealthBarSetterRva:
            return kLatestSteamHudHealthBarSetterRva;
        case kHeatMeterApplyRva:
            return kLatestSteamHeatMeterApplyRva;
        case kGfxSetVariableRva:
            return kLatestSteamGfxSetVariableRva;
        case kScaleformTimeRva:
            return kLatestSteamScaleformTimeRva;
        case kScaleformInitRva:
            return kLatestSteamScaleformInitRva;
        case kDofSetParametersRva:
            return kLatestSteamDofSetParametersRva;
        case kPass8ActivityTimerUpdateRva:
            return kLatestSteamPass8ActivityTimerUpdateRva;
        case kCharacterPhysicsGetVelocityRva:
            return kLatestSteamCharacterPhysicsGetVelocityRva;
        case kUiIsPlayerInCombatRva:
            return kLatestSteamUiIsPlayerInCombatRva;
        case kAntiAliasOwnerRva:
            return kLatestSteamAntiAliasOwnerRva;
        case kAntiAliasFxHandoffRva:
            return kLatestSteamAntiAliasFxHandoffRva;
        case kTimeDilateApplyRva:
            return kLatestSteamTimeDilateApplyRva;
        case kTimeOfDayClockUpdateRva:
            return kLatestSteamTimeOfDayClockUpdateRva;
        case kAnalogDriverApplyRva:
            return kLatestSteamAnalogDriverApplyRva;
        case kTransmissionShiftApplyRva:
            return kLatestSteamTransmissionShiftApplyRva;
        case kTransmissionRatioApplyRva:
            return kLatestSteamTransmissionRatioApplyRva;
        case kVehicleEngineApplyRva:
            return kLatestSteamVehicleEngineApplyRva;
        case kSteeringCallbackRva:
            return kLatestSteamSteeringCallbackRva;
        case kSteeringInputRva:
            return kLatestSteamSteeringInputRva;
        case kNisSetPlayTimeRva:
            return kLatestSteamNisSetPlayTimeRva;
        case kNisOwnerRva:
            return kLatestSteamNisOwnerRva;
        case kFrameFlowRva:
            return kLatestSteamFrameFlowRva;
        case kCutsceneFlowOwnerRva:
            return kLatestSteamCutsceneFlowOwnerRva;
        case fog_restoration::kLegacySetterRva:
            return fog_restoration::kLatestSteamSetterRva;
        case kFogSlicingModeRva:
            return kLatestSteamFogSlicingModeRva;
        case kPresentBufferRva:
            return kLatestSteamPresentBufferRva;
        case kAverageWindowInitializeRva:
            return kLatestSteamAverageWindowInitializeRva;
        case kMaterialOnLoadRva:
            return kLatestSteamMaterialOnLoadRva;
        case kUiIsGamePausedRva:
            return kLatestSteamUiIsGamePausedRva;
        case kIsCharacterInVehicleRva:
            return kLatestSteamIsCharacterInVehicleRva;
        case kHealthApplyDamageRva:
            return kLatestSteamHealthApplyDamageRva;
        case kSimObjectGetComponentRva:
            return kLatestSteamSimObjectGetComponentRva;
        case kTimeOfDayAccessorRva:
            return kLatestSteamTimeOfDayAccessorRva;
        case kCharacterHandleWaterCollisionRva:
            return kLatestSteamCharacterHandleWaterCollisionRva;
        case kCharacterEffectsUpdateRva:
            return kLatestSteamCharacterEffectsUpdateRva;
        case kCharacterEffectDispatchConsumeRva:
            return kLatestSteamCharacterEffectDispatchConsumeRva;
        case kCharacterEffectQueueBuilderRva:
            return kLatestSteamCharacterEffectQueueBuilderRva;
        case kDamageRigApplyPaintEffectRva:
            return kLatestSteamDamageRigApplyPaintEffectRva;
        case kCharacterDamageRigResetDamageRva:
            return kLatestSteamCharacterDamageRigResetDamageRva;
        case kPedestrianFrameRateThrottleRva:
            return kLatestSteamPedestrianFrameRateThrottleRva;
        case kPedestrianSpawnUpdateRva:
            return kLatestSteamPedestrianSpawnUpdateRva;
        case kPcFileReadRva:
            return kLatestSteamPcFileReadRva;
        case kPcFileSeekRva:
            return kLatestSteamPcFileSeekRva;
        case kPcFileTellRva:
            return kLatestSteamPcFileTellRva;
        case kPcFileSizeRva:
            return kLatestSteamPcFileSizeRva;
        case kQFileReadAtRva:
            return kLatestSteamQFileReadAtRva;
        case kQFileWriteAtRva:
            return kLatestSteamQFileWriteAtRva;
        case kQFileReadyRva:
            return kLatestSteamQFileReadyRva;
        case kQcmpDecompressRva:
            return kLatestSteamQcmpDecompressRva;
        case kStreamFileOpenRva:
            return kLatestSteamStreamFileOpenRva;
        case kStreamFileCloseRva:
            return kLatestSteamStreamFileCloseRva;
        case kResourceChunkDispatchRva:
            return kLatestSteamResourceChunkDispatchRva;
        default:
            return rva;
        }
}

std::uintptr_t ResolveAddress(std::uintptr_t rva) noexcept {
    const std::uintptr_t mapped_rva = RemapRvaForCurrentBuild(rva);
    if (g_module_base == 0 || g_module_end <= g_module_base ||
        mapped_rva >= g_module_end - g_module_base) {
        return 0;
    }
    return g_module_base + mapped_rva;
}

std::uint32_t FloatToBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float BitsToFloat(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float Clamp01(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

float ScalePercent(float value, int percent) {
    return value * (static_cast<float>(percent) / 100.0f);
}

bool NearlyEqual(float a, float b, float epsilon = 0.0005f) {
    return std::fabs(a - b) <= epsilon;
}

std::uintptr_t SafeResolveSimObjectComponent(std::uintptr_t sim_object,
                                              std::uint32_t type_uid) noexcept {
    std::uintptr_t component = 0;
    __try {
        component = ResolveSimObjectComponent(sim_object, type_uid);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        component = 0;
    }
    return component;
}

// Revalidates a cached component binding against the SimComponent base
// fields (+0x18 type UID, +0x28 owning SimObject). Returns the component when
// it is still bound to expected_sim_object, else 0 so the caller re-resolves
// instead of trusting a pooled component recycled by a spawn/teardown burst
// (fight-club pre-fight population). All reads are SEH-guarded; a stale or
// torn component degrades to re-resolve, never to a guess.
std::uintptr_t RevalidateComponentBinding(std::uintptr_t cached_component,
                                          std::uint32_t expected_type_uid,
                                          std::uintptr_t expected_sim_object) noexcept {
    if (cached_component == 0 || expected_sim_object == 0) {
        return 0;
    }
    std::uint32_t actual_type_uid = 0;
    std::uintptr_t actual_sim_object = 0;
    if (!SafeRead(cached_component + character_hooks::kSimComponentTypeUidOffset,
                  actual_type_uid) ||
        !SafeRead(cached_component + character_hooks::kSimComponentSimObjectOffset,
                  actual_sim_object) ||
        !character_hooks::IsComponentBindingValid(actual_type_uid,
                                                  expected_type_uid,
                                                  actual_sim_object,
                                                  expected_sim_object)) {
        return 0;
    }
    return cached_component;
}

bool IsValidCharacterWetnessAmount(float amount) noexcept {
    return std::isfinite(amount) && amount >= -1.0f && amount <= 1.0f;
}

void RestoreOwnedCharacterWetness(CharacterWetnessRuntimeState& state) {
    if (!state.owns_amount || state.character_look_component == 0) {
        state.owns_amount = false;
        return;
    }

    float current = 0.0f;
    if (SafeRead(state.character_look_component + 0xB8, current) &&
        NearlyEqual(current, state.last_written_amount, 0.002f)) {
        SafeWrite(state.character_look_component + 0xB8, state.baseline_amount);
    }
    state.owns_amount = false;
}

void ApplyCharacterWetnessRestore(void* character_effects_component,
                                  float delta_seconds,
                                  float weather_state,
                                  float weather_surface_wetness,
                                  float override_surface_wetness) {
    const std::uintptr_t effects_component =
        reinterpret_cast<std::uintptr_t>(character_effects_component);
    if (!g_config.restore_character_wetness || effects_component == 0) {
        return;
    }

    const std::uintptr_t sim_object = ReadSimObjectFromComponent(character_effects_component);
    if (sim_object == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_character_wetness_mutex);
    CharacterWetnessRuntimeState& state = g_character_wetness_state;
    const character_wetness::Timing timing{
        static_cast<float>(g_config.wetness_full_time_seconds),
        static_cast<float>(g_config.wetness_fade_time_seconds)};

    const unsigned long long water_generation =
        g_player_water_collision_generation.load(std::memory_order_acquire);
    if (water_generation != state.observed_water_generation) {
        state.observed_water_generation = water_generation;
        if (g_player_water_collision_component.load(std::memory_order_acquire) ==
            effects_component) {
            character_wetness::MarkWaterCollision(state.policy, timing);
        }
    }

    const bool raining = std::isfinite(weather_state) && weather_state > 1.0f;
    const float effective_surface_wetness =
        IsValidCharacterWetnessAmount(override_surface_wetness) &&
                override_surface_wetness >= 0.0f
            ? override_surface_wetness
            : weather_surface_wetness;
    const character_wetness::StepResult step = character_wetness::Advance(
        state.policy, delta_seconds, raining, effective_surface_wetness, timing);

    if (state.effects_component != effects_component || state.sim_object != sim_object) {
        RestoreOwnedCharacterWetness(state);
        state.effects_component = effects_component;
        state.sim_object = sim_object;
        state.character_look_component = 0;
    }

    if (state.character_look_component == 0) {
        state.character_look_component =
            SafeResolveSimObjectComponent(sim_object, 0xCC000001u);
    }
    const std::uintptr_t character_look = state.character_look_component;
    if (character_look == 0) {
        return;
    }

    std::uint32_t component_type_uid = 0;
    std::uintptr_t component_sim_object = 0;
    float current = 0.0f;
    if (!SafeRead(character_look + 0x18, component_type_uid) ||
        component_type_uid != 0xCC000001u ||
        !SafeRead(character_look + 0x28, component_sim_object) ||
        component_sim_object != sim_object || !SafeRead(character_look + 0xB8, current) ||
        !IsValidCharacterWetnessAmount(current)) {
        RestoreOwnedCharacterWetness(state);
        state.character_look_component = 0;
        return;
    }

    if (!step.active) {
        RestoreOwnedCharacterWetness(state);
        return;
    }

    const float desired = step.amount;
    if (state.owns_amount && character_wetness::ShouldYieldToStrongerOwner(
                                 current, state.last_written_amount, desired)) {
        // CharacterLook's stock update writes zero again every frame in DE.
        // Keep the physical water state authoritative over a lower value, but
        // yield to a stronger scripted/rain value and reacquire naturally if
        // that owner later releases the field while water is still active.
        state.owns_amount = false;
    }

    if (!state.owns_amount) {
        if (current + 0.0005f >= desired) {
            return;
        }
        state.baseline_amount = current;
    } else if (NearlyEqual(current, desired)) {
        return;
    }

    if (!SafeWrite(character_look + 0xB8, desired)) {
        state.owns_amount = false;
        return;
    }

    state.last_written_amount = desired;
    state.owns_amount = true;
    g_character_wet_force_count.fetch_add(1);
    float verified = 0.0f;
    if (SafeRead(character_look + 0xB8, verified) && NearlyEqual(verified, desired)) {
        g_character_wet_force_verify_count.fetch_add(1);
    }
}

bool IsValidCharacterSweatAmount(float amount) noexcept {
    return std::isfinite(amount) && amount >= -1.0f && amount <= 1.0f;
}

void RestoreOwnedCharacterSweat(CharacterSweatRuntimeState& state) {
    if (state.owns_amount && state.character_look_component != 0) {
        float current = 0.0f;
        if (SafeRead(state.character_look_component + 0xBC, current) &&
            NearlyEqual(current, state.last_written_amount, 0.002f)) {
            SafeWrite(state.character_look_component + 0xBC, state.baseline_amount);
        }
    }
    state = {};
    g_character_sweat_active_component.store(0, std::memory_order_release);
}

// Release only the material ownership while retaining the component identity
// and exertion timer.  The sweat policy has an onset phase during which its
// amount is intentionally zero; clearing the whole runtime state here would
// restart that timer every CharacterEffects::Update and make sweat impossible
// to trigger at the configured onset.
void ReleaseOwnedCharacterSweat(CharacterSweatRuntimeState& state) {
    if (state.owns_amount && state.character_look_component != 0) {
        float current = 0.0f;
        if (SafeRead(state.character_look_component + 0xBC, current) &&
            NearlyEqual(current, state.last_written_amount, 0.002f)) {
            SafeWrite(state.character_look_component + 0xBC, state.baseline_amount);
        }
    }
    state.owns_amount = false;
    state.last_written_amount = 0.0f;
}

void RestoreOwnedNpcCharacterWetness(NpcCharacterEffectsRuntimeState& state) {
    if (!state.wetness_owns_amount || state.character_look_component == 0) {
        state.wetness_owns_amount = false;
        state.wetness_policy = {};
        return;
    }

    float current = 0.0f;
    if (SafeRead(state.character_look_component + 0xB8, current) &&
        NearlyEqual(current, state.wetness_last_written_amount, 0.002f)) {
        SafeWrite(state.character_look_component + 0xB8, state.wetness_baseline_amount);
    }
    state.wetness_owns_amount = false;
    state.wetness_policy = {};
    state.wetness_baseline_amount = 0.0f;
    state.wetness_last_written_amount = 0.0f;
}

void RestoreOwnedNpcCharacterSweat(NpcCharacterEffectsRuntimeState& state) {
    if (!state.sweat_owns_amount || state.character_look_component == 0) {
        state.sweat_owns_amount = false;
        state.sweat_policy = {};
        return;
    }

    float current = 0.0f;
    if (SafeRead(state.character_look_component + 0xBC, current) &&
        NearlyEqual(current, state.sweat_last_written_amount, 0.002f)) {
        SafeWrite(state.character_look_component + 0xBC, state.sweat_baseline_amount);
    }
    state.sweat_owns_amount = false;
    state.sweat_policy = {};
    state.sweat_baseline_amount = 0.0f;
    state.sweat_last_written_amount = 0.0f;
}

// See ReleaseOwnedCharacterSweat.  Keep the per-NPC onset/exertion timer
// alive when the policy is still dry, otherwise every inactive step would
// erase the very state needed to cross the onset threshold.
void ReleaseOwnedNpcCharacterSweat(NpcCharacterEffectsRuntimeState& state) {
    if (state.sweat_owns_amount && state.character_look_component != 0) {
        float current = 0.0f;
        if (SafeRead(state.character_look_component + 0xBC, current) &&
            NearlyEqual(current, state.sweat_last_written_amount, 0.002f)) {
            SafeWrite(state.character_look_component + 0xBC,
                      state.sweat_baseline_amount);
        }
    }
    state.sweat_owns_amount = false;
    state.sweat_last_written_amount = 0.0f;
}

void ResetNpcCharacterEffectsState(NpcCharacterEffectsRuntimeState& state) {
    RestoreOwnedNpcCharacterWetness(state);
    RestoreOwnedNpcCharacterSweat(state);
    state = {};
}

void PruneNpcCharacterEffectsStatesLocked(unsigned long long now) {
    if (now < g_npc_character_effects_last_prune_tick ||
        now - g_npc_character_effects_last_prune_tick <
            kNpcCharacterStatePruneIntervalMs) {
        return;
    }
    g_npc_character_effects_last_prune_tick = now;
    for (auto it = g_npc_character_effects_states.begin();
         it != g_npc_character_effects_states.end();) {
        const auto& state = it->second;
        const bool stale = state.last_seen_tick == 0 ||
                           (now >= state.last_seen_tick &&
                            now - state.last_seen_tick >
                                kNpcCharacterStateKeepAliveMs);
        if (!stale) {
            ++it;
            continue;
        }
        ResetNpcCharacterEffectsState(it->second);
        it = g_npc_character_effects_states.erase(it);
    }
    while (g_npc_character_effects_states.size() > kNpcCharacterStateMaximumEntries) {
        auto oldest = g_npc_character_effects_states.end();
        for (auto it = g_npc_character_effects_states.begin();
             it != g_npc_character_effects_states.end();
             ++it) {
            if (oldest == g_npc_character_effects_states.end() ||
                it->second.last_seen_tick < oldest->second.last_seen_tick) {
                oldest = it;
            }
        }
        if (oldest == g_npc_character_effects_states.end()) {
            break;
        }
        ResetNpcCharacterEffectsState(oldest->second);
        g_npc_character_effects_states.erase(oldest);
    }
}

NpcCharacterEffectsRuntimeState* GetNpcCharacterEffectsStateLocked(
    std::uintptr_t effects_component,
    std::uintptr_t sim_object,
    unsigned long long now) {
    if (effects_component == 0 || sim_object == 0) {
        return nullptr;
    }

    try {
        auto [it, inserted] = g_npc_character_effects_states.try_emplace(effects_component);
        NpcCharacterEffectsRuntimeState& state = it->second;
        if (inserted || state.effects_component != effects_component ||
            state.sim_object != sim_object) {
            const bool water_contact_pending = state.water_contact_pending;
            if (!inserted) {
                ResetNpcCharacterEffectsState(state);
            }
            state.effects_component = effects_component;
            state.sim_object = sim_object;
            state.water_contact_pending = water_contact_pending;
        }
        state.last_seen_tick = now;
        return &state;
    } catch (...) {
        // NPCs are an optional enhancement. If the process cannot grow the
        // bounded cache, leave the stock path untouched for this update.
        return nullptr;
    }
}

void MarkNpcCharacterWaterContact(std::uintptr_t effects_component) noexcept {
    if (effects_component == 0) {
        return;
    }
    const unsigned long long now = GetTickCount64();
    try {
        std::lock_guard<std::mutex> lock(g_npc_character_effects_mutex);
        PruneNpcCharacterEffectsStatesLocked(now);
        NpcCharacterEffectsRuntimeState* state = nullptr;
        const auto existing = g_npc_character_effects_states.find(effects_component);
        if (existing != g_npc_character_effects_states.end()) {
            // The water callback does not carry a sim-object pointer. Preserve
            // the real identity resolved by an earlier effects update instead
            // of replacing it with the pre-update sentinel.
            existing->second.last_seen_tick = now;
            state = &existing->second;
        } else {
            state = GetNpcCharacterEffectsStateLocked(effects_component, 1, now);
        }
        if (state != nullptr) {
            // A water callback can arrive before the first effects update. A
            // sentinel sim object is replaced by the real one on that update.
            state->water_contact_pending = true;
        }
    } catch (...) {
        // Fail open if allocation/locking infrastructure is unavailable.
    }
}

void MarkCharacterCombatActivity(std::uintptr_t sim_object) noexcept {
    if (sim_object == 0 || g_config.sweat_combat_time_seconds <= 0) {
        return;
    }
    const unsigned long long now = GetTickCount64();
    const unsigned long long duration =
        static_cast<unsigned long long>(g_config.sweat_combat_time_seconds) * 1000ULL;
    const unsigned long long until =
        now > (std::numeric_limits<unsigned long long>::max() - duration)
            ? std::numeric_limits<unsigned long long>::max()
            : now + duration;
    try {
        std::lock_guard<std::mutex> lock(g_character_combat_activity_mutex);
        auto& current = g_character_combat_until_ticks[sim_object];
        current = std::max(current, until);
        for (auto it = g_character_combat_until_ticks.begin();
             it != g_character_combat_until_ticks.end();) {
            if (it->second < now) {
                it = g_character_combat_until_ticks.erase(it);
            } else {
                ++it;
            }
        }
        while (g_character_combat_until_ticks.size() > kNpcCharacterStateMaximumEntries) {
            auto oldest = g_character_combat_until_ticks.begin();
            for (auto it = g_character_combat_until_ticks.begin();
                 it != g_character_combat_until_ticks.end();
                 ++it) {
                if (it->second < oldest->second) {
                    oldest = it;
                }
            }
            g_character_combat_until_ticks.erase(oldest);
        }
    } catch (...) {
        // Combat-driven sweat is optional; never affect the stock damage path.
    }
}

bool IsCharacterCombatActive(std::uintptr_t sim_object,
                             unsigned long long now) noexcept {
    if (sim_object == 0) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(g_character_combat_activity_mutex);
        const auto it = g_character_combat_until_ticks.find(sim_object);
        if (it == g_character_combat_until_ticks.end()) {
            return false;
        }
        if (it->second < now) {
            g_character_combat_until_ticks.erase(it);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void ResetNpcCharacterEffectsStates() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_npc_character_effects_mutex);
        for (auto& [component, state] : g_npc_character_effects_states) {
            (void)component;
            ResetNpcCharacterEffectsState(state);
        }
        g_npc_character_effects_states.clear();
        g_npc_character_effects_last_prune_tick = 0;
    } catch (...) {
        // Teardown must not replace the original shutdown path with an
        // exception from optional NPC bookkeeping.
    }
    try {
        std::lock_guard<std::mutex> lock(g_character_combat_activity_mutex);
        g_character_combat_until_ticks.clear();
    } catch (...) {
    }
}

bool ReadPlayerHorizontalSpeedFromComponent(std::uintptr_t physics_component,
                                            float& speed) noexcept {
    speed = 0.0f;
    if (physics_component == 0 || g_character_physics_get_velocity == nullptr) {
        return false;
    }

    float velocity[3]{};
    bool call_succeeded = false;
    __try {
        call_succeeded = g_character_physics_get_velocity(
                             reinterpret_cast<void*>(physics_component), velocity) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        call_succeeded = false;
    }
    if (!call_succeeded || !std::isfinite(velocity[0]) || !std::isfinite(velocity[1]) ||
        !std::isfinite(velocity[2])) {
        return false;
    }

    const float horizontal_speed = std::sqrt((velocity[0] * velocity[0]) +
                                             (velocity[1] * velocity[1]));
    if (!std::isfinite(horizontal_speed) || horizontal_speed > 1000.0f) {
        return false;
    }
    speed = horizontal_speed;
    return true;
}

bool ReadPlayerCombatState(bool& in_combat) noexcept {
    in_combat = false;
    if (g_ui_is_player_in_combat == nullptr) {
        return false;
    }

    bool call_succeeded = false;
    __try {
        in_combat = g_ui_is_player_in_combat();
        call_succeeded = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        in_combat = false;
        call_succeeded = false;
    }
    return call_succeeded;
}

bool TryReadCharacterInVehicle(std::uintptr_t sim_object,
                               bool& in_vehicle) noexcept {
    in_vehicle = false;
    if (sim_object == 0 || g_is_character_in_vehicle == nullptr) {
        return false;
    }

    bool call_succeeded = false;
    __try {
        in_vehicle =
            g_is_character_in_vehicle(reinterpret_cast<void*>(sim_object));
        call_succeeded = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        in_vehicle = false;
        call_succeeded = false;
    }
    return call_succeeded;
}

// (Removed: ReadCharacterSweatPolicyAmount took g_character_sweat_mutex to
// return one float. It had no callers, and because that mutex is NOT recursive
// a future call from inside ApplyCharacterSweatRestore's critical section would
// have deadlocked the render thread instantly. Read the field directly while
// already holding the lock instead of reintroducing a nested accessor.)

#if !defined(SPATCH_FINAL_RELEASE)
float ReadNpcCharacterSweatPolicyAmount(std::uintptr_t effects_component) {
    if (effects_component == 0) {
        return 0.0f;
    }
    try {
        std::lock_guard<std::mutex> lock(g_npc_character_effects_mutex);
        const auto it = g_npc_character_effects_states.find(effects_component);
        return it == g_npc_character_effects_states.end()
                   ? 0.0f
                   : it->second.sweat_policy.amount;
    } catch (...) {
        return 0.0f;
    }
}
#endif

character_sweat::Timing ConfiguredCharacterSweatTiming() noexcept;

CharacterSweatDecisionSnapshot ApplyCharacterSweatRestore(
    void* character_effects_component,
    float delta_seconds,
    bool is_attached_to_player) {
    CharacterSweatDecisionSnapshot snapshot{};
    if (!g_config.restore_character_sweat) {
        std::lock_guard<std::mutex> lock(g_character_sweat_mutex);
        RestoreOwnedCharacterSweat(g_character_sweat_state);
        return snapshot;
    }
    if (character_effects_component == nullptr) {
        return snapshot;
    }

    const std::uintptr_t effects_component =
        reinterpret_cast<std::uintptr_t>(character_effects_component);
    if (!is_attached_to_player) {
        if (g_character_sweat_active_component.load(std::memory_order_acquire) !=
            effects_component) {
            return snapshot;
        }
        std::lock_guard<std::mutex> lock(g_character_sweat_mutex);
        if (g_character_sweat_state.effects_component == effects_component) {
            RestoreOwnedCharacterSweat(g_character_sweat_state);
        }
        return snapshot;
    }
    const std::uintptr_t sim_object = ReadSimObjectFromComponent(character_effects_component);
    if (sim_object == 0) {
        std::lock_guard<std::mutex> lock(g_character_sweat_mutex);
        RestoreOwnedCharacterSweat(g_character_sweat_state);
        return snapshot;
    }

    // INVARIANT for everything below: this critical section holds
    // g_character_sweat_mutex across indirect engine calls
    // (SafeResolveSimObjectComponent, RevalidateComponentBinding,
    // ReadPlayerHorizontalSpeedFromComponent, TryReadCharacterInVehicle,
    // ReadPlayerCombatState). That is only safe while all three of these hold:
    //
    //   1. ApplyCharacterSweatRestore has exactly one caller - the character
    //      effects component update detour - and that detour runs on the thread
    //      that updates the player's effects component. Do not add a second
    //      caller from a job/worker thread.
    //   2. g_character_sweat_mutex is never nested with
    //      g_npc_character_effects_mutex. The two are taken in strictly
    //      separate scopes today; nesting them in both orders would deadlock.
    //   3. No function called from inside this scope may take
    //      g_character_sweat_mutex again. It is a plain std::mutex, not
    //      recursive, so a nested accessor is an instant render-thread
    //      deadlock (this is why ReadCharacterSweatPolicyAmount was deleted).
    //
    // Breaking any of the three turns this into a hang, and a hang at the wrong
    // moment looks exactly like a crash to a player.
    std::lock_guard<std::mutex> lock(g_character_sweat_mutex);
    CharacterSweatRuntimeState& state = g_character_sweat_state;
    if (state.effects_component != effects_component || state.sim_object != sim_object) {
        RestoreOwnedCharacterSweat(state);
        state.effects_component = effects_component;
        state.sim_object = sim_object;
        state.physics_component =
            SafeResolveSimObjectComponent(sim_object, 0x7A000001u);
        g_character_sweat_active_component.store(effects_component, std::memory_order_release);
    }
    if (state.physics_component == 0 ||
        RevalidateComponentBinding(state.physics_component,
                                    character_hooks::kPhysicsComponentTypeUid,
                                    sim_object) == 0) {
        // The cached physics component was recycled under us by a
        // spawn/teardown burst. Re-resolve instead of trusting it; a
        // still-unresolvable component skips velocity for this tick and the
        // exertion decision degrades to cleanup-only below.
        state.physics_component = SafeResolveSimObjectComponent(
            sim_object, character_hooks::kPhysicsComponentTypeUid);
    }

    if (state.character_look_component == 0) {
        state.character_look_component =
            SafeResolveSimObjectComponent(sim_object, 0xCC000001u);
    }
    const     std::uintptr_t character_look = state.character_look_component;
    if (character_look == 0) {
        return snapshot;
    }

    float current = 0.0f;
    if (RevalidateComponentBinding(character_look,
                                    character_hooks::kCharacterLookComponentTypeUid,
                                    sim_object) == 0 ||
        !SafeRead(character_look + 0xBC, current) ||
        !IsValidCharacterSweatAmount(current)) {
        state.character_look_component = 0;
        return snapshot;
    }

    // A value that changed without being written by this policy belongs to a
    // stock/scripted owner.  Preserve it and let the policy reacquire only
    // after that owner falls back to its baseline.
    if (state.owns_amount &&
        !NearlyEqual(current, state.last_written_amount, 0.002f) &&
        current > state.last_written_amount) {
        state.owns_amount = false;
        state.policy.amount = std::max(state.policy.amount, current);
    }

    float horizontal_speed = 0.0f;
    bool in_vehicle = false;
    bool in_combat = false;
    const bool velocity_valid =
        ReadPlayerHorizontalSpeedFromComponent(state.physics_component, horizontal_speed);
    const bool velocity_candidate =
        velocity_valid &&
        character_sweat::IsRunning(horizontal_speed, g_config.sweat_run_speed);
    const bool vehicle_state_valid =
        velocity_candidate && TryReadCharacterInVehicle(sim_object, in_vehicle);
    const bool combat_valid = ReadPlayerCombatState(in_combat);
    snapshot.sampled = true;
    snapshot.velocity_valid = velocity_valid;
    snapshot.horizontal_speed = horizontal_speed;
    snapshot.run_speed_threshold = g_config.sweat_run_speed;
    snapshot.vehicle_state_valid = vehicle_state_valid;
    snapshot.in_vehicle = in_vehicle;
    snapshot.combat_valid = combat_valid;
    snapshot.in_combat = in_combat;
    snapshot.decision = character_sweat::EvaluateExertion(
        velocity_valid,
        horizontal_speed,
        g_config.sweat_run_speed,
        vehicle_state_valid,
        in_vehicle,
        combat_valid,
        in_combat);
    const bool running_decision_valid =
        velocity_valid && (!velocity_candidate || vehicle_state_valid);
    const bool cleanup_only = !character_sweat::IsExertionDecisionKnown(
        running_decision_valid, combat_valid, snapshot.decision);
    if (cleanup_only &&
        !character_sweat::CanSafelyDecayOwnedAmount(
            state.owns_amount, current, state.last_written_amount)) {
        // Without a usable exertion source, touch only the exact amount this
        // policy last wrote. A script/stock owner wins immediately.
        state.owns_amount = false;
        return snapshot;
    }

    const character_sweat::StepResult step =
        character_sweat::Advance(state.policy,
                                 delta_seconds,
                                 snapshot.decision.exerting,
                                 ConfiguredCharacterSweatTiming());

    if (!state.owns_amount && current > step.amount + 0.002f) {
        state.policy.amount = std::max(state.policy.amount, current);
        return snapshot;
    }
    if (!step.active) {
        ReleaseOwnedCharacterSweat(state);
        return snapshot;
    }

    if (state.owns_amount && NearlyEqual(current, step.amount, 0.002f)) {
        return snapshot;
    }
    if (!state.owns_amount) {
        state.baseline_amount = current;
    }
    if (!SafeWrite(character_look + 0xBC, step.amount)) {
        state.owns_amount = false;
        return snapshot;
    }

    state.last_written_amount = step.amount;
    state.owns_amount = true;
    return snapshot;
}

character_sweat::Timing ConfiguredCharacterSweatTiming() noexcept {
    return character_sweat::Timing{
        static_cast<float>(g_config.sweat_build_time_seconds),
        static_cast<float>(g_config.sweat_fade_time_seconds),
        static_cast<float>(g_config.sweat_onset_time_seconds)};
}

void AdvanceNpcWetnessLocked(NpcCharacterEffectsRuntimeState& state,
                             std::uintptr_t character_look,
                             float delta_seconds,
                             bool water_contact,
                             std::uint32_t active_wet_surface_uid,
                             float weather_state,
                             float weather_surface_wetness,
                             float override_surface_wetness) {
    const character_wetness::Timing timing{
        static_cast<float>(g_config.wetness_full_time_seconds),
        static_cast<float>(g_config.wetness_fade_time_seconds)};
    const bool surface_contact = active_wet_surface_uid != 0 &&
                                 active_wet_surface_uid !=
                                     state.last_active_wet_surface_uid;
    state.last_active_wet_surface_uid = active_wet_surface_uid;
    if (water_contact || surface_contact) {
        character_wetness::MarkWaterCollision(state.wetness_policy, timing);
        state.water_contact_pending = false;
    }

    const bool raining = std::isfinite(weather_state) && weather_state > 1.0f;
    const float effective_surface_wetness =
        IsValidCharacterWetnessAmount(override_surface_wetness) &&
                override_surface_wetness >= 0.0f
            ? override_surface_wetness
            : weather_surface_wetness;
    const character_wetness::StepResult step = character_wetness::Advance(
        state.wetness_policy,
        delta_seconds,
        raining,
        effective_surface_wetness,
        timing);
    if (!step.active) {
        RestoreOwnedNpcCharacterWetness(state);
        return;
    }

    float current = 0.0f;
    if (!SafeRead(character_look + 0xB8, current) ||
        !IsValidCharacterWetnessAmount(current)) {
        RestoreOwnedNpcCharacterWetness(state);
        return;
    }

    const float desired = step.amount;
    if (state.wetness_owns_amount && character_wetness::ShouldYieldToStrongerOwner(
                                  current,
                                  state.wetness_last_written_amount,
                                  desired)) {
        state.wetness_owns_amount = false;
    }
    if (!state.wetness_owns_amount && current > desired + 0.002f) {
        state.wetness_policy.amount = std::max(state.wetness_policy.amount, current);
        return;
    }
    if ((state.wetness_owns_amount && NearlyEqual(current, desired)) ||
        (!state.wetness_owns_amount && current + 0.0005f >= desired)) {
        return;
    }
    if (!state.wetness_owns_amount) {
        state.wetness_baseline_amount = current;
    }
    if (!SafeWrite(character_look + 0xB8, desired)) {
        state.wetness_owns_amount = false;
        return;
    }
    state.wetness_last_written_amount = desired;
    state.wetness_owns_amount = true;
}

void AdvanceNpcSweatLocked(NpcCharacterEffectsRuntimeState& state,
                           std::uintptr_t character_look,
                           float delta_seconds,
                           unsigned long long now) {
    float horizontal_speed = 0.0f;
    bool in_vehicle = false;
    const bool velocity_valid =
        ReadPlayerHorizontalSpeedFromComponent(state.physics_component, horizontal_speed);
    const bool velocity_candidate =
        velocity_valid &&
        character_sweat::IsRunning(horizontal_speed, g_config.sweat_run_speed);
    const bool vehicle_state_valid =
        velocity_candidate && TryReadCharacterInVehicle(state.sim_object, in_vehicle);
    const bool combat_active = IsCharacterCombatActive(state.sim_object, now);
    const bool running_decision_valid =
        velocity_valid && (!velocity_candidate || vehicle_state_valid);
    const character_sweat::ExertionDecision decision =
        character_sweat::EvaluateExertion(velocity_valid,
                                           horizontal_speed,
                                           g_config.sweat_run_speed,
                                           vehicle_state_valid,
                                           in_vehicle,
                                           true,
                                           combat_active);
    const bool cleanup_only = !character_sweat::IsExertionDecisionKnown(
        running_decision_valid, true, decision);
    if (cleanup_only && !state.sweat_owns_amount) {
        // No input may acquire a value from a stock/scripted owner.
        return;
    }
    float current = 0.0f;
    if (!SafeRead(character_look + 0xBC, current) ||
        !IsValidCharacterSweatAmount(current)) {
        if (cleanup_only) {
            return;
        }
        RestoreOwnedNpcCharacterSweat(state);
        return;
    }
    if (cleanup_only &&
        !character_sweat::CanSafelyDecayOwnedAmount(
            state.sweat_owns_amount, current, state.sweat_last_written_amount)) {
        state.sweat_owns_amount = false;
        return;
    }
    const character_sweat::StepResult step = character_sweat::Advance(
        state.sweat_policy,
        delta_seconds,
        decision.exerting,
        ConfiguredCharacterSweatTiming());
    if (state.sweat_owns_amount && current > state.sweat_last_written_amount + 0.002f) {
        state.sweat_owns_amount = false;
        state.sweat_policy.amount = std::max(state.sweat_policy.amount, current);
    }
    if (!state.sweat_owns_amount && current > step.amount + 0.002f) {
        state.sweat_policy.amount = std::max(state.sweat_policy.amount, current);
        return;
    }
    if (!step.active) {
        ReleaseOwnedNpcCharacterSweat(state);
        return;
    }
    if (state.sweat_owns_amount && NearlyEqual(current, step.amount, 0.002f)) {
        return;
    }
    if (!state.sweat_owns_amount) {
        state.sweat_baseline_amount = current;
    }
    if (!SafeWrite(character_look + 0xBC, step.amount)) {
        state.sweat_owns_amount = false;
        return;
    }
    state.sweat_last_written_amount = step.amount;
    state.sweat_owns_amount = true;
}

void ApplyNpcCharacterEffectsRestore(void* character_effects_component,
                                     float delta_seconds,
                                     std::uint32_t active_wet_surface_uid,
                                     float weather_state,
                                     float weather_surface_wetness,
                                     float override_surface_wetness) {
    if (character_effects_component == nullptr ||
        (!g_config.restore_character_wetness && !g_config.restore_character_sweat)) {
        return;
    }
    const std::uintptr_t effects_component =
        reinterpret_cast<std::uintptr_t>(character_effects_component);
    const std::uintptr_t sim_object = ReadSimObjectFromComponent(character_effects_component);
    if (sim_object == 0) {
        return;
    }

    const unsigned long long now = GetTickCount64();
    try {
        std::lock_guard<std::mutex> lock(g_npc_character_effects_mutex);
        // Pruning may erase map entries, so do it before retaining a pointer to
        // the state selected for this update.
        PruneNpcCharacterEffectsStatesLocked(now);
        NpcCharacterEffectsRuntimeState* state =
            GetNpcCharacterEffectsStateLocked(effects_component, sim_object, now);
        if (state == nullptr) {
            return;
        }

        if (g_config.restore_character_wetness && state->water_contact_pending) {
            // The water callback may precede this update by several frames;
            // consume it exactly once and let the policy own the fade timer.
            state->water_contact_pending = false;
            character_wetness::MarkWaterCollision(
                state->wetness_policy,
                character_wetness::Timing{
                    static_cast<float>(g_config.wetness_full_time_seconds),
                    static_cast<float>(g_config.wetness_fade_time_seconds)});
        }

        if (state->character_look_component == 0) {
            state->character_look_component =
                SafeResolveSimObjectComponent(sim_object, 0xCC000001u);
        }
        if (state->character_look_component == 0) {
            return;
        }

        if (RevalidateComponentBinding(state->character_look_component,
                                        character_hooks::kCharacterLookComponentTypeUid,
                                        sim_object) == 0) {
            RestoreOwnedNpcCharacterWetness(*state);
            RestoreOwnedNpcCharacterSweat(*state);
            state->character_look_component = 0;
            return;
        }

        if (g_config.restore_character_wetness) {
            AdvanceNpcWetnessLocked(*state,
                                    state->character_look_component,
                                    delta_seconds,
                                    false,
                                    active_wet_surface_uid,
                                    weather_state,
                                    weather_surface_wetness,
                                    override_surface_wetness);
        }
        if (g_config.restore_character_sweat) {
            if (state->physics_component == 0 ||
                RevalidateComponentBinding(state->physics_component,
                                            character_hooks::kPhysicsComponentTypeUid,
                                            sim_object) == 0) {
                // Same recycled-pool rule as the player path: re-resolve
                // instead of trusting a stale physics component. A
                // still-unresolvable component keeps velocity invalid for
                // this tick and AdvanceNpcSweatLocked degrades to cleanup.
                state->physics_component = SafeResolveSimObjectComponent(
                    sim_object, character_hooks::kPhysicsComponentTypeUid);
            }
            AdvanceNpcSweatLocked(*state,
                                  state->character_look_component,
                                  delta_seconds,
                                  now);
        }
    } catch (...) {
        // The NPC path is supplemental. Preserve stock behavior if an
        // allocation or synchronization failure occurs in the cache.
    }
}

template <typename T>
T* ReadGlobalPointer(std::uintptr_t address) {
    T* value = nullptr;
    SafeRead(address, value);
    return value;
}

float ReadVector3Magnitude(const void* vector_ptr) {
    const auto address = reinterpret_cast<std::uintptr_t>(vector_ptr);
    if (address == 0) {
        return 0.0f;
    }

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!SafeRead(address + 0x0, x) || !SafeRead(address + 0x4, y) || !SafeRead(address + 0x8, z)) {
        return 0.0f;
    }

    return std::sqrt((x * x) + (y * y) + (z * z));
}

bool IsApprox60HzDelta(float value) {
    return IsApprox60HzCutsceneDelta(value);
}

const char* ClassifySmallDelta(float delta) {
    if (delta >= 0.0f && delta < 0.0005f) {
        return "zero";
    }
    if (delta >= 0.030f && delta <= 0.036f) {
        return "approx_30hz";
    }
    if (delta >= 0.015f && delta <= 0.019f) {
        return "approx_60hz";
    }
    if (delta > 0.0f && delta < 0.2f) {
        return "other_small";
    }
    return "reset_or_large";
}

std::uintptr_t ToRva(std::uintptr_t address) {
    if (g_module_base == 0 || g_module_end <= g_module_base || address < g_module_base ||
        address >= g_module_end) {
        return 0;
    }

    return address - g_module_base;
}

std::uintptr_t ReadPointerSlot(std::uintptr_t slot_address) {
    std::uintptr_t value = 0;
    if (slot_address != 0) {
        SafeRead(slot_address, value);
    }
    return value;
}

void PersistSmaaConfig() {
    if (g_config_path.empty()) {
        return;
    }

    WriteBoolValue(g_config_path, L"smaa_enable", smaa::GetEnabled());
}

void MaybeApplyRumbleOverride() {
    if (g_rumble_override_attempted || g_config.override_rumble_enabled < 0) {
        return;
    }

    g_rumble_override_attempted = true;

    if (g_rumble_apply_helper == nullptr || g_rumble_apply_object == 0) {
        log::Warn("rumble_override skipped because helper/object was unresolved");
        return;
    }

    const bool enabled = g_config.override_rumble_enabled != 0;
    g_rumble_apply_helper(g_rumble_apply_object, enabled);
    g_rumble_override_apply_count.fetch_add(1);
    g_last_rumble_override_value.store(enabled ? 1 : 0);
    log::InfoF("rumble_override_applied enabled=%d object=0x%p helper=0x%p",
               enabled ? 1 : 0,
               reinterpret_cast<void*>(g_rumble_apply_object),
               reinterpret_cast<void*>(g_rumble_apply_helper));
}

// Is the simulation clock stopped?
//
// This used to call `bool g_simtime_is_paused()` at RVA 0x510980. That routine
// is NOT `bool()` and calling it that way was undefined behaviour with two
// visible consequences, both of which only appeared during cutscenes because
// this is the only caller and it only runs while a cutscene scope is active:
//
//   1. It performs an 8-byte store through RDX, which a zero-argument call
//      leaves holding whatever the previous call happened to leave behind. When
//      that leftover pointed at writable memory the store silently corrupted
//      engine state; when it did not, the resulting access violation was
//      swallowed by the SEH guard below and the whole query was skipped.
//   2. It returns a handle pointer, not a bool. Reinterpreted as a bool that is
//      the low byte of an aligned pointer, so "paused" came out effectively at
//      random - which is why voice lines were only SOMETIMES suppressed.
//
// The real disassembly (legacy_researched, sha256 C6DB199B...):
//
//   140510980  48 85 D2          test  rdx, rdx          ; rdx = OUT pointer
//   140510983  74 49             je    +0x49             ; null out -> return
//   140510985  53                push  rbx
//   140510991  48 8B DA          mov   rbx, rdx
//   14051098A  80 3D ... 00      cmp   byte [0x14203C95C], 0
//   140510994  74 26             je    <false path>
//   140510996  F3 0F 10 05 ...   movss xmm0, [0x14203C970]
//   1405109A1  0F 2E C1          ucomiss xmm1, xmm0      ; xmm1 = 0.0
//   1405109A4  75 16             jne   <false path>
//   1405109B3  48 89 03          mov   [rbx], rax        ; <- the stray store
//
// and the matching setter at 0x1405109D0 stores 0.0 into [0x14203C970] and
// `seta` into [0x14203C95C], i.e. gate = "a pause is being requested" and
// scale = the sim time scale. The decision the routine itself makes is exactly
// `gate != 0 && scale == 0.0f`, so read those two globals and make the same
// decision without the call, the store, or the bogus bool.
//
// Returns false when the layout is not the one this was derived from; callers
// then fall back to the UI pause flag alone, which is signature-verified.
bool ReadSimTimePausedSafely() {
    if (!g_simtime_pause_supported) {
        return false;
    }
    const std::uintptr_t gate_address = ResolveAddress(kSimTimePauseGateRva);
    const std::uintptr_t scale_address = ResolveAddress(kSimTimeScaleRva);
    if (gate_address == 0 || scale_address == 0) {
        return false;
    }
    std::uint8_t gate = 0;
    float scale = 1.0f;
    if (!SafeRead(gate_address, gate) || !SafeRead(scale_address, scale)) {
        return false;
    }
    if (!std::isfinite(scale)) {
        return false;
    }
    return gate != 0 && scale == 0.0f;
}

CutscenePauseState ReadCutscenePauseStateSafely() {
    CutscenePauseState pause_state{};

    __try {
        if (g_ui_is_game_paused != nullptr && g_ui_is_game_paused()) {
            pause_state.ui_paused = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    pause_state.simtime_paused = ReadSimTimePausedSafely();

    // One line per pause-state transition, budgeted. Without this a user log
    // cannot distinguish "the cutscene kept playing while paused" from "the UI
    // pause flag never reported the pause", and those two need different fixes.
    // Runs only inside a cutscene scope, and only on a change of state.
    {
        static std::atomic<int> transition_budget{32};
        static std::atomic<int> last_state{-1};
        const int state = (pause_state.ui_paused ? 1 : 0) |
                          (pause_state.simtime_paused ? 2 : 0);
        if (last_state.exchange(state, std::memory_order_relaxed) != state) {
            int remaining = transition_budget.load(std::memory_order_relaxed);
            if (remaining > 0 &&
                transition_budget.compare_exchange_strong(
                    remaining, remaining - 1, std::memory_order_relaxed)) {
                log::InfoF("cutscene_pause_state ui_paused=%d simtime_paused=%d",
                           pause_state.ui_paused ? 1 : 0,
                           pause_state.simtime_paused ? 1 : 0);
            }
        }
    }

    return pause_state;
}

bool IsGamePausedSafely() {
    const CutscenePauseState pause_state = ReadCutscenePauseStateSafely();
    return pause_state.ui_paused || pause_state.simtime_paused;
}

void PersistAaConfig() {
    if (g_config_path.empty()) {
        return;
    }

    WriteIntValue(g_config_path, L"aa_variant_mode", g_aa_runtime_variant_mode.load());
    WriteIntValue(g_config_path, L"aa_aux_mode", g_aa_runtime_aux_mode.load());
}

int ClampAaVariantMode(int mode) {
    if (mode < 0) {
        return 0;
    }
    if (mode > 2) {
        return 2;
    }
    return mode;
}

int ClampAaAuxMode(int mode) {
    if (mode < 0) {
        return 0;
    }
    if (mode > 2) {
        return 2;
    }
    return mode;
}

void SetAaVariantMode(int mode) {
    mode = ClampAaVariantMode(mode);
    const int previous = g_aa_runtime_variant_mode.exchange(mode);
    if (previous == mode) {
        return;
    }

    PersistAaConfig();
    log::InfoF("aa_variant mode=%d", mode);
}

void SetAaAuxMode(int mode) {
    mode = ClampAaAuxMode(mode);
    const int previous = g_aa_runtime_aux_mode.exchange(mode);
    if (previous == mode) {
        return;
    }

    PersistAaConfig();
    log::InfoF("aa_aux mode=%d", mode);
}

int ReadOptionSymbolValue(std::uintptr_t address) {
    int value = 0;
    if (address != 0) {
        SafeRead(address, value);
    }
    return value;
}

const char* NormalizeTextLookupToken(const char* token) {
    if (token == nullptr) {
        return nullptr;
    }
    return token[0] == '$' ? token + 1 : token;
}

bool TextLookupTokenEquals(const char* token, const char* expected) {
    const char* normalized = NormalizeTextLookupToken(token);
    if (normalized == nullptr || expected == nullptr) {
        return false;
    }
    return std::strcmp(normalized, expected) == 0;
}

bool ShouldProbeTextLookupToken(const char* token) {
    const char* normalized = NormalizeTextLookupToken(token);
    if (normalized == nullptr) {
        return false;
    }
    return std::strstr(normalized, "OPTIONS_QUALITY_AA") != nullptr ||
           std::strstr(normalized, "FXAA") != nullptr ||
           std::strstr(normalized, "AA") != nullptr;
}

bool ConsumeHotkeyPress(int virtual_key,
                        DiagnosticAtomic<unsigned long long>& last_tick,
                        unsigned long debounce_ms) {
    if ((GetAsyncKeyState(virtual_key) & 1) == 0) {
        return false;
    }

    const unsigned long long now = GetTickCount64();
    const unsigned long long last = last_tick.load();
    if (now - last < debounce_ms) {
        return false;
    }

    last_tick.store(now);
    return true;
}

void PollGraphicsDebugKeys() {
    if (!g_config.aa_variant_debug_keys && !g_config.aa_aux_debug_keys &&
        !g_config.smaa_debug_keys) {
        return;
    }

    if (g_config.smaa_debug_keys && smaa::GetDebugKeysEnabled()) {
        if (ConsumeHotkeyPress(VK_F2, g_last_smaa_toggle_key_tick, 200)) {
            const bool enabled = !smaa::GetEnabled();
            smaa::SetEnabled(enabled);
            PersistSmaaConfig();
            log::InfoF("smaa_toggle enabled=%d preset=%d", enabled ? 1 : 0, g_config.smaa_preset);
        }
    }

    if (g_config.aa_variant_debug_keys) {
        if (ConsumeHotkeyPress(VK_F7, g_last_aa_variant_up_key_tick, 150)) {
            int mode = g_aa_runtime_variant_mode.load();
            mode += 1;
            if (mode > 2) {
                mode = 2;
            }
            SetAaVariantMode(mode);
        }

        if (ConsumeHotkeyPress(VK_F6, g_last_aa_variant_down_key_tick, 150)) {
            int mode = g_aa_runtime_variant_mode.load();
            mode -= 1;
            if (mode < 0) {
                mode = 0;
            }
            SetAaVariantMode(mode);
        }
    }

    if (g_config.aa_aux_debug_keys) {
        if (ConsumeHotkeyPress(VK_F5, g_last_aa_aux_up_key_tick, 150)) {
            int mode = g_aa_runtime_aux_mode.load();
            mode += 1;
            if (mode > 2) {
                mode = 2;
            }
            SetAaAuxMode(mode);
        }

        if (ConsumeHotkeyPress(VK_F4, g_last_aa_aux_down_key_tick, 150)) {
            int mode = g_aa_runtime_aux_mode.load();
            mode -= 1;
            if (mode < 0) {
                mode = 0;
            }
            SetAaAuxMode(mode);
        }
    }
}

std::uintptr_t ReadProviderValue() {
    std::uintptr_t provider = 0;
    if (g_provider_slot != 0) {
        SafeRead(g_provider_slot, provider);
    }
    return provider;
}

int ReadCutsceneFlowStage(std::uintptr_t owner) {
    int stage = 0;
    if (owner != 0) {
        SafeRead(owner + 0x6C, stage);
    }
    return stage;
}

unsigned int ReadCutsceneFlowFlags(std::uintptr_t owner) {
    unsigned int flags = 0;
    if (owner != 0) {
        SafeRead(owner + 0x68, flags);
    }
    return flags;
}

int ReadTwitchState(std::uintptr_t owner) {
    int state = 0;
    if (owner != 0) {
        SafeRead(owner + 0x750, state);
    }
    return state;
}

std::uintptr_t ReadTwitchHandle(std::uintptr_t owner) {
    std::uintptr_t handle = 0;
    if (owner != 0) {
        SafeRead(owner + 0x50, handle);
    }
    return handle;
}

unsigned int ReadTwitchFlagByte(std::uintptr_t owner, std::uintptr_t offset) {
    unsigned char value = 0;
    if (owner != 0) {
        SafeRead(owner + offset, value);
    }
    return value;
}

std::uintptr_t ReadRenderContextInstance() {
    std::uintptr_t render_context = 0;
    if (g_render_context_instance_slot != 0) {
        SafeRead(g_render_context_instance_slot, render_context);
    }
    return render_context;
}

std::uintptr_t ReadTimeOfDayManagerInstance() {
    if (g_time_of_day_accessor == nullptr) {
        return 0;
    }
    // The accessor is an engine call. Guard it like every other engine call
    // on the per-frame path so a mid-transition fault degrades to unknown
    // weather instead of a crash. All reads from the returned manager remain
    // SafeRead-guarded at the call sites.
    std::uintptr_t instance = 0;
    __try {
        instance = g_time_of_day_accessor();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        instance = 0;
    }
    return instance;
}

std::uintptr_t ReadPointerField(std::uintptr_t base, std::uintptr_t offset) {
    std::uintptr_t value = 0;
    if (base != 0) {
        SafeRead(base + offset, value);
    }
    return value;
}

unsigned int ReadU32Field(std::uintptr_t base, std::uintptr_t offset) {
    unsigned int value = 0;
    if (base != 0) {
        SafeRead(base + offset, value);
    }
    return value;
}

std::uintptr_t ReadRenderContextMaterial(std::uintptr_t render_context, std::uintptr_t offset) {
    std::uintptr_t material = 0;
    if (render_context != 0) {
        SafeRead(render_context + offset, material);
    }
    return material;
}

QueueSnapshot ReadQueueSnapshot() {
    QueueSnapshot snapshot{};
    if (g_render_task_manager != 0) {
        SafeRead(g_render_task_manager + 0x218, snapshot.queued_total);
        SafeRead(g_render_task_manager + 0x21C, snapshot.completed_total);
    }
    return snapshot;
}

SceneryCounters ReadSceneryCounters() {
    SceneryCounters counters{};
    if (g_scenery_counter0 != 0) {
        SafeRead(g_scenery_counter0, counters.c0);
    }
    if (g_scenery_counter1 != 0) {
        SafeRead(g_scenery_counter1, counters.c1);
    }
    if (g_scenery_counter2 != 0) {
        SafeRead(g_scenery_counter2, counters.c2);
    }
    if (g_scenery_counter3 != 0) {
        SafeRead(g_scenery_counter3, counters.c3);
    }
    return counters;
}

void StoreSceneryCounters(const SceneryCounters& counters) {
    g_last_scenery_counter0.store(counters.c0);
    g_last_scenery_counter1.store(counters.c1);
    g_last_scenery_counter2.store(counters.c2);
    g_last_scenery_counter3.store(counters.c3);
}

unsigned long long ReadReadyCounterForScope(BuilderScope scope) {
    switch (scope) {
    case BuilderScope::SceneryPrepare:
        return g_scenery_prepare_ready_total.load();
    case BuilderScope::ScenerySetup:
        return g_scenery_setup_ready_total.load();
    case BuilderScope::RenderScenery:
        return g_render_scenery_ready_total.load();
    case BuilderScope::RasterizeBucket:
        return g_rasterize_bucket_ready_total.load();
    default:
        return 0;
    }
}

void AddReadyCounterForScope(BuilderScope scope) {
    switch (scope) {
    case BuilderScope::SceneryPrepare:
        g_scenery_prepare_ready_total.fetch_add(1);
        break;
    case BuilderScope::ScenerySetup:
        g_scenery_setup_ready_total.fetch_add(1);
        break;
    case BuilderScope::RenderScenery:
        g_render_scenery_ready_total.fetch_add(1);
        break;
    case BuilderScope::RasterizeBucket:
        g_rasterize_bucket_ready_total.fetch_add(1);
        break;
    default:
        break;
    }
}

struct BuilderScopeGuard {
    explicit BuilderScopeGuard(BuilderScope scope) : previous(g_current_builder_scope) {
        g_current_builder_scope = scope;
    }

    ~BuilderScopeGuard() {
        g_current_builder_scope = previous;
    }

    BuilderScope previous;
};

std::string ReadCString(std::uintptr_t address, std::size_t max_length = 96) {
    if (address == 0 || max_length == 0) {
        return {};
    }

    try {
        std::string text;
        text.reserve(max_length);

        for (std::size_t index = 0; index < max_length; ++index) {
            if (address > (std::numeric_limits<std::uintptr_t>::max)() - index) {
                break;
            }
            char value = '\0';
            if (!SafeRead(address + index, value) || value == '\0') {
                break;
            }

            const unsigned char code = static_cast<unsigned char>(value);
            if (code < 0x20 || code > 0x7E) {
                break;
            }

            text.push_back(value);
        }
        return text;
    } catch (...) {
        // Names are diagnostic-only and may be read from malformed game
        // metadata. Never let a diagnostic allocation failure escape a hook.
        return {};
    }
}

std::uintptr_t PeekQueuedTaskHeader(std::uintptr_t manager) {
    if (manager == 0) {
        return 0;
    }

    std::uintptr_t node = 0;
    if (!SafeRead(manager + 0x210, node) || node == 0 || node == manager + 0x208) {
        return 0;
    }

    return node - 0x10;
}

std::uintptr_t ReadTaskCallback(std::uintptr_t task_header) {
    std::uintptr_t callback = 0;
    if (task_header != 0) {
        SafeRead(task_header + 0x58, callback);
    }
    return callback;
}

#if !defined(SPATCH_FINAL_RELEASE)
bool IsInterestingCallback(std::uintptr_t callback) {
    if (callback == 0 || callback == static_cast<std::uintptr_t>(-1) ||
        callback == static_cast<std::uintptr_t>(-2)) {
        return false;
    }

    return g_module_base != 0 && g_module_end > g_module_base && callback >= g_module_base &&
           callback < g_module_end;
}

bool RecordSeenCallback(std::uintptr_t callback) {
    if (g_config.max_unique_callbacks == 0 || !IsInterestingCallback(callback)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_seen_callback_mutex);
    for (const std::uintptr_t seen : g_seen_callbacks) {
        if (seen == callback) {
            return false;
        }
    }

    if (g_seen_callbacks.size() >= g_config.max_unique_callbacks) {
        return false;
    }

    try {
        g_seen_callbacks.push_back(callback);
    } catch (...) {
        // Callback discovery is optional telemetry. Allocation failure must
        // not cross a game worker detour or turn a diagnostic setting into a
        // process crash.
        return false;
    }
    g_unique_callback_count.fetch_add(1);
    return true;
}
#endif

struct ChaseParameterProbeSnapshot {
    std::uintptr_t address = 0;
    std::uintptr_t name_pointer = 0;
    std::uint32_t name_symbol = 0;
    std::uint32_t context = (std::numeric_limits<std::uint32_t>::max)();
    float yaw_ang_vel_timespan = 0.0f;
    float look_offset_max = 0.0f;
    float look_offset_ang_vel_min = 0.0f;
    float look_offset_ang_vel_max = 0.0f;
    float pitch_offset_max = 0.0f;
    float pitch_offset_look_factor_up = 0.0f;
    float pitch_offset_look_factor_down = 0.0f;
    float pitch_offset_eye_factor_up = 0.0f;
    float pitch_offset_eye_factor_down = 0.0f;
    float centering_speed_min = 0.0f;
    float centering_speed_max = 0.0f;
    float reverse_speed = 0.0f;
    float orbit_speed = 0.0f;
    bool readable = false;
};

ChaseParameterProbeSnapshot ReadChaseParameterProbeSnapshot(
    std::uintptr_t address) {
    ChaseParameterProbeSnapshot snapshot{};
    snapshot.address = address;
    if (address == 0) {
        return snapshot;
    }

    ChaseParameterProbeSnapshot candidate{};
    candidate.address = address;
    candidate.readable =
        SafeRead(address + vehicle_camera::kParameterNamePointerOffset,
                 candidate.name_pointer) &&
        SafeRead(address + vehicle_camera::kParameterNameSymbolOffset,
                 candidate.name_symbol) &&
        SafeRead(address + vehicle_camera::kParameterContextOffset,
                 candidate.context) &&
        SafeRead(address +
                     vehicle_camera::kParameterYawAngVelTimespanOffset,
                 candidate.yaw_ang_vel_timespan) &&
        SafeRead(address + vehicle_camera::kParameterLookOffsetMaxOffset,
                 candidate.look_offset_max) &&
        SafeRead(address +
                     vehicle_camera::kParameterLookOffsetAngVelMinOffset,
                 candidate.look_offset_ang_vel_min) &&
        SafeRead(address +
                     vehicle_camera::kParameterLookOffsetAngVelMaxOffset,
                 candidate.look_offset_ang_vel_max) &&
        SafeRead(address + vehicle_camera::kParameterPitchOffsetMaxOffset,
                 candidate.pitch_offset_max) &&
        SafeRead(address +
                     vehicle_camera::kParameterPitchOffsetLookFactorUpOffset,
                 candidate.pitch_offset_look_factor_up) &&
        SafeRead(address +
                     vehicle_camera::kParameterPitchOffsetLookFactorDownOffset,
                 candidate.pitch_offset_look_factor_down) &&
        SafeRead(address +
                     vehicle_camera::kParameterPitchOffsetEyeFactorUpOffset,
                 candidate.pitch_offset_eye_factor_up) &&
        SafeRead(address +
                     vehicle_camera::kParameterPitchOffsetEyeFactorDownOffset,
                 candidate.pitch_offset_eye_factor_down) &&
        SafeRead(address + vehicle_camera::kParameterCenteringSpeedMinOffset,
                 candidate.centering_speed_min) &&
        SafeRead(address + vehicle_camera::kParameterCenteringSpeedMaxOffset,
                 candidate.centering_speed_max) &&
        SafeRead(address + vehicle_camera::kParameterReverseSpeedOffset,
                 candidate.reverse_speed) &&
        SafeRead(address + vehicle_camera::kParameterOrbitSpeedOffset,
                 candidate.orbit_speed);
    return candidate.readable ? candidate : snapshot;
}

bool ReadCameraParameterIdentity(
    std::uintptr_t address,
    vehicle_camera::ParameterIdentity& identity) noexcept {
    identity = {};
    return address != 0 &&
           SafeRead(address + vehicle_camera::kParameterNamePointerOffset,
                    identity.name_pointer) &&
           SafeRead(address + vehicle_camera::kParameterNameSymbolOffset,
                    identity.name_symbol) &&
           SafeRead(address + vehicle_camera::kParameterContextOffset,
                    identity.context);
}

bool ClaimCameraProbeLogBudget() noexcept {
    unsigned int remaining =
        g_gtaiv_camera_probe_log_budget.load(std::memory_order_relaxed);
    while (remaining != 0) {
        if (g_gtaiv_camera_probe_log_budget.compare_exchange_weak(
                remaining, remaining - 1, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

bool ClaimCameraProbeSample() noexcept {
    if (g_gtaiv_camera_probe_log_budget.load(std::memory_order_relaxed) == 0) {
        return false;
    }
    constexpr unsigned long long kSampleIntervalMilliseconds = 100;
    const unsigned long long now = GetTickCount64();
    unsigned long long next =
        g_gtaiv_camera_probe_next_sample_tick.load(std::memory_order_relaxed);
    while (now >= next) {
        if (g_gtaiv_camera_probe_next_sample_tick.compare_exchange_weak(
                next,
                now + kSampleIntervalMilliseconds,
                std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

struct DriverSideCameraDecision {
    bool policy_evaluated = false;
    bool base_drive_branch_readable = false;
    bool base_drive_branch_selected = false;
    vehicle_camera::BaseSelectorBranch base_selector_branch =
        vehicle_camera::BaseSelectorBranch::Unknown;
    std::uintptr_t subject = 0;
    std::uintptr_t physics_mover = 0;
    vehicle_camera::VehicleClass vehicle_class =
        vehicle_camera::VehicleClass::Unknown;
    vehicle_camera::DriverSideTargetProfile target_profile =
        vehicle_camera::DriverSideTargetProfile::None;
    vehicle_camera::ParameterSlotMask target_slot_match_mask = 0;
    vehicle_camera::ParameterSlotMask selected_slot_match_mask = 0;
    float blend_factor = 0.0f;
};

struct ChaseCameraDynamicSnapshot {
    bool readable = false;
    unsigned char is_looking_around = 0;
    unsigned char looking_back = 0;
    unsigned char aim_or_focus = 0;
    unsigned char eye_locked = 0;
    unsigned char look_locked = 0;
    float lookaround_joy_input = 0.0f;
    float lookup_joy_input = 0.0f;
    float lookup_mouse = 0.0f;
    float lookaround_center_timer = 0.0f;
    float lookaround_angle = 0.0f;
    float lookaround_angle_desired = 0.0f;
    float centering_speed_min = 0.0f;
    float centering_speed_max = 0.0f;
    float reverse_speed = 0.0f;
    float orbit_speed = 0.0f;
    float yaw_ang_vel_timespan = 0.0f;
    float look_offset_max = 0.0f;
    float look_offset_ang_vel_min = 0.0f;
    float look_offset_ang_vel_max = 0.0f;
    float pitch_offset_max = 0.0f;
    float pitch_offset_look_factor_up = 0.0f;
    float pitch_offset_look_factor_down = 0.0f;
    float pitch_offset_eye_factor_up = 0.0f;
    float pitch_offset_eye_factor_down = 0.0f;
    float forward_angle = 0.0f;
    float forward_angle_desired = 0.0f;
    float target_pitch_position = 0.0f;
    float target_pitch_target = 0.0f;
    float yaw_ang_vel_running_sum = 0.0f;
    float yaw_ang_vel_running_time = 0.0f;
    std::uintptr_t subject = 0;
    std::uintptr_t physics_mover = 0;
    std::uintptr_t physics_mover_vtable_rva = 0;
    vehicle_camera::VehicleClass vehicle_class =
        vehicle_camera::VehicleClass::Unknown;
    std::uintptr_t target_parameters = 0;
    vehicle_camera::ParameterSlotMask target_slot_match_mask = 0;
    vehicle_camera::DriverSideTargetProfile target_profile =
        vehicle_camera::DriverSideTargetProfile::None;
    std::uint32_t target_context =
        (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t target_symbol = 0;
    float transition_source_weight = 0.0f;
    bool transition_source_weight_valid = false;
    bool profile_readable = false;
    bool base_drive_branch_readable = false;
    bool base_drive_branch_selected = false;
    vehicle_camera::BaseSelectorBranch base_selector_branch =
        vehicle_camera::BaseSelectorBranch::Unknown;
    float forward_speed = 0.0f;
    bool forward_speed_readable = false;
    float steering_input = 0.0f;
    std::uint32_t input_flags = 0;
    bool input_state_readable = false;
};

bool TryReadBaseSelectorBranch(std::uintptr_t subject,
                               vehicle_camera::BaseSelectorBranch& branch)
    noexcept {
    using namespace vehicle_camera;

    branch = BaseSelectorBranch::Unknown;
    if (subject == 0) {
        return false;
    }

    unsigned char flee_active = 0;
    if (!SafeRead(subject + kSelectorFleeStateOffset, flee_active)) {
        return false;
    }
    if (flee_active != 0) {
        branch = BaseSelectorBranch::Flee;
        return true;
    }

    std::uintptr_t drive_object = 0;
    std::uintptr_t hijack_object = 0;
    if (!SafeRead(subject + kSelectorDriveObjectOffset, drive_object) ||
        !SafeRead(subject + kSelectorHijackObjectOffset, hijack_object)) {
        return false;
    }

    std::uintptr_t drive_object_state = 0;
    if (drive_object != 0 &&
        !SafeRead(drive_object + kSelectorDriveObjectStateOffset,
                  drive_object_state)) {
        return false;
    }
    std::uint32_t hijack_object_mode = 0;
    if (hijack_object != 0 &&
        !SafeRead(hijack_object + kSelectorHijackObjectModeOffset,
                  hijack_object_mode)) {
        return false;
    }

    const bool drive_predicate = drive_object != 0 &&
                                 drive_object_state != 0 &&
                                 hijack_object == 0;
    std::uintptr_t race_object = 0;
    if (drive_predicate &&
        !SafeRead(subject + kSelectorRaceObjectOffset, race_object)) {
        return false;
    }
    std::uintptr_t race_object_state = 0;
    if (race_object != 0 &&
        !SafeRead(race_object + kSelectorRaceObjectStateOffset,
                  race_object_state)) {
        return false;
    }

    branch = ResolveBaseSelectorBranch(BaseDriveBranchInputs{
        false,
        drive_object != 0,
        drive_object_state != 0,
        hijack_object != 0,
        hijack_object_mode,
        race_object != 0,
        race_object_state != 0,
    });
    return true;
}

bool ReadChaseCameraDynamicSnapshot(
    void* component,
    ChaseCameraDynamicSnapshot& snapshot) noexcept {
    using namespace vehicle_camera;

    snapshot = {};
    const std::uintptr_t address =
        reinterpret_cast<std::uintptr_t>(component);
    if (address == 0) {
        return false;
    }

    ChaseCameraDynamicSnapshot candidate{};
    if (!SafeRead(address + kSubjectOffset, candidate.subject) ||
        candidate.subject == 0) {
        return false;
    }
    candidate.readable =
        SafeRead(address + kIsLookingAroundFlagOffset,
                 candidate.is_looking_around) &&
        SafeRead(address + kLookaroundJoyInputOffset,
                 candidate.lookaround_joy_input) &&
        SafeRead(address + kLookupJoyInputOffset,
                 candidate.lookup_joy_input) &&
        SafeRead(address + kLookupMouseOffset, candidate.lookup_mouse) &&
        SafeRead(address + kLookaroundCenterTimerOffset,
                 candidate.lookaround_center_timer) &&
        SafeRead(address + kLookaroundAngleOffset,
                 candidate.lookaround_angle) &&
        SafeRead(address + kLookaroundAngleDesiredOffset,
                 candidate.lookaround_angle_desired) &&
        SafeRead(address + kCenteringSpeedMinComponentOffset,
                 candidate.centering_speed_min) &&
        SafeRead(address + kCenteringSpeedMaxComponentOffset,
                 candidate.centering_speed_max) &&
        SafeRead(address + kReverseSpeedComponentOffset,
                 candidate.reverse_speed) &&
        SafeRead(address + kOrbitSpeedComponentOffset,
                 candidate.orbit_speed) &&
        SafeRead(address + kYawAngVelTimespanComponentOffset,
                 candidate.yaw_ang_vel_timespan) &&
        SafeRead(address + kLookOffsetMaxComponentOffset,
                 candidate.look_offset_max) &&
        SafeRead(address + kLookOffsetAngVelMinComponentOffset,
                 candidate.look_offset_ang_vel_min) &&
        SafeRead(address + kLookOffsetAngVelMaxComponentOffset,
                 candidate.look_offset_ang_vel_max) &&
        SafeRead(address + kPitchOffsetMaxComponentOffset,
                 candidate.pitch_offset_max) &&
        SafeRead(address + kPitchOffsetLookFactorUpComponentOffset,
                 candidate.pitch_offset_look_factor_up) &&
        SafeRead(address + kPitchOffsetLookFactorDownComponentOffset,
                 candidate.pitch_offset_look_factor_down) &&
        SafeRead(address + kPitchOffsetEyeFactorUpComponentOffset,
                 candidate.pitch_offset_eye_factor_up) &&
        SafeRead(address + kPitchOffsetEyeFactorDownComponentOffset,
                 candidate.pitch_offset_eye_factor_down) &&
        SafeRead(address + kForwardAngleOffset, candidate.forward_angle) &&
        SafeRead(address + kForwardAngleDesiredOffset,
                 candidate.forward_angle_desired) &&
        SafeRead(address + kTargetPitchPositionOffset,
                 candidate.target_pitch_position) &&
        SafeRead(address + kTargetPitchTargetOffset,
                 candidate.target_pitch_target) &&
        SafeRead(address + kYawAngVelRunningSumOffset,
                 candidate.yaw_ang_vel_running_sum) &&
        SafeRead(address + kYawAngVelRunningTimeOffset,
                 candidate.yaw_ang_vel_running_time) &&
        SafeRead(address + kLookingBackFlagOffset, candidate.looking_back) &&
        SafeRead(address + kAimOrFocusFlagOffset, candidate.aim_or_focus) &&
        SafeRead(address + kEyeLockFlagOffset, candidate.eye_locked) &&
        SafeRead(address + kLookLockFlagOffset, candidate.look_locked);
    if (!candidate.readable) {
        return false;
    }

    candidate.is_looking_around =
        candidate.is_looking_around != 0 ? 1 : 0;
    candidate.looking_back = candidate.looking_back != 0 ? 1 : 0;
    candidate.aim_or_focus = candidate.aim_or_focus != 0 ? 1 : 0;
    candidate.eye_locked = candidate.eye_locked != 0 ? 1 : 0;
    candidate.look_locked = candidate.look_locked != 0 ? 1 : 0;

    candidate.base_drive_branch_readable = TryReadBaseSelectorBranch(
        candidate.subject, candidate.base_selector_branch);
    candidate.base_drive_branch_selected =
        candidate.base_selector_branch == BaseSelectorBranch::Drive;

    if (SafeRead(candidate.subject + kPhysicsMoverOffset,
                 candidate.physics_mover) &&
        candidate.physics_mover != 0) {
        std::uintptr_t physics_mover_vtable = 0;
        if (SafeRead(candidate.physics_mover, physics_mover_vtable)) {
            candidate.physics_mover_vtable_rva =
                ToRva(physics_mover_vtable);
            candidate.vehicle_class = ClassifyVehicle(
                candidate.physics_mover_vtable_rva,
                g_use_latest_steam_layout);
        }
        candidate.forward_speed_readable = SafeRead(
            candidate.physics_mover + kPhysicsMoverForwardSpeedOffset,
            candidate.forward_speed);
        candidate.input_state_readable =
            SafeRead(candidate.physics_mover +
                         kPhysicsMoverSteeringInputOffset,
                     candidate.steering_input) &&
            SafeRead(candidate.physics_mover +
                         kPhysicsMoverInputFlagsOffset,
                     candidate.input_flags);
    }

    float transition_source_weight = 0.0f;
    if (SafeRead(address + kTransitionSourceWeightOffset,
                 transition_source_weight) &&
        IsValidTransitionSourceWeight(transition_source_weight)) {
        candidate.transition_source_weight = transition_source_weight;
        candidate.transition_source_weight_valid = true;
    }

    std::uintptr_t target_parameters = 0;
    if (SafeRead(address + kTargetParametersOffset, target_parameters) &&
        target_parameters != 0) {
        std::array<std::uintptr_t, kParameterSlotOffsets.size()> slots{};
        bool all_slots_read = true;
        for (std::size_t index = 0; index < slots.size(); ++index) {
            if (!SafeRead(candidate.subject + kParameterSlotOffsets[index],
                          slots[index])) {
                all_slots_read = false;
                break;
            }
        }
        ParameterIdentity identity{};
        if (all_slots_read &&
            ReadCameraParameterIdentity(target_parameters,
                                        identity)) {
            candidate.target_parameters = target_parameters;
            candidate.target_slot_match_mask = ProfileSlotMatchMask(
                target_parameters, slots);
            candidate.target_profile =
                ClassifyDriverSideTargetProfileFromMatchMask(
                    candidate.vehicle_class,
                    target_parameters,
                    candidate.target_slot_match_mask,
                    slots,
                    identity,
                    candidate.base_selector_branch);
            candidate.target_context = identity.context;
            candidate.target_symbol = identity.name_symbol;
            candidate.profile_readable = true;
        }
    }

    std::uintptr_t verified_subject = 0;
    if (!SafeRead(address + kSubjectOffset, verified_subject) ||
        verified_subject != candidate.subject) {
        return false;
    }
    snapshot = candidate;
    return true;
}

bool IsGtaIvVehicleCameraDynamicDecisionEnabled(
    const DriverSideCameraDecision& decision) noexcept {
    using namespace vehicle_camera;

    if (!decision.policy_evaluated || decision.subject == 0 ||
        decision.physics_mover == 0) {
        return false;
    }
    switch (decision.target_profile) {
        case DriverSideTargetProfile::RoadDrive:
        case DriverSideTargetProfile::RoadRace:
        case DriverSideTargetProfile::RoadFlee:
            return g_config.gta_iv_car_camera &&
                   decision.vehicle_class == VehicleClass::CarPhysicsMover;
        case DriverSideTargetProfile::MotorcycleDriveBlock:
        case DriverSideTargetProfile::MotorcycleFlee:
            return g_config.gta_iv_bike_camera &&
                   decision.vehicle_class == VehicleClass::Motorcycle;
        default:
            return false;
    }
}

bool TryReadNativeVehicleCameraOverride(void* component,
                                        bool& has_override) noexcept {
    has_override = false;
    if (component == nullptr) {
        return false;
    }
    const std::uintptr_t address =
        reinterpret_cast<std::uintptr_t>(component);
    unsigned char looking_back = 0;
    unsigned char aim_or_focus = 0;
    unsigned char eye_locked = 0;
    unsigned char look_locked = 0;
    if (!SafeRead(address + vehicle_camera::kLookingBackFlagOffset,
                  looking_back) ||
        !SafeRead(address + vehicle_camera::kAimOrFocusFlagOffset,
                  aim_or_focus) ||
        !SafeRead(address + vehicle_camera::kEyeLockFlagOffset, eye_locked) ||
        !SafeRead(address + vehicle_camera::kLookLockFlagOffset, look_locked)) {
        return false;
    }
    has_override = looking_back != 0 || aim_or_focus != 0 ||
                   eye_locked != 0 || look_locked != 0;
    return true;
}

float ResolveObservedYawAngularVelocity(
    const ChaseCameraDynamicSnapshot& snapshot) noexcept {
    if (!snapshot.readable ||
        !std::isfinite(snapshot.yaw_ang_vel_running_sum) ||
        !std::isfinite(snapshot.yaw_ang_vel_running_time) ||
        snapshot.yaw_ang_vel_running_time <= 0.0f) {
        return 0.0f;
    }
    const float value = snapshot.yaw_ang_vel_running_sum /
                        snapshot.yaw_ang_vel_running_time;
    return std::isfinite(value) ? value : 0.0f;
}

// Speed rumble: recomputes intensity from published vehicle speed. Defined
// below with the rest of the slice; declared here because the vehicle-camera
// path that reads that speed is the first thing to call it.
void UpdateSpeedRumbleFromSpeed(float forward_speed_mps) noexcept;

struct ChaseCameraUpdateProbeScope {
    void* component = nullptr;
    float delta_seconds = 0.0f;
    unsigned long thread_id = 0;
    unsigned int depth = 0;
    vehicle_camera::DynamicProbeCallCounts calls{};
    const void* selected_parameters = nullptr;
    DriverSideCameraDecision decision{};
    bool capture_sample = false;
    bool stock_mouse_eye_offset_readable = false;
    float stock_mouse_eye_offset = 0.0f;
    bool mutation_applied = false;
    bool dynamics_mutation_applied = false;
    bool speed_shake_eligible = false;
    bool speed_shake_sample_prepared = false;
    bool speed_shake_state_advanced = false;
    bool speed_shake_pose_applied = false;
    float speed_shake_signed_forward_speed = 0.0f;
    gtav_speed_shake::Sample speed_shake_sample{};
    float manual_pitch_offset_radians = 0.0f;
    bool desired_pose_readable = false;
    CameraVector3 desired_eye{};
    CameraVector3 desired_look{};
    CameraVector3 desired_up{};
    CameraVector3 effective_desired_eye{};
    CameraVector3 effective_desired_look{};
    CameraVector3 effective_desired_up{};
    bool collide = false;
    ChaseCameraDynamicSnapshot before{};
};

inline constexpr std::size_t kChaseCameraUpdateProbeMaxDepth = 8;
thread_local std::array<ChaseCameraUpdateProbeScope,
                        kChaseCameraUpdateProbeMaxDepth>
    g_chase_camera_update_probe_stack{};
thread_local unsigned int g_chase_camera_update_probe_depth = 0;

struct GtaIvVehicleCameraDynamicState {
    void* component = nullptr;
    std::uintptr_t subject = 0;
    vehicle_camera::ManualPitchState manual_pitch{};
};

thread_local GtaIvVehicleCameraDynamicState
    g_gtaiv_vehicle_camera_dynamic_state{};

struct GtaVSpeedShakeRuntimeState {
    void* component = nullptr;
    std::uintptr_t subject = 0;
    std::uintptr_t physics_mover = 0;
    gtav_speed_shake::State shaker{};
};

thread_local GtaVSpeedShakeRuntimeState g_gtav_speed_shake_state{};

void ResetGtaVSpeedShakeState() noexcept {
    g_gtav_speed_shake_state = {};
}

void ResetGtaVSpeedShakeStateForComponent(void* component) noexcept {
    if (g_gtav_speed_shake_state.component == component) {
        ResetGtaVSpeedShakeState();
    }
}

void ResetGtaIvVehicleCameraDynamicState() noexcept {
    g_gtaiv_vehicle_camera_dynamic_state = {};
}

void ResetGtaIvVehicleCameraDynamicStateForComponent(
    void* component) noexcept {
    if (g_gtaiv_vehicle_camera_dynamic_state.component == component) {
        ResetGtaIvVehicleCameraDynamicState();
    }
}

ChaseCameraUpdateProbeScope* FindActiveChaseCameraUpdateProbeScope(
    void* component) noexcept {
    for (unsigned int index = g_chase_camera_update_probe_depth;
         index != 0;
         --index) {
        ChaseCameraUpdateProbeScope& scope =
            g_chase_camera_update_probe_stack[index - 1];
        if (scope.component == component) {
            return &scope;
        }
    }
    return nullptr;
}

__declspec(noinline) bool DetourAngularApproach(float* current,
                                                float target,
                                                float rate,
                                                float delta_seconds) {
    if (g_angular_approach_original == nullptr) {
        return false;
    }

    const auto invoke_native = [&]() noexcept {
        return g_angular_approach_original(
            current, target, rate, delta_seconds);
    };
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire) ||
        (!g_config.gta_iv_car_camera && !g_config.gta_iv_bike_camera) ||
        current == nullptr || !std::isfinite(rate) ||
        !std::isfinite(delta_seconds) || delta_seconds <= 0.0f) {
        return invoke_native();
    }

    const vehicle_camera::DynamicAddressProfile addresses =
        vehicle_camera::SelectDynamicAddresses(g_use_latest_steam_layout);
    const std::uintptr_t return_rva = ToRva(
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    std::size_t field_offset = 0;
    enum class AngularPath : unsigned char {
        Native,
        FollowYaw,
        ManualYawRecenter,
    } path = AngularPath::Native;
    if (return_rva == addresses.follow_yaw_return_rva) {
        path = AngularPath::FollowYaw;
        field_offset = vehicle_camera::kForwardAngleOffset;
    } else if (return_rva == addresses.manual_yaw_recenter_return_rva) {
        path = AngularPath::ManualYawRecenter;
        field_offset = vehicle_camera::kLookaroundAngleOffset;
    } else {
        return invoke_native();
    }

    const std::uintptr_t current_address =
        reinterpret_cast<std::uintptr_t>(current);
    if (current_address < field_offset) {
        return invoke_native();
    }
    void* component = reinterpret_cast<void*>(current_address - field_offset);
    ChaseCameraUpdateProbeScope* scope =
        FindActiveChaseCameraUpdateProbeScope(component);
    bool has_native_override = false;
    if (scope == nullptr ||
        !IsGtaIvVehicleCameraDynamicDecisionEnabled(scope->decision) ||
        !TryReadNativeVehicleCameraOverride(component, has_native_override) ||
        has_native_override) {
        return invoke_native();
    }

    std::uint32_t input_flags = 0;
    if (!SafeRead(scope->decision.physics_mover +
                      vehicle_camera::kPhysicsMoverInputFlagsOffset,
                  input_flags)) {
        return invoke_native();
    }
    const bool reversing =
        vehicle_camera::IsPhysicsMoverReversing(input_flags);
    if (reversing) {
        return invoke_native();
    }
    if (path == AngularPath::FollowYaw) {
        const bool handbrake =
            (input_flags &
             vehicle_camera::kPhysicsMoverHandbrakeFlag) != 0;
        const float adjusted_rate =
            vehicle_camera::ResolveGtaIvFollowRate(rate, handbrake);
        if (adjusted_rate != rate) {
            scope->dynamics_mutation_applied = true;
        }
        return g_angular_approach_original(
            current, target, adjusted_rate, delta_seconds);
    }

    const std::uintptr_t component_address =
        reinterpret_cast<std::uintptr_t>(component);
    float horizontal_input = 0.0f;
    float center_timer = 0.0f;
    if (!SafeRead(component_address +
                      vehicle_camera::kLookaroundJoyInputOffset,
                  horizontal_input) ||
        !SafeRead(component_address +
                      vehicle_camera::kLookaroundCenterTimerOffset,
                  center_timer)) {
        return invoke_native();
    }
    if (vehicle_camera::ShouldHoldGtaIvManualYaw(
            horizontal_input,
            center_timer,
            reversing)) {
        scope->dynamics_mutation_applied = true;
        return false;
    }

    const float adjusted_rate =
        vehicle_camera::ResolveGtaIvManualYawRecenterRate(rate);
    if (adjusted_rate != rate) {
        scope->dynamics_mutation_applied = true;
    }
    return g_angular_approach_original(
        current, target, adjusted_rate, delta_seconds);
}

bool ClaimDynamicCameraProbeSample() noexcept {
    if (g_gtaiv_camera_dynamic_probe_log_budget.load(
            std::memory_order_relaxed) == 0) {
        return false;
    }
    constexpr unsigned long long kSampleIntervalMilliseconds = 100;
    const unsigned long long now = GetTickCount64();
    unsigned long long next =
        g_gtaiv_camera_dynamic_probe_next_sample_tick.load(
            std::memory_order_relaxed);
    while (now >= next) {
        if (g_gtaiv_camera_dynamic_probe_next_sample_tick.compare_exchange_weak(
                next,
                now + kSampleIntervalMilliseconds,
                std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

bool ClaimDynamicCameraProbeLogBudget() noexcept {
    unsigned int remaining =
        g_gtaiv_camera_dynamic_probe_log_budget.load(
            std::memory_order_relaxed);
    while (remaining != 0) {
        if (g_gtaiv_camera_dynamic_probe_log_budget.compare_exchange_weak(
                remaining,
                remaining - 1,
                std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// NoWheelTurnBack + ModernBrakeReverse stack (ported from
// recovery/Hooks.cpp.gatefix-v2; recovery-note items 1, 3, 5, 6 applied).
// ---------------------------------------------------------------------------
struct BrakeReverseLatch {
    void *instance;
    bool braking_from_motion;
    bool brake_held;
    // Set by the shift stage every tick from the transmission's raw reverse
    // request. Kept for diagnostics only (the signed mover speed decides
    // direction now).
    bool transmission_reverse;
    float last_speed_kmh;
    // Disarm debounce: at the transmission's forward->reverse flip the brake
    // output drops one tick before the camera-published reverse flag rises,
    // so both held-signals can read 0 for a single tick. Disarm only after
    // this many consecutive all-clear ticks (9 ticks ~ 0.15 s at 60 Hz);
    // any held signal resets the streak.
    unsigned not_held_streak;
};
// Traffic fills the table: the session probe saw 10+ distinct vehicles, so
// the table must hold every instance or the player's latch gets evicted and
// recreated (observed as flaky arming in the 2026-09-26 23:47 capture).
static BrakeReverseLatch g_brake_reverse_latches[32] = {};
static std::size_t g_brake_reverse_latch_count = 0;
std::atomic<int> g_brake_reverse_veto_log_budget{20};
// Separate from the veto budget so a burst of shift vetoes cannot starve the
// drive-cut verdict line (the only observable that the armed window is active).
std::atomic<int> g_brake_reverse_drive_cut_log_budget{40};

BrakeReverseLatch *FindBrakeReverseLatch(void *instance) noexcept {
    for (std::size_t i = 0; i < g_brake_reverse_latch_count; ++i) {
        if (g_brake_reverse_latches[i].instance == instance) {
            return &g_brake_reverse_latches[i];
        }
    }
    return nullptr;
}

BrakeReverseLatch *FindOrCreateBrakeReverseLatch(void *instance) noexcept {
    if (BrakeReverseLatch *existing = FindBrakeReverseLatch(instance)) {
        return existing;
    }
    if (g_brake_reverse_latch_count >= 32) {
        return nullptr;
    }
    BrakeReverseLatch &created = g_brake_reverse_latches[g_brake_reverse_latch_count++];
    created.instance = instance;
    created.braking_from_motion = false;
    created.brake_held = false;
    created.transmission_reverse = false;
    created.last_speed_kmh = 0.0f;
    created.not_held_streak = 0;
    return &created;
}

// ModernBrakeReverse player-side state, published by the chase camera setter
// scope (the only per-frame site that resolves the player's physics mover).
// The mover's reverse input flag is the raw key state - steady while the key
// is held, unlike the driver OUTPUT the game's reverse speed limiter
// alternates between throttle and brake every ~0.2 s (log-verified), which is
// why output-brake-driven latching disarmed itself mid-hold.
std::atomic<std::uintptr_t> g_br_player_drivetrain_instance{0};
std::atomic<std::uintptr_t> g_br_player_mover{0};
std::atomic<std::uintptr_t> g_br_player_subject{0};
std::atomic<bool> g_br_player_reverse_held{false};
// Camera-side armed latch (see MaybeRunModernBrakeReverseFlagGate).
std::atomic<bool> g_br_camera_armed{false};

// Camera-published pointer maps of the player mover / subject (filled once
// per mover by the map dump; consumed by the driver-stage candidate match).
struct BrPointerMapEntry {
    std::uintptr_t offset;
    std::uintptr_t value;
};
static BrPointerMapEntry g_br_mover_map_entries[128];
static std::atomic<std::size_t> g_br_mover_map_entry_count{0};
static BrPointerMapEntry g_br_subject_map_entries[256];
static std::atomic<std::size_t> g_br_subject_map_entry_count{0};
// Signed forward speed in m/s * 100 (fixed point; atomics have no float
// overload on this codepath's toolset).
std::atomic<int> g_br_player_signed_speed_dm{0};

// Player's shaped steering input (m*1000) from the input-production detour,
// consumed by the NoWheelTurnBack steer-input scan lane.
std::atomic<int> g_br_player_shaped_steer_dm{0};

// NoWheelTurnBack: camera-published driving freshness (GetTickCount64 at the
// last chase-camera frame that resolved the player's vehicle mover) and the
// last driven wheel angle as float bits. The steering callback holds the
// recorded angle over the game's driverless-centering once the publish goes
// stale, which is the exact exit moment; the small window where a fresh
// publish may still follow the game's first decayed angle costs at most a
// few percent of the exit angle.
std::atomic<unsigned long long> g_br_player_driving_tick_ms{0};
std::atomic<unsigned int> g_br_held_wheel_angle_bits{0};
// Item 3 (recovery note): the CAPTURE publishes here and clears the recorder
// latch above after consuming it; the hold reads this cell, never the latch.
std::atomic<unsigned int> g_br_captured_angle_bits{0};
// Item 6 (recovery note): recorder diagnostics. counters advance on every
// player callback before the freshness gate, so "field always zero" stays
// distinguishable from "gate hides the turn".
std::atomic<unsigned long long> g_br_rec_calls{0};
std::atomic<unsigned long long> g_br_rec_stores{0};
std::atomic<unsigned> g_br_rec_maxabs_bits{0};
std::atomic<unsigned long long> g_br_cb_total{0};
// Item 5 (recovery note): per-capture log budgets, re-armed at every capture
// so the first exit cannot starve every later one.
std::atomic<int> g_br_hold_log_budget{240};
std::atomic<int> g_br_wheel_hold_trace_left{150};

// Reads the instance's own speed accessor (vtable 0x1797128 slot +0x68,
// audited on this exe). Returns an unsigned magnitude in km/h, SEH-guarded;
// on any other layout the caller degrades instead of guessing.
using VehicleInstanceSpeedFn = float(__fastcall *)(void *);

float ReadVehicleInstanceSpeedKmh(void *instance) noexcept {
    std::uintptr_t vtable = 0;
    if (!SafeRead(reinterpret_cast<std::uintptr_t>(instance), vtable)) {
        return 0.0f;
    }
    if (vtable != ResolveAddress(0x01797128)) {
        static std::atomic<bool> vtable_mismatch_logged{false};
        if (!vtable_mismatch_logged.exchange(true,
                                             std::memory_order_relaxed)) {
            log::InfoF("no_wheel_turn_back: instance vtable 0x%llX != "
                       "expected 0x%llX; speed unavailable",
                       static_cast<unsigned long long>(vtable),
                       static_cast<unsigned long long>(
                           ResolveAddress(0x01797128)));
        }
        return 0.0f;
    }
    std::uintptr_t speed_fn = 0;
    if (!SafeRead(vtable + 0x68, speed_fn) || speed_fn == 0) {
        return 0.0f;
    }
    const auto speed = reinterpret_cast<VehicleInstanceSpeedFn>(speed_fn);
    __try {
        return speed(instance);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0f;
    }
}

void MaybeRunWheelFieldHold(void *instance) noexcept;
void MaybeHoldPlayerSteerInput(void *instance, void *output) noexcept;

// Identity binding between the camera-resolved player mover and the driver
// stage's drivetrain instance. Evidence log: one-hop scans in both directions
// found nothing; the two-hop intermediate sweep found nothing; the
// build-3B3ED8A3 capture found the REAL bridge - the player mover references
// per-vehicle sub-objects inside the drivetrain component heap region
// (mover+0x1F8 etc.) - and also exposed the 0x7FF400000000 pattern constant
// that false-bound via slot 0xC0. Binding rules here are therefore:
//   1. Exact: one of the instance's pointer slots equals a garbage-filtered
//      mover/subject map value.
//   2. Back-pointer: one of the objects the player mover references stores
//      this instance's pointer (an AI instance pointer cannot appear inside
//      the player's own mover-referenced objects, so this cannot false-bind).
std::uintptr_t MaybeBindPlayerDrivetrainInstance(void *instance) noexcept {
    const std::size_t mover_count =
        g_br_mover_map_entry_count.load(std::memory_order_relaxed);
    const std::size_t subject_count =
        g_br_subject_map_entry_count.load(std::memory_order_relaxed);
    if (mover_count == 0 && subject_count == 0) {
        return 0;
    }
    const std::uintptr_t instance_address =
        reinterpret_cast<std::uintptr_t>(instance);
    // Rule 1: exact slot match against the published maps.
    const BrPointerMapEntry *maps[2] = {g_br_mover_map_entries,
                                        g_br_subject_map_entries};
    const std::size_t counts[2] = {mover_count, subject_count};
    for (std::uintptr_t slot_offset = 0; slot_offset < 0x400;
         slot_offset += 8) {
        std::uintptr_t slot_value = 0;
        if (!SafeRead(instance_address + slot_offset, slot_value) ||
            (slot_value & 0xFFFFull) == 0ull) {
            continue;
        }
        for (int m = 0; m < 2; ++m) {
            for (std::size_t e = 0; e < counts[m]; ++e) {
                if (maps[m][e].value == slot_value &&
                    (maps[m][e].value & 0xFFFFull) != 0ull) {
                    g_br_player_drivetrain_instance.store(
                        instance_address, std::memory_order_relaxed);
                    log::InfoF("modern_brake_reverse: bound player drivetrain "
                               "instance=0x%llX (exact slot 0x%llX == "
                               "%s_map[0x%llX])",
                               static_cast<unsigned long long>(
                                   instance_address),
                               static_cast<unsigned long long>(slot_offset),
                               m == 0 ? "mover" : "subject",
                               static_cast<unsigned long long>(
                                   maps[m][e].offset));
                    return instance_address;
                }
            }
        }
    }
    // Rule 2: back-pointer sweep. Rate-limit the (heavier) object reads per
    // instance and give up after a bounded number of sweeps.
    struct BindSweepState {
        void *instance;
        unsigned calls;
        bool exhausted;
    };
    static BindSweepState sweeps[8] = {};
    static std::size_t sweep_count = 0;
    BindSweepState *slot = nullptr;
    for (std::size_t i = 0; i < sweep_count; ++i) {
        if (sweeps[i].instance == instance) {
            slot = &sweeps[i];
            break;
        }
    }
    if (slot == nullptr && sweep_count < 8) {
        slot = &sweeps[sweep_count++];
        slot->instance = instance;
        slot->calls = 0;
        slot->exhausted = false;
    }
    if (slot == nullptr || slot->exhausted) {
        return 0;
    }
    ++slot->calls;
    if (slot->calls >= 960) {
        slot->exhausted = true;
        static std::atomic<bool> bind_failed_logged{false};
        if (!bind_failed_logged.exchange(true, std::memory_order_relaxed)) {
            log::InfoF("modern_brake_reverse: identity binding gave up after "
                       "the sweep budget; gate stays inert");
        }
        return 0;
    }
    if ((slot->calls % 16u) != 0u) {
        return 0;
    }
    // Mover map only: its targets are the player vehicle's own physics sub-
    // objects. Subject-map targets include UFG managers where an AI instance
    // pointer could legitimately appear, which would false-bind.
    for (std::size_t e = 0; e < mover_count; ++e) {
        const std::uintptr_t target = g_br_mover_map_entries[e].value;
        if ((target & 0xFFFFull) == 0ull) {
            continue;
        }
        for (std::uintptr_t offset = 0; offset < 0x400; offset += 8) {
            std::uintptr_t word = 0;
            if (!SafeRead(target + offset, word)) {
                break;
            }
            if (word == instance_address) {
                g_br_player_drivetrain_instance.store(
                    instance_address, std::memory_order_relaxed);
                log::InfoF("modern_brake_reverse: bound player drivetrain "
                           "instance=0x%llX (back-pointer: mover_map[0x%llX] "
                           "-> 0x%llX+0x%llX)",
                           static_cast<unsigned long long>(instance_address),
                           static_cast<unsigned long long>(
                               g_br_mover_map_entries[e].offset),
                           static_cast<unsigned long long>(target),
                           static_cast<unsigned long long>(offset));
                return instance_address;
            }
        }
    }
    return 0;
}

// Self-calibration moved to the driver stage (MaybeBindPlayerDrivetrainInstance):
// the latch table stays empty until the player instance is bound, so a
// camera-side scan over registered instances had no candidates to match.

// The instance vtable speed accessor (kVehicleInstanceVtableRva slot +0x68)
// was removed with the unsigned-speed latch design: it cannot tell forward
// from reverse, and the camera-published signed mover speed replaced it.

using AnalogDriverApplyFn = void(__fastcall *)(void *, float, void *, void *,
                                               void *);
AnalogDriverApplyFn g_analog_driver_apply_original = nullptr;
// Steering command derived at exit from the frozen angle and the car's
// max-steer constant (angle / *(steering+0x20), clamped to [-1,1]):
// re-injected into the analog driver output (+8) while the player is on
// foot so the steering chain keeps the wheels turned via the game's own
// actuation path.
static std::atomic<unsigned int> g_br_frozen_steer_input_bits{0};
std::atomic<int> g_driver_probe_log_budget{200};


void __fastcall DetourAnalogDriverApply(void *driver, float delta_seconds,
                                        void *instance, void *status,
                                        void *output) {
    if (g_analog_driver_apply_original != nullptr) {
        g_analog_driver_apply_original(driver, delta_seconds, instance,
                                       status, output);
    }
    if (instance != nullptr) {
        MaybeRunWheelFieldHold(instance);
        MaybeHoldPlayerSteerInput(instance, output);
    }
    if (instance == nullptr || output == nullptr) {
        return;
    }
    const std::uintptr_t output_address =
        reinterpret_cast<std::uintptr_t>(output);
    float accelerator = 0.0f;
    float service_brake = 0.0f;
    std::uint8_t handbrake = 0;
    if (!SafeRead(output_address, accelerator) ||
        !SafeRead(output_address + 4, service_brake) ||
        !SafeRead(output_address + 0x0C, handbrake)) {
        return;
    }
    if (!std::isfinite(accelerator) || !std::isfinite(service_brake)) {
        return;
    }
    if (g_config.enable_logging && g_config.diag_drivetrain_probe) {
        // Return-site capture: the game reaches the analog driver apply
        // through a vtable slot, so static scanning cannot find the caller.
        // Log each distinct call site once; disassembling around it shows
        // exactly which instruction loads the instance argument, which is the
        // authoritative identity link for the ModernBrakeReverse gate.
        static void *logged_sites[16] = {};
        static unsigned logged_site_count = 0;
        static std::atomic<int> return_site_log_budget{16};
        void *return_site = _ReturnAddress();
        bool site_seen = false;
        for (unsigned i = 0; i < logged_site_count; ++i) {
            if (logged_sites[i] == return_site) {
                site_seen = true;
                break;
            }
        }
        if (!site_seen && logged_site_count < 16) {
            int remaining = return_site_log_budget.load(
                std::memory_order_relaxed);
            if (remaining > 0 &&
                return_site_log_budget.compare_exchange_strong(
                    remaining, remaining - 1, std::memory_order_relaxed)) {
                logged_sites[logged_site_count++] = return_site;
                log::InfoF("modern_brake_reverse: driver_apply return_site=0x%llX "
                           "rva=0x%llX instance=0x%llX",
                           static_cast<unsigned long long>(
                               reinterpret_cast<std::uintptr_t>(return_site)),
                           static_cast<unsigned long long>(
                               reinterpret_cast<std::uintptr_t>(return_site) -
                               ResolveAddress(0)),
                           static_cast<unsigned long long>(
                               reinterpret_cast<std::uintptr_t>(instance)));
            }
        }
    }
    if (g_config.enable_logging && g_config.diag_drivetrain_probe &&
        accelerator < -0.001f) {
        static std::atomic<int> negative_acc_budget{30};
        int negative_remaining =
            negative_acc_budget.load(std::memory_order_relaxed);
        if (negative_remaining > 0 &&
            negative_acc_budget.compare_exchange_strong(
                negative_remaining, negative_remaining - 1,
                std::memory_order_relaxed)) {
            log::InfoF("drivetrain_probe negative accelerator instance=0x%llX "
                       "acc=%.4f brake=%.3f",
                       static_cast<unsigned long long>(
                           reinterpret_cast<std::uintptr_t>(instance)),
                       static_cast<double>(accelerator),
                       static_cast<double>(service_brake));
        }
    }
    if (g_config.enable_logging && g_config.diag_drivetrain_probe &&
        instance != nullptr) {
        // One-shot per-instance pointer map (first 3 instances) for offline
        // identity-chain reconstruction against the camera-side map.
        static void *mapped[8] = {};
        static std::size_t mapped_count = 0;
        bool already_mapped = false;
        for (std::size_t i = 0; i < mapped_count; ++i) {
            if (mapped[i] == instance) {
                already_mapped = true;
                break;
            }
        }
        if (!already_mapped && mapped_count < 8) {
            mapped[mapped_count++] = instance;
            const void *objects[3] = {instance, status, driver};
            const char *object_names[3] = {"instance", "status", "driverobj"};
            for (int o = 0; o < 3; ++o) {
                const std::uintptr_t base =
                    reinterpret_cast<std::uintptr_t>(objects[o]);
                if (base == 0) {
                    continue;
                }
                char line[900];
                std::size_t pos = static_cast<std::size_t>(std::snprintf(
                    line, sizeof(line),
                    "modern_brake_reverse: driver_map instance=0x%llX %s:",
                    static_cast<unsigned long long>(
                        reinterpret_cast<std::uintptr_t>(instance)),
                    object_names[o]));
                for (std::uintptr_t offset = 0; offset < 0x400; offset += 8) {
                    std::uintptr_t value = 0;
                    if (!SafeRead(base + offset, value) ||
                        value < 0x0000010000000000ull ||
                        value >= 0x0000800000000000ull ||
                        (value & 7ull) != 0ull) {
                        continue;
                    }
                    const int written = std::snprintf(
                        line + pos, sizeof(line) - pos, " %llX:%llX",
                        static_cast<unsigned long long>(offset),
                        static_cast<unsigned long long>(value));
                    if (written < 0 ||
                        pos + static_cast<std::size_t>(written) >=
                            sizeof(line) - 1) {
                        log::InfoF("%s", line);
                        pos = static_cast<std::size_t>(std::snprintf(
                            line, sizeof(line),
                            "modern_brake_reverse: driver_map cont:"));
                    } else {
                        pos += static_cast<std::size_t>(written);
                    }
                }
                log::InfoF("%s", line);
            }
        }
        // Near-match diagnostics only: log instance slots that sit close to
        // a camera-published mover/subject map value (vtable identified) for
        // the offline structural ruling. Binding lives exclusively in
        // MaybeBindPlayerDrivetrainInstance (garbage-filtered exact +
        // back-pointer rules); this path must never bind.
        static std::atomic<int> candidate_log_budget{24};
        const std::uintptr_t image_base = ResolveAddress(0);
        for (std::uintptr_t slot_offset = 0; slot_offset < 0x400;
             slot_offset += 8) {
            std::uintptr_t component = 0;
            if (!SafeRead(reinterpret_cast<std::uintptr_t>(instance) +
                              slot_offset,
                          component) ||
                component < 0x0000010000000000ull ||
                component >= 0x0000800000000000ull ||
                (component & 0xFFFFull) == 0ull) {
                continue;
            }
            for (int map_index = 0; map_index < 2; ++map_index) {
                const BrPointerMapEntry *entries =
                    map_index == 0 ? g_br_mover_map_entries
                                   : g_br_subject_map_entries;
                const std::size_t entry_count =
                    (map_index == 0 ? g_br_mover_map_entry_count
                                    : g_br_subject_map_entry_count)
                        .load(std::memory_order_relaxed);
                for (std::size_t e = 0; e < entry_count; ++e) {
                    const std::uintptr_t map_value = entries[e].value;
                    if (map_value == component) {
                        continue;
                    }
                    const std::uintptr_t delta =
                        map_value > component ? map_value - component
                                              : component - map_value;
                    if (delta == 0 || delta >= 0x2000) {
                        continue;
                    }
                    int remaining =
                        candidate_log_budget.load(std::memory_order_relaxed);
                    if (remaining <= 0 ||
                        !candidate_log_budget.compare_exchange_strong(
                            remaining, remaining - 1,
                            std::memory_order_relaxed)) {
                        continue;
                    }
                    std::uintptr_t candidate_vtable = 0;
                    std::uintptr_t vtable_rva = 0;
                    if (SafeRead(map_value - 8, candidate_vtable) &&
                        candidate_vtable > image_base) {
                        vtable_rva = candidate_vtable - image_base;
                    }
                    log::InfoF("modern_brake_reverse: near link "
                               "instance=0x%llX slot=0x%llX val=0x%llX "
                               "%s_map[0x%llX]=0x%llX delta=0x%llX "
                               "vtable_rva=0x%llX",
                               static_cast<unsigned long long>(
                                   reinterpret_cast<std::uintptr_t>(instance)),
                               static_cast<unsigned long long>(slot_offset),
                               static_cast<unsigned long long>(component),
                               map_index == 0 ? "mover" : "subject",
                               static_cast<unsigned long long>(
                                   entries[e].offset),
                               static_cast<unsigned long long>(map_value),
                               static_cast<unsigned long long>(delta),
                               static_cast<unsigned long long>(vtable_rva));
                }
            }
        }
    }
    // Mutating detours stay transparent until the hook transaction commits
    // (the invariant at the g_behavior_transaction_ready declaration).
    if ((g_config.modern_brake_reverse || g_config.no_wheel_turn_back) &&
        g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        const std::uintptr_t instance_address =
            reinterpret_cast<std::uintptr_t>(instance);
        std::uintptr_t player_instance =
            g_br_player_drivetrain_instance.load(std::memory_order_relaxed);
        if (player_instance == 0) {
            player_instance = MaybeBindPlayerDrivetrainInstance(instance);
        }
        // Player-only: the camera scope calibrates the mover -> drivetrain
        // binding, so the gate can never clip AI traffic (whose drivers also
        // run this stage).
        if (player_instance != 0 && instance_address == player_instance) {
            if (BrakeReverseLatch *latch =
                    FindOrCreateBrakeReverseLatch(instance)) {
                const bool reverse_held =
                    g_br_player_reverse_held.load(std::memory_order_relaxed);
                const float signed_speed_mps =
                    static_cast<float>(g_br_player_signed_speed_dm.load(
                        std::memory_order_relaxed)) /
                    100.0f;
                // Arm on "reverse/brake requested while still rolling
                // forward". Two signals, because the mover's reverse flag is
                // the game's reverse REQUEST (sets only when the transmission
                // flips, around 7 km/h) and not the raw key: the driver
                // output brake covers the pre-flip forward-braking phase
                // (steady 1.0 there, unlike the post-flip alternation), and
                // the flag covers the post-flip phases. The signed mover
                // speed excludes active reverse manoeuvres (negative there).
                const bool brake_proxy =
                    service_brake > kBrakeReverseBrakeHeldThreshold;
                if ((reverse_held || brake_proxy) &&
                    signed_speed_mps > kBrakeReverseArmSpeedMps) {
                    latch->braking_from_motion = true;
                    latch->not_held_streak = 0;
                }
                // Disarm on key release. A single all-clear debounce applies
                // at every speed: at the transmission's forward->reverse flip
                // the brake output drops one tick before the camera-published
                // reverse flag rises, and where that gap lands (above or
                // below the arm threshold) is timing luck - a two-tick gap
                // below the threshold disarmed instantly and the reverse
                // throttle passed (the random speed-dependent failure). Three
                // all-clear ticks (~50 ms) bridge the gap; any genuine human
                // release is far longer, so the release-and-repress contract
                // keeps working.
                if (!reverse_held && !brake_proxy) {
                    if (latch->not_held_streak < 3) {
                        ++latch->not_held_streak;
                    } else {
                        latch->braking_from_motion = false;
                    }
                } else {
                    latch->not_held_streak = 0;
                }
                latch->brake_held = reverse_held || brake_proxy;
                latch->last_speed_kmh = signed_speed_mps * 3.6f;
                // While armed the car gets a pure brake window: cut the
                // accelerator AND hold the service brake, so the car brakes
                // to a stop and keeps holding instead of coasting through
                // the limiter's throttle phases or creeping at standstill.
                if (latch->braking_from_motion) {
                    if (accelerator > 0.001f) {
                        (void)SafeWrite(output_address, 0.0f);
                        int remaining =
                            g_brake_reverse_drive_cut_log_budget.load(
                                std::memory_order_relaxed);
                        if (remaining > 0 &&
                            g_brake_reverse_drive_cut_log_budget
                                .compare_exchange_strong(
                                    remaining, remaining - 1,
                                    std::memory_order_relaxed)) {
                            log::InfoF("modern_brake_reverse: cut drive while "
                                       "braking instance=0x%llX acc=%.3f "
                                       "speed=%.2f",
                                       static_cast<unsigned long long>(
                                           instance_address),
                                       static_cast<double>(accelerator),
                                       static_cast<double>(signed_speed_mps *
                                                           3.6f));
                        }
                    }
                    if (service_brake < 1.0f) {
                        (void)SafeWrite(output_address + 4, 1.0f);
                    }
                }
            }
        }
    }
    if (!g_config.enable_logging || !g_config.diag_drivetrain_probe) {
        return;
    }
    struct DriverSample {
        void *instance;
        float accelerator;
        float brake;
    };
    static DriverSample samples[8] = {};
    static std::size_t sample_count = 0;
    DriverSample *slot = nullptr;
    for (std::size_t i = 0; i < sample_count; ++i) {
        if (samples[i].instance == instance) {
            slot = &samples[i];
            break;
        }
    }
    if (slot == nullptr && sample_count < 8) {
        slot = &samples[sample_count++];
        slot->instance = instance;
        slot->accelerator = accelerator;
        slot->brake = service_brake;
    }
    if (slot == nullptr) {
        return;
    }
    const bool changed =
        std::fabs(accelerator - slot->accelerator) > 0.15f ||
        std::fabs(service_brake - slot->brake) > 0.15f;
    slot->accelerator = accelerator;
    slot->brake = service_brake;
    if (!changed) {
        return;
    }
    int remaining = g_driver_probe_log_budget.load(std::memory_order_relaxed);
    if (remaining > 0 &&
        g_driver_probe_log_budget.compare_exchange_strong(
            remaining, remaining - 1, std::memory_order_relaxed)) {
        const BrakeReverseLatch *latch = FindBrakeReverseLatch(instance);
        log::InfoF("drivetrain_probe driver instance=0x%llX acc=%.3f "
                   "brake=%.3f hb=%u speed=%.2f armed=%u",
                   static_cast<unsigned long long>(
                       reinterpret_cast<std::uintptr_t>(instance)),
                   static_cast<double>(accelerator),
                   static_cast<double>(service_brake),
                   static_cast<unsigned>(handbrake),
                   static_cast<double>(latch ? latch->last_speed_kmh : 0.0f),
                   latch && latch->braking_from_motion ? 1u : 0u);
    }
}

using TransmissionShiftFn = void(__fastcall *)(void *, float, void *, void *);
TransmissionShiftFn g_transmission_shift_original = nullptr;
std::atomic<int> g_shift_probe_log_budget{120};

// Ratio probe: the ratio callback logs the gear ratio the transmission hands
// out. (The old negative-ratio flip was removed: the ratio is the final-drive
// constant on this path and the reverse force does not flow through it.)
using TransmissionRatioFn = float(__fastcall *)(void *, void *, void *);
TransmissionRatioFn g_transmission_ratio_original = nullptr;
// Engine probe: the engine apply follows the transmission stage and receives
// (driverOutput, transmissionOutput). Logging which struct pointer it gets,
// the reverse byte it sees there, and the torque it writes names the struct
// the reverse decision actually flows through.
using VehicleEngineApplyFn = void(__fastcall *)(void *, float, void *, void *,
                                                void *, void *);
VehicleEngineApplyFn g_vehicle_engine_apply_original = nullptr;
std::atomic<int> g_engine_probe_log_budget{400};

void __fastcall DetourVehicleEngineApply(void *engine, float delta_seconds,
                                         void *instance, void *driver_output,
                                         void *transmission_output,
                                         void *output) {
    // ModernBrakeReverse veto at the real consumption point: the engine (and
    // the later force path) reads the transmission output from the struct
    // passed here - a shared global, NOT the staging struct the shift stage
    // writes (the shift-stage veto was a no-op for this reason). While the
    // latch is armed, zero the reverse byte and restore the forward gear
    // BEFORE the engine computes its torque, so no reverse request reaches
    // the drivetrain.
    // Mutating detours stay transparent until the hook transaction commits.
    if (g_config.modern_brake_reverse && instance != nullptr &&
        transmission_output != nullptr &&
        g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        if (BrakeReverseLatch *latch = FindBrakeReverseLatch(instance)) {
            if (latch->braking_from_motion) {
                const std::uintptr_t trans_address =
                    reinterpret_cast<std::uintptr_t>(transmission_output);
                std::uint8_t reverse = 0;
                if (SafeRead(trans_address + 0x11, reverse) && reverse != 0) {
                    (void)SafeWrite(trans_address + 0x11,
                                    static_cast<std::uint8_t>(0));
                    std::uint8_t gear = 0;
                    if (SafeRead(trans_address + 0x12, gear) && gear == 0) {
                        (void)SafeWrite(trans_address + 0x12,
                                        static_cast<std::uint8_t>(1));
                    }
                    int remaining = g_brake_reverse_veto_log_budget.load(
                        std::memory_order_relaxed);
                    if (remaining > 0 &&
                        g_brake_reverse_veto_log_budget.compare_exchange_strong(
                            remaining, remaining - 1,
                            std::memory_order_relaxed)) {
                        log::InfoF("modern_brake_reverse: pre-engine reverse "
                                   "veto instance=0x%llX",
                                   static_cast<unsigned long long>(
                                       reinterpret_cast<std::uintptr_t>(
                                           instance)));
                    }
                }
            }
        }
    }
    if (g_vehicle_engine_apply_original != nullptr) {
        g_vehicle_engine_apply_original(engine, delta_seconds, instance,
                                        driver_output, transmission_output,
                                        output);
    }
    if (!g_config.enable_logging || !g_config.diag_drivetrain_probe ||
        instance == nullptr || transmission_output == nullptr ||
        output == nullptr) {
        return;
    }
    const std::uintptr_t trans_address =
        reinterpret_cast<std::uintptr_t>(transmission_output);
    const std::uintptr_t out_address =
        reinterpret_cast<std::uintptr_t>(output);
    std::uint8_t reverse = 0;
    std::uint8_t gear = 0;
    float torque = 0.0f;
    float rpm = 0.0f;
    float accelerator = 0.0f;
    if (!SafeRead(trans_address + 0x11, reverse) ||
        !SafeRead(trans_address + 0x12, gear) ||
        !SafeRead(out_address, torque) || !SafeRead(out_address + 4, rpm) ||
        !SafeRead(reinterpret_cast<std::uintptr_t>(driver_output),
                  accelerator)) {
        return;
    }
    if (!std::isfinite(torque) || !std::isfinite(rpm) ||
        !std::isfinite(accelerator)) {
        return;
    }
    struct EngineSample {
        void *instance;
        std::uint8_t reverse;
        std::uint8_t gear;
        int torque_sign;
    };
    static EngineSample samples[8] = {};
    static std::size_t sample_count = 0;
    EngineSample *slot = nullptr;
    for (std::size_t i = 0; i < sample_count; ++i) {
        if (samples[i].instance == instance) {
            slot = &samples[i];
            break;
        }
    }
    if (slot == nullptr && sample_count < 8) {
        slot = &samples[sample_count++];
        slot->instance = instance;
        slot->reverse = reverse;
        slot->gear = gear;
        slot->torque_sign = torque > 0.0f ? 1 : (torque < 0.0f ? -1 : 0);
    }
    if (slot == nullptr) {
        return;
    }
    const int torque_sign = torque > 0.0f ? 1 : (torque < 0.0f ? -1 : 0);
    const bool changed = slot->reverse != reverse || slot->gear != gear ||
                         slot->torque_sign != torque_sign;
    slot->reverse = reverse;
    slot->gear = gear;
    slot->torque_sign = torque_sign;
    if (!changed) {
        return;
    }
    int remaining = g_engine_probe_log_budget.load(std::memory_order_relaxed);
    if (remaining > 0 &&
        g_engine_probe_log_budget.compare_exchange_strong(
            remaining, remaining - 1, std::memory_order_relaxed)) {
        log::InfoF("drivetrain_probe engine instance=0x%llX "
                   "transOut=0x%llX reverse=%u gear=%u torque=%.2f rpm=%.1f "
                   "acc=%.3f",
                   static_cast<unsigned long long>(
                       reinterpret_cast<std::uintptr_t>(instance)),
                   static_cast<unsigned long long>(trans_address),
                   static_cast<unsigned>(reverse),
                   static_cast<unsigned>(gear),
                   static_cast<double>(torque), static_cast<double>(rpm),
                   static_cast<double>(accelerator));
    }
}

// Steering probe (NoWheelTurnBack research): the final-scalar callback
// (UFG::Steering::apply's direct call, SDHandlingIV T049) computes the wheel
// angle before the per-wheel multipliers, and the analog driver's +0x38 slot
// produces the shaped input scalar. Logging both per instance around a player
// exit names what straightens the wheels: input decaying to 0 with the angle
// following, or an explicit centering write with the input still held.
using SteeringCallbackFn = void(__fastcall *)(void *, float, void *, void *,
                                              void *);
SteeringCallbackFn g_steering_callback_original = nullptr;
std::atomic<int> g_steering_probe_log_budget{200};
using SteeringInputFn = float(__fastcall *)(void *, float, void *, void *);
SteeringInputFn g_steering_input_original = nullptr;
std::atomic<int> g_steering_input_probe_log_budget{160};

// NoWheelTurnBack field probe (diag): the callback's final scalar propagates
// into the per-wheel array, yet the visible wheels still straighten, so the
// real wheel-angle store is elsewhere. While the player drives with a
// distinctive angle (|angle| > 0.3 rad), scan the vehicle's component memory
// for floats matching the scalar; once the player has been on foot for 2 s,
// re-read the recorded addresses and log which ones the game zeroed.
struct BrWheelField {
    std::uintptr_t address;
    float recorded;
    bool zeroed_after_exit;
};
static BrWheelField g_br_wheel_fields[24];
static std::atomic<std::size_t> g_br_wheel_field_count{0};
static std::atomic<unsigned> g_br_wheel_field_phase{0};
static std::atomic<void *> g_br_wheel_field_instance{nullptr};
// Wheel angle frozen at the moment the driving publish went stale, so a
// later re-entry into a different car cannot retarget another car's hold.
static std::atomic<unsigned int> g_br_frozen_wheel_angle_bits{0};
static std::atomic<bool> g_br_wheel_angle_frozen{false};
// GetTickCount64 of the capture that armed the freeze. The injection holds for a
// bounded window after capture rather than only while the publish is stale: the
// exit transition re-freshens the driving publish intermittently for ~1 s, and
// dropping the hold the instant it refreshes is what made the wheels snap back.
static std::atomic<unsigned long long> g_nwtb_capture_tick_ms{0};
// overwrite the hold writes; a stale stamp means the chain is quiet.
static std::atomic<unsigned long long> g_nwtb_cb_last_tick_ms{0};


void MaybeProbePlayerWheelFields(void *instance, void *steering_component,
                                 void *output_angle_struct) noexcept {
    if (!g_config.enable_logging || !g_config.diag_drivetrain_probe) {
        return;
    }
    const std::uintptr_t player =
        g_br_player_drivetrain_instance.load(std::memory_order_relaxed);
    if (player == 0 ||
        reinterpret_cast<std::uintptr_t>(instance) != player) {
        return;
    }
    const unsigned phase = g_br_wheel_field_phase.load(std::memory_order_relaxed);
    const unsigned long long driving_age =
        GetTickCount64() -
        g_br_player_driving_tick_ms.load(std::memory_order_relaxed);
    if (phase == 2) {
        return;
    }
    if (phase == 0) {
        float angle = 0.0f;
        if (driving_age > 20 || output_angle_struct == nullptr ||
            !SafeRead(reinterpret_cast<std::uintptr_t>(output_angle_struct),
                      angle) ||
            !std::isfinite(angle) || std::fabs(angle) < 0.3f) {
            return;
        }
        // Scan candidate objects for floats within 0.02 of the scalar.
        const std::uintptr_t instance_address =
            reinterpret_cast<std::uintptr_t>(instance);
        struct Candidate {
            std::uintptr_t base;
            std::size_t size;
            const char *name;
            bool force_record;
        };
        Candidate candidates[12] = {};
        std::size_t candidate_count = 0;
        candidates[candidate_count++] = {instance_address, 0x400, "instance", false};
        // Havok wheel records: vehicleData (instance+0x38) keeps its wheel
        // records at +0xC0 in 0xE0-byte blocks (SDHandlingIV audit); the
        // physics wheel steer angle most plausibly lives there.
        std::uintptr_t vehicle_data = 0;
        if (SafeRead(instance_address + 0x38, vehicle_data) &&
            vehicle_data > 0x10000) {
            candidates[candidate_count++] = {vehicle_data + 0xC0, 0x460,
                                             "wheeldata", false};
        }
        if (steering_component != nullptr) {
            candidates[candidate_count++] = {
                reinterpret_cast<std::uintptr_t>(steering_component), 0x80,
                "steering", false};
            std::uintptr_t wheel_array = 0;
            if (SafeRead(reinterpret_cast<std::uintptr_t>(steering_component) +
                             0x10,
                         wheel_array) &&
                wheel_array > 0x10000) {
                candidates[candidate_count++] = {wheel_array, 0x20,
                                                 "wheelarray", false};
            }
            // UFG::Steering::apply reads the per-wheel ANGLE array from
            // steering+0x30 ([rdi+8] in the disasm loop) and writes
            // scalar*multiplier into it; the visible wheels most plausibly
            // consume these.
            std::uintptr_t angle_array = 0;
            if (SafeRead(reinterpret_cast<std::uintptr_t>(steering_component) +
                             0x30,
                         angle_array) &&
                angle_array > 0x10000) {
                // Elements are scalar*multiplier, so they do not match the
                // raw scalar within tolerance; record them unconditionally.
                candidates[candidate_count++] = {angle_array, 0x20,
                                                 "anglearray", true};
            }
        }
        for (std::uintptr_t slot = 0x38; slot <= 0x70 && candidate_count < 12;
             slot += 8) {
            std::uintptr_t component = 0;
            if (SafeRead(instance_address + slot, component) &&
                component > 0x10000) {
                candidates[candidate_count++] = {component, 0x500, "slot",
                                                 false};
            }
        }
        // The physics consumes the STEER INPUT (-1..1, VehicleInput[0]),
        // not the angle; a store holding it reads ~the shaped input while
        // driving and zeroes at exit. Scan for it alongside the angle.
        const float shaped_input = static_cast<float>(
            g_br_player_shaped_steer_dm.load(std::memory_order_relaxed)) /
        1000.0f;
        std::size_t found = 0;
        char line[900];
        std::size_t pos = static_cast<std::size_t>(std::snprintf(
            line, sizeof(line),
            "no_wheel_turn_back: field scan angle=%.4f shaped=%.3f matches:",
            static_cast<double>(angle),
            static_cast<double>(shaped_input)));
        for (std::size_t c = 0; c < candidate_count; ++c) {
            for (std::uintptr_t off = 0; off + 4 <= candidates[c].size;
                 off += 4) {
                float value = 0.0f;
                if (!SafeRead(candidates[c].base + off, value) ||
                    !std::isfinite(value)) {
                    continue;
                }
                if (!candidates[c].force_record) {
                    const bool direct = value > angle - 0.02f &&
                                        value < angle + 0.02f;
                    bool ratio = false;
                    if (angle > 0.1f || angle < -0.1f) {
                        const float scaled[] = {angle * 0.5f, angle * 2.0f,
                                                angle * 0.25f};
                        for (const float s : scaled) {
                            if (value > s - 0.02f && value < s + 0.02f) {
                                ratio = true;
                                break;
                            }
                        }
                    }
                    // Steer-input lane: matches the shaped input exactly
                    // (marked with a ' prefix in the comparison log via
                    // recorded != angle distance).
                    bool input_lane = false;
                    if ((shaped_input > 0.15f || shaped_input < -0.15f) &&
                        value > shaped_input - 0.02f &&
                        value < shaped_input + 0.02f) {
                        input_lane = true;
                    }
                    if (!direct && !ratio && !input_lane) {
                        continue;
                    }
                }
                if (found < 24) {
                    g_br_wheel_fields[found].address = candidates[c].base + off;
                    g_br_wheel_fields[found].recorded = value;
                    ++found;
                }
                const int written = std::snprintf(
                    line + pos, sizeof(line) - pos, " %s@0x%llX+0x%llX",
                    candidates[c].name,
                    static_cast<unsigned long long>(candidates[c].base),
                    static_cast<unsigned long long>(off));
                if (written < 0 ||
                    pos + static_cast<std::size_t>(written) >=
                        sizeof(line) - 1) {
                    log::InfoF("%s", line);
                    pos = static_cast<std::size_t>(
                        std::snprintf(line, sizeof(line),
                                      "no_wheel_turn_back: field scan cont:"));
                } else {
                    pos += static_cast<std::size_t>(written);
                }
            }
        }
        g_br_wheel_field_count.store(found, std::memory_order_relaxed);
        log::InfoF("%s", line);
        if (found > 0) {
            g_br_wheel_field_instance.store(instance, std::memory_order_relaxed);
            g_br_wheel_field_phase.store(1, std::memory_order_relaxed);
        }
        return;
    }
    // Phase 1 (comparison + hold) is hosted in the DRIVER detour: the
    // steering callback stops being called for parked empty cars, so it
    // cannot run the post-exit work. See MaybeRunWheelFieldHold.
}

// Driver-stage host for the post-exit freeze state machine (runs per tick
// even for parked empty cars). Arms the freeze on the first stale driving
// tick and clears it only after SUSTAINED driving. The re-injection itself
// runs in the analog driver detour (MaybeHoldPlayerSteerInput), which sits
// between the analog driver stage and the steering stage in the same tick.
void MaybeRunWheelFieldHold(void *instance) noexcept {
    if (!g_config.no_wheel_turn_back || instance == nullptr) {
        return;
    }
    const std::uintptr_t player =
        g_br_player_drivetrain_instance.load(std::memory_order_relaxed);
    if (player == 0 ||
        reinterpret_cast<std::uintptr_t>(instance) != player) {
        return;
    }
    const unsigned long long driving_age =
        GetTickCount64() -
        g_br_player_driving_tick_ms.load(std::memory_order_relaxed);
    static unsigned fresh_streak = 0;
    if (driving_age <= 20) {
        // The exit transition re-freshens the publish intermittently for
        // about a second; re-arm the freeze only after SUSTAINED driving, or
        // the re-freezes would capture the already-decayed state.
        if (fresh_streak < 60) {
            ++fresh_streak;
        } else if (fresh_streak == 60) {
            // ONE-SHOT per drive. The old unconditional else wiped these
            // cells on EVERY sustained-driving tick, racing the recorder:
            // during full lock the recorder re-stored each frame, but once
            // the wheels passed through center late in the drive nothing
            // re-filled the latch, so every exit captured angle=0 and the
            // hold never injected (the live "wheels straighten on exit"
            // symptom). Park the counter past 60 so this reset runs once;
            // the stale path below resets it to 0 for the next drive.
            g_br_wheel_angle_frozen.store(false, std::memory_order_relaxed);
            // The peak latch is also per-drive state. Clearing only the
            // freeze flag leaves the previous drive's peak in
            // g_br_held_wheel_angle_bits, so the NEXT exit would freeze a
            // minutes-old angle. Reset both cells together.
            g_br_held_wheel_angle_bits.store(0, std::memory_order_relaxed);
            g_br_captured_angle_bits.store(0, std::memory_order_relaxed);
            g_nwtb_capture_tick_ms.store(0, std::memory_order_relaxed);
            ++fresh_streak;
        }
        return;
    }
    fresh_streak = 0;
    // ALWAYS re-derive while stale - do not gate this on exchange(true).
    //
    // Gating on the freeze flag made a drive shorter than the 60-frame settle
    // never re-arm: the flag stayed true, the next exit's exchange returned
    // true, the capture block was skipped, and the PREVIOUS drive's frozen
    // command was re-injected at up to 15 km/h - the wrong wheel angle.
    //
    // The peak latch makes this idempotent. During the ~1 s post-exit flicker the
    // latch still holds the exit angle, so the derived command is unchanged;
    // after a genuine new drive the latch holds the new peak. Re-deriving every
    // stale frame is therefore both correct and cheap - no state to get stale.
    //
    // Reset the capture timestamp every re-derive so the 1.5 s hold window is
    // measured from the most recent derivation, not the first.
    // Item 3 (recovery note): consume the recorder peak ONCE into the
    // capture cell and clear the recorder latch; later re-derives while
    // frozen read the capture cell, so they stay idempotent.
    bool peak_consumed = false;
    const std::uint32_t peak_bits =
        g_br_held_wheel_angle_bits.load(std::memory_order_relaxed);
    float peak_angle = 0.0f;
    std::memcpy(&peak_angle, &peak_bits, sizeof(peak_angle));
    if (std::isfinite(peak_angle) && std::fabs(peak_angle) > 0.01f) {
        g_br_captured_angle_bits.store(peak_bits, std::memory_order_relaxed);
        g_br_held_wheel_angle_bits.store(0, std::memory_order_relaxed);
        peak_consumed = true;
    }
    const std::uint32_t angle_bits =
        g_br_captured_angle_bits.load(std::memory_order_relaxed);
    float held_angle = 0.0f;
    std::memcpy(&held_angle, &angle_bits, sizeof(held_angle));
    float command = 0.0f;
    float max_steer = 0.0f;
    std::uintptr_t steering = 0;
    if (std::isfinite(held_angle) && std::fabs(held_angle) > 0.01f &&
        SafeRead(reinterpret_cast<std::uintptr_t>(instance) + 0x48, steering) &&
        steering > 0x10000 &&
        SafeRead(steering + 0x20, max_steer) &&
        std::isfinite(max_steer) && std::fabs(max_steer) > 1e-6f) {
        // At full input the chain produces +0x3C == *(steering+0x20), so the
        // command is angle / max, clamped to the input range.
        command = held_angle / max_steer;
        if (command > 1.0f) {
            command = 1.0f;
        }
        if (command < -1.0f) {
            command = -1.0f;
        }
    }
    std::uint32_t command_bits = 0;
    std::memcpy(&command_bits, &command, sizeof(command_bits));
    g_br_frozen_steer_input_bits.store(command_bits, std::memory_order_relaxed);
    g_nwtb_capture_tick_ms.store(GetTickCount64(), std::memory_order_relaxed);
    const bool frozen_prev =
        g_br_wheel_angle_frozen.load(std::memory_order_relaxed);
    g_br_wheel_angle_frozen.store(true, std::memory_order_relaxed);
    if (!frozen_prev) {
        // Item 5: per-capture budgets, re-armed at every capture so the
        // first exit cannot starve every later one.
        g_br_hold_log_budget.store(240, std::memory_order_relaxed);
        g_br_wheel_hold_trace_left.store(150, std::memory_order_relaxed);
        // Item 6: the capture line carries the recorder diagnostics; 15
        // specifiers, 15 arguments, order matters.
        float live = 0.0f;
        std::uintptr_t chain = 0;
        if (SafeRead(reinterpret_cast<std::uintptr_t>(instance) + 0x48,
                     chain) &&
            chain > 0x10000) {
            (void)SafeRead(chain + 0x3C, live);
        }
        float shaped =
            static_cast<float>(
                g_br_player_shaped_steer_dm.load(std::memory_order_relaxed)) /
            1000.0f;
        float rec_maxabs = 0.0f;
        const std::uint32_t rec_maxabs_bits =
            g_br_rec_maxabs_bits.load(std::memory_order_relaxed);
        std::memcpy(&rec_maxabs, &rec_maxabs_bits, sizeof(rec_maxabs));
        const unsigned long long capture_cb_age =
            GetTickCount64() -
            g_nwtb_cb_last_tick_ms.load(std::memory_order_relaxed);
        int hold_budget =
            g_br_hold_log_budget.load(std::memory_order_relaxed);
        if (hold_budget > 0 &&
            g_br_hold_log_budget.compare_exchange_strong(
                hold_budget, hold_budget - 1, std::memory_order_relaxed)) {
            log::InfoF(
                "no_wheel_turn_back: player exit captured; freeze armed "
                "angle=%.3f max=%.3f cmd=%.3f dir=%s live=%.4f "
                "cb_age_ms=%llu shaped=%.3f speed=%.1f stale_ms=%llu "
                "frozen_prev=%u peak_consumed=%u rec_calls=%llu "
                "rec_stores=%llu rec_maxabs=%.4f cb_total=%llu",
                static_cast<double>(held_angle),
                static_cast<double>(max_steer),
                static_cast<double>(command),
                held_angle >= 0.0f ? "RIGHT" : "LEFT",
                static_cast<double>(live),
                static_cast<unsigned long long>(capture_cb_age),
                static_cast<double>(shaped),
                static_cast<double>(ReadVehicleInstanceSpeedKmh(instance)),
                static_cast<unsigned long long>(driving_age),
                frozen_prev ? 1u : 0u,
                peak_consumed ? 1u : 0u,
                static_cast<unsigned long long>(
                    g_br_rec_calls.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    g_br_rec_stores.load(std::memory_order_relaxed)),
                static_cast<double>(rec_maxabs),
                static_cast<unsigned long long>(
                    g_br_cb_total.load(std::memory_order_relaxed)));
        }
    }
}

// Re-inject the frozen exit steering command into the analog driver output
// (offset +8 = the steer command the steering stage reads from the same
// buffer one stage later in the tick) while the player is on foot and the
// car is parked. The steering chain then keeps the wheels at the exit angle
// through its own actuation path, so nothing downstream can decay them away.
// Service brake is forced while injected so a steered parked car cannot
// creep (speed < 15 km/h also keeps a hijacking AI driver unfought).
void MaybeHoldPlayerSteerInput(void *instance, void *output) noexcept {
    if (!g_config.no_wheel_turn_back || instance == nullptr ||
        output == nullptr) {
        return;
    }
    const std::uintptr_t player =
        g_br_player_drivetrain_instance.load(std::memory_order_relaxed);
    if (player == 0 ||
        reinterpret_cast<std::uintptr_t>(instance) != player) {
        return;
    }
    if (!g_br_wheel_angle_frozen.load(std::memory_order_relaxed)) {
        return;
    }
    const unsigned long long now_ms = GetTickCount64();
    const unsigned long long driving_age =
        now_ms - g_br_player_driving_tick_ms.load(std::memory_order_relaxed);
    // HOLD WINDOW. Holding only while the publish is stale cuts the hold at the
    // first re-fresh of that publish. Keep holding until 1.5 s after the capture,
    // or for as long as the publish stays stale, whichever is later - bounded so
    // a genuine re-entry into the car releases the wheels.
    const unsigned long long capture_age =
        now_ms - g_nwtb_capture_tick_ms.load(std::memory_order_relaxed);
    const bool hold_now = capture_age < 1500 || driving_age > 20;
    if (!hold_now) {
        return;
    }
    const std::uint32_t bits =
        g_br_frozen_steer_input_bits.load(std::memory_order_relaxed);
    float steer_command = 0.0f;
    std::memcpy(&steer_command, &bits, sizeof(steer_command));
    if (!std::isfinite(steer_command) || steer_command == 0.0f) {
        static unsigned zero_logged = 0;
        if (zero_logged < 3) {
            ++zero_logged;
            log::InfoF("no_wheel_turn_back: injection idle, frozen steering "
                       "command is zero (was the wheel straight at exit?)");
        }
        return;
    }
    const float speed_kmh = ReadVehicleInstanceSpeedKmh(instance);
    if (speed_kmh >= 15.0f) {
        return;
    }
    const std::uintptr_t output_address =
        reinterpret_cast<std::uintptr_t>(output);
    // Mutating detours stay transparent until the hook transaction commits.
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return;
    }
    // Guarded writes: a false bind or freed output buffer must not fault
    // inside a driver detour (house rule - every write pairs SafeRead or
    // uses SafeWrite).
    if (!SafeWrite(output_address + 8, steer_command)) {
        return;
    }
    (void)SafeWrite(output_address + 4, 1.0f);
    // Verdict (rate-capped): read the chain scalar at steering+0x3C, the same
    // field the recorder uses. +0xD0 is a DIFFERENT field and has always
    // printed garbage here - T003 ruled it out as a signal.
    int verdict_budget =
        g_br_hold_log_budget.load(std::memory_order_relaxed);
    if (verdict_budget > 0 &&
        g_br_hold_log_budget.compare_exchange_strong(
            verdict_budget, verdict_budget - 1, std::memory_order_relaxed)) {
        std::uintptr_t steering_chain = 0;
        float scalar_now = 0.0f;
        if (SafeRead(reinterpret_cast<std::uintptr_t>(instance) + 0x48,
                     steering_chain) &&
            steering_chain > 0x10000) {
            (void)SafeRead(steering_chain + 0x3C, scalar_now);
        }
        log::InfoF("no_wheel_turn_back: steer hold active in=%.3f +0x3C=%.3f "
                   "spd=%.1f",
                   static_cast<double>(steer_command),
                   static_cast<double>(scalar_now),
                   static_cast<double>(speed_kmh));
    }
}

// NoWheelTurnBack hold: must run on EVERY callback tick, independent of the
// diagnostic probe's dedupe early-returns (a parked car's angle never
// changes, so probe dedupe would starve the hold).
void MaybeHoldPlayerWheelAngle(void *instance, void *steering,
                                  void *output_angle) noexcept {
    if (!g_config.no_wheel_turn_back || instance == nullptr ||
        output_angle == nullptr) {
        return;
    }
    // Item 6: every callback counts, before any gate.
    g_br_cb_total.fetch_add(1, std::memory_order_relaxed);
    // The capture proved the game straightens driverless cars inside the
    // steering callback (the angle ramps to 0 in ~70 ms at exit, while the
    // raw input lane still carries the on-foot key presses and the
    // driverStatus bytes are identical to a driving frame with the key
    // released). The camera's driving-freshness publish is the only reliable
    // exit signal: while it is fresh, record the live angle; once it goes
    // stale (the player is on foot), overwrite the callback's output with the
    // recorded angle - but only while the car is parked, so an AI driver or a
    // rolling car is never fought. While rolling without the player, follow
    // the game's own angle so the held value stays sane.
    const std::uintptr_t player =
        g_br_player_drivetrain_instance.load(std::memory_order_relaxed);
    // Item 6 probe: above the binding early return - inside it a binding
    // failure is silent and indistinguishable from a dead offset. Diagnostic
    // only: gated and finite-budgeted so the per-frame detour never formats
    // log lines in a normal session.
    static std::atomic<unsigned long long> nwtb_bind_probe{0};
    if (g_config.enable_logging && g_config.diag_drivetrain_probe) {
        const unsigned long long bind_probe_index =
            nwtb_bind_probe.fetch_add(1, std::memory_order_relaxed);
        if (bind_probe_index % 120 == 0) {
            log::InfoF(
                "no_wheel_turn_back: nwtb_bind player=0x%llX instance=0x%llX",
                static_cast<unsigned long long>(player),
                static_cast<unsigned long long>(
                    reinterpret_cast<std::uintptr_t>(instance)));
        }
    }
    if (player == 0 ||
        reinterpret_cast<std::uintptr_t>(instance) != player) {
        return;
    }
    g_nwtb_cb_last_tick_ms.store(GetTickCount64(),
                                 std::memory_order_relaxed);
    // Item 1 (recovery note): record the CHAIN scalar at steering+0x3C, not
    // the callback's output_angle - the two disagree around exit, and the
    // capture derives from the chain scalar.
    float game_angle = 0.0f;
    const bool angle_ok =
        steering != nullptr &&
        SafeRead(reinterpret_cast<std::uintptr_t>(steering) + 0x3C,
                 game_angle) &&
        std::isfinite(game_angle);
    const unsigned long long driving_age =
        GetTickCount64() -
        g_br_player_driving_tick_ms.load(std::memory_order_relaxed);
    // Item 6: recorder diagnostics BEFORE the freshness gate.
    g_br_rec_calls.fetch_add(1, std::memory_order_relaxed);
    if (angle_ok) {
        std::uint32_t observed_bits = 0;
        std::memcpy(&observed_bits, &game_angle, sizeof(observed_bits));
        std::uint32_t prev_max_bits =
            g_br_rec_maxabs_bits.load(std::memory_order_relaxed);
        float prev_max = 0.0f;
        std::memcpy(&prev_max, &prev_max_bits, sizeof(prev_max));
        while (std::fabs(game_angle) > std::fabs(prev_max) &&
               !g_br_rec_maxabs_bits.compare_exchange_weak(
                   prev_max_bits, observed_bits,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
            std::memcpy(&prev_max, &prev_max_bits, sizeof(prev_max));
        }
    }
    const unsigned long long rec_cb_now =
        g_br_rec_calls.load(std::memory_order_relaxed);
    if (g_config.enable_logging && g_config.diag_drivetrain_probe &&
        rec_cb_now % 15 == 0) {
        float sample_max = 0.0f;
        const std::uint32_t sample_max_bits =
            g_br_rec_maxabs_bits.load(std::memory_order_relaxed);
        std::memcpy(&sample_max, &sample_max_bits, sizeof(sample_max));
        log::InfoF(
            "no_wheel_turn_back: nwtb_sample gate=%u angle=%.4f "
            "rec_calls=%llu rec_stores=%llu rec_maxabs=%.4f cb_total=%llu",
            driving_age <= 20 ? 1u : 0u,
            static_cast<double>(game_angle),
            static_cast<unsigned long long>(rec_cb_now),
            static_cast<unsigned long long>(
                g_br_rec_stores.load(std::memory_order_relaxed)),
            static_cast<double>(sample_max),
            static_cast<unsigned long long>(
                g_br_cb_total.load(std::memory_order_relaxed)));
    }
    // Fresh-within-one-frame: ticks this close to a camera publish are while
    // the player still drives. Older ticks are on-foot; the straighten decay
    // runs there, so the recorded angle must not follow it.
    if (driving_age <= 20) {
        if (angle_ok) {
            // PEAK LATCH. Recording the latest angle unconditionally destroys the
            // capture: the wheels pass through straight on almost every driving
            // frame, so a single 0.0000 overwrites the turned angle and the frozen
            // command comes out zero (the "injection idle" log below). Hold the
            // largest-magnitude angle instead, and treat an exact-zero OR
            // sub-threshold sample as "no new information" so it cannot overwrite
            // a real peak.
            //
            // The 0.01f floor is deliberate. Two earlier forms shipped broken:
            //   if (fabs(angle) <= 0.01f) keep = 0;   reset-on-straight, zeroes
            //                                           continuously while driving
            //   same_side = (cur>0&&angle>0)||(cur<0&&angle<0)
            //     with angle != 0.0f;                   with cur=0.6283 and
            //                                           angle=0, same_side is false
            //                                           so the peak is overwritten;
            //                                           and angle=-1e-7 re-triggers
            //                                           it via signed noise
            // The floor closes both: zero OR sub-threshold holds the peak, and the
            // opposite-sign test only applies to a genuinely real sample.
            float prev = 0.0f;
            const std::uint32_t prev_bits =
                g_br_held_wheel_angle_bits.load(std::memory_order_relaxed);
            std::memcpy(&prev, &prev_bits, sizeof(prev));
            const bool real = std::fabs(game_angle) > 0.01f;
            const bool opposite =
                real && prev != 0.0f &&
                ((game_angle > 0.0f) != (prev > 0.0f));
            const float keep =
                (real && (opposite || std::fabs(game_angle) > std::fabs(prev)))
                    ? game_angle : prev;
            if (keep != prev) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &keep, sizeof(bits));
                g_br_held_wheel_angle_bits.store(bits, std::memory_order_relaxed);
                g_br_rec_stores.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return;
    }
    // On-foot holding happens in the analog driver stage
    // (MaybeHoldPlayerSteerInput): it re-injects the frozen steering command
    // one stage BEFORE this callback runs, so the chain itself keeps the
    // wheels turned. Overwriting the output here only fought the chain's own
    // downstream consumers, so it is gone.
}

void __fastcall DetourSteeringCallback(void *steering, float delta_seconds,
                                       void *instance, void *driver_status,
                                       void *output_angle) {
    if (g_steering_callback_original != nullptr) {
        g_steering_callback_original(steering, delta_seconds, instance,
                                     driver_status, output_angle);
    }
    MaybeHoldPlayerWheelAngle(instance, steering, output_angle);
    MaybeProbePlayerWheelFields(instance, steering, output_angle);
    if (!g_config.enable_logging || !g_config.diag_drivetrain_probe ||
        instance == nullptr || output_angle == nullptr) {
        return;
    }
    float angle = 0.0f;
    if (!SafeRead(reinterpret_cast<std::uintptr_t>(output_angle), angle) ||
        !std::isfinite(angle)) {
        return;
    }
    std::uint8_t status_bytes[0x8] = {};
    for (std::size_t b = 0; b < sizeof(status_bytes); ++b) {
        SafeRead(reinterpret_cast<std::uintptr_t>(driver_status) + b,
                 status_bytes[b]);
    }
    struct SteeringSample {
        void *instance;
        float angle;
        std::uint8_t status0;
        std::uint8_t status1;
    };
    static SteeringSample samples[8] = {};
    static std::size_t sample_count = 0;
    SteeringSample *slot = nullptr;
    for (std::size_t i = 0; i < sample_count; ++i) {
        if (samples[i].instance == instance) {
            slot = &samples[i];
            break;
        }
    }
    if (slot == nullptr && sample_count < 8) {
        slot = &samples[sample_count++];
        slot->instance = instance;
        slot->angle = angle;
        slot->status0 = status_bytes[0];
        slot->status1 = status_bytes[1];
        // First sighting: log unconditionally so the capture starts with a
        // baseline line per instance.
        int first_remaining =
            g_steering_probe_log_budget.load(std::memory_order_relaxed);
        if (first_remaining > 0 &&
            g_steering_probe_log_budget.compare_exchange_strong(
                first_remaining, first_remaining - 1,
                std::memory_order_relaxed)) {
            log::InfoF("steering_probe callback instance=0x%llX player=%u "
                       "angle=%.4f status=%02X%02X%02X%02X%02X%02X%02X%02X "
                       "(first)",
                       static_cast<unsigned long long>(
                           reinterpret_cast<std::uintptr_t>(instance)),
                       static_cast<unsigned>(
                           reinterpret_cast<std::uintptr_t>(instance) ==
                           g_br_player_drivetrain_instance.load(
                               std::memory_order_relaxed)),
                       static_cast<double>(angle), status_bytes[0],
                       status_bytes[1], status_bytes[2], status_bytes[3],
                       status_bytes[4], status_bytes[5], status_bytes[6],
                       status_bytes[7]);
        }
        return;
    }
    if (slot == nullptr) {
        return;
    }
    const bool changed = std::fabs(angle - slot->angle) > 0.02f ||
                         slot->status0 != status_bytes[0] ||
                         slot->status1 != status_bytes[1];
    slot->angle = angle;
    slot->status0 = status_bytes[0];
    slot->status1 = status_bytes[1];
    if (!changed) {
        return;
    }
    int remaining = g_steering_probe_log_budget.load(std::memory_order_relaxed);
    if (remaining > 0 &&
        g_steering_probe_log_budget.compare_exchange_strong(
            remaining, remaining - 1, std::memory_order_relaxed)) {
        log::InfoF("steering_probe callback instance=0x%llX player=%u "
                   "angle=%.4f status=%02X%02X%02X%02X%02X%02X%02X%02X",
                   static_cast<unsigned long long>(
                       reinterpret_cast<std::uintptr_t>(instance)),
                   static_cast<unsigned>(
                       reinterpret_cast<std::uintptr_t>(instance) ==
                       g_br_player_drivetrain_instance.load(
                           std::memory_order_relaxed)),
                   static_cast<double>(angle), status_bytes[0],
                   status_bytes[1], status_bytes[2], status_bytes[3],
                   status_bytes[4], status_bytes[5], status_bytes[6],
                   status_bytes[7]);
    }
}

float __fastcall DetourSteeringInput(void *analog_driver, float delta_seconds,
                                     void *instance, void *analog_status) {
    const float shaped =
        g_steering_input_original != nullptr
            ? g_steering_input_original(analog_driver, delta_seconds, instance,
                                        analog_status)
            : 0.0f;
    if (g_config.enable_logging && g_config.diag_drivetrain_probe &&
        instance != nullptr &&
        reinterpret_cast<std::uintptr_t>(instance) ==
            g_br_player_drivetrain_instance.load(std::memory_order_relaxed)) {
        g_br_player_shaped_steer_dm.store(
            static_cast<int>(shaped * 1000.0f), std::memory_order_relaxed);
    }
    if (!g_config.enable_logging || !g_config.diag_drivetrain_probe ||
        instance == nullptr || !std::isfinite(shaped)) {
        return shaped;
    }
    struct InputSample {
        void *instance;
        float shaped;
    };
    static InputSample samples[8] = {};
    static std::size_t sample_count = 0;
    InputSample *slot = nullptr;
    for (std::size_t i = 0; i < sample_count; ++i) {
        if (samples[i].instance == instance) {
            slot = &samples[i];
            break;
        }
    }
    if (slot == nullptr && sample_count < 8) {
        slot = &samples[sample_count++];
        slot->instance = instance;
        slot->shaped = shaped;
        return shaped;
    }
    if (slot == nullptr) {
        return shaped;
    }
    const bool changed = std::fabs(shaped - slot->shaped) > 0.05f;
    slot->shaped = shaped;
    if (!changed) {
        return shaped;
    }
    int remaining =
        g_steering_input_probe_log_budget.load(std::memory_order_relaxed);
    if (remaining > 0 &&
        g_steering_input_probe_log_budget.compare_exchange_strong(
            remaining, remaining - 1, std::memory_order_relaxed)) {
        log::InfoF("steering_probe input instance=0x%llX player=%u "
                   "shaped=%.4f",
                   static_cast<unsigned long long>(
                       reinterpret_cast<std::uintptr_t>(instance)),
                   static_cast<unsigned>(
                       reinterpret_cast<std::uintptr_t>(instance) ==
                       g_br_player_drivetrain_instance.load(
                           std::memory_order_relaxed)),
                   static_cast<double>(shaped));
    }
    return shaped;
}


float __fastcall DetourTransmissionRatio(void *transmission, void *instance,
                                         void *output) {
    float ratio = 0.0f;
    if (g_transmission_ratio_original != nullptr) {
        ratio = g_transmission_ratio_original(transmission, instance, output);
    }
    // Trace: is the ratio called for the player's car around the engage, and
    // does it return a negative (reverse) value there? Budgeted per session.
    if (g_config.enable_logging && g_config.diag_drivetrain_probe &&
        instance != nullptr) {
        if (BrakeReverseLatch *latch = FindBrakeReverseLatch(instance)) {
            static std::atomic<int> ratio_trace_budget{800};
            int trace_remaining =
                ratio_trace_budget.load(std::memory_order_relaxed);
            if (trace_remaining > 0 &&
                ratio_trace_budget.compare_exchange_strong(
                    trace_remaining, trace_remaining - 1,
                    std::memory_order_relaxed)) {
                log::InfoF("drivetrain_probe ratio instance=0x%llX "
                           "armed=%u ratio=%.4f",
                           static_cast<unsigned long long>(
                               reinterpret_cast<std::uintptr_t>(instance)),
                           latch->braking_from_motion ? 1u : 0u,
                           static_cast<double>(ratio));
            }
        }
    }
    return ratio;
}

void __fastcall DetourTransmissionShift(void *transmission,
                                        float delta_seconds, void *instance,
                                        void *output) {
    if (g_transmission_shift_original != nullptr) {
        g_transmission_shift_original(transmission, delta_seconds, instance,
                                      output);
    }
    // Mutating detours stay transparent until the hook transaction commits.
    if (g_config.modern_brake_reverse && instance != nullptr &&
        output != nullptr &&
        g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        if (BrakeReverseLatch *latch = FindBrakeReverseLatch(instance)) {
            const std::uintptr_t output_address =
                reinterpret_cast<std::uintptr_t>(output);
            std::uint8_t reverse = 0;
            const bool reverse_valid =
                SafeRead(output_address + 0x11, reverse);
            if (reverse_valid) {
                // Record the raw reverse request every tick, BEFORE any veto:
                // the driver stage suppresses arming while the transmission is
                // still in reverse (its speed accessor cannot tell direction).
                latch->transmission_reverse = reverse != 0;
            }
            if (latch->braking_from_motion) {
                if (reverse_valid && reverse != 0) {
                    // Diagnostic (DrivetrainProbe): dump the transmission
                    // object and the output struct while reverse is engaged
                    // and vetoed, so a diff against the forward state names
                    // the internal reverse byte that actually drives the
                    // wheel-force sign.
                    if (g_config.enable_logging && g_config.diag_drivetrain_probe) {
                        static std::atomic<int> dump_budget{3};
                        int dump_remaining =
                            dump_budget.load(std::memory_order_relaxed);
                        if (dump_remaining > 0 &&
                            dump_budget.compare_exchange_strong(
                                dump_remaining, dump_remaining - 1,
                                std::memory_order_relaxed)) {
                                const std::uintptr_t transmission_ptr =
                                    reinterpret_cast<std::uintptr_t>(
                                        transmission);
                                if (transmission_ptr != 0) {
                                char dump[0x31 * 3] = {};
                                std::size_t pos = 0;
                                for (std::size_t b = 0; b < 0x30; ++b) {
                                    std::uint8_t byte = 0;
                                    SafeRead(transmission_ptr + b, byte);
                                    pos += static_cast<std::size_t>(std::snprintf(
                                        dump + pos, sizeof(dump) - pos,
                                        "%02X", byte));
                                }
                                log::InfoF("modern_brake_reverse: "
                                           "transmission dump 0x%llX: %s",
                                           static_cast<unsigned long long>(
                                               transmission_ptr),
                                           dump);
                            }
                            std::uint8_t out_bytes[0x14] = {};
                            for (std::size_t b = 0; b < sizeof(out_bytes); ++b) {
                                SafeRead(output_address + b, out_bytes[b]);
                            }
                            log::InfoF("modern_brake_reverse: output dump: "
                                       "%02X%02X%02X%02X%02X%02X%02X%02X "
                                       "%02X%02X%02X%02X%02X%02X%02X%02X "
                                       "%02X%02X%02X%02X",
                                       out_bytes[0], out_bytes[1],
                                       out_bytes[2], out_bytes[3],
                                       out_bytes[4], out_bytes[5],
                                       out_bytes[6], out_bytes[7],
                                       out_bytes[8], out_bytes[9],
                                       out_bytes[10], out_bytes[11],
                                       out_bytes[12], out_bytes[13],
                                       out_bytes[14], out_bytes[15],
                                       out_bytes[16], out_bytes[17],
                                       out_bytes[18], out_bytes[19]);
                        }
                    }
                    // Veto the delayed reverse application: the ratio stage
                    // sees the previous tick's gear, so restoring the forward
                    // first gear keeps its torque sign forward too.
                    (void)SafeWrite(output_address + 0x11,
                                    static_cast<std::uint8_t>(0));
                    std::uint8_t gear = 0;
                    if (SafeRead(output_address + 0x12, gear) && gear == 0) {
                        (void)SafeWrite(output_address + 0x12,
                                        static_cast<std::uint8_t>(1));
                    }
                    int remaining =
                        g_brake_reverse_veto_log_budget.load(std::memory_order_relaxed);
                    if (remaining > 0 &&
                        g_brake_reverse_veto_log_budget.compare_exchange_strong(
                            remaining, remaining - 1,
                            std::memory_order_relaxed)) {
                        log::InfoF("modern_brake_reverse: suppressed reverse "
                                   "engage instance=0x%llX",
                                   static_cast<unsigned long long>(
                                       reinterpret_cast<std::uintptr_t>(
                                           instance)));
                    }
                }
            }
            // Forward-state comparison dump (DrivetrainProbe): the same
            // transmission object while driving forward, for diffing against
            // the reverse-state dump above.
            if (g_config.enable_logging && g_config.diag_drivetrain_probe &&
                !latch->braking_from_motion &&
                latch->last_speed_kmh > 10.0f) {
                static std::atomic<int> fwd_dump_budget{2};
                int fwd_remaining = fwd_dump_budget.load(std::memory_order_relaxed);
                if (fwd_remaining > 0 &&
                    fwd_dump_budget.compare_exchange_strong(
                        fwd_remaining, fwd_remaining - 1,
                        std::memory_order_relaxed)) {
                        const std::uintptr_t transmission_ptr =
                            reinterpret_cast<std::uintptr_t>(transmission);
                        if (transmission_ptr != 0) {
                        char dump[0x31 * 3] = {};
                        std::size_t pos = 0;
                        for (std::size_t b = 0; b < 0x30; ++b) {
                            std::uint8_t byte = 0;
                            SafeRead(transmission_ptr + b, byte);
                            pos += static_cast<std::size_t>(std::snprintf(
                                dump + pos, sizeof(dump) - pos, "%02X",
                                byte));
                        }
                        log::InfoF("modern_brake_reverse: FORWARD "
                                   "transmission dump 0x%llX: %s",
                                   static_cast<unsigned long long>(
                                       transmission_ptr),
                                   dump);
                    }
                }
            }
        }
    }
    if (!g_config.enable_logging || !g_config.diag_drivetrain_probe ||
        transmission == nullptr || instance == nullptr || output == nullptr) {
        return;
    }
    const std::uintptr_t output_address =
        reinterpret_cast<std::uintptr_t>(output);
    std::uint8_t reverse = 0;
    std::uint8_t gear = 0;
    std::uint8_t delayed = 0;
    if (!SafeRead(output_address + 0x11, reverse) ||
        !SafeRead(output_address + 0x12, gear) ||
        !SafeRead(output_address + 0x13, delayed)) {
        return;
    }
    struct ShiftSample {
        void *transmission;
        std::uint8_t reverse;
        std::uint8_t gear;
        std::uint8_t delayed;
    };
    static ShiftSample samples[8] = {};
    static std::size_t sample_count = 0;
    ShiftSample *slot = nullptr;
    for (std::size_t i = 0; i < sample_count; ++i) {
        if (samples[i].transmission == transmission) {
            slot = &samples[i];
            break;
        }
    }
    if (slot == nullptr && sample_count < 8) {
        slot = &samples[sample_count++];
        slot->transmission = transmission;
        slot->reverse = reverse;
        slot->gear = gear;
        slot->delayed = delayed;
    }
    if (slot == nullptr) {
        return;
    }
    const bool changed = slot->reverse != reverse || slot->gear != gear ||
                         slot->delayed != delayed;
    slot->reverse = reverse;
    slot->gear = gear;
    slot->delayed = delayed;
    if (!changed) {
        return;
    }
    int remaining = g_shift_probe_log_budget.load(std::memory_order_relaxed);
    if (remaining > 0 &&
        g_shift_probe_log_budget.compare_exchange_strong(
            remaining, remaining - 1, std::memory_order_relaxed)) {
        log::InfoF("drivetrain_probe shift transmission=0x%llX "
                   "instance=0x%llX reverse=%u gear=%u delayed=%u",
                   static_cast<unsigned long long>(
                       reinterpret_cast<std::uintptr_t>(transmission)),
                   static_cast<unsigned long long>(
                       reinterpret_cast<std::uintptr_t>(instance)),
                   static_cast<unsigned>(reverse),
                   static_cast<unsigned>(gear),
                   static_cast<unsigned>(delayed));
    }
}

// Blip wrapper set-data (0xCB7F0): runs whenever a minimap blip wrapper
// receives new blip data. The engine itself hides one blip type here by
// clearing the wrapper's hidden byte (+0x20) when the data's +0 id matches a
// hard-coded global; the cop-graphic feature keys on the +4 graphic type id
// instead, which the blip-graphic factory dispatches against the two
// runtime-registered cop ids. Clearing the same hidden byte reuses the
// engine's own proven hide mechanism - no ids are rewritten and no graphics
// are patched, so every other blip (player arrow, objectives, stores,
// traffic) is untouched, and the wrapper keeps updating normally.

// One-shot scanner: walk the plausible heap pointers of the player mover and
// vehicle subject and record the plausible 8-byte-aligned slots in the two
// identity maps. Those maps are the sole input to
// MaybeBindPlayerDrivetrainInstance, so this is NOT diagnostic-only: gating
// it on the Debug flag left both counts at 0 with a stock ini, the bind
// returned 0, and every consumer bailed on a player == 0 guard. Population now
// runs whenever either feature is armed; only the verbose pointer LISTING
// stays behind DrivetrainProbe (T002-probe.md:535-542, build C65A5409).
void MaybeDumpModernBrakeReverseCameraMap(std::uintptr_t mover,
                                          std::uintptr_t subject) noexcept {
    if (!g_config.modern_brake_reverse && !g_config.no_wheel_turn_back) {
        return;
    }
    // RE-VERIFIED 2026-10-03: the paragraph below once read as a live rationale
    // and named `PushSpeedRumble` as if it did not exist. That claim was false
    // in the same way as an identical one about `MaybeFireDrivingSwearLine`,
    // and both are retracted. Measured: `PushSpeedRumble` is PRESENT AND LIVE
    // in this file (defined ~9440 gated on `g_config.speed_rumble`, called
    // from the frame-flow detour ~9613), so `[Gameplay] SpeedRumble` and its
    // four tuning keys work; no restore is needed. Earlier snapshots that
    // lacked it are the reason a retraction note exists here at all.
    //
    // Original text: Keyed on the MOVER, not a process-wide flag:
    // PushSpeedRumble zeroes the
    // binding on every mover change, so a one-shot population left every later
    // vehicle re-scanning against the FIRST car's pointers. Both bind rules
    // (exact slot and back-pointer) then compared the new instance against the
    // wrong vehicle's map and could never match. Re-walk once per vehicle; the
    // walk is a bounded 0x1000/0x2000 byte range, not a per-frame cost.
    static std::uintptr_t dumped_mover = 0;
    if (mover == dumped_mover || mover == 0) {
        return;
    }
    dumped_mover = mover;
    const std::uintptr_t bases[2] = {mover, subject};
    const char *names[2] = {"mover", "subject"};
    const std::uintptr_t ranges[2] = {0x1000, 0x2000};
    for (int b = 0; b < 2; ++b) {
        char line[900];
        std::size_t pos = static_cast<std::size_t>(
            std::snprintf(line, sizeof(line), "modern_brake_reverse: %s_map "
                          "0x%llX:", names[b],
                          static_cast<unsigned long long>(bases[b])));
        BrPointerMapEntry *entries = b == 0 ? g_br_mover_map_entries
                                            : g_br_subject_map_entries;
        std::atomic<std::size_t> &count = b == 0
                                              ? g_br_mover_map_entry_count
                                              : g_br_subject_map_entry_count;
        const std::size_t capacity = b == 0 ? 128 : 256;
        std::size_t filled = 0;
        for (std::uintptr_t offset = 0; offset < ranges[b]; offset += 8) {
            std::uintptr_t value = 0;
            if (!SafeRead(bases[b] + offset, value) ||
                value < 0x0000010000000000ull ||
                value >= 0x0000800000000000ull || (value & 7ull) != 0ull) {
                continue;
            }
            // Exclude pattern constants like 0x7FF400000000 (instance slot
            // 0xC0 filler) that false-bound the first exact-match attempt.
            if ((value & 0xFFFFull) == 0ull) {
                continue;
            }
            if (filled < capacity) {
                entries[filled].offset = offset;
                entries[filled].value = value;
                ++filled;
            }
            const int written = std::snprintf(line + pos, sizeof(line) - pos,
                                              " %llX:%llX",
                                              static_cast<unsigned long long>(
                                                  offset),
                                              static_cast<unsigned long long>(
                                                  value));
            if (written < 0 ||
                pos + static_cast<std::size_t>(written) >= sizeof(line) - 1) {
                if (g_config.diag_drivetrain_probe) {
                    log::InfoF("%s", line);
                }
                pos = static_cast<std::size_t>(
                    std::snprintf(line, sizeof(line),
                                  "modern_brake_reverse: %s_map cont:",
                                  names[b]));
            } else {
                pos += static_cast<std::size_t>(written);
            }
        }
        count.store(filled, std::memory_order_relaxed);
        if (g_config.diag_drivetrain_probe) {
            log::InfoF("%s", line);
        }
    }
}

void MaybeRunModernBrakeReverseFlagGate(std::uintptr_t mover,
                                        std::uint32_t input_flags,
                                        float signed_speed_mps) noexcept;

// ---------------------------------------------------------------------------
// ModernBrakeReverse player-state observer (rides the chase camera setter
// scope, depth 1). Publishes the player's raw reverse-key state and signed
// mover speed, and self-calibrates the mover -> drivetrain instance binding
// the driver-stage gate consumes.
// ---------------------------------------------------------------------------
void MaybeObserveModernBrakeReversePlayerState(
    ChaseCameraUpdateProbeScope& scope) noexcept {
    if ((!g_config.modern_brake_reverse && !g_config.no_wheel_turn_back) ||
        !g_behavior_transaction_ready.load(std::memory_order_acquire) ||
        scope.decision.physics_mover == 0) {
        return;
    }
    // Only road-driving counts as "driving" for the freshness publish: the
    // exit transition keeps resolving the vehicle mover for about a second
    // under other selector branches, which would keep the publish fresh
    // through the whole straighten and poison the frozen exit angle.
    using VC = vehicle_camera::VehicleClass;
    using TP = vehicle_camera::DriverSideTargetProfile;
    const bool driving_profile =
        (scope.decision.vehicle_class == VC::CarPhysicsMover &&
         (scope.decision.target_profile == TP::RoadDrive ||
          scope.decision.target_profile == TP::RoadRace ||
          scope.decision.target_profile == TP::RoadFlee)) ||
        (scope.decision.vehicle_class == VC::Motorcycle &&
         (scope.decision.target_profile == TP::MotorcycleDriveBlock ||
          scope.decision.target_profile == TP::MotorcycleFlee));
    if (!driving_profile) {
        return;
    }
    const std::uintptr_t mover = scope.decision.physics_mover;
    g_br_player_mover.store(mover, std::memory_order_relaxed);
    g_br_player_subject.store(scope.decision.subject,
                              std::memory_order_relaxed);
    g_br_player_driving_tick_ms.store(GetTickCount64(),
                                      std::memory_order_relaxed);
    MaybeDumpModernBrakeReverseCameraMap(mover, scope.decision.subject);
    // A changed mover means a different vehicle: drop the binding so the
    // driver-stage scan re-calibrates for the new car.
    static std::uintptr_t last_mover = 0;
    if (last_mover != mover) {
        last_mover = mover;
        g_br_player_drivetrain_instance.store(0, std::memory_order_relaxed);
    }
    float speed = 0.0f;
    std::uint32_t input_flags = 0;
    if (SafeRead(mover + vehicle_camera::kPhysicsMoverForwardSpeedOffset,
                 speed) &&
        std::isfinite(speed)) {
        g_br_player_signed_speed_dm.store(
            static_cast<int>(speed * 100.0f), std::memory_order_relaxed);
    }
    if (SafeRead(mover + vehicle_camera::kPhysicsMoverInputFlagsOffset,
                 input_flags)) {
        g_br_player_reverse_held.store((input_flags & 1u) != 0,
                                       std::memory_order_relaxed);
        MaybeRunModernBrakeReverseFlagGate(mover, input_flags, speed);
    }
}

// Camera-side brake-from-motion latch. Tracks the steady raw key state (the
// driver OUTPUT alternates throttle/brake under the game's reverse speed
// limiter, so it cannot drive the latch) and publishes the armed state. The
// mover-flag clear experiment was removed after the build-36A31CC8 capture
// proved the game re-sets the flag every frame from the held key.
void MaybeRunModernBrakeReverseFlagGate(std::uintptr_t /*mover*/,
                                        std::uint32_t input_flags,
                                        float signed_speed_mps) noexcept {
    static bool armed = false;
    static std::atomic<int> armed_log_budget{20};
    const bool reverse_held = (input_flags & 1u) != 0;
    const bool flags_valid = input_flags != 0;
    if (!flags_valid) {
        return;
    }
    if (!reverse_held) {
        if (armed) {
            armed = false;
            g_br_camera_armed.store(false, std::memory_order_relaxed);
        }
        return;
    }
    if (signed_speed_mps > kBrakeReverseArmSpeedMps && !armed) {
        armed = true;
        g_br_camera_armed.store(true, std::memory_order_relaxed);
        int remaining = armed_log_budget.load(std::memory_order_relaxed);
        if (remaining > 0 &&
            armed_log_budget.compare_exchange_strong(
                remaining, remaining - 1, std::memory_order_relaxed)) {
            log::InfoF("modern_brake_reverse: brake-from-motion latch armed "
                       "(reverse held while rolling forward)");
        }
    }
}

// ---------------------------------------------------------------------------

vehicle_camera::CameraFeatureRequests CurrentVehicleCameraFeatureRequests()
    noexcept {
    return vehicle_camera::CameraFeatureRequests{
        .gta_iv_car_camera = g_config.gta_iv_car_camera,
        .gta_iv_bike_camera = g_config.gta_iv_bike_camera,
        .gta_v_speed_shake = g_config.gtav_speed_shake,
        // NeedsVehicleCameraCoreHooks() ORs all of these, and the
        // designated initialiser lists were carrying only seven: converting the
        // donor's positional lists dropped these four. Each is therefore inert
        // when it is the ONLY camera flag set - including
        // DisableControllerCameraAutocenter, which the README documents as
        // working in vehicles without either GTA IV camera flag, which is
        // exactly that case. Declaration order is required for C++20
        // designated initialisers: these sit before speed_rumble.
        .disable_controller_camera_autocenter = g_config.disable_controller_camera_autocenter,
        .disable_onfoot_camera_autocenter = g_config.disable_onfoot_camera_autocenter,
        .driving_swear_lines = g_config.driving_swear_lines,
        // The speed-rumble path publishes vehicle speed from inside the vehicle
        // camera detour, so omitting this installs the hooks only when some
        // OTHER camera feature is on: with SpeedRumble=1 alone, NeedsVehicle-
        // CameraCoreHooks() reads false, the detour never exists, the intensity
        // is never computed, and PushSpeedRumble() pushes zero forever.
        .speed_rumble = g_config.speed_rumble,
        .disable_pursuit_camera = g_config.disable_pursuit_camera,
        // NeedsVehicleCameraCoreHooks() ORs all of these. This builder
        // is the one IsVehicleCameraCoreFeatureRequested() reads at runtime;
        // dropping fix_chase_aim_camera here left the chase-aim floor dead in
        // its own default config even with the hooks installed.
        .fix_chase_aim_camera = g_config.fix_chase_aim_camera,
        .modern_brake_reverse = g_config.modern_brake_reverse,
        .no_wheel_turn_back = g_config.no_wheel_turn_back,
    };
}

bool IsVehicleCameraCoreFeatureRequested() noexcept {
    return vehicle_camera::NeedsVehicleCameraCoreHooks(
        CurrentVehicleCameraFeatureRequests());
}

gtav_speed_shake::Tuning CurrentGtaVSpeedShakeTuning() noexcept {
    constexpr float kKilometresPerHourPerMetrePerSecond = 3.6f;
    return gtav_speed_shake::NormalizeTuning(
        gtav_speed_shake::Tuning{
            static_cast<float>(
                g_config.gtav_speed_shake_strength_percent) /
                100.0f,
            static_cast<float>(
                g_config.gtav_speed_shake_start_speed_kmh) /
                kKilometresPerHourPerMetrePerSecond,
            static_cast<float>(
                g_config.gtav_speed_shake_full_speed_kmh) /
                kKilometresPerHourPerMetrePerSecond,
        });
}

bool IsGtaIvVehicleCameraClassEnabled(
    vehicle_camera::VehicleClass vehicle_class) noexcept {
    switch (vehicle_class) {
        case vehicle_camera::VehicleClass::CarPhysicsMover:
            return g_config.gta_iv_car_camera;
        case vehicle_camera::VehicleClass::Motorcycle:
            return g_config.gta_iv_bike_camera;
        default:
            return false;
    }
}

bool IsVehicleCameraClassRequired(
    vehicle_camera::VehicleClass vehicle_class) noexcept {
    switch (vehicle_class) {
        case vehicle_camera::VehicleClass::CarPhysicsMover:
            return g_config.gta_iv_car_camera || g_config.gtav_speed_shake;
        case vehicle_camera::VehicleClass::Motorcycle:
            return g_config.gta_iv_bike_camera || g_config.gtav_speed_shake;
        default:
            return false;
    }
}

bool TryBuildDriverSideCameraParameters(
    void* component,
    const void* selected_parameters,
    vehicle_camera::ParameterBlock& adjusted_parameters,
    DriverSideCameraDecision& decision) noexcept {
    using namespace vehicle_camera;

    decision = {};
    const bool transaction_ready =
        g_behavior_transaction_ready.load(std::memory_order_acquire);
    const std::uintptr_t component_address =
        reinterpret_cast<std::uintptr_t>(component);
    const std::uintptr_t selected_address =
        reinterpret_cast<std::uintptr_t>(selected_parameters);
    if (!IsVehicleCameraCoreFeatureRequested() || !transaction_ready ||
        component_address == 0 || selected_address == 0) {
        return false;
    }

    std::uintptr_t subject = 0;
    std::uintptr_t target_parameters = 0;
    if (!SafeRead(component_address + kSubjectOffset, subject) ||
        !SafeRead(component_address + kTargetParametersOffset,
                   target_parameters) ||
        subject == 0 || target_parameters == 0) {
        return false;
    }
    decision.base_drive_branch_readable = TryReadBaseSelectorBranch(
        subject, decision.base_selector_branch);
    decision.base_drive_branch_selected =
        decision.base_selector_branch == BaseSelectorBranch::Drive;
    if (!decision.base_drive_branch_readable) {
        return false;
    }
    decision.subject = subject;

    std::uintptr_t physics_mover = 0;
    std::uintptr_t physics_mover_vtable = 0;
    if (!SafeRead(subject + kPhysicsMoverOffset, physics_mover) ||
        physics_mover == 0 ||
        !SafeRead(physics_mover, physics_mover_vtable)) {
        return false;
    }
    decision.physics_mover = physics_mover;
    decision.vehicle_class = ClassifyVehicle(
        ToRva(physics_mover_vtable), g_use_latest_steam_layout);
    const bool class_required =
        IsVehicleCameraClassRequired(decision.vehicle_class);
    if (!class_required) {
        return false;
    }

    std::array<std::uintptr_t, kParameterSlotOffsets.size()> slot_values{};
    for (std::size_t index = 0; index < kParameterSlotOffsets.size(); ++index) {
        if (!SafeRead(subject + kParameterSlotOffsets[index],
                      slot_values[index])) {
            return false;
        }
    }
    ParameterIdentity target_identity{};
    if (!ReadCameraParameterIdentity(target_parameters, target_identity)) {
        return false;
    }
    decision.target_slot_match_mask =
        ProfileSlotMatchMask(target_parameters, slot_values);
    if (g_config.enable_logging) {
        decision.selected_slot_match_mask =
            ProfileSlotMatchMask(selected_address, slot_values);
    }
    decision.target_profile = ClassifyDriverSideTargetProfileFromMatchMask(
        decision.vehicle_class,
        target_parameters,
        decision.target_slot_match_mask,
        slot_values,
        target_identity,
        decision.base_selector_branch);
    decision.policy_evaluated = true;
    if (decision.target_profile == DriverSideTargetProfile::None) {
        // When an outgoing supported snapshot targets an unsupported profile,
        // the game's stock transition naturally fades the existing offset.
        return false;
    }

    // Speed shake needs the stable vehicle/profile decision above, but it
    // never mutates the selected parameter block. Avoid the GTA-IV-only
    // look-back/blend reads when this vehicle class was classified solely for
    // the independent speed-shake feature.
    if (!IsGtaIvVehicleCameraClassEnabled(decision.vehicle_class)) {
        return false;
    }

    unsigned char looking_back = 0;
    unsigned char aim_or_focus = 0;
    float transition_source_weight = 0.0f;
    if (!SafeRead(component_address + kLookingBackFlagOffset,
                   looking_back) ||
        !SafeRead(component_address + kAimOrFocusFlagOffset,
                   aim_or_focus) ||
        !SafeRead(component_address + kTransitionSourceWeightOffset,
                  transition_source_weight)) {
        return false;
    }

    const DriverSideEligibility eligibility{
        g_config.gta_iv_car_camera,
        g_config.gta_iv_bike_camera,
        transaction_ready,
        decision.vehicle_class,
        decision.target_profile,
        looking_back != 0,
        aim_or_focus != 0,
        selected_address,
        target_parameters,
        transition_source_weight,
    };
    const float blend_factor = ResolveDriverSideBlendFactor(eligibility);
    decision.blend_factor = blend_factor;
    if (blend_factor <= 0.0f ||
        !SafeCopyBytes(selected_address,
                       adjusted_parameters.data(),
                       adjusted_parameters.size())) {
        return false;
    }

    if (OffsetDriverSideLateralParameters(
            adjusted_parameters,
            kDriverSideLateralOffsetMeters * blend_factor) !=
        ParameterOffsetResult::Applied) {
        return false;
    }
    return true;
}

void ObserveChaseCameraParameters(void* component,
                                  const void* selected_parameters,
                                  bool mutation_applied,
                                  const DriverSideCameraDecision& decision) {
    using namespace vehicle_camera;

    if (!ClaimCameraProbeSample()) {
        return;
    }
    const unsigned long long sample =
        g_gtaiv_camera_probe_sample_count.fetch_add(
            1, std::memory_order_relaxed) +
        1;
    const std::uintptr_t component_address =
        reinterpret_cast<std::uintptr_t>(component);
    const std::uintptr_t selected_address =
        reinterpret_cast<std::uintptr_t>(selected_parameters);

    std::uintptr_t subject = 0;
    const bool subject_readable =
        component_address != 0 &&
        SafeRead(component_address + kSubjectOffset, subject);
    struct ActiveCameraProbeFields {
        std::uintptr_t target_parameters = 0;
        unsigned char update_eye_offset = 0;
        unsigned char state_b5a = 0;
        unsigned char state_b5b = 0;
        unsigned char state_b5c = 0;
        unsigned char looking_back = 0;
        float transition_source_weight = 0.0f;
    } candidate_fields{};
    const bool active_fields_readable =
        subject_readable && subject != 0 &&
        SafeRead(component_address + kTargetParametersOffset,
                 candidate_fields.target_parameters) &&
        SafeRead(component_address + kUpdateEyeOffsetFlagOffset,
                 candidate_fields.update_eye_offset) &&
        SafeRead(component_address + 0xB5A, candidate_fields.state_b5a) &&
        SafeRead(component_address + 0xB5B, candidate_fields.state_b5b) &&
        SafeRead(component_address + kAimOrFocusFlagOffset,
                 candidate_fields.state_b5c) &&
        SafeRead(component_address + kLookingBackFlagOffset,
                 candidate_fields.looking_back) &&
        SafeRead(component_address + kTransitionSourceWeightOffset,
                 candidate_fields.transition_source_weight);
    const bool source_weight_valid =
        active_fields_readable && IsValidTransitionSourceWeight(
            candidate_fields.transition_source_weight);
    ActiveCameraProbeFields active_fields{};
    if (active_fields_readable) {
        active_fields = candidate_fields;
        active_fields.update_eye_offset =
            active_fields.update_eye_offset != 0 ? 1 : 0;
        active_fields.state_b5a = active_fields.state_b5a != 0 ? 1 : 0;
        active_fields.state_b5b = active_fields.state_b5b != 0 ? 1 : 0;
        active_fields.state_b5c = active_fields.state_b5c != 0 ? 1 : 0;
        active_fields.looking_back =
            active_fields.looking_back != 0 ? 1 : 0;
        if (!source_weight_valid) {
            active_fields.transition_source_weight = 0.0f;
        }
    }
    const std::uintptr_t target_parameters =
        active_fields.target_parameters;
    const unsigned char update_eye_offset =
        active_fields.update_eye_offset;
    const unsigned char state_b5a = active_fields.state_b5a;
    const unsigned char state_b5b = active_fields.state_b5b;
    const unsigned char state_b5c = active_fields.state_b5c;
    const unsigned char looking_back = active_fields.looking_back;
    const float transition_source_weight =
        active_fields.transition_source_weight;

    std::array<std::uintptr_t, kParameterSlotOffsets.size()> slot_values{};
    unsigned int slot_read_mask = 0;
    if (subject != 0) {
        for (std::size_t index = 0; index < kParameterSlotOffsets.size();
             ++index) {
            if (SafeRead(subject + kParameterSlotOffsets[index],
                         slot_values[index])) {
                slot_read_mask |= 1u << static_cast<unsigned int>(index);
            }
        }
    }
    std::uintptr_t physics_mover = 0;
    std::uintptr_t physics_mover_vtable = 0;
    if (subject != 0 &&
        SafeRead(subject + kPhysicsMoverOffset, physics_mover) &&
        physics_mover != 0) {
        (void)SafeRead(physics_mover, physics_mover_vtable);
    }
    const std::uintptr_t physics_mover_vtable_rva =
        ToRva(physics_mover_vtable);
    const VehicleClass vehicle_class =
        ClassifyVehicle(physics_mover_vtable_rva,
                        g_use_latest_steam_layout);
    BaseSelectorBranch observed_base_selector_branch =
        BaseSelectorBranch::Unknown;
    const bool observed_base_drive_branch_readable =
        TryReadBaseSelectorBranch(subject, observed_base_selector_branch);
    const bool observed_base_drive_branch_selected =
        observed_base_selector_branch == BaseSelectorBranch::Drive;

    const ChaseParameterProbeSnapshot target =
        ReadChaseParameterProbeSnapshot(target_parameters);
    const std::uintptr_t logged_target_parameters =
        target.readable ? target_parameters : 0;
    const std::size_t selected_slot =
        ProfileSlotOffsetFor(selected_address, slot_values);
    const std::size_t target_slot =
        ProfileSlotOffsetFor(logged_target_parameters, slot_values);
    const ParameterSlotMask observed_selected_slot_match_mask =
        ProfileSlotMatchMask(selected_address, slot_values);
    const ParameterSlotMask observed_target_slot_match_mask =
        ProfileSlotMatchMask(logged_target_parameters, slot_values);
    const bool transition_local =
        active_fields_readable && source_weight_valid && target.readable &&
        selected_address != target_parameters &&
        transition_source_weight > 0.0f;
    ChaseParameterProbeSnapshot selected{};
    selected.address = selected_address;
    if (!transition_local) {
        selected = ReadChaseParameterProbeSnapshot(selected_address);
    }
    const ChaseParameterProbeSnapshot transition_source =
        transition_local
            ? ReadChaseParameterProbeSnapshot(
                  component_address + kTransitionSourceParametersOffset)
            : ChaseParameterProbeSnapshot{};
    const ParameterIdentity observed_target_identity{
        target.name_pointer,
        target.name_symbol,
        target.context,
    };
    constexpr unsigned int kAllParameterSlotsReadMask =
        (1u << kParameterSlotOffsets.size()) - 1u;
    const bool all_parameter_slots_read =
        slot_read_mask == kAllParameterSlotsReadMask;
    const DriverSideTargetProfile observed_target_profile =
        target.readable && all_parameter_slots_read
            ? ClassifyDriverSideTargetProfileFromMatchMask(
                  vehicle_class,
                  logged_target_parameters,
                  observed_target_slot_match_mask,
                  slot_values,
                  observed_target_identity,
                  observed_base_selector_branch)
            : DriverSideTargetProfile::None;
    const bool class_enabled =
        IsGtaIvVehicleCameraClassEnabled(vehicle_class);
    const DriverSideTargetProfile logged_target_profile =
        decision.policy_evaluated ? decision.target_profile
                                  : observed_target_profile;
    const ParameterSlotMask logged_target_slot_match_mask =
        decision.policy_evaluated ? decision.target_slot_match_mask
                                  : observed_target_slot_match_mask;
    const ParameterSlotMask logged_selected_slot_match_mask =
        decision.policy_evaluated ? decision.selected_slot_match_mask
                                  : observed_selected_slot_match_mask;
    const bool logged_base_drive_branch_readable =
        decision.policy_evaluated
            ? decision.base_drive_branch_readable
            : observed_base_drive_branch_readable;
    const bool logged_base_drive_branch_selected =
        decision.policy_evaluated
            ? decision.base_drive_branch_selected
            : observed_base_drive_branch_selected;
    const BaseSelectorBranch logged_base_selector_branch =
        decision.policy_evaluated ? decision.base_selector_branch
                                  : observed_base_selector_branch;
    const bool probe_readable =
        active_fields_readable && source_weight_valid && target.readable &&
        (transition_local ? transition_source.readable : selected.readable) &&
        all_parameter_slots_read && logged_base_drive_branch_readable;

    const std::uint32_t state_context =
        target.readable ? target.context : selected.context;
    const std::uint32_t state_symbol =
        target.readable ? target.name_symbol : selected.name_symbol;
    unsigned long long state_key = 14695981039346656037ull;
    const auto mix_state = [&state_key](unsigned long long value) noexcept {
        state_key ^= value;
        state_key *= 1099511628211ull;
    };
    mix_state(state_symbol);
    mix_state(state_context);
    mix_state(logged_target_slot_match_mask);
    mix_state(logged_selected_slot_match_mask);
    mix_state(static_cast<unsigned long long>(looking_back != 0));
    mix_state(static_cast<unsigned long long>(state_b5a != 0));
    mix_state(static_cast<unsigned long long>(state_b5b != 0));
    mix_state(static_cast<unsigned long long>(state_b5c != 0));
    mix_state(static_cast<unsigned long long>(vehicle_class));
    mix_state(static_cast<unsigned long long>(logged_target_profile));
    mix_state(static_cast<unsigned long long>(logged_base_selector_branch));
    mix_state(static_cast<unsigned long long>(class_enabled));
    mix_state(static_cast<unsigned long long>(mutation_applied));
    mix_state(static_cast<unsigned long long>(transition_local));
    mix_state(static_cast<unsigned long long>(probe_readable));
    {
        std::lock_guard<std::mutex> lock(g_gtaiv_camera_probe_state_mutex);
        if (g_gtaiv_camera_probe_last_state == state_key &&
            g_gtaiv_camera_probe_last_subject == subject) {
            return;
        }
        g_gtaiv_camera_probe_last_state = state_key;
        g_gtaiv_camera_probe_last_subject = subject;
    }
    if (!ClaimCameraProbeLogBudget()) {
        return;
    }

    std::uintptr_t subject_vtable = 0;
    std::uint32_t subject_type_uid = 0;
    std::uint32_t subject_name_uid = 0;
    std::uint16_t subject_flags = 0;
    std::uintptr_t sim_object = 0;
    if (subject != 0) {
        (void)SafeRead(subject, subject_vtable);
        (void)SafeRead(subject + 0x18, subject_type_uid);
        (void)SafeRead(subject + 0x1C, subject_name_uid);
        (void)SafeRead(subject + 0x20, subject_flags);
        (void)SafeRead(subject + 0x28, sim_object);
    }
    const std::string target_name =
        target.readable ? ReadCString(target.name_pointer) : std::string{};
    const std::string selected_name =
        selected.readable ? ReadCString(selected.name_pointer) : std::string{};
    const std::string transition_source_name =
        transition_source.readable
            ? ReadCString(transition_source.name_pointer)
            : std::string{};
    const ChaseParameterProbeSnapshot& observed_parameters =
        target.readable ? target : selected;
    const char* observed_parameter_source =
        target.readable ? "target" : (selected.readable ? "selected" : "none");

    log::InfoF(
        "gtaiv_vehicle_camera_probe event=state_change mode=active mutation=%d "
        "car_enabled=%d bike_enabled=%d class_enabled=%d "
        "policy_evaluated=%d base_drive_branch_readable=%d "
        "base_drive_branch_selected=%d base_selector_branch=%s "
        "target_profile=%s "
        "target_slot_match_mask=0x%04X selected_slot_match_mask=0x%04X "
        "readable=%d blend_factor=%.3f base_offset_m=%.3f "
        "applied_delta_m=%.3f active_fields_readable=%d "
        "source_weight_valid=%d "
        "sample=%llu component=0x%p subject=0x%p subject_vtable=0x%p "
        "subject_type_uid=0x%08X subject_name_uid=0x%08X subject_flags=0x%04X "
        "sim_object=0x%p physics_mover=0x%p mover_vtable=0x%p "
        "mover_vtable_rva=0x%llX vehicle_class=%s "
        "target_parameters=0x%p selected_parameters=0x%p "
        "target_slot=0x%zX selected_slot=0x%zX selected_kind=%s "
        "target_context=%u selected_context=%u target_symbol=0x%08X "
        "selected_symbol=0x%08X target_name=\"%s\" selected_name=\"%s\" "
        "transition_source=0x%p source_context=%u source_symbol=0x%08X "
        "source_name=\"%s\" source_weight=%.6f update_eye=%u looking_back=%u "
        "state_b5a=%u state_b5b=%u state_b5c=%u "
        "parameter_source=%s yaw_window_s=%.6f look_offset_max=%.6f "
        "look_yaw_rate_min=%.6f look_yaw_rate_max=%.6f "
        "pitch_max=%.6f pitch_look_up=%.6f pitch_look_down=%.6f "
        "pitch_eye_up=%.6f pitch_eye_down=%.6f "
        "centering_speed_min=%.6f centering_speed_max=%.6f "
        "reverse_threshold_mps=%.6f unused_orbit_speed=%.6f "
        "slot_read_mask=0x%04X "
        "slots=370:0x%p,378:0x%p,380:0x%p,388:0x%p,390:0x%p,"
        "398:0x%p,3A0:0x%p,3A8:0x%p,3B0:0x%p,3B8:0x%p,"
        "3C0:0x%p,3C8:0x%p,3D0:0x%p",
        mutation_applied ? 1 : 0,
        g_config.gta_iv_car_camera ? 1 : 0,
        g_config.gta_iv_bike_camera ? 1 : 0,
        class_enabled ? 1 : 0,
        decision.policy_evaluated ? 1 : 0,
        logged_base_drive_branch_readable ? 1 : 0,
        logged_base_drive_branch_selected ? 1 : 0,
        BaseSelectorBranchName(logged_base_selector_branch),
        DriverSideTargetProfileName(logged_target_profile),
        static_cast<unsigned int>(logged_target_slot_match_mask),
        static_cast<unsigned int>(logged_selected_slot_match_mask),
        probe_readable ? 1 : 0,
        static_cast<double>(decision.blend_factor),
        static_cast<double>(kDriverSideLateralOffsetMeters),
        static_cast<double>(
            mutation_applied
                ? kDriverSideLateralOffsetMeters * decision.blend_factor
                : 0.0f),
        active_fields_readable ? 1 : 0,
        source_weight_valid ? 1 : 0,
        sample,
        component,
        reinterpret_cast<void*>(subject),
        reinterpret_cast<void*>(subject_vtable),
        subject_type_uid,
        subject_name_uid,
        static_cast<unsigned int>(subject_flags),
        reinterpret_cast<void*>(sim_object),
        reinterpret_cast<void*>(physics_mover),
        reinterpret_cast<void*>(physics_mover_vtable),
        static_cast<unsigned long long>(physics_mover_vtable_rva),
        VehicleClassName(vehicle_class),
        reinterpret_cast<void*>(logged_target_parameters),
        selected_parameters,
        target_slot,
        selected_slot,
        transition_local ? "transition_local" : "canonical",
        target.context,
        selected.context,
        target.name_symbol,
        selected.name_symbol,
        target_name.empty() ? "<unreadable>" : target_name.c_str(),
        transition_local
            ? "<transition_local_header_uninitialized>"
            : (selected_name.empty() ? "<unreadable>" :
                                       selected_name.c_str()),
        reinterpret_cast<void*>(transition_source.address),
        transition_source.context,
        transition_source.name_symbol,
        transition_source_name.empty() ? "<not_active_or_unreadable>" :
                                         transition_source_name.c_str(),
        static_cast<double>(transition_source_weight),
        static_cast<unsigned int>(update_eye_offset),
        static_cast<unsigned int>(looking_back),
        static_cast<unsigned int>(state_b5a),
        static_cast<unsigned int>(state_b5b),
        static_cast<unsigned int>(state_b5c),
        observed_parameter_source,
        static_cast<double>(observed_parameters.yaw_ang_vel_timespan),
        static_cast<double>(observed_parameters.look_offset_max),
        static_cast<double>(observed_parameters.look_offset_ang_vel_min),
        static_cast<double>(observed_parameters.look_offset_ang_vel_max),
        static_cast<double>(observed_parameters.pitch_offset_max),
        static_cast<double>(observed_parameters.pitch_offset_look_factor_up),
        static_cast<double>(observed_parameters.pitch_offset_look_factor_down),
        static_cast<double>(observed_parameters.pitch_offset_eye_factor_up),
        static_cast<double>(observed_parameters.pitch_offset_eye_factor_down),
        static_cast<double>(observed_parameters.centering_speed_min),
        static_cast<double>(observed_parameters.centering_speed_max),
        static_cast<double>(observed_parameters.reverse_speed),
        static_cast<double>(observed_parameters.orbit_speed),
        slot_read_mask,
        reinterpret_cast<void*>(slot_values[0]),
        reinterpret_cast<void*>(slot_values[1]),
        reinterpret_cast<void*>(slot_values[2]),
        reinterpret_cast<void*>(slot_values[3]),
        reinterpret_cast<void*>(slot_values[4]),
        reinterpret_cast<void*>(slot_values[5]),
        reinterpret_cast<void*>(slot_values[6]),
        reinterpret_cast<void*>(slot_values[7]),
        reinterpret_cast<void*>(slot_values[8]),
        reinterpret_cast<void*>(slot_values[9]),
        reinterpret_cast<void*>(slot_values[10]),
        reinterpret_cast<void*>(slot_values[11]),
        reinterpret_cast<void*>(slot_values[12]));
}

bool PrepareGtaVSpeedShakeSample(
    ChaseCameraUpdateProbeScope& scope) noexcept;
void MaybeFireDrivingSwearLine(ChaseCameraUpdateProbeScope& scope) noexcept;

// The base selector chooses the Flee block ahead of Drive, which is the camera
// behavior that engages during a police pursuit. Swapping that block for the
// Drive block keeps the ordinary road camera while the chase is running.
// RESTORED 2026-10-31 from the recovery snapshot: this arm was lost in the
// heat-meter-restore restructure, leaving DisablePursuitCamera inert on every
// layout (it only kept the hooks installed).
[[nodiscard]] const void* MaybeSubstituteFleeCameraBlock(void* component,
                                                        const void* parameters) {
    if (!g_config.disable_pursuit_camera || component == nullptr ||
        parameters == nullptr) {
        return parameters;
    }
    // The parameter slot array lives on the VehicleSubjectComponent, reached
    // from the chase camera component at kSubjectOffset - it is NOT on the
    // component that owns this setter. The gate is the game's own flee state
    // (the same signal the base selector reads) rather than the incoming
    // block pointer, so the substitution also covers the frames where the
    // camera is still blending towards the Flee block and is therefore not
    // that block yet.
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(component);
    std::uintptr_t subject = 0;
    if (!SafeRead(base + vehicle_camera::kSubjectOffset, subject) ||
        subject == 0) {
        return parameters;
    }
    vehicle_camera::BaseSelectorBranch branch =
        vehicle_camera::BaseSelectorBranch::Unknown;
    if (!TryReadBaseSelectorBranch(subject, branch) ||
        branch != vehicle_camera::BaseSelectorBranch::Flee) {
        return parameters;
    }
    std::uintptr_t drive_slot = 0;
    if (!SafeRead(subject + vehicle_camera::kDriveParametersSlotOffset,
                  drive_slot) ||
        drive_slot == 0) {
        return parameters;
    }
    static std::atomic<bool> logged{false};
    bool expected = false;
    if (g_config.enable_logging &&
        logged.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        log::InfoF("pursuit_camera: flee state active; using drive block 0x%llX "
                   "(subject=0x%llX)",
                   static_cast<unsigned long long>(drive_slot),
                   static_cast<unsigned long long>(subject));
    }
    return reinterpret_cast<const void*>(drive_slot);
}

void DetourChaseCameraSetParameters(void* component,
                                    const void* parameters) {
    // The block is populated atomically before mutation_applied can become
    // true. Leaving it uninitialized avoids zeroing 0x140 bytes on every
    // speed-shake-only selector call, where the original block is always used.
    alignas(16) vehicle_camera::ParameterBlock adjusted_parameters;
    const void* effective_parameters =
        MaybeSubstituteFleeCameraBlock(component, parameters);
    DriverSideCameraDecision decision{};
    const bool mutation_applied = TryBuildDriverSideCameraParameters(
        component, effective_parameters, adjusted_parameters, decision);
    vehicle_camera::InvokeSelectedParameterBlockOnce(
        [&](const void* selected_parameters) noexcept {
            g_chase_camera_set_parameters_original(component,
                                                   selected_parameters);
        },
        effective_parameters,
        adjusted_parameters,
        mutation_applied);
    if (ChaseCameraUpdateProbeScope* scope =
            FindActiveChaseCameraUpdateProbeScope(component)) {
        ++scope->calls.setter_calls;
        scope->selected_parameters = effective_parameters;
        scope->decision = decision;
        scope->mutation_applied = mutation_applied;
        if (scope->depth == 1) {
            (void)PrepareGtaVSpeedShakeSample(*scope);
            (void)MaybeFireDrivingSwearLine(*scope);
            (void)MaybeObserveModernBrakeReversePlayerState(*scope);
        }
    }
    if (g_behavior_transaction_ready.load(std::memory_order_acquire) &&
        IsVehicleCameraCoreFeatureRequested() &&
        g_config.enable_logging) {
        ObserveChaseCameraParameters(component,
                                     effective_parameters,
                                     mutation_applied,
                                     decision);
    }
}

bool TryBuildGtaIvDesiredPose(
    ChaseCameraUpdateProbeScope& scope,
    float delta_seconds,
    const CameraVector3& stock_eye,
    const CameraVector3& stock_look,
    const CameraVector3& stock_up,
    CameraVector3& adjusted_eye,
    CameraVector3& adjusted_look,
    CameraVector3& adjusted_up) noexcept {
    using namespace vehicle_camera;

    if (!g_behavior_transaction_ready.load(std::memory_order_acquire) ||
        scope.depth != 1 || !std::isfinite(delta_seconds)) {
        return false;
    }
    if (ShouldResetGtaIvManualPitchForUpdate(scope.depth, delta_seconds)) {
        ResetGtaIvVehicleCameraDynamicStateForComponent(scope.component);
        return false;
    }

    bool has_native_override = false;
    if (!IsGtaIvVehicleCameraDynamicDecisionEnabled(scope.decision) ||
        !TryReadNativeVehicleCameraOverride(scope.component,
                                            has_native_override) ||
        has_native_override || !scope.stock_mouse_eye_offset_readable ||
        !std::isfinite(scope.stock_mouse_eye_offset)) {
        ResetGtaIvVehicleCameraDynamicState();
        return false;
    }

    std::uint32_t input_flags = 0;
    if (!SafeRead(scope.decision.physics_mover +
                      kPhysicsMoverInputFlagsOffset,
                  input_flags) ||
        IsPhysicsMoverReversing(input_flags)) {
        ResetGtaIvVehicleCameraDynamicStateForComponent(scope.component);
        return false;
    }

    float vertical_input = 0.0f;
    if (!SafeRead(reinterpret_cast<std::uintptr_t>(scope.component) +
                      kLookupJoyInputOffset,
                  vertical_input) ||
        !std::isfinite(vertical_input)) {
        ResetGtaIvVehicleCameraDynamicState();
        return false;
    }

    GtaIvVehicleCameraDynamicState& state =
        g_gtaiv_vehicle_camera_dynamic_state;
    if (state.component != scope.component ||
        state.subject != scope.decision.subject) {
        state = {};
        state.component = scope.component;
        state.subject = scope.decision.subject;
    }

    const float applied_mouse_eye_offset = scope.stock_mouse_eye_offset;
    state.manual_pitch = StepGtaIvManualPitch(
        state.manual_pitch,
        ManualPitchFrameInput{
            delta_seconds,
            vertical_input,
        });
    const float manual_pitch_offset = ResolveGtaIvManualPitchOffset(
        state.manual_pitch.angle_radians, applied_mouse_eye_offset);
    scope.manual_pitch_offset_radians = manual_pitch_offset;

    adjusted_eye = stock_eye;
    adjusted_look = stock_look;
    adjusted_up = stock_up;
    adjusted_eye.z -= applied_mouse_eye_offset;

    const float radius_x = adjusted_eye.x - adjusted_look.x;
    const float radius_y = adjusted_eye.y - adjusted_look.y;
    const float radius_z = adjusted_eye.z - adjusted_look.z;
    const float horizontal_distance =
        std::sqrt(radius_x * radius_x + radius_y * radius_y);
    const float distance =
        std::sqrt(horizontal_distance * horizontal_distance +
                  radius_z * radius_z);
    if (!std::isfinite(horizontal_distance) || !std::isfinite(distance) ||
        horizontal_distance <= 0.0001f || distance <= 0.0001f) {
        return false;
    }

    const float stock_pitch = std::atan2(radius_z, horizontal_distance);
    const float applied_pitch = ClampCameraValue(
        stock_pitch + manual_pitch_offset,
        kGtaIvMinimumEyeOrbitPitchRadians,
        kGtaIvMaximumEyeOrbitPitchRadians);
    const float adjusted_horizontal_distance =
        std::cos(applied_pitch) * distance;
    const float horizontal_scale =
        adjusted_horizontal_distance / horizontal_distance;
    adjusted_eye.x = adjusted_look.x + radius_x * horizontal_scale;
    adjusted_eye.y = adjusted_look.y + radius_y * horizontal_scale;
    adjusted_eye.z = adjusted_look.z + std::sin(applied_pitch) * distance;
    if (!std::isfinite(adjusted_eye.x) ||
        !std::isfinite(adjusted_eye.y) ||
        !std::isfinite(adjusted_eye.z)) {
        return false;
    }

    return CameraAbs(manual_pitch_offset) > 0.0001f ||
           CameraAbs(applied_mouse_eye_offset) > 0.0001f;
}

bool PrepareGtaVSpeedShakeSample(
    ChaseCameraUpdateProbeScope& scope) noexcept {
    using namespace vehicle_camera;

    if (scope.speed_shake_sample_prepared) {
        return scope.speed_shake_eligible;
    }
    scope.speed_shake_sample_prepared = true;
    scope.speed_shake_sample = {};

    if (!g_config.gtav_speed_shake ||
        !g_behavior_transaction_ready.load(std::memory_order_acquire) ||
        scope.component == nullptr) {
        ResetGtaVSpeedShakeStateForComponent(scope.component);
        return false;
    }
    if (!std::isfinite(scope.delta_seconds) ||
        gtav_speed_shake::ShouldPreserveRuntimeStateWithoutAdvance(
            scope.depth, scope.delta_seconds)) {
        // GotoAngleSnap can nest Chase Update with dt=0.  That call must not
        // advance or erase the outer camera's persistent shaker state.
        return false;
    }

    const std::uintptr_t component_address =
        reinterpret_cast<std::uintptr_t>(scope.component);
    unsigned char aim_or_focus = 0;
    unsigned char eye_locked = 0;
    unsigned char look_locked = 0;
    std::uintptr_t verified_subject = 0;
    std::uintptr_t verified_physics_mover = 0;
    float signed_forward_speed = 0.0f;
    const bool camera_state_readable =
        SafeRead(component_address + kSubjectOffset, verified_subject) &&
        verified_subject == scope.decision.subject &&
        verified_subject != 0 &&
        SafeRead(verified_subject + kPhysicsMoverOffset,
                 verified_physics_mover) &&
        verified_physics_mover == scope.decision.physics_mover &&
        verified_physics_mover != 0 &&
        SafeRead(component_address + kAimOrFocusFlagOffset, aim_or_focus) &&
        SafeRead(component_address + kEyeLockFlagOffset, eye_locked) &&
        SafeRead(component_address + kLookLockFlagOffset, look_locked) &&
        SafeRead(verified_physics_mover +
                     kPhysicsMoverForwardSpeedOffset,
                 signed_forward_speed) &&
        std::isfinite(signed_forward_speed);
    scope.speed_shake_signed_forward_speed =
        camera_state_readable ? signed_forward_speed : 0.0f;
    // Recompute the speed-rumble intensity from the speed this camera path just
    // published. Deliberately unconditional on speed_shake_eligible: the field
    // is already zeroed when the camera state is unreadable, so the call is the
    // safe no-speed case rather than a stale-speed case.
    UpdateSpeedRumbleFromSpeed(scope.speed_shake_signed_forward_speed);

    scope.speed_shake_eligible = IsGtaVSpeedShakeEligible(
        GtaVSpeedShakeEligibility{
            g_config.gtav_speed_shake,
            g_behavior_transaction_ready.load(std::memory_order_acquire),
            camera_state_readable,
            scope.decision.vehicle_class,
            scope.decision.target_profile,
            aim_or_focus != 0,
            eye_locked != 0,
            look_locked != 0,
        });
    if (!scope.speed_shake_eligible) {
        ResetGtaVSpeedShakeStateForComponent(scope.component);
        return false;
    }

    GtaVSpeedShakeRuntimeState& runtime = g_gtav_speed_shake_state;
    if (runtime.component != scope.component ||
        runtime.subject != scope.decision.subject ||
        runtime.physics_mover != scope.decision.physics_mover) {
        runtime = {};
        runtime.component = scope.component;
        runtime.subject = scope.decision.subject;
        runtime.physics_mover = scope.decision.physics_mover;
    }
    const gtav_speed_shake::Tuning tuning =
        CurrentGtaVSpeedShakeTuning();
    if (!gtav_speed_shake::Advance(runtime.shaker,
                                   signed_forward_speed,
                                   scope.delta_seconds,
                                   tuning)) {
        ResetGtaVSpeedShakeStateForComponent(scope.component);
        scope.speed_shake_eligible = false;
        return false;
    }
    scope.speed_shake_state_advanced = true;
    scope.speed_shake_sample =
        gtav_speed_shake::CurrentSample(runtime.shaker, tuning);
    return true;
}

bool TryBuildGtaVSpeedShakePose(
    ChaseCameraUpdateProbeScope& scope,
    const CameraVector3& input_eye,
    const CameraVector3& input_look,
    const CameraVector3& input_up,
    CameraVector3& adjusted_eye,
    CameraVector3& adjusted_look,
    CameraVector3& adjusted_up) noexcept {
    if (!PrepareGtaVSpeedShakeSample(scope)) {
        return false;
    }

    const gtav_speed_shake::Pose input_pose{
        {input_eye.x, input_eye.y, input_eye.z},
        {input_look.x, input_look.y, input_look.z},
        {input_up.x, input_up.y, input_up.z},
    };
    gtav_speed_shake::Pose output_pose{};
    if (!gtav_speed_shake::TryApplyToPose(
            input_pose, scope.speed_shake_sample, output_pose)) {
        return false;
    }

    adjusted_eye = CameraVector3{
        output_pose.eye.x, output_pose.eye.y, output_pose.eye.z};
    adjusted_look = CameraVector3{
        output_pose.look.x, output_pose.look.y, output_pose.look.z};
    adjusted_up = CameraVector3{
        output_pose.up.x, output_pose.up.y, output_pose.up.z};
    scope.speed_shake_pose_applied = true;
    return true;
}

void ObserveChaseCameraUpdate(
    const ChaseCameraUpdateProbeScope& scope,
    const ChaseCameraDynamicSnapshot& after) {
    using namespace vehicle_camera;

    if (!g_behavior_transaction_ready.load(std::memory_order_acquire) ||
        !g_config.enable_logging ||
        !IsVehicleCameraCoreFeatureRequested()) {
        return;
    }

    const unsigned long long sample =
        g_gtaiv_camera_dynamic_probe_sample_count.fetch_add(
            1, std::memory_order_relaxed) +
        1;
    const float yaw_angular_velocity =
        ResolveObservedYawAngularVelocity(after);
    const DriverSideTargetProfile logged_target_profile =
        after.profile_readable ? after.target_profile
                               : scope.decision.target_profile;
    const VehicleClass logged_vehicle_class =
        after.vehicle_class != VehicleClass::Unknown
            ? after.vehicle_class
            : scope.decision.vehicle_class;
    const BaseSelectorBranch logged_base_selector_branch =
        after.base_drive_branch_readable ? after.base_selector_branch
                                         : scope.decision.base_selector_branch;
    log::InfoF(
        "gtaiv_vehicle_camera_dynamic event=update sample=%llu tid=%lu "
        "component=0x%p subject=0x%p physics_mover=0x%p "
        "mover_vtable_rva=0x%llX dt=%.6f depth=%u nested_updates=%u "
        "setter_calls=%u nested_setter_calls=%u pose_calls=%u "
        "nested_pose_calls=%u selected_parameters=0x%p "
        "policy_evaluated=%d base_drive_branch_readable=%d "
        "base_drive_branch_selected=%d base_selector_branch=%s "
        "target_profile=%s vehicle_class=%s "
        "profile_readable=%d target_parameters=0x%p "
        "target_slot_match_mask=0x%04X target_context=%u "
        "target_symbol=0x%08X source_weight=%.6f "
        "source_weight_valid=%d "
        "mutation=%d dynamics_mutation=%d manual_pitch_offset=%.6f "
        "speed_shake_enabled=%d speed_shake_eligible=%d "
        "speed_shake_state_advanced=%d speed_shake_pose_applied=%d "
        "speed_shake_signed_forward_mps=%.6f "
        "speed_shake_target_factor=%.6f speed_shake_factor=%.6f "
        "speed_shake_envelope=%.6f "
        "speed_shake_offset_right_m=%.6f "
        "speed_shake_offset_up_m=%.6f "
        "speed_shake_pitch_rad=%.6f speed_shake_roll_rad=%.6f "
        "before_readable=%d after_readable=%d "
        "input_pre=%.6f,%.6f,%.6f input_post=%.6f,%.6f,%.6f "
        "center_timer_pre=%.6f center_timer_post=%.6f "
        "manual_yaw_pre=%.6f manual_yaw_post=%.6f "
        "snap_yaw_pre=%.6f snap_yaw_post=%.6f "
        "follow_yaw=%.6f desired_yaw=%.6f yaw_rate_avg=%.6f "
        "target_pitch=%.6f pitch_target=%.6f "
        "signed_forward_speed_readable=%d signed_forward_speed_mps=%.6f "
        "input_state_readable=%d steering_input=%.6f "
        "input_flags=0x%08X in_reverse=%d handbrake=%d "
        "looking_around=%u looking_back=%u aim_or_focus=%u "
        "eye_locked=%u look_locked=%u "
        "centering_min=%.6f centering_max=%.6f "
        "reverse_threshold_mps=%.6f "
        "yaw_window_s=%.6f look_offset_max=%.6f "
        "look_yaw_rate_min=%.6f look_yaw_rate_max=%.6f "
        "pitch_offset_max=%.6f",
        sample,
        scope.thread_id,
        scope.component,
        reinterpret_cast<void*>(after.subject),
        reinterpret_cast<void*>(after.physics_mover),
        static_cast<unsigned long long>(after.physics_mover_vtable_rva),
        static_cast<double>(scope.delta_seconds),
        scope.depth,
        scope.calls.nested_updates,
        scope.calls.setter_calls,
        scope.calls.nested_setter_calls,
        scope.calls.desired_pose_calls,
        scope.calls.nested_desired_pose_calls,
        scope.selected_parameters,
        scope.decision.policy_evaluated ? 1 : 0,
        after.base_drive_branch_readable ? 1 : 0,
        after.base_drive_branch_selected ? 1 : 0,
        BaseSelectorBranchName(logged_base_selector_branch),
        DriverSideTargetProfileName(logged_target_profile),
        VehicleClassName(logged_vehicle_class),
        after.profile_readable ? 1 : 0,
        reinterpret_cast<void*>(after.target_parameters),
        static_cast<unsigned int>(after.target_slot_match_mask),
        after.target_context,
        after.target_symbol,
        static_cast<double>(after.transition_source_weight),
        after.transition_source_weight_valid ? 1 : 0,
        scope.mutation_applied ? 1 : 0,
        scope.dynamics_mutation_applied ? 1 : 0,
        static_cast<double>(scope.manual_pitch_offset_radians),
        g_config.gtav_speed_shake ? 1 : 0,
        scope.speed_shake_eligible ? 1 : 0,
        scope.speed_shake_state_advanced ? 1 : 0,
        scope.speed_shake_pose_applied ? 1 : 0,
        static_cast<double>(scope.speed_shake_signed_forward_speed),
        static_cast<double>(scope.speed_shake_sample.target_speed_factor),
        static_cast<double>(scope.speed_shake_sample.speed_factor),
        static_cast<double>(scope.speed_shake_sample.envelope),
        static_cast<double>(
            scope.speed_shake_sample.offsets.relative_x_meters),
        static_cast<double>(
            scope.speed_shake_sample.offsets.relative_z_meters),
        static_cast<double>(
            scope.speed_shake_sample.offsets.rotation_x_radians),
        static_cast<double>(
            scope.speed_shake_sample.offsets.rotation_y_radians),
        scope.before.readable ? 1 : 0,
        after.readable ? 1 : 0,
        static_cast<double>(scope.before.lookaround_joy_input),
        static_cast<double>(scope.before.lookup_joy_input),
        static_cast<double>(scope.before.lookup_mouse),
        static_cast<double>(after.lookaround_joy_input),
        static_cast<double>(after.lookup_joy_input),
        static_cast<double>(after.lookup_mouse),
        static_cast<double>(scope.before.lookaround_center_timer),
        static_cast<double>(after.lookaround_center_timer),
        static_cast<double>(scope.before.lookaround_angle),
        static_cast<double>(after.lookaround_angle),
        static_cast<double>(scope.before.lookaround_angle_desired),
        static_cast<double>(after.lookaround_angle_desired),
        static_cast<double>(after.forward_angle),
        static_cast<double>(after.forward_angle_desired),
        static_cast<double>(yaw_angular_velocity),
        static_cast<double>(after.target_pitch_position),
        static_cast<double>(after.target_pitch_target),
        after.forward_speed_readable ? 1 : 0,
        static_cast<double>(after.forward_speed),
        after.input_state_readable ? 1 : 0,
        static_cast<double>(after.steering_input),
        after.input_flags,
        (after.input_flags & kPhysicsMoverInReverseFlag) != 0 ? 1 : 0,
        (after.input_flags & kPhysicsMoverHandbrakeFlag) != 0 ? 1 : 0,
        static_cast<unsigned int>(after.is_looking_around),
        static_cast<unsigned int>(after.looking_back),
        static_cast<unsigned int>(after.aim_or_focus),
        static_cast<unsigned int>(after.eye_locked),
        static_cast<unsigned int>(after.look_locked),
        static_cast<double>(after.centering_speed_min),
        static_cast<double>(after.centering_speed_max),
        static_cast<double>(after.reverse_speed),
        static_cast<double>(after.yaw_ang_vel_timespan),
        static_cast<double>(after.look_offset_max),
        static_cast<double>(after.look_offset_ang_vel_min),
        static_cast<double>(after.look_offset_ang_vel_max),
        static_cast<double>(after.pitch_offset_max));

    if (!scope.desired_pose_readable) {
        return;
    }
    const float stock_delta_x = scope.desired_look.x - scope.desired_eye.x;
    const float stock_delta_y = scope.desired_look.y - scope.desired_eye.y;
    const float stock_delta_z = scope.desired_look.z - scope.desired_eye.z;
    const float stock_horizontal_distance =
        std::sqrt(stock_delta_x * stock_delta_x +
                  stock_delta_y * stock_delta_y);
    const float stock_pose_distance =
        std::sqrt(stock_horizontal_distance * stock_horizontal_distance +
                  stock_delta_z * stock_delta_z);
    const float stock_pose_yaw = std::atan2(stock_delta_y, stock_delta_x);
    const float stock_pose_pitch =
        std::atan2(stock_delta_z, stock_horizontal_distance);
    const float delta_x =
        scope.effective_desired_look.x - scope.effective_desired_eye.x;
    const float delta_y =
        scope.effective_desired_look.y - scope.effective_desired_eye.y;
    const float delta_z =
        scope.effective_desired_look.z - scope.effective_desired_eye.z;
    const float horizontal_distance =
        std::sqrt(delta_x * delta_x + delta_y * delta_y);
    const float pose_distance =
        std::sqrt(horizontal_distance * horizontal_distance +
                  delta_z * delta_z);
    const float pose_yaw = std::atan2(delta_y, delta_x);
    const float pose_pitch = std::atan2(delta_z, horizontal_distance);
    log::InfoF(
        "gtaiv_vehicle_camera_dynamic event=desired_pose sample=%llu "
        "component=0x%p collide=%d mutation=%d "
        "manual_pitch_offset=%.6f speed_shake_pose_applied=%d "
        "speed_shake_factor=%.6f speed_shake_envelope=%.6f "
        "speed_shake_offset_right_m=%.6f "
        "speed_shake_offset_up_m=%.6f "
        "speed_shake_pitch_rad=%.6f speed_shake_roll_rad=%.6f "
        "stock_eye=%.6f,%.6f,%.6f stock_look=%.6f,%.6f,%.6f "
        "stock_up=%.6f,%.6f,%.6f stock_distance=%.6f "
        "stock_yaw=%.6f stock_pitch=%.6f "
        "eye=%.6f,%.6f,%.6f look=%.6f,%.6f,%.6f "
        "up=%.6f,%.6f,%.6f distance=%.6f yaw=%.6f pitch=%.6f",
        sample,
        scope.component,
        scope.collide ? 1 : 0,
        scope.dynamics_mutation_applied ? 1 : 0,
        static_cast<double>(scope.manual_pitch_offset_radians),
        scope.speed_shake_pose_applied ? 1 : 0,
        static_cast<double>(scope.speed_shake_sample.speed_factor),
        static_cast<double>(scope.speed_shake_sample.envelope),
        static_cast<double>(
            scope.speed_shake_sample.offsets.relative_x_meters),
        static_cast<double>(
            scope.speed_shake_sample.offsets.relative_z_meters),
        static_cast<double>(
            scope.speed_shake_sample.offsets.rotation_x_radians),
        static_cast<double>(
            scope.speed_shake_sample.offsets.rotation_y_radians),
        static_cast<double>(scope.desired_eye.x),
        static_cast<double>(scope.desired_eye.y),
        static_cast<double>(scope.desired_eye.z),
        static_cast<double>(scope.desired_look.x),
        static_cast<double>(scope.desired_look.y),
        static_cast<double>(scope.desired_look.z),
        static_cast<double>(scope.desired_up.x),
        static_cast<double>(scope.desired_up.y),
        static_cast<double>(scope.desired_up.z),
        static_cast<double>(stock_pose_distance),
        static_cast<double>(stock_pose_yaw),
        static_cast<double>(stock_pose_pitch),
        static_cast<double>(scope.effective_desired_eye.x),
        static_cast<double>(scope.effective_desired_eye.y),
        static_cast<double>(scope.effective_desired_eye.z),
        static_cast<double>(scope.effective_desired_look.x),
        static_cast<double>(scope.effective_desired_look.y),
        static_cast<double>(scope.effective_desired_look.z),
        static_cast<double>(scope.effective_desired_up.x),
        static_cast<double>(scope.effective_desired_up.y),
        static_cast<double>(scope.effective_desired_up.z),
        static_cast<double>(pose_distance),
        static_cast<double>(pose_yaw),
        static_cast<double>(pose_pitch));
}

// Chase-aim camera clip fix. The pursuit (flee) branch's aim parameters hold
// the camera ~1.83 m from the car at body height, which puts the eye inside
// the body while aiming during a police chase (reporter capture 2026-10-02:
// flee-aim stock_distance steady at 1.81-1.86 with eye z 20.61 vs car body
// reference z ~20.5, while the drive-branch aim camera holds ~3.24 m and
// stays outside). SPatch writes no pose in that state today, so the clip is
// the game's own parameter block; this floor compensates it. The push moves
// the eye out along the eye-look ray and preserves look and up; the original
// desired-pose function still applies its collision pass to the result, so a
// push toward world geometry is re-resolved by the game. Cars only.
constexpr float kChaseAimCameraMinDistanceM = 3.2f;
std::atomic<unsigned long> g_chase_aim_floor_lines{0};

bool TryApplyChaseAimCameraFloor(
    const ChaseCameraUpdateProbeScope& scope,
    const CameraVector3& eye,
    const CameraVector3& look,
    CameraVector3& adjusted_eye) noexcept {
    if (!g_config.fix_chase_aim_camera) {
        return false;
    }
    // Same scoping rules as the sibling stages: only the top-level camera
    // update composes, and explicit eye/look locks stay native because this
    // detour cannot safely post-compose those overrides.
    if (scope.depth != 1) {
        return false;
    }
    if (scope.component == nullptr || !scope.decision.policy_evaluated ||
        scope.decision.subject == 0 ||
        scope.decision.vehicle_class !=
            vehicle_camera::VehicleClass::CarPhysicsMover) {
        return false;
    }
    const std::uintptr_t component_address =
        reinterpret_cast<std::uintptr_t>(scope.component);
    unsigned char aim_or_focus = 0;
    unsigned char flee_active = 0;
    unsigned char eye_locked = 0;
    unsigned char look_locked = 0;
    if (!SafeRead(component_address + vehicle_camera::kAimOrFocusFlagOffset,
                  aim_or_focus) ||
        aim_or_focus == 0 ||
        !SafeRead(component_address + vehicle_camera::kEyeLockFlagOffset,
                  eye_locked) ||
        eye_locked != 0 ||
        !SafeRead(component_address + vehicle_camera::kLookLockFlagOffset,
                  look_locked) ||
        look_locked != 0 ||
        !SafeRead(scope.decision.subject +
                      vehicle_camera::kSelectorFleeStateOffset,
                  flee_active) ||
        flee_active == 0) {
        return false;
    }
    const float dx = eye.x - look.x;
    const float dy = eye.y - look.y;
    const float dz = eye.z - look.z;
    const float distance =
        std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(distance) || distance <= 0.0f ||
        distance >= kChaseAimCameraMinDistanceM) {
        return false;
    }
    const float scale = kChaseAimCameraMinDistanceM / distance;
    const CameraVector3 pushed{look.x + dx * scale, look.y + dy * scale,
                               look.z + dz * scale};
    if (!std::isfinite(pushed.x) || !std::isfinite(pushed.y) ||
        !std::isfinite(pushed.z)) {
        return false;
    }
    // Per-fire verdicts stay unconditional and rate-capped so a play session
    // shows whether the fix engaged (see feature-verdicts discipline).
    const unsigned long line =
        g_chase_aim_floor_lines.fetch_add(1, std::memory_order_relaxed);
    if (line < 8 || (line % 512) == 0) {
        log::InfoF("chase_aim_camera_floor engaged distance=%.3f "
                   "component=0x%p",
                   distance,
                   scope.component);
    }
    adjusted_eye = pushed;
    return true;
}

// Publishes the chase camera's look target as the beam anchor.
//
// `desired_look` is the point the camera is looking at, and that is the thing
// it is following: the car while driving, the character on foot. `look - eye`
// flattened to the ground is its heading, which is the car's forward while
// driving. Both come straight from the game's own per-frame pose, so unlike a
// scanned matrix they cannot go stale.
//
// Driving is read from the camera component: the selected parameter block at
// +0xB50 equals the Drive slot at +0x388 exactly when the player is driving.
void UpdateBeamAnchorFromCamera(void* component,
                                const CameraVector3* desired_eye,
                                const CameraVector3* desired_look) noexcept {
    CameraVector3 eye{};
    CameraVector3 look{};
    if (!SafeRead(reinterpret_cast<std::uintptr_t>(desired_eye), eye) ||
        !SafeRead(reinterpret_cast<std::uintptr_t>(desired_look), look) ||
        !vehicle_beam_light::IsPlausibleWorldPosition(look.x, look.y, look.z) ||
        !vehicle_beam_light::IsPlausibleWorldPosition(eye.x, eye.y, eye.z)) {
        g_beam_anchor_valid.store(false, std::memory_order_relaxed);
        return;
    }
    g_beam_anchor_pos[0] = look.x;
    g_beam_anchor_pos[1] = look.y;
    g_beam_anchor_pos[2] = look.z;
    g_beam_anchor_dir[0] = look.x - eye.x;
    g_beam_anchor_dir[1] = look.y - eye.y;
    g_beam_anchor_dir[2] = 0.0f;  // ground plane: never aim at the sky
    g_beam_anchor_valid.store(true, std::memory_order_relaxed);

    std::uintptr_t selected = 0;
    std::uintptr_t drive_slot = 0;
    const bool slots_readable =
        component != nullptr &&
        SafeRead(reinterpret_cast<std::uintptr_t>(component) +
                     vehicle_camera::kTargetParametersOffset,
                 selected) &&
        SafeRead(reinterpret_cast<std::uintptr_t>(component) +
                     vehicle_camera::kDriveParametersSlotOffset,
                 drive_slot);
    if (!slots_readable || selected == 0 || drive_slot == 0) {
        g_beam_drive_state.store(0, std::memory_order_relaxed);
    } else {
        g_beam_drive_state.store(selected == drive_slot ? 1 : 2,
                                 std::memory_order_relaxed);
    }
}

void DetourGameCameraSetDesiredEyeLookUp(
    void* component,
    float delta_seconds,
    const CameraVector3* desired_eye,
    const CameraVector3* desired_look,
    const CameraVector3* desired_up,
    bool collide,
    CameraVector3* new_eye,
    CameraVector3* new_look) {
    CameraVector3 adjusted_eye{};
    CameraVector3 adjusted_look{};
    CameraVector3 adjusted_up{};
    CameraVector3 eye{};
    CameraVector3 look{};
    CameraVector3 up{};
    // The beam anchors on this pose: the look target is the vehicle while
    // driving, and look - eye is its heading. Cheap, read-only, and it must
    // happen before any early return.
    if (g_config.headlight_beam_light) {
        UpdateBeamAnchorFromCamera(component, desired_eye, desired_look);
    }
    bool pose_adjusted = false;
    bool gta_iv_pose_adjusted = false;
    bool gta_iv_pose_failed_to_arm = false;
    bool speed_shake_pose_adjusted = false;
    ChaseCameraUpdateProbeScope* scope =
        FindActiveChaseCameraUpdateProbeScope(component);
    if (scope != nullptr) {
        ++scope->calls.desired_pose_calls;
        scope->desired_pose_readable =
            SafeRead(reinterpret_cast<std::uintptr_t>(desired_eye), eye) &&
            SafeRead(reinterpret_cast<std::uintptr_t>(desired_look), look) &&
            SafeRead(reinterpret_cast<std::uintptr_t>(desired_up), up);
        if (scope->desired_pose_readable) {
            scope->desired_eye = eye;
            scope->desired_look = look;
            scope->desired_up = up;
            scope->collide = collide;
            adjusted_eye = eye;
            adjusted_look = look;
            adjusted_up = up;
            CameraVector3 gta_iv_eye{};
            CameraVector3 gta_iv_look{};
            CameraVector3 gta_iv_up{};
            gta_iv_pose_adjusted = TryBuildGtaIvDesiredPose(
                *scope,
                delta_seconds,
                eye,
                look,
                up,
                gta_iv_eye,
                gta_iv_look,
                gta_iv_up);
            if (gta_iv_pose_adjusted) {
                adjusted_eye = gta_iv_eye;
                adjusted_look = gta_iv_look;
                adjusted_up = gta_iv_up;
            }
            CameraVector3 speed_shake_eye{};
            CameraVector3 speed_shake_look{};
            CameraVector3 speed_shake_up{};
            speed_shake_pose_adjusted = TryBuildGtaVSpeedShakePose(
                *scope,
                adjusted_eye,
                adjusted_look,
                adjusted_up,
                speed_shake_eye,
                speed_shake_look,
                speed_shake_up);
            if (speed_shake_pose_adjusted) {
                adjusted_eye = speed_shake_eye;
                adjusted_look = speed_shake_look;
                adjusted_up = speed_shake_up;
            }
            // Chase-aim camera clip fix. Composed last so it acts on the
            // pose every other stage produced; it only ever pushes the eye
            // further from the look point during a pursuit aim.
            CameraVector3 chase_aim_eye{};
            const bool chase_aim_floor_applied = TryApplyChaseAimCameraFloor(
                *scope,
                adjusted_eye,
                adjusted_look,
                chase_aim_eye);
            if (chase_aim_floor_applied) {
                adjusted_eye = chase_aim_eye;
            }
            pose_adjusted =
                gta_iv_pose_adjusted || speed_shake_pose_adjusted ||
                chase_aim_floor_applied;
        }
    }

    struct AlternateLookRestore {
        std::uintptr_t address = 0;
        unsigned char value = 0;
        bool armed = false;

        ~AlternateLookRestore() {
            if (armed) {
                (void)SafeWrite(address, value);
            }
        }
    } alternate_look_restore;

    unsigned char alternate_look_flag = 0;
    if (gta_iv_pose_adjusted) {
        alternate_look_restore.address =
            reinterpret_cast<std::uintptr_t>(component) +
            vehicle_camera::kAlternateLookFlagOffset;
        if (!SafeRead(alternate_look_restore.address, alternate_look_flag)) {
            gta_iv_pose_adjusted = false;
            gta_iv_pose_failed_to_arm = true;
            ResetGtaIvVehicleCameraDynamicState();
        } else if (alternate_look_flag != 0) {
            const unsigned char disabled = 0;
            if (!SafeWrite(alternate_look_restore.address, disabled)) {
                gta_iv_pose_adjusted = false;
                gta_iv_pose_failed_to_arm = true;
                ResetGtaIvVehicleCameraDynamicState();
            } else {
                alternate_look_restore.value = alternate_look_flag;
                alternate_look_restore.armed = true;
            }
        }
    }
    if (gta_iv_pose_failed_to_arm && scope != nullptr &&
        scope->desired_pose_readable) {
        // If the GTA-IV-only alternate-look transaction could not be armed,
        // discard that larger pose change but retain the independently safe
        // speed shake by recomposing it from the stock pose.  The speed state
        // was already advanced once; this call only resamples it.
        CameraVector3 speed_shake_eye{};
        CameraVector3 speed_shake_look{};
        CameraVector3 speed_shake_up{};
        speed_shake_pose_adjusted = TryBuildGtaVSpeedShakePose(
            *scope,
            eye,
            look,
            up,
            speed_shake_eye,
            speed_shake_look,
            speed_shake_up);
        if (speed_shake_pose_adjusted) {
            adjusted_eye = speed_shake_eye;
            adjusted_look = speed_shake_look;
            adjusted_up = speed_shake_up;
        }
        // The chase-aim floor is recomputable from whatever survived the
        // discard (shake output if the shake re-armed, stock pose otherwise);
        // without this the floor visibly vanishes on failed-to-arm frames.
        {
            CameraVector3 chase_aim_eye{};
            const bool chase_aim_refloor = TryApplyChaseAimCameraFloor(
                *scope,
                speed_shake_pose_adjusted ? adjusted_eye : eye,
                speed_shake_pose_adjusted ? adjusted_look : look,
                chase_aim_eye);
            if (chase_aim_refloor) {
                adjusted_eye = chase_aim_eye;
            }
            pose_adjusted = speed_shake_pose_adjusted || chase_aim_refloor;
        }
    }
    if (scope != nullptr && scope->desired_pose_readable) {
        scope->effective_desired_eye = pose_adjusted ? adjusted_eye : eye;
        scope->effective_desired_look = pose_adjusted ? adjusted_look : look;
        scope->effective_desired_up = pose_adjusted ? adjusted_up : up;
        scope->dynamics_mutation_applied |= pose_adjusted;
    }
    vehicle_camera::InvokeDesiredEyeLookUpOnce(
        g_game_camera_set_desired_eye_look_up_original,
        component,
        delta_seconds,
        pose_adjusted ? &adjusted_eye : desired_eye,
        pose_adjusted ? &adjusted_look : desired_look,
        pose_adjusted ? &adjusted_up : desired_up,
        collide,
        new_eye,
        new_look);
}

void DetourChaseCameraUpdate(void* component, float delta_seconds) {
    if (g_chase_camera_update_original == nullptr) {
        return;
    }
    const unsigned int index = g_chase_camera_update_probe_depth;
    const bool transaction_ready =
        g_behavior_transaction_ready.load(std::memory_order_acquire);
    const bool feature_enabled = IsVehicleCameraCoreFeatureRequested();
    std::uintptr_t active_subject = 0;
    const bool has_active_subject =
        component != nullptr &&
        SafeRead(reinterpret_cast<std::uintptr_t>(component) +
                     vehicle_camera::kSubjectOffset,
                 active_subject) &&
        active_subject != 0;
    const bool should_attempt_outer_capture =
        vehicle_camera::ShouldAttemptOuterDynamicProbeCapture(
            index,
            transaction_ready,
            g_config.enable_logging,
            feature_enabled);
    const bool has_dynamic_probe_budget =
        g_gtaiv_camera_dynamic_probe_log_budget.load(
            std::memory_order_relaxed) != 0;
    const bool capture_outer_sample =
        should_attempt_outer_capture && has_active_subject &&
        has_dynamic_probe_budget &&
        ClaimDynamicCameraProbeSample() &&
        ClaimDynamicCameraProbeLogBudget();
    if (index == 0 && transaction_ready && feature_enabled &&
        component != nullptr && !has_active_subject &&
        g_gtaiv_vehicle_camera_dynamic_state.component == component) {
        ResetGtaIvVehicleCameraDynamicStateForComponent(component);
    }
    if (index == 0 && transaction_ready && feature_enabled &&
        component != nullptr && !has_active_subject &&
        g_gtav_speed_shake_state.component == component) {
        ResetGtaVSpeedShakeStateForComponent(component);
    }
    if (!vehicle_camera::ShouldEnterDynamicCameraScope(
            index,
            transaction_ready,
            feature_enabled,
            has_active_subject)) {
        if (index == 0 && g_driving_swear_trigger_state.component == component) {
            g_driving_swear_trigger_state = {};
        }
        vehicle_camera::InvokeChaseCameraUpdateOnce(
            g_chase_camera_update_original, component, delta_seconds);
        return;
    }
    if (index >= kChaseCameraUpdateProbeMaxDepth) {
        vehicle_camera::InvokeChaseCameraUpdateOnce(
            g_chase_camera_update_original, component, delta_seconds);
        return;
    }

    ChaseCameraUpdateProbeScope& scope =
        g_chase_camera_update_probe_stack[index];
    scope = {};
    scope.component = component;
    scope.delta_seconds = delta_seconds;
    scope.depth = index + 1;
    scope.capture_sample = capture_outer_sample;
    if (scope.capture_sample) {
        scope.thread_id = GetCurrentThreadId();
    }
    if (g_config.gta_iv_car_camera || g_config.gta_iv_bike_camera) {
        scope.stock_mouse_eye_offset_readable = SafeRead(
            reinterpret_cast<std::uintptr_t>(component) +
                vehicle_camera::kLookupMouseOffset,
            scope.stock_mouse_eye_offset);
    }
    if (scope.capture_sample) {
        (void)ReadChaseCameraDynamicSnapshot(component, scope.before);
    }
    ++g_chase_camera_update_probe_depth;
    vehicle_camera::InvokeChaseCameraUpdateOnce(
        g_chase_camera_update_original, component, delta_seconds);
    --g_chase_camera_update_probe_depth;

    if (index != 0) {
        ChaseCameraUpdateProbeScope& parent =
            g_chase_camera_update_probe_stack[index - 1];
        vehicle_camera::MergeNestedDynamicProbeCounts(parent.calls,
                                                       scope.calls);
        return;
    }
    if (scope.calls.setter_calls == 0 &&
        g_driving_swear_trigger_state.component == component) {
        // Only a gap in the owning camera invalidates its speed history.
        // Other inactive camera instances can update between driving frames.
        g_driving_swear_trigger_state = {};
    }
    if (scope.capture_sample) {
        ChaseCameraDynamicSnapshot after{};
        (void)ReadChaseCameraDynamicSnapshot(component, after);
        ObserveChaseCameraUpdate(scope, after);
    }
}

std::string ReadTaskName(std::uintptr_t task_header) {
    if (task_header == 0) {
        return {};
    }

    std::uintptr_t metadata = 0;
    if (!SafeRead(task_header + 0x30, metadata) || metadata == 0) {
        return {};
    }

    std::uintptr_t name_ptr = 0;
    if (!SafeRead(metadata + 0x38, name_ptr) || name_ptr == 0) {
        return {};
    }

    return ReadCString(name_ptr);
}

std::string ReadTaskNameNoThrow(std::uintptr_t task_header) noexcept {
    try {
        return ReadTaskName(task_header);
    } catch (...) {
        // Verbose task telemetry is optional; never let a diagnostic allocation
        // failure escape a worker/render detour.
        return {};
    }
}

#if !defined(SPATCH_FINAL_RELEASE)
void MaybeWriteSummary() {
    if (g_config.summary_interval_ms == 0) {
        return;
    }
    const unsigned long long now = GetTickCount64();
    unsigned long long last = g_last_summary_tick.load();
    if (now - last < g_config.summary_interval_ms) {
        return;
    }

    if (!g_last_summary_tick.compare_exchange_strong(last, now)) {
        return;
    }

    const smaa::Stats smaa_stats = smaa::GetStats();
    const SummaryRuntimeFields fields{
        .snapshot =
            {
                .task_ready = g_task_ready_count.load(),
                .task_dispatch = g_task_dispatch_count.load(),
                .wait_helper = g_wait_helper_count.load(),
                .wait_task = g_wait_helper_nonzero_task_count.load(),
                .wait_gt16 = g_wait_over_16ms_count.load(),
                .wait_gt100 = g_wait_over_100ms_count.load(),
                .wait_gt1000 = g_wait_over_1000ms_count.load(),
                .wait_gt5000 = g_wait_over_5000ms_count.load(),
                .scaleform_time = g_scaleform_time_count.load(),
                .provider_non_null = g_scaleform_provider_non_null_count.load(),
                .scaleform_init = g_scaleform_init_count.load(),
                .nis_time = g_nis_set_play_time_count.load(),
                .nis_sync = g_nis_sync_scene_time_count.load(),
                .nis_dt0 = g_nis_delta_zero_count.load(),
                .nis_dt30 = g_nis_delta_30hz_count.load(),
                .nis_dt60 = g_nis_delta_60hz_count.load(),
                .nis_dt_other = g_nis_delta_other_count.load(),
                .nis_scene_fix = g_nis_scene_time_fix_count.load(),
                .nis_play = g_nis_play_count.load(),
                .nis_play_adv = g_nis_play_advanced_count.load(),
                .nis_play_repeat = g_nis_play_repeat_count.load(),
                .nis_play_multi = g_nis_play_multi_tick_count.load(),
                .nis_boot = g_nis_bootstrap_count.load(),
                .nis_boot_s1 = g_nis_bootstrap_state1_count.load(),
                .nis_boot_s2 = g_nis_bootstrap_state2_count.load(),
                .nis_boot_fail = g_nis_bootstrap_fail_count.load(),
                .nis_owner = g_nis_owner_count.load(),
                .nis_owner_dt0 = g_nis_owner_dt_zero_count.load(),
                .nis_owner_dt30 = g_nis_owner_dt_30hz_count.load(),
                .nis_owner_dt60 = g_nis_owner_dt_60hz_count.load(),
                .nis_owner_dt_other = g_nis_owner_dt_other_count.load(),
                .nis_owner_adv = g_nis_owner_advanced_count.load(),
                .nis_owner_repeat = g_nis_owner_repeat_count.load(),
                .nis_owner_multi = g_nis_owner_multi_tick_count.load(),
                .nis_actor_setup = g_nis_actor_setup_count.load(),
                .nis_actor_restore = g_nis_actor_restore_count.load(),
                .nis_actor_restore_untracked = g_nis_actor_restore_untracked_count.load(),
                .nis_actor_setup_duplicate = g_nis_actor_setup_duplicate_count.load(),
                .nis_actor_restore_duplicate = g_nis_actor_restore_duplicate_count.load(),
                .nis_actor_restore_never_seen = g_nis_actor_restore_never_seen_count.load(),
                .nis_actor_restore_suppressed = g_nis_actor_restore_suppressed_count.load(),
                .twitch_tick = g_twitch_state_tick_count.load(),
                .twitch_login_callback = g_twitch_login_callback_count.load(),
                .twitch_login_failure = g_twitch_login_failure_count.load(),
                .frameflow = g_frame_flow_count.load(),
                .frameflow_dt0 = g_frame_flow_dt_zero_count.load(),
                .frameflow_dt60 = g_frame_flow_dt_60hz_count.load(),
                .frameflow_dt_other = g_frame_flow_dt_other_count.load(),
                .frameflow_from_cutscene = g_frame_flow_from_cutscene_owner_count.load(),
                .cutscene_flow = g_cutscene_flow_owner_count.load(),
                .cutscene_flow_dt0 = g_cutscene_flow_owner_dt_zero_count.load(),
                .cutscene_flow_fwd0 = g_cutscene_flow_owner_forwarded_zero_count.load(),
                .cutscene_flow_fwd60 = g_cutscene_flow_owner_forwarded_60hz_count.load(),
                .cutscene_flow_fwd_other = g_cutscene_flow_owner_forwarded_other_count.load(),
                .cutscene_flow_fix = g_cutscene_zero_dt_override_count.load(),
                .cutscene_flow_fix_paused = g_cutscene_zero_dt_pause_override_count.load(),
                .fog_slicing = g_fog_slicing_mode_count.load(),
                .fog_clamps = g_fog_slicing_clamp_count.load(),
                .aa_owner = g_aa_owner_count.load(),
                .aa_skip = g_aa_skip_count.load(),
                .aa_main = g_aa_main_count.load(),
                .aa_hairblur0 = g_aa_hair_blur_zero_count.load(),
                .aa_state = g_last_aa_state_gate.load(),
                .aa_hair_gate = g_last_aa_hair_blur_gate.load(),
                .aa_variant = g_last_aa_variant_mode.load(),
                .aa_variant_apply = g_aa_variant_apply_count.load(),
                .aa_aux_mode = g_last_aa_aux_mode.load(),
                .aa_aux_apply = g_aa_aux_apply_count.load(),
                .aa_shader = g_last_aa_shader_uid.load(),
                .aa_raster = g_last_aa_raster_uid.load(),
                .aa_aux = g_last_aa_aux_uid.load(),
                .aa_material = g_last_aa_material.load(),
                .aa_target = g_last_aa_target.load(),
                .aa_src_a = g_last_aa_source_a.load(),
                .aa_src_b = g_last_aa_source_b.load(),
                .aa_fx = g_aa_fx_handoff_count.load(),
                .aa_fx_arg1 = g_last_aa_fx_arg1.load(),
                .aa_fx_arg2 = g_last_aa_fx_arg2.load(),
                .aa_fx_arg3 = g_last_aa_fx_arg3.load(),
                .rumble_override = g_rumble_override_apply_count.load(),
                .rumble_value = g_last_rumble_override_value.load(),
                .post_submit = g_post_material_submit_count.load(),
                .post_comp_lights = g_post_composite_lights_submit_count.load(),
                .post_comp_final = g_post_composite_final_submit_count.load(),
                .post_bloom = g_post_bloom_threshold_submit_count.load(),
                .post_lightshaft = g_post_lightshaft_submit_count.load(),
                .post_shadow_collector = g_post_shadow_collector_submit_count.load(),
                .post_final_chg = g_post_composite_final_snapshot_change_count.load(),
                .post_final_flags = g_last_post_final_flags.load(),
                .post_final_cmd = g_last_post_final_cmd.load(),
                .post_final_params = g_last_post_final_params.load(),
                .post_final_p0 = g_last_post_final_param0.load(),
                .post_final_p1 = g_last_post_final_param1.load(),
                .post_final_p2 = g_last_post_final_param2.load(),
                .post_final_p3 = g_last_post_final_param3.load(),
                .char_water = g_character_water_collision_count.load(),
                .char_fx_update = g_character_effects_update_count.load(),
                .char_fx_wet_updates = g_character_effects_wet_surface_count.load(),
                .char_fx_surface = g_last_character_active_surface_uid.load(),
                .char_fx_wet_surface = g_last_character_active_wet_surface_uid.load(),
                .char_fx_gate = g_last_character_wet_gate_counter.load(),
                .char_fx_onfire = g_last_character_is_on_fire.load(),
                .char_fx_smolder = g_last_character_is_smoldering.load(),
                .char_fx_attached = g_last_character_is_attached_to_player.load(),
                .char_health_apply = g_character_health_apply_damage_count.load(),
                .char_health_proj = g_character_health_apply_projectile_count.load(),
                .char_health_melee = g_character_health_apply_melee_count.load(),
                .char_health_anim_found = g_character_health_anim_found_count.load(),
                .char_health_hitreact_found = g_character_health_hitreact_found_count.load(),
                .char_health_damage = g_last_character_health_damage.load(),
                .char_health_projectile = g_last_character_health_projectile.load(),
                .char_health_component = g_last_character_health_component.load(),
                .char_health_anim = g_last_character_health_anim_component.load(),
                .char_health_hitreact = g_last_character_health_hitreact_component.load(),
                .char_health_attacker = g_last_character_health_attacker.load(),
                .char_health_hit = g_last_character_health_hit_record.load(),
                .char_wet_force = g_character_wet_force_count.load(),
                .char_wet_force_verify = g_character_wet_force_verify_count.load(),
                .char_charred_anim = g_character_charred_anim_count.load(),
                .char_charred_rig = g_character_charred_rig_count.load(),
                .char_dispatch_owner = g_character_effect_dispatch_owner_count.load(),
                .char_dispatch_consume = g_character_effect_dispatch_consume_count.load(),
                .char_dispatch_owner_ptr = g_last_character_effect_dispatch_owner.load(),
                .char_dispatch_component = g_last_character_effect_dispatch_component.load(),
                .char_queue_build = g_character_effect_queue_builder_count.load(),
                .char_queue_build_tracked = g_character_effect_queue_builder_tracked_count.load(),
                .char_queue_build_owner = g_last_character_effect_queue_builder_owner.load(),
                .char_queue_build_component = g_last_character_effect_queue_builder_component.load(),
                .char_queue_build_mode = g_last_character_effect_queue_builder_mode.load(),
                .char_paint_owner = g_character_paint_owner_count.load(),
                .char_paint_owner_ptr = g_last_character_paint_owner_ptr.load(),
                .char_paint_owner_component = g_last_character_paint_owner_component.load(),
                .char_paint_consumer = g_character_paint_consumer_count.load(),
                .char_paint_anim = g_character_paint_anim_count.load(),
                .char_paint_rig = g_character_paint_rig_count.load(),
                .char_paint_enable = g_last_character_paint_enable.load(),
                .char_damage_create = g_character_damage_rig_create_count.load(),
                .char_damage_reset = g_character_damage_rig_reset_count.load(),
                .post_last = g_last_post_material.load(),
                .unique_callbacks = g_unique_callback_count.load(),
                .scenery_prepare = g_scenery_prepare_count.load(),
                .scenery_setup = g_scenery_setup_count.load(),
                .render_scenery = g_render_scenery_builder_count.load(),
                .rasterize_bucket = g_rasterize_bucket_builder_count.load(),
                .scenery_prepare_ready = g_scenery_prepare_ready_total.load(),
                .scenery_setup_ready = g_scenery_setup_ready_total.load(),
                .render_scenery_ready = g_render_scenery_ready_total.load(),
                .rasterize_bucket_ready = g_rasterize_bucket_ready_total.load(),
                .scenery_setup_qdelta = g_scenery_setup_queue_delta_total.load(),
                .render_scenery_qdelta = g_render_scenery_queue_delta_total.load(),
                .rasterize_bucket_qdelta = g_rasterize_bucket_queue_delta_total.load(),
                .scenery_count0 = g_last_scenery_counter0.load(),
                .scenery_count1 = g_last_scenery_counter1.load(),
                .scenery_count2 = g_last_scenery_counter2.load(),
                .scenery_count3 = g_last_scenery_counter3.load(),
            },
        .nis_last_bits = g_last_nis_scene_time_bits.load(),
        .nis_last_delta_bits = g_last_nis_scene_delta_bits.load(),
        .nis_owner_last_dt_bits = g_last_nis_owner_dt_bits.load(),
        .frameflow_last_dt_bits = g_last_frame_flow_dt_bits.load(),
        .cutscene_flow_in_bits = g_last_cutscene_flow_input_dt_bits.load(),
        .cutscene_flow_fwd_bits = g_last_cutscene_flow_forwarded_dt_bits.load(),
        .char_water_speed_bits = g_last_character_water_speed_bits.load(),
        .char_fx_tod_weather_bits = g_last_character_timeofday_weather_bits.load(),
        .char_fx_tod_override_bits = g_last_character_timeofday_override_bits.load(),
        .char_fx_fire_time_bits = g_last_character_fire_extinguish_bits.load(),
        .char_fx_smolder_time_bits = g_last_character_smolder_extinguish_bits.load(),
        .char_fx_queued_damage_bits = g_last_character_queued_health_damage_bits.load(),
        .char_charred_amount_bits = g_last_character_charred_amount_bits.load(),
        .char_paint_r_bits = g_last_character_paint_r_bits.load(),
        .char_paint_g_bits = g_last_character_paint_g_bits.load(),
        .char_paint_b_bits = g_last_character_paint_b_bits.load(),
        .smaa_stats = smaa_stats,
        .provider_ptr = ReadProviderValue(),
    };

    try {
        const SummarySnapshot snapshot = BuildSummarySnapshot(fields);
        log::Info(FormatSummaryMessage(snapshot));
    } catch (...) {
        // Summary generation is optional telemetry and allocates a sizeable
        // formatted string.  A low-memory event must not escape a render/NIS
        // detour and terminate the game; the next interval can retry.
    }
}
#endif

void LogTaskState(const char* prefix,
                  void* owner_or_manager,
                  void* resolved_manager_ptr,
                  void* task_header,
                  unsigned int worker_index) {
    const auto manager = reinterpret_cast<std::uintptr_t>(resolved_manager_ptr);
    const auto owner = reinterpret_cast<std::uintptr_t>(owner_or_manager);
    const auto task = reinterpret_cast<std::uintptr_t>(task_header);

    int worker_count = 0;
    int active_workers_guess = 0;
    int queue_depth_counter = 0;
    int queued_total = 0;
    int completed_total = 0;
    unsigned short task_state = 0;
    unsigned short dependency_count = 0;
    std::uintptr_t callback = 0;
    std::string task_name;

    if (manager != 0) {
        SafeRead(manager + 0x8, worker_count);
        SafeRead(manager + 0x14, active_workers_guess);
        SafeRead(manager + 0x58, queue_depth_counter);
        SafeRead(manager + 0x218, queued_total);
        SafeRead(manager + 0x21C, completed_total);
    }

    if (task != 0) {
        SafeRead(task + 0x20, task_state);
        SafeRead(task + 0x22, dependency_count);
        callback = ReadTaskCallback(task);
        task_name = ReadTaskNameNoThrow(task);
    }

    log::InfoF(
        "%s owner=0x%p manager=0x%p task=0x%p worker_index=%u worker_count=%d "
        "active_workers_guess=%d queue_lock_depth=%d queued_total=%d completed_total=%d "
        "task_state=%hu deps=%hu callback=0x%p callback_rva=0x%llX task_name=%s provider=0x%p",
        prefix,
        reinterpret_cast<void*>(owner),
        reinterpret_cast<void*>(manager),
        reinterpret_cast<void*>(task),
        worker_index,
        worker_count,
        active_workers_guess,
        queue_depth_counter,
        queued_total,
        completed_total,
        task_state,
        dependency_count,
        reinterpret_cast<void*>(callback),
        static_cast<unsigned long long>(ToRva(callback)),
        task_name.empty() ? "<unnamed>" : task_name.c_str(),
        reinterpret_cast<void*>(ReadProviderValue()));
}

void DetourTaskReady(void* task_manager_owner, void* task_header) {
    g_task_ready_count.fetch_add(1);
    if (g_current_builder_scope != BuilderScope::None) {
        AddReadyCounterForScope(g_current_builder_scope);
    }

    std::uintptr_t resolved_manager = 0;
    SafeRead(reinterpret_cast<std::uintptr_t>(task_manager_owner), resolved_manager);

    if (ShouldLogVerbose(g_task_ready_verbose)) {
        LogTaskState("task_ready",
                     task_manager_owner,
                     reinterpret_cast<void*>(resolved_manager),
                     task_header,
                     0);
    }

    MaybeWriteSummary();
    g_task_ready_original(task_manager_owner, task_header);
}

void DetourTaskDispatch(void* task_manager, unsigned int worker_index) {
    g_task_dispatch_count.fetch_add(1);

#if !defined(SPATCH_FINAL_RELEASE)
    const std::uintptr_t queued_task = PeekQueuedTaskHeader(reinterpret_cast<std::uintptr_t>(task_manager));
    const std::uintptr_t callback = ReadTaskCallback(queued_task);

    if (RecordSeenCallback(callback)) {
        const std::string task_name = ReadTaskNameNoThrow(queued_task);
        unsigned short task_state = 0;
        unsigned short dependency_count = 0;
        SafeRead(queued_task + 0x20, task_state);
        SafeRead(queued_task + 0x22, dependency_count);

        log::InfoF(
            "task_callback_first_seen callback=0x%p callback_rva=0x%llX task=0x%p worker_index=%u "
            "task_state=%hu deps=%hu task_name=%s",
            reinterpret_cast<void*>(callback),
            static_cast<unsigned long long>(ToRva(callback)),
            reinterpret_cast<void*>(queued_task),
            worker_index,
            task_state,
            dependency_count,
            task_name.empty() ? "<unnamed>" : task_name.c_str());
    }

    if (ShouldLogVerbose(g_task_dispatch_verbose)) {
        LogTaskState("task_dispatch",
                     task_manager,
                     task_manager,
                     reinterpret_cast<void*>(queued_task),
                     worker_index);
    }
#else
    (void)worker_index;
#endif

    MaybeWriteSummary();
    g_task_dispatch_original(task_manager, worker_index);
}

void DetourWaitHelper(void* task_manager_owner, void* task_header) {
    g_wait_helper_count.fetch_add(1);
    if (task_header != nullptr) {
        g_wait_helper_nonzero_task_count.fetch_add(1);
    }

    std::uintptr_t resolved_manager = 0;
    SafeRead(reinterpret_cast<std::uintptr_t>(task_manager_owner), resolved_manager);

    const unsigned long long before = GetTickCount64();
    g_wait_helper_original(task_manager_owner, task_header);
    const unsigned long long elapsed = GetTickCount64() - before;

    if (elapsed >= kWaitLogThresholdMs) {
        g_wait_over_16ms_count.fetch_add(1);
    }
    if (elapsed >= kWaitLongThresholdMs) {
        g_wait_over_100ms_count.fetch_add(1);
    }
    if (elapsed >= kWaitVeryLongThresholdMs) {
        g_wait_over_1000ms_count.fetch_add(1);
    }
    if (elapsed >= 5000) {
        g_wait_over_5000ms_count.fetch_add(1);
    }

    if ((elapsed >= kWaitLogThresholdMs && ShouldLogVerbose(g_wait_helper_verbose)) ||
        elapsed >= kWaitVeryLongThresholdMs) {
        const std::uintptr_t callback = ReadTaskCallback(reinterpret_cast<std::uintptr_t>(task_header));
        log::InfoF(
            "wait_helper owner=0x%p manager=0x%p task=0x%p elapsed_ms=%llu task_state=%hu "
            "deps=%hu callback=0x%p callback_rva=0x%llX task_name=%s",
            task_manager_owner,
            reinterpret_cast<void*>(resolved_manager),
            task_header,
            elapsed,
            [&]() {
                unsigned short value = 0;
                SafeRead(reinterpret_cast<std::uintptr_t>(task_header) + 0x20, value);
                return value;
            }(),
            [&]() {
                unsigned short value = 0;
                SafeRead(reinterpret_cast<std::uintptr_t>(task_header) + 0x22, value);
                return value;
            }(),
            reinterpret_cast<void*>(callback),
            static_cast<unsigned long long>(ToRva(callback)),
            [&]() -> const char* {
                static thread_local std::string name;
                try {
                    name = ReadTaskName(reinterpret_cast<std::uintptr_t>(task_header));
                } catch (...) {
                    name.clear();
                }
                return name.empty() ? "<all_tasks>" : name.c_str();
            }());
    }

    MaybeWriteSummary();
}

void DetourSamplerBuilder(void* sampler_state, void* descriptor_source) {
    if (g_behavior_transaction_ready.load(std::memory_order_acquire) &&
        texture_filtering::ShouldInstallWriter(
            g_config.anisotropic_filtering)) {
        const std::uintptr_t value_address =
            ResolveAddress(texture_filtering::kAnisotropyValueRva);
        int previous_anisotropy = 0;
        int applied_anisotropy = 0;
        const bool read_previous =
            SafeRead(value_address, previous_anisotropy);
        const bool wrote_requested =
            read_previous &&
            (previous_anisotropy == g_config.anisotropic_filtering ||
             SafeWrite(value_address, g_config.anisotropic_filtering));
        const bool verified =
            wrote_requested && SafeRead(value_address, applied_anisotropy) &&
            applied_anisotropy == g_config.anisotropic_filtering;
        bool expected = false;
        if (verified &&
            g_sampler_builder_first_verified_invocation_logged
                .compare_exchange_strong(expected, true,
                                         std::memory_order_relaxed)) {
            log::InfoF(
                "texture_filtering sampler_builder_first_verified_invocation=1 "
                "previous_anisotropy=%d anisotropy=%d",
                previous_anisotropy, applied_anisotropy);
        } else if (!verified &&
                   g_sampler_builder_failure_logged.compare_exchange_strong(
                       expected, true, std::memory_order_relaxed)) {
            log::WarnF(
                "texture_filtering sampler_builder_apply_failed=1 "
                "read_previous=%d previous_anisotropy=%d requested=%d",
                read_previous ? 1 : 0, previous_anisotropy,
                g_config.anisotropic_filtering);
        }
    }

    g_sampler_builder_original(sampler_state, descriptor_source);
}

void DetourAnisotropyWriter(int stock_exponent) {
    const bool behavior_active =
        g_behavior_transaction_ready.load(std::memory_order_acquire);
    const int effective_exponent = behavior_active
        ? texture_filtering::ResolveWriterExponent(
              stock_exponent, g_config.anisotropic_filtering)
        : stock_exponent;
    g_anisotropy_writer_original(effective_exponent);

    if (behavior_active &&
        texture_filtering::ShouldInstallWriter(
            g_config.anisotropic_filtering)) {
        int applied_anisotropy = 0;
        const bool verified = SafeRead(
            ResolveAddress(texture_filtering::kAnisotropyValueRva),
            applied_anisotropy) &&
            applied_anisotropy == g_config.anisotropic_filtering;
        bool expected = false;
        if (verified &&
            g_anisotropy_writer_first_verified_invocation_logged
                .compare_exchange_strong(expected, true,
                                         std::memory_order_relaxed)) {
            log::InfoF(
                "texture_filtering writer_first_verified_invocation=1 "
                "stock_exponent=%d effective_exponent=%d anisotropy=%d",
                stock_exponent, effective_exponent, applied_anisotropy);
        }
    }
}

std::uint64_t DetourPcFileRead(void* device, void* file, void* buffer, std::uint64_t byte_count) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_pc_file_read_original(device, file, buffer, byte_count);
    }
    return qfile_io::ReadFileDevice(device, file, buffer, byte_count);
}

bool DetourPcFileSeek(void* device, void* file, std::uint32_t origin, std::int64_t offset) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_pc_file_seek_original(device, file, origin, offset);
    }
    return qfile_io::SeekFileDevice(device, file, origin, offset);
}

std::uint64_t DetourPcFileTell(void* device, void* file) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_pc_file_tell_original(device, file);
    }
    return qfile_io::TellFileDevice(device, file);
}

std::uint64_t DetourPcFileSize(void* device, void* file) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_pc_file_size_original(device, file);
    }
    return qfile_io::SizeFileDevice(device, file);
}

std::uint64_t DetourQFileReadAt(void* file, void* buffer, std::uint64_t byte_count,
                                std::int64_t offset, std::uint32_t origin) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_qfile_read_at_original(file, buffer, byte_count, offset, origin);
    }
    return qfile_io::ReadAt(g_qfile_ready, file, buffer, byte_count, offset, origin);
}

std::uint64_t DetourQFileWriteAt(void* file, const void* buffer, std::uint64_t byte_count,
                                 std::int64_t offset, std::uint32_t origin, bool* disk_full) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_qfile_write_at_original(file, buffer, byte_count, offset, origin, disk_full);
    }
    return qfile_io::WriteAt(g_qfile_ready, file, buffer, byte_count, offset, origin, disk_full);
}

// Engine-owned streaming buffers can become stale while an asynchronous load
// is being cancelled.  Keep the policy code allocation-free and zero-copy,
// but isolate every raw buffer access behind a non-inlined SEH boundary.  On
// x64 this adds no per-byte validation work to the valid path.
__declspec(noinline) qcmp::Result DecodeQcmpSafely(
    void* source,
    std::uint64_t source_size,
    void* destination,
    std::uint64_t destination_capacity,
    unsigned long& seh_code) noexcept {
    seh_code = 0;
    qcmp::Result result{};
    __try {
        result = qcmp::Decode(source, source_size, destination, destination_capacity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh_code = GetExceptionCode();
        result = {qcmp::Error::BufferAccessException};
    }
    return result;
}

__declspec(noinline) bool ClearQcmpDestinationSafely(void* destination) noexcept {
    __try {
        *static_cast<std::byte*>(destination) = std::byte{0};
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) chunk_stream::Result ValidateChunkStreamSafely(
    const void* buffer,
    std::size_t size,
    unsigned long& seh_code) noexcept {
    seh_code = 0;
    chunk_stream::Result result{};
    __try {
        result = chunk_stream::Validate(buffer, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh_code = GetExceptionCode();
        result = {chunk_stream::Error::BufferAccessException};
    }
    return result;
}

std::uint64_t DetourQcmpDecompress(void* source,
                                   std::uint64_t source_size,
                                   void* destination,
                                   std::uint64_t destination_capacity,
                                   const char* label) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_qcmp_decompress_original(
            source, source_size, destination, destination_capacity, label);
    }
    unsigned long seh_code = 0;
    const qcmp::Result result = DecodeQcmpSafely(
        source, source_size, destination, destination_capacity, seh_code);
    if (!result.valid()) {
        const bool clear_attempted = destination != nullptr && destination_capacity != 0 &&
                                     destination_capacity != qcmp::kFailure;
        const bool clear_succeeded =
            !clear_attempted || ClearQcmpDestinationSafely(destination);
        if (!g_qcmp_rejection_logged.exchange(true, std::memory_order_relaxed)) {
            log::WarnF(
                "qcmp rejected reason=%s source_size=%llu destination_capacity=%llu "
                "data_offset=%u stream_end=%llu declared_output=%llu "
                "exception_code=0x%08lX destination_clear_attempted=%d "
                "destination_clear_succeeded=%d label_ptr=0x%p",
                qcmp::ErrorName(result.error),
                static_cast<unsigned long long>(source_size),
                static_cast<unsigned long long>(destination_capacity),
                result.data_offset,
                static_cast<unsigned long long>(result.stream_end),
                static_cast<unsigned long long>(result.declared_output_size),
                seh_code,
                clear_attempted ? 1 : 0,
                clear_succeeded ? 1 : 0,
                static_cast<const void*>(label));
        }
        return qcmp::kFailure;
    }
    return result.output_size;
}

void ResetArchiveEntryGuard() noexcept {
    for (ArchiveSizeCacheEntry& entry : g_archive_size_cache) {
        entry.inventory.store(0, std::memory_order_relaxed);
        entry.file.store(0, std::memory_order_relaxed);
        entry.native_handle.store(0, std::memory_order_relaxed);
        entry.resource_uid.store(0, std::memory_order_relaxed);
        entry.size.store(qfile_io::kOperationFailure, std::memory_order_relaxed);
    }
    for (std::atomic<std::uint64_t>& key : g_archive_rejection_keys) {
        key.store(0, std::memory_order_relaxed);
    }
}

std::uint64_t CachedArchiveSize(void* inventory, void* file, std::uint32_t resource_uid) noexcept {
    const HANDLE handle = qfile_io::NativeHandle(file);
    if (inventory == nullptr || file == nullptr || !qfile_io::IsUsableHandle(handle)) {
        return qfile_io::kOperationFailure;
    }

    const auto inventory_key = reinterpret_cast<std::uintptr_t>(inventory);
    const auto file_key = reinterpret_cast<std::uintptr_t>(file);
    const auto handle_key = reinterpret_cast<std::uintptr_t>(handle);
    for (const ArchiveSizeCacheEntry& entry : g_archive_size_cache) {
        if (entry.inventory.load(std::memory_order_acquire) == inventory_key &&
            entry.file.load(std::memory_order_relaxed) == file_key &&
            entry.native_handle.load(std::memory_order_relaxed) == handle_key &&
            entry.resource_uid.load(std::memory_order_relaxed) == resource_uid) {
            return entry.size.load(std::memory_order_relaxed);
        }
    }

    const std::uint64_t size = qfile_io::SizeFileDevice(nullptr, file);
    if (size == qfile_io::kOperationFailure) {
        return size;
    }

    for (ArchiveSizeCacheEntry& entry : g_archive_size_cache) {
        std::uintptr_t expected = 0;
        if (entry.inventory.compare_exchange_strong(expected, kArchiveSizeCacheClaimed,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_relaxed)) {
            entry.file.store(file_key, std::memory_order_relaxed);
            entry.native_handle.store(handle_key, std::memory_order_relaxed);
            entry.resource_uid.store(resource_uid, std::memory_order_relaxed);
            entry.size.store(size, std::memory_order_relaxed);
            entry.inventory.store(inventory_key, std::memory_order_release);
            break;
        }
    }
    return size;
}

bool MarkArchiveRejectionForLogging(std::uint32_t resource_uid, std::uint32_t entry_uid) noexcept {
    std::uint64_t key = (static_cast<std::uint64_t>(resource_uid) << 32) | entry_uid;
    if (key == 0) {
        key = 1;
    }
    const std::size_t start =
        static_cast<std::size_t>((key ^ (key >> 33)) % kArchiveSizeCacheCapacity);
    for (std::size_t probe = 0; probe < kArchiveSizeCacheCapacity; ++probe) {
        std::atomic<std::uint64_t>& slot =
            g_archive_rejection_keys[(start + probe) % kArchiveSizeCacheCapacity];
        std::uint64_t existing = slot.load(std::memory_order_relaxed);
        if (existing == key) {
            return false;
        }
        if (existing == 0 &&
            slot.compare_exchange_strong(existing, key, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void* DetourStreamFileOpen(const char* path, std::uint32_t access, bool allow_loose_file,
                           void* device, void* user_data) {
    void* const stream_file =
        g_stream_file_open_original(path, access, allow_loose_file, device, user_data);
    if (stream_file == nullptr || !g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return stream_file;
    }

    std::uint32_t kind = 0;
    void* inventory = nullptr;
    void* entry = nullptr;
    void* file = nullptr;
    std::uint32_t resource_uid = 0;
    archive_io::EntryDescriptor descriptor{};
    const std::uintptr_t stream_file_address = reinterpret_cast<std::uintptr_t>(stream_file);
    if (!SafeRead(stream_file_address + archive_io::kStreamFileKindOffset, kind) ||
        kind != archive_io::kArchiveStreamFileKind ||
        !SafeRead(stream_file_address + archive_io::kStreamFileInventoryOffset, inventory) ||
        !SafeRead(stream_file_address + archive_io::kStreamFileEntryOffset, entry) ||
        !SafeRead(stream_file_address + archive_io::kStreamFileQFileOffset, file) ||
        inventory == nullptr || entry == nullptr || file == nullptr ||
        !SafeRead(reinterpret_cast<std::uintptr_t>(inventory) +
                      archive_io::kInventoryResourceUidOffset,
                  resource_uid) ||
        !SafeRead(reinterpret_cast<std::uintptr_t>(entry), descriptor)) {
        return stream_file;
    }

    const std::uint64_t archive_size = CachedArchiveSize(inventory, file, resource_uid);
    if (archive_size == qfile_io::kOperationFailure) {
        return stream_file;
    }

    archive_io::ReadRange range{};
    if (archive_io::IsWithinArchive(descriptor, archive_size, &range)) {
        return stream_file;
    }

    if (MarkArchiveRejectionForLogging(resource_uid, descriptor.uid)) {
        log::WarnF("archive entry rejected path=%s inventory_uid=0x%08X entry_uid=0x%08X "
                   "offset=%llu read_size=%llu archive_size=%llu",
                   path != nullptr ? path : "<null>", resource_uid, descriptor.uid,
                   static_cast<unsigned long long>(range.offset),
                   static_cast<unsigned long long>(range.size),
                   static_cast<unsigned long long>(archive_size));
    }
    g_stream_file_close(stream_file);
    return nullptr;
}

void DetourResourceChunkDispatch(void* manager, void* buffer, std::uint32_t size) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_resource_chunk_dispatch_original(manager, buffer, size);
        return;
    }
    unsigned long seh_code = 0;
    const chunk_stream::Result result =
        ValidateChunkStreamSafely(buffer, size, seh_code);
    if (result.valid()) {
        g_resource_chunk_dispatch_original(manager, buffer, size);
        return;
    }

    if (!g_resource_chunk_rejection_logged.exchange(true, std::memory_order_relaxed)) {
        log::WarnF(
            "resource chunk stream rejected reason=%s offset=%zu total_size=%u "
            "chunk_id=0x%08X chunk_size=%d data_size=%d data_offset=%u "
            "big_file_reason=%s entry_count=%u entries_offset=%zu "
            "offending_index=%u exception_code=0x%08lX",
            chunk_stream::ErrorName(result.error),
            result.offset,
            size,
            result.header.id,
            result.header.chunk_size,
            result.header.data_size,
            result.header.data_offset,
            big_file_index::ErrorName(result.big_file_result.error),
            result.big_file_result.entry_count,
            result.big_file_result.entries_offset,
            result.big_file_result.offending_index,
            seh_code);
    }
}

void DetourScaleformTime() {
    g_scaleform_time_count.fetch_add(1);

    const std::uintptr_t provider = ReadProviderValue();
    if (provider != 0) {
        g_scaleform_provider_non_null_count.fetch_add(1);
    }

    if (provider != 0 && ShouldLogVerbose(g_scaleform_time_verbose)) {
        log::InfoF("scaleform_time provider=0x%p", reinterpret_cast<void*>(provider));
    }

    MaybeWriteSummary();
    g_scaleform_time_original();
}

void DetourScaleformInit(void* heap_desc, void* sys_alloc) {
    g_scaleform_init_count.fetch_add(1);
    g_scaleform_init_original(heap_desc, sys_alloc);

    const std::uintptr_t provider = ReadProviderValue();
    if (ShouldLogVerbose(g_scaleform_init_verbose)) {
        log::InfoF("scaleform_init heap_desc=0x%p sys_alloc=0x%p provider=0x%p",
                   heap_desc,
                   sys_alloc,
                   reinterpret_cast<void*>(provider));
    }

    MaybeWriteSummary();
}

void DetourNisSetPlayTime(void* nis_manager, float scene_time, bool sync_scene_time) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_nis_set_play_time_original(nis_manager, scene_time, sync_scene_time);
        return;
    }
    g_nis_set_play_time_count.fetch_add(1);
    if (sync_scene_time) {
        g_nis_sync_scene_time_count.fetch_add(1);
    }

    const auto manager = reinterpret_cast<std::uintptr_t>(nis_manager);
    const auto previous_manager = g_last_nis_manager_tls;
    const auto previous_instance = g_last_nis_instance_tls;
    const float previous_raw_scene_time = g_last_nis_raw_scene_time_tls;
    const float previous_applied_scene_time = g_last_nis_applied_scene_time_tls;
    const bool previous_history_valid = g_last_nis_scene_history_valid_tls;
    const CutsceneCadenceTracker previous_cadence_tracker =
        g_cutscene_cadence_tracker_tls;

    int state_before = 0;
    std::uintptr_t active_instance = 0;
    if (manager != 0) {
        SafeRead(manager + 0x44, state_before);
        SafeRead(manager + 0x10, active_instance);
    }

    const bool in_cutscene_scope = g_cutscene_flow_owner_scope_depth != 0 ||
                                   g_nis_owner_scope_depth != 0;
    const CutscenePauseState pause_state =
        g_config.fix_cutscene_scene_time_step && in_cutscene_scope
            ? ReadCutscenePauseStateSafely()
            : CutscenePauseState{};
    // History is per engine thread and per active NIS instance. A singleton
    // manager can be reused for another scene, so manager identity alone is
    // not a safe continuity signal.
    const bool scene_time_rewound =
        previous_history_valid && std::isfinite(scene_time) &&
        std::isfinite(previous_raw_scene_time) && scene_time < previous_raw_scene_time;
    const bool timeline_must_reset =
        sync_scene_time || pause_state.ui_paused || scene_time_rewound ||
        (previous_history_valid &&
         (!in_cutscene_scope || state_before != 4 || manager == 0 || active_instance == 0 ||
          previous_manager != manager || previous_instance != active_instance));
    if (timeline_must_reset) {
        ResetTrackedCutsceneTimeline();
    }
    const bool same_timeline = previous_history_valid && !timeline_must_reset;

    float current_dt = (g_nis_owner_scope_depth != 0 &&
                        g_current_nis_owner_manager == manager)
                           ? g_current_nis_owner_dt
                           : 0.0f;
    if (!IsSaneCutsceneDelta(current_dt)) {
        current_dt = g_current_cutscene_dt;
    }
    if (!IsSaneCutsceneDelta(current_dt)) {
        current_dt = g_cutscene_flow_input_dt;
    }
    if (!IsSaneCutsceneDelta(current_dt)) {
        // Keep the completed cadence thread-local.  The diagnostic atomics
        // are process-wide and may belong to another NIS worker thread.
        current_dt = g_last_cutscene_flow_completed_dt_tls;
    }
    if (timeline_must_reset && active_instance != 0 && in_cutscene_scope && state_before == 4 &&
        !pause_state.ui_paused && !sync_scene_time) {
        // The owner sample was observed before SetPlayTime exposed an identity
        // change or raw rewind. Re-seed after clearing the old timeline so the
        // new baseline retains this frame's cadence immediately.
        current_dt = TrackCutsceneCadence(current_dt);
    } else {
        current_dt = ResolveTrackedCutsceneBaseDelta(
            current_dt, g_cutscene_cadence_tracker_tls.stable_delta, g_config.cutscene_fps);
    }
    // Keep the scene-time resolver aware of a recognized 30/60-Hz candidate
    // while the cadence tracker is still inside its hysteresis window.  A
    // target such as 62 FPS is close enough to 60 that a proximity-only
    // live/scene comparison would otherwise suppress the correction before
    // the candidate is either adopted or rejected.
    const bool legacy_fallback_pending =
        IsLegacyFallbackPending(g_cutscene_cadence_tracker_tls, g_config.cutscene_fps);

    const auto scene_decision = ResolveCutsceneSceneTime(
        in_cutscene_scope && state_before == 4,
        // Either pause flag means the game wants the clock stopped; the scene
        // time must not be advanced past it.
        pause_state.ui_paused || pause_state.simtime_paused,
        g_config.fix_cutscene_scene_time_step && same_timeline,
        sync_scene_time,
        previous_raw_scene_time,
        previous_applied_scene_time,
        scene_time,
        current_dt,
        g_config.cutscene_fps,
        legacy_fallback_pending);
    const float applied_scene_time = scene_decision.applied_scene_time;
    const bool scene_time_fixed = scene_decision.corrected;
    const bool timing_repaired = scene_decision.repaired_timing;
    if (timing_repaired) {
        g_nis_scene_time_fix_count.fetch_add(1);
        // Same reason as the zero-dt log above: this repair re-bases the
        // timeline the director samples, so it can put object tracks ahead of
        // the bones they are attached to. Name every application, budgeted.
        static std::atomic<int> scene_time_repair_budget{16};
        int remaining = scene_time_repair_budget.load(std::memory_order_relaxed);
        while (remaining > 0 &&
               !scene_time_repair_budget.compare_exchange_weak(
                   remaining, remaining - 1, std::memory_order_relaxed)) {
        }
        if (remaining > 0) {
            log::InfoF("cutscene_scene_time_repair raw=%.6f applied=%.6f "
                       "raw_delta=%.6f applied_delta=%.6f",
                       scene_time,
                       applied_scene_time,
                       scene_time - previous_raw_scene_time,
                       applied_scene_time - previous_applied_scene_time);
        }
    }
    const float outer_dt = current_dt;

    float delta = 0.0f;
    if (same_timeline && applied_scene_time >= previous_applied_scene_time) {
        delta = applied_scene_time - previous_applied_scene_time;
        if (delta >= 0.0f && delta < 0.0005f) {
            g_nis_delta_zero_count.fetch_add(1);
        } else if (delta > 0.0f && delta < 0.2f) {
            if (delta >= 0.030f && delta <= 0.036f) {
                g_nis_delta_30hz_count.fetch_add(1);
            } else if (IsApprox60HzDelta(delta)) {
                g_nis_delta_60hz_count.fetch_add(1);
            } else {
                g_nis_delta_other_count.fetch_add(1);
            }
        }
    }

    if (g_nis_play_scope_depth != 0) {
        ++g_nis_play_scope_calls;
        if (delta >= 0.0f && delta < 0.0005f && same_timeline) {
            ++g_nis_play_scope_zero_delta_calls;
        } else if (delta > 0.0f) {
            ++g_nis_play_scope_advanced_calls;
        }
    }

    if (g_nis_owner_scope_depth != 0) {
        ++g_nis_owner_scope_calls;
        if (delta >= 0.0f && delta < 0.0005f && same_timeline) {
            ++g_nis_owner_scope_zero_delta_calls;
        } else if (delta > 0.0f) {
            ++g_nis_owner_scope_advanced_calls;
        }
    }

    g_last_nis_scene_time_bits.store(FloatToBits(applied_scene_time));
    g_last_nis_scene_delta_bits.store(FloatToBits(delta));
    g_last_nis_manager.store(manager);
    g_last_nis_manager_tls = manager;
    g_last_nis_instance_tls = active_instance;
    g_last_nis_raw_scene_time_tls = scene_time;
    g_last_nis_applied_scene_time_tls = applied_scene_time;
    g_last_nis_scene_history_valid_tls =
        g_config.fix_cutscene_scene_time_step && in_cutscene_scope && state_before == 4 &&
        active_instance != 0 && !pause_state.ui_paused && !sync_scene_time &&
        std::isfinite(scene_time) && std::isfinite(applied_scene_time) &&
        CanRetainCutsceneTimelineHistory(
            same_timeline, previous_applied_scene_time, applied_scene_time);

    if (ShouldLogVerbose(g_nis_set_play_time_verbose)) {
        const char* bucket = ClassifySmallDelta(delta);

        log::InfoF(
            "nis_set_play_time manager=0x%p scene_time=%.6f applied_scene_time=%.6f delta=%.6f sync=%d state=%d active_instance=0x%p bucket=%s scene_fix=%d scene_repair=%d outer_dt=%.6f",
            nis_manager,
            scene_time,
            applied_scene_time,
            delta,
            sync_scene_time ? 1 : 0,
            state_before,
            reinterpret_cast<void*>(active_instance),
            bucket,
            scene_time_fixed ? 1 : 0,
            timing_repaired ? 1 : 0,
            outer_dt);
    }

    MaybeWriteSummary();
    bool original_completed = false;
    __try {
        g_nis_set_play_time_original(nis_manager, applied_scene_time, sync_scene_time);
        original_completed = true;
    } __finally {
        if (!original_completed) {
            g_last_nis_manager_tls = previous_manager;
            g_last_nis_instance_tls = previous_instance;
            g_last_nis_raw_scene_time_tls = previous_raw_scene_time;
            g_last_nis_applied_scene_time_tls = previous_applied_scene_time;
            g_last_nis_scene_history_valid_tls = previous_history_valid;
            g_cutscene_cadence_tracker_tls = previous_cadence_tracker;
        }
    }
}

void DetourNisPlay(void* nis_manager) {
    g_nis_play_count.fetch_add(1);

    int state_before = 0;
    int phase_before = 0;
    std::uintptr_t active_instance_before = 0;
    int active_mode_before = 0;
    if (nis_manager != nullptr) {
        const auto manager = reinterpret_cast<std::uintptr_t>(nis_manager);
        SafeRead(manager + 0x44, state_before);
        SafeRead(manager + 0x5C, phase_before);
        SafeRead(manager + 0x10, active_instance_before);
        if (active_instance_before != 0) {
            SafeRead(active_instance_before + 0x28, active_mode_before);
        }
    }

    const unsigned int previous_depth = g_nis_play_scope_depth++;
    if (previous_depth == 0) {
        g_nis_play_scope_calls = 0;
        g_nis_play_scope_advanced_calls = 0;
        g_nis_play_scope_zero_delta_calls = 0;
    }

    bool original_completed = false;
    __try {
        g_nis_play_original(nis_manager);
        original_completed = true;
    } __finally {
        if (!original_completed) {
            // Do not let a structured exception in the stock NIS routine
            // strand this worker in a permanently nested timing scope.  The
            // exception continues through the engine, while the TLS state is
            // restored before the next cutscene callback on this thread.
            if (g_nis_play_scope_depth != 0) {
                --g_nis_play_scope_depth;
            }
            if (g_nis_play_scope_depth == 0) {
                g_nis_play_scope_calls = 0;
                g_nis_play_scope_advanced_calls = 0;
                g_nis_play_scope_zero_delta_calls = 0;
            }
        }
    }

    const unsigned int inner_calls = g_nis_play_scope_calls;
    const unsigned int advanced_calls = g_nis_play_scope_advanced_calls;
    const unsigned int zero_delta_calls = g_nis_play_scope_zero_delta_calls;
    --g_nis_play_scope_depth;
    if (g_nis_play_scope_depth == 0) {
        g_nis_play_scope_calls = 0;
        g_nis_play_scope_advanced_calls = 0;
        g_nis_play_scope_zero_delta_calls = 0;
    }

    if (advanced_calls != 0) {
        g_nis_play_advanced_count.fetch_add(1);
    } else if (inner_calls != 0) {
        g_nis_play_repeat_count.fetch_add(1);
    }

    if (inner_calls > 1) {
        g_nis_play_multi_tick_count.fetch_add(1);
    }

    if (ShouldLogVerbose(g_nis_play_verbose) || inner_calls == 0 ||
        inner_calls > 1) {
        // (zero_delta_calls is budgeted separately below: while a cutscene is
        // paused the play cadence keeps running with zero deltas, so that
        // condition is a steady state and would log every frame without a
        // cap; the other two are genuine per-session anomalies.)
        int state_after = 0;
        int phase_after = 0;
        std::uintptr_t active_instance_after = 0;
        int active_mode_after = 0;
        if (nis_manager != nullptr) {
            const auto manager = reinterpret_cast<std::uintptr_t>(nis_manager);
            SafeRead(manager + 0x44, state_after);
            SafeRead(manager + 0x5C, phase_after);
            SafeRead(manager + 0x10, active_instance_after);
            if (active_instance_after != 0) {
                SafeRead(active_instance_after + 0x28, active_mode_after);
            }
        }

        log::InfoF(
            "nis_play manager=0x%p inner_calls=%u advanced=%u zero_delta=%u "
            "state_before=%d state_after=%d phase_before=%d phase_after=%d "
            "active_before=0x%p mode_before=%d active_after=0x%p mode_after=%d",
            nis_manager,
            inner_calls,
            advanced_calls,
            zero_delta_calls,
            state_before,
            state_after,
            phase_before,
            phase_after,
            reinterpret_cast<void*>(active_instance_before),
            active_mode_before,
            reinterpret_cast<void*>(active_instance_after),
            active_mode_after);
    }

    // Zero-delta steady state (paused cutscene): budgeted, because the play
    // cadence keeps calling this every frame with zero_delta_calls != 0 for
    // the whole pause.
    if (zero_delta_calls != 0) {
        static std::atomic<int> zero_delta_log_budget{16};
        int remaining = zero_delta_log_budget.load(std::memory_order_relaxed);
        while (remaining > 0 &&
               !zero_delta_log_budget.compare_exchange_weak(
                   remaining, remaining - 1, std::memory_order_relaxed)) {
        }
        if (remaining > 0) {
            log::InfoF("nis_play zero_delta steady state inner_calls=%u "
                       "zero_delta=%u state_before=%d",
                       inner_calls,
                       zero_delta_calls,
                       state_before);
        }
    }

    MaybeWriteSummary();
}

unsigned char DetourNisBootstrap(void* nis_manager, void* scene_asset) {
    g_nis_bootstrap_count.fetch_add(1);

    const unsigned char result = g_nis_bootstrap_original(nis_manager, scene_asset);

    int state = 0;
    std::uint32_t timeout_bits = 0;
    std::uintptr_t active_instance = 0;
    if (nis_manager != nullptr) {
        const auto manager = reinterpret_cast<std::uintptr_t>(nis_manager);
        SafeRead(manager + 0x44, state);
        SafeRead(manager + 0x48, timeout_bits);
        SafeRead(manager + 0x10, active_instance);
    }

    if (result == 0) {
        g_nis_bootstrap_fail_count.fetch_add(1);
    } else if (state == 1) {
        g_nis_bootstrap_state1_count.fetch_add(1);
    } else if (state == 2) {
        g_nis_bootstrap_state2_count.fetch_add(1);
    }

    if (ShouldLogVerbose(g_nis_bootstrap_verbose) || result == 0) {
        log::InfoF(
            "nis_bootstrap manager=0x%p scene_asset=0x%p result=%u state=%d timeout=%.6f active_instance=0x%p",
            nis_manager,
            scene_asset,
            static_cast<unsigned int>(result),
            state,
            BitsToFloat(timeout_bits),
            reinterpret_cast<void*>(active_instance));
    }

    MaybeWriteSummary();
    return result;
}

void DetourNisOwner(void* nis_manager, float delta_seconds) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_nis_owner_original(nis_manager, delta_seconds);
        return;
    }
    g_nis_owner_count.fetch_add(1);
    g_last_nis_owner_dt_bits.store(FloatToBits(delta_seconds));

    if (delta_seconds >= 0.0f && delta_seconds < 0.0005f) {
        g_nis_owner_dt_zero_count.fetch_add(1);
    } else if (delta_seconds > 0.0f && delta_seconds < 0.2f) {
        if (delta_seconds >= 0.030f && delta_seconds <= 0.036f) {
            g_nis_owner_dt_30hz_count.fetch_add(1);
        } else if (delta_seconds >= 0.015f && delta_seconds <= 0.019f) {
            g_nis_owner_dt_60hz_count.fetch_add(1);
        } else {
            g_nis_owner_dt_other_count.fetch_add(1);
        }
    }

    int state_before = 0;
    int phase_before = 0;
    std::uintptr_t active_instance_before = 0;
    if (nis_manager != nullptr) {
        const auto manager = reinterpret_cast<std::uintptr_t>(nis_manager);
        SafeRead(manager + 0x44, state_before);
        SafeRead(manager + 0x5C, phase_before);
        SafeRead(manager + 0x10, active_instance_before);
    }

    const unsigned int previous_depth = g_nis_owner_scope_depth++;
    const std::uintptr_t previous_nis_owner_manager =
        g_current_nis_owner_manager;
    g_current_nis_owner_manager = reinterpret_cast<std::uintptr_t>(nis_manager);
    if (previous_depth == 0) {
        g_nis_owner_scope_calls = 0;
        g_nis_owner_scope_advanced_calls = 0;
        g_nis_owner_scope_zero_delta_calls = 0;
    }

    const float previous_nis_owner_dt = g_current_nis_owner_dt;
    g_current_nis_owner_dt = delta_seconds;
    __try {
        g_nis_owner_original(nis_manager, delta_seconds);
    } __finally {
        // Detours can be entered by engine code that raises a structured
        // exception.  Always restore the TLS cadence so a later frame cannot
        // inherit a stale owner delta.
        g_current_nis_owner_dt = previous_nis_owner_dt;
        // The previous manager already lives in this recursive call's stack
        // frame, so restoring it does not need a fixed-size TLS side stack.
        // Deep but valid nesting can no longer overflow and lose its owner.
        g_current_nis_owner_manager = previous_nis_owner_manager;
        if (g_nis_owner_scope_depth != 0) {
            --g_nis_owner_scope_depth;
        }
    }

    const unsigned int inner_calls = g_nis_owner_scope_calls;
    const unsigned int advanced_calls = g_nis_owner_scope_advanced_calls;
    const unsigned int zero_delta_calls = g_nis_owner_scope_zero_delta_calls;
    if (g_nis_owner_scope_depth == 0) {
        g_nis_owner_scope_calls = 0;
        g_nis_owner_scope_advanced_calls = 0;
        g_nis_owner_scope_zero_delta_calls = 0;
    }

    if (advanced_calls != 0) {
        g_nis_owner_advanced_count.fetch_add(1);
    } else if (inner_calls != 0) {
        g_nis_owner_repeat_count.fetch_add(1);
    }

    if (inner_calls > 1) {
        g_nis_owner_multi_tick_count.fetch_add(1);
    }

    if (ShouldLogVerbose(g_nis_owner_verbose)) {
        int state_after = 0;
        int phase_after = 0;
        std::uintptr_t active_instance_after = 0;
        if (nis_manager != nullptr) {
            const auto manager = reinterpret_cast<std::uintptr_t>(nis_manager);
            SafeRead(manager + 0x44, state_after);
            SafeRead(manager + 0x5C, phase_after);
            SafeRead(manager + 0x10, active_instance_after);
        }

        log::InfoF(
            "nis_owner manager=0x%p dt=%.6f dt_bucket=%s inner_calls=%u advanced=%u zero_delta=%u "
            "state_before=%d state_after=%d phase_before=%d phase_after=%d active_before=0x%p active_after=0x%p",
            nis_manager,
            delta_seconds,
            ClassifySmallDelta(delta_seconds),
            inner_calls,
            advanced_calls,
            zero_delta_calls,
            state_before,
            state_after,
            phase_before,
            phase_after,
            reinterpret_cast<void*>(active_instance_before),
            reinterpret_cast<void*>(active_instance_after));
    }

    MaybeWriteSummary();
}

void DetourNisActorSetup(std::uintptr_t actor_state, std::uintptr_t actor_target) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_nis_actor_setup_original(actor_state, actor_target);
        return;
    }
    g_nis_actor_setup_count.fetch_add(1);

    const nisprobe::SetupResult lifecycle = TrackNisActorSetupLocked(actor_state, actor_target);

    if (lifecycle.duplicate) {
        g_nis_actor_setup_duplicate_count.fetch_add(1);
    }

    if (ShouldLogVerbose(g_nis_actor_state_verbose) || actor_state == 0 || actor_target == 0 ||
        lifecycle.duplicate) {
        log::InfoF("nis_actor_setup state=0x%p target=0x%p duplicate=%d active=%llu setup_count=%u "
                   "restore_count=%u last_target=0x%p",
                   reinterpret_cast<void*>(actor_state),
                   reinterpret_cast<void*>(actor_target),
                   lifecycle.duplicate ? 1 : 0,
                   lifecycle.active_count,
                   lifecycle.setup_count,
                   lifecycle.restore_count,
                   reinterpret_cast<void*>(lifecycle.last_target));
    }

    bool original_completed = false;
    __try {
        g_nis_actor_setup_original(actor_state, actor_target);
        original_completed = true;
    } __finally {
        if (!original_completed) {
            // Tracking is updated before the stock call so a nested restore can
            // see the actor.  If stock setup faults, discard the optional table
            // rather than leaving a phantom live actor that later suppresses a
            // legitimate restore.
            ResetNisActorTrackerLocked();
        }
    }
    MaybeWriteSummary();
}

void DetourNisActorRestore(std::uintptr_t actor_state) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_nis_actor_restore_original(actor_state);
        return;
    }
    g_nis_actor_restore_count.fetch_add(1);

    const nisprobe::RestoreResult lifecycle = TrackNisActorRestoreLocked(actor_state);

    const bool tracked = lifecycle.disposition == nisprobe::RestoreDisposition::tracked;
    const bool duplicate = lifecycle.disposition == nisprobe::RestoreDisposition::duplicate;
    const bool never_seen = lifecycle.disposition == nisprobe::RestoreDisposition::never_seen;
    const auto forwarding = nisprobe::ResolveRestoreForwardingDecision(
        lifecycle.disposition, g_config.fix_nis_actor_restore_duplicates);

    if (duplicate) {
        g_nis_actor_restore_duplicate_count.fetch_add(1);
    }
    if (never_seen) {
        g_nis_actor_restore_never_seen_count.fetch_add(1);
    }
    if (!tracked) {
        g_nis_actor_restore_untracked_count.fetch_add(1);
    }
    if (forwarding.suppressed_duplicate) {
        g_nis_actor_restore_suppressed_count.fetch_add(1);
    }

    const bool should_log_restore = ShouldLogVerbose(g_nis_actor_state_verbose) || actor_state == 0 ||
                                    never_seen || (duplicate && g_config.hook_nis_actor_state);
    if (should_log_restore) {
        log::WarnF("nis_actor_restore state=0x%p disposition=%s tracked=%d duplicate=%d active=%llu "
                   "setup_count=%u restore_count=%u last_target=0x%p call_original=%d suppressed=%d",
                   reinterpret_cast<void*>(actor_state),
                   nisprobe::DescribeDisposition(lifecycle.disposition),
                   tracked ? 1 : 0,
                   duplicate ? 1 : 0,
                   lifecycle.active_count,
                   lifecycle.setup_count,
                   lifecycle.restore_count,
                   reinterpret_cast<void*>(lifecycle.last_target),
                   forwarding.call_original ? 1 : 0,
                   forwarding.suppressed_duplicate ? 1 : 0);
    }

    if (forwarding.call_original) {
        bool original_completed = false;
        __try {
            g_nis_actor_restore_original(actor_state);
            original_completed = true;
        } __finally {
            if (!original_completed) {
                // A failed stock restore must not make the next real restore
                // look like a duplicate.  Resetting the optional tracker is a
                // conservative fail-open rollback that is safe across threads.
                ResetNisActorTrackerLocked();
            }
        }
    }
    MaybeWriteSummary();
}

void DetourTwitchStateTick(std::uintptr_t owner) {
    g_twitch_state_tick_count.fetch_add(1);

    const int state_before = ReadTwitchState(owner);
    const std::uintptr_t handle_before = ReadTwitchHandle(owner);
    const unsigned int flag_754_before = ReadTwitchFlagByte(owner, 0x754);
    const unsigned int flag_755_before = ReadTwitchFlagByte(owner, 0x755);
    const unsigned int flag_756_before = ReadTwitchFlagByte(owner, 0x756);

    g_twitch_state_tick_original(owner);

    const int state_after = ReadTwitchState(owner);
    const std::uintptr_t handle_after = ReadTwitchHandle(owner);
    const unsigned int flag_754_after = ReadTwitchFlagByte(owner, 0x754);
    const unsigned int flag_755_after = ReadTwitchFlagByte(owner, 0x755);
    const unsigned int flag_756_after = ReadTwitchFlagByte(owner, 0x756);
    const bool interesting = owner == 0 || state_before != state_after || state_before == 3 ||
                             state_before == 5 || state_before == 7;

    if (interesting || ShouldLogVerbose(g_twitch_probe_verbose)) {
        log::InfoF(
            "twitch_tick owner=0x%p state_before=%d state_after=%d handle_before=0x%p handle_after=0x%p "
            "flags_before=%u/%u/%u flags_after=%u/%u/%u",
            reinterpret_cast<void*>(owner),
            state_before,
            state_after,
            reinterpret_cast<void*>(handle_before),
            reinterpret_cast<void*>(handle_after),
            flag_754_before,
            flag_755_before,
            flag_756_before,
            flag_754_after,
            flag_755_after,
            flag_756_after);
    }

    MaybeWriteSummary();
}

void MaybeWritePass8Summary() {
    const unsigned long long now = GetTickCount64();
    if (g_pass8_summary_last_tick == 0) {
        g_pass8_summary_last_tick = now;
        return;
    }
    if (now - g_pass8_summary_last_tick < 10000) {
        return;
    }
    g_pass8_summary_last_tick = now;
    // Counters are cumulative for the whole run; allocation volumes are
    // derived offline from the doc-295 per-message allocation mapping.
    log::InfoF(
        "pass8_probe summary messages_total=%llu mousemove_total=%llu "
        "mousemove_same_position=%llu timer_calls=%llu timer_advanced=%llu "
        "timer_pulse_advance=%llu osgameurl_calls=%llu",
        g_pass8_messages_total.load(std::memory_order_relaxed),
        g_pass8_mousemove_total.load(std::memory_order_relaxed),
        g_pass8_mousemove_same_position.load(std::memory_order_relaxed),
        g_pass8_timer_calls.load(std::memory_order_relaxed),
        g_pass8_timer_advanced.load(std::memory_order_relaxed),
        g_pass8_timer_pulse_advance.load(std::memory_order_relaxed),
        g_pass8_os_game_url_calls.load(std::memory_order_relaxed));
}

long long DetourPass8WndProc(void* hwnd,
                             unsigned int message,
                             unsigned long long wparam,
                             long long lparam) {
    g_pass8_messages_total.fetch_add(1, std::memory_order_relaxed);
    if (message == 0x200) {  // WM_MOUSEMOVE
        g_pass8_mousemove_total.fetch_add(1, std::memory_order_relaxed);
        const int x = static_cast<int>(static_cast<short>(lparam & 0xFFFF));
        const int y = static_cast<int>(static_cast<short>((lparam >> 16) & 0xFFFF));
        if (g_pass8_have_mouse_position && x == g_pass8_last_mouse_x &&
            y == g_pass8_last_mouse_y) {
            g_pass8_mousemove_same_position.fetch_add(1, std::memory_order_relaxed);
        }
        g_pass8_last_mouse_x = x;
        g_pass8_last_mouse_y = y;
        g_pass8_have_mouse_position = true;
    }
    MaybeWritePass8Summary();
    return g_pass8_wndproc_original(hwnd, message, wparam, lparam);
}

int DetourPass8OsGameUrlProbe(void* result_struct, const char* url) {
    const unsigned long long call =
        g_pass8_os_game_url_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    LARGE_INTEGER start = {};
    LARGE_INTEGER end = {};
    QueryPerformanceCounter(&start);
    const int result = g_pass8_os_game_url_original(result_struct, url);
    QueryPerformanceCounter(&end);
    char url_text[97] = {};
    SafeCopyBytes(reinterpret_cast<std::uintptr_t>(url), url_text, sizeof(url_text) - 1);
    double elapsed_ms = -1.0;
    LARGE_INTEGER frequency = {};
    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
        elapsed_ms = (static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0) /
                     static_cast<double>(frequency.QuadPart);
    }
    log::InfoF("pass8_osgameurl call=%llu elapsed_ms=%.3f result=%d url=\"%.96s\"",
               call,
               elapsed_ms,
               result,
               url_text);
    return result;
}

void DetourFrameCountedActivityTimerFix(void* component, float delta_seconds) {
    // Docs 292/296/297: the stock writer refreshes the +0x1E5C activity
    // accumulator with a fixed 0.1 per call, event-counted, advancing 0.1
    // even at dt=0. Scale the pulse by the 60 Hz reference (0.1 * 60 * dt)
    // so wall-clock transition pacing matches the design point at any call
    // rate. Only the measured stock pulse shape is corrected; every other
    // stock outcome (clamps, selects, no-advance) is preserved.
    const std::uintptr_t accumulator_address =
        reinterpret_cast<std::uintptr_t>(component) + 0x1E5C;
    float before = 0.0f;
    const bool readable = SafeRead(accumulator_address, before);
    g_activity_timer_fix_original(component, delta_seconds);
    if (!readable || delta_seconds < 0.0f) {
        return;
    }
    float after = 0.0f;
    if (!SafeRead(accumulator_address, after)) {
        return;
    }
    const float advance = after - before;
    if (advance <= 0.0995f || advance > 0.1005f) {
        return;
    }
    const float corrected = before + 0.1f * (60.0f * delta_seconds);
    if (!SafeWrite(accumulator_address, corrected)) {
        return;
    }
    const unsigned long long correction =
        g_activity_timer_fix_corrections.fetch_add(1, std::memory_order_relaxed) + 1;
    const unsigned long long verbose =
        g_activity_timer_fix_verbose.fetch_add(1, std::memory_order_relaxed);
    if (verbose < 16) {
        log::InfoF(
            "activity_timer_fix correction=%llu dt=%.6f stock_before=%.6f "
            "stock_after=%.6f corrected=%.6f",
            correction,
            delta_seconds,
            before,
            after,
            corrected);
    }
}

void DetourPass8ActivityTimerUpdate(void* component, float delta_seconds) {
    const unsigned long long call =
        g_pass8_timer_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    // The accumulator is the doc-292 activity timer at +0x1E5C; the stock
    // writer refreshes it with a fixed 0.1 per call at 0x3DE144.
    const std::uintptr_t accumulator_address =
        reinterpret_cast<std::uintptr_t>(component) + 0x1E5C;
    float before = 0.0f;
    const bool readable = SafeRead(accumulator_address, before);
    g_pass8_activity_timer_original(component, delta_seconds);
    if (!readable) {
        return;
    }
    float after = 0.0f;
    if (!SafeRead(accumulator_address, after)) {
        return;
    }
    const float advance = after - before;
    if (advance > 1.0e-6f) {
        g_pass8_timer_advanced.fetch_add(1, std::memory_order_relaxed);
    }
    // The frame-counted pulse signature: an advance of ~0.1 regardless of dt.
    if (advance > 0.0995f && advance <= 0.1005f) {
        g_pass8_timer_pulse_advance.fetch_add(1, std::memory_order_relaxed);
    }
    const unsigned long long verbose =
        g_pass8_timer_verbose.fetch_add(1, std::memory_order_relaxed);
    if (verbose < 16) {
        log::InfoF(
            "pass8_activity_timer call=%llu dt=%.6f before=%.6f after=%.6f advance=%.6f",
            call,
            delta_seconds,
            before,
            after,
            advance);
    }
}

void DetourTwitchLoginCallback(int result, std::uintptr_t owner) {
    g_twitch_login_callback_count.fetch_add(1);
    const bool login_failure = result >= 1;
    if (login_failure) {
        g_twitch_login_failure_count.fetch_add(1);
    }

    const int state_before = ReadTwitchState(owner);
    const std::uintptr_t handle_before = ReadTwitchHandle(owner);
    const unsigned int flag_754_before = ReadTwitchFlagByte(owner, 0x754);
    const unsigned int flag_755_before = ReadTwitchFlagByte(owner, 0x755);
    const unsigned int flag_756_before = ReadTwitchFlagByte(owner, 0x756);

    g_twitch_login_callback_original(result, owner);

    const int state_after = ReadTwitchState(owner);
    const std::uintptr_t handle_after = ReadTwitchHandle(owner);
    const unsigned int flag_754_after = ReadTwitchFlagByte(owner, 0x754);
    const unsigned int flag_755_after = ReadTwitchFlagByte(owner, 0x755);
    const unsigned int flag_756_after = ReadTwitchFlagByte(owner, 0x756);

    if (login_failure || owner == 0 || state_before != state_after ||
        ShouldLogVerbose(g_twitch_probe_verbose)) {
        log::WarnF(
            "twitch_login_callback owner=0x%p result=%d failure=%d state_before=%d state_after=%d "
            "handle_before=0x%p handle_after=0x%p flags_before=%u/%u/%u flags_after=%u/%u/%u",
            reinterpret_cast<void*>(owner),
            result,
            login_failure ? 1 : 0,
            state_before,
            state_after,
            reinterpret_cast<void*>(handle_before),
            reinterpret_cast<void*>(handle_after),
            flag_754_before,
            flag_755_before,
            flag_756_before,
            flag_754_after,
            flag_755_after,
            flag_756_after);
    }

    MaybeWriteSummary();
}

bool ApplyOriginalFog(bool force_write) {
    if (!g_config.restore_original_fog || g_volumetric_fog_intensity == 0) {
        return false;
    }
    if (!force_write &&
        g_original_fog_frame_initialized.load(std::memory_order_acquire)) {
        return true;
    }

    float previous_intensity = 0.0f;
    const bool read_previous = SafeRead(g_volumetric_fog_intensity, previous_intensity);
    const bool wrote_original =
        read_previous &&
        (previous_intensity == fog_restoration::kOriginalGameIntensity ||
         SafeWrite(g_volumetric_fog_intensity, fog_restoration::kOriginalGameIntensity));
    float verified_intensity = 0.0f;
    const bool verified =
        wrote_original && SafeRead(g_volumetric_fog_intensity, verified_intensity) &&
        verified_intensity == fog_restoration::kOriginalGameIntensity;
    if (!verified) {
        if (!g_original_fog_failure_logged.exchange(true, std::memory_order_acq_rel)) {
            log::WarnF("original_fog_and_neon_restore apply_failed target_rva=0x%llX",
                       static_cast<unsigned long long>(
                           fog_restoration::kIntensityRva));
        }
        return false;
    }

    if (!g_original_fog_applied.exchange(true, std::memory_order_acq_rel)) {
        log::InfoF("original_fog_and_neon_restore applied=1 previous_intensity=%.3f "
                   "restored_intensity=%.3f target_rva=0x%llX",
                   previous_intensity,
                   verified_intensity,
                   static_cast<unsigned long long>(fog_restoration::kIntensityRva));
    }
    if (!force_write) {
        g_original_fog_frame_initialized.store(true, std::memory_order_release);
    }
    return true;
}

void DetourVolumetricFogSetter(void* script_context) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire) ||
        !g_config.restore_original_fog) {
        g_volumetric_fog_setter_original(script_context);
        return;
    }

    if (!g_original_fog_setter_logged.exchange(true, std::memory_order_acq_rel)) {
        log::Info("original_fog_and_neon_restore blocked_de_volumetric_fog_setter=1");
    }
    (void)ApplyOriginalFog(true);
}

// Forward declaration: the one-shot ActorAudioComponent uid dump is defined with the
// push-voice-line helpers, after DetourFrameFlow.
void LogPlayerComponentUids() noexcept;

// Speed-based controller rumble: intensity in [0,1] recomputed each frame from the
// vehicle's forward speed, consumed by the XInputSetState detour below.
std::atomic<float> g_speed_rumble_intensity{0.0f};
// Freshness: the intensity is only trusted for a short window after the vehicle
// camera path last refreshed it, so leaving the car stops the rumble instead of
// leaving the last value stuck on.
std::atomic<unsigned long long> g_speed_rumble_frame{0};
std::atomic<unsigned long long> g_frame_flow_counter{0};
constexpr unsigned long long kSpeedRumbleStaleFrames = 45;
unsigned short g_game_vibration_left = 0;
unsigned short g_game_vibration_right = 0;
std::atomic<unsigned long long> g_game_vibration_frame{0};
constexpr unsigned long long kGameVibrationFreshFrames = 4;
// Last value this feature actually wrote to the pad, so the falling edge can be
// cleared explicitly. Stopping the writes is not enough: the controller latches
// the last vibration, which is why pausing kept it buzzing in the menu.
unsigned short g_rumble_pushed_left = 0;
unsigned short g_rumble_pushed_right = 0;
// The sampled intensity is only authoritative for a short window; outside it the
// value is stale (car left, game paused) and must not be applied anywhere.
float EffectiveSpeedRumbleIntensity() noexcept {
    const unsigned long long frame =
        g_frame_flow_counter.load(std::memory_order_relaxed);
    const unsigned long long sampled =
        g_speed_rumble_frame.load(std::memory_order_relaxed);
    if (frame > sampled + kSpeedRumbleStaleFrames) {
        return 0.0f;
    }
    return g_speed_rumble_intensity.load(std::memory_order_relaxed);
}

void UpdateSpeedRumbleFromSpeed(float forward_speed_mps) noexcept {
    if (!g_config.speed_rumble) {
        g_speed_rumble_intensity.store(0.0f, std::memory_order_relaxed);
        return;
    }
    const float kmh = std::fabs(forward_speed_mps) * 3.6f;
    const float start = static_cast<float>(g_config.speed_rumble_start_kmh);
    const float full = static_cast<float>(g_config.speed_rumble_full_kmh);
    float factor = 0.0f;
    if (kmh >= full) {
        factor = 1.0f;
    } else if (kmh > start) {
        factor = (kmh - start) / (full - start);
    }
    const float strength =
        static_cast<float>(g_config.speed_rumble_strength_percent) / 100.0f;
    g_speed_rumble_intensity.store(factor * strength, std::memory_order_relaxed);
    g_speed_rumble_frame.store(g_frame_flow_counter.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
}

// Speed rumble injection. XInputSetState is called by the game with its own
// vibration; the detour raises each motor to at least our speed-based value, so the
// game's rumble is never reduced, only reinforced.
struct SpeedRumbleVibration {
    unsigned short wLeftMotorSpeed;
    unsigned short wRightMotorSpeed;
};
using XInputSetStateFn = unsigned long(__stdcall*)(unsigned long, SpeedRumbleVibration*);
XInputSetStateFn g_xinput_set_state_original = nullptr;

// Push the speed rumble every frame. The game only calls XInputSetState when it
// wants rumble, so adding to that call alone gave a lumpy, sometimes-absent
// effect; this keeps it continuous and lets it decay when the car is left.
void PushSpeedRumble() noexcept {
    if (!g_config.speed_rumble || g_xinput_set_state_original == nullptr) {
        return;
    }
    const unsigned long long frame =
        g_frame_flow_counter.load(std::memory_order_relaxed);
    const float intensity = EffectiveSpeedRumbleIntensity();
    unsigned short left = 0;
    unsigned short right = 0;
    const unsigned long long game_frame =
        g_game_vibration_frame.load(std::memory_order_relaxed);
    if (frame <= game_frame + kGameVibrationFreshFrames) {
        left = g_game_vibration_left;
        right = g_game_vibration_right;
    }
    const auto add = static_cast<unsigned short>(intensity * 65535.0f);
    if (left < add) {
        left = add;
    }
    if (right < add) {
        right = add;
    }
    if (left == 0 && right == 0) {
        // Falling edge: the pad keeps whatever was last written, so a rumble that
        // simply stops being pushed would keep buzzing (e.g. in the pause menu).
        // Only a single explicit zero write is needed to release it.
        if (g_rumble_pushed_left != 0 || g_rumble_pushed_right != 0) {
            SpeedRumbleVibration stop{};
            (void)g_xinput_set_state_original(0, &stop);
            g_rumble_pushed_left = 0;
            g_rumble_pushed_right = 0;
        }
        return;
    }
    SpeedRumbleVibration vibration{};
    vibration.wLeftMotorSpeed = left;
    vibration.wRightMotorSpeed = right;
    (void)g_xinput_set_state_original(0, &vibration);
    g_rumble_pushed_left = left;
    g_rumble_pushed_right = right;
}

unsigned long __stdcall DetourXInputSetState(unsigned long index,
                                             SpeedRumbleVibration* vibration) {
    // Pass-through until the hook transaction commits.
    //
    // This detour MUTATES the caller's struct, and Initialize publishes
    // g_behavior_transaction_ready LAST so that no mutating detour can observe
    // half-published feature readiness. The IAT slot goes live during
    // Initialize, well before that store, so without this guard the feature
    // would be reinforcing vibration while the rest of the mod is still
    // installing - breaking the invariant every other mutating detour in this
    // file upholds (see DetourPcFileRead for the same shape).
    if (vibration != nullptr &&
        g_behavior_transaction_ready.load(std::memory_order_acquire) &&
        g_config.speed_rumble) {
        g_game_vibration_left = vibration->wLeftMotorSpeed;
        g_game_vibration_right = vibration->wRightMotorSpeed;
        g_game_vibration_frame.store(
            g_frame_flow_counter.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        const float intensity = EffectiveSpeedRumbleIntensity();
        if (intensity > 0.0f) {
            const auto add = static_cast<unsigned short>(intensity * 65535.0f);
            if (vibration->wLeftMotorSpeed < add) {
                vibration->wLeftMotorSpeed = add;
            }
            if (vibration->wRightMotorSpeed < add) {
                vibration->wRightMotorSpeed = add;
            }
        }
    }
    if (g_xinput_set_state_original != nullptr) {
        return g_xinput_set_state_original(index, vibration);
    }
    return 1167ul;  // ERROR_DEVICE_NOT_CONNECTED
}

// HookTargetGuard only accepts targets INSIDE the game image, so the imported
// XInputSetState cannot be detoured with MinHook. Instead the game's own IAT slot
// for it is rewritten, which is inside the image and is the same mechanism the
// loader would have used anyway.
bool PatchXInputSetStateIat(void* detour, void** original) noexcept {
    if (detour == nullptr || original == nullptr) {
        return false;
    }
    const std::uintptr_t base =
        reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    if (base == 0) {
        return false;
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const auto& dir = nt->OptionalHeader
                          .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) {
        return false;
    }
    auto* descriptor =
        reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        const char* dll = reinterpret_cast<const char*>(base + descriptor->Name);
        bool is_xinput = false;
        for (const char* wanted : {"XINPUT9_1_0.dll", "xinput9_1_0.dll",
                                   "XINPUT1_4.dll", "xinput1_4.dll",
                                   "XINPUT1_3.dll", "xinput1_3.dll"}) {
            const char* a = dll;
            const char* b = wanted;
            while (*a != '\0' && *b != '\0' && *a == *b) {
                ++a;
                ++b;
            }
            if (*a == '\0' && *b == '\0') {
                is_xinput = true;
                break;
            }
        }
        if (!is_xinput) {
            continue;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + (descriptor->OriginalFirstThunk != 0
                        ? descriptor->OriginalFirstThunk
                        : descriptor->FirstThunk));
        auto* thunks =
            reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData != 0 && thunks->u1.Function != 0;
             ++names, ++thunks) {
            if ((names->u1.Ordinal & IMAGE_ORDINAL_FLAG) != 0) {
                continue;
            }
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + names->u1.AddressOfData);
            const char* proc = reinterpret_cast<const char*>(import->Name);
            const char* wanted = "XInputSetState";
            const char* a = proc;
            const char* b = wanted;
            while (*a != '\0' && *b != '\0' && *a == *b) {
                ++a;
                ++b;
            }
            if (*a != '\0' || *b != '\0') {
                continue;
            }
            DWORD old_protect = 0;
            void** slot = reinterpret_cast<void**>(&thunks->u1.Function);
            if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE,
                                 &old_protect) == FALSE) {
                return false;
            }
            *original = *slot;
            *slot = detour;
            ::VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
            return true;
        }
    }
    return false;
}

// Anything a 60 Hz game would call a hitch. The frame that carries the pause
// transition is the one that can deform a skinned prop before the simulation
// stops, so it is worth naming in the log rather than inferring.
constexpr float kLongFrameLogThresholdSeconds = 0.05f;

void DetourFrameFlow(float delta_seconds, void* callback) {
    // Speed rumble. The counter is bumped first so the freshness comparison in
    // EffectiveSpeedRumbleIntensity() sees this frame, and both sit ABOVE the
    // transaction guard: the game only calls XInputSetState when it wants
    // rumble, so a push that stopped during a behaviour transaction would make
    // the effect lumpy and sometimes absent - the exact failure this was
    // written to fix.
    (void)g_frame_flow_counter.fetch_add(1, std::memory_order_relaxed);
    PushSpeedRumble();
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_frame_flow_original(delta_seconds, callback);
        return;
    }
    (void)ApplyOriginalFog(false);
    PollGraphicsDebugKeys();
    MaybeApplyRumbleOverride();
    // One-shot ActorAudioComponent uid discovery. Fires on a frame tick so it needs
    // no gameplay: as soon as the player SimObject exists, dump every component uid.
    // Diagnostic-only, gated on the same diag flag as the rest of the push-line probe,
    // and self-limiting to one dump per process (ledger sec 37).
    if (g_config.restore_push_voice_lines && g_config.diag_push_voice_line_events) {
        LogPlayerComponentUids();
    }


    g_frame_flow_count.fetch_add(1);
    g_last_frame_flow_dt_bits.store(FloatToBits(delta_seconds));

    const char* bucket = ClassifySmallDelta(delta_seconds);
    if (delta_seconds >= 0.0f && delta_seconds < 0.0005f) {
        g_frame_flow_dt_zero_count.fetch_add(1);
    } else if (delta_seconds >= 0.015f && delta_seconds <= 0.019f) {
        g_frame_flow_dt_60hz_count.fetch_add(1);
    } else if (delta_seconds > 0.0f && delta_seconds < 0.2f) {
        g_frame_flow_dt_other_count.fetch_add(1);
    }

    const bool from_cutscene_scope = g_cutscene_flow_owner_scope_depth != 0;
    // UI/simtime queries are SEH-protected but still relatively expensive at
    // high refresh rates. They are only relevant while the cutscene owner is
    // active; ordinary gameplay keeps the fast path free of those calls.
    const CutscenePauseState pause_state =
        from_cutscene_scope ? ReadCutscenePauseStateSafely() : CutscenePauseState{};
    const bool game_paused = pause_state.ui_paused || pause_state.simtime_paused;
    if (from_cutscene_scope && pause_state.ui_paused) {
        ResetTrackedCutsceneTimeline();
    }
    if (from_cutscene_scope) {
        g_frame_flow_from_cutscene_owner_count.fetch_add(1);
    }

    const float tracked_cutscene_input_dt =
        from_cutscene_scope && !pause_state.ui_paused
            ? ResolveTrackedCutsceneBaseDelta(g_cutscene_flow_input_dt,
                                              g_cutscene_cadence_tracker_tls.stable_delta,
                                              g_config.cutscene_fps)
            : g_cutscene_flow_input_dt;

    const CutsceneFrameflowDecision cutscene_decision = ResolveCutsceneFrameflowDelta(
        from_cutscene_scope,
        pause_state,
        g_config.fix_cutscene_zero_dt,
        g_config.cutscene_fps,
        delta_seconds,
        tracked_cutscene_input_dt);
    float forwarded_dt = cutscene_decision.forwarded_dt;
    const bool applied_cutscene_zero_fix = cutscene_decision.applied_zero_dt_fix;
    if (applied_cutscene_zero_fix) {
        g_cutscene_zero_dt_override_count.fetch_add(1);
        if (cutscene_decision.applied_while_game_paused) {
            g_cutscene_zero_dt_pause_override_count.fetch_add(1);
        }
        // The two cutscene repairs are independent and one of them deforms
        // props at high frame rates. Naming each application makes the log
        // say which one is actually rewriting time during a cutscene.
        static std::atomic<int> zero_dt_repair_budget{16};
        int remaining = zero_dt_repair_budget.load(std::memory_order_relaxed);
        while (remaining > 0 &&
               !zero_dt_repair_budget.compare_exchange_weak(
                   remaining, remaining - 1, std::memory_order_relaxed)) {
        }
        if (remaining > 0) {
            log::InfoF("cutscene_zero_dt_repair dt=%.6f applied=%.6f input=%.6f",
                       delta_seconds,
                       forwarded_dt,
                       tracked_cutscene_input_dt);
        }
    }

    // Long frames are the other way a paused cutscene can be damaged, and the
    // opposite of "time keeps running": the pause transition stalls the game
    // thread, the next frame carries the whole stall, and a cloth or chain
    // solver that steps once with that delta overshoots its constraints. The
    // world does not simulate while paused, so the deformation simply stays on
    // screen. Log the spikes that matter instead of inferring them.
    if (delta_seconds >= kLongFrameLogThresholdSeconds) {
        static std::atomic<int> long_frame_budget{32};
        int remaining = long_frame_budget.load(std::memory_order_relaxed);
        while (remaining > 0 &&
               !long_frame_budget.compare_exchange_weak(
                   remaining, remaining - 1, std::memory_order_relaxed)) {
        }
        if (remaining > 0) {
            log::InfoF(
                "frame_flow_long_frame dt=%.4f bucket=%s cutscene=%d ui_paused=%d "
                "simtime_paused=%d forwarded=%.4f",
                delta_seconds,
                bucket,
                from_cutscene_scope ? 1 : 0,
                pause_state.ui_paused ? 1 : 0,
                pause_state.simtime_paused ? 1 : 0,
                forwarded_dt);
        }
    }

    // The frame that carries the pause transition is the one that matters: it
    // is the last frame the simulation ever sees before it stops, so a bad
    // delta there is baked into whatever the pause freezes. cutscene_pause_state
    // says when the flags flipped; this says what the frame looked like.
    {
        const int pause_bits =
            (pause_state.ui_paused ? 1 : 0) | (pause_state.simtime_paused ? 2 : 0);
        const bool changed = pause_bits != g_last_frame_flow_pause_bits_tls;
        g_last_frame_flow_pause_bits_tls = pause_bits;
        if (changed && from_cutscene_scope) {
            static std::atomic<int> transition_budget{16};
            int remaining = transition_budget.load(std::memory_order_relaxed);
            while (remaining > 0 &&
                   !transition_budget.compare_exchange_weak(
                       remaining, remaining - 1, std::memory_order_relaxed)) {
            }
            if (remaining > 0) {
                log::InfoF(
                    "frame_flow_pause_transition ui_paused=%d simtime_paused=%d "
                    "dt=%.4f bucket=%s forwarded=%.4f",
                    pause_state.ui_paused ? 1 : 0,
                    pause_state.simtime_paused ? 1 : 0,
                    delta_seconds,
                    bucket,
                    forwarded_dt);
            }
        }
    }

    if (from_cutscene_scope) {
        g_cutscene_flow_forwarded = true;
        g_cutscene_flow_forwarded_dt = forwarded_dt;
        g_current_cutscene_dt = forwarded_dt;
        g_cutscene_flow_forwarded_callback = callback;
    }

    if (((from_cutscene_scope && applied_cutscene_zero_fix) ||
         (delta_seconds >= 0.0f && delta_seconds < 0.0005f)) &&
        ShouldLogVerbose(g_frame_flow_verbose)) {
        log::InfoF(
            "frame_flow dt=%.6f applied_dt=%.6f bucket=%s callback=0x%p callback_rva=0x%llX "
            "from_cutscene=%d paused=%d ui_paused=%d simtime_paused=%d "
            "cutscene_fix=%d cutscene_fix_paused=%d cutscene_fix_simtime=%d cutscene_input_dt=%.6f",
            delta_seconds,
            forwarded_dt,
            bucket,
            callback,
            static_cast<unsigned long long>(ToRva(reinterpret_cast<std::uintptr_t>(callback))),
            from_cutscene_scope ? 1 : 0,
            game_paused ? 1 : 0,
            pause_state.ui_paused ? 1 : 0,
            pause_state.simtime_paused ? 1 : 0,
            applied_cutscene_zero_fix ? 1 : 0,
            cutscene_decision.applied_while_game_paused ? 1 : 0,
            cutscene_decision.applied_while_simtime_paused ? 1 : 0,
            tracked_cutscene_input_dt);
    }

    g_frame_flow_original(forwarded_dt, callback);
    cut_content::OnGameThreadFrame();
    MaybeWriteSummary();
}

void DetourCutsceneFlowOwner(void* owner, float delta_seconds) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_cutscene_flow_owner_original(owner, delta_seconds);
        return;
    }
    g_cutscene_flow_owner_count.fetch_add(1);
    g_last_cutscene_flow_input_dt_bits.store(FloatToBits(delta_seconds));

    if (delta_seconds >= 0.0f && delta_seconds < 0.0005f) {
        g_cutscene_flow_owner_dt_zero_count.fetch_add(1);
    }

    const auto owner_ptr = reinterpret_cast<std::uintptr_t>(owner);
    const int stage_before = ReadCutsceneFlowStage(owner_ptr);
    const unsigned int flags_before = ReadCutsceneFlowFlags(owner_ptr);

    const unsigned int depth_before = g_cutscene_flow_owner_scope_depth++;
    const bool outermost = depth_before == 0;
    const bool have_timing_slot = depth_before < g_cutscene_owner_timing_stack.size();
    const bool new_scope = outermost && g_last_cutscene_owner_tls != owner_ptr;
    if (new_scope) {
        g_last_cutscene_owner_tls = owner_ptr;
        ResetTrackedCutsceneTimeline();
        // Fresh budget per cutscene: the cadence evidence must cover every
        // scene, not just the first one after boot.
        g_cutscene_owner_cadence_budget.store(32, std::memory_order_relaxed);
    }
    if (outermost) {
        TrackCutsceneCadence(delta_seconds);
        // Director-starvation probe: the game thread keeps rendering while the
        // cutscene director goes uncalled (2026-10-31 live session: a 3.98 s
        // hole mid-scene with the frame counter advancing at exactly 60 fps).
        // Quantify every such hole so the next repair targets a measured
        // mechanism instead of a guess.
        static std::atomic<unsigned long long> s_last_owner_tick{0};
        static std::atomic<unsigned long long> s_last_gap_log_tick{0};
        const unsigned long long owner_now = GetTickCount64();
        const unsigned long long owner_last =
            s_last_owner_tick.exchange(owner_now, std::memory_order_relaxed);
        if (owner_last != 0 && owner_now - owner_last > 250) {
            const unsigned long long silence = owner_now - owner_last;
            unsigned long long gap_last =
                s_last_gap_log_tick.load(std::memory_order_relaxed);
            if (owner_now - gap_last >= 5000 &&
                s_last_gap_log_tick.compare_exchange_strong(
                    gap_last, owner_now, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                log::WarnF("cutscene_director_gap silence=%.2fs input_dt=%.6f "
                           "same_owner=%d",
                           static_cast<double>(silence) / 1000.0,
                           delta_seconds,
                           new_scope ? 0 : 1);
            }
        }
    }

    // The real cadence the engine feeds its cutscene director. Every attempt to
    // smooth cutscene motion has been designed against a guess at this pattern,
    // and the guesses disagree on whether a repair is even safe. Thirty samples
    // per cutscene settle it: zero frames, render-sized frames, or full 30 Hz
    // ticks.
    {
        int remaining = g_cutscene_owner_cadence_budget.load(std::memory_order_relaxed);
        while (remaining > 0 &&
               !g_cutscene_owner_cadence_budget.compare_exchange_weak(
                   remaining, remaining - 1, std::memory_order_relaxed)) {
        }
        if (remaining > 0) {
            log::InfoF("cutscene_owner_dt dt=%.6f bucket=%s",
                       delta_seconds,
                       ClassifySmallDelta(delta_seconds));
        }
    }
    if (have_timing_slot) {
        g_cutscene_owner_timing_stack[depth_before] = CutsceneOwnerTimingState{
            g_cutscene_flow_input_dt,
            g_current_cutscene_dt,
            g_cutscene_flow_forwarded,
            g_cutscene_flow_forwarded_dt,
            g_cutscene_flow_forwarded_callback};
    }
    if (outermost) {
        g_cutscene_flow_forwarded = false;
        g_cutscene_flow_forwarded_dt = 0.0f;
        g_cutscene_flow_forwarded_callback = nullptr;
        g_last_cutscene_flow_completed_dt_tls = 0.0f;
        // Do not let an early NIS call in a new scope consume the previous
        // owner's completed frame.
        g_last_cutscene_flow_forwarded_dt_bits.store(0);
    } else if (have_timing_slot) {
        // Isolate nested owner state.  Its result is merged into the parent
        // only after the nested original returns.
        g_cutscene_flow_forwarded = false;
        g_cutscene_flow_forwarded_dt = 0.0f;
        g_cutscene_flow_forwarded_callback = nullptr;
    }
    // Each owner invocation has its own live cadence.  The previous code kept
    // the outer owner's value during re-entry, which made nested NIS/frameflow
    // calls consume a stale frame delta.
    g_cutscene_flow_input_dt = delta_seconds;
    g_current_cutscene_dt = 0.0f;

    bool forwarded = false;
    float forwarded_dt = 0.0f;
    void* forwarded_callback = nullptr;
    __try {
        g_cutscene_flow_owner_original(owner, delta_seconds);
    } __finally {
        forwarded = g_cutscene_flow_forwarded;
        forwarded_dt = g_cutscene_flow_forwarded_dt;
        forwarded_callback = g_cutscene_flow_forwarded_callback;

        if (g_cutscene_flow_owner_scope_depth != 0) {
            --g_cutscene_flow_owner_scope_depth;
        }
        if (have_timing_slot) {
            const CutsceneOwnerTimingState previous = g_cutscene_owner_timing_stack[depth_before];
            g_cutscene_flow_input_dt = previous.input_dt;
            g_current_cutscene_dt = previous.current_dt;
            g_cutscene_flow_forwarded = previous.forwarded;
            g_cutscene_flow_forwarded_dt = previous.forwarded_dt;
            g_cutscene_flow_forwarded_callback = previous.forwarded_callback;

            if (!outermost && forwarded) {
                g_cutscene_flow_forwarded = true;
                g_cutscene_flow_forwarded_dt = forwarded_dt;
                g_cutscene_flow_forwarded_callback = forwarded_callback;
            }
        } else {
            // Defensive overflow path: keep the detour scope balanced and do
            // not leave a nested cadence active indefinitely.
            g_cutscene_flow_input_dt = 0.0f;
            g_current_cutscene_dt = 0.0f;
            g_cutscene_flow_forwarded = false;
            g_cutscene_flow_forwarded_dt = 0.0f;
            g_cutscene_flow_forwarded_callback = nullptr;
        }
        if (g_cutscene_flow_owner_scope_depth == 0) {
            g_last_cutscene_flow_completed_dt_tls = outermost && forwarded ? forwarded_dt : 0.0f;
            g_cutscene_flow_forwarded = false;
            g_cutscene_flow_forwarded_dt = 0.0f;
            g_cutscene_flow_forwarded_callback = nullptr;
            g_cutscene_flow_input_dt = 0.0f;
            g_current_cutscene_dt = 0.0f;
        }
    }

    const int stage_after = ReadCutsceneFlowStage(owner_ptr);
    const unsigned int flags_after = ReadCutsceneFlowFlags(owner_ptr);

    if (outermost) {
        // Publish zero as well as non-zero values; retaining the previous
        // frame here made NIS corrections fire during a paused/outro frame.
        g_last_cutscene_flow_forwarded_dt_bits.store(FloatToBits(forwarded ? forwarded_dt : 0.0f));
    }
    if (outermost && forwarded) {
        if (forwarded_dt >= 0.0f && forwarded_dt < 0.0005f) {
            g_cutscene_flow_owner_forwarded_zero_count.fetch_add(1);
        } else if (forwarded_dt >= 0.015f && forwarded_dt <= 0.019f) {
            g_cutscene_flow_owner_forwarded_60hz_count.fetch_add(1);
        } else {
            g_cutscene_flow_owner_forwarded_other_count.fetch_add(1);
        }
    }

    const bool verbose_cutscene_flow = ShouldLogVerbose(g_cutscene_flow_owner_verbose);
    if (verbose_cutscene_flow) {
        log::InfoF(
            "cutscene_flow_owner owner=0x%p input_dt=%.6f forwarded=%d forwarded_dt=%.6f "
            "forwarded_callback=0x%p forwarded_rva=0x%llX stage_before=%d stage_after=%d "
            "flags_before=0x%X flags_after=0x%X",
            owner,
            delta_seconds,
            outermost && forwarded ? 1 : 0,
            outermost && forwarded ? forwarded_dt : 0.0f,
            outermost ? forwarded_callback : nullptr,
            static_cast<unsigned long long>(
                ToRva(reinterpret_cast<std::uintptr_t>(outermost ? forwarded_callback : nullptr))),
            stage_before,
            stage_after,
            flags_before,
            flags_after);
    }

    MaybeWriteSummary();
}

void DetourFogSlicingMode(void* time_of_day_manager, int update_interval) {
    // The stock setter stores the interval and immediately divides by
    // (interval * 8).  MinHook can expose this detour briefly while the
    // bootstrap transaction is still committing, so the not-ready fast path
    // must receive the same divisor guard as the normal path.
    const int clamped_interval =
        ClampFogSlicingInterval(update_interval, g_config.min_fog_slicing_interval);
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_fog_slicing_mode_original(time_of_day_manager, clamped_interval);
        return;
    }
    g_fog_slicing_mode_count.fetch_add(1);

    if (clamped_interval != update_interval) {
        g_fog_slicing_clamp_count.fetch_add(1);
        bool expected = false;
        if (g_fog_clamp_warning_emitted.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            log::WarnF(
                "fog_slicing_clamp manager=0x%p requested=%d applied=%d (further occurrences suppressed)",
                time_of_day_manager,
                update_interval,
                clamped_interval);
        }
    } else if (ShouldLogVerbose(g_fog_slicing_verbose)) {
        log::InfoF("fog_slicing_mode manager=0x%p interval=%d",
                   time_of_day_manager,
                   clamped_interval);
    }

    g_fog_slicing_mode_original(time_of_day_manager, clamped_interval);
    MaybeWriteSummary();
}

void DetourCharacterHandleWaterCollision(void* character_effects_component,
                                         const void* mat,
                                         const void* character_velocity) {
    g_character_water_collision_count.fetch_add(1);
    const float speed = ReadVector3Magnitude(character_velocity);
    g_last_character_wet_component.store(reinterpret_cast<std::uintptr_t>(character_effects_component));
    g_last_character_water_speed_bits.store(FloatToBits(speed));

    if (g_behavior_transaction_ready.load(std::memory_order_acquire) &&
        g_config.restore_character_wetness && character_effects_component != nullptr) {
        unsigned char is_attached_to_player = 0;
        const bool attachment_read = SafeRead(
            reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x1A0,
            is_attached_to_player);
        if (attachment_read && is_attached_to_player != 0) {
            g_player_water_collision_component.store(
                reinterpret_cast<std::uintptr_t>(character_effects_component),
                std::memory_order_release);
            g_player_water_collision_generation.fetch_add(1, std::memory_order_acq_rel);
        } else if (attachment_read) {
            MarkNpcCharacterWaterContact(
                reinterpret_cast<std::uintptr_t>(character_effects_component));
        }
    }

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_water_collision component=0x%p speed=%.3f velocity=0x%p mat=0x%p",
                   character_effects_component,
                   speed,
                   character_velocity,
                   mat);
    }

    g_character_handle_water_collision_original(character_effects_component, mat, character_velocity);
    MaybeWriteSummary();
}

void MaybePollSlowMoScale() noexcept;

void DetourCharacterEffectsUpdate(void* character_effects_component, float delta_seconds) {
    g_character_effects_update_count.fetch_add(1);
    // Per-frame slow-motion poll. The 2026-10-02 stunt-slice restore ported
    // MaybePollSlowMoScale and the time-dilate detour but dropped this call
    // site that Hooks.cpp.restored-fixed carried, which would have left the
    // suppression clamp and the probe blind on every frame where the engine
    // does not call the time-dilate apply point.
    MaybePollSlowMoScale();

    g_character_effects_update_original(character_effects_component, delta_seconds);

    const bool behavior_ready =
        g_behavior_transaction_ready.load(std::memory_order_acquire);
    const std::uintptr_t component_ptr =
        reinterpret_cast<std::uintptr_t>(character_effects_component);
    const std::uintptr_t tracked_wet_component = g_last_character_wet_component.load();
    const bool is_tracked_wet_component =
        component_ptr != 0 && tracked_wet_component != 0 && component_ptr == tracked_wet_component;

    const bool runtime_effects_requested =
        behavior_ready &&
        (g_config.restore_character_wetness || g_config.restore_character_sweat);
    bool diagnostic_fields_requested =
        is_tracked_wet_component || g_config.summary_interval_ms != 0;
#if !defined(SPATCH_FINAL_RELEASE)
    diagnostic_fields_requested = diagnostic_fields_requested ||
        g_config.max_verbose_events != 0 ||
        g_character_surface_probe.load(std::memory_order_relaxed);
#endif
    if (!runtime_effects_requested && !diagnostic_fields_requested) {
        MaybeWriteSummary();
        return;
    }

    unsigned int active_surface_uid = 0;
    unsigned int active_wet_surface_uid = 0;
    unsigned char is_on_fire = 0;
    unsigned char is_smoldering = 0;
    unsigned char is_attached_to_player = 0;
    float fire_extinguish_time = 0.0f;
    float smolder_extinguish_time = 0.0f;
    float queued_health_damage = 0.0f;
    int wet_gate_counter = 0;
    float timeofday_weather_state = 0.0f;
    float timeofday_surface_wetness = 0.0f;
    float timeofday_override_surface_wetness = -1.0f;
    bool attachment_read = false;

    if (character_effects_component != nullptr) {
        attachment_read = SafeRead(
            reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x1A0,
            is_attached_to_player);
        if (diagnostic_fields_requested) {
            SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x164,
                     active_surface_uid);
            SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x168,
                     active_wet_surface_uid);
            SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x16C,
                     is_on_fire);
            SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x17C,
                     is_smoldering);
            SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x18C,
                     fire_extinguish_time);
            SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x190,
                     smolder_extinguish_time);
            SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x194,
                     queued_health_damage);
        } else if (behavior_ready && g_config.restore_character_wetness &&
                   attachment_read && is_attached_to_player == 0) {
            // NPC wetness needs only the active wet-surface UID. Avoid seven
            // unrelated diagnostic reads on every visible character.
            SafeRead(reinterpret_cast<std::uintptr_t>(character_effects_component) + 0x168,
                     active_wet_surface_uid);
        }
    }

    if (diagnostic_fields_requested && g_wet_surface_block_counter != 0) {
        SafeRead(g_wet_surface_block_counter, wet_gate_counter);
    }

    const bool weather_requested = diagnostic_fields_requested ||
        (behavior_ready && g_config.restore_character_wetness && attachment_read);
    const std::uintptr_t time_of_day_manager =
        weather_requested ? ReadTimeOfDayManagerInstance() : 0;
    if (weather_requested && time_of_day_manager != 0) {
        // TimeOfDayManager::m_WeatherState is at +0x34.  The actual surface
        // wetness values consumed by rendering are m_WeatherSurfaceWetness at
        // +0x48 and m_OverrideSurfaceWetness at +0x74.
        SafeRead(time_of_day_manager + 0x34, timeofday_weather_state);
        SafeRead(time_of_day_manager + 0x48, timeofday_surface_wetness);
        SafeRead(time_of_day_manager + 0x74, timeofday_override_surface_wetness);
    }

    const bool apply_wetness_restore = behavior_ready &&
                                       g_config.restore_character_wetness &&
                                       attachment_read &&
                                       is_attached_to_player != 0;
    // Learn the player SimObject from the engine attached-to-player flag so
    // push-voice-line detection can identify player crowd contact (ledger sec 33).
    if (attachment_read && is_attached_to_player != 0) {
        std::uintptr_t attached_sim = 0;
        __try {
            attached_sim = ReadSimObjectFromComponent(character_effects_component);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            attached_sim = 0;
        }
        if (attached_sim != 0) {
            g_player_sim_object_from_effects.store(attached_sim, std::memory_order_release);
        }
    }
    if (apply_wetness_restore) {
        // Spawn/teardown bursts (fight-club pre-fight population) recycle
        // pooled components between ticks. Every read/write inside is already
        // SEH-guarded; this boundary degrades an unforeseen fault in
        // SPatch-owned apply work to a skipped tick instead of a crash. The
        // engine update above already ran and is never affected by this guard.
        __try {
            ApplyCharacterWetnessRestore(character_effects_component,
                                         delta_seconds,
                                         timeofday_weather_state,
                                         timeofday_surface_wetness,
                                         timeofday_override_surface_wetness);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    if (behavior_ready && attachment_read && is_attached_to_player == 0 &&
        (g_config.restore_character_wetness || g_config.restore_character_sweat)) {
        __try {
            ApplyNpcCharacterEffectsRestore(character_effects_component,
                                            delta_seconds,
                                            active_wet_surface_uid,
                                            timeofday_weather_state,
                                            timeofday_surface_wetness,
                                            timeofday_override_surface_wetness);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    [[maybe_unused]] CharacterSweatDecisionSnapshot player_sweat_snapshot{};
    if (behavior_ready && attachment_read && g_config.restore_character_sweat &&
        is_attached_to_player != 0) {
        __try {
            player_sweat_snapshot = ApplyCharacterSweatRestore(
                character_effects_component, delta_seconds, is_attached_to_player != 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

#if !defined(SPATCH_FINAL_RELEASE)
    if (g_character_surface_probe.load(std::memory_order_relaxed) &&
        is_attached_to_player != 0 && component_ptr != 0) {
        std::uintptr_t sim_object = 0;
        std::uintptr_t character_look_component = 0;
        __try {
            sim_object = ReadSimObjectFromComponent(character_effects_component);
            character_look_component = ResolveSimObjectComponent(sim_object, 0xCC000001u);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            sim_object = 0;
            character_look_component = 0;
        }

        std::uintptr_t render_data = 0;
        if (character_look_component != 0) {
            SafeRead(character_look_component + 0x78, render_data);
        }

        const unsigned long long now = GetTickCount64();
        unsigned long long last =
            g_character_surface_probe_last_log_tick.load(std::memory_order_relaxed);
        if (now - last >= 1000 &&
            g_character_surface_probe_last_log_tick.compare_exchange_strong(
                last, now, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            std::array<std::uint8_t, 0xC0> character_bytes{};
            std::array<std::uint8_t, 0x40> render_bytes{};
            const bool component_valid = SafeCopyBytes(
                character_look_component, character_bytes.data(), character_bytes.size());
            const bool render_valid =
                SafeCopyBytes(render_data, render_bytes.data(), render_bytes.size());

            const auto live_snapshot = character_surface::CaptureSnapshot(
                component_valid ? character_bytes.data() : nullptr,
                component_valid ? character_bytes.size() : 0,
                render_valid ? render_bytes.data() : nullptr,
                render_valid ? render_bytes.size() : 0,
                render_valid ? render_bytes.data() : nullptr,
                render_valid ? render_bytes.size() : 0);
            const unsigned long long water_generation =
                g_player_water_collision_generation.load(std::memory_order_acquire);
            const unsigned long long observed_water_generation =
                g_character_surface_observed_water_generation.exchange(
                    water_generation, std::memory_order_acq_rel);
            const bool water_contact = character_surface::IsNewMatchingWaterContact(
                water_generation,
                observed_water_generation,
                g_player_water_collision_component.load(std::memory_order_acquire),
                component_ptr);
            log::InfoF(
                "character_surface scope=player component_valid=%u render_valid=%u copied_valid=0 copied_causal=0 finite=%u decision_sampled=%u velocity_valid=%u speed=%.9g run_speed_threshold=%.9g combat_valid=%u combat=%u vehicle_state_valid=%u in_vehicle=%u velocity_candidate=%u exerting=%u weather=%.3f wet_surface=%u wet_surface_uid=0x%08X water_generation=%llu water_contact=%u component_wetness=%.3f component_sweat=%.3f staged_wetness=%.3f staged_sweat=%.3f consumer_causal=0 consumer_wetness=%.3f consumer_sweat=%.3f copied_wetness=0.000 copied_sweat=0.000",
                component_valid ? 1u : 0u,
                render_valid ? 1u : 0u,
                live_snapshot.all_finite() ? 1u : 0u,
                player_sweat_snapshot.sampled ? 1u : 0u,
                player_sweat_snapshot.velocity_valid ? 1u : 0u,
                player_sweat_snapshot.horizontal_speed,
                player_sweat_snapshot.run_speed_threshold,
                player_sweat_snapshot.combat_valid ? 1u : 0u,
                player_sweat_snapshot.in_combat ? 1u : 0u,
                player_sweat_snapshot.vehicle_state_valid ? 1u : 0u,
                player_sweat_snapshot.in_vehicle ? 1u : 0u,
                player_sweat_snapshot.decision.velocity_candidate ? 1u : 0u,
                player_sweat_snapshot.decision.exerting ? 1u : 0u,
                timeofday_surface_wetness,
                active_wet_surface_uid != 0 ? 1u : 0u,
                active_wet_surface_uid,
                water_generation,
                water_contact ? 1u : 0u,
                live_snapshot.component_wetness.value,
                live_snapshot.component_sweat.value,
                live_snapshot.staged_wetness.value,
                live_snapshot.staged_sweat.value,
                live_snapshot.prebridge_slot_38.value,
                live_snapshot.prebridge_slot_3c.value);
        }
    }
#endif

    if (is_tracked_wet_component) {
        g_last_character_active_surface_uid.store(active_surface_uid);
        g_last_character_active_wet_surface_uid.store(active_wet_surface_uid);
        g_last_character_wet_gate_counter.store(wet_gate_counter);
        g_last_character_is_on_fire.store(is_on_fire != 0 ? 1u : 0u);
        g_last_character_is_smoldering.store(is_smoldering != 0 ? 1u : 0u);
        g_last_character_is_attached_to_player.store(is_attached_to_player != 0 ? 1u : 0u);
        g_last_character_fire_extinguish_bits.store(FloatToBits(fire_extinguish_time));
        g_last_character_smolder_extinguish_bits.store(FloatToBits(smolder_extinguish_time));
        g_last_character_queued_health_damage_bits.store(FloatToBits(queued_health_damage));
        g_last_character_timeofday_weather_bits.store(FloatToBits(timeofday_surface_wetness));
        g_last_character_timeofday_override_bits.store(
            FloatToBits(timeofday_override_surface_wetness));
    }

    if (is_tracked_wet_component && active_wet_surface_uid != 0) {
        g_character_effects_wet_surface_count.fetch_add(1);
    }

    if (ShouldLogVerbose(g_character_regression_verbose) &&
        (active_surface_uid != 0 || active_wet_surface_uid != 0 || is_on_fire != 0 ||
         is_smoldering != 0 || wet_gate_counter != 0 ||
         !NearlyEqual(timeofday_surface_wetness, 0.0f) ||
         !NearlyEqual(timeofday_override_surface_wetness, -1.0f))) {
        log::InfoF(
            "character_effects_update component=0x%p dt=%.3f surface=0x%08X wet_surface=0x%08X wet_gate=%d tod_surface_wetness=%.3f tod_override_surface_wetness=%.3f on_fire=%u smolder=%u attached=%u fire_time=%.3f smolder_time=%.3f queued_damage=%.3f",
            character_effects_component,
            delta_seconds,
            active_surface_uid,
            active_wet_surface_uid,
            wet_gate_counter,
            timeofday_surface_wetness,
            timeofday_override_surface_wetness,
            is_on_fire != 0 ? 1u : 0u,
            is_smoldering != 0 ? 1u : 0u,
            is_attached_to_player != 0 ? 1u : 0u,
            fire_extinguish_time,
            smolder_extinguish_time,
            queued_health_damage);
    }

    MaybeWriteSummary();
}

// Forward declaration: the push-voice-line bump handler is defined after the
// speech helpers it depends on (player singleton, audio component, invocation).
void MaybeFirePushVoiceLineOnBump(void* attacker, int damage, bool projectile_damage);
// Forward declaration: the restored Wei combat-line handler runs on the same
// detour. It speaks through a voice's own audio component, so it receives the
// health component (rcx) to resolve the victim when the all-voices key is on,
// and is gated by restore_combat_lines.
void MaybeFireWeiVoiceLineOnDamage(void* health_component,
                                   void* attacker,
                                   int damage,
                                   bool projectile_damage);
// Forward declaration: the restored NPC voice-line handler (police +
// encouragement families) also runs on this detour, speaking through the
// VICTIM's own audio component, so it receives the health component too.
void MaybeFireNpcVoiceLineOnDamage(void* health_component,
                                   void* attacker,
                                   int damage,
                                   bool projectile_damage);

bool DetourHealthApplyDamage(void* health_component,
                             int damage,
                             void* attacker,
                             void* hit_record,
                             bool projectile_damage) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_health_apply_damage_original(
            health_component, damage, attacker, hit_record, projectile_damage);
    }

    MaybeFirePushVoiceLineOnBump(attacker, damage, projectile_damage);
    MaybeFireWeiVoiceLineOnDamage(health_component, attacker, damage,
                                   projectile_damage);
    // NPC voice lines speak through the VICTIM's own component, so the
    // handler needs the health component this detour was handed (rcx).
    MaybeFireNpcVoiceLineOnDamage(
        health_component, attacker, damage, projectile_damage);
    // Sweat-only installs this detour solely as an NPC melee-combat signal.
    // Keep animation, hit-react, and trace diagnostics off this path.
    if (g_sweat_only_health_fast_path) {
        if (!projectile_damage && damage > 0) {
            std::uintptr_t sim_object = 0;
            __try {
                sim_object = ReadSimObjectFromComponent(health_component);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                sim_object = 0;
            }
            MarkCharacterCombatActivity(sim_object);
        }
        return g_health_apply_damage_original(
            health_component, damage, attacker, hit_record, projectile_damage);
    }
    g_character_health_apply_damage_count.fetch_add(1);
    if (projectile_damage) {
        g_character_health_apply_projectile_count.fetch_add(1);
    } else {
        g_character_health_apply_melee_count.fetch_add(1);
    }

    g_last_character_health_component.store(reinterpret_cast<std::uintptr_t>(health_component));
    g_last_character_health_attacker.store(reinterpret_cast<std::uintptr_t>(attacker));
    g_last_character_health_hit_record.store(reinterpret_cast<std::uintptr_t>(hit_record));
    g_last_character_health_damage.store(damage);
    g_last_character_health_projectile.store(projectile_damage ? 1u : 0u);

    std::uintptr_t sim_object = 0;
    std::uintptr_t anim_component = 0;
    std::uintptr_t hitreact_component = 0;
    __try {
        sim_object = ReadSimObjectFromComponent(health_component);
        anim_component = ResolveSimObjectComponent(sim_object, 0xC6000003u);
        hitreact_component = ResolveSimObjectComponent(sim_object, 0xA8000001u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sim_object = 0;
        anim_component = 0;
        hitreact_component = 0;
    }
    if (!projectile_damage && damage > 0) {
        // The target's health callback is the only build-stable combat signal
        // available for NPCs. Keep a short activity window so stationary
        // attack/hit animations still build sweat without guessing from
        // arbitrary animation memory.
        MarkCharacterCombatActivity(sim_object);
    }
    g_last_character_health_anim_component.store(anim_component);
    g_last_character_health_hitreact_component.store(hitreact_component);
    if (anim_component != 0) {
        g_character_health_anim_found_count.fetch_add(1);
    }
    if (hitreact_component != 0) {
        g_character_health_hitreact_found_count.fetch_add(1);
    }

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF(
            "character_health_apply component=0x%p damage=%d projectile=%d attacker=0x%p hit=0x%p sim_object=0x%p anim=0x%p hitreact=0x%p",
            health_component,
            damage,
            projectile_damage ? 1 : 0,
            attacker,
            hit_record,
            reinterpret_cast<void*>(sim_object),
            reinterpret_cast<void*>(anim_component),
            reinterpret_cast<void*>(hitreact_component));
    }

    AppendCharacterTraceLine("enter",
                             health_component,
                             reinterpret_cast<void*>(anim_component),
                             reinterpret_cast<void*>(hitreact_component),
                             damage,
                             projectile_damage,
                             false);

    const bool result = g_health_apply_damage_original(
        health_component, damage, attacker, hit_record, projectile_damage);
    AppendCharacterTraceLine("after_orig",
                             health_component,
                             reinterpret_cast<void*>(anim_component),
                             reinterpret_cast<void*>(hitreact_component),
                             damage,
                             projectile_damage,
                             result);

    MaybeWriteSummary();
    return result;
}

void DetourCharacterAnimationCreateDamageRig(void* character_animation_component) {
    g_character_damage_rig_create_count.fetch_add(1);

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_damage_rig_create component=0x%p", character_animation_component);
    }

    MaybeWriteSummary();
    g_character_animation_create_damage_rig_original(character_animation_component);
}

void DetourCharacterDamageRigResetDamage(void* damage_rig) {
    g_character_damage_rig_reset_count.fetch_add(1);
    g_last_character_damage_rig.store(reinterpret_cast<std::uintptr_t>(damage_rig));

    if (ShouldLogVerbose(g_character_regression_verbose)) {
        log::InfoF("character_damage_rig_reset rig=0x%p", damage_rig);
    }

    MaybeWriteSummary();
    g_character_damage_rig_reset_damage_original(damage_rig);
}

void DetourAntiAliasOwner(std::uintptr_t render_context,
                          std::uintptr_t param_2,
                          std::uintptr_t param_3,
                          std::uintptr_t* param_4,
                          std::uintptr_t* param_5) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_antialias_owner_original(render_context, param_2, param_3, param_4, param_5);
        return;
    }
    if (g_config.hook_smaa_present) {
        smaa::MaybeInstallHooks();
    }

    g_aa_owner_count.fetch_add(1);

    if (g_config.hook_smaa_present && g_config.smaa_disable_stock_aa &&
        smaa::CanReplaceStockAa()) {
        g_aa_skip_count.fetch_add(1);
        g_last_aa_state_gate.store(0);
        g_last_aa_hair_blur_gate.store(0);
        g_last_aa_variant_mode.store(g_aa_runtime_variant_mode.load());
        g_last_aa_material.store(0);
        g_last_aa_target.store(0);
        g_last_aa_source_a.store(0);
        g_last_aa_source_b.store(0);
        g_last_aa_shader_uid.store(0);
        g_last_aa_raster_uid.store(0);
        g_last_aa_aux_uid.store(0);
        MaybeWriteSummary();
        return;
    }

    const int variant_mode = ClampAaVariantMode(g_aa_runtime_variant_mode.load());
    int aa_state_gate = 0;
    int hair_blur_gate = 0;
    int branch_gate = 0;
    std::uintptr_t aa_material = 0;
    std::uintptr_t aa_target = 0;
    std::uintptr_t source_a = 0;
    std::uintptr_t source_b = 0;

    SafeRead(render_context + 0x49C, aa_state_gate);
    SafeRead(render_context + 0x148, hair_blur_gate);
    SafeRead(render_context + 0x178, branch_gate);
    SafeRead(render_context + 0x330, aa_material);
    SafeRead(render_context + 0x338, aa_target);
    if (param_4 != nullptr) {
        SafeRead(reinterpret_cast<std::uintptr_t>(param_4), source_a);
    }
    if (param_5 != nullptr) {
        SafeRead(reinterpret_cast<std::uintptr_t>(param_5), source_b);
    }

    g_last_aa_state_gate.store(aa_state_gate);
    g_last_aa_hair_blur_gate.store(hair_blur_gate);
    g_last_aa_variant_mode.store(variant_mode);
    g_last_aa_material.store(aa_material);
    g_last_aa_target.store(aa_target);
    g_last_aa_source_a.store(source_a);
    g_last_aa_source_b.store(source_b);

    if (aa_state_gate == 1) {
        g_aa_main_count.fetch_add(1);
    } else {
        g_aa_skip_count.fetch_add(1);
    }

    if (hair_blur_gate == 0) {
        g_aa_hair_blur_zero_count.fetch_add(1);
    }

    bool variant_applied = false;
    bool wrote_hair_gate = false;
    bool wrote_branch_gate = false;
    if (variant_mode != 0 && render_context != 0) {
        const int forced_hair_blur_gate = 1;
        const int forced_branch_gate = variant_mode == 2 ? 1 : 0;
        const bool hair_gate_needs_write = hair_blur_gate != forced_hair_blur_gate;
        const bool branch_gate_needs_write = branch_gate != forced_branch_gate;
        const bool hair_gate_ready =
            !hair_gate_needs_write || SafeWrite(render_context + 0x148, forced_hair_blur_gate);
        const bool branch_gate_ready =
            hair_gate_ready &&
            (!branch_gate_needs_write || SafeWrite(render_context + 0x178, forced_branch_gate));
        variant_applied = hair_gate_ready && branch_gate_ready;
        if (variant_applied) {
            wrote_hair_gate = hair_gate_needs_write;
            wrote_branch_gate = branch_gate_needs_write;
            g_aa_variant_apply_count.fetch_add(1);
        } else {
            // The stock routine must never observe half of a requested variant.
            // Roll the first field back immediately if the second write failed;
            // the __finally block below owns restoration only after full commit.
            if (branch_gate_needs_write && branch_gate_ready) {
                SafeWrite(render_context + 0x178, branch_gate);
            }
            if (hair_gate_needs_write && hair_gate_ready) {
                SafeWrite(render_context + 0x148, hair_blur_gate);
            }
        }
    }

    if (ShouldLogVerbose(g_aa_probe_verbose)) {
        log::InfoF(
            "aa_probe render_context=0x%p state_gate=%d hair_blur_gate=%d branch_gate=%d variant=%d applied=%d material=0x%p target=0x%p src_a=0x%p src_b=0x%p",
            reinterpret_cast<void*>(render_context),
            aa_state_gate,
            hair_blur_gate,
            branch_gate,
            variant_mode,
            variant_applied ? 1 : 0,
            reinterpret_cast<void*>(aa_material),
            reinterpret_cast<void*>(aa_target),
            reinterpret_cast<void*>(source_a),
            reinterpret_cast<void*>(source_b));
    }

    ++g_antialias_owner_scope_depth;
    __try {
        g_antialias_owner_original(render_context, param_2, param_3, param_4, param_5);
    } __finally {
        // Restore each field independently.  A partial SafeWrite must not
        // leave one stock-AA gate forced for every subsequent frame.
        if (wrote_hair_gate && hair_blur_gate != 1) {
            SafeWrite(render_context + 0x148, hair_blur_gate);
        }
        if (wrote_branch_gate && branch_gate != (variant_mode == 2 ? 1 : 0)) {
            SafeWrite(render_context + 0x178, branch_gate);
        }
        if (g_antialias_owner_scope_depth != 0) {
            --g_antialias_owner_scope_depth;
        }
    }

    std::uintptr_t resolved_aa_material = aa_material;
    SafeRead(render_context + 0x330, resolved_aa_material);

    unsigned int resolved_shader_uid = 0;
    unsigned int resolved_raster_uid = 0;
    unsigned int resolved_aux_uid = 0;
    if (resolved_aa_material != 0) {
        SafeRead(resolved_aa_material + 0xA8, resolved_shader_uid);
        SafeRead(resolved_aa_material + 0x150, resolved_raster_uid);
        SafeRead(resolved_aa_material + 0x1C0, resolved_aux_uid);
    }

    g_last_aa_material.store(resolved_aa_material);
    g_last_aa_shader_uid.store(resolved_shader_uid);
    g_last_aa_raster_uid.store(resolved_raster_uid);
    g_last_aa_aux_uid.store(resolved_aux_uid);

    MaybeWriteSummary();
}

void DetourAntiAliasFxHandoff(std::uintptr_t render_context,
                              std::uintptr_t arg1,
                              std::uintptr_t arg2,
                              std::uintptr_t arg3) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_antialias_fx_handoff_original(render_context, arg1, arg2, arg3);
        return;
    }
    g_aa_fx_handoff_count.fetch_add(1);
    g_last_aa_fx_arg1.store(arg1);
    g_last_aa_fx_arg2.store(arg2);
    g_last_aa_fx_arg3.store(arg3);

    if (ShouldLogVerbose(g_aa_fx_probe_verbose)) {
        log::InfoF(
            "aa_fx_probe render_context=0x%p arg1=0x%p arg2=0x%p arg3=0x%p aa_material=0x%p aa_target=0x%p",
            reinterpret_cast<void*>(render_context),
            reinterpret_cast<void*>(arg1),
            reinterpret_cast<void*>(arg2),
            reinterpret_cast<void*>(arg3),
            reinterpret_cast<void*>(g_last_aa_material.load()),
            reinterpret_cast<void*>(g_last_aa_target.load()));
    }

    if (g_config.hook_smaa_present && g_config.smaa_disable_stock_aa &&
        smaa::CanReplaceStockAa()) {
        MaybeWriteSummary();
        return;
    }

    g_antialias_fx_handoff_original(render_context, arg1, arg2, arg3);
    MaybeWriteSummary();
}

char DetourAntiAliasAuxStateQuery(void* state) {
    const char original = g_antialias_aux_state_query_original(state);
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return original;
    }

    const int aux_mode = ClampAaAuxMode(g_aa_runtime_aux_mode.load());
    g_last_aa_aux_mode.store(aux_mode);
    if (aux_mode == 0) {
        return original;
    }

    if (g_antialias_owner_scope_depth == 0) {
        return original;
    }

    g_aa_aux_apply_count.fetch_add(1);
    return aux_mode == 1 ? 0 : 1;
}

void DetourPresentBuffer(void* render_packet) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_present_buffer_original(render_packet);
        return;
    }
    if (g_config.hook_smaa_present) {
        smaa::MaybeInstallHooks();
    }
    // AgX installs from the same per-present tick as SMAA. Without this call
    // `agx::Configure` arms `g_enabled` and nothing ever installs the
    // pixel-shader detour, so the schema gate would go green while AgX stayed
    // dead - the exact partial-restore shape this three-site change avoids.
    if (g_config.agx_enabled) {
        agx::MaybeInstallHooks();
    }

    if (!g_config.hook_frameflow) {
        PollGraphicsDebugKeys();
    }

    g_present_buffer_original(render_packet);

    MaybeWriteSummary();
}

void DetourRenderSubmitMaterial(void* render_cmd,
                                void* submit_params,
                                void* material,
                                int flags) {
    g_post_material_submit_count.fetch_add(1);

    const std::uintptr_t material_ptr = reinterpret_cast<std::uintptr_t>(material);
    g_last_post_material.store(material_ptr);

    const std::uintptr_t render_context = ReadRenderContextInstance();
    const std::uintptr_t composite_lights = ReadRenderContextMaterial(render_context, 0x2B8);
    const std::uintptr_t composite_final = ReadRenderContextMaterial(render_context, 0x2C0);
    const std::uintptr_t shadow_collector = ReadRenderContextMaterial(render_context, 0x2C8);
    const std::uintptr_t bloom_threshold = ReadRenderContextMaterial(render_context, 0x328);
    const std::uintptr_t post_lightshaft = ReadRenderContextMaterial(render_context, 0x340);

    const char* matched = nullptr;
    if (material_ptr != 0) {
        if (material_ptr == composite_lights) {
            g_post_composite_lights_submit_count.fetch_add(1);
            matched = "composite_lights";
        } else if (material_ptr == composite_final) {
            g_post_composite_final_submit_count.fetch_add(1);
            matched = "composite_final";
            const std::uintptr_t render_cmd_ptr =
                reinterpret_cast<std::uintptr_t>(render_cmd);
            const std::uintptr_t submit_params_ptr =
                reinterpret_cast<std::uintptr_t>(submit_params);
            const std::uintptr_t param0 = ReadPointerField(submit_params_ptr, 0x00);
            const std::uintptr_t param1 = ReadPointerField(submit_params_ptr, 0x08);
            const std::uintptr_t param2 = ReadPointerField(submit_params_ptr, 0x10);
            const std::uintptr_t param3 = ReadPointerField(submit_params_ptr, 0x18);

            const bool snapshot_changed =
                g_last_post_final_flags.load() != flags ||
                g_last_post_final_cmd.load() != render_cmd_ptr ||
                g_last_post_final_params.load() != submit_params_ptr ||
                g_last_post_final_param0.load() != param0 ||
                g_last_post_final_param1.load() != param1 ||
                g_last_post_final_param2.load() != param2 ||
                g_last_post_final_param3.load() != param3;

            if (snapshot_changed) {
                g_post_composite_final_snapshot_change_count.fetch_add(1);
            }

            g_last_post_final_flags.store(flags);
            g_last_post_final_cmd.store(render_cmd_ptr);
            g_last_post_final_params.store(submit_params_ptr);
            g_last_post_final_param0.store(param0);
            g_last_post_final_param1.store(param1);
            g_last_post_final_param2.store(param2);
            g_last_post_final_param3.store(param3);

            if (snapshot_changed && ShouldLogVerbose(g_post_material_verbose)) {
                log::InfoF(
                    "post_composite_final_snapshot render_context=0x%p material=0x%p flags=%d cmd=0x%p params=0x%p p0=0x%p p1=0x%p p2=0x%p p3=0x%p",
                    reinterpret_cast<void*>(render_context),
                    material,
                    flags,
                    render_cmd,
                    submit_params,
                    reinterpret_cast<void*>(param0),
                    reinterpret_cast<void*>(param1),
                    reinterpret_cast<void*>(param2),
                    reinterpret_cast<void*>(param3));
            }
        } else if (material_ptr == bloom_threshold) {
            g_post_bloom_threshold_submit_count.fetch_add(1);
            matched = "bloom_threshold";
        } else if (material_ptr == post_lightshaft) {
            g_post_lightshaft_submit_count.fetch_add(1);
            matched = "post_lightshaft";
        } else if (material_ptr == shadow_collector) {
            g_post_shadow_collector_submit_count.fetch_add(1);
            matched = "shadow_collector";
        }
    }

    if (matched != nullptr && ShouldLogVerbose(g_post_material_verbose)) {
        log::InfoF(
            "post_material_submit match=%s render_context=0x%p material=0x%p flags=%d cmd=0x%p params=0x%p",
            matched,
            reinterpret_cast<void*>(render_context),
            material,
            flags,
            render_cmd,
            submit_params);
    }

    g_render_submit_material_original(render_cmd, submit_params, material, flags);
    MaybeWriteSummary();
}

void DetourSceneryPrepare(std::uintptr_t scenery_state) {
    g_scenery_prepare_count.fetch_add(1);
    const unsigned long long ready_before = ReadReadyCounterForScope(BuilderScope::SceneryPrepare);
    const SceneryCounters before_counters = ReadSceneryCounters();

    if (ShouldLogVerbose(g_scenery_prepare_verbose)) {
        log::InfoF("scenery_prepare state=0x%p counters_before=%u/%u/%u/%u",
                   reinterpret_cast<void*>(scenery_state),
                   before_counters.c0,
                   before_counters.c1,
                   before_counters.c2,
                   before_counters.c3);
    }

    {
        BuilderScopeGuard scope_guard(BuilderScope::SceneryPrepare);
        g_scenery_prepare_original(scenery_state);
    }

    const SceneryCounters after_counters = ReadSceneryCounters();
    StoreSceneryCounters(after_counters);
    const unsigned long long ready_delta =
        ReadReadyCounterForScope(BuilderScope::SceneryPrepare) - ready_before;
    if (ShouldLogVerbose(g_scenery_prepare_verbose)) {
        log::InfoF("scenery_prepare_complete state=0x%p ready_delta=%llu counters_after=%u/%u/%u/%u",
                   reinterpret_cast<void*>(scenery_state),
                   ready_delta,
                   after_counters.c0,
                   after_counters.c1,
                   after_counters.c2,
                   after_counters.c3);
    }

    MaybeWriteSummary();
}

void DetourScenerySetup(std::uintptr_t query_context,
                        std::uint32_t query_kind,
                        void* param_3,
                        void* param_4,
                        float range_scale) {
    g_scenery_setup_count.fetch_add(1);

    const QueueSnapshot before_queue = ReadQueueSnapshot();
    const SceneryCounters before_counters = ReadSceneryCounters();
    const unsigned long long ready_before = ReadReadyCounterForScope(BuilderScope::ScenerySetup);

    {
        BuilderScopeGuard scope_guard(BuilderScope::ScenerySetup);
        g_scenery_setup_original(query_context, query_kind, param_3, param_4, range_scale);
    }

    const QueueSnapshot after_queue = ReadQueueSnapshot();
    const SceneryCounters after_counters = ReadSceneryCounters();
    StoreSceneryCounters(after_counters);
    const unsigned long long ready_delta =
        ReadReadyCounterForScope(BuilderScope::ScenerySetup) - ready_before;

    const long long queued_delta =
        static_cast<long long>(after_queue.queued_total) - before_queue.queued_total;
    if (queued_delta > 0) {
        g_scenery_setup_queue_delta_total.fetch_add(
            static_cast<unsigned long long>(queued_delta));
    }

    if (ShouldLogVerbose(g_scenery_setup_verbose) || queued_delta > 0) {
        log::InfoF(
            "scenery_setup context=0x%p kind=%u range=%.3f queue_delta=%lld ready_delta=%llu "
            "counters_before=%u/%u/%u/%u counters_after=%u/%u/%u/%u",
            reinterpret_cast<void*>(query_context),
            query_kind,
            range_scale,
            queued_delta,
            ready_delta,
            before_counters.c0,
            before_counters.c1,
            before_counters.c2,
            before_counters.c3,
            after_counters.c0,
            after_counters.c1,
            after_counters.c2,
            after_counters.c3);
    }

    MaybeWriteSummary();
}

std::uintptr_t DetourRenderSceneryBuilder(std::uintptr_t param_1,
                                          std::uintptr_t param_2,
                                          void* param_3,
                                          std::uint32_t param_4,
                                          std::uint32_t param_5) {
    g_render_scenery_builder_count.fetch_add(1);

    const QueueSnapshot before_queue = ReadQueueSnapshot();
    const SceneryCounters before_counters = ReadSceneryCounters();
    const unsigned long long ready_before =
        ReadReadyCounterForScope(BuilderScope::RenderScenery);

    std::uintptr_t result = 0;
    {
        BuilderScopeGuard scope_guard(BuilderScope::RenderScenery);
        result = g_render_scenery_builder_original(param_1, param_2, param_3, param_4, param_5);
    }

    const QueueSnapshot after_queue = ReadQueueSnapshot();
    const SceneryCounters after_counters = ReadSceneryCounters();
    StoreSceneryCounters(after_counters);
    const unsigned long long ready_delta =
        ReadReadyCounterForScope(BuilderScope::RenderScenery) - ready_before;

    const long long queued_delta =
        static_cast<long long>(after_queue.queued_total) - before_queue.queued_total;
    if (queued_delta > 0) {
        g_render_scenery_queue_delta_total.fetch_add(
            static_cast<unsigned long long>(queued_delta));
    }

    if (ShouldLogVerbose(g_render_scenery_builder_verbose) || queued_delta > 0) {
        log::InfoF(
            "render_scenery_builder state=0x%p scene=0x%p dependency=0x%p mode=%u flags=%u "
            "queue_delta=%lld ready_delta=%llu counters_before=%u/%u/%u/%u counters_after=%u/%u/%u/%u result=0x%p",
            reinterpret_cast<void*>(param_1),
            reinterpret_cast<void*>(param_2),
            param_3,
            param_4,
            param_5,
            queued_delta,
            ready_delta,
            before_counters.c0,
            before_counters.c1,
            before_counters.c2,
            before_counters.c3,
            after_counters.c0,
            after_counters.c1,
            after_counters.c2,
            after_counters.c3,
            reinterpret_cast<void*>(result));
    }

    MaybeWriteSummary();
    return result;
}

unsigned long long DetourRasterizeBucketBuilder(std::uintptr_t state,
                                                int bucket_index,
                                                void* dependency) {
    g_rasterize_bucket_builder_count.fetch_add(1);

    const QueueSnapshot before_queue = ReadQueueSnapshot();
    const SceneryCounters before_counters = ReadSceneryCounters();
    const unsigned long long ready_before =
        ReadReadyCounterForScope(BuilderScope::RasterizeBucket);

    unsigned long long result = 0;
    {
        BuilderScopeGuard scope_guard(BuilderScope::RasterizeBucket);
        result = g_rasterize_bucket_builder_original(state, bucket_index, dependency);
    }

    const QueueSnapshot after_queue = ReadQueueSnapshot();
    const SceneryCounters after_counters = ReadSceneryCounters();
    StoreSceneryCounters(after_counters);
    const unsigned long long ready_delta =
        ReadReadyCounterForScope(BuilderScope::RasterizeBucket) - ready_before;
    const long long queued_delta =
        static_cast<long long>(after_queue.queued_total) - before_queue.queued_total;
    if (queued_delta > 0) {
        g_rasterize_bucket_queue_delta_total.fetch_add(
            static_cast<unsigned long long>(queued_delta));
    }

    if (ShouldLogVerbose(g_rasterize_bucket_builder_verbose) || queued_delta > 0) {
        log::InfoF(
            "rasterize_bucket_builder state=0x%p bucket=%d dependency=0x%p queue_delta=%lld "
            "ready_delta=%llu counters_before=%u/%u/%u/%u counters_after=%u/%u/%u/%u result=%llu",
            reinterpret_cast<void*>(state),
            bucket_index,
            dependency,
            queued_delta,
            ready_delta,
            before_counters.c0,
            before_counters.c1,
            before_counters.c2,
            before_counters.c3,
            after_counters.c0,
            after_counters.c1,
            after_counters.c2,
            after_counters.c3,
            result);
    }

    MaybeWriteSummary();
    return result;
}

void DetourPedestrianSpawnUpdate(void* manager, float delta_seconds) {
    if (g_pedestrian_spawn_update_original == nullptr) {
        return;
    }
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_pedestrian_spawn_update_original(manager, delta_seconds);
        return;
    }

    const bool previous_valid = g_pedestrian_update_delta_valid_tls;
    const float previous_delta = g_pedestrian_update_delta_tls;
    if (!previous_valid) {
        if (manager != g_pedestrian_manager_tls) {
            g_pedestrian_throttle_scheduler_tls.Reset();
            g_pedestrian_manager_tls = manager;
        }
    }
    g_pedestrian_update_delta_valid_tls = true;
    g_pedestrian_update_delta_tls = delta_seconds;

    __try {
        g_pedestrian_spawn_update_original(manager, delta_seconds);
    } __finally {
        g_pedestrian_update_delta_valid_tls = previous_valid;
        g_pedestrian_update_delta_tls = previous_delta;
    }
}

void DetourAverageWindowInitialize(void* window,
                                   float maximum_timespan,
                                   float assumed_sample_rate) {
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_average_window_initialize_original(window, maximum_timespan, assumed_sample_rate);
        return;
    }
    g_average_window_initialize_count.fetch_add(1, std::memory_order_relaxed);
    const average_window::CapacityDecision decision =
        average_window::ResolveCapacity(maximum_timespan, assumed_sample_rate);
    if (decision.expanded) {
        assumed_sample_rate = decision.effective_sample_rate;
        g_average_window_expanded_count.fetch_add(1, std::memory_order_relaxed);
    } else {
        bool expected = false;
        if (g_average_window_fallback_logged.compare_exchange_strong(
                expected, true, std::memory_order_relaxed)) {
            log::WarnF("average_window_capacity unchanged max_timespan=%.6f "
                       "sample_rate=%.6f recognized=%d entry_count=%zu",
                       maximum_timespan,
                       assumed_sample_rate,
                       decision.recognized_stock_rate ? 1 : 0,
                       decision.entry_count);
        }
    }

    g_average_window_initialize_original(window, maximum_timespan, assumed_sample_rate);
}

void DetourMaterialOnLoad(void* material) {
    if (g_behavior_transaction_ready.load(std::memory_order_acquire) &&
        g_config.restore_original_eye_reflections && material != nullptr) {
        const std::uintptr_t material_address =
            reinterpret_cast<std::uintptr_t>(material);
        std::uint32_t material_uid = 0;
        if (SafeRead(material_address + character_eye::kMaterialNameUidOffset, material_uid) &&
            character_eye::IsTargetMaterialUid(material_uid)) {
            const std::uint32_t original_diffuse =
                character_eye::OriginalDiffuseForMaterial(material_uid);
            const std::uint32_t log_bit = character_eye::LogBitForMaterial(material_uid);
            std::array<std::byte, character_eye::kRequiredMaterialBytes> snapshot{};
            character_eye::RestoreResult result =
                character_eye::RestoreResult::InvalidLayout;
            if (SafeRead(material_address, snapshot)) {
                result = character_eye::ApplyOriginalWeiEyeDiffuse(snapshot);
            }

            if (result == character_eye::RestoreResult::Applied) {
                const std::uintptr_t diffuse_address =
                    material_address + character_eye::kDiffuseResourceUidOffset;
                std::uint32_t current_diffuse = 0;
                if (SafeRead(diffuse_address, current_diffuse) &&
                    current_diffuse == character_eye::kDefinitiveFallbackDiffuseUid &&
                    SafeWrite(diffuse_address, original_diffuse)) {
                    const std::uint32_t previous =
                        g_character_eye_restore_applied_mask.fetch_or(
                            log_bit, std::memory_order_acq_rel);
                    if ((previous & log_bit) == 0) {
                        log::InfoF(
                            "character_eye_restore applied=1 material_uid=0x%08X "
                            "stock_diffuse=0x%08X original_diffuse=0x%08X",
                            material_uid,
                            character_eye::kDefinitiveFallbackDiffuseUid,
                            original_diffuse);
                    }
                } else if (current_diffuse == original_diffuse) {
                    result = character_eye::RestoreResult::AlreadyApplied;
                } else {
                    result = character_eye::RestoreResult::InvalidLayout;
                }
            }

            if (result == character_eye::RestoreResult::AlreadyApplied) {
                const std::uint32_t previous =
                    g_character_eye_restore_already_present_mask.fetch_or(
                        log_bit, std::memory_order_acq_rel);
                if ((previous & log_bit) == 0) {
                    log::InfoF(
                        "character_eye_restore already_present=1 material_uid=0x%08X "
                        "diffuse=0x%08X",
                        material_uid,
                        original_diffuse);
                }
            } else if (result == character_eye::RestoreResult::InvalidLayout) {
                const std::uint32_t previous =
                    g_character_eye_restore_failure_mask.fetch_or(
                        log_bit, std::memory_order_acq_rel);
                if ((previous & log_bit) == 0) {
                    log::WarnF(
                        "character_eye_restore skipped material_uid=0x%08X "
                        "reason=unexpected_layout",
                        material_uid);
                }
            }
        }
    }

    g_material_on_load_original(material);
}

// Action name carried by ambient push-reaction elements (round68 ledger
// section 8: the NPC reaction family uses plain lowercase action names).
constexpr char kPushReactionName[] = "pushed";
// CORRECTED 2026-10-25 (supersedes the old "call sites pass fully-qualified
// names" conclusion, ledger sec 36, which was inverted): SpeakByName
// (0x5A8930) takes the ACTION name and builds the full line name itself -
// '%s.%s.001.' with family = [component+0x1A0]->[0x48] (the voice tag) and
// action = the argument, then resolves the built name in the audio name
// table (with the per-component played-take variant). The engine's own
// dispatcher (0x5A6450) passes the element's action name ("pushed")
// verbatim. So the invoke passes "move" and the bank leaf names
// "Wei.move.001.<A..M>" below are what the ENGINE builds, not what we pass.
//
// Bank facts that still hold (verified against the DE install 2026-08-28):
// a raw byte scan of every *.big finds the substring "Wei.move" exactly ONCE,
// in UI.big at offset 144826923 (a DBG-BLK name table that also carries
// "DirtyMing.M_NMC.062.A" and "Wei.M_NMC.501.A"); the original non-DE
// install has no "Wei.move" string at all.
constexpr char kPushVoiceLineFamily[] = "Wei.move.001.";
// All thirteen takes are used. AudioSFX.xml binds only twelve to .wem paths (001.K
// has no <Path>), which initially looked like K had no audio - but the RUNTIME voice
// bank is the authority, and BOTH English(US).pck files contain all thirteen labels
// with RIFF containers around them:
//   DE    data\Audio\SD2\English(US).pck  (947 MB)  -> 13 variants, RIFF present
//   XBOX  Data\Audio\HK\English(US).pck   (1176 MB) -> 13 variants, RIFF present
// So K is real audio and AudioSFX.xml's binding list is simply incomplete. Prefer
// the runtime asset container over a metadata table when they disagree.
constexpr char kPushVoiceLineVariants[] = "ABCDEFGHIJKLM";
// Canonical ids of all thirteen takes, straight from AudioNames_Voices.xml and
// cross-checked by FNV-1a (13/13 match). Each variant has its OWN id, so the
// runtime check compares against the id of the variant actually selected rather
// than a single shared constant.
constexpr std::uint32_t kPushVoiceLineNameIds[13] = {
    168514527u,  // A
    168514524u,  // B
    168514525u,  // C
    168514522u,  // D
    168514523u,  // E
    168514520u,  // F
    168514521u,  // G
    168514518u,  // H
    168514519u,  // I
    168514516u,  // J
    168514517u,  // K
    168514514u,  // L
    168514515u,  // M
};
// Compile-time mirror of the runtime name construction (family + variant).
// Used by the static_assert above so the guard covers the variant list itself
// rather than a literal that could drift from what the code actually builds.
struct NameBuffer {
    // 64, not 32: the longest combat_pre_posture family in the full NPC roster
    // is "ChineseLittleKenny.combat_pre_posture.001." (42 chars) + one variant
    // char + NUL = 44. At 32 bytes AssembleName silently truncated, so the hash
    // below was computed over a partial name and the guard would have blamed
    // the ids instead of reporting the real cause. Truncation stays
    // fail-closed (the hash then mismatches and the static_assert fires), and
    // kAssembledNameCapacityIsEnough below pins that it never has to.
    char value[64]{};
};
constexpr NameBuffer AssembleName(const char* family,
                                  const char* variants,
                                  std::size_t index) {
    NameBuffer out{};
    std::size_t n = 0;
    for (const char* p = family; *p != '\0' && n + 1 < sizeof(out.value); ++p) {
        out.value[n++] = *p;
    }
    out.value[n] = variants[index];
    return out;
}
// Compile-time proof that no assembled family name can be truncated by the
// buffer above. Checked against the real table below rather than a hand-count,
// so adding a longer voice later fails the build here. `take_count` is the
// row's variant count; the assembled name is the family plus ONE take letter
// plus NUL, and the variant list must fit the buffer as well.
constexpr bool AssembledNameFits(const char* family, std::size_t take_count) {
    std::size_t n = 0;
    for (const char* p = family; *p != '\0'; ++p) {
        ++n;
    }
    // family + one take letter + NUL, and separately the letter list + its NUL.
    return n + 1 + 1 <= sizeof(NameBuffer{}.value) &&
           take_count + 1 <= sizeof(NameBuffer{}.value);
}
// FNV-1a 32-bit over the lowercased name, mirroring the engine's resolver at
// 0xA41FD0 (basis 0x811C9DC5, prime 0x01000193, A-Z mapped to a-z). Used by the
// static_asserts below to pin the bank names at compile time.
constexpr std::uint32_t Fnv1aLower(const char* text) {
    std::uint32_t hash = 0x811C9DC5u;
    for (const char* p = text; *p != '\0'; ++p) {
        const char c = (*p >= 'A' && *p <= 'Z')
                           ? static_cast<char>(*p + ('a' - 'A'))
                           : *p;
        hash = (hash * 0x01000193u);
        hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
    }
    return hash;
}
// Shared compile-time check used by the push family below and by every Wei
// voice-line family: every letter in `variants` must make the assembled
// "<family><letter>" name hash to the id at the same index. Parameterized so
// each family's assert covers its own full variant list.
constexpr bool FamilyVariantsMatchCanonicalIds(const char* family,
                                               const char* variants,
                                               const std::uint32_t* ids) {
    for (std::size_t i = 0; variants[i] != '\0'; ++i) {
        if (Fnv1aLower(AssembleName(family, variants, i).value) != ids[i]) {
            return false;
        }
    }
    return true;
}
// Compile-time guard against re-introducing the failure this feature already had.
//
// The previous variant list was "ABCDEFGHIJKLM", but a byte scan of every DE
// archive (tools/verify_bank_name.py) shows the bank holds exactly ONE take:
// "Wei.move.001.A". Twelve of every thirteen bumps therefore asked for a name
// that does not exist; the FNV-1a resolver returned 0 and the game played
// nothing, silently. Two static_asserts now prevent a recurrence:
//   - the variant list must be exactly "A";
//   - the assembled name must hash to 168514527, the id of the one real take.
// Both are checked by tools/verify_bank_name.py, whose output is recorded in the
// ledger. Changing either without re-running that script is a compile error, not
// a silent runtime no-op.
// The list must cover exactly the thirteen takes the name table defines.
static_assert(sizeof(kPushVoiceLineVariants) - 1 == 13,
              "kPushVoiceLineVariants must list exactly the thirteen Wei.move takes "
              "present in both English(US).pck voice banks and AudioNames_Voices.xml.");
// Hash the ASSEMBLED name, not a hardcoded literal.
//
// A first version of this assert hashed the literal "Wei.move.001.A". That was
// self-defeating: the bank leaf names are built as kPushVoiceLineFamily +
// a variant letter (the same construction the engine performs internally from
// the voice tag and the action), so editing the variant list left the literal
// untouched and the assert still passed. Sabotage test: setting the variants to
// "B" (a take that does NOT exist in the bank) compiled CLEAN with the
// literal-based assert - it failed to catch the precise bug it exists to prevent.
// Guards must be tested against the failure they target, not merely compiled.
//
// AssembleName() is the same construction the runtime uses, so this assert now
// covers the variant list, the family, and their concatenation.
// Assert EVERY variant, not just the first. With a single-variant check, adding a
// fabricated letter to the list could not be detected once the list was longer than
// one entry - the assert would only ever look at index 0.
constexpr bool AllVariantsMatchCanonicalIds() {
    return FamilyVariantsMatchCanonicalIds(kPushVoiceLineFamily,
                                           kPushVoiceLineVariants,
                                           kPushVoiceLineNameIds);
}
static_assert(AllVariantsMatchCanonicalIds(),
              "A push voice-line variant no longer hashes to its canonical id from "
              "AudioNames_Voices.xml. Every variant must be verified against the "
              "extracted name table before being added.");


// ---------------------------------------------------------------------------
// Wei voice-line families (restore_combat_lines) - round69 v1 catalog.
//
// Same guard pattern as the push family above, one block per v1 family. The
// letter lists and ids below were EXTRACTED from
// FilesRepo\AudioNames_Voices.xml (canonical extracted name table; every id
// independently reproduced by FNV-1a over the lowercased name, 83/83 MATCH),
// not extrapolated: combat_pre_posture runs A-Z (26 takes, beyond the push
// family's A-M), and the others were read off the XML as-is. The DE runtime
// bank covers every take (round69 t003c_matrix: de bank ids 26/20/14/23).
// Changing a letter list or id without re-running
// tools/verify_wei_voice_line_names.py is a compile error.
// ---------------------------------------------------------------------------
//
// The by-name speech entry (0x5A8930) takes the ACTION name only (ledger sec
// 41 ground truth); the engine composes "<voice_tag>.<action>.001." and picks
// the variation itself. The per-take ids below pin the bank family each action
// selects from, exactly like kPushVoiceLineNameIds does for "move".
// One row of the voice-take roster: the voice tag the engine composes names
// from, the bank family prefix it composes with, the take letters that family
// owns, and those takes' canonical bank ids.
struct VoiceTakeFamily {
    const char* voice;
    const char* family;
    const char* variants;
    const std::uint32_t* ids;
    std::size_t id_count;
};

// Deduce the id count from the array itself so a row can never disagree with
// its own variant list. A hand-written count is exactly the drift this table
// exists to eliminate.
template <std::size_t N>
constexpr VoiceTakeFamily MakeVoiceRow(const char* voice,
                                      const char* family,
                                      const char* variants,
                                      const std::uint32_t (&ids)[N]) {
    return VoiceTakeFamily{voice, family, variants, ids, N};
}

// ---- combat_pre_posture, the FULL NPC voice roster (C1) -----------------
//
// 66 voices, 400 takes. Wei's 26 takes were the only ones wired before; the
// other 374 ship in the DE banks and are driven by the same unwired code path.
//
// Every id was EXTRACTED from FilesRepo\AudioNames_Voices.xml and independently
// reproduced by FNV-1a over the lowercased
// "<voice>.combat_pre_posture.001.<letter>" name - 400/400 match, and the 26
// Wei ids match the single-voice block this replaces byte for byte
// (artifacts/reverse-engineering/2026-10-01-gtaiv-port-campaign/
// C1-voice-family-data.md).
//
// ONE table, not 65 copies of a per-voice block: MakeVoiceRow deduces each
// row's id COUNT from its own array, so a row can never disagree with its
// variant list, and the fold below runs the canonical-id check over all 66
// rows at compile time. A drifted family string or a stale id is a BUILD
// failure, not a silent runtime no-op - which is the exact class of bug the
// single-voice block was written to prevent.
constexpr std::uint32_t kAnton2PrePostureIds[8] = {
    2636953464u, 2636953467u, 2636953466u, 2636953469u,
    2636953468u, 2636953471u, 2636953470u, 2636953457u
};
constexpr std::uint32_t kBigAl2PrePostureIds[8] = {
    2137271833u, 2137271834u, 2137271835u, 2137271836u,
    2137271837u, 2137271838u, 2137271839u, 2137271824u
};
constexpr std::uint32_t kBoscoPrePostureIds[8] = {
    1283434954u, 1283434953u, 1283434952u, 1283434959u,
    1283434958u, 1283434957u, 1283434956u, 1283434947u
};
constexpr std::uint32_t kCaseyPrePostureIds[8] = {
    2062666239u, 2062666236u, 2062666237u, 2062666234u,
    2062666235u, 2062666232u, 2062666233u, 2062666230u
};
constexpr std::uint32_t kChineseAntonPrePostureIds[8] = {
    4157804375u, 4157804372u, 4157804373u, 4157804370u,
    4157804371u, 4157804368u, 4157804369u, 4157804382u
};
constexpr std::uint32_t kChineseChiHungPrePostureIds[3] = {
    3391670973u, 3391670974u, 3391670975u
};
constexpr std::uint32_t kChineseDerekPrePostureIds[8] = {
    2648756056u, 2648756059u, 2648756058u, 2648756061u,
    2648756060u, 2648756063u, 2648756062u, 2648756049u
};
constexpr std::uint32_t kChineseDonniePrePostureIds[8] = {
    2669898948u, 2669898951u, 2669898950u, 2669898945u,
    2669898944u, 2669898947u, 2669898946u, 2669898957u
};
constexpr std::uint32_t kChineseFloydPrePostureIds[8] = {
    4259654593u, 4259654594u, 4259654595u, 4259654596u,
    4259654597u, 4259654598u, 4259654599u, 4259654600u
};
constexpr std::uint32_t kChineseFreddiePrePostureIds[8] = {
    983096322u, 983096321u, 983096320u, 983096327u,
    983096326u, 983096325u, 983096324u, 983096331u
};
constexpr std::uint32_t kChineseJacksonPrePostureIds[7] = {
    2006179750u, 2006179749u, 2006179748u, 2006179747u,
    2006179746u, 2006179745u, 2006179744u
};
constexpr std::uint32_t kChineseKneecapsPrePostureIds[8] = {
    2597173605u, 2597173606u, 2597173607u, 2597173600u,
    2597173601u, 2597173602u, 2597173603u, 2597173612u
};
constexpr std::uint32_t kChineseLarryPrePostureIds[8] = {
    933526161u, 933526162u, 933526163u, 933526164u,
    933526165u, 933526166u, 933526167u, 933526168u
};
constexpr std::uint32_t kChineseLittleKennyPrePostureIds[8] = {
    1133512192u, 1133512195u, 1133512194u, 1133512197u,
    1133512196u, 1133512199u, 1133512198u, 1133512201u
};
constexpr std::uint32_t kChineseLittleMartyPrePostureIds[8] = {
    3165849476u, 3165849479u, 3165849478u, 3165849473u,
    3165849472u, 3165849475u, 3165849474u, 3165849485u
};
constexpr std::uint32_t kChineseMrKaoPrePostureIds[3] = {
    2512038243u, 2512038240u, 2512038241u
};
constexpr std::uint32_t kChineseMrYangPrePostureIds[3] = {
    744222277u, 744222278u, 744222279u
};
constexpr std::uint32_t kChineseSamsonPrePostureIds[3] = {
    3821240814u, 3821240813u, 3821240812u
};
constexpr std::uint32_t kChineseSangPrePostureIds[8] = {
    10630332u, 10630335u, 10630334u, 10630329u,
    10630328u, 10630331u, 10630330u, 10630325u
};
constexpr std::uint32_t kChineseShadyLiuPrePostureIds[8] = {
    198122798u, 198122797u, 198122796u, 198122795u,
    198122794u, 198122793u, 198122792u, 198122791u
};
constexpr std::uint32_t kChineseSleepyDongPrePostureIds[3] = {
    112160539u, 112160536u, 112160537u
};
constexpr std::uint32_t kChineseTerencePrePostureIds[8] = {
    879726811u, 879726808u, 879726809u, 879726814u,
    879726815u, 879726812u, 879726813u, 879726802u
};
constexpr std::uint32_t kChineseTommyPrePostureIds[3] = {
    2450285977u, 2450285978u, 2450285979u
};
constexpr std::uint32_t kChineseTruckerPrePostureIds[3] = {
    296843367u, 296843364u, 296843365u
};
constexpr std::uint32_t kDannyPrePostureIds[3] = {
    2562345878u, 2562345877u, 2562345876u
};
constexpr std::uint32_t kDayoPrePostureIds[3] = {
    455104613u, 455104614u, 455104615u
};
constexpr std::uint32_t kDerekPrePostureIds[3] = {
    2403281115u, 2403281112u, 2403281113u
};
constexpr std::uint32_t kDuckiePrePostureIds[8] = {
    3708189653u, 3708189654u, 3708189655u, 3708189648u,
    3708189649u, 3708189650u, 3708189651u, 3708189660u
};
constexpr std::uint32_t kEkinPrePostureIds[3] = {
    1356272269u, 1356272270u, 1356272271u
};
constexpr std::uint32_t kFaanPrePostureIds[3] = {
    1783736440u, 1783736443u, 1783736442u
};
constexpr std::uint32_t kFaz2PrePostureIds[3] = {
    1177325651u, 1177325648u, 1177325649u
};
constexpr std::uint32_t kFloyd2PrePostureIds[4] = {
    1186033522u, 1186033521u, 1186033520u, 1186033527u
};
constexpr std::uint32_t kFreddiePrePostureIds[8] = {
    2844524157u, 2844524158u, 2844524159u, 2844524152u,
    2844524153u, 2844524154u, 2844524155u, 2844524148u
};
constexpr std::uint32_t kGary2PrePostureIds[4] = {
    4085046021u, 4085046022u, 4085046023u, 4085046016u
};
constexpr std::uint32_t kGuoPrePostureIds[3] = {
    4194897897u, 4194897898u, 4194897899u
};
constexpr std::uint32_t kHarry2PrePostureIds[8] = {
    4186643346u, 4186643345u, 4186643344u, 4186643351u,
    4186643350u, 4186643349u, 4186643348u, 4186643355u
};
constexpr std::uint32_t kHorton2PrePostureIds[4] = {
    2179255824u, 2179255827u, 2179255826u, 2179255829u
};
constexpr std::uint32_t kJerry2PrePostureIds[8] = {
    1844049528u, 1844049531u, 1844049530u, 1844049533u,
    1844049532u, 1844049535u, 1844049534u, 1844049521u
};
constexpr std::uint32_t kKaiPrePostureIds[3] = {
    4061825203u, 4061825200u, 4061825201u
};
constexpr std::uint32_t kKangPrePostureIds[8] = {
    1597199021u, 1597199022u, 1597199023u, 1597199016u,
    1597199017u, 1597199018u, 1597199019u, 1597199012u
};
constexpr std::uint32_t kKneecaps2PrePostureIds[9] = {
    3214322844u, 3214322847u, 3214322846u, 3214322841u,
    3214322840u, 3214322843u, 3214322842u, 3214322837u,
    3214322836u
};
constexpr std::uint32_t kLamPrePostureIds[3] = {
    90892598u, 90892597u, 90892596u
};
constexpr std::uint32_t kLansonPrePostureIds[3] = {
    813321711u, 813321708u, 813321709u
};
constexpr std::uint32_t kM_ZOD_ChineseThug1PrePostureIds[8] = {
    1398751558u, 1398751557u, 1398751556u, 1398751555u,
    1398751554u, 1398751553u, 1398751552u, 1398751567u
};
constexpr std::uint32_t kM_ZOD_ChineseThug2PrePostureIds[8] = {
    3957046395u, 3957046392u, 3957046393u, 3957046398u,
    3957046399u, 3957046396u, 3957046397u, 3957046386u
};
constexpr std::uint32_t kM_ZOD_ChineseThug3PrePostureIds[8] = {
    3862141740u, 3862141743u, 3862141742u, 3862141737u,
    3862141736u, 3862141739u, 3862141738u, 3862141733u
};
constexpr std::uint32_t kM_ZOD_ChineseThug4PrePostureIds[8] = {
    1745470129u, 1745470130u, 1745470131u, 1745470132u,
    1745470133u, 1745470134u, 1745470135u, 1745470136u
};
constexpr std::uint32_t kM_ZOD_Thug2PrePostureIds[8] = {
    4166916656u, 4166916659u, 4166916658u, 4166916661u,
    4166916660u, 4166916663u, 4166916662u, 4166916665u
};
constexpr std::uint32_t kMack2PrePostureIds[8] = {
    1766550282u, 1766550281u, 1766550280u, 1766550287u,
    1766550286u, 1766550285u, 1766550284u, 1766550275u
};
constexpr std::uint32_t kMrCheungPrePostureIds[3] = {
    1884855343u, 1884855340u, 1884855341u
};
constexpr std::uint32_t kMrPangPrePostureIds[3] = {
    3517085285u, 3517085286u, 3517085287u
};
constexpr std::uint32_t kNastyYingPrePostureIds[8] = {
    1728851548u, 1728851551u, 1728851550u, 1728851545u,
    1728851544u, 1728851547u, 1728851546u, 1728851541u
};
constexpr std::uint32_t kNelson2PrePostureIds[3] = {
    1802662341u, 1802662342u, 1802662343u
};
constexpr std::uint32_t kOldManChaoPrePostureIds[3] = {
    4014476686u, 4014476685u, 4014476684u
};
constexpr std::uint32_t kOliver2PrePostureIds[2] = {
    115511271u, 115511268u
};
constexpr std::uint32_t kPunPrePostureIds[8] = {
    822147467u, 822147464u, 822147465u, 822147470u,
    822147471u, 822147468u, 822147469u, 822147458u
};
constexpr std::uint32_t kRedPrePostureIds[8] = {
    2468998375u, 2468998372u, 2468998373u, 2468998370u,
    2468998371u, 2468998368u, 2468998369u, 2468998382u
};
constexpr std::uint32_t kRobsonPrePostureIds[8] = {
    1657725179u, 1657725176u, 1657725177u, 1657725182u,
    1657725183u, 1657725180u, 1657725181u, 1657725170u
};
constexpr std::uint32_t kRobson2PrePostureIds[3] = {
    2253381233u, 2253381234u, 2253381235u
};
constexpr std::uint32_t kSimonPrePostureIds[10] = {
    1742819438u, 1742819437u, 1742819436u, 1742819435u,
    1742819434u, 1742819433u, 1742819432u, 1742819431u,
    1742819430u, 1742819429u
};
constexpr std::uint32_t kSmoothBobbyPrePostureIds[3] = {
    1457657960u, 1457657963u, 1457657962u
};
constexpr std::uint32_t kTeddyPrePostureIds[3] = {
    3945069902u, 3945069901u, 3945069900u
};
constexpr std::uint32_t kUgly2PrePostureIds[3] = {
    3562436651u, 3562436648u, 3562436649u
};
constexpr std::uint32_t kWaynePrePostureIds[8] = {
    2233410094u, 2233410093u, 2233410092u, 2233410091u,
    2233410090u, 2233410089u, 2233410088u, 2233410087u
};
constexpr std::uint32_t kWeiPrePostureIds[26] = {
    360710163u, 360710160u, 360710161u, 360710166u,
    360710167u, 360710164u, 360710165u, 360710170u,
    360710171u, 360710168u, 360710169u, 360710174u,
    360710175u, 360710172u, 360710173u, 360710146u,
    360710147u, 360710144u, 360710145u, 360710150u,
    360710151u, 360710148u, 360710149u, 360710154u,
    360710155u, 360710152u
};
constexpr std::uint32_t kYukSingPrePostureIds[3] = {
    1976762564u, 1976762567u, 1976762566u
};

constexpr VoiceTakeFamily kVoicePrePostureFamilies[] = {
    MakeVoiceRow("Anton2", "Anton2.combat_pre_posture.001.",
                 "ABCDEFGH", kAnton2PrePostureIds),
    MakeVoiceRow("BigAl2", "BigAl2.combat_pre_posture.001.",
                 "ABCDEFGH", kBigAl2PrePostureIds),
    MakeVoiceRow("Bosco", "Bosco.combat_pre_posture.001.",
                 "ABCDEFGH", kBoscoPrePostureIds),
    MakeVoiceRow("Casey", "Casey.combat_pre_posture.001.",
                 "ABCDEFGH", kCaseyPrePostureIds),
    MakeVoiceRow("ChineseAnton", "ChineseAnton.combat_pre_posture.001.",
                 "ABCDEFGH", kChineseAntonPrePostureIds),
    MakeVoiceRow("ChineseChiHung", "ChineseChiHung.combat_pre_posture.001.",
                 "ABC", kChineseChiHungPrePostureIds),
    MakeVoiceRow("ChineseDerek", "ChineseDerek.combat_pre_posture.001.",
                 "ABCDEFGH", kChineseDerekPrePostureIds),
    MakeVoiceRow("ChineseDonnie", "ChineseDonnie.combat_pre_posture.001.",
                 "ABCDEFGH", kChineseDonniePrePostureIds),
    MakeVoiceRow("ChineseFloyd", "ChineseFloyd.combat_pre_posture.001.",
                 "ABCDEFGH", kChineseFloydPrePostureIds),
    MakeVoiceRow("ChineseFreddie", "ChineseFreddie.combat_pre_posture.001.",
                 "ABCDEFGH", kChineseFreddiePrePostureIds),
    MakeVoiceRow("ChineseJackson", "ChineseJackson.combat_pre_posture.001.",
                 "ABCDEFG", kChineseJacksonPrePostureIds),
    MakeVoiceRow("ChineseKneecaps", "ChineseKneecaps.combat_pre_posture.001.",
                 "ABCDEFGH", kChineseKneecapsPrePostureIds),
    MakeVoiceRow("ChineseLarry", "ChineseLarry.combat_pre_posture.001.",
                 "ABCDEFGH", kChineseLarryPrePostureIds),
    MakeVoiceRow("ChineseLittleKenny", "ChineseLittleKenny.combat_pre_posture.001.",
                 "ABCDEFGH", kChineseLittleKennyPrePostureIds),
    MakeVoiceRow("ChineseLittleMarty", "ChineseLittleMarty.combat_pre_posture.001.",
                 "ABCDEFGH", kChineseLittleMartyPrePostureIds),
    MakeVoiceRow("ChineseMrKao", "ChineseMrKao.combat_pre_posture.001.",
                 "ABC", kChineseMrKaoPrePostureIds),
    MakeVoiceRow("ChineseMrYang", "ChineseMrYang.combat_pre_posture.001.",
                 "ABC", kChineseMrYangPrePostureIds),
    MakeVoiceRow("ChineseSamson", "ChineseSamson.combat_pre_posture.001.",
                 "ABC", kChineseSamsonPrePostureIds),
    MakeVoiceRow("ChineseSang", "ChineseSang.combat_pre_posture.001.",
                 "ABCDEFGH", kChineseSangPrePostureIds),
    MakeVoiceRow("ChineseShadyLiu", "ChineseShadyLiu.combat_pre_posture.001.",
                 "ABCDEFGH", kChineseShadyLiuPrePostureIds),
    MakeVoiceRow("ChineseSleepyDong", "ChineseSleepyDong.combat_pre_posture.001.",
                 "ABC", kChineseSleepyDongPrePostureIds),
    MakeVoiceRow("ChineseTerence", "ChineseTerence.combat_pre_posture.001.",
                 "ABCDEFGH", kChineseTerencePrePostureIds),
    MakeVoiceRow("ChineseTommy", "ChineseTommy.combat_pre_posture.001.",
                 "ABC", kChineseTommyPrePostureIds),
    MakeVoiceRow("ChineseTrucker", "ChineseTrucker.combat_pre_posture.001.",
                 "ABC", kChineseTruckerPrePostureIds),
    MakeVoiceRow("Danny", "Danny.combat_pre_posture.001.",
                 "ABC", kDannyPrePostureIds),
    MakeVoiceRow("Dayo", "Dayo.combat_pre_posture.001.",
                 "ABC", kDayoPrePostureIds),
    MakeVoiceRow("Derek", "Derek.combat_pre_posture.001.",
                 "ABC", kDerekPrePostureIds),
    MakeVoiceRow("Duckie", "Duckie.combat_pre_posture.001.",
                 "ABCDEFGH", kDuckiePrePostureIds),
    MakeVoiceRow("Ekin", "Ekin.combat_pre_posture.001.",
                 "ABC", kEkinPrePostureIds),
    MakeVoiceRow("Faan", "Faan.combat_pre_posture.001.",
                 "ABC", kFaanPrePostureIds),
    MakeVoiceRow("Faz2", "Faz2.combat_pre_posture.001.",
                 "ABC", kFaz2PrePostureIds),
    MakeVoiceRow("Floyd2", "Floyd2.combat_pre_posture.001.",
                 "ABCD", kFloyd2PrePostureIds),
    MakeVoiceRow("Freddie", "Freddie.combat_pre_posture.001.",
                 "ABCDEFGH", kFreddiePrePostureIds),
    MakeVoiceRow("Gary2", "Gary2.combat_pre_posture.001.",
                 "ABCD", kGary2PrePostureIds),
    MakeVoiceRow("Guo", "Guo.combat_pre_posture.001.",
                 "ABC", kGuoPrePostureIds),
    MakeVoiceRow("Harry2", "Harry2.combat_pre_posture.001.",
                 "ABCDEFGH", kHarry2PrePostureIds),
    MakeVoiceRow("Horton2", "Horton2.combat_pre_posture.001.",
                 "ABCD", kHorton2PrePostureIds),
    MakeVoiceRow("Jerry2", "Jerry2.combat_pre_posture.001.",
                 "ABCDEFGH", kJerry2PrePostureIds),
    MakeVoiceRow("Kai", "Kai.combat_pre_posture.001.",
                 "ABC", kKaiPrePostureIds),
    MakeVoiceRow("Kang", "Kang.combat_pre_posture.001.",
                 "ABCDEFGH", kKangPrePostureIds),
    MakeVoiceRow("Kneecaps2", "Kneecaps2.combat_pre_posture.001.",
                 "ABCDEFGHI", kKneecaps2PrePostureIds),
    MakeVoiceRow("Lam", "Lam.combat_pre_posture.001.",
                 "ABC", kLamPrePostureIds),
    MakeVoiceRow("Lanson", "Lanson.combat_pre_posture.001.",
                 "ABC", kLansonPrePostureIds),
    MakeVoiceRow("M_ZOD_ChineseThug1", "M_ZOD_ChineseThug1.combat_pre_posture.001.",
                 "ABCDEFGH", kM_ZOD_ChineseThug1PrePostureIds),
    MakeVoiceRow("M_ZOD_ChineseThug2", "M_ZOD_ChineseThug2.combat_pre_posture.001.",
                 "ABCDEFGH", kM_ZOD_ChineseThug2PrePostureIds),
    MakeVoiceRow("M_ZOD_ChineseThug3", "M_ZOD_ChineseThug3.combat_pre_posture.001.",
                 "ABCDEFGH", kM_ZOD_ChineseThug3PrePostureIds),
    MakeVoiceRow("M_ZOD_ChineseThug4", "M_ZOD_ChineseThug4.combat_pre_posture.001.",
                 "ABCDEFGH", kM_ZOD_ChineseThug4PrePostureIds),
    MakeVoiceRow("M_ZOD_Thug2", "M_ZOD_Thug2.combat_pre_posture.001.",
                 "ABCDEFGH", kM_ZOD_Thug2PrePostureIds),
    MakeVoiceRow("Mack2", "Mack2.combat_pre_posture.001.",
                 "ABCDEFGH", kMack2PrePostureIds),
    MakeVoiceRow("MrCheung", "MrCheung.combat_pre_posture.001.",
                 "ABC", kMrCheungPrePostureIds),
    MakeVoiceRow("MrPang", "MrPang.combat_pre_posture.001.",
                 "ABC", kMrPangPrePostureIds),
    MakeVoiceRow("NastyYing", "NastyYing.combat_pre_posture.001.",
                 "ABCDEFGH", kNastyYingPrePostureIds),
    MakeVoiceRow("Nelson2", "Nelson2.combat_pre_posture.001.",
                 "ABC", kNelson2PrePostureIds),
    MakeVoiceRow("OldManChao", "OldManChao.combat_pre_posture.001.",
                 "ABC", kOldManChaoPrePostureIds),
    MakeVoiceRow("Oliver2", "Oliver2.combat_pre_posture.001.",
                 "AB", kOliver2PrePostureIds),
    MakeVoiceRow("Pun", "Pun.combat_pre_posture.001.",
                 "ABCDEFGH", kPunPrePostureIds),
    MakeVoiceRow("Red", "Red.combat_pre_posture.001.",
                 "ABCDEFGH", kRedPrePostureIds),
    MakeVoiceRow("Robson", "Robson.combat_pre_posture.001.",
                 "ABCDEFGH", kRobsonPrePostureIds),
    MakeVoiceRow("Robson2", "Robson2.combat_pre_posture.001.",
                 "ABC", kRobson2PrePostureIds),
    MakeVoiceRow("Simon", "Simon.combat_pre_posture.001.",
                 "ABCDEFGHIJ", kSimonPrePostureIds),
    MakeVoiceRow("SmoothBobby", "SmoothBobby.combat_pre_posture.001.",
                 "ABC", kSmoothBobbyPrePostureIds),
    MakeVoiceRow("Teddy", "Teddy.combat_pre_posture.001.",
                 "ABC", kTeddyPrePostureIds),
    MakeVoiceRow("Ugly2", "Ugly2.combat_pre_posture.001.",
                 "ABC", kUgly2PrePostureIds),
    MakeVoiceRow("Wayne", "Wayne.combat_pre_posture.001.",
                 "ABCDEFGH", kWaynePrePostureIds),
    MakeVoiceRow("Wei", "Wei.combat_pre_posture.001.",
                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ", kWeiPrePostureIds),
    MakeVoiceRow("YukSing", "YukSing.combat_pre_posture.001.",
                 "ABC", kYukSingPrePostureIds),
};

// ---- compile-time guards over the WHOLE roster ----------------------------
//
// One predicate per row, folded across the table. It checks the two things
// that made the push family fail silently in the first place: that the
// variant list is exactly as long as the id array (MakeVoiceRow already ties
// those together, so this is belt-and-braces for a future hand-built row),
// and that EVERY take still hashes to its canonical id. The hash is taken
// over the name AssembleName builds - the same construction the runtime
// uses - so editing a family string, a variant list, or an id is a compile
// error rather than a name that resolves to nothing in the game.
constexpr bool VoiceRowIsConsistent(const VoiceTakeFamily& row) {
    std::size_t variant_count = 0;
    while (row.variants[variant_count] != '\0') {
        ++variant_count;
    }
    if (variant_count != row.id_count) {
        return false;
    }
    if (!AssembledNameFits(row.family, variant_count)) {
        return false;
    }
    return FamilyVariantsMatchCanonicalIds(row.family, row.variants, row.ids);
}

constexpr bool AllVoiceRowsAreConsistent() {
    for (const VoiceTakeFamily& row : kVoicePrePostureFamilies) {
        if (!VoiceRowIsConsistent(row)) {
            return false;
        }
    }
    return true;
}

static_assert(AllVoiceRowsAreConsistent(),
              "A combat_pre_posture row no longer matches AudioNames_Voices.xml: "
              "its variant list, id count, or a take id drifted. Re-extract from "
              "the name table before editing; a stale row would otherwise resolve "
              "to a name the bank does not hold and play nothing.");
static_assert(std::size(kVoicePrePostureFamilies) == 66,
              "the combat_pre_posture roster must cover all 66 NPC voices that "
              "carry the family in AudioNames_Voices.xml");

// ---- runtime resolution ---------------------------------------------------
//
// The single generic path every caller uses. It maps (voice, variant index)
// to the fully-qualified bank name and that take's canonical id.
//
// Returns false - never an out-of-range index - for an unknown voice or an
// index past the voice's take count, so the caller can fall back instead of
// reading off the end of a row. The table is a fixed 66-entry array scanned in
// order; it is read on the damage path behind the same gate the line itself
// uses, and 66 short strcmp calls is not worth a hash map here.
bool ResolveVoiceTake(const char* voice,
                      std::size_t variant_index,
                      NameBuffer& out_name,
                      std::uint32_t& out_id) {
    if (voice == nullptr) {
        return false;
    }
    for (const VoiceTakeFamily& row : kVoicePrePostureFamilies) {
        if (std::strcmp(row.voice, voice) != 0) {
            continue;
        }
        if (variant_index >= row.id_count) {
            return false;
        }
        out_name = AssembleName(row.family, row.variants, variant_index);
        out_id = row.ids[variant_index];
        return true;
    }
    return false;
}

// Wei is the shipped default speaker and the fallback whenever a victim's own
// voice cannot be determined. Named so the fallback and the table's Wei row
// cannot drift apart unnoticed.
constexpr char kWeiVoiceTag[] = "Wei";

// combat_taunt_cold (20 takes, ids 70089568-70089591).
constexpr char kWeiTauntColdFamily[] = "Wei.combat_taunt_cold.001.";
constexpr char kWeiTauntColdVariants[] = "ABCDEFGHIJKLMNOPQRST";
constexpr std::uint32_t kWeiTauntColdNameIds[20] = {
    70089570u,  // A
    70089569u,  // B
    70089568u,  // C
    70089575u,  // D
    70089574u,  // E
    70089573u,  // F
    70089572u,  // G
    70089579u,  // H
    70089578u,  // I
    70089577u,  // J
    70089576u,  // K
    70089583u,  // L
    70089582u,  // M
    70089581u,  // N
    70089580u,  // O
    70089587u,  // P
    70089586u,  // Q
    70089585u,  // R
    70089584u,  // S
    70089591u,  // T
};
static_assert(sizeof(kWeiTauntColdVariants) - 1 == 20,
              "kWeiTauntColdVariants must list exactly the 20 combat_taunt_cold "
              "takes present in AudioNames_Voices.xml and the DE voice bank.");
static_assert(FamilyVariantsMatchCanonicalIds(kWeiTauntColdFamily,
                                              kWeiTauntColdVariants,
                                              kWeiTauntColdNameIds),
              "A combat_taunt_cold variant no longer hashes to its canonical id "
              "from AudioNames_Voices.xml.");

// combat_taunt_angry (14 takes, ids 1337716624-1337716639).
constexpr char kWeiTauntAngryFamily[] = "Wei.combat_taunt_angry.001.";
constexpr char kWeiTauntAngryVariants[] = "ABCDEFGHIJKLMN";
constexpr std::uint32_t kWeiTauntAngryNameIds[14] = {
    1337716627u,  // A
    1337716624u,  // B
    1337716625u,  // C
    1337716630u,  // D
    1337716631u,  // E
    1337716628u,  // F
    1337716629u,  // G
    1337716634u,  // H
    1337716635u,  // I
    1337716632u,  // J
    1337716633u,  // K
    1337716638u,  // L
    1337716639u,  // M
    1337716636u,  // N
};
static_assert(sizeof(kWeiTauntAngryVariants) - 1 == 14,
              "kWeiTauntAngryVariants must list exactly the 14 combat_taunt_angry "
              "takes present in AudioNames_Voices.xml and the DE voice bank.");
static_assert(FamilyVariantsMatchCanonicalIds(kWeiTauntAngryFamily,
                                              kWeiTauntAngryVariants,
                                              kWeiTauntAngryNameIds),
              "A combat_taunt_angry variant no longer hashes to its canonical id "
              "from AudioNames_Voices.xml.");

// hit_apology (23 takes, ids 1082677960-1082677983).
constexpr char kWeiHitApologyFamily[] = "Wei.hit_apology.001.";
constexpr char kWeiHitApologyVariants[] = "ABCDEFGHIJKLMNOPQRSTUVW";
constexpr std::uint32_t kWeiHitApologyNameIds[23] = {
    1082677979u,  // A
    1082677976u,  // B
    1082677977u,  // C
    1082677982u,  // D
    1082677983u,  // E
    1082677980u,  // F
    1082677981u,  // G
    1082677970u,  // H
    1082677971u,  // I
    1082677968u,  // J
    1082677969u,  // K
    1082677974u,  // L
    1082677975u,  // M
    1082677972u,  // N
    1082677973u,  // O
    1082677962u,  // P
    1082677963u,  // Q
    1082677960u,  // R
    1082677961u,  // S
    1082677966u,  // T
    1082677967u,  // U
    1082677964u,  // V
    1082677965u,  // W
};
static_assert(sizeof(kWeiHitApologyVariants) - 1 == 23,
              "kWeiHitApologyVariants must list exactly the 23 hit_apology "
              "takes present in AudioNames_Voices.xml and the DE voice bank.");
static_assert(FamilyVariantsMatchCanonicalIds(kWeiHitApologyFamily,
                                              kWeiHitApologyVariants,
                                              kWeiHitApologyNameIds),
              "A hit_apology variant no longer hashes to its canonical id "
              "from AudioNames_Voices.xml.");

// swear_cops (12 takes, ids 1496118560-1496118575). The DE's Wei swearing
// family - it exists in AudioNames_Voices.xml and the DE voice bank but no
// game code ever plays it. Played by DrivingSwearLines as the bark (new behavior, not a restoration)
// for hard vehicle impacts.
constexpr char kWeiSwearCopsFamily[] = "Wei.swear_cops.001.";
constexpr char kWeiSwearCopsVariants[] = "ABCDEFGHIJKL";
constexpr std::uint32_t kWeiSwearCopsNameIds[12] = {
    1496118568u,  // A
    1496118571u,  // B
    1496118570u,  // C
    1496118573u,  // D
    1496118572u,  // E
    1496118575u,  // F
    1496118574u,  // G
    1496118561u,  // H
    1496118560u,  // I
    1496118563u,  // J
    1496118562u,  // K
    1496118565u,  // L
};
static_assert(sizeof(kWeiSwearCopsVariants) - 1 == 12,
              "kWeiSwearCopsVariants must list exactly the 12 swear_cops "
              "takes present in AudioNames_Voices.xml and the DE voice bank.");
static_assert(FamilyVariantsMatchCanonicalIds(kWeiSwearCopsFamily,
                                              kWeiSwearCopsVariants,
                                              kWeiSwearCopsNameIds),
              "A swear_cops variant no longer hashes to its canonical id "
              "from AudioNames_Voices.xml.");
// Police-directed takes belong to the cop-pursuit bark feature, not the
// generic impact path. User-identified by ear (2026-09-26, WAVs extracted
// from the DE English(US).pck): A, D, E, F ("come on guys, lay off!"), I,
// J ("not now guys, come on"), K ("this is almost ironic") and L are
// police-directed; only the neutral B, C, G and H takes remain here.
constexpr std::uint32_t kDrivingSwearCopsNameIds[] = {
    kWeiSwearCopsNameIds[1], kWeiSwearCopsNameIds[2], kWeiSwearCopsNameIds[6],
    kWeiSwearCopsNameIds[7]};
static_assert(sizeof(kDrivingSwearCopsNameIds) / sizeof(kDrivingSwearCopsNameIds[0]) == 4,
              "driving bark pool must retain the four neutral takes B, C, G, H");
// Driving-swear impact detector (restored with MaybeFireDrivingSwearLine):
// fire when the mover loses >= 6 m/s within one frame from >= 6 m/s (a wall
// or car impact), rate-limited by the per-feature gate.
constexpr float kDrivingSwearMinPreImpactSpeed = 6.0f;
constexpr float kDrivingSwearMinSpeedDrop = 6.0f;
constexpr unsigned kDrivingSwearCooldownMs = 6000;

// Action names passed to the by-name entry, with their fnv1 ids (same hash the
// resolver computes; the push "move" id 0xB37B55B2 was additionally found in
// the d_tags dialogue-event data). The four names below are the canonical
// family names from AudioNames_Voices.xml; their hashes are pinned here so the
// runtime comparison cannot drift from the compile-time constant.
constexpr char kPushVoiceLineActionName[] = "move";
static_assert(Fnv1aLower(kPushVoiceLineActionName) == 0xB37B55B2u,
              "move action hash drifted from the d_tags-anchored id");
constexpr std::uint32_t kPushVoiceLineActionId = 0xB37B55B2u;
constexpr char kWeiPrePostureAction[] = "combat_pre_posture";
constexpr char kWeiTauntColdAction[] = "combat_taunt_cold";
constexpr char kWeiTauntAngryAction[] = "combat_taunt_angry";
constexpr char kWeiHitApologyAction[] = "hit_apology";
constexpr char kWeiSwearCopsAction[] = "swear_cops";
static_assert(Fnv1aLower(kWeiPrePostureAction) == 0x9A9FAB1Eu,
              "combat_pre_posture action hash drifted");
static_assert(Fnv1aLower(kWeiTauntColdAction) == 0x02A432E9u,
              "combat_taunt_cold action hash drifted");
static_assert(Fnv1aLower(kWeiTauntAngryAction) == 0xB2B37CBEu,
              "combat_taunt_angry action hash drifted");
static_assert(Fnv1aLower(kWeiHitApologyAction) == 0x0126570Cu,
              "hit_apology action hash drifted");
// NPC voice-line family names (restore_police_voice_lines /
// restore_encourage_voice_lines). Canonical family names from
// AudioNames_Voices.xml; per-take ids are NOT pinned here (69 voices x up to
// 10 takes does not scale to a compile-time table) - tools/
// verify_npc_voice_line_names.py verifies every leaf name offline (gate G1).
constexpr char kNpcPoliceHarassAction[] = "police_harass";
constexpr char kNpcPoliceSpotAction[] = "police_spot";
constexpr char kNpcEncourageAction[] = "combat_encourage";
static_assert(Fnv1aLower(kNpcPoliceHarassAction) == 0x1C96068Au,
              "police_harass action hash drifted");
static_assert(Fnv1aLower(kNpcPoliceSpotAction) == 0x64715D8Au,
              "police_spot action hash drifted");
static_assert(Fnv1aLower(kNpcEncourageAction) == 0xBC0C28A3u,
              "combat_encourage action hash drifted");
// Reaction/priority id passed in ARG5 - the field guard 4 thresholds against
// (0x5A8994: cmp r12d,[rdi+0x1a8]; jbe -> reject).
//
// MEASURED (sec 38, session 22:23): the engine's own accepted calls use reaction=5.
// Mirroring proven-working traffic is safer than inventing a value. 0xA is only the
// dispatcher's fallback for elements with flags bit 8 set (0x5A647D cmovne esi,0xA).
constexpr unsigned int kPushReactionIdFallback = 5;
// Voice id passed in ARG2.
//
// CORRECTED (sec 38, session 22:23): the engine passes voice=0 on every one of its
// own speak_by_name calls (sample: "voice=0 reaction=5 name=ANECDOTE"). I previously
// passed 5, which came from misreading an older log whose field labels were swapped.
// 0 almost certainly means "use the speaker's assigned voice", so it is both what the
// engine does and the safest choice. Note the inner speak overwrites edx at
// 0x5A7C42 (xor edx,edx) before its decisive virtual call, so this value matters only
// for the earlier portion of the call.
// Which value is correct is NOT settled, because the diagnostic log's field labels
// were SWAPPED between builds (proved by comparing sessions):
//     20:37 build: "reaction=0 voice=5     name=GAME_ANTICIPATE"
//     22:24 build: "voice=0    reaction=5  name=ANECDOTE"
// Same {0,5} pair, opposite labels - so "the engine uses voice=0" was an artifact of
// reading swapped columns, and I must not keep flip-flopping on inference.
//
// Per the dispatcher (0x5A6450): ARG2 (edx) is a VOICE id - it is copied to r15d at
// 0x5A647A and SUBSTITUTED IN at 0x5A649A (cmove edi,r15d) when the element's
// [+0x18] equals the player voice id from 0x181140 (global 0x235AA6C). ARG3 (r8d) is
// the REACTION id (0x5A646E mov esi,r8d, with cmovne to 0xA at 0x5A647D).
//
// The engine's own PUSHED dispatches therefore carry a real voice id in ARG2; the
// observed small values are 5 (and reaction 5 or 7). Default to 5 and make both
// overridable so ONE live run settles it empirically instead of another round of
// inference. See kConfigPushVoiceId / kConfigPushReactionId defaults in Config.h.
constexpr unsigned int kPushVoiceIdDefault = 5;
constexpr unsigned int kPushReactionIdDefault = 5;

// One-shot gate for the audio-object "can speak" probe. See its use site for why
// this probe must never run more than once per process (advisor review, sec 38).
std::atomic_flag g_push_voice_line_gate_probed = ATOMIC_FLAG_INIT;

// Upper sanity bound for the latched priority at [component+0x1A8]. Guard 4 rejects
// when reaction <= latch, so the value only needs to clear the latch; anything past
// this is far outside what the engine uses (observed live values were single digits,
// and one garbage read on a non-audio component was 3393036593).
constexpr std::uint32_t kMaxPlausibleReactionId = 64;
// The ambient conversation feed produces hundreds of NON_COMMIT/ANECDOTE
// dispatches per minute; a small cap saturated on that noise before any
// player push could be observed. Keep the budget high enough to see push
// events during a diagnostic session (round68 ledger section 29).
// The ambient conversation feed emits hundreds of NON_COMMIT/ANECDOTE
// topics per minute and carries NO push events (ledger sec 30), so keep this
// small and readable; bump detection is diagnosed via kPushVoiceLineBumpProbeBudget.
constexpr unsigned long kPushVoiceLineDiagLogBudget = 64;

bool SafeReadCString(std::uintptr_t address, char* buffer, std::size_t capacity) {
    if (address == 0 || buffer == nullptr || capacity == 0) {
        return false;
    }
    if (!SafeCopyBytes(address, buffer, capacity)) {
        return false;
    }
    buffer[capacity - 1] = '\0';
    return buffer[0] != '\0';
}


// Calls the audio object's "can speak" virtual (vtable slot 1) with a single
// zeroed argument, mirroring the inner speak's decisive gate at 0x5A7C44.
// Uses a naked-style stub? No: we cannot safely emit arbitrary calls with shadow
// space from a normal C++ function pointer on MSVC x64, because the compiler
// already manages the stack. Instead we declare the callee as taking one pointer
// arg (rcx) - the extra zeroed argument the engine passes in edx is not needed to
// answer the "can you speak" question, and omitting it is safe because the callee
// only reads rcx.
using AudioObjectCanSpeakFn = bool (*)(void* audio_object);

bool CallAudioObjectCanSpeak(std::uintptr_t function, void* audio_object) noexcept {
    if (function == 0 || audio_object == nullptr) {
        return false;
    }
    __try {
        return reinterpret_cast<AudioObjectCanSpeakFn>(function)(audio_object);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Speak one of Wei's crowd-clearing lines through the game's own by-name entry.
//
// Why by-name and not by-id (ledger sec 36):
//   - The BY-ID wrapper (0x5A8E20) takes (name_id, voice_id) in that order; the
//     previous code passed them swapped, so the game looked up a "name" that was
//     really a voice id and silently played nothing. (The by-NAME entry below has
//     different semantics - see the SpeakByNameFn typedef.)
//   - The name resolver is an exact-match FNV-1 lookup, so a bare "wei.move" does
//     not resolve; only fully-qualified names like "Wei.move.001.A" exist.
// SpeakByName (0x5A8930) resolves the name itself and runs the same guards and the
// same inner implementation, so it removes both failure modes at once.
void InvokePushVoiceLine(std::uintptr_t target_component) {
    if (g_push_voice_line_invoking || g_speak_by_name_original == nullptr ||
        target_component == 0) {
        return;
    }

    // ROOT CAUSE (2026-10-25, live-log + disasm): SpeakByName (0x5A8930) does
    // NOT take a fully-qualified line name. Its play path builds the final
    // audio name itself - format '%s.%s.001.' at 0x5A8CAE with
    //   family = [component+0x1A0]->[0x48]  (the component's voice tag)
    //   action = this argument (verbatim via the 0x5AADC70 passthrough)
    // then hashes the built string and looks it up in the audio name table
    // (0x1464E0 with the per-component played-take variant), rejecting with an
    // internal "Speech file not found" when the lookup misses. The engine's
    // own dispatcher (0x5A6450) passes the element's ACTION name verbatim
    // (0x5A64E2 mov r8,[rbx+0x38]; the push elements carry "pushed").
    // Passing "Wei.move.001.E" therefore built "Wei.Wei.move.001.E.001.",
    // which never resolves - accepted=0 on every bump (0/18 in the 18:45
    // session, 0 accepted in the whole log). The action form is what every
    // other restored voice feature already passes through the shared invoke.
    // The take (A..M) is picked by the engine itself from the component's
    // played-take array, so the mod-side rotation is gone.
    const char* name = kPushVoiceLineActionName;

    // CORRECTED (sec 38): the engine's argument SEMANTICS are the reverse of
    // my parameter names. From the real caller (dispatcher 0x5A6450):
    //   0x5A648D  mov edi, [rcx+0x18]     ; ARG2 = voice/actor id
    //   0x5A646E  mov esi, r8d
    //   0x5A647D  cmovne esi, 0xA         ; ARG5 = reaction id, defaulting to 0xA
    //   0x5A64B3  mov edx, edi            ; ARG2
    //   0x5A64C0  mov [rsp+0x20], esi     ; ARG5
    // So ARG2 carries the VOICE id and ARG5 carries the REACTION id - and guard 4
    // (0x5A8994: cmp r12d,[rdi+0x1a8]; jbe reject, where r12d = ARG5) therefore
    // compares the REACTION id against the component threshold.
    //
    // Mirror the engine exactly: voice id in ARG2, reaction id 0xA in ARG5, clearing
    // the live threshold if the component's is at or above it.
    const unsigned int voice_id = g_config.push_voice_line_voice_id;
    unsigned int reaction_id = g_config.push_voice_line_reaction_id;
    std::uint32_t component_priority = 0;
    const bool priority_known =
        SafeRead(target_component + kSpeechGuardThresholdOffset, component_priority);
    // Only trust the latched value if it is a plausible priority. On a component that
    // is not an ActorAudioComponent, [+0x1A8] is an unrelated field and can hold
    // garbage - observed live as 3393036593, which produced a nonsense reaction id
    // (sec 38). Clamp, and never let the read drive the value outside a sane range.
    if (priority_known && component_priority >= reaction_id &&
        component_priority <= kMaxPlausibleReactionId) {
        reaction_id = component_priority + 1u;
    }
    // Note: the dispatcher (0x5A647D) substitutes 0xA into ARG5 only when the
    // element's flags have bit 8 set; the ordinary value the game sends is the one
    // observed on the engine's own dispatches, which is what the default holds.
    //
    // [+0x1A8] is a RATCHET, not a fixed threshold: the inner speak writes the
    // reaction id back into it on success (0x5A7C74 mov [rsi+0x1a8], r15d, where
    // r15d was loaded at 0x5A7B05 mov r15d, r8d = ARG5). So each accepted line
    // raises the bar for every later line. Left alone, our injected lines would
    // progressively block the game's own Wei lines, which use lower reaction ids.
    // We therefore snapshot the latch and restore it afterwards.

    // Diagnostic: ask the audio object directly whether it can speak, using the same
    // virtual call the inner speak uses as its decisive gate
    // (0x5A7C37 mov rcx,[rdi]; 0x5A7C44 call [rax+8], with rdi = &component[0x190]).
    // If this returns false, no argument we pass can make the line play, and the
    // problem is the audio object's state rather than our call shape (sec 38).
    //
    // SAFETY (advisor review, 2026-08-28): this is a SIDE-EFFECTING call on a live
    // game audio object through a vtable slot I have not statically identified. It is
    // therefore doubly fenced:
    //   1. opt-in only - g_config.diag_push_voice_line_events defaults to false and
    //      lives in the [Debug] section, so no end user ever reaches it;
    //   2. ONE-SHOT - an atomic_flag gate fires it at most once per process, so even
    //      with diagnostics enabled it cannot poke the audio object repeatedly.
    // Without fence 2, enabling diagnostics would call an unverified virtual on every
    // single bump, which is how a diagnostic becomes a regression.
    if (g_config.diag_push_voice_line_events &&
        !g_push_voice_line_gate_probed.test_and_set(std::memory_order_acq_rel)) {
        std::uintptr_t audio_object = 0;
        std::uintptr_t object_vtable = 0;
        std::uintptr_t slot1 = 0;
        __try {
            if (SafeRead(target_component + kSpeechGuardObjectOffset, audio_object) &&
                audio_object != 0 &&
                SafeRead(audio_object, object_vtable) &&
                object_vtable != 0 &&
                SafeRead(object_vtable + 0x08, slot1)) {
                // On Windows x64 the callee expects 32 bytes of shadow space above
                // the return address; a raw pointer call still needs it allocated.
                const bool can_speak = CallAudioObjectCanSpeak(
                    slot1, reinterpret_cast<void*>(audio_object));
                log::InfoF("push_voice_line_gate audio_object=0x%p slot1=0x%X can_speak=%u",
                           reinterpret_cast<void*>(audio_object),
                           static_cast<unsigned int>(slot1 - g_module_base),
                           can_speak ? 1u : 0u);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            log::InfoF("push_voice_line_gate audio_object=0x%p probe_faulted=1",
                       reinterpret_cast<void*>(audio_object));
        }
    }

    // Log the state of every guard SpeakByName applies, so an accepted=0 can be
    // localized to a specific gate instead of guessed at.
    //
    // FULL GUARD INVENTORY (disassembled, 0x5A8930; CORRECTED 2026-10-25 by
    // tracing every jump target): only TWO gates actually reject.
    //   G1 0x5A8968  cmp byte [rcx+0x168],0   jne continue; fall = reject
    //   G4 0x5A8994  cmp r12d,[rdi+0x1a8]     jbe 0x5A8971 (reject)
    // The voice-count (0x5A897F jle) and audio-object (0x5A8988 je) tests jump
    // FORWARD to 0x5A89B0 - the SAME shared continuation as the fall-through -
    // so they never reject; the play path uses [component+0x1A0]->[0x48] (the
    // voice tag) rather than the transient audio object. The bit-3 test at
    // 0x5A899D only skips a 500 ms sleep. The remaining rejections are all in
    // the post-hash body (a blocked-name global, the special-name id filter,
    // and the audio-name-table lookup of the BUILT name - see the root-cause
    // comment at the top of this function).
    if (g_config.diag_push_voice_line_events) {
        std::uint8_t enabled = 0;
        std::uint32_t voice_count = 0;
        std::uintptr_t object = 0;
        std::uint32_t latch = 0;
        std::uint8_t flags = 0;
        SafeRead(target_component + kSpeechGuardEnabledOffset, enabled);
        SafeRead(target_component + kSpeechGuardCountOffset, voice_count);
        SafeRead(target_component + kSpeechGuardObjectOffset, object);
        SafeRead(target_component + kSpeechGuardThresholdOffset, latch);
        SafeRead(target_component + kSpeechGuardFlagsOffset, flags);
        log::InfoF("push_voice_line_guards enabled=%u voice_count=%u object=%s "
                   "latch=%u reaction=%u flags=0x%02X",
                   static_cast<unsigned int>(enabled),
                   voice_count,
                   object != 0 ? "ok" : "NULL",
                   latch,
                   reaction_id,
                   static_cast<unsigned int>(flags));
        // The engine builds "<voice_tag>.<action>.001.<take>" from
        // [component+0x1A0]->[0x48]; log the tag once per session budget so a
        // misspoken family shows up directly in the log.
        std::uintptr_t voice_config = 0;
        std::uintptr_t tag_ptr = 0;
        char family[24]{};
        if (SafeRead(target_component + 0x1A0, voice_config) &&
            voice_config != 0 &&
            SafeRead(voice_config + 0x48, tag_ptr) &&
            !SafeReadCString(tag_ptr, family, sizeof(family))) {
            family[0] = '\0';
        }
        log::InfoF("push_voice_line_family tag=%s voice_config=%s",
                   family[0] != '\0' ? family : "(unreadable)",
                   voice_config != 0 ? "ok" : "NULL");
    }

    // Verify the name before calling. NOTE: the resolver does NOT look the name up -
    // it only hashes it (see below), so this check is an equality test against a
    // known-good id, never a "did it resolve" test.
    if (g_audio_name_to_id != nullptr) {
        unsigned int resolved_name_id = 0;
        __try {
            resolved_name_id = g_audio_name_to_id(name);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            resolved_name_id = 0;
        }
        // A nonzero id does NOT prove the name is real.
        //
        // VERIFIED BY DISASSEMBLY 2026-08-28: the resolver reached through
        // kAudioNameToIdRva (0x143890 -> 0xA41FD0) is a pure FNV-1a COMPUTATION.
        // It lowercases the input, runs FNV-1a (basis 0x811C9DC5, prime 0x01000193)
        // and returns at 0xA420BC - there is no table lookup and no existence test
        // anywhere in it. So a name absent from the bank still yields a plausible
        // nonzero id: measured, "Wei.move.001.B" hashes to 168514524 while the one
        // real take "Wei.move.001.A" hashes to 168514527.
        //
        // That is precisely why the original bug survived a "does it resolve" check
        // in spirit for so long: the check could not fail. The only sound test is
        // equality against the known id of a name proven to exist by
        // tools/verify_bank_name.py.
        // The resolver only hashes (0xA41FD0 is a pure FNV-1a computation,
        // verified 2026-08-28), so a nonzero id never proved existence. With
        // the action-name form the only sound check is equality against the
        // d_tags-anchored id of "move", pinned at compile time next to the
        // constant. The full line name is built inside the engine, so the
        // per-take id table is no longer consulted at runtime.
        const std::uint32_t expected_name_id = kPushVoiceLineActionId;
        if (resolved_name_id != expected_name_id) {
            log::ErrorF("push_voice_line_name_unresolved name=%s id=%u expected=%u "
                        "(the resolver only hashes, so a nonzero id does not mean the "
                        "name exists; verify with tools/verify_bank_name.py)",
                        name,
                        resolved_name_id,
                        expected_name_id);
            return;
        }
        if (g_config.diag_push_voice_line_events) {
            log::InfoF("push_voice_line_name_resolved name=%s hash=%u expected=%u match=%u",
                       name,
                       resolved_name_id,
                       expected_name_id,
                       resolved_name_id == expected_name_id ? 1u : 0u);
        }
    }

    bool accepted = false;
    bool call_succeeded = false;
    // Same implausible-latch clear as InvokeVoiceLineAction: an NPC-style
    // scheduler timestamp at [+0x1A8] cannot be exceeded by any reaction id.
    // The restore below puts the original value back after the call.
    if (priority_known && component_priority > kMaxPlausibleReactionId) {
        SafeWrite(target_component + kSpeechGuardThresholdOffset, 0u);
    }
    g_push_voice_line_invoking = true;
    __try {
        accepted = g_speak_by_name_original(reinterpret_cast<void*>(target_component),
                                            voice_id,
                                            name,
                                            0,
                                            reaction_id,
                                            0,
                                            0);
        call_succeeded = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        accepted = false;
        call_succeeded = false;
        // Record WHY, not just that: an access violation on the chosen component
        // means the signature probe picked a wrong component, a different fault from
        // a bad argument (ledger sec 38).
        log::WarnF("push_voice_line_invoke_exception code=0x%08X component=0x%p reaction=%u voice=%u name=%s",
                   static_cast<unsigned int>(GetExceptionCode()),
                   reinterpret_cast<void*>(target_component),
                   reaction_id,
                   static_cast<unsigned int>(voice_id),
                   name);
    }
    g_push_voice_line_invoking = false;

    // Undo the ratchet so the game's own speech is not progressively blocked.
    if (priority_known) {
        std::uint32_t latched = 0;
        __try {
            if (SafeRead(target_component + kSpeechGuardThresholdOffset, latched) &&
                latched != component_priority) {
                std::uint32_t restore = component_priority;
                SafeWrite(target_component + kSpeechGuardThresholdOffset, restore);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Restoring is best-effort; failing to do so only costs future lines.
        }
    }

    if (call_succeeded) {
        g_push_voice_line_fired.fetch_add(1, std::memory_order_relaxed);
        if (g_config.diag_push_voice_line_events) {
            // Log the latch as well: if accepted=0, the pair (reaction, latch)
            // shows immediately whether guard 4 rejected us or something later did.
            std::uint32_t latch_after = 0;
            __try {
                SafeRead(target_component + kSpeechGuardThresholdOffset, latch_after);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                latch_after = 0;
            }
            log::InfoF("push_voice_line_invoke voice=%u name=%s reaction=%u latch=%u accepted=%u",
                       static_cast<unsigned int>(voice_id),
                       name,
                       reaction_id,
                       latch_after,
                       accepted ? 1u : 0u);
        }
    } else {
        log::WarnF("push_voice_line_invoke failed reason=exception");
    }
}


// One restored voice line, spoken through the same machinery as the shipped
// push line but with the caller's own action and per-feature bookkeeping.
//
// GROUND TRUTH (sec 41, from the game's own traffic logged by the
// DetourSpeakByName observation): real by-name calls pass the ACTION NAME ONLY
// as the name argument ("PUSHED", "STARTLED", "ANECDOTE") - the setter composes
// "<voice_tag>.<action>.001." itself and picks the variation itself. A full
// leaf name composes garbage and is silently rejected. The push feature keeps
// its own leaf-name path; the Wei and NPC families pass their action names.
//
// The invocation is shared by the restored Wei voice-line families
// (restore_combat_lines) and the NPC families (restore_police_voice_lines /
// restore_encourage_voice_lines); the caller picks the action, the log prefix,
// and the feature's own fire counter.
struct SpeechLineInvocation {
    // ACTION name only (ledger sec 41 ground truth).
    const char* action_name;
    // fnv1(action_name), pinned at compile time at the call site.
    std::uint32_t expected_action_id;
    // Log prefix, e.g. "wei_voice_line" or "npc_voice_line".
    const char* log_prefix;
    unsigned int voice_id;
    bool diag;
    // The invoke verdict line logs on every attempt for the restored families
    // (the live gate greps for it).
    bool always_log_invoke;
    // Per-feature fire counter (keeps each feature's cadence independent).
    std::atomic<unsigned long>* fired_counter;
    // Optional exact leaf selected through the native by-ID wrapper.
    unsigned int explicit_name_id = 0;
};

void InvokeVoiceLineAction(std::uintptr_t target_component,
                           const SpeechLineInvocation& invocation) {
    if (g_push_voice_line_invoking ||
        (g_speak_by_name_original == nullptr &&
         g_speech_wrapper_original == nullptr) ||
        target_component == 0) {
        return;
    }

    char name[32]{};
    std::snprintf(name, sizeof(name), "%s", invocation.action_name);

    // ARG2 carries the VOICE id and ARG5 the REACTION id (argument semantics
    // derived from the dispatcher are documented in InvokePushVoiceLine above),
    // so guard 4 (0x5A8994: cmp r12d,[rdi+0x1a8]; jbe reject) compares the
    // reaction id against the component's latched threshold. [+0x1A8] is a
    // RATCHET - the inner speak writes the reaction id back on success - so the
    // latch is snapshotted here and restored afterwards, or our lines would
    // progressively block the game's own Wei lines.
    const unsigned int voice_id = invocation.voice_id;
    unsigned int reaction_id = g_config.push_voice_line_reaction_id;
    std::uint32_t component_priority = 0;
    const bool priority_known =
        SafeRead(target_component + kSpeechGuardThresholdOffset, component_priority);
    // Only trust the latched value if it is a plausible priority (see the same
    // clamp in InvokePushVoiceLine: on a non-audio component [+0x1A8] is an
    // unrelated field and can hold garbage).
    if (priority_known && component_priority >= reaction_id &&
        component_priority <= kMaxPlausibleReactionId) {
        reaction_id = component_priority + 1u;
    }

    // Verify the action name before calling. The resolver only HASHES the name,
    // so this is an equality test against the compile-time-pinned fnv1 id, not
    // a "did it resolve" test (a nonzero id does not prove the name exists).
    if (g_audio_name_to_id != nullptr) {
        unsigned int resolved_name_id = 0;
        __try {
            resolved_name_id = g_audio_name_to_id(name);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            resolved_name_id = 0;
        }
        const std::uint32_t expected_name_id = invocation.expected_action_id;
        if (resolved_name_id != expected_name_id) {
            log::ErrorF("%s_name_unresolved name=%s id=%u expected=%u "
                        "(the resolver only hashes, so a nonzero id does not mean the "
                        "name exists; verify against AudioNames_Voices.xml)",
                        invocation.log_prefix,
                        name,
                        resolved_name_id,
                        expected_name_id);
            return;
        }
    }

    bool accepted = false;
    bool call_succeeded = false;
    // Post-hash filter state (disasm 0x5A8BC2..0x5A8C22): when the action name
    // is not one of the engine's special reactions ('pushed', 'panic', ...),
    // the speak body skips its id filter only while a flag byte at
    // [manager+0x84] or [manager+0x85] is zero (manager singleton at
    // module+0x441BE30). Logged with the verdict so a filter rejection is
    // distinguishable from a built-name table miss.
    std::uint8_t speech_flag_84 = 0;
    std::uint8_t speech_flag_85 = 0;
    {
        std::uintptr_t manager = 0;
        if (SafeRead(g_module_base + 0x441BE30, manager) && manager != 0) {
            SafeRead(manager + 0x84, speech_flag_84);
            SafeRead(manager + 0x85, speech_flag_85);
        }
    }
    // Guard 4 compares the reaction id against [+0x1A8]. On NPC components
    // that field can hold a huge, time-increasing value (live 2026-10-25:
    // 2847636000 -> 2850944656 across one minute on cop components - a
    // microsecond-scale scheduler timestamp, not a reaction ratchet), which no
    // reaction id can exceed: 18/18 npc_voice_line invokes were rejected with
    // latch=2850944656 reaction=5 in the 18:45 session. Clear the field for
    // the duration of the invoke; the post-invoke restore below puts the
    // original value back, so the game's own scheduler state is preserved and
    // a concurrent game write during the window is overwritten by the restore
    // at worst resetting one bark cooldown.
    if (priority_known && component_priority > kMaxPlausibleReactionId) {
        SafeWrite(target_component + kSpeechGuardThresholdOffset, 0u);
    }
    g_push_voice_line_invoking = true;
    __try {
        if (invocation.explicit_name_id != 0 && g_speech_wrapper_original != nullptr) {
            // The by-ID wrapper takes voice id in ARG2 and leaf name id in ARG3.
            accepted = g_speech_wrapper_original(reinterpret_cast<void*>(target_component),
                                                 0,
                                                 invocation.explicit_name_id,
                                                 0,
                                                 reaction_id,
                                                 0);
        } else if (g_speak_by_name_original != nullptr) {
            accepted = g_speak_by_name_original(reinterpret_cast<void*>(target_component),
                                                voice_id,
                                                name,
                                                0,
                                                reaction_id,
                                                0,
                                                0);
        }
        call_succeeded = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        accepted = false;
        call_succeeded = false;
        log::WarnF("%s_invoke_exception code=0x%08X component=0x%p reaction=%u voice=%u name=%s",
                   invocation.log_prefix,
                   static_cast<unsigned int>(GetExceptionCode()),
                   reinterpret_cast<void*>(target_component),
                   reaction_id,
                   static_cast<unsigned int>(voice_id),
                   name);
    }
    g_push_voice_line_invoking = false;

    // Undo the ratchet so the game's own speech is not progressively blocked.
    if (priority_known) {
        std::uint32_t latched = 0;
        __try {
            if (SafeRead(target_component + kSpeechGuardThresholdOffset, latched) &&
                latched != component_priority) {
                std::uint32_t restore = component_priority;
                SafeWrite(target_component + kSpeechGuardThresholdOffset, restore);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Restoring is best-effort; failing to do so only costs future lines.
        }
    }

    if (call_succeeded) {
        invocation.fired_counter->fetch_add(1, std::memory_order_relaxed);
        if (invocation.always_log_invoke || invocation.diag) {
            // Log the latch as well: if accepted=0, the pair (reaction, latch)
            // shows immediately whether guard 4 rejected us or something later did.
            std::uint32_t latch_after = 0;
            __try {
                SafeRead(target_component + kSpeechGuardThresholdOffset, latch_after);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                latch_after = 0;
            }
            // component= groups accepts/rejects by speaker: with the NPC features
            // the same line serves cop and civilian victims, and only
            // per-component grouping shows which victims were cops.
            log::InfoF("%s_invoke voice=%u name=%s reaction=%u latch=%u accepted=%u component=0x%p sf=%u/%u",
                       invocation.log_prefix,
                       static_cast<unsigned int>(voice_id),
                       name,
                       reaction_id,
                       latch_after,
                       accepted ? 1u : 0u,
                       reinterpret_cast<void*>(target_component),
                       static_cast<unsigned int>(speech_flag_84),
                       static_cast<unsigned int>(speech_flag_85));
        }
    } else {
        log::WarnF("%s_invoke failed reason=exception", invocation.log_prefix);
    }
}

// Driving swear trigger (restored 2026-10-31 from the recovery snapshot).
// Fires one of Wei's neutral swear_cops takes when the player's vehicle
// loses speed abruptly (an impact) while the camera hooks are live. The
// camera-side ChaseCameraUpdateProbeScope supplies vehicle class and the
// physics mover; the speech-side invoke is the shared restored-line path.
bool ReadUiModeState(std::uint32_t& out_mode);
std::uintptr_t ResolvePlayerAudioComponent() noexcept;
void MaybeFireDrivingSwearLine(ChaseCameraUpdateProbeScope& scope) noexcept {
    if (!g_config.driving_swear_lines ||
        !g_behavior_transaction_ready.load(std::memory_order_acquire) ||
        scope.component == nullptr) {
        g_driving_swear_trigger_state = {};
        return;
    }
    // DrivingSwearLinesFreeRoamOnly / SwearLinesFreeRoamOnly: suppress while
    // a mission is running (UI mode == 2). A failed mode read degrades to
    // unsuppressed.
    if (g_config.driving_swear_lines_free_roam_only ||
        g_config.swear_lines_free_roam_only) {
        std::uint32_t mission_mode = 0;
        if (ReadUiModeState(mission_mode) && mission_mode == 2) {
            g_driving_swear_trigger_state = {};
            return;
        }
    }
    if (scope.decision.vehicle_class == vehicle_camera::VehicleClass::Unknown) {
        if (g_driving_swear_trigger_state.component == scope.component) {
            g_driving_swear_trigger_state = {};
        }
        return;
    }
    if (IsGamePausedSafely()) {
        g_driving_swear_trigger_state = {};
        return;
    }

    DrivingSwearTriggerState& state = g_driving_swear_trigger_state;
    if (state.component != scope.component ||
        state.physics_mover != scope.decision.physics_mover) {
        state = {};
        state.component = scope.component;
        state.physics_mover = scope.decision.physics_mover;
    }

    const float dt = scope.delta_seconds;
    float speed = 0.0f;
    if (!std::isfinite(dt) || dt <= 0.0f || dt >= 0.1f ||
        !SafeRead(state.physics_mover + vehicle_camera::kPhysicsMoverForwardSpeedOffset,
                  speed) ||
        !std::isfinite(speed)) {
        // A rejected frame breaks the one-frame speed-drop observation.
        // The next valid sample seeds history instead of inventing an impact.
        state.has_prev_speed = false;
        return;
    }
    const bool has_prev = state.has_prev_speed;
    const float prev_speed = state.prev_speed;
    state.has_prev_speed = true;
    state.prev_speed = speed;

    if (g_config.enable_logging) {
        const unsigned long sample =
            g_driving_swear_observations.fetch_add(1, std::memory_order_relaxed);
        if (sample < 4 || (sample < 8192 && sample % 256 == 0)) {
            log::InfoF("driving_swear_line_observe sample=%lu has_prev=%u "
                       "pre=%.6f post=%.6f dt=%.6f mover=0x%p",
                       sample, has_prev ? 1u : 0u,
                       static_cast<double>(prev_speed), static_cast<double>(speed),
                       static_cast<double>(dt), reinterpret_cast<void*>(state.physics_mover));
        }
    }
    if (!has_prev || speed < 0.0f || prev_speed < kDrivingSwearMinPreImpactSpeed ||
        (prev_speed - speed) < kDrivingSwearMinSpeedDrop) {
        return;
    }
    const std::uintptr_t audio_component = ResolvePlayerAudioComponent();
    if (g_config.enable_logging &&
        g_driving_swear_candidates.fetch_add(1, std::memory_order_relaxed) < 64) {
        log::InfoF("driving_swear_line_candidate pre=%.6f post=%.6f "
                   "dt=%.6f audio_ready=%u mover=0x%p",
                   static_cast<double>(prev_speed), static_cast<double>(speed),
                   static_cast<double>(dt), audio_component != 0 ? 1u : 0u,
                   reinterpret_cast<void*>(state.physics_mover));
    }
    if (audio_component == 0) {
        return;
    }
    push_voice_line::Decision decision;
    {
        std::lock_guard<std::mutex> lock(g_driving_swear_line_gate_mutex);
        decision = g_driving_swear_line_gate.Evaluate(
            GetTickCount64(), false, kDrivingSwearCooldownMs);
    }
    if (!decision.fire) {
        return;
    }
    const unsigned long variant_index =
        g_driving_swear_variant_cursor.fetch_add(1, std::memory_order_relaxed) %
        (sizeof(kDrivingSwearCopsNameIds) / sizeof(kDrivingSwearCopsNameIds[0]));
    SpeechLineInvocation invocation{
        kWeiSwearCopsAction,
        Fnv1aLower(kWeiSwearCopsAction),
        "driving_swear_line",
        0,  // voice=0 = the component's own voice.
        false,
        true,
        &g_driving_swear_line_fired,
        kDrivingSwearCopsNameIds[variant_index]};
    InvokeVoiceLineAction(audio_component, invocation);
}

// Evaluate the push-voice-line gate under a lock. PushVoiceLineGate holds plain
// non-atomic members and Evaluate() is a read-modify-write over them, while two
// distinct hook paths (conversation dispatch and HealthApplyDamage) can reach it
// from different engine threads. Without the lock the cooldown and per-minute
// budget can leak under contention (sec 38, adversarial review).
push_voice_line::Decision EvaluatePushVoiceLineGate() {
    std::lock_guard<std::mutex> lock(g_push_voice_line_gate_mutex);
    return g_push_voice_line_gate.Evaluate(GetTickCount64(), false);
}

// Shared push-detection used by both conversation entry points. The base
// dispatcher and the ScriptedConversationElement entry twin carry the same
// element layout, so one implementation observes either entry (round68 sec 27).
// `origin` names the entry that observed the dispatch for the diagnostic log.
// Forward declaration: the audio-component resolver is defined with the push
// voice-line helpers, after the conversation element observers.
std::uintptr_t ResolvePlayerAudioComponent() noexcept;

void ObservePushVoiceLineElement(void* element,
                                 unsigned int reaction_id,
                                 const char* origin) {
    const std::uintptr_t element_address = reinterpret_cast<std::uintptr_t>(element);
    std::uint32_t reaction_type = 0xFFFFFFFF;
    std::uintptr_t name_ptr = 0;
    std::uintptr_t target_component = 0;
    const bool type_valid = SafeRead(element_address + kElementReactionTypeOffset, reaction_type);
    const bool name_valid = SafeRead(element_address + kElementActionNameOffset, name_ptr);
    const bool target_valid = SafeRead(element_address + kElementTargetComponentOffset, target_component);

    const bool want_diag = g_config.diag_push_voice_line_events;
    if (want_diag &&
        g_push_voice_line_diag_events.fetch_add(1, std::memory_order_relaxed) <
            kPushVoiceLineDiagLogBudget) {
        char name[24]{};
        const bool has_name = name_valid && SafeReadCString(name_ptr, name, sizeof(name));
        log::InfoF("conversation_dispatch origin=%s type=%u(%s) name=%s target=%s reaction_id=%u",
                   origin,
                   type_valid ? reaction_type : 0xFFFFFFFFu,
                   type_valid ? "ok" : "unreadable",
                   has_name ? name : "(none)",
                   target_valid && target_component != 0 ? "ok" : "none",
                   reaction_id);
    }

    const bool want_fix = g_config.restore_push_voice_lines;
    bool is_push_reaction = false;
    if (want_fix && type_valid && target_valid && target_component != 0 &&
        reaction_type != 0) {
        char name[16]{};
        if (name_valid && SafeReadCString(name_ptr, name, sizeof(name))) {
            // The local buffer is NUL-terminated by SafeReadCString, so the
            // comparison cannot read past it.
            is_push_reaction =
                std::strncmp(name, kPushReactionName, sizeof(name)) == 0;
        }
    }
    if (is_push_reaction) {
        // Resolve the invocation inputs first so the cooldown/budget only
        // ticks when a line can actually play; menus and cutscene pauses
        // stay silent alongside combat.
        //
        // SPEAKER IDENTITY (sec 38, adversarial review blocker): [element+0x68] is
        // the SPEAKER's component (0x5A646A mov rbp,[rcx+0x68]; 0x5A64B5 mov rcx,rbp),
        // and for a player-bump element the speaker is the PEDESTRIAN being pushed,
        // not Wei. Speaking "Wei.move.001.X" through the NPC's component would play
        // Wei's line from the wrong character. Both paths must therefore use Wei's
        // own audio component, resolved from the player SimObject.
        bool in_combat = false;
        const bool combat_known = ReadPlayerCombatState(in_combat);
        if (!(combat_known && in_combat) && !IsGamePausedSafely()) {
            const push_voice_line::Decision decision =
                EvaluatePushVoiceLineGate();
            if (decision.fire) {
                const std::uintptr_t speaker = ResolvePlayerAudioComponent();
                if (speaker != 0) {
                    InvokePushVoiceLine(speaker);
                }
            } else if (want_diag &&
                       g_push_voice_line_diag_events.load(std::memory_order_relaxed) <
                           kPushVoiceLineDiagLogBudget) {
                log::InfoF("push_voice_line_suppressed cooldown=%u budget=%u",
                           decision.suppressed_by_cooldown ? 1u : 0u,
                           decision.suppressed_by_budget ? 1u : 0u);
            }
        }
    }
}

bool DetourConversationDispatcher(void* element,
                                  unsigned int reaction_id,
                                  unsigned int arg3,
                                  unsigned char arg4) {
    if (g_conversation_dispatcher_original == nullptr) {
        return false;
    }
    if (element == nullptr || g_push_voice_line_invoking ||
        !g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_conversation_dispatcher_original(element, reaction_id, arg3, arg4);
    }

    ObservePushVoiceLineElement(element, reaction_id, "base_dispatcher");

    return g_conversation_dispatcher_original(element, reaction_id, arg3, arg4);
}

// ScriptedConversationElement entry twin. The conversation system drives this
// virtual; it tail-jumps to the base dispatcher only when [element+0xa8]==0.
// Observing it here covers scripted push elements, which never reach the base
// dispatcher detour (round68 ledger section 27).
bool DetourScriptedConversationEntry(void* element,
                                     unsigned int reaction_id,
                                     unsigned int arg3,
                                     unsigned char arg4) {
    if (g_scripted_conversation_entry_original == nullptr) {
        return false;
    }
    if (element == nullptr || g_push_voice_line_invoking ||
        !g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        return g_scripted_conversation_entry_original(element, reaction_id, arg3, arg4);
    }

    ObservePushVoiceLineElement(element, reaction_id, "scripted_entry");

    return g_scripted_conversation_entry_original(element, reaction_id, arg3, arg4);
}


// Forces the engine's own spotlight shadow technique on one deferred-light
// render object after the stock selector declined it. Mirrors the stock
// change-detection shape: the technique slot at +0xA8 plus the dirty bit
// through the +0x78 pointer so the object re-binds. SEH-guarded: the object
// can be freed between selection and this write on render threads.
void ForceDeferredShadowTechnique(std::uint8_t* render_object) noexcept {
    __try {
        *reinterpret_cast<std::uint32_t*>(render_object +
            vehicle_light_shadow::kRenderTechniqueOffset) =
            vehicle_light_shadow::kDeferredShadowTechniqueId;
        const std::uintptr_t dirty_base =
            *reinterpret_cast<std::uintptr_t*>(render_object +
                vehicle_light_shadow::kRenderDirtyFlagPointerOffset);
        if (dirty_base != 0) {
            *reinterpret_cast<std::uint16_t*>(
                dirty_base + vehicle_light_shadow::kRenderDirtyFlagPointerOffset +
                reinterpret_cast<std::uintptr_t>(render_object)) |=
                vehicle_light_shadow::kRenderDirtyFlag;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_light_submit_flag_write_faults.fetch_add(1, std::memory_order_relaxed);
    }
}

void DetourLightSubmitSelector(void* render_object,
                               const void* light_record,
                               void* packed_entry) {
    if (g_light_submit_selector_original == nullptr) {
        return;
    }
    const bool feature_on =
        g_config.headlight_shadows || g_config.siren_shadows;
    const bool beam_on = g_config.headlight_beam_light;
    const bool diag_on = g_config.diag_light_submit_events;
    bool headlight_match = false;
    bool siren_match = false;
    bool beam_match = false;
    std::uint8_t light_type = 0;
    bool type_ok = false;
    const bool active =
        (feature_on || beam_on || diag_on) && render_object != nullptr &&
        light_record != nullptr && packed_entry != nullptr &&
        g_behavior_transaction_ready.load(std::memory_order_acquire);
    if (active) {
        g_light_submit_selector_calls.fetch_add(1, std::memory_order_relaxed);
        // Our components' record blocks are ours by identity; no type
        // heuristic, and both headlights count.
        beam_match = beam_on && IsBeamRecord(light_record);
        type_ok = SafeRead(reinterpret_cast<std::uintptr_t>(light_record) +
                               vehicle_light_shadow::kLightTypeOffset,
                           light_type);
        if (type_ok && feature_on) {
            headlight_match =
                g_config.headlight_shadows &&
                vehicle_light_shadow::ShouldForceShadow(
                    light_type, g_config.headlight_shadow_light_type);
            siren_match =
                g_config.siren_shadows &&
                vehicle_light_shadow::ShouldForceShadow(
                    light_type, g_config.siren_shadow_light_type);
        }
    }
    g_light_submit_selector_original(render_object, light_record, packed_entry);
    if (!active) {
        return;
    }
    std::uint32_t technique = 0;
    const bool technique_ok = SafeRead(
        reinterpret_cast<std::uintptr_t>(render_object) +
            vehicle_light_shadow::kRenderTechniqueOffset,
        technique);
    const bool matched = headlight_match || siren_match || beam_match;
    if (beam_match) {
        g_beam_submits.fetch_add(1, std::memory_order_relaxed);
    }
    if (matched && technique_ok &&
        vehicle_light_shadow::ShouldRewriteTechnique(technique)) {
        ForceDeferredShadowTechnique(static_cast<std::uint8_t*>(render_object));
        g_light_submit_shadows_forced.fetch_add(1, std::memory_order_relaxed);
        technique = vehicle_light_shadow::kDeferredShadowTechniqueId;
    } else if (matched && technique_ok) {
        g_light_submit_shadows_already_set.fetch_add(
            1, std::memory_order_relaxed);
    }
    if (diag_on) {
        const std::size_t type_slot =
            type_ok && light_type < kLightSubmitTypeSlots - 1
                ? static_cast<std::size_t>(light_type)
                : kLightSubmitTypeSlots - 1;
        g_light_submit_type_counts[type_slot].fetch_add(
            1, std::memory_order_relaxed);
        if (type_ok && technique_ok) {
            const std::uint32_t kind =
                (static_cast<std::uint32_t>(light_type) << 8) |
                static_cast<std::uint8_t>(technique);
            bool log_kind = false;
            {
                std::lock_guard<std::mutex> lock(g_light_submit_kind_mutex);
                log_kind = g_light_submit_kinds.insert(kind).second;
            }
            if (log_kind) {
                log::InfoF("light_submit_kind type=%u technique=0x%08X%s",
                           static_cast<unsigned>(light_type), technique,
                           matched ? " forced" : "");
            }
        }
        const std::uint64_t now_ms = GetTickCount64();
        std::uint64_t last =
            g_light_submit_last_summary_ms.load(std::memory_order_relaxed);
        if (now_ms - last >= 15000 &&
            g_light_submit_last_summary_ms.compare_exchange_strong(
                last, now_ms, std::memory_order_relaxed)) {
            log::InfoF(
                "light_submit_summary calls=%lu type0=%lu type1=%lu type2=%lu "
                "type3=%lu type4=%lu type5=%lu type6=%lu type7=%lu other=%lu "
                "forced=%lu already=%lu faults=%lu",
                g_light_submit_selector_calls.load(std::memory_order_relaxed),
                g_light_submit_type_counts[0].load(std::memory_order_relaxed),
                g_light_submit_type_counts[1].load(std::memory_order_relaxed),
                g_light_submit_type_counts[2].load(std::memory_order_relaxed),
                g_light_submit_type_counts[3].load(std::memory_order_relaxed),
                g_light_submit_type_counts[4].load(std::memory_order_relaxed),
                g_light_submit_type_counts[5].load(std::memory_order_relaxed),
                g_light_submit_type_counts[6].load(std::memory_order_relaxed),
                g_light_submit_type_counts[7].load(std::memory_order_relaxed),
                g_light_submit_type_counts[8].load(std::memory_order_relaxed),
                g_light_submit_shadows_forced.load(std::memory_order_relaxed),
                g_light_submit_shadows_already_set.load(
                    std::memory_order_relaxed),
                g_light_submit_flag_write_faults.load(
                    std::memory_order_relaxed));
        }
    }
}

// SEH-guarded memcpy of an engine object; the source lives on a render thread
// and can be freed between the list walk and the copy.
bool CopyEngineBlock(const void* source, void* out, std::size_t bytes) noexcept {
    __try {
        memcpy(out, source, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Reads the light-list head. The list is circular with the sentinel living at
// kLightListSentinelRva ({next at +0, prev at +8}); an empty list points the
// sentinel at itself.
bool ReadLightListHead(std::uintptr_t& head) noexcept {
    __try {
        head = *reinterpret_cast<std::uintptr_t*>(
            ResolveAddress(vehicle_beam_light::kLightListSentinelRva));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Walks the light list looking for a live spot component to clone. Returns the
// component base (node - 0x40) or null. Every hop is SEH-guarded: the list is
// mutated on render threads while we read it.
const std::uint8_t* FindSpotTemplateComponent(std::uint8_t& template_type) noexcept {
    const std::uintptr_t sentinel =
        ResolveAddress(vehicle_beam_light::kLightListSentinelRva);
    std::uintptr_t node = 0;
    if (!ReadLightListHead(node)) {
        return nullptr;
    }
    for (std::uint32_t hop = 0; hop < vehicle_beam_light::kMaxListWalk; ++hop) {
        if (node == 0 || node == sentinel) {
            return nullptr;
        }
        std::uintptr_t next = 0;
        std::uint8_t type = 0;
        __try {
            next = *reinterpret_cast<std::uintptr_t*>(node);
            type = *reinterpret_cast<std::uint8_t*>(
                node - vehicle_beam_light::kComponentNodeOffset +
                vehicle_beam_light::kComponentTypeOffset);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        if (vehicle_beam_light::IsSpotTemplate(type)) {
            template_type = type;
            return reinterpret_cast<const std::uint8_t*>(
                node - vehicle_beam_light::kComponentNodeOffset);
        }
        node = next;
    }
    return nullptr;
}

// True when our component is still linked in the engine's list. If the engine
// ever unlinks or frees it, the pointer is dead and must be dropped rather
// than reused.
bool ComponentStillLinked(const std::uint8_t* component) noexcept {
    const std::uintptr_t sentinel =
        ResolveAddress(vehicle_beam_light::kLightListSentinelRva);
    std::uintptr_t node = 0;
    if (component == nullptr || !ReadLightListHead(node)) {
        return false;
    }
    const std::uintptr_t own_node =
        reinterpret_cast<std::uintptr_t>(component) +
        vehicle_beam_light::kComponentNodeOffset;
    for (std::uint32_t hop = 0; hop < vehicle_beam_light::kMaxListWalk; ++hop) {
        if (node == 0 || node == sentinel) {
            return false;
        }
        if (node == own_node) {
            return true;
        }
        std::uintptr_t next = 0;
        __try {
            next = *reinterpret_cast<std::uintptr_t*>(node);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        node = next;
    }
    return false;
}

// Splices a component's light-list node in at the head, replicating the stock
// constructor byte for byte:
//   head->prev = node; node->next = head; node->prev = &sentinel; sentinel.next = node
bool SpliceIntoLightList(std::uint8_t* component) noexcept {
    __try {
        const std::uintptr_t sentinel =
            ResolveAddress(vehicle_beam_light::kLightListSentinelRva);
        std::uintptr_t* const sentinel_next =
            reinterpret_cast<std::uintptr_t*>(sentinel);
        std::uintptr_t* const node = reinterpret_cast<std::uintptr_t*>(
            component + vehicle_beam_light::kComponentNodeOffset);
        const std::uintptr_t first = *sentinel_next;
        *reinterpret_cast<std::uintptr_t*>(first + 8) =
            reinterpret_cast<std::uintptr_t>(node);   // first->prev = node
        node[0] = first;                              // node->next = first
        node[1] = sentinel;                           // node->prev = sentinel
        *sentinel_next = reinterpret_cast<std::uintptr_t>(node);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Creates the beam component once: allocate from the engine pool, clone a live
// spot component's 0xF0 bytes, re-initialise the two intrusive qNodes (the
// clone must not inherit the template's neighbours) and splice it into the
// light list.
//
// The clone deliberately skips the stock reflection registration
// (FUN_140190040): that call writes the object id into the parent's component
// table at [parent->table + index], and a clone has no parent slot of its own.
bool CreateBeamComponent(int slot) noexcept {
    if (slot < 0 || slot >= vehicle_beam_light::kBeamLightCount) {
        return false;
    }
    std::uint8_t template_type = 0;
    const std::uint8_t* template_component =
        FindSpotTemplateComponent(template_type);
    if (template_component == nullptr) {
        return false;
    }
    g_beam_template_type.store(static_cast<int>(template_type),
                               std::memory_order_relaxed);
    std::uintptr_t block = 0;
    __try {
        using PoolGetterFn = std::uintptr_t (*)();
        using AllocFn = void* (*)(std::uintptr_t, std::size_t, const char*,
                                  int, int);
        const auto pool_getter = reinterpret_cast<PoolGetterFn>(
            ResolveAddress(vehicle_beam_light::kComponentPoolGetterRva));
        const auto alloc = reinterpret_cast<AllocFn>(
            ResolveAddress(vehicle_beam_light::kComponentAllocRva));
        if (pool_getter == nullptr || alloc == nullptr) {
            return false;
        }
        // FUN_140190c90 returns the global pool pointer (DAT_14235c278) and
        // FUN_140166b60 dereferences it at +0x242c on its first instruction.
        // A null pool would be an instant fault, so check it here.
        const std::uintptr_t pool = pool_getter();
        if (pool == 0) {
            return false;
        }
        void* const raw = alloc(pool,
                                vehicle_beam_light::kComponentAllocSize,
                                "DynamicLightComponent", 0, 1);
        block = reinterpret_cast<std::uintptr_t>(raw);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (block == 0) {
        return false;
    }
    auto* const component = reinterpret_cast<std::uint8_t*>(block);
    // Zero the whole allocation first: the surplus past 0xF0 must read as
    // zero, not as recycled pool garbage, because the renderer may probe
    // fields out there on any object it finds in the list.
    __try {
        memset(component, 0, vehicle_beam_light::kComponentAllocSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!CopyEngineBlock(template_component, component,
                         vehicle_beam_light::kComponentSize)) {
        return false;
    }
    __try {
        // Same class, but pin the vtable explicitly: a wrong vtable is a crash
        // on the first virtual call.
        *reinterpret_cast<std::uintptr_t*>(
            component + vehicle_beam_light::kComponentVftableOffset) =
            ResolveAddress(vehicle_beam_light::kComponentVftableRva);
        // The SimComponent qNode at +0x30 must be self-linked. The clone would
        // otherwise point into the TEMPLATE's parent component list and corrupt
        // it on the next walk.
        std::uintptr_t* const self_node = reinterpret_cast<std::uintptr_t*>(
            component + vehicle_beam_light::kComponentSelfListOffset);
        self_node[0] = reinterpret_cast<std::uintptr_t>(self_node);
        self_node[1] = reinterpret_cast<std::uintptr_t>(self_node);
        // The light-list qNode is rewritten by the splice below; pre-seed it
        // self-linked so a failed splice leaves a harmless detached object.
        std::uintptr_t* const list_node = reinterpret_cast<std::uintptr_t*>(
            component + vehicle_beam_light::kComponentNodeOffset);
        list_node[0] = reinterpret_cast<std::uintptr_t>(list_node);
        list_node[1] = reinterpret_cast<std::uintptr_t>(list_node);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!SpliceIntoLightList(component)) {
        return false;
    }
    g_beam_components[slot] = component;
    g_beam_records[slot] = component + vehicle_beam_light::kRecordOffsetInComponent;
    log::InfoF("beam_light_component created=1 slot=%d template_type=%d "
               "component=0x%llx template=0x%llx",
               slot,
               static_cast<int>(template_type),
               static_cast<unsigned long long>(
                   reinterpret_cast<std::uintptr_t>(component)),
               static_cast<unsigned long long>(
                   reinterpret_cast<std::uintptr_t>(template_component)));
    return true;
}

// Copies the hero (player-attached) light's position out of this frame's
// record table. The table holds `component + 0x50` pointers built by the walk
// we are hooked around. `hero_record` receives the record address so the
// caller can dump its fields when the position looks wrong.
bool ReadHeroLightPosition(std::int64_t record_table,
                           float& x,
                           float& y,
                           float& z,
                           std::uintptr_t& hero_record) noexcept {
    __try {
        auto* const table = reinterpret_cast<std::uintptr_t*>(record_table);
        if (table == nullptr) {
            return false;
        }
        for (std::uint32_t index = 0; index < vehicle_beam_light::kMaxListWalk;
             ++index) {
            const std::uintptr_t record = table[index];
            if (record == 0) {
                break;
            }
            const auto* const bytes = reinterpret_cast<const std::uint8_t*>(record);
            if (bytes[vehicle_beam_light::kRecordTypeOffset] !=
                vehicle_beam_light::kLightTypeHero) {
                continue;
            }
            std::memcpy(&x, bytes + vehicle_beam_light::kRecordPositionOffset,
                        sizeof(x));
            std::memcpy(&y, bytes + vehicle_beam_light::kRecordPositionOffset + 4,
                        sizeof(y));
            std::memcpy(&z, bytes + vehicle_beam_light::kRecordPositionOffset + 8,
                        sizeof(z));
            hero_record = record;
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

// True when our component's record made it into this frame's record table,
// i.e. the walk admitted it. This is the difference between "the engine
// rejected the light" and "the collector filtered it out later".
bool ScanTableForRecord(std::int64_t record_table,
                        int total,
                        const std::uint8_t* record) noexcept {
    if (record_table == 0 || record == nullptr || total <= 0) {
        return false;
    }
    __try {
        auto* const table = reinterpret_cast<std::uintptr_t*>(record_table);
        const int limit = total < static_cast<int>(vehicle_beam_light::kMaxListWalk)
                              ? total
                              : static_cast<int>(vehicle_beam_light::kMaxListWalk);
        for (int index = 0; index < limit; ++index) {
            if (table[index] == reinterpret_cast<std::uintptr_t>(record)) {
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

// Reads the camera out of the collection job's context. This is the exact
// camera the collector gates every light against, so a light placed from it
// cannot disagree with the gate that judges it.
//
// Session 4 disproved the previous anchor: the view object returned by
// FUN_14006aa70 has [0.38 0 0] at +0x30, which is not a world position (the
// player was at [935 -157 26]). Anchoring there parked the beam at the level
// origin and the collector culled it every frame: submits=0.
bool ReadCollectorCamera(const std::uint32_t* context,
                         float& pos_x,
                         float& pos_y,
                         float& pos_z,
                         float& forward_x,
                         float& forward_y,
                         float& forward_z) noexcept {
    if (context == nullptr) {
        return false;
    }
    __try {
        const auto* const bytes = reinterpret_cast<const std::uint8_t*>(context);
        std::memcpy(&pos_x, bytes + vehicle_beam_light::kCtxCameraPosOffset, 4);
        std::memcpy(&pos_y, bytes + vehicle_beam_light::kCtxCameraPosOffset + 4, 4);
        std::memcpy(&pos_z, bytes + vehicle_beam_light::kCtxCameraPosOffset + 8, 4);
        std::memcpy(&forward_x, bytes + vehicle_beam_light::kCtxCameraForwardOffset, 4);
        std::memcpy(&forward_y, bytes + vehicle_beam_light::kCtxCameraForwardOffset + 4, 4);
        std::memcpy(&forward_z, bytes + vehicle_beam_light::kCtxCameraForwardOffset + 8, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// FUN_14004e230 RETURNS the record count in EAX and the orchestrator passes it
// to the collection job as the number of entries to walk. This detour MUST
// return it: a void detour leaves EAX holding whatever the last call happened
// to produce, the job then walks past the end of the record table and dies on
// the first garbage pointer (crash at 0x14007df80, session 2026-10-22 19:20).
int DetourRecordTableBuilder(std::int64_t record_table,
                             std::int64_t texture_table) {
    if (g_record_table_builder_original == nullptr) {
        return 0;
    }
    const bool beam_on = g_config.headlight_beam_light &&
                         g_behavior_transaction_ready.load(
                             std::memory_order_acquire);
    if (beam_on) {
        g_beam_frames_seen.fetch_add(1, std::memory_order_relaxed);
        for (int slot = 0; slot < vehicle_beam_light::kBeamLightCount; ++slot) {
            if (g_beam_components[slot] != nullptr &&
                !ComponentStillLinked(g_beam_components[slot])) {
                // The engine dropped our object (level change, list rebuild).
                // Do not touch the stale pointer again.
                g_beam_components[slot] = nullptr;
                g_beam_records[slot] = nullptr;
            }
            if (g_beam_components[slot] == nullptr &&
                !g_beam_create_attempted[slot].exchange(true)) {
                if (!CreateBeamComponent(slot)) {
                    g_beam_append_faults.fetch_add(1,
                                                   std::memory_order_relaxed);
                    log::InfoF(
                        "beam_light_component created=0 slot=%d "
                        "reason=no_template",
                        slot);
                }
            }
        }
        // Placement is done in the collector detour below, where the camera
        // the gates actually use is available. The builder's only job is to
        // make sure the components exist before the walk runs, so the records
        // are in this frame's table.
    }
    const int total = g_record_table_builder_original(record_table,
                                                      texture_table);
    if (!beam_on) {
        return total;
    }
    float hero_x = 0.0f;
    float hero_y = 0.0f;
    float hero_z = 0.0f;
    std::uintptr_t hero_record = 0;
    const bool hero_found = ReadHeroLightPosition(record_table, hero_x, hero_y,
                                                  hero_z, hero_record);
    if (hero_found) {
        g_beam_tracker.ObserveHeroPosition(hero_x, hero_y, hero_z);
        g_beam_hero_found.fetch_add(1, std::memory_order_relaxed);
        g_beam_last_hero_pos[0] = hero_x;
        g_beam_last_hero_pos[1] = hero_y;
        g_beam_last_hero_pos[2] = hero_z;
    }
    {
        int in_table = 0;
        for (int slot = 0; slot < vehicle_beam_light::kBeamLightCount; ++slot) {
            if (g_beam_records[slot] != nullptr &&
                ScanTableForRecord(record_table, total, g_beam_records[slot])) {
                ++in_table;
            }
        }
        g_beam_in_table.store(in_table, std::memory_order_relaxed);
    }
    // One-shot dump of the hero record so a wrong anchor is diagnosable from
    // the log instead of by guessing: the record layout is the same for every
    // type, so a type-3 record whose +0x30 is not a world position means the
    // anchor has to come from somewhere else.
    if (hero_found && !g_beam_probe_done.exchange(true)) {
        const auto* const bytes = reinterpret_cast<const std::uint8_t*>(hero_record);
        float field[12] = {};
        __try {
            std::memcpy(field, bytes, sizeof(field));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(field, 0, sizeof(field));
        }
        // A known-good reference point: the first type-1 spot record in the
        // same table. Every record here passed the same admission test, so if
        // the hero position looks nothing like a world position while this one
        // does, the anchor is the problem, not the component.
        float lamp_pos[3] = {0.0f, 0.0f, 0.0f};
        float lamp_normal[3] = {0.0f, 0.0f, 0.0f};
        float lamp_radius = 0.0f;
        float lamp_fov = 0.0f;
        __try {
            auto* const table = reinterpret_cast<std::uintptr_t*>(record_table);
            const int limit = total < static_cast<int>(vehicle_beam_light::kMaxListWalk)
                                  ? total
                                  : static_cast<int>(vehicle_beam_light::kMaxListWalk);
            for (int index = 0; index < limit; ++index) {
                const auto* const candidate =
                    reinterpret_cast<const std::uint8_t*>(table[index]);
                if (candidate == nullptr ||
                    candidate[vehicle_beam_light::kRecordTypeOffset] !=
                        vehicle_beam_light::kLightTypeSpot) {
                    continue;
                }
                std::memcpy(lamp_pos, candidate + vehicle_beam_light::kRecordPositionOffset,
                            sizeof(lamp_pos));
                std::memcpy(lamp_normal, candidate + vehicle_beam_light::kRecordBasisOffset + 0x20,
                            sizeof(lamp_normal));
                std::memcpy(&lamp_radius, candidate + vehicle_beam_light::kRecordRadiusOffset,
                            sizeof(lamp_radius));
                std::memcpy(&lamp_fov, candidate + vehicle_beam_light::kRecordFovOuterOffset,
                            sizeof(lamp_fov));
                break;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(lamp_pos, 0, sizeof(lamp_pos));
            memset(lamp_normal, 0, sizeof(lamp_normal));
        }
        log::InfoF(
            "beam_light_probe hero_record=0x%llx type=%u "
            "basis=[%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f "
            "%.2f] pos=[%.2f %.2f %.2f] lamp_pos=[%.2f %.2f %.2f] "
            "lamp_normal=[%.2f %.2f %.2f] lamp_radius=%.2f lamp_fov=%.3f "
            "table_total=%d in_table=%d",
            static_cast<unsigned long long>(hero_record),
            static_cast<unsigned>(bytes[vehicle_beam_light::kRecordTypeOffset]),
            field[0], field[1], field[2], field[3], field[4], field[5], field[6],
            field[7], field[8], field[9], field[10], field[11], hero_x, hero_y,
            hero_z, lamp_pos[0], lamp_pos[1], lamp_pos[2], lamp_normal[0],
            lamp_normal[1], lamp_normal[2], lamp_radius, lamp_fov, total,
            g_beam_in_table.load(std::memory_order_relaxed));
    }
    g_beam_last_total = total > 0 ? static_cast<std::uint32_t>(total) : 0;
    LogBeamSummaryMaybe(g_beam_last_total, 0, 0, 0, hero_found,
                        g_beam_records[0] != nullptr);
    return total;
}

// Places and aims the beam immediately before the collector walks the table.
//
// The record's position is the cone's BASE CENTRE and its third basis row is
// the cone axis: both collector gates step backwards along that axis to find
// the apex and the bounding sphere they frustum-test. A clone of a street lamp
// therefore inherits a DOWNWARD axis, and the test sphere lands tens of metres
// in the air - which is why the beam was admitted for 35 frames in session 3
// and dropped for the rest.
//
// Here the axis is set to the camera's own forward vector and the base centre
// is pushed ahead of the camera along it, so the cone opens away from the
// player and the sphere sits behind the base centre, on the camera side.
void PlaceBeamFromCollectorContext(const std::uint32_t* context) {
    bool have_any = false;
    for (int slot = 0; slot < vehicle_beam_light::kBeamLightCount; ++slot) {
        have_any = have_any || g_beam_records[slot] != nullptr;
    }
    if (!have_any) {
        return;
    }
    float cam_x = 0.0f;
    float cam_y = 0.0f;
    float cam_z = 0.0f;
    float cam_fwd_x = 0.0f;
    float cam_fwd_y = 0.0f;
    float cam_fwd_z = 0.0f;
    if (!ReadCollectorCamera(context, cam_x, cam_y, cam_z, cam_fwd_x,
                             cam_fwd_y, cam_fwd_z)) {
        return;
    }
    if (!vehicle_beam_light::IsPlausibleWorldPosition(cam_x, cam_y, cam_z)) {
        return;
    }
    const float cam_fwd_len = std::sqrt(cam_fwd_x * cam_fwd_x +
                                        cam_fwd_y * cam_fwd_y +
                                        cam_fwd_z * cam_fwd_z);
    if (!(cam_fwd_len > 0.5f && cam_fwd_len < 2.0f)) {
        return;  // not a unit direction; leave the records alone this frame
    }
    cam_fwd_x /= cam_fwd_len;
    cam_fwd_y /= cam_fwd_len;
    cam_fwd_z /= cam_fwd_len;
    g_beam_cached_camera[0] = cam_x;
    g_beam_cached_camera[1] = cam_y;
    g_beam_cached_camera[2] = cam_z;
    g_beam_cached_camera_valid.store(true, std::memory_order_relaxed);

    // A headlight on a pedestrian is not the feature: park the lights far out
    // of frustum whenever the player is not driving. A light that far outside
    // the view cone fails the collector's distance gate and is simply not
    // submitted, so this is a safe way to switch it off.
    const int drive_state = g_beam_drive_state.load(std::memory_order_relaxed);
    const bool anchored = g_beam_anchor_valid.load(std::memory_order_relaxed);
    if (drive_state == 2) {
        __try {
            for (int slot = 0; slot < vehicle_beam_light::kBeamLightCount;
                 ++slot) {
                if (g_beam_records[slot] == nullptr) {
                    continue;
                }
                vehicle_beam_light::ParkBeamRecord(
                    g_beam_records[slot], cam_x, cam_y,
                    cam_z - vehicle_beam_light::kBeamParkDropMeters);
            }
            g_beam_parked.store(1, std::memory_order_relaxed);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_beam_append_faults.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    vehicle_beam_light::BeamTuning tuning{};
    tuning.radius = std::clamp(g_config.headlight_beam_radius,
                               vehicle_beam_light::kBeamRadiusMin,
                               vehicle_beam_light::kBeamRadiusMax);
    tuning.color_r = g_config.headlight_beam_color_r;
    tuning.color_g = g_config.headlight_beam_color_g;
    tuning.color_b = g_config.headlight_beam_color_b;
    // Base point and heading, decided by a pure function so the direction is
    // unit-tested rather than argued about.
    //
    // The anchor contributes a DISTANCE only. Deriving the direction from it
    // has now failed twice - `look - eye` (undocumented argument order) and
    // `anchor - camera` (inverts when the anchor sits behind the camera) - and
    // both times the tell was the same: growing the offset pushed the beam
    // further BEHIND the car. The collector camera's forward cannot be
    // inverted, so it is the only direction used.
    float anchor_distance = 0.0f;
    if (anchored) {
        const float ax = g_beam_anchor_pos[0] - cam_x;
        const float ay = g_beam_anchor_pos[1] - cam_y;
        const float az = g_beam_anchor_pos[2] - cam_z;
        anchor_distance = std::sqrt(ax * ax + ay * ay + az * az);
    }
    const vehicle_beam_light::BeamHeading heading =
        vehicle_beam_light::ResolveBeamHeading(cam_fwd_x, cam_fwd_y,
                                               anchor_distance, anchored);
    float dir_x = heading.dir_x;
    float dir_y = heading.dir_y;
    // Horizontal on purpose: the offset is tens of metres now, and pushing it
    // along a pitched camera vector would drive the light underground.
    const float dir_z = 0.0f;
    float lead = heading.lead;
    float drop = vehicle_beam_light::kBeamVehicleDropMeters;
    float ahead = vehicle_beam_light::kBeamAheadMeters;
    if (heading.from_anchor) {
        // The record is the cone's BASE and the cone runs BACKWARDS to the
        // apex, so the base must sit a full cone height beyond where the beam
        // should start.
        float fov = 0.0f;
        __try {
            std::memcpy(&fov,
                        g_beam_records[0] +
                            vehicle_beam_light::kRecordFovOuterOffset,
                        sizeof(fov));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            fov = 0.0f;
        }
        ahead = lead +
                vehicle_beam_light::BeamRecordAheadOffset(tuning.radius, fov);
    }
    g_beam_last_lead = lead;
    g_beam_cached_forward[0] = dir_x;
    g_beam_cached_forward[1] = dir_y;
    g_beam_cached_forward[2] = dir_z;
    // Everything is measured from the camera now: `ahead` already carries the
    // lead out to the car plus the cone's own height.
    const float base_x = cam_x;
    const float base_y = cam_y;
    const float base_z = cam_z;
    // Right vector on the ground plane: cross(heading, world up).
    float right[2] = {0.0f, 0.0f};
    vehicle_beam_light::BeamRightVector(dir_x, dir_y, right);
    const float right_x = right[0];
    const float right_y = right[1];

    __try {
        for (int slot = 0; slot < vehicle_beam_light::kBeamLightCount; ++slot) {
            std::uint8_t* const record = g_beam_records[slot];
            if (record == nullptr) {
                continue;
            }
            // Alternate sides: a car has two headlights, symmetric about the
            // centre line.
            const float lateral =
                (slot == 0 ? -1.0f : 1.0f) *
                vehicle_beam_light::kBeamHeadlightLateralMeters;
            vehicle_beam_light::PatchBeamRecordAt(
                record, base_x + right_x * lateral,
                base_y + right_y * lateral, base_z, dir_x, dir_y, dir_z, ahead,
                drop, tuning);
            vehicle_beam_light::WriteAimBasis(record, dir_x, dir_y, dir_z);
            if (slot == 0) {
                g_beam_last_our_pos[0] = base_x + right_x * lateral +
                                         dir_x * ahead;
                g_beam_last_our_pos[1] = base_y + right_y * lateral +
                                         dir_y * ahead;
                g_beam_last_our_pos[2] = base_z + dir_z * ahead - drop;
            }
        }
        g_beam_frames_appended.fetch_add(1, std::memory_order_relaxed);
        g_beam_parked.store(0, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_beam_append_faults.fetch_add(1, std::memory_order_relaxed);
    }
}

// One-shot dump of everything the two collector gates read, so the next
// session answers "which gate rejects it, and by how much" with numbers
// instead of another hypothesis.
void LogBeamGateProbe(const std::uint32_t* context,
                      float cam_x,
                      float cam_y,
                      float cam_z,
                      float fwd_x,
                      float fwd_y,
                      float fwd_z) noexcept {
    if (context == nullptr || g_beam_records[0] == nullptr) {
        return;
    }
    float our_pos[3] = {0.0f, 0.0f, 0.0f};
    float our_normal[3] = {0.0f, 0.0f, 0.0f};
    float our_radius = 0.0f;
    float our_fov_in = 0.0f;
    float our_fov_out = 0.0f;
    float params[8] = {};
    float planes[24] = {};
    __try {
        std::memcpy(our_pos, g_beam_records[0] + vehicle_beam_light::kRecordPositionOffset,
                    sizeof(our_pos));
        std::memcpy(our_normal,
                    g_beam_records[0] + vehicle_beam_light::kRecordBasisOffset + 0x20,
                    sizeof(our_normal));
        std::memcpy(&our_radius, g_beam_records[0] + vehicle_beam_light::kRecordRadiusOffset,
                    sizeof(our_radius));
        std::memcpy(&our_fov_in, g_beam_records[0] + vehicle_beam_light::kRecordFovInnerOffset,
                    sizeof(our_fov_in));
        std::memcpy(&our_fov_out, g_beam_records[0] + vehicle_beam_light::kRecordFovOuterOffset,
                    sizeof(our_fov_out));
        const auto* const bytes = reinterpret_cast<const std::uint8_t*>(context);
        // param_3[2..8] then param_3[0x2b] - the scalars the distance gates use
        std::memcpy(&params[0], bytes + 0x08, 4);
        std::memcpy(&params[1], bytes + 0x0C, 4);
        std::memcpy(&params[2], bytes + 0x10, 4);
        std::memcpy(&params[3], bytes + 0x14, 4);
        std::memcpy(&params[4], bytes + 0x18, 4);
        std::memcpy(&params[5], bytes + 0x1C, 4);
        std::memcpy(&params[6], bytes + 0x20, 4);
        std::memcpy(&params[7], bytes + 0xAC, 4);
        std::memcpy(planes, bytes + vehicle_beam_light::kCtxPlanesOffset,
                    sizeof(planes));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    log::InfoF(
        "beam_gate_probe cam=[%.2f %.2f %.2f] fwd=[%.3f %.3f %.3f] "
        "our_pos=[%.2f %.2f %.2f] our_normal=[%.3f %.3f %.3f] "
        "our_radius=%.2f fov_in=%.3f fov_out=%.3f",
        cam_x, cam_y, cam_z, fwd_x, fwd_y, fwd_z, our_pos[0], our_pos[1],
        our_pos[2], our_normal[0], our_normal[1], our_normal[2], our_radius,
        our_fov_in, our_fov_out);
    log::InfoF(
        "beam_gate_params p2=%.3f p3=%.3f p4=%.3f p5=%.3f p6=%.3f p7=%.3f "
        "p8=%.3f p2b=%.3f",
        params[0], params[1], params[2], params[3], params[4], params[5],
        params[6], params[7]);
    log::InfoF(
        "beam_gate_planes "
        "p0=[%.4f %.4f %.4f %.3f] p1=[%.4f %.4f %.4f %.3f] "
        "p2=[%.4f %.4f %.4f %.3f] p3=[%.4f %.4f %.4f %.3f] "
        "p4=[%.4f %.4f %.4f %.3f] p5=[%.4f %.4f %.4f %.3f]",
        planes[0], planes[1], planes[2], planes[3], planes[4], planes[5],
        planes[6], planes[7], planes[8], planes[9], planes[10], planes[11],
        planes[12], planes[13], planes[14], planes[15], planes[16], planes[17],
        planes[18], planes[19], planes[20], planes[21], planes[22], planes[23]);
}

// The collector stores its four batch counts indirectly: the context holds
// POINTERS at 0xC8/0xD0/0xD8/0xE0 and the counts live where they point
// (`**(uint**)(ctx + 0xC8) = groupA` at the end of FUN_14007dd40). Reading the
// pointers is the only way to see the collector's verdict from outside.
bool ReadCollectorGroupCounts(const std::uint32_t* context,
                              unsigned long out[4]) noexcept {
    if (context == nullptr) {
        return false;
    }
    __try {
        for (int index = 0; index < 4; ++index) {
            const std::uintptr_t slot =
                reinterpret_cast<std::uintptr_t>(context) + 0xC8 +
                static_cast<std::uintptr_t>(index) * 8;
            const auto* const pointer =
                *reinterpret_cast<const std::uint32_t* const*>(slot);
            if (pointer == nullptr) {
                return false;
            }
            out[index] = *pointer;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void DetourLightCollector(std::int64_t arg1,
                          std::int64_t arg2,
                          std::uint32_t* context) {
    if (g_light_collector_original == nullptr) {
        return;
    }
    const bool beam_on = g_config.headlight_beam_light &&
                         g_behavior_transaction_ready.load(
                             std::memory_order_acquire);
    if (beam_on && (g_beam_records[0] != nullptr ||
                    g_beam_records[1] != nullptr)) {
        PlaceBeamFromCollectorContext(context);
        if (!g_beam_gate_probe_done.exchange(true)) {
            float cam_x = 0.0f;
            float cam_y = 0.0f;
            float cam_z = 0.0f;
            float fwd_x = 0.0f;
            float fwd_y = 0.0f;
            float fwd_z = 0.0f;
            if (ReadCollectorCamera(context, cam_x, cam_y, cam_z, fwd_x, fwd_y,
                                    fwd_z)) {
                LogBeamGateProbe(context, cam_x, cam_y, cam_z, fwd_x, fwd_y,
                                 fwd_z);
            }
        }
    }
    g_light_collector_original(arg1, arg2, context);
    if (beam_on) {
        unsigned long counts[4] = {0, 0, 0, 0};
        if (ReadCollectorGroupCounts(context, counts)) {
            g_beam_group_a.store(counts[0], std::memory_order_relaxed);
            g_beam_group_b.store(counts[1], std::memory_order_relaxed);
            g_beam_group_c.store(counts[2], std::memory_order_relaxed);
            g_beam_group_d.store(counts[3], std::memory_order_relaxed);
        }
    }
}

// Periodic beam summary: whole-session visibility for the component path.
// `frames_appended` is now the per-frame update count (should track the frame
// rate), `lamp` reports whether our component exists, and `last_total` is the
// size of the record table the builder produced.
void LogBeamSummaryMaybe(std::uint32_t total,
                         std::uint32_t c0,
                         std::uint32_t d0,
                         std::uint32_t e0,
                         bool hero_found,
                         bool lamp_found) noexcept {
    const std::uint64_t now_ms = GetTickCount64();
    std::uint64_t last =
        g_beam_last_summary_ms.load(std::memory_order_relaxed);
    if (now_ms - last < 15000 ||
        !g_beam_last_summary_ms.compare_exchange_strong(
            last, now_ms, std::memory_order_relaxed)) {
        return;
    }
    // Live fields of our own record, read here rather than assumed: a record
    // that is in the table with the wrong radius or a stale axis is a
    // different failure from one that is simply rejected, and the summary has
    // to distinguish them.
    float our_radius = 0.0f;
    float our_normal[3] = {0.0f, 0.0f, 0.0f};
    unsigned our_clip = 0;
    if (g_beam_records[0] != nullptr) {
        __try {
            std::memcpy(&our_radius,
                        g_beam_records[0] +
                            vehicle_beam_light::kRecordRadiusOffset,
                        sizeof(our_radius));
            std::memcpy(our_normal,
                        g_beam_records[0] +
                            vehicle_beam_light::kRecordBasisOffset + 0x20,
                        sizeof(our_normal));
            our_clip = g_beam_records[0]
                           [vehicle_beam_light::kRecordClipPlaneCountOffset];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            our_radius = 0.0f;
            our_normal[0] = our_normal[1] = our_normal[2] = 0.0f;
            our_clip = 0;
        }
    }
    // Read BOTH records back out of memory. Until now the summary logged
    // g_beam_last_our_pos, which is what the placement code MEANT to write -
    // so a slot that was never updated, or a write that did not land, would
    // have looked healthy. Two lights are visible-or-not independently, so
    // they have to be reported independently.
    float r0_pos[3] = {0.0f, 0.0f, 0.0f};
    float r1_pos[3] = {0.0f, 0.0f, 0.0f};
    __try {
        if (g_beam_records[0] != nullptr) {
            std::memcpy(r0_pos,
                        g_beam_records[0] +
                            vehicle_beam_light::kRecordPositionOffset,
                        sizeof(r0_pos));
        }
        if (g_beam_records[1] != nullptr) {
            std::memcpy(r1_pos,
                        g_beam_records[1] +
                            vehicle_beam_light::kRecordPositionOffset,
                        sizeof(r1_pos));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r0_pos[0] = r0_pos[1] = r0_pos[2] = 0.0f;
        r1_pos[0] = r1_pos[1] = r1_pos[2] = 0.0f;
    }
    // Where the light actually sits relative to the camera, along its own aim
    // axis. Negative means the cone is behind the camera - which is exactly
    // the "I can only see it with the look-back camera" report, and it should
    // never be something I have to infer from a screenshot again.
    // Projected onto the CAMERA's forward, not the record's own axis. The old
    // version used our_normal, which made the result positive by construction
    // whatever the light was doing.
    float ahead_of_cam = 0.0f;
    if (g_beam_cached_camera_valid.load(std::memory_order_relaxed)) {
        ahead_of_cam =
            (r0_pos[0] - g_beam_cached_camera[0]) * g_beam_cached_forward[0] +
            (r0_pos[1] - g_beam_cached_camera[1]) * g_beam_cached_forward[1] +
            (r0_pos[2] - g_beam_cached_camera[2]) * g_beam_cached_forward[2];
    }
    log::InfoF(
        "beam_light_summary frames_appended=%lu faults=%lu last_total=%u "
        "component=%u template_type=%d hero=%u hero_total=%lu seen=%lu "
        "submits=%lu in_table=%d hero_pos=[%.2f %.2f %.2f] "
        "our_pos=[%.2f %.2f %.2f] drive_state=%d parked=%d "
        "anchor=[%.2f %.2f %.2f] our_radius=%.2f "
        "our_normal=[%.3f %.3f %.3f] clip=%u groups=%lu/%lu/%lu/%lu "
        "lead=%.1f ahead_of_cam=%.1f cam=[%.2f %.2f %.2f] "
        "r0=[%.2f %.2f %.2f] r1=[%.2f %.2f %.2f]",
        g_beam_frames_appended.load(std::memory_order_relaxed),
        g_beam_append_faults.load(std::memory_order_relaxed),
        total,
        lamp_found ? 1u : 0u,
        g_beam_template_type.load(std::memory_order_relaxed),
        hero_found ? 1u : 0u,
        g_beam_hero_found.load(std::memory_order_relaxed),
        g_beam_frames_seen.load(std::memory_order_relaxed),
        g_beam_submits.load(std::memory_order_relaxed),
        g_beam_in_table.load(std::memory_order_relaxed),
        g_beam_last_hero_pos[0], g_beam_last_hero_pos[1], g_beam_last_hero_pos[2],
        g_beam_last_our_pos[0], g_beam_last_our_pos[1], g_beam_last_our_pos[2],
        g_beam_drive_state.load(std::memory_order_relaxed),
        g_beam_parked.load(std::memory_order_relaxed),
        g_beam_anchor_pos[0], g_beam_anchor_pos[1], g_beam_anchor_pos[2],
        our_radius, our_normal[0], our_normal[1], our_normal[2], our_clip,
        g_beam_group_a.load(std::memory_order_relaxed),
        g_beam_group_b.load(std::memory_order_relaxed),
        g_beam_group_c.load(std::memory_order_relaxed),
        g_beam_group_d.load(std::memory_order_relaxed),
        g_beam_last_lead, ahead_of_cam, g_beam_cached_camera[0],
        g_beam_cached_camera[1], g_beam_cached_camera[2], r0_pos[0], r0_pos[1],
        r0_pos[2], r1_pos[0], r1_pos[1], r1_pos[2]);
    (void)c0;
    (void)d0;
    (void)e0;
}

// Returns the live player SimObject, or 0 when the player is not yet spawned.// Returns the live player SimObject, or 0 when the player is not yet spawned.
// Read under SEH: the slot is cleared during load and level transitions.
std::uintptr_t TryReadPlayerSimObject() noexcept {
    const std::uintptr_t slot = ResolveAddress(kPlayerSimObjectGlobalRva);
    if (slot == 0) {
        return 0;
    }
    std::uintptr_t player = 0;
    __try {
        SafeRead(slot, player);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        player = 0;
    }
    return player;
}

// True when this damage event was caused by the player. The engine makes the
// same comparison inside the hooked function (attacker vs *0x235C488); we repeat
// it in the detour to gate the restored line.
bool IsPlayerAttacker(void* attacker) noexcept {
    const std::uintptr_t player = TryReadPlayerSimObject();
    return player != 0 && reinterpret_cast<std::uintptr_t>(attacker) == player;
}



// Offsets into SimObject's component array, read off GetComponent (0x190AD0):
//   [sim+0x60] = count, [sim+0x68] = array of 16-byte entries {ptr, uid}.
constexpr std::uintptr_t kSimObjectComponentCountOffset = 0x60;
constexpr std::uintptr_t kSimObjectComponentArrayOffset = 0x68;
constexpr std::uintptr_t kComponentEntryStride = 0x10;
constexpr std::uint32_t kMaxComponentScan = 512;

// True when this component's vftable is ActorAudioComponent's.
// The vftable is a static .rdata address, so this is a real CLASS check - unlike a
// type uid, which the engine assigns at runtime and never stores.
bool HasActorAudioVftable(std::uintptr_t component) noexcept {
    if (component == 0 || g_module_base == 0) {
        return false;
    }
    std::uintptr_t vftable = 0;
    __try {
        if (!SafeRead(component, vftable)) {
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (vftable <= g_module_base) {
        return false;
    }
    const std::uintptr_t expected_vftable_rva =
        g_use_latest_steam_layout
            ? kLatestSteamActorAudioComponentVftableRva
            : kActorAudioComponentVftableRva;
    return (vftable - g_module_base) == expected_vftable_rva;
}

// Operational test for an ActorAudioComponent, taken from the speech wrapper's own
// guards (0x5A8E20 / 0x5A8930): [+0x168] != 0 (speech enabled) and [+0x1BC] > 0
// (voice count). The wrapper rejects anything failing either, so matching on the
// guards is stricter than matching on a type uid.
bool IsPlausibleAudioComponent(std::uintptr_t component) noexcept {
    if (component == 0) {
        return false;
    }
    // Class check first: the guards below can pass coincidentally on
    // unrelated components (latest layout 2026-10-30: an impostor got
    // returned to the cop-bark invoke and faulted 0xC0000005 inside the
    // game's speak body). The resolver contract is "a genuine
    // ActorAudioComponent or 0, never an impostor" - enforce it here.
    if (!HasActorAudioVftable(component)) {
        return false;
    }
    std::uint8_t speech_enabled = 0;
    std::int32_t voice_count = 0;
    std::uintptr_t audio_object = 0;
    __try {
        if (!SafeRead(component + kSpeechEnabledOffset, speech_enabled) ||
            !SafeRead(component + kSpeechVoiceCountOffset, voice_count) ||
            !SafeRead(component + kSpeechGuardObjectOffset, audio_object)) {
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    // Three guards, all checked by the wrapper before it does any work
    // (0x5A8E4A / 0x5A8E62 / 0x5A8E6B-0x5A8E75): speech enabled, voice count > 0,
    // and a non-null audio object it goes on to dereference. Requiring the third
    // matters: bytes at +0x168 / +0x1BC can be coincidentally non-zero on unrelated
    // components, and the probe returns the FIRST match, so a two-guard test can
    // select the wrong component (sec 38).
    if (speech_enabled == 0 || voice_count <= 0 || audio_object == 0) {
        return false;
    }
    // The audio object must be readable, since the wrapper dereferences it.
    std::uintptr_t probe = 0;
    __try {
        if (!SafeRead(audio_object, probe)) {
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
 }

// ---- reading a speaker's own voice tag ------------------------------------
//
// The by-name speech entry (0x5A8930) composes "<voice_tag>.<action>.001."
// from the TARGET COMPONENT's own voice tag and picks the variation itself
// (ledger sec 41 ground truth, confirmed by the shipped NPC voice lines
// speaking through 35+ NPC components). So to play a SPECIFIC voice's family
// the engine must be handed that voice's own component; there is no separate
// "speak as voice X" argument.
//
// Chain: [+0x1A0] is the component's voice profile, and [+0x48] on that
// profile is the tag C-string. Both hops are the ones the speech path itself
// walks before queueing a line, and the sep28 recovery snapshot read them
// from the live component (Hooks.cpp.sep28 IsPlausibleAudioComponent), so this
// is a known-shaped read rather than a guessed offset.
//
// HONEST LIMIT: this is the only route to a speaker's voice tag that the
// project has evidence for. It is not a per-ped identity field, so a
// component whose profile is not populated yet yields no tag - and a tag this
// read cannot supply must NOT be guessed. Callers fall back to Wei.
constexpr std::uintptr_t kSpeechVoiceProfileOffset = 0x1A0;
constexpr std::uintptr_t kVoiceProfileTagOffset = 0x48;
// Longest tag in the roster is "M_ZOD_ChineseThug1" (18); 32 leaves headroom
// and keeps the read bounded on a hostile pointer.
constexpr std::size_t kVoiceTagReadCapacity = 32;

// Reads a component's voice tag. Returns false on any unreadable hop, a null
// pointer at either hop, or a tag that does not fit the capacity - every case
// the caller must treat as "voice unknown" rather than as Wei.
bool TryReadVoiceTag(std::uintptr_t component, char* out_tag, std::size_t capacity) noexcept {
    if (component == 0 || out_tag == nullptr || capacity == 0) {
        return false;
    }
    out_tag[0] = '\0';
    std::uintptr_t profile = 0;
    std::uintptr_t tag = 0;
    __try {
        if (!SafeRead(component + kSpeechVoiceProfileOffset, profile) || profile == 0 ||
            !SafeRead(profile + kVoiceProfileTagOffset, tag) || tag == 0) {
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return SafeReadCString(tag, out_tag, capacity);
}


// Resolve Wei's ActorAudioComponent.
//
// Two independent routes, in preference order:
//
//  1. SIGNATURE PROBE (default). The speech guards define an ActorAudioComponent
//     operationally: [+0x168] != 0 (speech enabled) and [+0x1BC] > 0 (voice count).
//     Anything satisfying both IS the component the wrapper will accept, so this
//     needs no type-uid knowledge at all.
//
//  2. There is no type-uid route. Component type uids are assigned at runtime and
//     appear nowhere in the binary, so no uid can be known. A uid route would also
//     fail silently (GetComponent returns 0 and the line no-ops), which is exactly
//     the class of bug this feature already suffered from, so it is not offered.
//
// Sec 37: the player's own component is required - the victim's would play an NPC
// reaction instead of Wei's line.
// One-shot diagnostic: log every component uid attached to the player SimObject.
//
// SimObject layout, read off GetComponent (0x190AD0):
//   [sim+0x60] = component count
//   [sim+0x68] = pointer to an array of 16-byte entries
//   entry+0x00 = component pointer
//   entry+0x08 = uid dword
// A uid matches a request when the high 7 bits are equal and the low 25 bits of the
// request are a subset of the stored value, so the high 7 bits are the component
// TYPE TAG and the low 25 bits are subtype flags.
//
// This exists because no component type uid is knowable: type uids are assigned at
// runtime by the engine, so the only way to see them is to enumerate the live array.
// One live run tells us what the player actually has attached.
void LogPlayerComponentUids() noexcept {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    const std::uintptr_t player = TryReadPlayerSimObject();
    if (player == 0) {
        log::InfoF("component_uid_scan player=0");
        return;
    }
    std::uint32_t count = 0;
    std::uintptr_t array = 0;
    __try {
        if (!SafeRead(player + kSimObjectComponentCountOffset, count) ||
            !SafeRead(player + kSimObjectComponentArrayOffset, array)) {
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (array == 0 || count == 0 || count > kMaxComponentScan) {
        log::InfoF("component_uid_scan count=%u array=0x%p (skipped)", count,
                   reinterpret_cast<void*>(array));
        return;
    }
    log::InfoF("component_uid_scan player=0x%p count=%u",
               reinterpret_cast<void*>(player), count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uintptr_t component = 0;
        std::uint32_t uid = 0;
        __try {
            if (!SafeRead(array + index * kComponentEntryStride, component) ||
                !SafeRead(array + index * kComponentEntryStride + 0x08, uid)) {
                continue;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        // Only the high 7 bits are the type tag; report both parts. Also log the
        // vftable so a component can be identified by CLASS rather than by the
        // runtime-assigned uid (uids are never stored statically - sec 38).
        std::uintptr_t vftable = 0;
        __try {
            SafeRead(component, vftable);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            vftable = 0;
        }
        const std::uintptr_t vftable_rva =
            (vftable != 0 && vftable > g_module_base) ? vftable - g_module_base : 0;
        log::InfoF("component_uid index=%u uid=0x%08X tag=0x%02X low=0x%07X ptr=0x%p vftable_rva=0x%X",
                   index, uid, uid >> 25, uid & 0x01FFFFFF,
                   reinterpret_cast<void*>(component),
                   static_cast<unsigned int>(vftable_rva));
    }
}

std::uintptr_t ResolvePlayerAudioComponent() noexcept {
    const std::uintptr_t player = TryReadPlayerSimObject();
    if (player == 0) {
        return 0;
    }

    std::uint32_t count = 0;
    std::uintptr_t array = 0;
    __try {
        if (!SafeRead(player + kSimObjectComponentCountOffset, count) ||
            !SafeRead(player + kSimObjectComponentArrayOffset, array)) {
            return 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (array == 0 || count == 0 || count > kMaxComponentScan) {
        return 0;
    }
    // Pass 1: exact CLASS match on the vftable, AND speak-ready.
    //
    // The vftable identifies the CLASS, not speak-readiness. MEASURED LIVE
    // (session 23:23): the first vftable match (index 40, uid 0xD2000003, ptr
    // 0x...BF0F6FA0) reported
    //     push_voice_line_guards enabled=1 voice_count=0 object=NULL latch=0
    // on 6 of 8 attempts - gate G2 (voice_count>0) and G3 (audio object non-null)
    // both failed, so SpeakByName rejected before ever reaching the inner speak.
    // A second vftable match that did have object=ok was skipped because pass 1
    // returned the FIRST class match unconditionally.
    //
    // So: prefer the first vftable match that is actually speak-capable; only if
    // none qualifies, fall back to the first plain class match (still better than
    // the heuristic pass 2, and the guard log will show why it fails).
    std::uintptr_t class_match = 0;
    std::uint32_t class_matches = 0;
    // Diagnostics for the first class match, so the warning below can name the
    // exact gate that failed. Without these the line only says "no live audio
    // object", which cannot distinguish voice_count==0 from a null object from
    // speech being disabled - and those need different fixes.
    std::uint8_t class_match_enabled = 0;
    std::uint32_t class_match_voice_count = 0;
    std::uintptr_t class_match_audio_object = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uintptr_t component = 0;
        __try {
            if (!SafeRead(array + index * kComponentEntryStride, component)) {
                continue;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (!HasActorAudioVftable(component)) {
            continue;
        }
        std::uint8_t enabled = 0;
        std::uint32_t voice_count = 0;
        std::uintptr_t audio_object = 0;
        __try {
            SafeRead(component + kSpeechGuardEnabledOffset, enabled);
            SafeRead(component + kSpeechGuardCountOffset, voice_count);
            SafeRead(component + kSpeechGuardObjectOffset, audio_object);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (class_match == 0) {
            class_match = component;
            class_match_enabled = enabled;
            class_match_voice_count = voice_count;
            class_match_audio_object = audio_object;
        }
        ++class_matches;
        if (enabled != 0 && voice_count > 0 && audio_object != 0) {
            if (g_config.diag_push_voice_line_events) {
                log::InfoF("push_voice_line_pick ptr=0x%p index=%u voice_count=%u "
                           "object=0x%p speak_ready=yes",
                           reinterpret_cast<void*>(component),
                           index,
                           voice_count,
                           reinterpret_cast<void*>(audio_object));
            }
            return component;
        }
        if (g_config.diag_push_voice_line_events) {
            log::InfoF("push_voice_line_candidate ptr=0x%p index=%u enabled=%u "
                       "voice_count=%u object=%s (not speak-ready)",
                       reinterpret_cast<void*>(component),
                       index,
                       static_cast<unsigned int>(enabled),
                       voice_count,
                       audio_object != 0 ? "ok" : "NULL");
        }
    }
    if (class_match != 0) {
        log::WarnF("push_voice_line_class_match_not_speak_ready ptr=0x%p "
                   "candidates=%u enabled=%u voice_count=%u object=%s",
                   reinterpret_cast<void*>(class_match),
                   class_matches,
                   static_cast<unsigned int>(class_match_enabled),
                   class_match_voice_count,
                   class_match_audio_object != 0 ? "ok" : "NULL");
        return class_match;
    }
    // Pass 2: no ActorAudioComponent present. Fall back to the guard heuristic so a
    // layout change degrades to a best effort rather than total silence, and log it
    // so the fallback is never mistaken for a confirmed pick.
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uintptr_t component = 0;
        __try {
            if (!SafeRead(array + index * kComponentEntryStride, component)) {
                continue;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (IsPlausibleAudioComponent(component)) {
            log::WarnF("push_voice_line_no_audio_component fallback_ptr=0x%p",
                       reinterpret_cast<void*>(component));
            return component;
        }
    }
    return 0;
}

// Resolve an ARBITRARY SimObject's ActorAudioComponent - the NPC-side
// generalization of ResolvePlayerAudioComponent above. The restored NPC voice
// lines speak through the VICTIM's own component (design fact 2): the engine itself
// talks through NPC components with the same ActorAudioComponent vftable
// (round68 sec 41), and the component-array walker ([sim+0x60] count,
// [sim+0x68] array, stride 0x10) is SimObject-generic (sec 37, 0x190AD0 disasm).
// Only the SOURCE of the SimObject differs: the caller passes the SimObject read
// from the victim's health component instead of the player global.
//
// Same two-pass shape, same class-first discipline:
//   Pass 1: the first vftable 0x17C9FD8 match that is actually speak-ready
//           (guards +0x168 enabled, +0x1BC voice count > 0, +0x190 audio object).
//   Pass 2: the first IsPlausibleAudioComponent match (class check + guards).
// Returns 0 when nothing qualifies, making the feature a no-op instead of a fault.
std::uintptr_t ResolveSimObjectAudioComponent(std::uintptr_t sim_object) noexcept {
    if (sim_object == 0) {
        return 0;
    }

    std::uint32_t count = 0;
    std::uintptr_t array = 0;
    __try {
        if (!SafeRead(sim_object + kSimObjectComponentCountOffset, count) ||
            !SafeRead(sim_object + kSimObjectComponentArrayOffset, array)) {
            return 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (array == 0 || count == 0 || count > kMaxComponentScan) {
        return 0;
    }
    // Pass 1: exact CLASS match on the vftable, AND speak-ready. See
    // ResolvePlayerAudioComponent for why the first class match alone is not
    // enough (session 23:23: a streaming component failed guards G2/G3 on 6 of
    // 8 attempts while a later match was ready).
    std::uintptr_t class_match = 0;
    std::uint32_t class_matches = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uintptr_t component = 0;
        __try {
            if (!SafeRead(array + index * kComponentEntryStride, component)) {
                continue;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (!HasActorAudioVftable(component)) {
            continue;
        }
        if (class_match == 0) {
            class_match = component;
        }
        std::uint8_t enabled = 0;
        std::uint32_t voice_count = 0;
        std::uintptr_t audio_object = 0;
        __try {
            SafeRead(component + kSpeechGuardEnabledOffset, enabled);
            SafeRead(component + kSpeechGuardCountOffset, voice_count);
            SafeRead(component + kSpeechGuardObjectOffset, audio_object);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        ++class_matches;
        if (IsPlausibleAudioComponent(component)) {
            if (g_config.diag_npc_voice_line_events) {
                log::InfoF("npc_voice_line_pick sim=0x%p ptr=0x%p index=%u "
                           "voice_count=%u object=0x%p speak_ready=yes",
                           reinterpret_cast<void*>(sim_object),
                           reinterpret_cast<void*>(component),
                           index,
                           voice_count,
                           reinterpret_cast<void*>(audio_object));
            }
            return component;
        }
        if (g_config.diag_npc_voice_line_events) {
            log::InfoF("npc_voice_line_candidate sim=0x%p ptr=0x%p index=%u "
                       "enabled=%u voice_count=%u object=%s (not speak-ready)",
                       reinterpret_cast<void*>(sim_object),
                       reinterpret_cast<void*>(component),
                       index,
                       static_cast<unsigned int>(enabled),
                       voice_count,
                       audio_object != 0 ? "ok" : "NULL");
        }
    }
    // Diagnostics-gated (unlike the push resolver's unconditional warn): NPC
    // damage events fire for every struck character in a fight, so an
    // unconditional line per unresolved victim would flood SPatch.log.
    if (class_match != 0 && g_config.diag_npc_voice_line_events) {
        log::InfoF("npc_voice_line_class_match_not_speak_ready sim=0x%p "
                   "ptr=0x%p candidates=%u (no vftable match was speak-ready; "
                   "not used)",
                   reinterpret_cast<void*>(sim_object),
                   reinterpret_cast<void*>(class_match),
                   class_matches);
    }
    // Pass 2: same fallback discipline as the player resolver - a genuine
    // ActorAudioComponent or 0, never an impostor.
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uintptr_t component = 0;
        __try {
            if (!SafeRead(array + index * kComponentEntryStride, component)) {
                continue;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (IsPlausibleAudioComponent(component)) {
            if (g_config.diag_npc_voice_line_events) {
                log::InfoF("npc_voice_line_no_audio_component sim=0x%p "
                           "fallback_ptr=0x%p",
                           reinterpret_cast<void*>(sim_object),
                           reinterpret_cast<void*>(component));
            }
            return component;
        }
    }
    return 0;
}

// Observation-only detour: records that the by-name speech route was used by the
// game itself. It never alters the result.
bool DetourSpeakByName(void* target_component,
                       unsigned int voice_id,
                       const char* name,
                       unsigned int zero,
                       unsigned int reaction_id,
                       unsigned char flag,
                       unsigned char trailing_flag) {
    if (g_speak_by_name_original == nullptr) {
        return false;
    }
    if (!g_push_voice_line_invoking && g_config.diag_push_voice_line_events &&
        g_push_voice_line_wrapper_diag_events.fetch_add(1, std::memory_order_relaxed) <
            kPushVoiceLineDiagLogBudget) {
        // Gauntlet stage 16: this runs inside a hook on the host process. The name
        // pointer belongs to the game and may be invalid, so the read is SEH-guarded;
        // a diagnostic must never be able to fault and take the host down.
        char safe_name[24]{};
        bool has_name = false;
        __try {
            has_name = name != nullptr &&
                       SafeReadCString(reinterpret_cast<std::uintptr_t>(name),
                                       safe_name,
                                       sizeof(safe_name));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            has_name = false;
            safe_name[0] = '\0';
        }
        // Log the component's vftable RVA too. The engine's OWN calls are ground
        // truth for what an ActorAudioComponent looks like, so if our resolved
        // component ever mismatches, this makes the comparison possible from the log
        // instead of leaving a silent no-op (sec 38).
        std::uintptr_t spoken_vftable = 0;
        std::uintptr_t spoken_rva = 0;
        __try {
            if (SafeRead(reinterpret_cast<std::uintptr_t>(target_component),
                         spoken_vftable) &&
                spoken_vftable > g_module_base) {
                spoken_rva = spoken_vftable - g_module_base;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            spoken_rva = 0;
        }
        log::InfoF("speak_by_name component=0x%p vftable_rva=0x%X voice=%u reaction=%u name=%s",
                   target_component,
                   static_cast<unsigned int>(spoken_rva),
                   voice_id,
                   reaction_id,
                   has_name ? safe_name : "(none)");
    }
    __try {
        return g_speak_by_name_original(target_component, voice_id, name, zero,
                                        reaction_id, flag, trailing_flag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Forward declarations: the UI-mode query and the push-line mission
// suppress counter are defined further below (beside the combat-line
// precedent); this bump handler runs earlier in the file.
bool ReadUiModeState(std::uint32_t& out_mode);
extern std::atomic<unsigned long long> g_push_voice_line_mission_suppress_count;

// Push voice line on a PLAYER crowd bump (see comment in
// MaybeFirePushVoiceLineOnBump for the detection rationale).
// Fires Wei's crowd-clearing line when the PLAYER bumps a pedestrian.
//
// Sec 37 (verified): HealthApplyDamage already performs the player-as-attacker
// test in its prologue -
//   0x521FA9  call 0x360C20    ; rax = *0x235C488 (player SimObject singleton)
//   0x521FAE  cmp rdi, rax     ; rdi = `attacker`
// so a constant `attacker` across many distinct victims is the PLAYER pushing
// through a crowd, not a system object. The earlier inverted reading is corrected.
//
// Sec 36/38 (verified): the invocation previously failed twice over - by-id args
// were swapped (arg2 is the NAME id), and a bare "wei.move" cannot resolve because
// the resolver is exact-match FNV-1 over fully-qualified names. Both are fixed by
// using the by-name entry 0x5A8930 with its correct SEVEN-argument contract.
//
// Gauntlet stage 16: this runs inside a hook on the host process, so every host
// memory read is SEH-guarded, there is no allocation and no unbounded stack use,
// and a fault here degrades to silence rather than terminating the game.
void MaybeFirePushVoiceLineOnBump(void* attacker,
                                  int damage,
                                  bool projectile_damage) {
    if (!g_config.restore_push_voice_lines || projectile_damage || damage != 0 ||
        g_push_voice_line_invoking) {
        return;
    }
    // PushLinesFreeRoamOnly: skip the restored lines while the game is in
    // its mission-scoped mode. A failed read degrades to unsuppressed.
    if (g_config.restore_push_voice_lines_free_roam_only) {
        std::uint32_t mode = 0;
        if (ReadUiModeState(mode) && mode == 2) {
            const unsigned long long n =
                g_push_voice_line_mission_suppress_count.fetch_add(
                    1, std::memory_order_relaxed);
            if (n < 8) {
                log::InfoF("push_voice_line_mission_suppress mode=%u calls=%llu",
                           mode,
                           static_cast<unsigned long long>(n));
            }
            return;
        }
    }
    if (!IsPlayerAttacker(attacker)) {
        return;
    }
    if (IsGamePausedSafely()) {
        return;
    }
    bool in_combat = false;
    if (ReadPlayerCombatState(in_combat) && in_combat) {
        return;
    }

    if (g_config.diag_push_voice_line_events) {
        LogPlayerComponentUids();
    }
    // Wei's OWN audio component must speak; the victim's would play an NPC grunt.
    const std::uintptr_t audio_component = ResolvePlayerAudioComponent();
    if (g_config.diag_push_voice_line_events) {
        // Report the uid tag too. The probe is a heuristic, so knowing WHICH
        // component it picked is what makes the next log decisive rather than
        // merely pass/fail (sec 38).
        std::uint32_t chosen_uid = 0;
        std::uintptr_t player_sim = TryReadPlayerSimObject();
        if (player_sim != 0 && audio_component != 0) {
            std::uint32_t count = 0;
            std::uintptr_t array = 0;
            __try {
                if (SafeRead(player_sim + kSimObjectComponentCountOffset, count) &&
                    SafeRead(player_sim + kSimObjectComponentArrayOffset, array) &&
                    array != 0 && count != 0 && count <= kMaxComponentScan) {
                    for (std::uint32_t index = 0; index < count; ++index) {
                        std::uintptr_t candidate = 0;
                        if (SafeRead(array + index * kComponentEntryStride, candidate) &&
                            candidate == audio_component) {
                            SafeRead(array + index * kComponentEntryStride + 0x08, chosen_uid);
                            break;
                        }
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                chosen_uid = 0;
            }
        }
        log::InfoF("push_voice_line_component resolved=0x%p uid=0x%08X tag=0x%02X player_bump=yes",
                   reinterpret_cast<void*>(audio_component),
                   chosen_uid,
                   chosen_uid >> 25);
    }
    if (audio_component == 0) {
        return;
    }

    const push_voice_line::Decision decision = EvaluatePushVoiceLineGate();
    if (decision.fire) {
        InvokePushVoiceLine(audio_component);
    } else if (g_config.diag_push_voice_line_events &&
               g_push_voice_line_diag_events.load(std::memory_order_relaxed) <
                   kPushVoiceLineDiagLogBudget) {
        log::InfoF("push_voice_line_suppressed cooldown=%u budget=%u",
                   decision.suppressed_by_cooldown ? 1u : 0u,
                   decision.suppressed_by_budget ? 1u : 0u);
    }
}

// UI mode global: the query thunks at RVA 0x5EE120/0x5EE130/0x5EE140 all
// test this one dword (==0 / ==1 / ==2 respectively), i.e. a mode enum.
// Mode 2 is the mission-scoped gameplay state. The first suppressions log
// the observed value so the semantics stay verified against live play.
constexpr std::uintptr_t kUiModeStateRva = 0x2432F00;
std::atomic<unsigned long long> g_combat_lines_mission_suppress_count{0};
std::atomic<unsigned long long> g_push_voice_line_mission_suppress_count{0};

bool ReadUiModeState(std::uint32_t& out_mode) {
    const std::uintptr_t address = ResolveAddress(kUiModeStateRva);
    if (address == 0) {
        return false;
    }
    std::uint32_t value = 0;
    if (!SafeRead(address, value)) {
        return false;
    }
    out_mode = value;
    return true;
}
// ---- Slow-motion probe + suppression ([Debug] SlowMoProbe, options below) --
//
// The engine keeps one applied simulation time scale at RVA 0x203C958 that every
// slow-motion feature writes. This block runs from the existing character-effects
// update, so it needs no engine hook of its own and sees whichever writer moved
// the value.
//
// Suppression is a per-frame clamp: if the applied scale matches a disabled
// feature's factor, it is written back to 1.0. Request values observed live
// (2026-09-15): 0.30 = the one-shot stunt hit; the drive-by aim ramp walks
// 0.915 -> 0.50.
constexpr std::uintptr_t kSlowMoScaleRva = 0x00203C958;
constexpr std::uintptr_t kSlowMoFreezeFlagRva = 0x00203C95C;
constexpr std::uintptr_t kSlowMoRequestRva = 0x00203C97C;
// Unmoved in the latest layout (reader-disp votes, site-evidence-wave2.md);
// ResolveAddress falls through to the same addresses there.
constexpr float kStuntSlowMoFactor = 0.3f;
constexpr float kDrivebySlowMoLowFactor = 0.45f;
constexpr float kDrivebySlowMoHighFactor = 0.98f;
std::atomic<unsigned long> g_slow_mo_probe_lines{0};
std::atomic<std::uint32_t> g_slow_mo_probe_last_bits{0xFFFFFFFFu};

// Recent-sound ring buffer. The swoosh plays just BEFORE the time scale drops,
// so an in-window scan can never see it. Instead, keep the last 256 posts and
// dump them the moment a slow-motion transition starts.
struct SoundRingEntry {
    unsigned int id;
    unsigned char path;  // 1 = 0x1440b0, 2 = 0x14C150
    float scale;
};
constexpr unsigned int kSoundRingSize = 256;
SoundRingEntry g_sound_ring[kSoundRingSize]{};
std::atomic<unsigned int> g_sound_ring_pos{0};
std::atomic<unsigned int> g_sound_ring_dumps{0};
std::atomic<std::uint32_t> g_sound_prev_scale_bits{0x3F800000u};

void SoundRingPush(unsigned int id, unsigned char path, float scale) noexcept {
    const unsigned int pos = g_sound_ring_pos.fetch_add(1, std::memory_order_relaxed);
    SoundRingEntry& entry = g_sound_ring[pos % kSoundRingSize];
    entry.id = id;
    entry.path = path;
    entry.scale = scale;
}

void SoundRingDump() noexcept {
    if (g_sound_ring_dumps.load(std::memory_order_relaxed) >= 6) {
        return;
    }
    (void)g_sound_ring_dumps.fetch_add(1, std::memory_order_relaxed);
    const unsigned int end = g_sound_ring_pos.load(std::memory_order_relaxed);
    const unsigned int count = end < kSoundRingSize ? end : kSoundRingSize;
    log::InfoF("sound_ring_dump count=%u", count);
    for (unsigned int i = 0; i < count; ++i) {
        const unsigned int idx = (end - count + i) % kSoundRingSize;
        const SoundRingEntry entry = g_sound_ring[idx];
        log::InfoF("sound_ring path=%u id=0x%08X scale=%.4f",
                   static_cast<unsigned int>(entry.path), entry.id, entry.scale);
    }
}

using SoundNameResolveFn = std::uint32_t(__fastcall*)(const char*);
SoundNameResolveFn g_sound_name_resolve_original = nullptr;

using SoundPostFn = bool(__fastcall*)(long long, int, unsigned long long,
                                      unsigned long long, unsigned long long);
SoundPostFn g_sound_post_original = nullptr;

std::atomic<unsigned long> g_sound_probe_lines{0};
std::atomic<unsigned long> g_sound_post_lines{0};

std::atomic<int> g_slowmo_recent_frames{0};
void MaybePollSlowMoScale() noexcept {
    // Per-frame call: exit before any address resolution or memory read when
    // no slow-motion feature is enabled.
    if (!g_config.probe_slow_mo && !g_config.disable_driveby_slow_mo &&
        !g_config.disable_stunt_slow_mo) {
        return;
    }
    // kSlowMoScaleRva and kSlowMoFreezeFlagRva are legacy-layout globals with
    // no latest_steam mapping, so on that layout ResolveAddress hands back an
    // in-range but unrelated address. The read would sample garbage and, any
    // time the garbage lands inside the drive-by band (0.45-0.98), the
    // suppression below would WRITE to that unrelated address every frame.
    // Fail closed like the sound-event hooks; warn once so a latest_steam
    // session log says why the option did nothing.
    if (g_use_latest_steam_layout) {
        // The slow-mo globals are unmoved in the latest layout
        // (site-evidence-wave2.md), so the poll runs there too.
    }
    const std::uintptr_t scale_address = ResolveAddress(kSlowMoScaleRva);
    if (scale_address == 0) {
        return;
    }
    float applied = 1.0f;
    if (!SafeRead(scale_address, applied)) {
        return;
    }

    if (applied < 1.0f) {
        g_slowmo_recent_frames.store(90, std::memory_order_relaxed);
    } else if (g_slowmo_recent_frames.load(std::memory_order_relaxed) > 0) {
        (void)g_slowmo_recent_frames.fetch_sub(1, std::memory_order_relaxed);
    }

    if (applied > 0.0f && applied < 1.0f &&
        (g_config.disable_driveby_slow_mo || g_config.disable_stunt_slow_mo)) {
        const bool stunt_match =
            g_config.disable_stunt_slow_mo &&
            std::fabs(applied - kStuntSlowMoFactor) <= 0.02f;
        const bool driveby_match =
            g_config.disable_driveby_slow_mo &&
            applied >= kDrivebySlowMoLowFactor &&
            applied <= kDrivebySlowMoHighFactor;
        if (stunt_match || driveby_match) {
            (void)SafeWrite(scale_address, 1.0f);
            const std::uintptr_t flag_address = ResolveAddress(kSlowMoFreezeFlagRva);
            if (flag_address != 0) {
                (void)SafeWrite(flag_address, static_cast<unsigned char>(0));
            }
            applied = 1.0f;
        }
    }

    // Slow-motion onset: dump the recent-sound ring so the pre-stunt sounds
    // (the swoosh fires before the scale drops) are captured.
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &applied, sizeof(bits));
        const std::uint32_t previous_bits =
            g_sound_prev_scale_bits.exchange(bits, std::memory_order_relaxed);
        const bool was_normal = previous_bits >= 0x3F800000u;
        const bool is_slow = applied < 1.0f;
        if (was_normal && is_slow && g_config.probe_slow_mo) {
            SoundRingDump();
        }
    }
    if (!g_config.probe_slow_mo) {
        return;
    }
    if (g_slow_mo_probe_lines.load(std::memory_order_relaxed) >= 800) {
        return;
    }
    std::uint32_t bits = 0;
    std::memcpy(&bits, &applied, sizeof(bits));
    const std::uint32_t previous =
        g_slow_mo_probe_last_bits.exchange(bits, std::memory_order_relaxed);
    if (previous == bits) {
        return;
    }
    std::uint32_t mode = 0;
    const bool mode_known = ReadUiModeState(mode);
    bool in_vehicle = false;
    bool vehicle_known = false;
    const std::uintptr_t player = TryReadPlayerSimObject();
    if (player != 0) {
        vehicle_known = TryReadCharacterInVehicle(player, in_vehicle);
    }
    (void)g_slow_mo_probe_lines.fetch_add(1, std::memory_order_relaxed);
    log::InfoF("slowmo_probe applied=%.6f prev=0x%08X mode_known=%u mode=%u "
               "vehicle_known=%u in_vehicle=%u",
               applied,
               previous,
               mode_known ? 1u : 0u,
               mode,
               vehicle_known ? 1u : 0u,
               in_vehicle ? 1u : 0u);
}

// The engine's time-dilation apply point (kept for the request log; the
// suppression above no longer depends on it).
using TimeDilateApplyFn = void(__fastcall*)(void*);
TimeDilateApplyFn g_time_dilate_apply_original = nullptr;

void __fastcall DetourTimeDilateApply(void* component) {
    if (g_time_dilate_apply_original != nullptr) {
        g_time_dilate_apply_original(component);
    }
    MaybePollSlowMoScale();
}

// GTAVITimeClock:
// flatten the stock day-night rate into ONE uniform speed so a full in-game
// day passes in gta_vi_time_clock_day_minutes real minutes and the stock
// 1.33x faster night progression is cancelled. dt is scaled instead of
// touching the .rdata rate constants, so the change is per-frame reversible
// and config-driven. rate_selected replicates the stock window test on the
// current TOD (10800 <= tod <= 75600 -> 1.0 else 1.33; all five rate
// constants verified byte-identical across both layouts 2026-10-31); scale
// is the object's own [obj+0x20] multiplier so mission/cutscene time control
// still dominates. Guard: scale * rate <= 0 -> forward dt unchanged (fail
// open). The script-override branch ([0x202412C] > 0) ignores dt entirely
// and stays untouched.
using TimeOfDayClockUpdateFn = void(__fastcall*)(void*, float);
TimeOfDayClockUpdateFn g_time_of_day_clock_update_original = nullptr;

void __fastcall DetourTimeOfDayClockUpdate(void* object, float dt) {
    float scaled_dt = dt;
    if (object != nullptr) {
        float tod = 0.0f;
        float scale = 0.0f;
        const auto* bytes = static_cast<const std::uint8_t*>(object);
        std::memcpy(&tod, bytes + 0x24, sizeof(tod));
        std::memcpy(&scale, bytes + 0x20, sizeof(scale));
        const float rate = (tod >= 10800.0f && tod <= 75600.0f) ? 1.0f : 1.33f;
        const float denominator = scale * rate;
        if (denominator > 0.0f) {
            const float real_seconds_per_game_second =
                86400.0f / (static_cast<float>(
                                g_config.gta_vi_time_clock_day_minutes) *
                            60.0f);
            scaled_dt = dt * real_seconds_per_game_second / denominator;
        }
    }
    if (g_time_of_day_clock_update_original != nullptr) {
        g_time_of_day_clock_update_original(object, scaled_dt);
    }
}

// Sound-event probe (two hooks, both diagnostic and gated by [Debug] SlowMoProbe).
//
//  0x143890  resolves an event-name string to an id; the caller stores the
//            returned eax, so the detour must return the original value.
//  0x1440B0  posts the event: (owner, event_id, 0, 0) - confirmed at 0x673E65
//            right after the Play_vehicle_whooshes id is loaded. Returns a value
//            in rax, so the detour must return it too.
//
// Logging the resolved name table plus the posted id stream lets a stunt window be
// matched to its event by elimination.
constexpr std::uintptr_t kSoundNameResolveRva = 0x00143890;
// 0x1440B0 is only a wrapper; it calls 0x144230 to actually start playback, and
// other callers start sounds by calling 0x144230 directly. Hooking the inner
// function covers every caller, which is what the wrapper hook was missing.
constexpr std::uintptr_t kSoundPostRva = 0x00144230;
// Suppressed sound ids. A path suppresses only the ids listed here and passes
// every other event through untouched.
//
//  post1 (0x144230):
//    0xE488B9D7  UNNAMED, and confirmed by the operator on 2026-09-28 as the
//                stunt swoosh. It posts on the first frame of every stunt: the
//                15:0x capture logged it at 15:01:41.479 and again at
//                15:03:11.706, each as the FIRST id of its own slow-motion
//                window, and the 14:01 capture's raw per-event probe put it 1 ms
//                before the engine's applied time scale dropped to 0.3
//                (14:01:27.966 sound_post against 14:01:27.967 slowmo_probe, and
//                the same pairing at 14:01:29.456/.457). It is unnamed because
//                DetourSoundNameResolve is a table lookup rather than a hash
//                (fnv1a/fnv1/djb2/sdbm/crc32 scored 0 matches over 5 known
//                name->id pairs) and this id never routes through it.
//
//  Ids deliberately REMOVED. Each was established wrong by a play session, and
//  each is kept out rather than left as a shim:
//    0x3A157AA7  REFUTED. Promoted here as "the only remaining candidate", but it
//                did not post once in either stunt window of the 15:0x capture.
//                Its 09:49:51-52 occurrences belonged to a different context, so
//                suppressing it was a no-op that could never have worked.
//    0x46A60BB3  preimage "Play_vehicle_whooshes". A vehicle PASS-BY whoosh, not
//                the stunt one: all 16 of its suppressions landed 35 s BEFORE the
//                confirmed stunt. This option means "the swoosh when doing a
//                stunt", so muting an ordinary-driving sound was never in scope.
//  post2 (0x14C150):
//    nothing is suppressed on this path any more. 0xBDE53B23 (preimage
//    "whoosh_rel_speed") was the relative-speed companion of the pass-by whoosh,
//    so it goes for the same reason - and it is a per-frame parameter re-set at
//    ~2 Hz, not a discrete play event, so dropping it was the wrong shape of fix.
//
// The ids an earlier ring-buffer capture suggested (0xAD7F8DD9, 0xBE422A50,
// 0xFE76B658, 0x0BC5D3BB, 0xAE45C158, 0xC2A8B808) came from an ORDERING of posts
// just before the onset, not an identity: the whoosh fires in that window, and so
// do the events the engine resolves every frame (burnout_amount,
// skid_angle_player, skid_strength_player). Acting on the ordering logged 203,010
// lines in one session, cut the engine's RTPC drive, and never removed the whoosh.
// That capture EXONERATED the ring and refuted the ordering heuristic, which is
// why the real target had to come from a per-stunt-window inventory instead.
constexpr unsigned int kStuntSwooshEventId = 0xE488B9D7u;

bool IsStuntSuppressedPost1Id(unsigned int event_id) noexcept {
    // The ~780 Hz ids the 2026-09-28 capture also showed inside the stunt window
    // (0x7A9B39B5 x864, 0x5979CECB x864, 0xBDE2B78C x793) are deliberately not
    // matched: at that rate they are per-frame engine/tyre parameters, and
    // dropping them would silence the stunt itself instead of the swoosh.
    return event_id == kStuntSwooshEventId;
}

// Cap on the post1 detour's "suppressed" log. A plain std::atomic, not
// DiagnosticAtomic: DiagnosticAtomic::load() folds to a constant 0 in
// SPATCH_FINAL_RELEASE, so a load()-compared cap would never engage in the
// shipped binary. The value returned by the increment is the value compared, so
// one emitted line is exactly one count. post1 is the only path that suppresses
// anything now, so it is the only one that needs a counter.
constexpr unsigned long kStuntSoundSuppressedLogCap = 16;
std::atomic<unsigned long> g_stunt_sound_post1_suppressed_lines{0};

// Distinct-id backing store for the sound_name join table. Sized well above
// the ~2500 distinct ids a full session emits; the linear scan is the right
// trade here because it runs once per resolver CALL on a path that is not
// frame-critical, and it keeps the table a flat array with no allocation.
constexpr std::size_t kSoundNameSeenCapacity = 8192;
std::uint32_t g_sound_name_seen[kSoundNameSeenCapacity]{};
// The resolver can be entered from more than one audio thread, so the count
// is claimed atomically: a check-then-act on a plain size_t could let two
// threads both pass the capacity test and index one past the array.
std::atomic<std::size_t> g_sound_name_seen_count{0};
bool g_sound_name_table_full_logged = false;
std::uint32_t __fastcall DetourSoundNameResolve(const char* name) {
    std::uint32_t id = 0;
    if (g_sound_name_resolve_original != nullptr) {
        id = g_sound_name_resolve_original(name);
    }
    // This path is the JOIN KEY for the whole stunt-sound analysis: every id
    // reported by stunt_window_sound is resolved against this table to recover
    // a name, so a truncated table silently yields unresolved ids - and an
    // unresolved id looks like a real result rather than a missing one.
    //
    // The table is DEDUPED ON THE RETURNED ID, not merely rate-capped. A plain
    // per-call counter does not bound it: the 2026-09-28 capture logs
    // `sound_name name=MrsChus_ExtraOFF id=0x6A30DA04` on two consecutive
    // lines, and LivingWorld, AmbientUnlocker and DynamicTutorialSlice likewise
    // in pairs, so the resolver is called repeatedly for one id and a counter
    // grows with play volume rather than with the number of distinct sounds.
    // Dedupe makes the emitted line count equal the distinct-id count, which is
    // what the join needs, and makes truncation impossible short of exhausting
    // the distinct-id space itself.
    if (g_config.probe_slow_mo && name != nullptr) {
        bool already_seen = false;
        const std::size_t seen_count =
            g_sound_name_seen_count.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < seen_count; ++i) {
            if (g_sound_name_seen[i] == id) {
                already_seen = true;
                break;
            }
        }
        if (!already_seen) {
            // Atomically claim a slot: the fetch_add bounds the index even
            // when two threads race the capacity check.
            const std::size_t slot = g_sound_name_seen_count.fetch_add(
                1, std::memory_order_relaxed);
            if (slot < kSoundNameSeenCapacity) {
                g_sound_name_seen[slot] = id;
                log::InfoF("sound_name name=%s id=0x%08X", name, id);
            } else if (!g_sound_name_table_full_logged) {
                // Truncation MUST be visible. A table that stops silently makes
                // every later id come back unresolved, and an unresolved id is
                // indistinguishable from a genuine finding - the join would
                // produce a wrong answer while looking healthy. Emitted once,
                // loudly, so the log itself says the join is incomplete.
                g_sound_name_table_full_logged = true;
                log::WarnF("sound_name_table_full count=%zu capacity=%zu - ids "
                          "beyond this point will NOT resolve; the "
                          "stunt_window_sound join is INCOMPLETE",
                          g_sound_name_seen_count.load(
                              std::memory_order_relaxed),
                          kSoundNameSeenCapacity);
            }
        }
    }
    return id;
}

// Distinct-id inventory for one slow-motion window, kept SEPARATELY PER PATH.
// The raw event stream is unusable for this question: in-window post2 events
// accumulate thousands per second, so a session-lifetime cap saturates inside
// the FIRST stunt and blinds every stunt after it. Measured 2026-09-28 14:01:
// post2 logged exactly its 3000-line ceiling between 14:01:27.968 and
// 14:01:28.745 and then emitted nothing for the rest of the session, so the
// second stunt was invisible; post1 was already spent before the stunt began,
// its 6000-line ceiling being consumed by out-of-window ambient posts. What
// the diagnostic needs is the SET of ids that play while slow motion is
// active, per path - a handful of lines, and it is what separates the swoosh
// from the ~780 Hz engine/tyre parameters. The sets are per path because the
// same id can post on both, and a shared set would log it once and mislabel it.
constexpr std::size_t kStuntWindowIdCapacity = 64;
unsigned int g_stunt_window_post1_ids[kStuntWindowIdCapacity]{};
unsigned int g_stunt_window_post2_ids[kStuntWindowIdCapacity]{};
std::size_t g_stunt_window_post1_count = 0;
std::size_t g_stunt_window_post2_count = 0;
bool g_stunt_window_open = false;

void LogStuntWindowIdOnce(unsigned int event_id, int path) noexcept {
    if (!g_stunt_window_open) {
        g_stunt_window_post1_count = 0;
        g_stunt_window_post2_count = 0;
        g_stunt_window_open = true;
    }
    unsigned int* const ids =
        path == 1 ? g_stunt_window_post1_ids : g_stunt_window_post2_ids;
    std::size_t& count =
        path == 1 ? g_stunt_window_post1_count : g_stunt_window_post2_count;
    for (std::size_t i = 0; i < count; ++i) {
        if (ids[i] == event_id) {
            return;
        }
    }
    if (count >= kStuntWindowIdCapacity) {
        return;
    }
    ids[count++] = event_id;
    log::InfoF("stunt_window_sound id=0x%08X path=%d", event_id, path);
}
bool __fastcall DetourSoundPost(long long owner, int event_id,
                                unsigned long long a3,
                                unsigned long long a4,
                                unsigned long long a5) {
    // One id is dropped here and nothing else: 0xE488B9D7, the stunt swoosh the
    // operator confirmed on 2026-09-28. It posts on the first frame of every
    // stunt through this 0x144230 entry. Every other event passes through
    // untouched, including the ~780 Hz engine/tyre parameters, so the stunt's
    // engine and tyre audio cannot be swallowed by this filter.
    //
    // SCOPE: only while the applied time scale is below 1.0, i.e. only inside a
    // slow-motion window. This is the same test the desaturation detour uses at
    // 13856. It is required, not an optimisation - the option means "disable the
    // swoosh DURING STUNTS", and the scale test is what makes that literal
    // rather than "disable this event everywhere". Without it the id would also
    // be silenced in ordinary driving, which is a different sound: the pass-by
    // whoosh was removed from this list for exactly that reason, and an
    // unconditional drop would quietly bring that behaviour back.
    bool in_stunt_window = false;
    {
        const std::uintptr_t scale_address = ResolveAddress(kSlowMoScaleRva);
        float applied_scale = 1.0f;
        in_stunt_window = scale_address != 0 &&
                          SafeRead(scale_address, applied_scale) &&
                          applied_scale < 1.0f;
    }
    if (g_config.disable_stunt_sound_fx && in_stunt_window &&
        IsStuntSuppressedPost1Id(static_cast<unsigned int>(event_id))) {
        const unsigned long line =
            g_stunt_sound_post1_suppressed_lines.fetch_add(1, std::memory_order_relaxed);
        if (line < kStuntSoundSuppressedLogCap) {
            log::InfoF("sound_suppressed path=post1 id=0x%08X",
                       static_cast<unsigned int>(event_id));
        }
        return false;
    }
    bool result = false;
    if (g_sound_post_original != nullptr) {
        result = g_sound_post_original(owner, event_id, a3, a4, a5);
    }
    {
        float ring_scale = 1.0f;
        const std::uintptr_t ring_addr = ResolveAddress(kSlowMoScaleRva);
        if (ring_addr != 0) {
            (void)SafeRead(ring_addr, ring_scale);
        }
        SoundRingPush(static_cast<unsigned int>(event_id), 1, ring_scale);
    }
    if (g_config.probe_slow_mo) {
        // Same per-window, per-distinct-id treatment as post2 (see
        // LogStuntWindowIdOnce). The previous guard was a session-global
        // 6000-line ceiling incremented on EVERY post, window or not, so
        // ordinary ambient traffic exhausted it before any stunt could be
        // observed - which is why the 2026-09-28 14:01 window showed a single
        // sound_post line. The scale is still read so the window is defined by
        // the engine's own applied time scale, not by wall-clock guessing.
        float scale = 1.0f;
        const std::uintptr_t scale_address = ResolveAddress(kSlowMoScaleRva);
        if (scale_address != 0) {
            (void)SafeRead(scale_address, scale);
        }
        if (scale < 1.0f) {
            LogStuntWindowIdOnce(static_cast<unsigned int>(event_id), 1);
        } else {
            // The window must be closable from EITHER path. Without this the
            // reset depends on a post2 event happening to land in the gap
            // between two stunts; if none does, the next stunt inherits the
            // previous stunt's id set and two stunts merge into one inventory.
            g_stunt_window_open = false;
        }
    }
    return result;
}

// Second sound path: `0x14C150(owner, id, float)` is a different post entry point
// (its ids do not come from the 0x143890 resolver). Logged with the sim scale so
// the stunt window can be filtered, same as the 0x1440b0 path.
constexpr std::uintptr_t kSoundPost2Rva = 0x0014C150;
using SoundPost2Fn = unsigned char(__fastcall*)(void*, unsigned int, float);
SoundPost2Fn g_sound_post2_original = nullptr;
std::atomic<unsigned long> g_sound_post2_lines{0};

unsigned char __fastcall DetourSoundPost2(void* owner, unsigned int event_id,
                                          float value) {
    // This path suppresses NOTHING. It used to drop 0xBDE53B23 (preimage
    // "whoosh_rel_speed"), the relative-speed companion of the pass-by whoosh
    // 0x46A60BB3, but both are PASS-BY sounds, not the stunt swoosh: every
    // logged occurrence of 0xBDE53B23 was 59 s before the confirmed stunt
    // window opened. Removing the pass-by pair from the suppression list in
    // post1 would have left this branch as a half-applied fix of an
    // out-of-scope behaviour, so it goes with them.
    //
    // The path still matters as a DIAGNOSTIC: the 15:0x capture showed
    // 0x20C43027 posting here on the stunt's first frame and
    // 0x229743A3 (preimage "Effect_in_trunk") here as an ambient vehicle
    // sound, neither of which the operator identified as the swoosh. That is
    // why the window inventory below is kept per path: the same id can post on
    // both, and a shared set would log it once and mislabel it.
    unsigned char result = 0;
    if (g_sound_post2_original != nullptr) {
        result = g_sound_post2_original(owner, event_id, value);
    }
    {
        float ring_scale = 1.0f;
        const std::uintptr_t ring_addr = ResolveAddress(kSlowMoScaleRva);
        if (ring_addr != 0) {
            (void)SafeRead(ring_addr, ring_scale);
        }
        SoundRingPush(event_id, 2, ring_scale);
    }
    if (g_config.probe_slow_mo) {
        // Per-window, per-DISTINCT-id. This replaces a session-lifetime 3000-line
        // cap that saturated inside the first stunt (see kStuntWindowIdCapacity
        // above) and made every later stunt unobservable. The window closes when
        // the applied scale returns to 1.0, so the next stunt starts a fresh
        // inventory rather than inheriting an exhausted budget.
        float scale = 1.0f;
        const std::uintptr_t scale_address = ResolveAddress(kSlowMoScaleRva);
        if (scale_address != 0) {
            (void)SafeRead(scale_address, scale);
        }
        if (scale < 1.0f) {
            LogStuntWindowIdOnce(static_cast<unsigned int>(event_id), 2);
        } else {
            g_stunt_window_open = false;
        }
    }
    return result;
}

// Forward declaration: the victim SimObject read lives in a leaf helper below
// (MSVC forbids __try in a function needing unwinding, C2712).
std::uintptr_t TryReadVictimSimObject(void* health_component) noexcept;

// Resolve the SPEAKER for a combat_pre_posture line from the victim, and write
// that voice tag into out_voice. Returns the victim's own ActorAudioComponent,
// or 0 when the speaker cannot be established - the caller then falls back to
// Wei rather than inventing a voice.
//
// Three conditions must ALL hold before a victim's voice is used:
//   1. the victim SimObject resolves from the health component;
//   2. the victim has a speak-ready ActorAudioComponent;
//   3. that component's voice tag is readable AND names a voice the roster
//      owns a combat_pre_posture family for.
// Condition 3 is what keeps this honest: the engine would otherwise silently
// reject a voice that does not own the family, so consulting the table first
// turns an invisible no-op into a decidable outcome.
//
// Lives in its own function because MSVC forbids __try in a function that needs
// object unwinding (C2712); the caller holds a lock_guard scope.
std::uintptr_t ResolveVictimPrePostureSpeaker(void* health_component,
                                            const char*& out_voice) {
    out_voice = kWeiVoiceTag;
    if (health_component == nullptr) {
        return 0;
    }
    const std::uintptr_t sim_object = TryReadVictimSimObject(health_component);
    if (sim_object == 0) {
        return 0;
    }
    const std::uintptr_t audio_component =
        ResolveSimObjectAudioComponent(sim_object);
    if (audio_component == 0) {
        return 0;
    }
    char tag[kVoiceTagReadCapacity] = {};
    if (!TryReadVoiceTag(audio_component, tag, sizeof(tag))) {
        return 0;
    }
    // Roster membership test. This is the same lookup that builds the name, so
    // it cannot disagree with it: a tag that resolves here names a voice whose
    // pre-posture family provably exists in the bank.
    NameBuffer resolved_name{};
    std::uint32_t resolved_id = 0;
    if (!ResolveVoiceTake(tag, 0, resolved_name, resolved_id)) {
        // A real voice, just not one that owns combat_pre_posture. Speaking
        // here would compose a name the bank does not hold, so decline and let
        // the caller fall back to Wei.
        return 0;
    }
    out_voice = tag;
    return audio_component;
}
// ---- Restored combat voice lines (restore_combat_lines) -------------------
 //
// Wei's shipped-but-unwired combat barks (pre-fight posture, cold/angry combat
// taunts, low-damage apologies outside combat) exist in the DE audio banks but
// the game never triggers them. This path picks one per qualifying damage
// event through the pure classifier in WeiVoiceLinePolicy.h and speaks it
// through the speaker's OWN audio component, exactly like the push line.
//
// combat_pre_posture ships for 66 voices, not just Wei (kVoicePrePostureFamilies).
// With RestoreCombatLinesAllVoices off - the default - this is the shipped Wei
// behaviour byte for byte: Wei's own component, the action name, and the engine
// picking the variation. With it on, the pre-posture line speaks through the
// VICTIM's own component so the engine composes "<victim_voice>.combat_pre_posture.001."
// and plays that voice's family. The taunt and apology families stay Wei-only:
// the roster is a combat_pre_posture catalog, so there is no evidence-backed
// roster for them and guessing one would play names the bank does not hold.
//
// Detection rides the same detour as the push bump, because HealthApplyDamage
// is the only place that observes both the player combat flag and the
// attacker/damage pair.
//
// Gauntlet stage 16 discipline, same as MaybeFirePushVoiceLineOnBump: every
// host memory read is SEH-guarded, there is no allocation and no unbounded
// stack use, and a fault degrades to silence. The whole path early-outs on
// the config key before any combat read or gate work, so a default install is
// inert.
void MaybeFireWeiVoiceLineOnDamage(void* health_component,
                                   void* attacker,
                                   int damage,
                                   bool projectile_damage) {
    if (!g_config.restore_combat_lines || projectile_damage ||
        g_push_voice_line_invoking) {
        return;
    }

    // CombatLinesFreeRoamOnly: skip the restored lines while the game is in
    // its mission-scoped mode. A failed read degrades to unsuppressed.
    if (g_config.restore_combat_lines_free_roam_only) {
        std::uint32_t mode = 0;
        if (ReadUiModeState(mode) && mode == 2) {
            const unsigned long long n =
                g_combat_lines_mission_suppress_count.fetch_add(
                    1, std::memory_order_relaxed);
            if (n < 8) {
                log::InfoF("combat_line_mission_suppress mode=%u calls=%llu",
                           mode,
                           static_cast<unsigned long long>(n));
            }
            return;
        }
    }

    // Combat-state observation. Unknown state suppresses taunts AND apologies
    // (SelectAction treats "unknown" as neither in nor out of combat).
    bool in_combat = false;
    const bool combat_known = ReadPlayerCombatState(in_combat);
    bool combat_entry = false;
    if (combat_known && in_combat) {
        combat_entry = !g_wei_voice_line_combat_active.exchange(
            true, std::memory_order_relaxed);
        if (combat_entry) {
            g_wei_voice_line_entry_pending.store(true,
                                                 std::memory_order_relaxed);
        }
    } else if (combat_known) {
        g_wei_voice_line_combat_active.store(false, std::memory_order_relaxed);
        g_wei_voice_line_entry_pending.store(false, std::memory_order_relaxed);
    }

    wei_voice_line::Action action = wei_voice_line::SelectAction(
        combat_entry,
        combat_known,
        in_combat,
        IsPlayerAttacker(attacker),
        damage,
        static_cast<std::uint32_t>(g_wei_voice_line_taunt_counter.load(
            std::memory_order_relaxed)));
    // A pending entry line outranks this event's taunt (documented priority)
    // and keeps waiting until the gate lets it through or combat ends.
    const bool entry_pending =
        combat_known && in_combat &&
        g_wei_voice_line_entry_pending.load(std::memory_order_relaxed);
    if (entry_pending) {
        action = wei_voice_line::Action::CombatPrePosture;
    }
    if (action == wei_voice_line::Action::None) {
        return;
    }
    // Taunt-only minimum gap (wei_voice_line::kTauntCooldownMs), checked
    // BEFORE the shared gate so a gapped taunt consumes no gate slot; see
    // the constant's tuning note in WeiVoiceLinePolicy.h.
    const bool is_taunt =
        action == wei_voice_line::Action::CombatTauntCold ||
        action == wei_voice_line::Action::CombatTauntAngry;
    if (is_taunt) {
        const std::uint64_t now_ms = GetTickCount64();
        const std::uint64_t last_taunt_ms =
            g_wei_voice_line_last_taunt_ms.load(std::memory_order_relaxed);
        if (!wei_voice_line::TauntReady(now_ms, last_taunt_ms)) {
            return;
        }
        g_wei_voice_line_last_taunt_ms.store(now_ms, std::memory_order_relaxed);
    }
    if (IsGamePausedSafely()) {
        return;
    }

    // Speaker selection. ONLY the pre-posture family has a roster for voices
    // beyond Wei, so only that action can be redirected; the taunt and apology
    // families always speak through Wei as before.
    std::uintptr_t audio_component = 0;
    const char* speaker_voice = kWeiVoiceTag;
    if (g_config.restore_combat_lines_all_voices &&
        action == wei_voice_line::Action::CombatPrePosture) {
        audio_component = ResolveVictimPrePostureSpeaker(
            health_component, speaker_voice);
    }
    if (audio_component == 0) {
        // Wei's OWN audio component must speak, as with the push line. This is
        // also the fallback when the victim's voice could not be determined: an
        // unreadable or absent voice tag means the voice is UNKNOWN, and an
        // unknown voice must never be filled in with a random roster entry.
        audio_component = ResolvePlayerAudioComponent();
    }
    if (audio_component == 0) {
        return;
    }

    // Second, independent gate instance: these lines do not consume the push
    // line's cooldown or budget. Combat suppression inside the gate is
    // deliberately disabled (Evaluate's in_combat = false) because the taunt
    // families fire IN combat; the design note records that the shared v1
    // budget may need tightening after live testing.
    push_voice_line::Decision decision;
    {
        std::lock_guard<std::mutex> lock(g_wei_voice_line_gate_mutex);
        decision = g_wei_voice_line_gate.Evaluate(GetTickCount64(), false);
    }
    if (!decision.fire) {
        if (g_config.diag_combat_line_events &&
            g_wei_voice_line_diag_events.fetch_add(1, std::memory_order_relaxed) <
                kPushVoiceLineDiagLogBudget) {
            log::InfoF("wei_voice_line_suppressed cooldown=%u budget=%u",
                       decision.suppressed_by_cooldown ? 1u : 0u,
                       decision.suppressed_by_budget ? 1u : 0u);
        }
        return;
    }
    // The entry line survived the gate: consume the pending flag now so it
    // fires exactly once per combat flip.
    if (entry_pending) {
        g_wei_voice_line_entry_pending.store(false, std::memory_order_relaxed);
    }

    SpeechLineInvocation invocation{};
    switch (action) {
        case wei_voice_line::Action::CombatPrePosture:
            invocation = {kWeiPrePostureAction,
                          Fnv1aLower(kWeiPrePostureAction),
                          "wei_voice_line",
                          0,  // voice=0 = the component's own voice (sec 41).
                          g_config.diag_combat_line_events,
                          true,
                          &g_wei_voice_line_fired};
            break;
        case wei_voice_line::Action::CombatTauntCold:
            invocation = {kWeiTauntColdAction,
                          Fnv1aLower(kWeiTauntColdAction),
                          "wei_voice_line",
                          0,
                          g_config.diag_combat_line_events,
                          true,
                          &g_wei_voice_line_fired};
            break;
        case wei_voice_line::Action::CombatTauntAngry:
            invocation = {kWeiTauntAngryAction,
                          Fnv1aLower(kWeiTauntAngryAction),
                          "wei_voice_line",
                          0,
                          g_config.diag_combat_line_events,
                          true,
                          &g_wei_voice_line_fired};
            break;
        case wei_voice_line::Action::HitApology:
            invocation = {kWeiHitApologyAction,
                          Fnv1aLower(kWeiHitApologyAction),
                          "wei_voice_line",
                          0,
                          g_config.diag_combat_line_events,
                          true,
                          &g_wei_voice_line_fired};
            break;
        case wei_voice_line::Action::None:
            return;
    }
    // Log the voice that is about to speak, so a session can prove WHICH voice
    // fired rather than inferring it from the accept/reject verdict alone. The
    // invocation itself still names only the action - the engine composes the
    // full "<voice>.<action>.001." name from the component we hand it - so this
    // is the only place the resolved voice is visible.
    if (action == wei_voice_line::Action::CombatPrePosture &&
        g_config.diag_combat_line_events &&
        g_wei_voice_line_diag_events.fetch_add(1, std::memory_order_relaxed) <
            kPushVoiceLineDiagLogBudget) {
        log::InfoF("combat_pre_posture_voice voice=%s source=%s",
                   speaker_voice,
                   (g_config.restore_combat_lines_all_voices &&
                    speaker_voice != kWeiVoiceTag)
                       ? "victim"
                       : "wei");
    }
    InvokeVoiceLineAction(audio_component, invocation);
    if (action == wei_voice_line::Action::CombatTauntCold ||
        action == wei_voice_line::Action::CombatTauntAngry) {
        g_wei_voice_line_taunt_counter.fetch_add(1, std::memory_order_relaxed);
    }
}

// ---- Restored NPC voice lines --------------------------------------------
//
// police_harass / police_spot (restore_police_voice_lines) and
// combat_encourage (restore_encourage_voice_lines) are shipped in the DE banks
// but never triggered by gameplay. Unlike the Wei families these speak through
// the VICTIM's own audio component (design fact 2), so they need the health
// component this detour was handed.
//
// Gauntlet stage 16: same discipline as MaybeFireWeiVoiceLineOnDamage - every
// host memory read SEH-guarded, no allocation, no unbounded stack use, and a
// fault degrades to silence. The whole path early-outs when both keys are 0,
// before any combat read or gate work, so a default install is inert.
//
// The victim SimObject read lives in this leaf helper because MSVC forbids
// __try in a function that requires object unwinding (C2712): the trigger
// function below holds std::lock_guard scopes for its two gate instances.
std::uintptr_t TryReadVictimSimObject(void* health_component) noexcept {
    std::uintptr_t sim_object = 0;
    __try {
        sim_object = ReadSimObjectFromComponent(health_component);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sim_object = 0;
    }
    return sim_object;
}

void MaybeFireNpcVoiceLineOnDamage(void* health_component,
                                   void* attacker,
                                   int damage,
                                   bool projectile_damage) {
    if ((!g_config.restore_police_voice_lines &&
         !g_config.restore_encourage_voice_lines) ||
        health_component == nullptr || g_push_voice_line_invoking) {
        return;
    }

    // Player attribution once: the same comparison the engine makes in the
    // hooked function's prologue (attacker vs the player SimObject singleton).
    const bool player_attacker = IsPlayerAttacker(attacker);
    if (!player_attacker) {
        return;
    }

    // POLICE trigger: zero-damage non-projectile bump harasses; positive
    // damage spots. Projectiles never qualify (a drive-by is not a shove).
    const bool police_enabled = g_config.restore_police_voice_lines;
    const char* police_action = nullptr;
    if (police_enabled && !projectile_damage) {
        if (damage == 0) {
            police_action = kNpcPoliceHarassAction;
        } else if (damage > 0) {
            police_action = kNpcPoliceSpotAction;
        }
    }
    // ENCOURAGE trigger: the player just dealt real damage while combat is
    // known and active. Unknown combat state suppresses (the design requires
    // in_combat, not merely unobserved).
    bool encourage_trigger = false;
    if (g_config.restore_encourage_voice_lines && !projectile_damage &&
        damage > 0) {
        bool in_combat = false;
        if (ReadPlayerCombatState(in_combat) && in_combat) {
            encourage_trigger = true;
        }
    }
    if (police_action == nullptr && !encourage_trigger) {
        return;
    }
    if (IsGamePausedSafely()) {
        return;
    }

    // The VICTIM's SimObject comes from the health component this detour was
    // handed (rcx in HealthApplyDamage, design fact 1) - SEH-guarded like the
    // existing reads of the same field in DetourHealthApplyDamage.
    const std::uintptr_t sim_object = TryReadVictimSimObject(health_component);
    if (sim_object == 0) {
        return;
    }
    const std::uintptr_t audio_component =
        ResolveSimObjectAudioComponent(sim_object);
    if (audio_component == 0) {
        return;
    }

    // Two fresh gate instances, same v1 constants (1.5 s cooldown, 15/min).
    // Combat suppression is disabled for BOTH (Evaluate's in_combat = false):
    // the encourage trigger fires IN combat by design, and the police
    // triggers must also fire outside combat - the same accepted deviation as
    // the Wei gate (see MaybeFireWeiVoiceLineOnDamage). Each gate is locked
    // because HealthApplyDamage can run on different engine threads.
    if (police_action != nullptr) {
        push_voice_line::Decision police_decision;
        {
            std::lock_guard<std::mutex> lock(g_npc_police_voice_line_gate_mutex);
            police_decision = g_npc_police_voice_line_gate.Evaluate(
                GetTickCount64(), false);
        }
        if (police_decision.fire) {
            const SpeechLineInvocation police_invocation = {
                police_action,
                Fnv1aLower(police_action),
                "npc_voice_line",
                0,  // voice=0 = the victim component's own voice (sec 41).
                g_config.diag_npc_voice_line_events,
                true,
                &g_npc_voice_line_fired};
            InvokeVoiceLineAction(audio_component, police_invocation);
        } else if (g_config.diag_npc_voice_line_events &&
                   g_npc_voice_line_diag_events.fetch_add(
                       1, std::memory_order_relaxed) <
                       kPushVoiceLineDiagLogBudget) {
            log::InfoF("npc_voice_line_suppressed family=police cooldown=%u "
                       "budget=%u",
                       police_decision.suppressed_by_cooldown ? 1u : 0u,
                       police_decision.suppressed_by_budget ? 1u : 0u);
        }
    }
    if (encourage_trigger) {
        push_voice_line::Decision encourage_decision;
        {
            std::lock_guard<std::mutex> lock(
                g_npc_encourage_voice_line_gate_mutex);
            encourage_decision = g_npc_encourage_voice_line_gate.Evaluate(
                GetTickCount64(), false);
        }
        if (encourage_decision.fire) {
            const SpeechLineInvocation encourage_invocation = {
                kNpcEncourageAction,
                Fnv1aLower(kNpcEncourageAction),
                "npc_voice_line",
                0,
                g_config.diag_npc_voice_line_events,
                true,
                &g_npc_voice_line_fired};
            InvokeVoiceLineAction(audio_component, encourage_invocation);
        } else if (g_config.diag_npc_voice_line_events &&
                   g_npc_voice_line_diag_events.fetch_add(
                       1, std::memory_order_relaxed) <
                       kPushVoiceLineDiagLogBudget) {
            log::InfoF("npc_voice_line_suppressed family=encourage "
                       "cooldown=%u budget=%u",
                       encourage_decision.suppressed_by_cooldown ? 1u : 0u,
                       encourage_decision.suppressed_by_budget ? 1u : 0u);
        }
    }
}

bool DetourSpeechWrapper(void* target_component,
                         int voice_id,
                         unsigned int name_id,
                         unsigned int zero,
                         unsigned int reaction_id,
                         unsigned char flag) {
    if (g_speech_wrapper_original == nullptr) {
        return false;
    }
    if (!g_push_voice_line_invoking && g_config.diag_push_voice_line_events &&
        g_push_voice_line_wrapper_diag_events.fetch_add(1, std::memory_order_relaxed) <
            kPushVoiceLineDiagLogBudget) {
        const std::uintptr_t component = reinterpret_cast<std::uintptr_t>(target_component);
        std::uint8_t enabled = 0;
        std::uint32_t count = 0;
        std::uintptr_t object = 0;
        std::uint32_t threshold = 0;
        std::uint8_t flags = 0;
        SafeRead(component + kSpeechGuardEnabledOffset, enabled);
        SafeRead(component + kSpeechGuardCountOffset, count);
        SafeRead(component + kSpeechGuardObjectOffset, object);
        SafeRead(component + kSpeechGuardThresholdOffset, threshold);
        SafeRead(component + kSpeechGuardFlagsOffset, flags);
        log::InfoF("speech_wrapper voice=%d name_id=%u reaction=%u guard_enabled=%u "
                   "count=%u object=%s threshold=%u flags=%u",
                   voice_id,
                   name_id,
                   reaction_id,
                   enabled,
                   count,
                   object != 0 ? "ok" : "none",
                   threshold,
                   flags);
    }
    return g_speech_wrapper_original(target_component, voice_id, name_id, zero,
                                     reaction_id, flag);
}

void DetourPedestrianFrameRateThrottle() {
    if (g_pedestrian_frame_rate_throttle_original == nullptr) {
        return;
    }
    if (!g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        g_pedestrian_frame_rate_throttle_original();
        return;
    }
    if (!g_pedestrian_update_delta_valid_tls) {
        // Preserve calls made from an owner that has not been mapped. The
        // verified PedSpawnManager update supplies a delta through the TLS
        // scope above, while unknown callers retain stock behavior.
        g_pedestrian_frame_rate_throttle_original();
        g_pedestrian_throttle_stock_call_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const pedestrian_timing::StepResult step =
        g_pedestrian_throttle_scheduler_tls.Advance(g_pedestrian_update_delta_tls);
    g_pedestrian_throttle_frame_count.fetch_add(1, std::memory_order_relaxed);
    if (step.frame_delta_clamped) {
        g_pedestrian_throttle_clamped_frame_count.fetch_add(1, std::memory_order_relaxed);
    }
    for (std::uint32_t index = 0; index < step.steps; ++index) {
        g_pedestrian_frame_rate_throttle_original();
        g_pedestrian_throttle_stock_call_count.fetch_add(1, std::memory_order_relaxed);
    }

}

// Stimulus::Init emitter capture.
//
// RDX is the StimulusEmitterComponent* at entry (see
// StimulusInitEmitterProbe.h for the byte evidence). This is a standalone
// mechanism: it does NOT walk a SimObject component array. That walk cannot
// work here, because the emitter is a qNode over the CENTRAL
// StimulusManagerUpdateList rather than a self-node on any SimObject, so it is
// never reachable from a ped's component array at all.
//
// Three properties make the line trustworthy:
//   1. CONTROL. Every logged uid is one this function read itself, behind
//      __try/__except. A zero "readable" count is therefore an
//      uninterpretable instrument failure, reported as UNREADABLE -- never
//      as a claim that the engine has no emitter.
//   2. BUDGET. The guarded read is charged against a fixed budget that is set
//      only when the detour actually installs, so this engine entry cannot
//      pay for the probe forever and cannot pay at all when the hook is off.
//   3. ONCE. The uid and vtable log exactly once, on the first successful
//      read. Repeating them per call would bury the census under one line per
//      stimulus for the rest of the session.
void DetourStimulusInit(void* stimulus,
                        void* emitter,
                        void* stimulus_target) {
    g_stimulus_init_captures.fetch_add(1, std::memory_order_relaxed);
    if (emitter != nullptr) {
        g_stimulus_init_non_null.fetch_add(1, std::memory_order_relaxed);
    }

    // Charge the guarded read against a fixed budget and refund whatever the
    // gate declines to measure, so the cap means "at most N guarded reads"
    // rather than "at most N calls". A run of null emitters therefore cannot
    // starve the probe before it ever reads anything.
    const long budget_before =
        g_stimulus_init_probe_budget.fetch_sub(1, std::memory_order_relaxed);
    const bool read_attempted = budget_before > 0 && emitter != nullptr;
    if (read_attempted) {
        std::uintptr_t vtable = 0;
        std::uint32_t uid = 0;
        bool read_succeeded = false;
        // The emitter is engine-owned and its lifetime is not ours to know;
        // a stale or mid-teardown pointer must never take the process down.
        __try {
            vtable = *reinterpret_cast<const std::uintptr_t*>(emitter);
            uid = *reinterpret_cast<const std::uint32_t*>(
                      reinterpret_cast<std::uintptr_t>(emitter) +
                      stimulus::kEmitterTypeUidOffset);
            read_succeeded = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            read_succeeded = false;
        }
        if (read_succeeded) {
            g_stimulus_init_readable.fetch_add(1, std::memory_order_relaxed);
            // The control. A successful dereference proves only that the memory
            // was mapped; the vtable comparison is what proves the object was
            // actually a StimulusEmitterComponent. A mismatch is counted
            // separately rather than discarded, so a stale RVA is visibly
            // distinguishable from the engine passing some other object.
            const bool control_ok =
                vtable == ResolveAddress(kStimulusEmitterVftableRva);
            if (control_ok) {
                g_stimulus_init_control_ok.fetch_add(1, std::memory_order_relaxed);
                bool expected = false;
                if (g_stimulus_init_uid_logged.compare_exchange_strong(
                        expected, true, std::memory_order_relaxed)) {
                    g_stimulus_init_first_uid.store(uid,
                                                    std::memory_order_relaxed);
                    g_stimulus_init_first_vftable.store(vtable,
                                                        std::memory_order_relaxed);
                }
            } else {
                g_stimulus_init_control_miss.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } else {
        g_stimulus_init_probe_budget.fetch_add(1, std::memory_order_relaxed);
    }

    // Transparent: this probe changes no game behaviour, so the call always
    // reaches the original even if the capture above failed.
    __try {
        g_stimulus_init_original(stimulus, emitter, stimulus_target);
    } __finally {
        // __finally so a fault raised inside the original cannot skip the
        // remaining logging work and desynchronise the counts.
    }

    // Report exactly once, and only on a verdict the data actually supports:
    // the first SUCCESSFUL read, or -- if nothing was ever readable -- the
    // moment the budget is spent. Reporting on the first call instead would be
    // the failure mode that killed the four earlier emitter attempts: one call
    // that happened to carry a null emitter would log UNREADABLE and then stay
    // silent for the rest of the session, turning a later working sample into
    // silence. A negative here is only ever printed after N real attempts.
    const std::uint32_t readable =
        g_stimulus_init_readable.load(std::memory_order_relaxed);
    const std::uint32_t control_ok =
        g_stimulus_init_control_ok.load(std::memory_order_relaxed);
    const bool budget_spent = budget_before == 0;
    if (readable > 0 || budget_spent) {
        bool expected = false;
        if (g_stimulus_init_reported.compare_exchange_strong(
                expected, true, std::memory_order_relaxed)) {
            const std::uint32_t total_captures =
                g_stimulus_init_captures.load(std::memory_order_relaxed);
            const std::uint32_t non_null =
                g_stimulus_init_non_null.load(std::memory_order_relaxed);
            const std::uint32_t control_miss =
                g_stimulus_init_control_miss.load(std::memory_order_relaxed);
            const std::uint32_t uid =
                g_stimulus_init_first_uid.load(std::memory_order_relaxed);
            const std::uintptr_t vtable =
                g_stimulus_init_first_vftable.load(std::memory_order_relaxed);
            const stimulus::InitEmitterState state =
                stimulus::ClassifyInitEmitter(total_captures,
                                              readable,
                                              control_ok,
                                              uid);
            // `control` reports the VTABLE match, not "the read did not fault".
            // Those are different claims and conflating them is what let four
            // earlier readings in this campaign look proven when they were not.
            log::InfoF(
                "stimulus_init_emitter captures=%u non_null=%u readable=%u "
                "control=%s control_miss=%u uid=0x%08X vtable=0x%016llX "
                "expect_vtable=0x%016llX state=%s",
                total_captures,
                non_null,
                readable,
                control_ok != 0 ? "PASS" : "FAIL",
                control_miss,
                uid,
                static_cast<unsigned long long>(vtable),
                static_cast<unsigned long long>(
                    ResolveAddress(kStimulusEmitterVftableRva)),
                stimulus::NameOf(state));
        }
    }
}
bool ValidateHookSignature(std::uintptr_t address,
                           std::span<const std::uint8_t> signature,
                           const char* name) {
    if (runtime_patch::MatchesBytes(address, signature)) {
        return true;
    }
    log::WarnF("hook disabled name=%s reason=unexpected_bytes target=0x%p",
               name,
               reinterpret_cast<void*>(address));
    g_hook_creation_transaction_open = false;
    return false;
}

// Signature check for OPTIONAL, degrade-gracefully installs: same byte test,
// but a mismatch only skips the one feature. It must NOT close the hook
// creation transaction - ValidateHookSignature does that on mismatch, which
// would silently reject every hook the transaction installs afterwards (the
// cop radar hooks among them) behind a single generic warn.
bool ValidateOptionalHookSignature(std::uintptr_t address,
                                   std::span<const std::uint8_t> signature,
                                   const char* name) {
    if (runtime_patch::MatchesBytes(address, signature)) {
        return true;
    }
    log::WarnF("optional hook skipped name=%s reason=unexpected_bytes "
               "target=0x%p",
               name,
               reinterpret_cast<void*>(address));
    return false;
}

bool PreflightRequiredHookTarget(
    std::uintptr_t address,
    std::span<const std::uint8_t> signature,
    const char* name) {
    if (!runtime_patch::MatchesBytes(address, signature)) {
        log::ErrorF(
            "required hook preflight failed name=%s reason=unexpected_bytes "
            "target=0x%p",
            name,
            reinterpret_cast<void*>(address));
        g_hook_creation_transaction_open = false;
        return false;
    }
    const hook_guard::Result guard_result = g_hook_target_guard.Verify(
        reinterpret_cast<const void*>(address),
        hook_guard::kMinHookTargetVerificationBytes);
    if (guard_result.verified()) {
        return true;
    }
    log::ErrorF(
        "required hook preflight failed name=%s reason=target_rejected "
        "target=0x%p "
        "status=%s pe_status=%s rva=0x%X mismatch=%zu",
        name,
        reinterpret_cast<void*>(address),
        hook_guard::StatusName(guard_result.status),
        hook_guard::PeStatusName(guard_result.pe_status),
        guard_result.target_rva,
        guard_result.mismatch_offset);
    g_hook_creation_transaction_open = false;
    return false;
}

bool CreateHookDetour(std::uintptr_t address, void* detour, void** original, const char* name) {
    if (!g_hook_creation_transaction_open) {
        return false;
    }
    const hook_guard::Result guard_result = g_hook_target_guard.Verify(
        reinterpret_cast<const void*>(address), hook_guard::kMinHookTargetVerificationBytes);
    if (!guard_result.verified()) {
        log::ErrorF("hook target rejected name=%s target=0x%p status=%s pe_status=%s "
                    "rva=0x%X mismatch=%zu",
                    name,
                    reinterpret_cast<void*>(address),
                    hook_guard::StatusName(guard_result.status),
                    hook_guard::PeStatusName(guard_result.pe_status),
                    guard_result.target_rva,
                    guard_result.mismatch_offset);
        g_hook_creation_transaction_open = false;
        return false;
    }
    // Reserve the process-lifetime hook record before MinHook mutates
    // executable code. If allocation fails, no detour has been created and
    // stack unwinding is safe.
    try {
        g_created_hook_targets.push_back(address);
    } catch (...) {
        log::ErrorF("hook registry allocation failed before creating %s at 0x%p",
                    name,
                    reinterpret_cast<void*>(address));
        g_hook_creation_transaction_open = false;
        return false;
    }
    const MH_STATUS create_status =
        MH_CreateHook(reinterpret_cast<void*>(address), detour, original);
    if (create_status != MH_OK) {
        g_created_hook_targets.pop_back();
        log::ErrorF("MH_CreateHook failed for %s at 0x%p: %d",
                    name,
                    reinterpret_cast<void*>(address),
                    static_cast<int>(create_status));
        g_hook_creation_transaction_open = false;
        return false;
    }

    // The SDmodding MinHook build enables a hook as part of MH_CreateHook.
    // Keep one authoritative registry so a later failure can retain every
    // earlier detour as a transparent pass-through without losing ownership.
    log::InfoF("hook attached name=%s target=0x%p behavior=pending_commit",
               name,
               reinterpret_cast<void*>(address));
    return true;
}

bool RetainCreatedHooks() {
    if (g_created_hook_targets.empty()) {
        return true;
    }
    const bool was_retained =
        g_minhook_retained_process_lifetime.exchange(true, std::memory_order_acq_rel);
    if (!was_retained) {
        log::InfoF("minhook_hooks_retained_process_lifetime count=%zu",
                   g_created_hook_targets.size());
    }
    // Do not call MH_RemoveHook.  The bundled fork frees each original
    // trampoline there, while another thread can already be executing through
    // that pointer.  Keeping the registry also prevents a later retry from
    // accidentally creating a second hook for the same target.
    return true;
}

}  // namespace

// DOF uses its native parameter path, never a skipped render dispatcher.
// FUN_14005D550 refreshes six floats via FUN_140009300. The two blur
// strengths are copied to stage+0x18/+0x20 and are read by both the render
// dispatcher and the later compositors' native IsActive predicate. Skipping
// only the dispatcher leaves IsActive true and samples unproduced textures.
// The mode byte is the same source the render caller uses to choose NIS.
constexpr std::uintptr_t kDofModeRva = 0x02136362;
constexpr std::array<std::uint8_t, 16> kDofSetParametersSignature{
    0x8B, 0x02, 0x89, 0x41, 0x0C, 0x8B, 0x42, 0x04,
    0x89, 0x41, 0x10, 0x8B, 0x42, 0x08, 0x89, 0x41};
using DofSetParametersFn = void (__fastcall *)(std::uintptr_t, const void*);
DofSetParametersFn g_dof_set_parameters_original = nullptr;

void __fastcall DetourDofSetParameters(std::uintptr_t stage, const void* parameters) {
    if (g_dof_set_parameters_original == nullptr) {
        return;
    }
    if (g_behavior_transaction_ready.load(std::memory_order_acquire) &&
        (g_config.disable_dof || g_config.disable_cutscene_dof)) {
        std::uint8_t nis_mode = 0;
        const std::uintptr_t mode_address = ResolveAddress(kDofModeRva);
        if (mode_address != 0 && SafeRead(mode_address, nis_mode) &&
            (nis_mode ? g_config.disable_cutscene_dof : g_config.disable_dof)) {
            std::array<float, 6> adjusted{};
            if (parameters != nullptr &&
                SafeRead(reinterpret_cast<std::uintptr_t>(parameters), adjusted)) {
                adjusted[3] = 0.0f;  // Near blur strength, not near range.
                adjusted[5] = 0.0f;  // Far blur strength, not far range.
                g_dof_set_parameters_original(stage, adjusted.data());
                return;
            }
        }
    }
    g_dof_set_parameters_original(stage, parameters);
}

// DisableDOF install path: independent of the probe/radar gating, byte
// verified, and inert unless a DoF key is set.
//
// Deliberately NOT the donor's raw MH_CreateHook + runtime_patch::MatchesBytes.
// This goes through CreateHookDetour / ValidateHookSignature so the target joins
// the hook-creation transaction and the rollback registry, and the log lines
// match every other install in the tree. Live contains exactly one raw
// MH_CreateHook call and it is inside CreateHookDetour itself.
void InstallDofParametersHook() {
    // `config` is an Initialize() parameter; the file-scope accessor is
    // g_config, which is what the donor read too.
    if (!g_config.disable_dof && !g_config.disable_cutscene_dof) {
        return;
    }
    const std::uintptr_t address = ResolveAddress(kDofSetParametersRva);
    if (address == 0 ||
        !ValidateOptionalHookSignature(address, kDofSetParametersSignature,
                                       "dof_parameters")) {
        return;
    }
    if (!CreateHookDetour(address,
                          reinterpret_cast<void*>(&DetourDofSetParameters),
                          reinterpret_cast<void**>(&g_dof_set_parameters_original),
                          "dof_parameters")) {
        return;
    }
    log::Info("dof_parameters hooked native_render_dispatch=preserved");
}

// Script API `set_blip_type_visible`: shows/hides a blip type. Verified by tracing
// the Minimap script registration at 0x4D45B0 (name rva 0x17B21D8 -> impl 0x4ED3F0).
constexpr std::uintptr_t kBlipTypeVisibleRva = 0x004ED3F0;

// Script blip creator `add_blip_xy`. Its first argument resolves as a string through
// the same chain as set_blip_type_visible (verified in its body at 0x4E2D00: args
// at [rcx+0x60], args[0] -> +8 -> +0x20 -> deref -> string), so a script-created blip
// names its own type here.
constexpr std::uintptr_t kAddBlipXyRva = 0x004E2D00;

// Script `enable_vehicles_blip`. Verified in its body at 0x4E5370: one argument in
// RCX (args at [rcx+0x60]), args[0] is the flag, and args[1]'s value is stored into
// the global at rva 0x2418394. Logging it shows whether the game drives vehicle
// (police) blips through script at all.
constexpr std::uintptr_t kEnableVehiclesBlipRva = 0x004E5370;
// Blip symbol application (was 0x5223E0): removed with the symbol-skip
// detour (see the removal note at its former site). The four cop-symbol
// RVAs below were its exclusive evidence; the wrapper evidence that follows
// stays for the diag probe and the documented hiding mechanism.
// Blip wrapper set-data (0xCB7F0): the per-blip update the engine runs when a
// minimap blip wrapper receives new blip data. The blip data struct carries a
// graphic type id at +4 which the blip-graphic factory (0xC1540) dispatches
// against runtime-registered type ids; the two cop graphic ids live in the
// globals below (filled from Data\UI\Game\ui_ambient_map_blips.xml at UI
// init, verified 2026-09-26 from the factory's cmp chain at 0xC2C0A/0xC2C54).
// The game hides a blip type itself at 0xCB84C by clearing the wrapper's
// hidden byte at +0x20 when the data's +0 id matches the global at
// 0x21739C0 - the feature reuses that exact native mechanism for the cop
// graphic types instead of patching ids or graphics.
constexpr std::uintptr_t kBlipWrapperSetDataRva = 0x000CB7F0;
constexpr std::uintptr_t kCopBlipTypeGraphicAGva = 0x02174768;
constexpr std::uintptr_t kCopBlipTypeGraphicBGva = 0x021744E8;
constexpr std::uintptr_t kVehiclesBlipGlobalRva = 0x002418394;

// Blip probe. `set_blip_type_visible` is the script API that shows/hides a blip
// type, so the game's own scripts reveal the police blip type by flipping it when the
// wanted state changes. Read-only: the call is logged and forwarded untouched.
// Verified from the function body: it takes a single argument in RCX (the script
// context; the arguments live at [rcx+0x60]) and it RETURNS a value in RAX, so the
// detour must forward that value rather than discard it.
using BlipTypeVisibleFn = std::uintptr_t(__fastcall*)(std::uintptr_t);
BlipTypeVisibleFn g_blip_type_visible_original = nullptr;
std::atomic<int> g_blip_probe_budget{600};

std::uintptr_t __fastcall DetourBlipTypeVisible(std::uintptr_t context) {
    if (g_config.enable_logging && g_config.diag_blip_probe) {
        int remaining = g_blip_probe_budget.load(std::memory_order_relaxed);
        if (remaining > 0 &&
            g_blip_probe_budget.compare_exchange_strong(remaining, remaining - 1,
                                                        std::memory_order_relaxed)) {
            std::uintptr_t args = 0, name_arg = 0, visible_arg = 0;
            std::uintptr_t name_value = 0, visible_value = 0, name_holder = 0;
            std::uintptr_t name = 0;
            std::uint64_t visible = 0;
            const bool decoded =
                SafeRead(context + 0x60, args) && args != 0 &&
                SafeRead(args, name_arg) && name_arg != 0 &&
                SafeRead(args + 8, visible_arg) && visible_arg != 0 &&
                SafeRead(name_arg + 8, name_value) && name_value != 0 &&
                SafeRead(visible_arg + 8, visible_value) && visible_value != 0 &&
                SafeRead(name_value + 0x20, name_holder) && name_holder != 0 &&
                SafeRead(name_holder, name) && name != 0 &&
                SafeRead(visible_value + 0x20, visible);
            const std::string text = decoded ? ReadCString(name, 96) : std::string{};
            // Only report a transition: the wanted-state flip is the signal, and
            // repeated identical calls would otherwise eat the whole budget.
            // The dedupe state is mutex-guarded: script dispatch can reach
            // this detour from more than one thread and the assignment below
            // allocates.
            static std::mutex last_name_mutex;
            static std::string last_name;
            static int last_visible = -1;
            std::scoped_lock dedupe_lock(last_name_mutex);
            const int visible_flag = visible != 0 ? 1 : 0;
            if (text != last_name || visible_flag != last_visible) {
                last_name = text;
                last_visible = visible_flag;
                log::InfoF(
                    "blip_probe set_blip_type_visible decoded=%d name=%s visible=%d",
                    decoded ? 1 : 0, text.c_str(), visible_flag);
            }
        }
    }
    if (g_blip_type_visible_original != nullptr) {
        // Forward the original's return value: the caller can consume it, and
        // discarding it hands back whatever happened to be in RAX.
        return g_blip_type_visible_original(context);
    }
    return 0;
}

// Second half of the blip probe: name each blip the game adds through the script API.
using AddBlipXyFn = std::uintptr_t(__fastcall*)(std::uintptr_t);
AddBlipXyFn g_add_blip_xy_original = nullptr;

std::uintptr_t __fastcall DetourAddBlipXy(std::uintptr_t context) {
    if (g_config.enable_logging && g_config.diag_blip_probe) {
        int remaining = g_blip_probe_budget.load(std::memory_order_relaxed);
        if (remaining > 0 &&
            g_blip_probe_budget.compare_exchange_strong(remaining, remaining - 1,
                                                        std::memory_order_relaxed)) {
            std::uintptr_t args = 0, arg0 = 0, payload = 0, holder = 0, name = 0;
            const bool decoded =
                SafeRead(context + 0x60, args) && args != 0 &&
                SafeRead(args, arg0) && arg0 != 0 &&
                SafeRead(arg0 + 8, payload) && payload != 0 &&
                SafeRead(payload + 0x20, holder) && holder != 0 &&
                SafeRead(holder, name) && name != 0;
            const std::string text =
                decoded ? ReadCString(name, 96) : std::string{};
            // Mutex-guarded for the same multi-thread reason as the
            // set_blip_type_visible dedupe above.
            static std::mutex last_name_mutex;
            static std::string last_name;
            std::scoped_lock dedupe_lock(last_name_mutex);
            if (text != last_name) {
                last_name = text;
                log::InfoF("blip_probe add_blip_xy decoded=%d name=%s",
                           decoded ? 1 : 0, text.c_str());
            }
        }
    }
    if (g_add_blip_xy_original != nullptr) {
        return g_add_blip_xy_original(context);
    }
    return 0;
}

// Read-only blip probe for the vehicles-blip control.
using EnableVehiclesBlipFn = std::uintptr_t(__fastcall*)(std::uintptr_t);
EnableVehiclesBlipFn g_enable_vehicles_blip_original = nullptr;

std::uintptr_t __fastcall DetourEnableVehiclesBlip(std::uintptr_t context) {
    if (g_config.enable_logging && g_config.diag_blip_probe) {
        int remaining = g_blip_probe_budget.load(std::memory_order_relaxed);
        if (remaining > 0 &&
            g_blip_probe_budget.compare_exchange_strong(remaining, remaining - 1,
                                                        std::memory_order_relaxed)) {
            std::uintptr_t args = 0, flag_arg = 0, value_arg = 0, flag_obj = 0,
                           value_obj = 0;
            std::uint64_t flag = 0, value = 0;
            const bool readable =
                SafeRead(context + 0x60, args) && args != 0 &&
                SafeRead(args, flag_arg) && flag_arg != 0 &&
                SafeRead(args + 8, value_arg) && value_arg != 0 &&
                SafeRead(flag_arg + 8, flag_obj) && flag_obj != 0 &&
                SafeRead(value_arg + 8, value_obj) && value_obj != 0 &&
                SafeRead(flag_obj + 0x20, flag) &&
                SafeRead(value_obj + 0x20, value);
            const std::uintptr_t global = ResolveAddress(kVehiclesBlipGlobalRva);
            std::uint32_t stored = 0;
            (void)SafeRead(global, stored);
            log::InfoF("blip_probe enable_vehicles_blip readable=%d flag=%lld "
                       "value=%lld global_before=%u",
                       readable ? 1 : 0, static_cast<long long>(flag),
                       static_cast<long long>(value), static_cast<unsigned>(stored));
        }
    }
    if (g_enable_vehicles_blip_original != nullptr) {
        return g_enable_vehicles_blip_original(context);
    }
    return 0;
}

// NOTE (fight-club crash hunt): the cop-symbol skip detour lived here. Its
// presence crashed the game within ~40-90 s of boot on every run with
// HideCopRadarIcons=1 (7/7), while every run without it stayed clean (3/3)
// - yet disassembly of all 9 callers shows every one ignores its return
// value, so no arm of it can explain the kill. Mechanism unidentified;
// presence itself is refused instead of chaining into it. Cop icons stay
// hidden through the pursuit heat-controller path below. The four cop
// symbol RVAs were its exclusive address evidence and are removed with it;
// the wrapper detour below stays for the diag probe only.

// Blip wrapper set-data (0xCB7F0): runs whenever a minimap blip wrapper
// receives new blip data. The engine itself hides one blip type here by
// clearing the wrapper's hidden byte (+0x20) when the data's +0 id matches a
// hard-coded global; the cop-graphic feature keys on the +4 graphic type id
// instead, which the blip-graphic factory dispatches against the two
// runtime-registered cop ids. Clearing the same hidden byte reuses the
// engine's own proven hide mechanism - no ids are rewritten and no graphics
// are patched, so every other blip (player arrow, objectives, stores,
// traffic) is untouched, and the wrapper keeps updating normally.
using BlipWrapperSetDataFn = void(__fastcall *)(void *, void *);
BlipWrapperSetDataFn g_blip_wrapper_set_data_original = nullptr;
std::atomic<int> g_cop_blip_hide_log_budget{20};
std::atomic<int> g_blip_type_probe_budget{64};

void __fastcall DetourBlipWrapperSetData(void *wrapper, void *blip_data) {
    if (g_blip_wrapper_set_data_original != nullptr) {
        g_blip_wrapper_set_data_original(wrapper, blip_data);
    }
    const std::uintptr_t wrapper_address =
        reinterpret_cast<std::uintptr_t>(wrapper);
    const std::uintptr_t data_address =
        reinterpret_cast<std::uintptr_t>(blip_data);
    if (wrapper_address == 0 || data_address == 0) {
        return;
    }
    std::uint32_t category_type = 0;
    std::uint32_t graphic_type = 0;
    if (!SafeRead(data_address, category_type) ||
        !SafeRead(data_address + 4, graphic_type)) {
        return;
    }
    if (g_config.enable_logging && g_config.diag_blip_probe) {
        // Deduped per (category, graphic) pair so the budget is spent on new
        // blip kinds rather than per-frame chatter.
        static std::atomic<std::uint64_t> seen_types[32] = {};
        const std::uint64_t key =
            (static_cast<std::uint64_t>(category_type) << 32) | graphic_type;
        bool known = false;
        for (auto &slot : seen_types) {
            std::uint64_t expected = 0;
            if (slot.load(std::memory_order_relaxed) == key) {
                known = true;
                break;
            }
            if (slot.compare_exchange_strong(expected, key,
                                             std::memory_order_relaxed)) {
                break;
            }
            if (slot.load(std::memory_order_relaxed) == key) {
                known = true;
                break;
            }
        }
        if (!known) {
            int remaining = g_blip_type_probe_budget.load(std::memory_order_relaxed);
            if (remaining > 0 &&
                g_blip_type_probe_budget.compare_exchange_strong(
                    remaining, remaining - 1, std::memory_order_relaxed)) {
                log::InfoF("blip_probe wrapper_set_data category=%u graphic=%u",
                           static_cast<unsigned>(category_type),
                           static_cast<unsigned>(graphic_type));
            }
        }
    }
    if (!g_config.hide_cop_radar_icons) {
        return;
    }
    std::uint32_t cop_type_a = 0;
    std::uint32_t cop_type_b = 0;
    const bool a_registered =
        SafeRead(ResolveAddress(kCopBlipTypeGraphicAGva), cop_type_a) &&
        cop_type_a != 0 && cop_type_a != 0xFFFFFFFFu;
    const bool b_registered =
        SafeRead(ResolveAddress(kCopBlipTypeGraphicBGva), cop_type_b) &&
        cop_type_b != 0 && cop_type_b != 0xFFFFFFFFu;
    const bool is_cop_graphic =
        (a_registered && graphic_type == cop_type_a) ||
        (b_registered && graphic_type == cop_type_b);
    if (!is_cop_graphic) {
        return;
    }
    // The engine's own hidden byte (see the native check at 0xCB84C).
    //
    // Written through SafeWrite, not as a raw store. This runs on the game
    // thread for EVERY blip update, and a wrapper pointer that is stale or
    // mid-teardown would take the process down - which is precisely the failure
    // SafeWrite exists to prevent, and why every other engine write in this
    // file goes through it. A failed write also must not be reported as a
    // successful hide, so the skip-log is gated on it.
    const unsigned char hidden = 0;
    if (!SafeWrite(wrapper_address + 0x20, hidden)) {
        return;
    }
    int remaining = g_cop_blip_hide_log_budget.load(std::memory_order_relaxed);
    if (remaining > 0 &&
        g_cop_blip_hide_log_budget.compare_exchange_strong(
            remaining, remaining - 1, std::memory_order_relaxed)) {
        log::InfoF("cop_radar_icons: hid blip wrapper 0x%llX category=%u "
                   "graphic=%u",
                   static_cast<unsigned long long>(wrapper_address),
                   static_cast<unsigned>(category_type),
                   static_cast<unsigned>(graphic_type));
    }
}

// First-seen heat-controller instance = the primary HUD heat meter. Atomic and
// reset in Initialize so a level reload that rebuilds the instances cannot
// leave a stale pointer marking the real primary as a pursuit instance (which
// would pin the main HUD heat meter at zero for the whole session).
std::atomic<std::uintptr_t> g_primary_heat_sub{0};
constexpr std::array<std::uint8_t, 16> kHeatMeterApplySignature{
    0x48, 0x85, 0xD2, 0x0F, 0x84, 0xAF, 0x02, 0x00, 0x00, 0x48, 0x8B, 0xC4,
    0x55, 0x41, 0x54, 0x41};
using HeatMeterApplyFn = void (__fastcall *)(std::uintptr_t, std::uintptr_t);
HeatMeterApplyFn g_heat_meter_apply_original = nullptr;

constexpr std::array<std::uint8_t, 16> kGfxSetVariableSignature{
    0x48, 0x8B, 0x49, 0x18, 0x48, 0x8B, 0x01, 0x48, 0xFF, 0xA0, 0x80, 0x01,
    0x00, 0x00, 0xCC, 0xCC};
struct GfxValueMirror {
    std::uint8_t data[16];
    std::uint64_t aux;
    std::uint32_t type;
    std::uint8_t pad_to_value[4];
    std::uint8_t bool_value;
    std::uint8_t pad[15];
};
static_assert(offsetof(GfxValueMirror, type) == 0x18 &&
                  offsetof(GfxValueMirror, bool_value) == 0x20 &&
                  sizeof(GfxValueMirror) == 0x30,
              "Scaleform boolean mirror must match native value layout");
using GfxSetVariableFn = void (__fastcall *)(std::uintptr_t, const char *,
                                             GfxValueMirror *, std::uint32_t);
GfxSetVariableFn g_gfx_set_variable = nullptr;

struct DynamicRadarState {
    bool known;
    bool show;
};

DynamicRadarState DynamicRadarSampleState() {
    DynamicRadarState state{false, true};
    const std::uintptr_t player = TryReadPlayerSimObject();
    bool in_vehicle = false;
    if (player == 0 || !TryReadCharacterInVehicle(player, in_vehicle)) {
        return state;
    }
    std::uint32_t mode = 0;
    const bool mode_known = ReadUiModeState(mode);
    state.known = true;
    state.show = in_vehicle || (mode_known && mode == 2);
    return state;
}

// Second DynamicRadar hide layer, ported from the donor snapshot
// (Hooks.cpp.restored-fixed 135-152, 15584-15620, 15764-15790). The
// heat-meter-restore restructure dropped this layer on the theory that the
// clip-based hide in DetourHeatMeterApply covers the whole cluster; the first
// live session on that build (2026-10-02 22:15) showed the radar cluster
// hiding only partially, so the layer is restored as the donor had it: the
// minimap-visible global write plus dirty byte and the direct
// mc_HealthBar._visible setter, every frame in both directions, riding the
// HUD UI apply tick. The heat-meter clip arm above stays as-is; the donor ran
// both layers together.
constexpr std::uintptr_t kUiMinimapVisibleValueRva = 0x208E6C0;
constexpr std::uintptr_t kHudScreenPointerRva = 0x2430CC8;
constexpr std::array<std::uint8_t, 16> kHudUiApplySignature{
    0x48, 0x8B, 0xC4, 0x55, 0x48, 0x8B, 0xEC, 0x48, 0x81, 0xEC, 0x80, 0x00,
    0x00, 0x00, 0x48, 0xC7};
using HudUiApplyFn = void (__fastcall *)(std::uintptr_t);
HudUiApplyFn g_hud_ui_apply_original = nullptr;
// FUN_1405E1A40(ignored, screen, byte visible): direct mc_HealthBar._visible
// setter over the same GFx movie root the applier uses.
constexpr std::array<std::uint8_t, 16> kHudHealthBarSetterSignature{
    0x4C, 0x8B, 0xDC, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x49, 0xC7, 0x43, 0xB8,
    0xFE, 0xFF, 0xFF, 0xFF};
using HudHealthBarSetterFn = void (__fastcall *)(std::uintptr_t,
                                                 std::uintptr_t,
                                                 std::uint8_t);
HudHealthBarSetterFn g_hud_health_bar_setter = nullptr;

void __fastcall DetourHudUiApply(std::uintptr_t hud) {
    // Mutating arm stays transparent until the hook transaction commits.
    if (g_config.dynamic_radar &&
        g_behavior_transaction_ready.load(std::memory_order_acquire)) {
        const DynamicRadarState state = DynamicRadarSampleState();
        if (state.known) {
            const std::uintptr_t value_global =
                ResolveAddress(kUiMinimapVisibleValueRva);
            const std::uintptr_t hud_slot_global =
                ResolveAddress(kHudScreenPointerRva);
            std::uintptr_t hud_object = 0;
            if (value_global != 0 && hud_slot_global != 0 &&
                SafeRead(hud_slot_global, hud_object) && hud_object != 0) {
                const std::uint8_t visible = state.show ? 1 : 0;
                SafeWrite(value_global, visible);
                const std::uint8_t dirty = 1;
                SafeWrite(hud_object + 0xBE8, dirty);
                if (g_hud_health_bar_setter) {
                    // The GFx movie root can tear down mid-session (level
                    // streaming, pursuit start/end). Every other engine call
                    // in this file is SEH-guarded; this setter was the odd
                    // one out. Fail open: a fault skips only this write and
                    // the engine applier below still runs.
                    static std::atomic<int> hud_setter_fault_budget{8};
                    __try {
                        // Both directions, matching the donor's live-verified
                        // behavior: the old two-layer build forced the health bar
                        // visible here every frame and vehicles were unaffected,
                        // so this setter write does not reproduce the clip-arm's
                        // documented forced-true conflict.
                        g_hud_health_bar_setter(hud, hud, visible);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        int remaining =
                            hud_setter_fault_budget.load(std::memory_order_relaxed);
                        if (remaining > 0 &&
                            hud_setter_fault_budget.compare_exchange_strong(
                                remaining, remaining - 1,
                                std::memory_order_relaxed)) {
                            log::WarnF("dynamic_radar hud setter fault skipped "
                                       "visible=%u",
                                       static_cast<unsigned>(visible));
                        }
                    }
                }
            }
        }
    }
    if (g_hud_ui_apply_original) {
        g_hud_ui_apply_original(hud);
    }
}

// ---- Script full-screen filter capture ([Debug] ScreenFilterProbe) --------
//
// Every script filter request (enable_screen_filter, enable_fullscreen_filter,
// disable_fullscreen_filter, disable_all_fullscreen_filters) converges on
// FUN_14005d830(id, enable, priority) - see
// artifacts/reverse-engineering/2026-09-28-stunt-screen-fx-desaturation-driver.md.
// Diagnostic-first: log every ENABLE request (capped), forward everything.
// Once the id behind a wash the player wants gone is captured from a live
// session, this funnel becomes the suppression seam.
constexpr std::uintptr_t kScriptScreenFilterApplyRva = 0x5D830;
constexpr std::array<std::uint8_t, 12> kScriptScreenFilterApplySignature{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48};
using ScriptScreenFilterApplyFn =
    void (__fastcall*)(std::uint32_t, std::uint8_t, std::int32_t);
ScriptScreenFilterApplyFn g_screen_filter_apply_original = nullptr;
std::atomic<unsigned long> g_screen_filter_probe_calls{0};

void __fastcall DetourScriptScreenFilterApply(std::uint32_t filter_id,
                                              std::uint8_t enable,
                                              std::int32_t priority) {
    if (g_config.probe_screen_filter && enable != 0) {
        const unsigned long call = g_screen_filter_probe_calls.fetch_add(
            1, std::memory_order_relaxed);
        if (call < 64) {
            log::InfoF("screen_filter_probe call=%lu id=0x%08X enable=%u "
                       "priority=%d",
                       call,
                       filter_id,
                       static_cast<unsigned>(enable),
                       static_cast<int>(priority));
        }
    }
    if (g_screen_filter_apply_original != nullptr) {
        g_screen_filter_apply_original(filter_id, enable, priority);
    }
}

void __fastcall DetourHeatMeterApply(std::uintptr_t sub, std::uintptr_t hud) {
    // HideCopRadarIcons: the pursuit-spawned heat-controller instance is the
    // minimap's cop indicator - it invokes Cops_SetHeatLevel on its own movie
    // (live evidence 2026-09-26 21:15: a third controller instance appears
    // only when the pursuit starts, b4=1, with its own movie pointer, while
    // the init-time instance is the main HUD heat meter). Zeroing the heat
    // byte (+4) of every non-primary instance each frame keeps the indicator
    // at level 0 - no cop icons, no flashing - while the primary instance
    // keeps driving the main HUD heat meter. Display-only: the controller
    // still runs, nothing else is touched. The mutating arms stay transparent
    // until the hook transaction commits; the passthrough below always runs.
    const bool behavior_ready =
        g_behavior_transaction_ready.load(std::memory_order_acquire);
    if (behavior_ready && g_config.hide_cop_radar_icons && sub != 0) {
        std::uintptr_t primary_heat_sub =
            g_primary_heat_sub.load(std::memory_order_relaxed);
        if (primary_heat_sub == 0) {
            g_primary_heat_sub.compare_exchange_strong(primary_heat_sub, sub,
                                                        std::memory_order_relaxed,
                                                        std::memory_order_relaxed);
        } else if (sub != primary_heat_sub) {
            SafeWrite(sub + 4, 0);
            static std::atomic<int> pursuit_zero_log_budget{12};
            int remaining = pursuit_zero_log_budget.load(std::memory_order_relaxed);
            if (remaining > 0 &&
                pursuit_zero_log_budget.compare_exchange_strong(
                    remaining, remaining - 1, std::memory_order_relaxed)) {
                log::InfoF("cop_radar_icons: zeroed pursuit heat instance "
                           "sub=0x%llX hud=0x%llX",
                           static_cast<unsigned long long>(sub),
                           static_cast<unsigned long long>(hud));
            }
        }
    }
    // sub != 0 is load-bearing, matching the cop-icon and probe arms below:
    // SafeWrite only rejects address == 0, so a null `sub` would reach it as
    // 5 / 6 and write into the first page instead of failing.
    if (behavior_ready && g_config.dynamic_radar && sub != 0) {
        const DynamicRadarState state = DynamicRadarSampleState();
        if (state.known) {
            const std::uint8_t hidden = 0;
            const std::uint8_t dirty = 1;
            if (!state.show) {
                SafeWrite(sub + 5, hidden);
                SafeWrite(sub + 6, dirty);
            }
            // Face gauge: _level0.mc_CombatMeter is never touched by game
            // code, so it is forced every frame in BOTH directions (hiding
            // and restore). The three game-managed clips (minimapBorder,
            // OuterRing, HealthBar) are forced false per frame only while
            // the cluster is hidden - that re-assertion is what defeats the
            // game's transient damage-flash re-shows; when the cluster is
            // shown they are left to the game's own deferred applier,
            // because externally writing them true every frame conflicts
            // with its restore path and leaves the dragon frame and bars
            // missing in vehicles.
            std::uintptr_t inner = 0;
            std::uintptr_t movie = 0;
            if (g_gfx_set_variable && hud != 0 &&
                SafeRead(hud + 0x60, inner) && inner != 0 &&
                SafeRead(inner + 0x140, movie) && movie != 0) {
                GfxValueMirror value{};
                value.type = 2;
                value.bool_value = state.show ? 1 : 0;
                // The GFx movie object is only SafeRead-validated, never
                // pinned: it can tear down between the read and this call.
                // Fail open with a budgeted first-fault record instead of
                // taking the process down from a display-only write.
                static std::atomic<int> gfx_set_variable_fault_budget{8};
                __try {
                    if (state.show) {
                        g_gfx_set_variable(movie, "mc_CombatMeter._visible",
                                           &value, 1);
                    } else {
                        static const char *const kClusterClips[] = {
                            "mc_minimapBorder._visible",
                            "mcOuterRing._visible",
                            "mc_HealthBar._visible",
                            "mc_CombatMeter._visible",
                        };
                        for (const char *path : kClusterClips) {
                            g_gfx_set_variable(movie, path, &value, 1);
                        }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    int remaining = gfx_set_variable_fault_budget.load(
                        std::memory_order_relaxed);
                    if (remaining > 0 &&
                        gfx_set_variable_fault_budget.compare_exchange_strong(
                            remaining, remaining - 1,
                            std::memory_order_relaxed)) {
                        log::WarnF("dynamic_radar gfx_set_variable fault skipped "
                                   "show=%d",
                                   state.show ? 1 : 0);
                    }
                }
            }
        }
    }
    if (g_heat_meter_apply_original) {
        g_heat_meter_apply_original(sub, hud);
    }
    // Heat-controller probe (read-only, appended after the feature logic so it cannot
    // change DynamicRadar's behaviour). Logs each distinct controller instance once: if
    // the minimap stars are driven by a second instance, it appears here with its own
    // movie pointer and state bytes.
    if (g_config.enable_logging && g_config.diag_blip_probe && sub != 0) {
        static std::uintptr_t seen_subs[8] = {};
        static std::size_t seen_sub_count = 0;
        bool known = false;
        for (std::size_t i = 0; i < seen_sub_count; ++i) {
            if (seen_subs[i] == sub) {
                known = true;
                break;
            }
        }
        if (!known && seen_sub_count < 8) {
            seen_subs[seen_sub_count++] = sub;
            std::uint8_t byte4 = 0;
            std::uint8_t byte5 = 0;
            std::uint8_t byte6 = 0;
            (void)SafeRead(sub + 4, byte4);
            (void)SafeRead(sub + 5, byte5);
            (void)SafeRead(sub + 6, byte6);
            std::uintptr_t inner = 0;
            std::uintptr_t movie = 0;
            if (hud != 0) {
                (void)SafeRead(hud + 0x60, inner);
                if (inner != 0) {
                    (void)SafeRead(inner + 0x140, movie);
                }
            }
            log::InfoF(
                "heat_probe sub=0x%llX b4=%u b5=%u b6=%u hud=0x%llX movie=0x%llX",
                static_cast<unsigned long long>(sub), static_cast<unsigned>(byte4),
                static_cast<unsigned>(byte5), static_cast<unsigned>(byte6),
                static_cast<unsigned long long>(hud),
                static_cast<unsigned long long>(movie));
        }
    }
}

// Heat-meter / DynamicRadar install.
//
// STRUCTURED DELIBERATELY DIFFERENTLY FROM THE DONOR, and the difference is
// load-bearing. The donor early-returns on `!dynamic_radar` at 15782-15784
// BEFORE this hook, and DetourHeatMeterApply is what carries the THIRD
// hide_cop_radar_icons suppression path. Taken verbatim, HideCopRadarIcons=1
// with DynamicRadar=0 installs nothing and the feature is back to 2-of-3 -
// silently, with a green build. Same defect class as the CameraFeatureRequests
// field drop, reached a different way.
//
// So: the heat hook installs under EITHER key, and the face-gauge resolve stays
// `dynamic_radar`-only because it serves mc_CombatMeter._visible, which the
// cop-icon path does not touch.
void InstallDynamicRadarHook() {
    const bool cop_icons_requested = g_config.hide_cop_radar_icons;
    const bool radar_requested = g_config.dynamic_radar;
    if (!cop_icons_requested && !radar_requested) {
        return;
    }

    const std::uintptr_t heat_address = ResolveAddress(kHeatMeterApplyRva);
    if (heat_address == 0 ||
        !ValidateOptionalHookSignature(heat_address, kHeatMeterApplySignature,
                               "heat_meter_apply")) {
        return;
    }
    if (!CreateHookDetour(heat_address,
                          reinterpret_cast<void*>(&DetourHeatMeterApply),
                          reinterpret_cast<void**>(&g_heat_meter_apply_original),
                          "heat_meter_apply")) {
        log::Warn("heat_meter: hook did not attach; cop icons stay visible");
        return;
    }
    log::InfoF("heat_meter hooked radar=%d cop_icons=%d",
               radar_requested ? 1 : 0, cop_icons_requested ? 1 : 0);

    if (!radar_requested) {
        return;
    }
    // Face gauge only. mc_CombatMeter._visible is never written by game code,
    // so the dynamic radar has to drive it through the GFx variable setter.
    const std::uintptr_t setvar_address = ResolveAddress(kGfxSetVariableRva);
    if (setvar_address == 0 ||
        !ValidateOptionalHookSignature(setvar_address, kGfxSetVariableSignature,
                               "gfx_set_variable")) {
        log::Warn("dynamic_radar: gfx_set_variable unresolved; face gauge stays native");
        return;
    }
    g_gfx_set_variable =
        reinterpret_cast<GfxSetVariableFn>(setvar_address);
    log::Info("dynamic_radar gfx_set_variable resolved");

    // Second hide layer (minimap-visible global + dirty byte + direct
    // mc_HealthBar._visible setter) on the HUD UI apply tick. Signature
    // failure here degrades to the clip-only hide rather than skipping the
    // whole install: the face-gauge layer above is already armed.
    const std::uintptr_t hud_apply_address = ResolveAddress(kHudUiApplyRva);
    if (hud_apply_address == 0 ||
        !ValidateOptionalHookSignature(hud_apply_address, kHudUiApplySignature,
                               "hud_ui_apply")) {
        log::Warn("dynamic_radar: hud_ui_apply unavailable; minimap visible "
                  "global and health bar stay native");
        return;
    }
    if (!CreateHookDetour(hud_apply_address,
                          reinterpret_cast<void*>(&DetourHudUiApply),
                          reinterpret_cast<void**>(&g_hud_ui_apply_original),
                          "hud_ui_apply")) {
        log::Warn("dynamic_radar: hud_ui_apply did not attach; minimap "
                  "visible global and health bar stay native");
        return;
    }
    const std::uintptr_t hb_address = ResolveAddress(kHudHealthBarSetterRva);
    if (hb_address == 0 ||
        !ValidateOptionalHookSignature(hb_address, kHudHealthBarSetterSignature,
                               "hud_health_bar_setter")) {
        g_hud_health_bar_setter = nullptr;
        return;
    }
    g_hud_health_bar_setter =
        reinterpret_cast<HudHealthBarSetterFn>(hb_address);
    log::Info("dynamic_radar health_bar_setter resolved");
}

// Cop-symbol data patch (replaces the removed blip detours, see the
// removal note at the former DetourBlipSymbolApply site). The four cop
// symbol names live as inline ASCII in .rdata and the engine resolves them
// per blip apply; zeroing the first byte of each - after verifying the full
// expected contents - makes lookups miss, and the engine yields no icon: the
// same empty result its own code already handles (all symbol consumers
// ignore the empty result). No detour, no per-frame code, nothing for a
// foreign chain to trip over. Fail-closed: any mismatch applies nothing.
// Restored in Shutdown().
struct CopSymbolSite {
    std::uintptr_t legacy_rva;
    // Latest-Steam layout: .rdata shifts +0x140 in this range; all four
    // strings verified at the new addresses (unique content matches).
    std::uintptr_t latest_rva;
    const char* expected;
};
constexpr CopSymbolSite kCopSymbolSites[] = {
    {0x0017BEB28, 0x0017BEC68, "cop_foot_patrol_symbol"},
    {0x0017BEB40, 0x0017BEC80, "cop_vehicle_patrol_symbol"},
    {0x0017BEB60, 0x0017BECA0, "cop_foot_symbol"},
    {0x0017BEB70, 0x0017BECB0, "cop_symbol"},
};
runtime_patch::Registry g_cop_symbol_patch_registry;

bool ApplyCopSymbolDataPatch() {
    for (const CopSymbolSite& site : kCopSymbolSites) {
        const std::uintptr_t address =
            ResolveAddress(g_use_latest_steam_layout ? site.latest_rva
                                                     : site.legacy_rva);
        const std::size_t length = std::strlen(site.expected) + 1;
        const std::span<const std::uint8_t> expected_full(
            reinterpret_cast<const std::uint8_t*>(site.expected), length);
        if (address == 0 ||
            !runtime_patch::MatchesBytes(address, expected_full)) {
            log::Warn("cop_radar_icons: symbol-data patch refused "
                      "(content mismatch); heat-zero path still hides "
                      "pursuit icons");
            return false;
        }
    }
    const std::size_t checkpoint = g_cop_symbol_patch_registry.checkpoint();
    constexpr std::array<std::uint8_t, 1> kClearedFirstByte{0x00};
    for (const CopSymbolSite& site : kCopSymbolSites) {
        const std::uintptr_t address =
            ResolveAddress(g_use_latest_steam_layout ? site.latest_rva
                                                     : site.legacy_rva);
        const std::array<std::uint8_t, 1> expected_first{
            static_cast<std::uint8_t>(site.expected[0])};
        const runtime_patch::ApplyResult result =
            g_cop_symbol_patch_registry.Apply("cop_symbol_data", address,
                                              expected_first,
                                              kClearedFirstByte);
        if (result != runtime_patch::ApplyResult::Applied &&
            result != runtime_patch::ApplyResult::AlreadyApplied) {
            (void)g_cop_symbol_patch_registry.RestoreTo(checkpoint);
            log::ErrorF("cop_radar_icons: symbol-data patch failed "
                        "result=%s; heat-zero path still hides pursuit icons",
                        runtime_patch::ApplyResultName(result));
            return false;
        }
    }
    log::Info("cop_radar_icons: symbol-data patch applied (4 cop names "
              "unresolvable; no blip detours)");
    return true;
}

bool Initialize(const Config& config,
                const std::filesystem::path& config_path,
                const std::filesystem::path& display_settings_path,
                std::string_view build_id,
                const bool hook_layout_supported) {
    std::lock_guard<std::mutex> lifecycle_lock(g_hook_lifecycle_mutex);
    if (g_cleanup_pending.load(std::memory_order_acquire)) {
        log::Error("hook initialization blocked while a previous teardown is pending");
        return false;
    }
    if (g_hooks_initialized.load(std::memory_order_acquire)) {
        if (g_behavior_transaction_ready.load(std::memory_order_acquire)) {
            return true;
        }
        log::Error("hook initialization blocked: retained detours belong to a closed transaction");
        return false;
    }
    if (g_minhook_retained_process_lifetime.load(std::memory_order_acquire)) {
        log::Error("hook initialization blocked: process-lifetime MinHook state is retained");
        return false;
    }

    g_behavior_transaction_ready.store(false, std::memory_order_release);
    g_config = config;
    g_config_path = config_path;
    g_player_water_collision_component.store(0, std::memory_order_relaxed);
    g_player_water_collision_generation.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> wetness_lock(g_character_wetness_mutex);
        g_character_wetness_state = {};
    }
    {
        std::lock_guard<std::mutex> sweat_lock(g_character_sweat_mutex);
        g_character_sweat_state = {};
    }
    ResetNpcCharacterEffectsStates();
    g_character_physics_get_velocity = nullptr;
    g_ui_is_player_in_combat = nullptr;
    g_is_character_in_vehicle = nullptr;
    g_character_sweat_active_component.store(0, std::memory_order_relaxed);
#if !defined(SPATCH_FINAL_RELEASE)
    const bool character_surface_probe_requested =
        FieldProbeRequested(config_path.parent_path() / L"SPatch.wetness.probe",
                            L"SPATCH_FORCE_WETNESS") ||
        FieldProbeRequested(config_path.parent_path() / L"SPatch.sweat.probe",
                            L"SPATCH_TRACE_SWEAT");
    g_character_surface_probe.store(character_surface_probe_requested,
                                    std::memory_order_relaxed);
    g_character_surface_probe_last_log_tick.store(0, std::memory_order_relaxed);
    g_character_surface_observed_water_generation.store(
        g_player_water_collision_generation.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    if (g_character_surface_probe.load(std::memory_order_relaxed)) {
        log::Warn("observation-only character surface telemetry enabled by an explicit test trigger");
    }
#endif
    // Behaviour fixes own the minimal hook dependencies they need. Keeping
    // these as separate user-editable switches used to let a harmless-looking
    // probe toggle silently disable the shipped cutscene fix (and FinalRelease
    // disabled the same hooks unconditionally).
    if (g_config.fix_cutscene_scene_time_step) {
        g_config.hook_nis_timing = true;
        g_config.hook_nis_owner = true;
    }
    if (g_config.fix_cutscene_zero_dt) {
        g_config.hook_frameflow = true;
    }
    g_use_latest_steam_layout = (build_id == "latest_steam");
    // Trace-file writes are synchronous. Keep them strictly diagnostic so a
    // normal end-user install never opens or flushes a file on the damage thread.
    g_character_trace_budget.store(
        (g_use_latest_steam_layout && config.hook_character_regression_probe) ? 128 : 0);
    g_module_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_module_base == 0) {
        log::Error("hook initialization failed: process image base unavailable");
        return false;
    }
    // ResolveAddress is used throughout the hook transaction, so establish a
    // bounded image end without dereferencing an untrusted e_lfanew/PE header
    // directly.  GetModuleHandle normally returns a valid image, but keeping
    // this path SEH-guarded makes safe-compatibility and malformed-loader
    // cases fail closed instead of crashing the bootstrap worker.
    g_module_end = 0;
    IMAGE_DOS_HEADER dos_header{};
    if (SafeRead(g_module_base, dos_header) && dos_header.e_magic == IMAGE_DOS_SIGNATURE &&
        dos_header.e_lfanew >= static_cast<LONG>(sizeof(IMAGE_DOS_HEADER))) {
        const std::uintptr_t nt_offset = static_cast<std::uintptr_t>(dos_header.e_lfanew);
        constexpr std::uintptr_t max_address =
            (std::numeric_limits<std::uintptr_t>::max)();
        if (nt_offset <= max_address - g_module_base &&
            sizeof(IMAGE_NT_HEADERS64) <=
                max_address - (g_module_base + nt_offset)) {
            IMAGE_NT_HEADERS64 nt_headers{};
            const std::uintptr_t nt_address = g_module_base + nt_offset;
            if (SafeRead(nt_address, nt_headers) &&
                nt_headers.Signature == IMAGE_NT_SIGNATURE &&
                nt_headers.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
                const std::uintptr_t image_size = nt_headers.OptionalHeader.SizeOfImage;
                if (image_size != 0 && g_module_base <= max_address - image_size) {
                    g_module_end = g_module_base + image_size;
                }
            }
        }
    }
    hook_guard::Result hook_guard_result;
    if (!g_hook_target_guard.Initialize(
            reinterpret_cast<void*>(g_module_base), &hook_guard_result)) {
        log::ErrorF("hook target guard initialization failed status=%s pe_status=%s win32=%u",
                    hook_guard::StatusName(hook_guard_result.status),
                    hook_guard::PeStatusName(hook_guard_result.pe_status),
                    hook_guard_result.win32_error);
        return false;
    }
    struct HookTargetGuardReset {
        ~HookTargetGuardReset() { g_hook_target_guard.Reset(); }
    } hook_target_guard_reset;

    if (hook_layout_supported) {
        cut_content::Initialize(g_module_base,
                                g_use_latest_steam_layout,
                                config_path.parent_path() / L"SPatch.cutcontent.probe");
    }
    g_provider_slot = ResolveAddress(kScaleformTimeProviderRva);
    g_render_task_manager = ResolveAddress(kRenderTaskManagerRva);
    g_render_context_instance_slot = ResolveAddress(kRenderContextInstanceRva);
    g_scenery_counter0 = ResolveAddress(kSceneryCounter0Rva);
    g_scenery_counter1 = ResolveAddress(kSceneryCounter1Rva);
    g_scenery_counter2 = ResolveAddress(kSceneryCounter2Rva);
    g_scenery_counter3 = ResolveAddress(kSceneryCounter3Rva);
    g_d3d_device_slot = ResolveAddress(kD3DDeviceSlotRva);
    g_d3d_context_slot = ResolveAddress(kD3DContextSlotRva);
    g_dxgi_swapchain_slot = ResolveAddress(kDxgiSwapChainSlotRva);
    g_present_rtv_slot = ResolveAddress(kPresentRtvSlotRva);
    g_rumble_apply_object = ResolveAddress(kRumbleApplyObjectRva);
    g_wet_surface_block_counter = ResolveAddress(kWetSurfaceBlockCounterRva);
    g_volumetric_fog_intensity = ResolveAddress(fog_restoration::kIntensityRva);
    g_rumble_apply_helper =
        reinterpret_cast<RumbleApplyHelperFn>(ResolveAddress(kRumbleApplyHelperRva));
    // Fingerprint the simtime-pause routine before trusting the two globals it
    // reads. This is a layout check only - the routine itself is never called,
    // because it is not `bool()` (see ReadSimTimePausedSafely). The legacy-only
    // guard matters: these data RVAs are absent from RemapRvaForCurrentBuild,
    // so on the latest layout they would resolve to unrelated memory.
    g_simtime_pause_supported = false;
    if (!g_use_latest_steam_layout) {
        const std::uintptr_t simtime_pause_address =
            ResolveAddress(kSimTimeIsPausedRva);
        if (runtime_patch::MatchesBytes(simtime_pause_address,
                                        kSimTimeIsPausedSignature)) {
            g_simtime_pause_supported = true;
        } else {
            log::Error("simtime pause signature mismatch; simtime pause "
                       "detection disabled (UI pause flag still used)");
        }
    }
    const std::uintptr_t ui_pause_address = ResolveAddress(kUiIsGamePausedRva);
    g_ui_is_game_paused =
        runtime_patch::MatchesBytes(ui_pause_address, kUiIsGamePausedSignature)
            ? reinterpret_cast<UiIsGamePausedFn>(ui_pause_address)
            : nullptr;
    const std::uintptr_t in_vehicle_address =
        ResolveAddress(kIsCharacterInVehicleRva);
    const auto& in_vehicle_signature = g_use_latest_steam_layout
        ? kLatestIsCharacterInVehicleSignature
        : kLegacyIsCharacterInVehicleSignature;
    if (runtime_patch::MatchesBytes(in_vehicle_address, in_vehicle_signature)) {
        g_is_character_in_vehicle =
            reinterpret_cast<IsCharacterInVehicleFn>(in_vehicle_address);
    }
    {
        const std::uintptr_t velocity_address = ResolveAddress(kCharacterPhysicsGetVelocityRva);
        if (runtime_patch::MatchesBytes(velocity_address,
                                       kCharacterPhysicsGetVelocitySignature)) {
            g_character_physics_get_velocity =
                reinterpret_cast<CharacterPhysicsGetVelocityFn>(velocity_address);
        }
        const std::uintptr_t combat_address = ResolveAddress(kUiIsPlayerInCombatRva);
        const auto& combat_signature =
            g_use_latest_steam_layout ? kLatestSteamUiIsPlayerInCombatSignature
                                      : kUiIsPlayerInCombatSignature;
        if (runtime_patch::MatchesBytes(combat_address, combat_signature)) {
            g_ui_is_player_in_combat =
                reinterpret_cast<UiIsPlayerInCombatFn>(combat_address);
        }
    }
    g_simobject_get_component =
        reinterpret_cast<SimObjectGetComponentFn>(ResolveAddress(kSimObjectGetComponentRva));
    g_time_of_day_accessor =
        reinterpret_cast<TimeOfDayAccessorFn>(ResolveAddress(kTimeOfDayAccessorRva));
    g_qfile_ready = reinterpret_cast<qfile_io::QFileReadyFn>(ResolveAddress(kQFileReadyRva));
    ResetArchiveEntryGuard();
    g_sampler_builder_first_verified_invocation_logged.store(
        false, std::memory_order_relaxed);
    g_sampler_builder_failure_logged.store(false, std::memory_order_relaxed);
    g_anisotropy_writer_first_verified_invocation_logged.store(
        false, std::memory_order_relaxed);
    g_chase_camera_set_parameters_original = nullptr;
    g_chase_camera_update_original = nullptr;
    g_angular_approach_original = nullptr;
    g_game_camera_set_desired_eye_look_up_original = nullptr;
    g_chase_camera_update_probe_depth = 0;
    g_chase_camera_update_probe_stack = {};
    ResetGtaIvVehicleCameraDynamicState();
    ResetGtaVSpeedShakeState();
    g_gtaiv_camera_probe_sample_count.store(0,
                                                 std::memory_order_relaxed);
    g_gtaiv_camera_probe_log_budget.store(
        vehicle_camera::NeedsVehicleCameraCoreHooks(
            vehicle_camera::CameraFeatureRequests{
                config.gta_iv_car_camera,
                config.gta_iv_bike_camera,
                config.gtav_speed_shake,
            }) &&
                config.enable_logging
            ? 32u
            : 0u,
        std::memory_order_relaxed);
    g_gtaiv_camera_probe_next_sample_tick.store(0,
                                                std::memory_order_relaxed);
    g_gtaiv_camera_dynamic_probe_sample_count.store(
        0, std::memory_order_relaxed);
    g_gtaiv_camera_dynamic_probe_log_budget.store(
        vehicle_camera::NeedsVehicleCameraCoreHooks(
            vehicle_camera::CameraFeatureRequests{
                config.gta_iv_car_camera,
                config.gta_iv_bike_camera,
                config.gtav_speed_shake,
            }) &&
                config.enable_logging
            ? 2400u
            : 0u,
        std::memory_order_relaxed);
    g_gtaiv_camera_dynamic_probe_next_sample_tick.store(
        0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_gtaiv_camera_probe_state_mutex);
        g_gtaiv_camera_probe_last_state =
            (std::numeric_limits<unsigned long long>::max)();
        g_gtaiv_camera_probe_last_subject =
            (std::numeric_limits<std::uintptr_t>::max)();
    }
    g_qcmp_rejection_logged.store(false, std::memory_order_relaxed);
    g_resource_chunk_rejection_logged.store(false, std::memory_order_relaxed);
    // Heat-controller identity must re-latch on the first instance of the new
    // session, never inherit the previous one.
    g_primary_heat_sub.store(0, std::memory_order_relaxed);
    g_rumble_override_attempted = false;
    g_last_rumble_override_value.store(-1);
    g_aa_runtime_variant_mode.store(ClampAaVariantMode(config.aa_variant_mode));
    g_aa_runtime_aux_mode.store(ClampAaAuxMode(config.aa_aux_mode));

    g_hook_creation_transaction_open = true;
    g_created_hook_targets.clear();
    // Allocate the complete process-lifetime hook registry before SMAA or any
    // other hook can mutate executable code. A low-memory failure remains a
    // clean, transparent initialization failure.
    try {
        g_created_hook_targets.reserve(128);
    } catch (...) {
        g_hook_creation_transaction_open = false;
        log::Error("hook initialization failed: hook registry allocation failed");
        return false;
    }

    bool smaa_initialized = true;
    smaa::SetEnabled(config.hook_smaa_present && config.smaa_enable);
    // R6: the frame limiter rides the present hook. It paces even when the
    // SMAA replacement itself is off, as long as the present detour exists.
    if (config.hook_smaa_present) {
        smaa::SetFrameLimiter(config.custom_frame_limiter,
                              config.custom_frame_limiter_fps);
    }
    smaa::SetDebugKeysEnabled(config.hook_smaa_present && config.smaa_debug_keys);
    if (config.hook_smaa_present) {
        smaa::SetPreset(config.smaa_preset);
        // These two setters had no caller anywhere in the tree, so
        // SMAAEdgeThreshold and IconStabilizer were parsed, written back to the
        // INI and documented as shipped while doing nothing. The renderer
        // already honours both (g_edge_threshold / g_icon_stabilizer).
        smaa::SetEdgeThreshold(config.smaa_edge_threshold);
        smaa::SetIconStabilizer(config.icon_stabilizer);
        smaa_initialized =
            smaa::Initialize(g_module_base,
                             g_d3d_device_slot,
                             g_d3d_context_slot,
                             g_dxgi_swapchain_slot,
                             g_present_rtv_slot);
    }
    // AgX shares the engine device slot resolved above. Substitution is a
    // strict subset of SMAA ownership: AgX never installs when disabled,
    // so publishing the slot unconditionally is fail-open. `Configure` is the
    // ONLY writer of g_enabled, which MaybeInstallHooks() gates on - it had no
    // caller anywhere in the tree, so the whole [Tonemapping] surface
    // (including AgxDebugBloom) was parsed and documented while doing nothing.
    agx::Configure(config.agx_enabled,
                   config.agx_strength,
                   config.agx_exposure,
                   config.agx_look,
                   config.agx_debug_bloom);
    agx::SetDeviceSlot(g_d3d_device_slot);
    StoreSceneryCounters(ReadSceneryCounters());
#if !defined(SPATCH_FINAL_RELEASE)
    g_last_summary_tick.store(GetTickCount64());
#endif
    ResetNisActorTrackerLocked();

    bool ok = !config.hook_smaa_present || smaa_initialized;
    if ((config.fix_cutscene_zero_dt || config.fix_cutscene_scene_time_step) &&
        g_ui_is_game_paused == nullptr) {
        log::Error("cutscene pause function signature mismatch");
        ok = false;
    }
    const bool need_nis_actor_state_hooks =
        config.hook_nis_actor_state || config.fix_nis_actor_restore_duplicates;
#if !defined(SPATCH_FINAL_RELEASE)
    const bool need_character_surface_probe =
        g_character_surface_probe.load(std::memory_order_relaxed);
#else
    constexpr bool need_character_surface_probe = false;
#endif
    const character_hooks::HookPlan character_hook_plan = character_hooks::BuildHookPlan({
        .restore_wetness = config.restore_character_wetness,
        .restore_sweat = config.restore_character_sweat,
        // Without them the detour installs only when sweat is on, and every
        // restored voice line is dead code even though its INI key is set.
        .restore_npc_voice_lines = config.restore_police_voice_lines ||
                                   config.restore_encourage_voice_lines ||
                                   config.diag_npc_voice_line_events,
        .restore_push_voice_lines = config.restore_push_voice_lines ||
                                    config.diag_push_voice_line_events,
        .restore_combat_lines = config.restore_combat_lines ||
                                config.diag_combat_line_events,
        .regression_probe = config.hook_character_regression_probe,
        .wetness_field_probe = need_character_surface_probe,
        .sweat_field_probe = need_character_surface_probe,
        .slow_mo_probe = config.probe_slow_mo,
        .slow_mo_suppress = config.disable_driveby_slow_mo ||
                            config.disable_stunt_slow_mo,
    });
    const bool need_character_regression_probe =
        character_hook_plan.install_regression_hooks;
    const bool need_character_effects_hooks = character_hook_plan.install_effects_hooks;
    const bool need_character_damage_hooks =
        character_hook_plan.require_simobject_component;
    g_sweat_only_health_fast_path =
        character_hook_plan.use_sweat_only_health_fast_path;

    if (config.hook_nis_actor_state && g_use_latest_steam_layout) {
        log::Warn(
            "nis_actor_state probe requested on latest_steam, but only legacy_researched is mapped; probe disabled");
    }
    if (config.fix_nis_actor_restore_duplicates && g_use_latest_steam_layout) {
        log::Warn(
            "fix_nis_actor_restore_duplicates requested on latest_steam, but only legacy_researched is mapped; fix disabled");
    }
    if (config.hook_twitch_probe && g_use_latest_steam_layout) {
        log::Warn(
            "twitch_probe requested on latest_steam, but only legacy_researched is mapped; probe disabled");
    }
    if (config.stimulus_init_emitter_probe && g_use_latest_steam_layout) {
        log::Warn(
            "stimulus_init_emitter_probe requested on latest_steam, but only legacy_researched is mapped; probe disabled");
    }
    if (config.override_rumble_enabled >= 0 &&
        (g_rumble_apply_helper == nullptr || g_rumble_apply_object == 0)) {
        log::Error("rumble override resolution failed");
        ok = false;
    }
    if (need_character_damage_hooks && g_simobject_get_component == nullptr) {
        log::Error("simobject_get_component resolution failed");
        ok = false;
    }
    if (config.restore_character_sweat && g_character_physics_get_velocity == nullptr &&
        g_ui_is_player_in_combat == nullptr) {
        log::Warn(
            "character sweat restoration disabled: exertion inputs are not mapped for this executable");
    } else if (config.restore_character_sweat &&
               g_character_physics_get_velocity != nullptr &&
               g_is_character_in_vehicle == nullptr) {
        log::Warn(
            "character sweat running input disabled: vehicle occupancy predicate failed verification");
    }
    if (config.hook_smaa_present &&
        (g_d3d_device_slot == 0 || g_d3d_context_slot == 0 || g_dxgi_swapchain_slot == 0 ||
         g_present_rtv_slot == 0)) {
        log::Error("smaa slot resolution failed");
        ok = false;
    }
    if (config.restore_original_fog &&
        (g_volumetric_fog_intensity < g_module_base ||
         g_module_end < g_volumetric_fog_intensity + sizeof(float))) {
        log::Error("original atmosphere intensity resolution failed");
        ok = false;
    }
    if (!ok) {
        g_hook_creation_transaction_open = false;
    }
    g_pedestrian_throttle_frame_count.store(0, std::memory_order_relaxed);
    g_pedestrian_throttle_stock_call_count.store(0, std::memory_order_relaxed);
    g_pedestrian_throttle_clamped_frame_count.store(0, std::memory_order_relaxed);
    g_average_window_initialize_count.store(0, std::memory_order_relaxed);
    g_average_window_expanded_count.store(0, std::memory_order_relaxed);
    g_average_window_fallback_logged.store(false, std::memory_order_relaxed);
    g_fog_clamp_warning_emitted.store(false, std::memory_order_relaxed);
    g_original_fog_applied.store(false, std::memory_order_relaxed);
    g_original_fog_frame_initialized.store(false, std::memory_order_relaxed);
    g_original_fog_failure_logged.store(false, std::memory_order_relaxed);
    g_original_fog_setter_logged.store(false, std::memory_order_relaxed);
    g_character_eye_restore_applied_mask.store(0, std::memory_order_relaxed);
    g_character_eye_restore_already_present_mask.store(0, std::memory_order_relaxed);
    g_character_eye_restore_failure_mask.store(0, std::memory_order_relaxed);
    log::InfoF("engine_fixes pedestrian_density=%d spherical_reflections=%d "
               "original_fog_and_neon=%d original_eye_reflections=%d remove_120_fps_cap=%d "
               "resource_loading=%d corrupt_save_guard=%d thread_failure_guard=%d",
               config.fix_pedestrian_density_at_high_fps ? 1 : 0,
               config.improve_spherical_reflections ? 1 : 0,
               config.restore_original_fog ? 1 : 0,
               config.restore_original_eye_reflections ? 1 : 0,
               config.remove_hidden_120_fps_cap ? 1 : 0,
               config.fix_resource_loading ? 1 : 0,
               config.fix_corrupt_save_handling ? 1 : 0,
               config.fix_thread_creation_failure ? 1 : 0);

    const bool anisotropy_override_requested =
        texture_filtering::ShouldInstallWriter(
            config.anisotropic_filtering);
    const texture_filtering::AddressProfile texture_filtering_addresses =
        texture_filtering::SelectAddresses(g_use_latest_steam_layout);
    const std::uintptr_t sampler_builder_address =
        anisotropy_override_requested && hook_layout_supported
            ? ResolveAddress(
                  texture_filtering_addresses.sampler_builder_rva)
            : 0;
    if (anisotropy_override_requested) {
        const std::uintptr_t writer_address =
            hook_layout_supported
                ? ResolveAddress(
                      texture_filtering_addresses.anisotropy_writer_rva)
                : 0;
        bool installed = false;
        if (writer_address == 0) {
            log::Warn(
                "texture_filtering anisotropy_writer disabled=1 "
                "reason=unsupported_hook_layout");
        } else if (ValidateHookSignature(
                       writer_address,
                       texture_filtering::kAnisotropyWriterSignature,
                       "anisotropy_writer")) {
            installed = CreateHookDetour(
                writer_address,
                reinterpret_cast<void*>(&DetourAnisotropyWriter),
                reinterpret_cast<void**>(&g_anisotropy_writer_original),
                "anisotropy_writer");
        }
        ok &= installed;
        log::InfoF(
            "texture_filtering anisotropy_writer requested=%d installed=%d "
            "layout=%s target=0x%p behavior=pending_commit",
            config.anisotropic_filtering,
            installed ? 1 : 0,
            g_use_latest_steam_layout ? "latest_steam" : "legacy_researched",
            reinterpret_cast<void*>(writer_address));
    }

    if (config.fix_resource_loading) {
        const std::uintptr_t pc_file_read_address = ResolveAddress(kPcFileReadRva);
        const std::uintptr_t pc_file_seek_address = ResolveAddress(kPcFileSeekRva);
        const std::uintptr_t pc_file_tell_address = ResolveAddress(kPcFileTellRva);
        const std::uintptr_t pc_file_size_address = ResolveAddress(kPcFileSizeRva);
        const std::uintptr_t qfile_read_at_address = ResolveAddress(kQFileReadAtRva);
        const std::uintptr_t qfile_write_at_address = ResolveAddress(kQFileWriteAtRva);
        const std::uintptr_t qfile_ready_address = ResolveAddress(kQFileReadyRva);
        const std::uintptr_t qcmp_decompress_address = ResolveAddress(kQcmpDecompressRva);
        const std::uintptr_t stream_file_open_address = ResolveAddress(kStreamFileOpenRva);
        const std::uintptr_t stream_file_close_address = ResolveAddress(kStreamFileCloseRva);
        const std::uintptr_t resource_chunk_dispatch_address =
            ResolveAddress(kResourceChunkDispatchRva);
        const bool qfile_targets_valid =
        ValidateHookSignature(pc_file_read_address, kPcFileReadSignature, "pc_file_read") &&
        ValidateHookSignature(pc_file_seek_address, kPcFileSeekSignature, "pc_file_seek") &&
        ValidateHookSignature(pc_file_tell_address, kPcFileTellAndSizeSignature, "pc_file_tell") &&
        ValidateHookSignature(pc_file_size_address, kPcFileTellAndSizeSignature, "pc_file_size") &&
        ValidateHookSignature(qfile_read_at_address, kQFileReadAtSignature, "qfile_read_at") &&
        ValidateHookSignature(qfile_write_at_address, kQFileWriteAtSignature, "qfile_write_at") &&
        ValidateHookSignature(qfile_ready_address, kQFileReadySignature, "qfile_ready") &&
        ValidateHookSignature(qcmp_decompress_address, kQcmpDecompressSignature,
                              "qcmp_decompress") &&
        ValidateHookSignature(stream_file_open_address, kStreamFileOpenSignature,
                              "stream_file_open") &&
        ValidateHookSignature(stream_file_close_address, kStreamFileCloseSignature,
                              "stream_file_close") &&
        ValidateHookSignature(resource_chunk_dispatch_address,
                              kResourceChunkDispatchSignature,
                              "resource_chunk_dispatch");
        if (qfile_targets_valid) {
            g_stream_file_close = reinterpret_cast<StreamFileCloseFn>(stream_file_close_address);
            const bool read_installed =
            CreateHookDetour(pc_file_read_address, reinterpret_cast<void*>(&DetourPcFileRead),
                             reinterpret_cast<void**>(&g_pc_file_read_original), "pc_file_read");
            const bool seek_installed =
            CreateHookDetour(pc_file_seek_address, reinterpret_cast<void*>(&DetourPcFileSeek),
                             reinterpret_cast<void**>(&g_pc_file_seek_original), "pc_file_seek");
            const bool tell_installed =
            CreateHookDetour(pc_file_tell_address, reinterpret_cast<void*>(&DetourPcFileTell),
                             reinterpret_cast<void**>(&g_pc_file_tell_original), "pc_file_tell");
            const bool size_installed =
            CreateHookDetour(pc_file_size_address, reinterpret_cast<void*>(&DetourPcFileSize),
                             reinterpret_cast<void**>(&g_pc_file_size_original), "pc_file_size");
            const bool read_at_installed =
            CreateHookDetour(qfile_read_at_address, reinterpret_cast<void*>(&DetourQFileReadAt),
                             reinterpret_cast<void**>(&g_qfile_read_at_original), "qfile_read_at");
            const bool write_at_installed = CreateHookDetour(
            qfile_write_at_address, reinterpret_cast<void*>(&DetourQFileWriteAt),
            reinterpret_cast<void**>(&g_qfile_write_at_original), "qfile_write_at");
            const bool qcmp_guard_installed = CreateHookDetour(
            qcmp_decompress_address, reinterpret_cast<void*>(&DetourQcmpDecompress),
            reinterpret_cast<void**>(&g_qcmp_decompress_original), "qcmp_decompress");
            const bool archive_guard_installed = CreateHookDetour(
            stream_file_open_address, reinterpret_cast<void*>(&DetourStreamFileOpen),
            reinterpret_cast<void**>(&g_stream_file_open_original), "stream_file_open");
            const bool resource_chunk_guard_installed = CreateHookDetour(
            resource_chunk_dispatch_address,
            reinterpret_cast<void*>(&DetourResourceChunkDispatch),
            reinterpret_cast<void**>(&g_resource_chunk_dispatch_original),
            "resource_chunk_dispatch");
            const bool qfile_io_installed =
                read_installed && seek_installed && tell_installed && size_installed &&
                read_at_installed && write_at_installed && qcmp_guard_installed &&
                archive_guard_installed && resource_chunk_guard_installed;
            ok &= qfile_io_installed;
            if (qfile_io_installed) {
                log::Info(
                    "engine_fix requested=1 hooks_installed=1 behavior=pending_commit "
                    "qfile_io_error_propagation=1 archive_entry_bounds=1 qcmp_bounds=1 "
                    "resource_chunk_bounds=1 big_file_index_bounds=1");
            }
        } else {
            ok = false;
        }
    }

    if (config.fix_high_fps_average_windows) {
        const std::uintptr_t address = ResolveAddress(kAverageWindowInitializeRva);
        if (ValidateHookSignature(address, kAverageWindowInitializeSignature,
                                  "average_window_initialize")) {
            const bool installed =
                CreateHookDetour(address, reinterpret_cast<void*>(&DetourAverageWindowInitialize),
                                 reinterpret_cast<void**>(&g_average_window_initialize_original),
                                 "average_window_initialize");
            ok &= installed;
            if (installed) {
                log::InfoF("average_window_fix requested=1 hooks_installed=1 "
                           "behavior=pending_commit sample_rate=%.0f max_entries=%zu",
                           average_window::kExpandedSampleRate,
                           average_window::kMaximumEntries);
            }
        }
    }

    // The pedestrian-spawn seam carries only FixPedestrianDensity now: the
    // GTA IV density curve was removed outright (its write lived here), so
    // the install gates on the fix alone.
    const bool want_density_fix = config.fix_pedestrian_density_at_high_fps;
    if (want_density_fix) {
        const std::uintptr_t update_address = ResolveAddress(kPedestrianSpawnUpdateRva);
        // Each hook validates on its own signature. The previous block required
        // both to match before installing either, which meant a throttle
        // signature change silently took the curve's seam down with it.
        if (ValidateHookSignature(update_address,
                                  kPedestrianSpawnUpdateSignature,
                                  "pedestrian_spawn_update")) {
            const bool update_installed = CreateHookDetour(
                update_address,
                reinterpret_cast<void*>(&DetourPedestrianSpawnUpdate),
                reinterpret_cast<void**>(&g_pedestrian_spawn_update_original),
                "pedestrian_spawn_update");
            ok &= update_installed;
        }

        // The throttle retimes the stock pedestrian controller at its original
        // 30 Hz cadence.
        if (want_density_fix) {
            const std::uintptr_t throttle_address =
                ResolveAddress(kPedestrianFrameRateThrottleRva);
            const auto& throttle_signature = g_use_latest_steam_layout
                                                 ? kLatestPedestrianThrottleSignature
                                                 : kLegacyPedestrianThrottleSignature;
            if (ValidateHookSignature(throttle_address,
                                      throttle_signature,
                                      "pedestrian_frame_rate_throttle")) {
                const bool throttle_installed = CreateHookDetour(
                    throttle_address,
                    reinterpret_cast<void*>(&DetourPedestrianFrameRateThrottle),
                    reinterpret_cast<void**>(&g_pedestrian_frame_rate_throttle_original),
                    "pedestrian_frame_rate_throttle");
                ok &= throttle_installed;
                if (throttle_installed) {
                    log::InfoF(
                        "pedestrian_density_fix requested=1 hooks_installed=1 "
                        "behavior=pending_commit cadence_hz=%.1f max_steps_per_frame=%u",
                        pedestrian_timing::kStockThrottleHz,
                        pedestrian_timing::kMaximumStepsPerFrame);
                }
            }
        }
    }


    // Vehicle light shadows, the beam light and the light-submit diagnostic
    // share one detour (round75/77). The beam needs it because the stock
    // selector's shadow decision is a GLOBAL query (FUN_14000b140 /
    // FUN_14069f820 over DAT_14243A300): when that query declines, even a
    // shadow-eligible type-1 light is handed 0x36ACE9F8. The detour forces the
    // shadow technique on the beam record only, by pointer identity.
    // Legacy researched layout only: the selector RVA is not mapped for the
    // later Steam profile, so those keys stay inert there instead of guessing.
    if (config.headlight_shadows || config.siren_shadows ||
        config.headlight_beam_light || config.diag_light_submit_events) {
        if (g_use_latest_steam_layout) {
            log::InfoF(
                "vehicle_light_shadows unavailable reason=unmapped_build_profile");
        } else {
            const std::uintptr_t selector_address =
                ResolveAddress(vehicle_light_shadow::kLightSubmitSelectorRva);
            if (ValidateHookSignature(
                    selector_address,
                    vehicle_light_shadow::kLightSubmitSelectorSignature,
                    "light_submit_selector")) {
                const bool selector_installed = CreateHookDetour(
                    selector_address,
                    reinterpret_cast<void*>(&DetourLightSubmitSelector),
                    reinterpret_cast<void**>(
                        &g_light_submit_selector_original),
                    "light_submit_selector");
                ok &= selector_installed;
                if (selector_installed) {
                    log::InfoF(
                        "vehicle_light_shadows selector_hook_installed=1 "
                        "headlight=%u siren=%u headlight_type=%d "
                        "siren_type=%d diag=%u",
                        config.headlight_shadows ? 1u : 0u,
                        config.siren_shadows ? 1u : 0u,
                        config.headlight_shadow_light_type,
                        config.siren_shadow_light_type,
                        config.diag_light_submit_events ? 1u : 0u);
                }
            }
        }
    }

    // Vehicle headlight beam light (round78). Legacy researched layout only.
    // The light is a real engine component spliced into the deferred light
    // list, so it is picked up by the per-frame record table builder - which
    // is also where it is updated, because that walk runs before the
    // collection job is queued and therefore before the shadow pass.
    if (config.headlight_beam_light) {
        if (g_use_latest_steam_layout) {
            log::InfoF(
                "vehicle_beam_light unavailable reason=unmapped_build_profile");
        } else {
            const std::uintptr_t builder_address =
                ResolveAddress(vehicle_beam_light::kRecordTableBuilderRva);
            if (ValidateHookSignature(
                    builder_address,
                    vehicle_beam_light::kRecordTableBuilderSignature,
                    "record_table_builder")) {
                const bool builder_installed = CreateHookDetour(
                    builder_address,
                    reinterpret_cast<void*>(&DetourRecordTableBuilder),
                    reinterpret_cast<void**>(
                        &g_record_table_builder_original),
                    "record_table_builder");
                ok &= builder_installed;
                if (builder_installed) {
                    log::InfoF(
                        "vehicle_beam_light component_hook_installed=1 "
                        "radius=%.1f",
                        config.headlight_beam_radius);
                }
            }
            // Placement needs the camera the collector gates against, so the
            // collector is hooked too. Without it the beam keeps whatever
            // position and axis the template lamp had, which the frustum gate
            // rejects.
            const std::uintptr_t collector_address =
                ResolveAddress(vehicle_beam_light::kCollectorRva);
            if (ValidateHookSignature(collector_address,
                                      vehicle_beam_light::kCollectorSignature,
                                      "light_collector")) {
                const bool collector_installed = CreateHookDetour(
                    collector_address,
                    reinterpret_cast<void*>(&DetourLightCollector),
                    reinterpret_cast<void**>(&g_light_collector_original),
                    "light_collector");
                ok &= collector_installed;
                if (collector_installed) {
                    log::InfoF(
                        "vehicle_beam_light collector_hook_installed=1 "
                        "ahead=%.1f",
                        vehicle_beam_light::kBeamAheadMeters);
                }
            }
        }
    }

    // Every voice feature shares this one speech chain. Gating it on the push
    // keys alone leaves g_speak_by_name_original null whenever the push lines
    // are off, and InvokeVoiceLineAction then early-returns for all of them.
    if (config.restore_push_voice_lines || config.diag_push_voice_line_events ||
        config.restore_combat_lines || config.diag_combat_line_events ||
        config.restore_police_voice_lines || config.restore_encourage_voice_lines ||
        config.diag_npc_voice_line_events || config.driving_swear_lines ||
        config.cop_pursuit_swear_lines) {
        {
            // Legacy and latest-Steam layouts are both mapped now
            // (latest remap: site-evidence-wave1.md); ResolveAddress
            // selects the right site per build.
            const std::uintptr_t dispatcher_address =
                ResolveAddress(kConversationDispatcherRva);
            if (ValidateHookSignature(dispatcher_address,
                                      kConversationDispatcherSignature,
                                      "conversation_dispatcher")) {
                const bool dispatcher_installed = CreateHookDetour(
                    dispatcher_address,
                    reinterpret_cast<void*>(&DetourConversationDispatcher),
                    reinterpret_cast<void**>(&g_conversation_dispatcher_original),
                    "conversation_dispatcher");
                ok &= dispatcher_installed;
                if (dispatcher_installed) {
                    g_audio_name_to_id = reinterpret_cast<AudioNameToIdFn>(
                        ResolveAddress(kAudioNameToIdRva));
                    // Every arm the voice features gate on is reported here.
                    // Without these flags the log cannot show whether combat,
                    // police or encourage lines were actually armed.
                    //
                    // driving= / cop_pursuit= report the CONFIG keys only. Their
                    // bark triggers (MaybeFireDrivingSwearLine) take a
                    // ChaseCameraUpdateProbeScope belonging to the chase-camera
                    // work scrapped at config v62, so they are wired to the speech
                    // chain but nothing ever fires them.
                    //
                    // RE-VERIFIED 2026-10-02: the line above is ACCURATE and was
                    // briefly edited into a false claim that the function was
                    // absent from every recovery snapshot. Measured, it is the
                    // opposite: `MaybeFireDrivingSwearLine` appears 3x in
                    // the recovered Hooks.cpp - declared at 5165, called at 5280, defined
                    // at 14651 - and its gate is `g_config.driving_swear_lines`.
                    // It is absent from THIS file, so the trigger is recoverable
                    // rather than merely dormant. *_trigger=0 states that
                    // rather than letting driving=1 read as "will speak".
                    log::InfoF("push_voice_lines dispatcher_hook_installed=1 push_fix=%u "
                               "push_diag=%u combat=%u police=%u encourage=%u "
                               "npc_diag=%u driving=%u cop_pursuit=%u "
                               "driving_trigger=0 cop_pursuit_trigger=0 "
                               "name_resolver=%s",
                               config.restore_push_voice_lines ? 1u : 0u,
                               config.diag_push_voice_line_events ? 1u : 0u,
                               config.restore_combat_lines ? 1u : 0u,
                               config.restore_police_voice_lines ? 1u : 0u,
                               config.restore_encourage_voice_lines ? 1u : 0u,
                               config.diag_npc_voice_line_events ? 1u : 0u,
                               config.driving_swear_lines ? 1u : 0u,
                               config.cop_pursuit_swear_lines ? 1u : 0u,
                               g_audio_name_to_id != nullptr ? "resolved" : "null");
                }
            }
            // ScriptedConversationElement entry twin: the conversation system
            // drives this virtual, so a base-dispatcher-only detector misses
            // scripted push elements entirely (round68 ledger section 27).
            const std::uintptr_t scripted_entry_address =
                ResolveAddress(kScriptedConversationEntryRva);
            if (ValidateHookSignature(scripted_entry_address,
                                      kScriptedConversationEntrySignature,
                                      "scripted_conversation_entry")) {
                const bool scripted_entry_installed = CreateHookDetour(
                    scripted_entry_address,
                    reinterpret_cast<void*>(&DetourScriptedConversationEntry),
                    reinterpret_cast<void**>(&g_scripted_conversation_entry_original),
                    "scripted_conversation_entry");
                ok &= scripted_entry_installed;
            }

            // Speech-by-name entry: the route the fix actually calls. Installed
            // with the same signature validation and transaction gating as the
            // other conversation hooks (ledger sec 36).
            const std::uintptr_t speak_by_name_address = ResolveAddress(kSpeakByNameRva);
            if (ValidateHookSignature(speak_by_name_address,
                                      kSpeakByNameSignature,
                                      "speak_by_name")) {
                const bool speak_by_name_installed = CreateHookDetour(
                    speak_by_name_address,
                    reinterpret_cast<void*>(&DetourSpeakByName),
                    reinterpret_cast<void**>(&g_speak_by_name_original),
                    "speak_by_name");
                ok &= speak_by_name_installed;
            }

            // The fix path invokes the wrapper through its detour trampoline,
            // so the wrapper hook is required for either switch.
            const std::uintptr_t wrapper_address = ResolveAddress(kSpeechWrapperRva);
            if (ValidateHookSignature(wrapper_address,
                                      kSpeechWrapperSignature,
                                      "speech_wrapper")) {
                const bool wrapper_installed = CreateHookDetour(
                    wrapper_address,
                    reinterpret_cast<void*>(&DetourSpeechWrapper),
                    reinterpret_cast<void**>(&g_speech_wrapper_original),
                    "speech_wrapper");
                ok &= wrapper_installed;
            }
        }
    }

    if (config.hook_queue_ready) {
        ok &= CreateHookDetour(ResolveAddress(kTaskReadyRva),
                               reinterpret_cast<void*>(&DetourTaskReady),
                               reinterpret_cast<void**>(&g_task_ready_original), "task_ready");
    }

    if (config.hook_task_dispatch) {
        ok &= CreateHookDetour(
            ResolveAddress(kTaskDispatchRva), reinterpret_cast<void*>(&DetourTaskDispatch),
            reinterpret_cast<void**>(&g_task_dispatch_original), "task_dispatch");
    }

    if (config.hook_wait_helper) {
        ok &= CreateHookDetour(ResolveAddress(kWaitHelperRva),
                               reinterpret_cast<void*>(&DetourWaitHelper),
                               reinterpret_cast<void**>(&g_wait_helper_original), "wait_helper");
    }

    if (config.hook_scaleform_time) {
        ok &= CreateHookDetour(
            ResolveAddress(kScaleformTimeRva), reinterpret_cast<void*>(&DetourScaleformTime),
            reinterpret_cast<void**>(&g_scaleform_time_original), "scaleform_time");
    }

    if (config.hook_scaleform_init) {
        ok &= CreateHookDetour(
            ResolveAddress(kScaleformInitRva), reinterpret_cast<void*>(&DetourScaleformInit),
            reinterpret_cast<void**>(&g_scaleform_init_original), "scaleform_init");
    }

    const bool need_nis_timing_hook = config.hook_nis_timing || config.fix_cutscene_scene_time_step;
    const bool need_nis_owner_hook = config.hook_nis_owner || config.fix_cutscene_scene_time_step;
    // Scene-time repair uses the owner cadence tracker to distinguish a brief
    // legacy 30/60-Hz leak from a genuine sustained slowdown. It therefore
    // owns CutsceneFlowOwner even when zero-dt/frameflow repair is disabled.
    const bool need_cutscene_frameflow_hook = config.hook_frameflow ||
                                              config.fix_cutscene_zero_dt ||
                                              config.fix_cutscene_scene_time_step;

    if (need_nis_timing_hook) {
        ok &= CreateHookDetour(
            ResolveAddress(kNisSetPlayTimeRva), reinterpret_cast<void*>(&DetourNisSetPlayTime),
            reinterpret_cast<void**>(&g_nis_set_play_time_original), "nis_set_play_time");
    }

    if (config.hook_nis_runtime) {
        ok &= CreateHookDetour(ResolveAddress(kNisPlayRva), reinterpret_cast<void*>(&DetourNisPlay),
                               reinterpret_cast<void**>(&g_nis_play_original), "nis_play");
        ok &= CreateHookDetour(
            ResolveAddress(kNisBootstrapRva), reinterpret_cast<void*>(&DetourNisBootstrap),
            reinterpret_cast<void**>(&g_nis_bootstrap_original), "nis_bootstrap");
    }

    if (need_nis_owner_hook) {
        ok &=
            CreateHookDetour(ResolveAddress(kNisOwnerRva), reinterpret_cast<void*>(&DetourNisOwner),
                             reinterpret_cast<void**>(&g_nis_owner_original), "nis_owner");
    }
    if (need_nis_actor_state_hooks && !g_use_latest_steam_layout) {
        ok &= CreateHookDetour(
            ResolveAddress(kNisActorSetupRva), reinterpret_cast<void*>(&DetourNisActorSetup),
            reinterpret_cast<void**>(&g_nis_actor_setup_original), "nis_actor_setup");
        ok &= CreateHookDetour(
            ResolveAddress(kNisActorRestoreRva), reinterpret_cast<void*>(&DetourNisActorRestore),
            reinterpret_cast<void**>(&g_nis_actor_restore_original), "nis_actor_restore");
    }
    if (config.hook_twitch_probe && !g_use_latest_steam_layout) {
        ok &= CreateHookDetour(
            ResolveAddress(kTwitchStateTickRva), reinterpret_cast<void*>(&DetourTwitchStateTick),
            reinterpret_cast<void**>(&g_twitch_state_tick_original), "twitch_tick");
        ok &= CreateHookDetour(ResolveAddress(kTwitchLoginCallbackRva),
                               reinterpret_cast<void*>(&DetourTwitchLoginCallback),
                               reinterpret_cast<void**>(&g_twitch_login_callback_original),
                               "twitch_login_callback");
    }

    if (config.stimulus_init_emitter_probe && !g_use_latest_steam_layout) {
        // Legacy_researched only: 0x366490 is not mapped for the later Steam
        // layout, and the address profile carries no counterpart.
        const std::uintptr_t stimulus_init_address =
            ResolveAddress(kStimulusInitRva);
        if (ValidateHookSignature(stimulus_init_address,
                                  kStimulusInitSignature,
                                  "stimulus_init")) {
            g_stimulus_init_probe_budget.store(
                stimulus::kInitEmitterProbeBudget);
            ok &= CreateHookDetour(
                stimulus_init_address,
                reinterpret_cast<void*>(&DetourStimulusInit),
                reinterpret_cast<void**>(&g_stimulus_init_original),
                "stimulus_init");
        } else {
            // A signature mismatch means the RVA points at something else on
            // this build. Installing anyway would detour a stranger, so the
            // feature reports itself unavailable instead.
            log::Warn("stimulus_init unavailable reason=signature_mismatch");
        }
    }

    if (config.hook_pass8_probe && !g_use_latest_steam_layout) {
        bool wndproc_installed = false;
        bool os_game_url_installed = false;
        bool activity_timer_installed = false;
        const std::uintptr_t wndproc_address = ResolveAddress(kPass8WndProcRva);
        if (ValidateHookSignature(wndproc_address, kPass8WndProcSignature, "pass8_wndproc")) {
            wndproc_installed = CreateHookDetour(
                wndproc_address,
                reinterpret_cast<void*>(&DetourPass8WndProc),
                reinterpret_cast<void**>(&g_pass8_wndproc_original),
                "pass8_wndproc");
        }
        const std::uintptr_t os_game_url_address =
            ResolveAddress(kPass8OsGameUrlProbeRva);
        if (ValidateHookSignature(
                os_game_url_address, kPass8OsGameUrlProbeSignature, "pass8_osgameurl")) {
            os_game_url_installed = CreateHookDetour(
                os_game_url_address,
                reinterpret_cast<void*>(&DetourPass8OsGameUrlProbe),
                reinterpret_cast<void**>(&g_pass8_os_game_url_original),
                "pass8_osgameurl");
        }
        const std::uintptr_t activity_timer_address =
            ResolveAddress(kPass8ActivityTimerUpdateRva);
        if (ValidateHookSignature(
                activity_timer_address,
                kPass8ActivityTimerUpdateSignature,
                "pass8_activity_timer")) {
            activity_timer_installed = CreateHookDetour(
                activity_timer_address,
                reinterpret_cast<void*>(&DetourPass8ActivityTimerUpdate),
                reinterpret_cast<void**>(&g_pass8_activity_timer_original),
                "pass8_activity_timer");
        }
        ok &= wndproc_installed && os_game_url_installed && activity_timer_installed;
        log::InfoF(
            "pass8_probe requested=1 wndproc=%d osgameurl=%d activity_timer=%d "
            "layout=legacy_researched wndproc_target=0x%p osgameurl_target=0x%p "
            "activity_timer_target=0x%p behavior=observe_only",
            wndproc_installed ? 1 : 0,
            os_game_url_installed ? 1 : 0,
            activity_timer_installed ? 1 : 0,
            reinterpret_cast<void*>(wndproc_address),
            reinterpret_cast<void*>(os_game_url_address),
            reinterpret_cast<void*>(activity_timer_address));
    } else if (config.hook_pass8_probe) {
        log::Warn(
            "pass8_probe requested on latest_steam, but only legacy_researched "
            "is mapped; probe disabled");
    }

    if (config.fix_online_probe_stall) {
        // Doc-295/296 P2, revised per doc 299: the whole-function replacement
        // of v1 skipped the stock probe's manager-construction side effects,
        // which a later online tick dereferenced as NULL. v2 redirects only
        // the probe's single InternetOpenUrlA call site so the stock flow
        // (and all of its side effects) runs without the blocking network
        // wait. The observe-only pass8 probe detour on the same function
        // coexists with the call-site patch.
        ok &= engine_fixes::ApplyOnlineProbeStallFix(g_module_base,
                                                     g_use_latest_steam_layout);
    }

    if (config.fix_frame_counted_activity_timer && !config.hook_pass8_probe) {
        bool activity_timer_fix_installed = false;
        const std::uintptr_t activity_timer_address =
            ResolveAddress(kPass8ActivityTimerUpdateRva);
        if (ValidateHookSignature(
                activity_timer_address,
                kPass8ActivityTimerUpdateSignature,
                "activity_timer_fix")) {
            activity_timer_fix_installed = CreateHookDetour(
                activity_timer_address,
                reinterpret_cast<void*>(&DetourFrameCountedActivityTimerFix),
                reinterpret_cast<void**>(&g_activity_timer_fix_original),
                "activity_timer_fix");
        }
        ok &= activity_timer_fix_installed;
        log::InfoF(
            "activity_timer_fix requested=%d installed=%d "
            "layout=%s target=0x%p behavior=dt_scaled_pulse",
            config.fix_frame_counted_activity_timer ? 1 : 0,
            activity_timer_fix_installed ? 1 : 0,
            g_use_latest_steam_layout ? "latest_steam" : "legacy_researched",
            reinterpret_cast<void*>(activity_timer_address));
    } else if (config.fix_frame_counted_activity_timer && config.hook_pass8_probe) {
        log::Warn(
            "activity_timer_fix deferred: hook_pass8_probe owns the same "
            "target for observe-only probing");
    }

    if (config.restore_original_fog) {
        const std::uintptr_t setter_address =
            ResolveAddress(fog_restoration::kLegacySetterRva);
        const auto setter_signature =
            fog_restoration::SetterSignature(g_use_latest_steam_layout);
        if (ValidateHookSignature(
                setter_address, setter_signature, "volumetric_fog_setter")) {
            ok &= CreateHookDetour(
                setter_address,
                reinterpret_cast<void*>(&DetourVolumetricFogSetter),
                reinterpret_cast<void**>(&g_volumetric_fog_setter_original),
                "volumetric_fog_setter");
        }
    }

    const bool need_frameflow_hook = config.hook_frameflow || config.fix_cutscene_zero_dt ||
                                     config.restore_original_fog ||
                                     config.override_rumble_enabled >= 0 ||
                                     cut_content::IsArmed();
    if (need_frameflow_hook) {
        ok &= CreateHookDetour(ResolveAddress(kFrameFlowRva),
                               reinterpret_cast<void*>(&DetourFrameFlow),
                               reinterpret_cast<void**>(&g_frame_flow_original), "frame_flow");
    }
    if (need_cutscene_frameflow_hook) {
        ok &= CreateHookDetour(ResolveAddress(kCutsceneFlowOwnerRva),
                               reinterpret_cast<void*>(&DetourCutsceneFlowOwner),
                               reinterpret_cast<void**>(&g_cutscene_flow_owner_original),
                               "cutscene_flow_owner");
    }

    const vehicle_camera::CameraFeatureRequests camera_feature_requests{
        .gta_iv_car_camera = config.gta_iv_car_camera,
        .gta_iv_bike_camera = config.gta_iv_bike_camera,
        .gta_v_speed_shake = config.gtav_speed_shake,
        // NeedsVehicleCameraCoreHooks() ORs all of these, and the
        // designated initialiser lists were carrying only seven: converting the
        // donor's positional lists dropped these four. Each is therefore inert
        // when it is the ONLY camera flag set - including
        // DisableControllerCameraAutocenter, which the README documents as
        // working in vehicles without either GTA IV camera flag, which is
        // exactly that case. Declaration order is required for C++20
        // designated initialisers: these sit before speed_rumble.
        .disable_controller_camera_autocenter = config.disable_controller_camera_autocenter,
        .disable_onfoot_camera_autocenter = config.disable_onfoot_camera_autocenter,
        .driving_swear_lines = config.driving_swear_lines,
        .speed_rumble = config.speed_rumble,
        .disable_pursuit_camera = config.disable_pursuit_camera,
        .fix_chase_aim_camera = config.fix_chase_aim_camera,
        .modern_brake_reverse = config.modern_brake_reverse,
        .no_wheel_turn_back = config.no_wheel_turn_back,
    };
    if (vehicle_camera::NeedsVehicleCameraCoreHooks(
            camera_feature_requests)) {
        const bool angular_approach_required =
            vehicle_camera::NeedsGtaIvAngularApproachHook(
                camera_feature_requests);
        const vehicle_camera::AddressProfile camera_addresses =
            vehicle_camera::SelectAddresses(g_use_latest_steam_layout);
        const vehicle_camera::DynamicAddressProfile dynamic_addresses =
            vehicle_camera::SelectDynamicAddresses(
                g_use_latest_steam_layout);
        const std::uintptr_t setter_address =
            hook_layout_supported
                ? ResolveAddress(camera_addresses.parameter_setter_rva)
                : 0;
        const std::uintptr_t update_address =
            hook_layout_supported
                ? ResolveAddress(dynamic_addresses.update_rva)
                : 0;
        const std::uintptr_t desired_pose_address =
            hook_layout_supported
                ? ResolveAddress(dynamic_addresses.desired_eye_look_up_rva)
                : 0;
        const std::uintptr_t angular_approach_address =
            hook_layout_supported && angular_approach_required
                ? ResolveAddress(dynamic_addresses.angular_approach_rva)
                : 0;
        const auto angular_approach_signature =
            vehicle_camera::SelectAngularApproachSignature(
                g_use_latest_steam_layout);
        bool setter_installed = false;
        bool update_installed = false;
        bool desired_pose_installed = false;
        bool angular_approach_installed = false;
        if (!hook_layout_supported) {
            log::WarnF(
                "gtaiv_vehicle_camera car_requested=%d bike_requested=%d "
                "speed_shake_requested=%d installed=0 "
                "reason=unsupported_hook_layout",
                config.gta_iv_car_camera ? 1 : 0,
                config.gta_iv_bike_camera ? 1 : 0,
                config.gtav_speed_shake ? 1 : 0);
        } else {
            const bool camera_preflight =
                PreflightRequiredHookTarget(
                    setter_address,
                    vehicle_camera::kParameterSetterSignature,
                    "chase_camera_set_parameters") &&
                PreflightRequiredHookTarget(
                    update_address,
                    vehicle_camera::kChaseUpdateSignature,
                    "chase_camera_update") &&
                PreflightRequiredHookTarget(
                    desired_pose_address,
                    vehicle_camera::kDesiredEyeLookUpSignature,
                    "game_camera_desired_pose") &&
                (!angular_approach_required ||
                 PreflightRequiredHookTarget(
                     angular_approach_address,
                     angular_approach_signature,
                     "angular_approach"));
            if (!camera_preflight) {
                ok = false;
            } else {
                setter_installed = CreateHookDetour(
                    setter_address,
                    reinterpret_cast<void*>(
                        &DetourChaseCameraSetParameters),
                    reinterpret_cast<void**>(
                        &g_chase_camera_set_parameters_original),
                    "chase_camera_set_parameters");
                ok &= setter_installed;
                if (setter_installed) {
                    update_installed = CreateHookDetour(
                        update_address,
                        reinterpret_cast<void*>(&DetourChaseCameraUpdate),
                        reinterpret_cast<void**>(
                            &g_chase_camera_update_original),
                        "chase_camera_update");
                }
                if (update_installed) {
                    desired_pose_installed = CreateHookDetour(
                        desired_pose_address,
                        reinterpret_cast<void*>(
                            &DetourGameCameraSetDesiredEyeLookUp),
                        reinterpret_cast<void**>(
                            &g_game_camera_set_desired_eye_look_up_original),
                        "game_camera_desired_pose");
                }
                if (desired_pose_installed && angular_approach_required) {
                    angular_approach_installed = CreateHookDetour(
                        angular_approach_address,
                        reinterpret_cast<void*>(&DetourAngularApproach),
                        reinterpret_cast<void**>(&g_angular_approach_original),
                        "angular_approach");
                }
            }
        }
        const bool angular_approach_complete =
            !angular_approach_required || angular_approach_installed;
        const bool installed = setter_installed && update_installed &&
                               desired_pose_installed &&
                               angular_approach_complete;
        if (hook_layout_supported) {
            ok &= installed;
        }
        log::InfoF(
            "gtaiv_vehicle_camera car_requested=%d bike_requested=%d "
            "speed_shake_requested=%d installed=%d "
            "setter_installed=%d update_installed=%d "
            "desired_pose_installed=%d angular_approach_installed=%d "
            "angular_approach_required=%d "
            "layout=%s target=0x%p update_target=0x%p "
            "desired_pose_target=0x%p angular_approach_target=0x%p "
            "mode=%s mutation=%d dynamics_mutation=%d "
            "speed_shake_mutation=%d "
            "car_behavior=road_vehicle_drive_and_flee "
            "heavy_drive_alias_guard=base_selector_replay "
            "bike_behavior=motorcycle_drive_block "
            "bike_flee_behavior=%s direction=right "
            "lateral_offset_m=%.3f",
            config.gta_iv_car_camera ? 1 : 0,
            config.gta_iv_bike_camera ? 1 : 0,
            config.gtav_speed_shake ? 1 : 0,
            installed ? 1 : 0,
            setter_installed ? 1 : 0,
            update_installed ? 1 : 0,
            desired_pose_installed ? 1 : 0,
            angular_approach_installed ? 1 : 0,
            angular_approach_required ? 1 : 0,
            g_use_latest_steam_layout ? "latest_steam" :
                                        "legacy_researched",
            reinterpret_cast<void*>(setter_address),
            reinterpret_cast<void*>(update_address),
            reinterpret_cast<void*>(desired_pose_address),
            reinterpret_cast<void*>(angular_approach_address),
            installed ? "active" : "disabled",
            installed && (config.gta_iv_car_camera ||
                          config.gta_iv_bike_camera)
                ? 1
                : 0,
            installed && angular_approach_required ? 1 : 0,
            installed && config.gtav_speed_shake ? 1 : 0,
            !config.gta_iv_bike_camera
                ? "disabled_by_config"
                : (installed ? "active" : "unavailable"),
            static_cast<double>(
                vehicle_camera::kDriverSideLateralOffsetMeters));
        if (config.gtav_speed_shake) {
            const gtav_speed_shake::Tuning speed_shake_tuning =
                CurrentGtaVSpeedShakeTuning();
            const gtav_speed_shake::Offsets speed_shake_limits =
                gtav_speed_shake::MaximumChannelOffsets(
                    speed_shake_tuning);
            const bool stock_exact =
                config.gtav_speed_shake_strength_percent == 100 &&
                config.gtav_speed_shake_start_speed_kmh == 90 &&
                config.gtav_speed_shake_full_speed_kmh == 162;
            log::InfoF(
                "gtav_speed_shake requested=1 installed=%d "
                "setter_installed=%d update_installed=%d "
                "desired_pose_installed=%d "
                "feature_angular_approach_required=0 "
                "transaction_angular_approach_required=%d "
                "transaction_angular_approach_installed=%d "
                "strength_percent=%d minimum_forward_mps=%.6f "
                "maximum_forward_mps=%.6f "
                "curve=cosine_slow_in_out "
                "stock_minimum_forward_mps=%.3f "
                "stock_maximum_forward_mps=%.3f stock_exact=%d "
                "spring_constant=%.3f attack_seconds=%.3f "
                "sustain_envelope=%.3f max_offset_right_m=%.9f "
                "max_offset_up_m=%.9f max_pitch_rad=%.9f "
                "max_roll_rad=%.9f placement=pre_native_collision "
                "native_impact_state=untouched",
                installed ? 1 : 0,
                setter_installed ? 1 : 0,
                update_installed ? 1 : 0,
                desired_pose_installed ? 1 : 0,
                angular_approach_required ? 1 : 0,
                angular_approach_installed ? 1 : 0,
                config.gtav_speed_shake_strength_percent,
                static_cast<double>(speed_shake_tuning.start_speed_mps),
                static_cast<double>(speed_shake_tuning.full_speed_mps),
                static_cast<double>(
                    gtav_speed_shake::
                        kMinimumForwardSpeedMetersPerSecond),
                static_cast<double>(
                    gtav_speed_shake::
                        kMaximumForwardSpeedMetersPerSecond),
                stock_exact ? 1 : 0,
                static_cast<double>(
                    gtav_speed_shake::kSpeedResponseSpringConstant),
                static_cast<double>(
                    gtav_speed_shake::kAttackDurationSeconds),
                static_cast<double>(
                    gtav_speed_shake::kSustainEnvelope),
                static_cast<double>(
                    speed_shake_limits.relative_x_meters),
                static_cast<double>(
                    speed_shake_limits.relative_z_meters),
                static_cast<double>(
                    speed_shake_limits.rotation_x_radians),
                static_cast<double>(
                    speed_shake_limits.rotation_y_radians));
        }
    }

    if (config.hook_fog_slicing_guard) {
        ok &= CreateHookDetour(
            ResolveAddress(kFogSlicingModeRva), reinterpret_cast<void*>(&DetourFogSlicingMode),
            reinterpret_cast<void**>(&g_fog_slicing_mode_original), "fog_slicing_mode");
    }

    // Stock-AA suppression is a behavioral dependency of SMAA, not a
    // developer-probe dependency.  The AntiAlias targets are remapped for
    // both layouts now (site-evidence-wave2.md), so suppression runs on
    // latest too.
    const bool need_stock_aa_suppression =
        config.hook_smaa_present && config.smaa_disable_stock_aa &&
        (config.smaa_enable || config.smaa_debug_keys);
    const bool need_aa_owner_hook =
        config.hook_aa_probe || need_stock_aa_suppression;
    const bool need_aa_fx_hook =
        config.hook_aa_fx_probe || need_stock_aa_suppression;

    if (need_aa_owner_hook) {
        const bool installed = CreateHookDetour(
            ResolveAddress(kAntiAliasOwnerRva), reinterpret_cast<void*>(&DetourAntiAliasOwner),
            reinterpret_cast<void**>(&g_antialias_owner_original), "antialias_owner");
        ok &= installed;
        log::InfoF("aa_owner_hook requested=1 installed=%d probe=%d stock_suppression=%d",
                   installed ? 1 : 0,
                   config.hook_aa_probe ? 1 : 0,
                   need_stock_aa_suppression ? 1 : 0);
    }

    if (need_aa_fx_hook) {
        const bool installed = CreateHookDetour(
            ResolveAddress(kAntiAliasFxHandoffRva),
            reinterpret_cast<void*>(&DetourAntiAliasFxHandoff),
            reinterpret_cast<void**>(&g_antialias_fx_handoff_original),
            "antialias_fx_handoff");
        ok &= installed;
        log::InfoF("aa_fx_hook requested=1 installed=%d probe=%d stock_suppression=%d",
                   installed ? 1 : 0,
                   config.hook_aa_fx_probe ? 1 : 0,
                   need_stock_aa_suppression ? 1 : 0);
    }
    const bool need_aa_aux_hook =
        !g_use_latest_steam_layout && (config.hook_aa_probe || config.hook_aa_fx_probe ||
                                       config.aa_aux_mode != 0 || config.aa_aux_debug_keys);
    if (need_aa_aux_hook) {
        ok &= CreateHookDetour(ResolveAddress(kAntiAliasAuxStateQueryRva),
                               reinterpret_cast<void*>(&DetourAntiAliasAuxStateQuery),
                               reinterpret_cast<void**>(&g_antialias_aux_state_query_original),
                               "antialias_aux_state_query");
    }
    if (config.hook_aa_probe || config.hook_aa_fx_probe) {
        log::InfoF("aa_variant mode=%d debug_keys=%d key_down=F6 key_up=F7",
                   g_aa_runtime_variant_mode.load(), config.aa_variant_debug_keys ? 1 : 0);
        log::InfoF("aa_aux mode=%d debug_keys=%d key_down=F4 key_up=F5",
                   g_aa_runtime_aux_mode.load(), config.aa_aux_debug_keys ? 1 : 0);
    }
    if (config.hook_smaa_present) {
        log::InfoF("smaa requested=%d debug_keys=%d preset=%d toggle=F2 device_slot=0x%p "
                   "context_slot=0x%p swapchain_slot=0x%p rtv_slot=0x%p",
                   config.smaa_enable ? 1 : 0, config.smaa_debug_keys ? 1 : 0, config.smaa_preset,
                   reinterpret_cast<void*>(g_d3d_device_slot),
                   reinterpret_cast<void*>(g_d3d_context_slot),
                   reinterpret_cast<void*>(g_dxgi_swapchain_slot),
                   reinterpret_cast<void*>(g_present_rtv_slot));
    }

    if (config.hook_post_material_submit) {
        ok &= CreateHookDetour(ResolveAddress(kRenderSubmitMaterialRva),
                               reinterpret_cast<void*>(&DetourRenderSubmitMaterial),
                               reinterpret_cast<void**>(&g_render_submit_material_original),
                               "render_submit_material");
    }

    if (config.hook_smaa_present) {
        ok &= CreateHookDetour(ResolveAddress(kPresentBufferRva),
                               reinterpret_cast<void*>(&DetourPresentBuffer),
                               reinterpret_cast<void**>(&g_present_buffer_original),
                               "present_buffer");
    }

    bool character_effects_hooks_installed = !need_character_effects_hooks;
    if (need_character_effects_hooks) {
        const bool water_hook_installed = CreateHookDetour(
            ResolveAddress(kCharacterHandleWaterCollisionRva),
            reinterpret_cast<void*>(&DetourCharacterHandleWaterCollision),
            reinterpret_cast<void**>(&g_character_handle_water_collision_original),
            "character_handle_water_collision");
        const bool update_hook_installed = CreateHookDetour(
            ResolveAddress(kCharacterEffectsUpdateRva),
            reinterpret_cast<void*>(&DetourCharacterEffectsUpdate),
            reinterpret_cast<void**>(&g_character_effects_update_original),
            "character_effects_update");
        character_effects_hooks_installed =
            water_hook_installed && update_hook_installed;
        ok &= character_effects_hooks_installed;
    }
    // The time-dilation apply point is remapped for both layouts now
    // (2026-10-31, 40-instruction lockstep evidence), so the drive-by and
    // stunt slow-mo disables run on latest too.
    const bool need_time_dilate_hook =
        g_config.probe_slow_mo || g_config.disable_driveby_slow_mo ||
        g_config.disable_stunt_slow_mo;
    if (need_time_dilate_hook) {
        ok &= CreateHookDetour(ResolveAddress(kTimeDilateApplyRva),
                               reinterpret_cast<void*>(&DetourTimeDilateApply),
                               reinterpret_cast<void**>(&g_time_dilate_apply_original),
                               "time_dilate_apply");
    }

    // GTAVITimeClock: flatten the day-night clock onto DayMinutes real
    // minutes (plan decision 8.2). Unexpected prologue bytes skip only this
    // feature; a CreateHookDetour failure still closes the transaction like
    // every other install.
    if (g_config.gta_vi_time_clock) {
        const std::uintptr_t tod_clock_address =
            ResolveAddress(kTimeOfDayClockUpdateRva);
        if (tod_clock_address == 0) {
            log::Warn("gtavi_time_clock requested but unresolved for the "
                      "current layout; feature disabled");
        } else if (ValidateOptionalHookSignature(
                       tod_clock_address,
                       kTimeOfDayClockUpdateSignature,
                       "time_of_day_clock_update") &&
                   CreateHookDetour(
                       tod_clock_address,
                       reinterpret_cast<void*>(&DetourTimeOfDayClockUpdate),
                       reinterpret_cast<void**>(
                           &g_time_of_day_clock_update_original),
                       "time_of_day_clock_update")) {
            log::InfoF("gtavi_time_clock installed day_minutes=%d target=0x%p",
                       g_config.gta_vi_time_clock_day_minutes,
                       reinterpret_cast<void*>(tod_clock_address));
        }
    }

    // The sound-event entry points (0x143890 name->id, 0x144230 and 0x14C150
    // playback) are researched for the legacy layout only. On latest_steam
    // RemapRvaForCurrentBuild returns an unmapped legacy RVA unchanged, so
    // ResolveAddress would hand CreateHookDetour a garbage-but-in-range
    // address; warn and skip rather than patch it.
    const bool need_sound_event_hooks =
        !g_use_latest_steam_layout &&
        (g_config.probe_slow_mo || g_config.disable_stunt_sound_fx);
    if ((g_config.probe_slow_mo || g_config.disable_stunt_sound_fx) &&
        g_use_latest_steam_layout) {
        log::Warn("sound_event_hooks requested on latest_steam, but only "
                  "legacy_researched is mapped; sound_name_resolve/sound_post/"
                  "sound_post2 disabled");
    }

    if (need_sound_event_hooks) {
        ok &= CreateHookDetour(ResolveAddress(kSoundNameResolveRva),
                               reinterpret_cast<void*>(&DetourSoundNameResolve),
                               reinterpret_cast<void**>(&g_sound_name_resolve_original),
                               "sound_name_resolve");
        ok &= CreateHookDetour(ResolveAddress(kSoundPostRva),
                               reinterpret_cast<void*>(&DetourSoundPost),
                               reinterpret_cast<void**>(&g_sound_post_original),
                               "sound_post");
    }

    if (need_sound_event_hooks) {
        ok &= CreateHookDetour(ResolveAddress(kSoundPost2Rva),
                               reinterpret_cast<void*>(&DetourSoundPost2),
                               reinterpret_cast<void**>(&g_sound_post2_original),
                               "sound_post2");
    }

    if (g_config.diag_drivetrain_probe || g_config.modern_brake_reverse ||
        g_config.no_wheel_turn_back) {
        // The six drivetrain/steering RVAs are remapped for both layouts now
        // (2026-10-31, vtable-slot + lockstep evidence), so ModernBrakeReverse
        // and NoWheelTurnBack run on latest too. Targets resolved from RVAs
        // and pre-verified: a failure only skips the probe and never aborts
        // the hook transaction.
        {
        // Flat statements, targets resolved from RVAs and pre-verified: a
        // failure only skips the probe and never aborts the hook
        // transaction, and no probe is nested inside another's branch.
        const std::uintptr_t driver_target = ResolveAddress(kAnalogDriverApplyRva);
        const hook_guard::Result driver_guard = g_hook_target_guard.Verify(
            reinterpret_cast<const void*>(driver_target),
            hook_guard::kMinHookTargetVerificationBytes);
        if (!driver_guard.verified()) {
            log::Warn("drivetrain_probe: driver target not verified; skipped");
        } else if (!CreateHookDetour(
                       driver_target,
                       reinterpret_cast<void*>(&DetourAnalogDriverApply),
                       reinterpret_cast<void**>(&g_analog_driver_apply_original),
                       "drivetrain_probe_driver")) {
            log::Warn("drivetrain_probe: driver hook did not attach");
        } else {
            log::Info("drivetrain_probe: analog driver apply hooked");
        }

        const std::uintptr_t shift_target =
            ResolveAddress(kTransmissionShiftApplyRva);
        const hook_guard::Result shift_guard = g_hook_target_guard.Verify(
            reinterpret_cast<const void*>(shift_target),
            hook_guard::kMinHookTargetVerificationBytes);
        if (!shift_guard.verified()) {
            log::Warn("drivetrain_probe: shift target not verified; skipped");
        } else if (!CreateHookDetour(
                       shift_target,
                       reinterpret_cast<void*>(&DetourTransmissionShift),
                       reinterpret_cast<void**>(&g_transmission_shift_original),
                       "drivetrain_probe_shift")) {
            log::Warn("drivetrain_probe: shift hook did not attach");
        } else {
            log::Info("drivetrain_probe: transmission shift hooked");
        }

        const std::uintptr_t ratio_target =
            ResolveAddress(kTransmissionRatioApplyRva);
        const hook_guard::Result ratio_guard = g_hook_target_guard.Verify(
            reinterpret_cast<const void*>(ratio_target),
            hook_guard::kMinHookTargetVerificationBytes);
        if (!ratio_guard.verified()) {
            log::Warn("drivetrain_probe: ratio target not verified; skipped");
        } else if (!CreateHookDetour(
                       ratio_target,
                       reinterpret_cast<void*>(&DetourTransmissionRatio),
                       reinterpret_cast<void**>(&g_transmission_ratio_original),
                       "drivetrain_probe_ratio")) {
            log::Warn("drivetrain_probe: ratio hook did not attach");
        } else {
            log::Info("drivetrain_probe: transmission ratio hooked");
        }

        const std::uintptr_t engine_target =
            ResolveAddress(kVehicleEngineApplyRva);
        const hook_guard::Result engine_guard = g_hook_target_guard.Verify(
            reinterpret_cast<const void*>(engine_target),
            hook_guard::kMinHookTargetVerificationBytes);
        if (!engine_guard.verified()) {
            log::Warn("drivetrain_probe: engine target not verified; skipped");
        } else if (!CreateHookDetour(
                       engine_target,
                       reinterpret_cast<void*>(&DetourVehicleEngineApply),
                       reinterpret_cast<void**>(
                           &g_vehicle_engine_apply_original),
                       "drivetrain_probe_engine")) {
            log::Warn("drivetrain_probe: engine hook did not attach");
        } else {
            log::Info("drivetrain_probe: engine apply hooked");
        }

        const std::uintptr_t steering_callback_target =
            ResolveAddress(kSteeringCallbackRva);
        const hook_guard::Result steering_callback_guard =
            g_hook_target_guard.Verify(
                reinterpret_cast<const void*>(steering_callback_target),
                hook_guard::kMinHookTargetVerificationBytes);
        if (!steering_callback_guard.verified()) {
            log::Warn("steering_probe: callback target not verified; skipped");
        } else if (!CreateHookDetour(
                       steering_callback_target,
                       reinterpret_cast<void*>(&DetourSteeringCallback),
                       reinterpret_cast<void**>(
                           &g_steering_callback_original),
                       "steering_probe_callback")) {
            log::Warn("steering_probe: callback hook did not attach");
        } else {
            log::Info("steering_probe: steering callback hooked");
        }

        const std::uintptr_t steering_input_target =
            ResolveAddress(kSteeringInputRva);
        const hook_guard::Result steering_input_guard =
            g_hook_target_guard.Verify(
                reinterpret_cast<const void*>(steering_input_target),
                hook_guard::kMinHookTargetVerificationBytes);
        if (!steering_input_guard.verified()) {
            log::Warn("steering_probe: input target not verified; skipped");
        } else if (!CreateHookDetour(
                       steering_input_target,
                       reinterpret_cast<void*>(&DetourSteeringInput),
                       reinterpret_cast<void**>(&g_steering_input_original),
                       "steering_probe_input")) {
            log::Warn("steering_probe: input hook did not attach");
        } else {
            log::Info("steering_probe: steering input hooked");
        }
        if (g_config.modern_brake_reverse) {
            log::Info("modern_brake_reverse: armed (config parsed enabled; "
                      "both layouts mapped)");
        } else if (g_config.diag_drivetrain_probe) {
            log::Info("modern_brake_reverse: config parsed disabled; probe "
                      "diagnostics only");
        }
        }
    }

    bool health_damage_hook_installed =
        !character_hook_plan.install_health_damage_hook;
    bool character_regression_hooks_installed =
        !need_character_regression_probe;
    if (need_character_regression_probe) {
        character_regression_hooks_installed = CreateHookDetour(
            ResolveAddress(kHealthApplyDamageRva),
            reinterpret_cast<void*>(&DetourHealthApplyDamage),
            reinterpret_cast<void**>(&g_health_apply_damage_original),
            "health_apply_damage");
        health_damage_hook_installed = character_regression_hooks_installed;
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterPaintOwnerRva),
            reinterpret_cast<void*>(&DetourCharacterPaintOwner),
            reinterpret_cast<void**>(&g_character_paint_owner_original),
            "character_paint_owner");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterPaintConsumerRva),
            reinterpret_cast<void*>(&DetourCharacterPaintConsumer),
            reinterpret_cast<void**>(&g_character_paint_consumer_original),
            "character_paint_consumer");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterEffectDispatchOwnerRva),
            reinterpret_cast<void*>(&DetourCharacterEffectDispatchOwner),
            reinterpret_cast<void**>(&g_character_effect_dispatch_owner_original),
            "character_effect_dispatch_owner");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterEffectDispatchConsumeRva),
            reinterpret_cast<void*>(&DetourCharacterEffectDispatchConsume),
            reinterpret_cast<void**>(&g_character_effect_dispatch_consume_original),
            "character_effect_dispatch_consume");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterEffectQueueBuilderRva),
            reinterpret_cast<void*>(&DetourCharacterEffectQueueBuilder),
            reinterpret_cast<void**>(&g_character_effect_queue_builder_original),
            "character_effect_queue_builder");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterAnimationApplyCharredEffectRva),
            reinterpret_cast<void*>(&DetourCharacterAnimationApplyCharredEffect),
            reinterpret_cast<void**>(&g_character_animation_apply_charred_effect_original),
            "character_animation_apply_charred_effect");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterAnimationApplyPaintEffectRva),
            reinterpret_cast<void*>(&DetourCharacterAnimationApplyPaintEffect),
            reinterpret_cast<void**>(&g_character_animation_apply_paint_effect_original),
            "character_animation_apply_paint_effect");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterAnimationCreateDamageRigRva),
            reinterpret_cast<void*>(&DetourCharacterAnimationCreateDamageRig),
            reinterpret_cast<void**>(&g_character_animation_create_damage_rig_original),
            "character_animation_create_damage_rig");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kDamageRigApplyCharredEffectRva),
            reinterpret_cast<void*>(&DetourDamageRigApplyCharredEffect),
            reinterpret_cast<void**>(&g_damage_rig_apply_charred_effect_original),
            "damage_rig_apply_charred_effect");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kDamageRigApplyPaintEffectRva),
            reinterpret_cast<void*>(&DetourDamageRigApplyPaintEffect),
            reinterpret_cast<void**>(&g_damage_rig_apply_paint_effect_original),
            "damage_rig_apply_paint_effect");
        character_regression_hooks_installed &= CreateHookDetour(
            ResolveAddress(kCharacterDamageRigResetDamageRva),
            reinterpret_cast<void*>(&DetourCharacterDamageRigResetDamage),
            reinterpret_cast<void**>(&g_character_damage_rig_reset_damage_original),
            "character_damage_rig_reset_damage");
        ok &= character_regression_hooks_installed;
    } else if (character_hook_plan.install_health_damage_hook) {
        health_damage_hook_installed = CreateHookDetour(
            ResolveAddress(kHealthApplyDamageRva),
            reinterpret_cast<void*>(&DetourHealthApplyDamage),
            reinterpret_cast<void**>(&g_health_apply_damage_original),
            "health_apply_damage");
        ok &= health_damage_hook_installed;
    }

    if (config.restore_character_wetness) {
        log::InfoF(
            "character_wetness_restore requested=1 hooks_installed=%d scope=player_npc full_time_seconds=%d fade_time_seconds=%d",
            character_effects_hooks_installed ? 1 : 0,
            config.wetness_full_time_seconds,
            config.wetness_fade_time_seconds);
    }
    if (config.restore_character_sweat) {
        const bool sweat_hooks_installed =
            character_effects_hooks_installed && health_damage_hook_installed;
        log::InfoF(
            "character_sweat_restore requested=1 hooks_installed=%d scope=player_npc build_seconds=%d fade_seconds=%d onset_seconds=%d run_speed=%.2f combat_time=%d velocity_input=%d vehicle_input=%d combat_input=%d",
            sweat_hooks_installed ? 1 : 0,
            config.sweat_build_time_seconds,
            config.sweat_fade_time_seconds,
            config.sweat_onset_time_seconds,
            config.sweat_run_speed,
            config.sweat_combat_time_seconds,
            g_character_physics_get_velocity != nullptr ? 1 : 0,
            g_is_character_in_vehicle != nullptr ? 1 : 0,
            g_ui_is_player_in_combat != nullptr ? 1 : 0);
    }
    if (need_character_regression_probe) {
        const bool probe_hooks_installed =
            character_effects_hooks_installed &&
            character_regression_hooks_installed;
        log::InfoF(
            "character_regression_probe requested=1 hooks_installed=%d "
            "wetness=HandleWaterCollision/CharacterEffects::Update/DispatchOwner/DispatchConsume/"
            "CharacterPaintConsumer material=ApplyCharredEffect/ApplyPaintEffect "
            "damage=ApplyHealthDamage/CreateDamageRig/ResetDamage",
            probe_hooks_installed ? 1 : 0);
    }

    if (config.override_rumble_enabled >= 0) {
        log::InfoF("rumble_override configured=%d object=0x%p helper=0x%p",
                   config.override_rumble_enabled, reinterpret_cast<void*>(g_rumble_apply_object),
                   reinterpret_cast<void*>(g_rumble_apply_helper));
    }
    if (config.hook_scenery_builders) {
        ok &= CreateHookDetour(
            ResolveAddress(kSceneryPrepareRva), reinterpret_cast<void*>(&DetourSceneryPrepare),
            reinterpret_cast<void**>(&g_scenery_prepare_original), "scenery_prepare");
        ok &= CreateHookDetour(
            ResolveAddress(kScenerySetupRva), reinterpret_cast<void*>(&DetourScenerySetup),
            reinterpret_cast<void**>(&g_scenery_setup_original), "scenery_setup");
        ok &= CreateHookDetour(ResolveAddress(kRenderSceneryBuilderRva),
                               reinterpret_cast<void*>(&DetourRenderSceneryBuilder),
                               reinterpret_cast<void**>(&g_render_scenery_builder_original),
                               "render_scenery_builder");
        ok &= CreateHookDetour(ResolveAddress(kRasterizeBucketBuilderRva),
                               reinterpret_cast<void*>(&DetourRasterizeBucketBuilder),
                               reinterpret_cast<void**>(&g_rasterize_bucket_builder_original),
                               "rasterize_bucket_builder");
    }

    // Install this narrow resource-load hook last. It avoids the confirmed
    // render-submit hot path and minimizes the pre-commit window in which a
    // streamed material could pass through while behavior is still gated.
    if (config.restore_original_eye_reflections) {
        const std::uintptr_t material_on_load_address = ResolveAddress(kMaterialOnLoadRva);
        if (ValidateHookSignature(
                material_on_load_address, kMaterialOnLoadSignature, "material_on_load")) {
            const bool installed = CreateHookDetour(
                material_on_load_address,
                reinterpret_cast<void*>(&DetourMaterialOnLoad),
                reinterpret_cast<void**>(&g_material_on_load_original),
                "material_on_load");
            ok &= installed;
            if (installed) {
                log::Info(
                    "character_eye_restore requested=1 hooks_installed=1 "
                    "behavior=pending_commit scope=wei_eye_materials variants=3");
            }
        }
    }

    bool sampler_builder_prevalidated = false;
    if (ok && anisotropy_override_requested) {
        bool installed = false;
        if (sampler_builder_address == 0) {
            log::Warn(
                "texture_filtering sampler_builder disabled=1 "
                "reason=unsupported_hook_layout");
        } else if (ValidateHookSignature(
                       sampler_builder_address,
                       texture_filtering::kSamplerBuilderPrologue,
                       "sampler_builder")) {
            installed = CreateHookDetour(
                sampler_builder_address,
                reinterpret_cast<void*>(&DetourSamplerBuilder),
                reinterpret_cast<void**>(&g_sampler_builder_original),
                "sampler_builder");
        }
        ok &= installed;
        sampler_builder_prevalidated = installed;
        log::InfoF(
            "texture_filtering sampler_builder requested=%d installed=%d "
            "layout=%s target=0x%p behavior=pending_commit",
            config.anisotropic_filtering,
            installed ? 1 : 0,
            g_use_latest_steam_layout ? "latest_steam" :
                                        "legacy_researched",
            reinterpret_cast<void*>(sampler_builder_address));
    }

    ok = ok && g_hook_creation_transaction_open;
    if (ok) {
        // Static byte patches commit last. All MinHook detours are already
        // installed but remain transparent behind the global behavior gate,
        // eliminating the former long window where engine bytes were live
        // while later hook creation could still fail and roll them back.
        ok = engine_fixes::InitializeStaticPatches(
            config,
            g_module_base,
            g_use_latest_steam_layout,
            sampler_builder_prevalidated,
            display_settings_path);
    }

    // Depth of field. Must be inside the hook-creation transaction: the
    // install goes through CreateHookDetour, which registers the target for
    // rollback on a later failure. Outside the window a partial init could
    // leave a live DoF detour behind with nothing left to unwind it.
    // A failure here is non-fatal and leaves the keys inert.
    InstallDofParametersHook();

    // Heat meter + dynamic radar. Optional: a failure here must never abort
    // initialization, so this is a flat call, not `ok &=`.
    InstallDynamicRadarHook();

    // Script screen-filter capture (see the block above DetourHeatMeterApply).
    // Legacy-only: the funnel RVA is unmapped for latest Steam, and the guard
    // prevents ResolveAddress from handing back an unrelated function.
    if (g_config.probe_screen_filter) {
        if (g_use_latest_steam_layout) {
            log::Warn("screen_filter_probe skipped reason=unmapped_latest_steam");
        } else {
            const std::uintptr_t screen_filter_address =
                ResolveAddress(kScriptScreenFilterApplyRva);
            if (screen_filter_address == 0 ||
                !ValidateOptionalHookSignature(
                    screen_filter_address, kScriptScreenFilterApplySignature,
                    "screen_filter_apply")) {
                log::Warn("screen_filter_probe: funnel target not verified; "
                          "capture unavailable");
            } else if (!CreateHookDetour(
                           screen_filter_address,
                           reinterpret_cast<void*>(
                               &DetourScriptScreenFilterApply),
                           reinterpret_cast<void**>(
                               &g_screen_filter_apply_original),
                           "screen_filter_apply")) {
                log::Warn("screen_filter_probe: hook did not attach");
            } else {
                log::Info("screen_filter_probe hooked diagnostic=1");
            }
        }
    }

    if (g_config.diag_blip_probe && g_use_latest_steam_layout) {
        log::Warn("blip probe hooks skipped reason=unmapped_latest_steam");
    } else if (g_config.diag_blip_probe) {
        // Diagnostic only. Three rules, each learned the hard way:
        //  * the target must be resolved from the RVA (the hook API takes an absolute
        //    address, and a bare RVA is rejected);
        //  * the target must be verified BEFORE attempting the hook, because a rejected
        //    target aborts the whole hook transaction;
        //  * every probe must be a flat statement - a probe nested inside another
        //    probe's failure branch silently never runs, which produces a false
        //    "the game never calls it" result.
        const std::uintptr_t blip_target = ResolveAddress(kBlipTypeVisibleRva);
        const hook_guard::Result blip_guard = g_hook_target_guard.Verify(
            reinterpret_cast<const void*>(blip_target),
            hook_guard::kMinHookTargetVerificationBytes);
        if (!blip_guard.verified()) {
            log::Warn("blip_probe: blip_type_visible target not verified; skipped");
        } else if (!CreateHookDetour(
                       blip_target,
                       reinterpret_cast<void*>(&DetourBlipTypeVisible),
                       reinterpret_cast<void**>(&g_blip_type_visible_original),
                       "blip_type_visible")) {
            log::Warn("blip_probe: set_blip_type_visible hook did not attach");
        } else {
            log::Info("blip_probe: set_blip_type_visible hooked");
        }

        const std::uintptr_t add_blip_target = ResolveAddress(kAddBlipXyRva);
        const hook_guard::Result add_blip_guard = g_hook_target_guard.Verify(
            reinterpret_cast<const void*>(add_blip_target),
            hook_guard::kMinHookTargetVerificationBytes);
        if (!add_blip_guard.verified()) {
            log::Warn("blip_probe: add_blip_xy target not verified; skipped");
        } else if (!CreateHookDetour(
                       add_blip_target,
                       reinterpret_cast<void*>(&DetourAddBlipXy),
                       reinterpret_cast<void**>(&g_add_blip_xy_original),
                       "add_blip_xy")) {
            log::Warn("blip_probe: add_blip_xy hook did not attach");
        } else {
            log::Info("blip_probe: add_blip_xy hooked");
        }

        const std::uintptr_t veh_blip_target = ResolveAddress(kEnableVehiclesBlipRva);
        const hook_guard::Result veh_blip_guard = g_hook_target_guard.Verify(
            reinterpret_cast<const void*>(veh_blip_target),
            hook_guard::kMinHookTargetVerificationBytes);
        if (!veh_blip_guard.verified()) {
            log::Warn("blip_probe: enable_vehicles_blip target not verified; skipped");
        } else if (!CreateHookDetour(
                       veh_blip_target,
                       reinterpret_cast<void*>(&DetourEnableVehiclesBlip),
                       reinterpret_cast<void**>(&g_enable_vehicles_blip_original),
                       "enable_vehicles_blip")) {
            log::Warn("blip_probe: enable_vehicles_blip hook did not attach");
        } else {
            log::Info("blip_probe: enable_vehicles_blip hooked");
        }
    }
    // Cop-symbol data patch (replaces the removed blip detours, see the
    // removal note at the former DetourBlipSymbolApply site) is applied by
    // ApplyCopSymbolDataPatch above: zero the first byte of each name after
    // verifying full contents. Fail-closed, restored in Shutdown().
    // The two blip detours are permanently not installed: their presence
    // crashed the game within ~40-90 s of boot on every HideCopRadarIcons=1
    // run, while every run without them stayed clean. Cop icons stay hidden
    // through the pursuit heat-controller path plus the symbol-data patch
    // below; the blip probes further below remain available under BlipProbe
    // for diagnosis.
    if (g_config.hide_cop_radar_icons) {
        log::Warn("cop_radar_icons: blip detours not installed (removed: "
                  "presence crash); hiding via heat-controller path only");
    }
    if (g_config.hide_cop_radar_icons && !ApplyCopSymbolDataPatch()) {
        log::Warn("cop_radar_icons: symbol-data patch not applied; heat-zero "
                  "path still hides pursuit icons");
    }
    if (g_config.diag_blip_probe && g_use_latest_steam_layout) {
        log::Warn("cop_radar_icons: blip wrapper hook skipped reason=unmapped_latest_steam");
    } else if (g_config.diag_blip_probe) {
        // Flat statement (never nested inside another probe's branch), target
        // resolved from the RVA and pre-verified: a failure only disables the
        // feature and never aborts the hook transaction.
        const std::uintptr_t wrapper_target =
            ResolveAddress(kBlipWrapperSetDataRva);
        const hook_guard::Result wrapper_guard = g_hook_target_guard.Verify(
            reinterpret_cast<const void*>(wrapper_target),
            hook_guard::kMinHookTargetVerificationBytes);
        if (!wrapper_guard.verified()) {
            log::Warn("cop_radar_icons: blip_wrapper_set_data target not "
                      "verified; feature disabled");
        } else if (!CreateHookDetour(
                       wrapper_target,
                       reinterpret_cast<void*>(&DetourBlipWrapperSetData),
                       reinterpret_cast<void**>(
                           &g_blip_wrapper_set_data_original),
                       "blip_wrapper_set_data")) {
            log::Warn("cop_radar_icons: blip_wrapper_set_data hook did not "
                      "attach; feature disabled");
        } else {
            log::Info("cop_radar_icons: blip_wrapper_set_data hooked");
        }
    }
    g_hook_creation_transaction_open = false;

    log::InfoF("hook_layout selected=%s",
               g_use_latest_steam_layout ? "latest_steam" : "legacy_researched");
    if (!ok) {
        // The bundled MinHook variant enables hooks during creation, but its
        // only teardown API frees original trampolines.  A failed late probe
        // therefore retains every successfully-created detour for process
        // lifetime instead of attempting an unsafe rollback.  SMAA is gated
        // fail-open and static byte patches still get their normal verified
        // restore attempt.
        (void)RetainCreatedHooks();
        const bool smaa_quiesced = smaa::Shutdown();
        const bool static_patches_restored = engine_fixes::ShutdownStaticPatches();
        const bool smaa_hooks_retained =
            smaa::GetStats().any_hook_retained;
        if (smaa_hooks_retained) {
            g_minhook_retained_process_lifetime.store(true, std::memory_order_release);
        }
        const bool hooks_retained =
            !g_created_hook_targets.empty() || smaa_hooks_retained;
        if (!smaa_quiesced) {
            log::Error("SMAA teardown could not quiesce; D3D resources retained");
        }
        if (!static_patches_restored) {
            log::Error("static patch restore incomplete; patch ownership retained");
        }
        g_hooks_initialized.store(hooks_retained, std::memory_order_release);
        g_cleanup_pending.store(!static_patches_restored, std::memory_order_release);
        log::WarnF("hook_initialization_partial behavior=pass_through "
                   "retained_hook_count=%zu smaa_hook_retained=%d static_restore=%d",
                   g_created_hook_targets.size(),
                   smaa_hooks_retained ? 1 : 0,
                   static_patches_restored ? 1 : 0);
        return false;
    }

    g_cleanup_pending.store(false, std::memory_order_release);

    if (config.speed_rumble) {
        // Optional feature: a failure here must never abort mod initialization,
        // and it is placed on the committed-success path so a partial init
        // never leaves a process-lifetime input patch behind.
        //
        // XInputSetState is an IMPORT, so MinHook's image guard rejects it as a
        // target. The game's own IAT slot for it is rewritten instead, which is
        // inside the image and is what the loader would have done anyway.
        if (PatchXInputSetStateIat(reinterpret_cast<void*>(&DetourXInputSetState),
                                   reinterpret_cast<void**>(&g_xinput_set_state_original))) {
            log::InfoF("speed_rumble installed via IAT patch");
        } else {
            log::Warn("speed_rumble: XInputSetState IAT slot not found; feature disabled");
        }
    }

    if (config.hook_smaa_present) {
        // Initialize only prepared CPU state. Open DXGI discovery after every
        // game hook and static patch has committed, while mutating parent
        // detours are still behind the global pass-through gate.
        smaa::Activate();
    }
    // Publish the global gate last. Until this release every mutating detour is
    // pass-through, so it cannot observe half-published feature readiness.
    g_behavior_transaction_ready.store(true, std::memory_order_release);
    g_hooks_initialized.store(true);
    log::InfoF("hook_transaction_committed behavior=active installed_hook_count=%zu",
               g_created_hook_targets.size());
    return true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lifecycle_lock(g_hook_lifecycle_mutex);
    cut_content::Shutdown();
    if (!g_cleanup_pending.load(std::memory_order_acquire) &&
        !g_hooks_initialized.load() && g_created_hook_targets.empty() &&
        !engine_fixes::IsInitialized() &&
        !smaa::GetStats().any_hook_retained &&
        !g_minhook_retained_process_lifetime.load(std::memory_order_acquire)) {
        return;
    }

    // Retained detours must become transparent before any companion D3D or
    // static state starts tearing down.
    g_hook_creation_transaction_open = false;
    g_behavior_transaction_ready.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> wetness_lock(g_character_wetness_mutex);
        RestoreOwnedCharacterWetness(g_character_wetness_state);
        g_character_wetness_state = {};
    }
    {
        std::lock_guard<std::mutex> sweat_lock(g_character_sweat_mutex);
        RestoreOwnedCharacterSweat(g_character_sweat_state);
    }
    ResetNpcCharacterEffectsStates();

    // Gate SMAA and release only its D3D resources after active calls drain.
    // MinHook detours themselves are process-lifetime objects: MH_RemoveHook
    // frees their trampolines and can race a thread that has already loaded an
    // original pointer.  Main.cpp pins this module, so retaining the hooks is
    // both safer and bounded (the OS reclaims them at process exit).
    if (!smaa::Shutdown()) {
        log::Error("hook shutdown deferred until SMAA calls quiesce");
        g_cleanup_pending.store(true, std::memory_order_release);
        return;
    }
    (void)RetainCreatedHooks();
    if (!engine_fixes::ShutdownStaticPatches()) {
        log::Error("hook shutdown deferred because static patch restoration failed");
        g_cleanup_pending.store(true, std::memory_order_release);
        return;
    }
    ResetNisActorTrackerLocked();
    if (!g_cop_symbol_patch_registry.empty() &&
        !g_cop_symbol_patch_registry.RestoreAll()) {
        log::Error("cop_symbol_data restoration incomplete");
    }
    log::InfoF("pedestrian_density_summary frames=%llu stock_calls=%llu clamped_frames=%llu",
               g_pedestrian_throttle_frame_count.load(std::memory_order_relaxed),
               g_pedestrian_throttle_stock_call_count.load(std::memory_order_relaxed),
               g_pedestrian_throttle_clamped_frame_count.load(std::memory_order_relaxed));
    log::InfoF("average_window_summary initialized=%llu expanded=%llu",
               g_average_window_initialize_count.load(std::memory_order_relaxed),
               g_average_window_expanded_count.load(std::memory_order_relaxed));
    if (!g_created_hook_targets.empty() ||
        smaa::GetStats().any_hook_retained) {
        g_minhook_retained_process_lifetime.store(true, std::memory_order_release);
        g_hooks_initialized.store(true, std::memory_order_release);
    } else {
        g_hooks_initialized.store(false, std::memory_order_release);
    }
    g_cleanup_pending.store(false, std::memory_order_release);
}

}  // namespace spatch::hooks
