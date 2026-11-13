#pragma once

#include <cstdint>

namespace spatch::character_hooks {

struct HookRequest {
    bool restore_wetness = false;
    bool restore_sweat = false;
    bool restore_npc_voice_lines = false;
    bool restore_push_voice_lines = false;
    bool restore_combat_lines = false;
    bool regression_probe = false;
    bool wetness_field_probe = false;
    bool sweat_field_probe = false;
    // The slow-motion poll rides the character-effects update tick, so any
    // slow-motion feature must keep that detour installed or the per-frame
    // clamp/probe is blind on every frame the engine's time-dilate apply
    // point does not run.
    bool slow_mo_probe = false;
    bool slow_mo_suppress = false;
};

struct HookPlan {
    bool install_effects_hooks = false;
    bool install_health_damage_hook = false;
    bool install_regression_hooks = false;
    bool require_simobject_component = false;
    bool use_sweat_only_health_fast_path = false;
};

[[nodiscard]] constexpr HookPlan BuildHookPlan(const HookRequest& request) noexcept {
    HookPlan plan{};
    plan.install_effects_hooks =
        request.regression_probe || request.wetness_field_probe || request.sweat_field_probe ||
        request.restore_wetness || request.restore_sweat ||
        request.slow_mo_probe || request.slow_mo_suppress;
    plan.install_health_damage_hook =
        request.regression_probe || request.restore_sweat ||
        request.restore_npc_voice_lines || request.restore_push_voice_lines ||
        request.restore_combat_lines;
    plan.install_regression_hooks = request.regression_probe;
    plan.require_simobject_component =
        plan.install_effects_hooks || plan.install_health_damage_hook;
    plan.use_sweat_only_health_fast_path =
        request.restore_sweat && !request.regression_probe;
    return plan;
}

// SimComponent base fields (see ReadSimObjectFromComponent in Hooks.cpp):
// m_TypeUID at +0x18, m_pSimObject at +0x28. Every component lookup in this
// file is validated against these two fields before use.
inline constexpr std::uintptr_t kSimComponentTypeUidOffset = 0x18;
inline constexpr std::uintptr_t kSimComponentSimObjectOffset = 0x28;

// Component type UIDs resolved through SimObject::GetComponent on the
// character path. Named here so the cached-binding revalidation in Hooks.cpp
// cannot drift from the resolve sites.
inline constexpr std::uint32_t kPhysicsComponentTypeUid = 0x7A000001u;
inline constexpr std::uint32_t kCharacterLookComponentTypeUid = 0xCC000001u;

// A cached look/physics binding is usable only while it still carries the
// expected type UID and still belongs to the expected SimObject.
// Spawn/teardown bursts (e.g. the fight-club pre-fight population) recycle
// pooled components between ticks, so a binding that fails either check must
// be re-resolved, never trusted. An unknown binding is never filled in with
// a guess: expected_sim_object == 0 is always invalid.
[[nodiscard]] constexpr bool IsComponentBindingValid(
    std::uint32_t actual_type_uid,
    std::uint32_t expected_type_uid,
    std::uintptr_t actual_sim_object,
    std::uintptr_t expected_sim_object) noexcept {
    return expected_sim_object != 0 &&
           actual_sim_object == expected_sim_object &&
           actual_type_uid == expected_type_uid;
}

}  // namespace spatch::character_hooks
