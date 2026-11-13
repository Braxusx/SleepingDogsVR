#include "BootstrapPolicy.h"

#include "InputPolicy.h"
#include "TextureFilteringPolicy.h"

namespace spatch {

Config BuildSafeCompatibilityConfig(Config config) {
    config.hook_queue_ready = false;
    config.hook_task_dispatch = false;
    config.hook_wait_helper = false;
    config.hook_scaleform_time = false;
    config.hook_scaleform_init = false;
    config.hook_nis_timing = false;
    config.hook_nis_runtime = false;
    config.hook_nis_owner = false;
    config.hook_nis_actor_state = false;
    config.hook_twitch_probe = false;
    config.hook_frameflow = false;
    config.hook_scenery_builders = false;
    config.hook_fog_slicing_guard = false;
    config.hook_aa_probe = false;
    config.hook_aa_fx_probe = false;
    config.aa_variant_debug_keys = false;
    config.aa_aux_debug_keys = false;
    config.hook_smaa_present = false;
    config.smaa_disable_stock_aa = false;
    config.hook_post_material_submit = false;
    config.hook_character_regression_probe = false;
    config.override_rumble_enabled = -1;
    config.force_raw_mouse_input = false;
    config.disable_camera_smoothing = false;
    config.gta_iv_car_camera = false;
    config.gta_iv_bike_camera = false;
    config.disable_controller_camera_autocenter = false;
    config.disable_onfoot_camera_autocenter = false;
    config.gtav_speed_shake = false;
    // Settings-only compatibility mode cannot install the clock detour.
    config.gta_vi_time_clock = false;
    config.controller_left_stick_deadzone = input::kStockDeadzone;
    config.controller_right_stick_deadzone = input::kStockDeadzone;
    config.fix_cutscene_zero_dt = false;
    config.fix_cutscene_scene_time_step = false;
    config.fix_nis_actor_restore_duplicates = false;
    config.smaa_enable = false;
    config.smaa_debug_keys = false;
    config.restore_original_fog = false;
    config.restore_character_wetness = false;
    config.restore_character_sweat = false;
    // Settings-only compatibility mode cannot install the speech chain.
    config.restore_push_voice_lines = false;
    config.restore_combat_lines = false;
    config.restore_combat_lines_all_voices = false;
    config.restore_police_voice_lines = false;
    config.restore_encourage_voice_lines = false;
    config.driving_swear_lines = false;
    config.cop_pursuit_swear_lines = false;
    config.diag_push_voice_line_events = false;
    config.diag_combat_line_events = false;
    config.diag_npc_voice_line_events = false;
    config.restore_original_eye_reflections = false;
    config.fix_pedestrian_density_at_high_fps = false;
    config.fix_high_fps_average_windows = false;
    config.improve_spherical_reflections = false;
    config.anisotropic_filtering = texture_filtering::kOriginalAnisotropy;
    config.force_anisotropic_filtering = false;
    config.remove_hidden_120_fps_cap = false;
    config.fix_first_run_resolution = false;
    config.fix_scaleform_qpc_clock = false;
    config.fix_file_timestamp_open_mode = false;
    config.fix_audio_file_open = false;
    config.fix_large_file_sizes = false;
    config.fix_vram_pool_lock = false;
    config.fix_vram_capacity_reporting = false;
    config.fix_resource_loading = false;
    config.fix_contact_list_overflow = false;
    config.fix_corrupt_save_handling = false;
    config.fix_thread_creation_failure = false;
    config.fix_qfile_ready_sleep_yield = false;
    config.fix_combat_list_selection = false;
    config.aa_variant_mode = 0;
    config.aa_aux_mode = 0;
    return config;
}

Config BuildLatestSteamCompatibilityConfig(Config config) {
    // Only hooks with independently mapped latest-Steam RVAs may survive this
    // profile. Falling through to a legacy RVA is worse than losing optional
    // diagnostics because it can detour an unrelated function.
    //
    // Wave 2 (2026-10-30): scaleform, SMAA/stock-AA, the camera behaviors,
    // chase-aim, sweat, HUD/radar, heat meter, DOF, slow-mo, and the
    // online-probe/activity-timer fixes are remapped (site-evidence-wave2.md)
    // and their config keys apply as written here. Still fail-closed below:
    // the diag hooks, NIS actor-state work, post-material-submit, and the
    // rumble override.
    config.hook_wait_helper = false;
    config.hook_nis_runtime = false;
    config.hook_character_regression_probe = false;
    config.hook_nis_actor_state = false;
    config.hook_twitch_probe = false;
    config.hook_scenery_builders = false;
    config.hook_aa_probe = false;
    config.hook_aa_fx_probe = false;
    config.aa_variant_debug_keys = false;
    config.aa_aux_debug_keys = false;
    config.hook_post_material_submit = false;
    config.fix_nis_actor_restore_duplicates = false;
    config.override_rumble_enabled = -1;
    config.aa_variant_mode = 0;
    config.aa_aux_mode = 0;
    return config;
}

bool IsUsableBootstrapModulePath(
    const std::filesystem::path& module_path) {
    if (module_path.empty() || !module_path.is_absolute() ||
        !module_path.has_filename()) {
        return false;
    }
    const std::filesystem::path module_directory = module_path.parent_path();
    return !module_directory.empty() && module_directory.is_absolute();
}

HookInstallPlan BuildHookInstallPlan(const Config& config, const BuildCheckResult& build) {
    HookInstallPlan plan{};
    plan.effective_config = config;

    if (!build.supported && !config.allow_unverified_build) {
        return plan;
    }

    if (ShouldUseSafeCompatibilityMode(build, config.allow_unverified_build)) {
        plan.safe_compatibility_mode = true;
        plan.effective_config = BuildSafeCompatibilityConfig(plan.effective_config);
        return plan;
    }

    plan.install_hooks = true;

    if (build.build_id == "latest_steam") {
        plan.latest_steam_profile = true;
        plan.effective_config = BuildLatestSteamCompatibilityConfig(plan.effective_config);
    }

    return plan;
}

BootstrapPrepareResult PrepareBootstrapHooks(const Config& config,
                                             const BuildCheckResult& build,
                                             bool detaching_after_inspect) {
    BootstrapPrepareResult result{};
    result.effective_config = config;

    if (!config.enabled) {
        result.status = BootstrapPrepareStatus::DisabledByConfig;
        return result;
    }

    if (detaching_after_inspect) {
        result.status = BootstrapPrepareStatus::DetachedBeforeHooks;
        return result;
    }

    const HookInstallPlan plan = BuildHookInstallPlan(config, build);
    result.effective_config = plan.effective_config;
    result.safe_compatibility_mode = plan.safe_compatibility_mode;
    result.latest_steam_profile = plan.latest_steam_profile;
    if (plan.safe_compatibility_mode) {
        result.status = BootstrapPrepareStatus::ReadyForSettingsOnly;
    } else {
        result.status = plan.install_hooks
                            ? BootstrapPrepareStatus::ReadyForHookInitialization
                            : BootstrapPrepareStatus::UnsupportedBuild;
    }
    return result;
}

bool ShouldApplyDisplaySettings(BootstrapPrepareStatus status) {
    return status == BootstrapPrepareStatus::ReadyForHookInitialization ||
           status == BootstrapPrepareStatus::ReadyForSettingsOnly;
}

BootstrapFinalizeStatus FinalizeBootstrapHooks(bool hook_initialize_succeeded) {
    return hook_initialize_succeeded ? BootstrapFinalizeStatus::Initialized
                                     : BootstrapFinalizeStatus::HookInitializationFailed;
}

}  // namespace spatch
