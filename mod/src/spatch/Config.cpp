#include "AgxPolicy.h"
#include "Config.h"

#include "FrameLimiterPolicy.h"

#include "InputPolicy.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace spatch {
namespace {

constexpr wchar_t kSection[] = L"SPatch";
constexpr wchar_t kCutsceneSection[] = L"Cutscenes";
constexpr wchar_t kDisplaySection[] = L"Display";
constexpr wchar_t kGraphicsSection[] = L"Graphics";
constexpr wchar_t kTextureFilteringSection[] = L"TextureFiltering";
constexpr wchar_t kRendererSection[] = L"Renderer";
constexpr wchar_t kShadowSection[] = L"Shadows";
constexpr wchar_t kAaSection[] = L"AntiAliasing";
constexpr wchar_t kToneSection[] = L"Tonemapping";
constexpr wchar_t kAoSection[] = L"AmbientOcclusion";
constexpr wchar_t kGiSection[] = L"GlobalIllumination";
constexpr wchar_t kPbrSection[] = L"PhysicallyBasedRendering";
constexpr wchar_t kSssSection[] = L"SubsurfaceScattering";
constexpr wchar_t kMaterialScatteringSection[] = L"MaterialScattering";
constexpr wchar_t kInputSection[] = L"Input";
constexpr wchar_t kGameplaySection[] = L"Gameplay";
constexpr wchar_t kStabilitySection[] = L"Stability";
constexpr wchar_t kAdvancedSection[] = L"Advanced";
constexpr wchar_t kDiagnosticsSection[] = L"Diagnostics";
constexpr wchar_t kDebugSection[] = L"Debug";
constexpr unsigned long kMaxSummaryIntervalMs = 60UL * 60UL * 1000UL;
constexpr unsigned long kMaxVerboseEvents = 100000UL;
constexpr unsigned long kMaxUniqueCallbacks = 100000UL;
std::mutex g_config_write_mutex;
std::atomic<unsigned long> g_config_write_serial = 0;

struct FileIdentity {
    DWORD volume_serial = 0;
    DWORD file_index_high = 0;
    DWORD file_index_low = 0;
    DWORD size_high = 0;
    DWORD size_low = 0;
    FILETIME last_write_time{};

    bool operator==(const FileIdentity& other) const noexcept {
        return volume_serial == other.volume_serial &&
            file_index_high == other.file_index_high &&
            file_index_low == other.file_index_low &&
            size_high == other.size_high && size_low == other.size_low &&
            last_write_time.dwLowDateTime ==
                other.last_write_time.dwLowDateTime &&
            last_write_time.dwHighDateTime ==
                other.last_write_time.dwHighDateTime;
    }
};

bool ReadFileIdentity(const std::filesystem::path& path,
                      FileIdentity& identity) noexcept {
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    const bool succeeded =
        GetFileInformationByHandle(file, &information) != FALSE;
    CloseHandle(file);
    if (!succeeded) {
        return false;
    }
    identity.volume_serial = information.dwVolumeSerialNumber;
    identity.file_index_high = information.nFileIndexHigh;
    identity.file_index_low = information.nFileIndexLow;
    identity.size_high = information.nFileSizeHigh;
    identity.size_low = information.nFileSizeLow;
    identity.last_write_time = information.ftLastWriteTime;
    return true;
}

struct IniKeyAlias {
    std::wstring_view legacy_key;
    const wchar_t* section;
    const wchar_t* user_key;
};

constexpr std::array kIniKeyAliases{
    IniKeyAlias{L"config_version", kSection, L"ConfigVersion"},
    IniKeyAlias{L"enabled", kSection, L"Enabled"},
    IniKeyAlias{L"allow_unverified_build", kSection, L"AllowUnverifiedBuild"},
    IniKeyAlias{L"allow_unsupported_build", kSection, L"AllowUnsupportedBuild"},
    IniKeyAlias{L"write_minidumps", kDebugSection, L"WriteCrashDumps"},
    IniKeyAlias{L"enable_logging", kDebugSection, L"Logging"},
    IniKeyAlias{L"cutscene_fps", kCutsceneSection, L"CutsceneFPS"},
    IniKeyAlias{L"fix_cutscene_zero_dt", kCutsceneSection, L"FixCutsceneFPS"},
    // The timeline rewrite is no longer part of the FixCutsceneFPS master: it
    // is the repair that deforms props, so it has to be a deliberate choice.
    IniKeyAlias{L"fix_cutscene_scene_time_step", kCutsceneSection, L"FixCutsceneSceneTime"},
    IniKeyAlias{L"override_cutscene_zero_dt", kCutsceneSection, L"FixCutsceneZeroDT"},
    IniKeyAlias{L"override_cutscene_scene_time", kCutsceneSection, L"FixCutsceneSceneTime"},
    IniKeyAlias{L"fix_nis_actor_restore_duplicates", kCutsceneSection, L"FixDuplicateCutsceneActorRestore"},
    IniKeyAlias{L"disable_time_step_smoothing", kCutsceneSection, L"DisableTimeStepSmoothing"},
    IniKeyAlias{L"time_step_smoothing_frames", kCutsceneSection, L"TimeStepSmoothingFrames"},
    IniKeyAlias{L"time_step_smoothing", kCutsceneSection, L"TimeStepSmoothing"},
    IniKeyAlias{L"force_raw_mouse_input", kInputSection, L"ForceRawMouseInput"},
    IniKeyAlias{L"disable_camera_smoothing", kInputSection, L"DisableCameraSmoothing"},
    IniKeyAlias{L"gta_iv_car_camera", kInputSection, L"GTAIVCarCamera"},
    IniKeyAlias{L"disable_pursuit_camera", kInputSection, L"DisablePursuitCamera"},
    IniKeyAlias{L"fix_chase_aim_camera", kInputSection, L"FixChaseAimCamera"},
    IniKeyAlias{L"hide_cop_radar_icons", kGameplaySection, L"HideCopRadarIcons"},
    IniKeyAlias{L"modern_brake_reverse", kGameplaySection, L"ModernBrakeReverse"},
    IniKeyAlias{L"no_wheel_turn_back", kGameplaySection, L"NoWheelTurnBack"},
    IniKeyAlias{L"gta_iv_bike_camera", kInputSection, L"GTAIVBikeCamera"},
    IniKeyAlias{L"disable_controller_camera_autocenter", kInputSection, L"DisableControllerCameraAutocenter"},
    IniKeyAlias{L"disable_onfoot_camera_autocenter", kInputSection, L"DisableOnFootCameraAutocenter"},
    IniKeyAlias{L"gtav_speed_shake", kInputSection, L"GTAVSpeedShake"},
    IniKeyAlias{L"gtav_speed_shake_strength", kInputSection,
                L"GTAVSpeedShakeStrength"},
    IniKeyAlias{L"gtav_speed_shake_start_speed_kmh", kInputSection,
                L"GTAVSpeedShakeStartSpeedKmh"},
    IniKeyAlias{L"gtav_speed_shake_full_speed_kmh", kInputSection,
                L"GTAVSpeedShakeFullSpeedKmh"},
    IniKeyAlias{L"speed_rumble", kInputSection, L"SpeedRumble"},
    IniKeyAlias{L"speed_rumble_start_kmh", kInputSection, L"SpeedRumbleStartKmh"},
    IniKeyAlias{L"speed_rumble_full_kmh", kInputSection, L"SpeedRumbleFullKmh"},
    IniKeyAlias{L"speed_rumble_strength_percent", kInputSection,
                L"SpeedRumbleStrengthPercent"},
    IniKeyAlias{L"gta_vi_time_clock", kGameplaySection, L"GTAVITimeClock"},
    IniKeyAlias{L"gta_vi_time_clock_day_minutes", kGameplaySection,
                L"GTAVITimeClockDayMinutes"},
    IniKeyAlias{L"diag_onfoot_yaw_probe_events", kDebugSection, L"OnFootYawProbeEvents"},
    IniKeyAlias{L"diag_fov_probe", kDebugSection, L"FovProbe"},
    IniKeyAlias{L"diag_blip_probe", kDebugSection, L"BlipProbe"},
    IniKeyAlias{L"diag_drivetrain_probe", kDebugSection, L"DrivetrainProbe"},
    IniKeyAlias{L"diag_stimulus_events", kDebugSection, L"StimulusProbe"},
    IniKeyAlias{L"stimulus_init_emitter_probe", kDiagnosticsSection,
                 L"StimulusInitEmitterProbe"},
    IniKeyAlias{L"controller_left_stick_deadzone", kInputSection, L"LeftStickDeadzone"},
    IniKeyAlias{L"controller_right_stick_deadzone", kInputSection, L"RightStickDeadzone"},
    IniKeyAlias{L"hook_fog_slicing_guard", kStabilitySection, L"FixFogSlicing"},
    IniKeyAlias{L"min_fog_slicing_interval", kStabilitySection, L"MinimumFogSlicingInterval"},
    IniKeyAlias{L"pool_headroom_mib", kStabilitySection, L"PoolHeadroomMiB"},
    IniKeyAlias{L"warn_low_res_buffer", kDisplaySection, L"WarnLowResolutionBuffer"},
    IniKeyAlias{L"restore_original_fog", kGraphicsSection, L"RestoreOriginalFogAndNeon"},
    IniKeyAlias{L"disable_dof", kGraphicsSection, L"DisableDOF"},
    IniKeyAlias{L"disable_cutscene_dof", kGraphicsSection, L"DisableCutsceneDOF"},
    IniKeyAlias{L"restore_original_eye_reflections", kGraphicsSection, L"RestoreOriginalEyeReflections"},
    IniKeyAlias{L"restore_character_wetness", kGraphicsSection, L"RestoreWetness"},
    IniKeyAlias{L"restore_character_sweat", kGraphicsSection, L"RestoreSweat"},
    IniKeyAlias{L"restore_push_voice_lines", kGameplaySection, L"RestorePushVoiceLines"},
    IniKeyAlias{L"restore_push_voice_lines_free_roam_only", kGameplaySection,
                L"PushVoiceLinesFreeRoamOnly"},
    IniKeyAlias{L"restore_combat_lines", kGameplaySection, L"RestoreCombatLines"},
    IniKeyAlias{L"restore_combat_lines_free_roam_only", kGameplaySection, L"CombatLinesFreeRoamOnly"},
    IniKeyAlias{L"restore_combat_lines_all_voices", kGameplaySection, L"RestoreCombatLinesAllVoices"},
    IniKeyAlias{L"driving_swear_lines", kGameplaySection, L"DrivingSwearLines"},
    IniKeyAlias{L"driving_swear_lines_free_roam_only", kGameplaySection,
                L"DrivingSwearLinesFreeRoamOnly"},
    // Canonical public key. Both earlier spellings remain read-compatible
    // aliases so existing installs keep working.
    IniKeyAlias{L"cop_pursuit_lines", kGameplaySection, L"CopPursuitLines"},
    IniKeyAlias{L"cop_pursuit_lines_free_roam_only", kGameplaySection,
                L"CopPursuitLinesFreeRoamOnly"},
    IniKeyAlias{L"swear_lines_free_roam_only", kGameplaySection,
                L"SwearLinesFreeRoamOnly"},
    IniKeyAlias{L"disable_driveby_slow_mo", kGameplaySection, L"DisableDrivebySlowMo"},
    IniKeyAlias{L"disable_stunt_slow_mo", kGameplaySection, L"DisableStuntSlowMo"},
    IniKeyAlias{L"disable_stunt_sound_fx", kGameplaySection, L"DisableStuntSoundFx"},
    IniKeyAlias{L"disable_stunt_screen_fx", kGameplaySection, L"DisableStuntScreenFx"},
    IniKeyAlias{L"cops_persuit_lines", kGameplaySection, L"CopsPersuitLines"},
    IniKeyAlias{L"cop_pursuit_swear_lines", kGameplaySection, L"CopPursuitSwearLines"},
    IniKeyAlias{L"restore_police_voice_lines", kGameplaySection, L"RestorePoliceVoiceLines"},
    IniKeyAlias{L"restore_encourage_voice_lines", kGameplaySection, L"RestoreEncourageVoiceLines"},
    IniKeyAlias{L"wetness_full_time_seconds", kGraphicsSection, L"WetnessFullTime"},
    IniKeyAlias{L"wetness_fade_time_seconds", kGraphicsSection, L"WetnessFadeTime"},
    IniKeyAlias{L"sweat_build_time_seconds", kGraphicsSection, L"SweatBuildTime"},
    IniKeyAlias{L"sweat_fade_time_seconds", kGraphicsSection, L"SweatFadeTime"},
    IniKeyAlias{L"sweat_onset_time_seconds", kGraphicsSection, L"SweatOnsetTime"},
    IniKeyAlias{L"sweat_run_speed", kGraphicsSection, L"SweatRunSpeed"},
    IniKeyAlias{L"sweat_combat_time_seconds", kGraphicsSection, L"SweatCombatTime"},
    IniKeyAlias{L"fix_pedestrian_density_at_high_fps", kGraphicsSection, L"FixPedestrianDensity"},
    IniKeyAlias{L"fix_high_fps_average_windows", kGraphicsSection, L"FixHighFPSAverages"},
    IniKeyAlias{L"improve_spherical_reflections", kGraphicsSection, L"ImproveSphericalReflections"},
    IniKeyAlias{L"spherical_reflection_width", kGraphicsSection, L"SphericalReflectionWidth"},
    IniKeyAlias{L"fix_vram_capacity_reporting", kGraphicsSection, L"FixVRAMReporting"},
    IniKeyAlias{L"remove_hidden_120_fps_cap", kDisplaySection, L"Remove120FPSCap"},
    IniKeyAlias{L"fix_first_run_resolution", kDisplaySection, L"FixFirstRunResolution"},
    IniKeyAlias{L"fix_scaleform_qpc_clock", kStabilitySection, L"FixScaleformTimerOverflow"},
    IniKeyAlias{L"fix_file_timestamp_open_mode", kStabilitySection, L"FixFileTimestampUpdates"},
    IniKeyAlias{L"fix_audio_file_open", kStabilitySection, L"FixAudioFileOpen"},
    IniKeyAlias{L"fix_large_file_sizes", kStabilitySection, L"FixLargeFileSizes"},
    IniKeyAlias{L"fix_vram_pool_lock", kStabilitySection, L"FixVRAMPoolLock"},
    IniKeyAlias{L"fix_resource_loading", kStabilitySection, L"FixResourceLoading"},
    IniKeyAlias{L"fix_contact_list_overflow", kStabilitySection, L"FixContactListOverflow"},
    IniKeyAlias{L"fix_corrupt_save_handling", kStabilitySection, L"FixCorruptSaveCrash"},
    IniKeyAlias{L"fix_thread_creation_failure", kStabilitySection, L"FixThreadCreationFailure"},
    IniKeyAlias{L"fix_qfile_ready_sleep_yield", kStabilitySection, L"FixFileLoadCoreSpin"},
    IniKeyAlias{L"fix_combat_list_selection", kStabilitySection, L"FixCombatListSortChurn"},
    IniKeyAlias{L"fix_online_probe_stall", kStabilitySection, L"FixOnlineProbeStall"},
    IniKeyAlias{L"fix_frame_counted_activity_timer", kStabilitySection,
                L"FixFrameCountedActivityTimer"},
    IniKeyAlias{L"prefer_max_refresh_rate", kDisplaySection, L"UseMaximumRefreshRate"},
    IniKeyAlias{L"override_fullscreen", kDisplaySection, L"Fullscreen"},
    IniKeyAlias{L"custom_frame_limiter", kDisplaySection, L"CustomFrameLimiter"},
    IniKeyAlias{L"custom_frame_limiter_fps", kDisplaySection,
                L"CustomFrameLimiterFPS"},
    IniKeyAlias{L"override_low_res_buffer", kDisplaySection, L"LowResolutionBuffer"},
    IniKeyAlias{L"override_shadow_filter", kShadowSection, L"OriginalShadowFilter"},
    IniKeyAlias{L"shadow_resolution", kShadowSection, L"ShadowResolution"},
    IniKeyAlias{L"override_texture_detail_level", kGraphicsSection, L"TextureDetail"},
    IniKeyAlias{L"anisotropic_filtering", kTextureFilteringSection, L"AnisotropicFiltering"},
    IniKeyAlias{L"force_anisotropic_filtering", kTextureFilteringSection, L"ForceAnisotropicFiltering"},
    IniKeyAlias{L"override_motion_blur", kGraphicsSection, L"MotionBlur"},
    IniKeyAlias{L"override_world_density", kGameplaySection, L"WorldDensity"},
    IniKeyAlias{L"override_rumble_enabled", kGameplaySection, L"Rumble"},
    IniKeyAlias{L"hook_smaa_present", kAaSection, L"SMAA"},
    IniKeyAlias{L"smaa_enable", kAaSection, L"SMAA"},
    IniKeyAlias{L"smaa_preset", kAaSection, L"SMAAPreset"},
    IniKeyAlias{L"smaa_edge_threshold", kAaSection, L"SMAAEdgeThreshold"},
    IniKeyAlias{L"icon_stabilizer", kAaSection, L"IconStabilizer"},
    IniKeyAlias{L"dynamic_radar", kGameplaySection, L"DynamicRadar"},
    IniKeyAlias{L"hide_world_markers_probe", kDebugSection, L"HideWorldMarkersProbe"},
    IniKeyAlias{L"probe_action_requests", kDebugSection, L"ActionRequestProbe"},
    IniKeyAlias{L"probe_slow_mo", kDebugSection, L"SlowMoProbe"},
    IniKeyAlias{L"probe_screen_filter", kDebugSection, L"ScreenFilterProbe"},
    IniKeyAlias{L"agx_enable", kToneSection, L"AgX"},
    IniKeyAlias{L"agx_look", kToneSection, L"AgXLook"},
    IniKeyAlias{L"agx_strength_percent", kToneSection, L"AgXStrength"},
    IniKeyAlias{L"agx_exposure_scale_percent", kToneSection, L"AgXExposure"},
    IniKeyAlias{L"agx_debug_bloom", kToneSection, L"AgxDebugBloom"},
    IniKeyAlias{L"summary_interval_ms", kDiagnosticsSection, L"SummaryInterval"},
    IniKeyAlias{L"max_verbose_events", kDiagnosticsSection, L"VerboseEvents"},
    IniKeyAlias{L"max_unique_callbacks", kDiagnosticsSection, L"UniqueCallbacks"},
    IniKeyAlias{L"diag_push_voice_line_events", kDebugSection, L"PushVoiceLineEvents"},
    IniKeyAlias{L"diag_combat_line_events", kDebugSection, L"CombatLineEvents"},
    IniKeyAlias{L"diag_npc_voice_line_events", kDebugSection, L"NpcVoiceLineEvents"},
    IniKeyAlias{L"push_voice_line_voice_id", kDebugSection, L"PushVoiceLineVoiceId"},
    IniKeyAlias{L"push_voice_line_reaction_id", kDebugSection, L"PushVoiceLineReactionId"},
    IniKeyAlias{L"push_voice_line_voice_id_sweep", kDebugSection, L"PushVoiceLineVoiceIdSweep"},
    IniKeyAlias{L"headlight_shadows", kShadowSection, L"HeadlightShadows"},
    IniKeyAlias{L"siren_shadows", kShadowSection, L"SirenShadows"},
    // v70 renamed the round75 keys: the feature is an enhancement, not a
    // restoration. The v69 spellings stay readable.
    IniKeyAlias{L"restore_headlight_shadows", kShadowSection, L"HeadlightShadows"},
    IniKeyAlias{L"restore_siren_shadows", kShadowSection, L"SirenShadows"},
    IniKeyAlias{L"headlight_beam_light", kShadowSection, L"HeadlightBeamLight"},
    IniKeyAlias{L"headlight_beam_radius", kShadowSection, L"HeadlightBeamRadius"},
    IniKeyAlias{L"headlight_beam_color_r", kShadowSection, L"HeadlightBeamColorR"},
    IniKeyAlias{L"headlight_beam_color_g", kShadowSection, L"HeadlightBeamColorG"},
    IniKeyAlias{L"headlight_beam_color_b", kShadowSection, L"HeadlightBeamColorB"},
    IniKeyAlias{L"headlight_shadow_light_type", kShadowSection, L"HeadlightShadowLightType"},
    IniKeyAlias{L"siren_shadow_light_type", kShadowSection, L"SirenShadowLightType"},
    IniKeyAlias{L"diag_light_submit_events", kDebugSection, L"LightSubmitEvents"},
    IniKeyAlias{L"diag_census_shadow_consumers", kDebugSection, L"CensusShadowConsumers"},
};

const IniKeyAlias* FindIniKeyAlias(const wchar_t* key) {
    const std::wstring_view value(key == nullptr ? L"" : key);
    const auto it = std::find_if(
        kIniKeyAliases.begin(), kIniKeyAliases.end(), [value](const IniKeyAlias& alias) {
            return alias.legacy_key == value;
        });
    return it == kIniKeyAliases.end() ? nullptr : &*it;
}

const wchar_t* SectionForKey(const wchar_t* key) {
    const std::wstring_view value(key == nullptr ? L"" : key);
    if (value == L"enabled" || value == L"allow_unverified_build" ||
        value == L"allow_unsupported_build" ||
        value == L"config_version") {
        return kSection;
    }
    if (value == L"write_minidumps") {
        return kDebugSection;
    }
    if (value.find(L"cutscene") != std::wstring_view::npos ||
        value == L"disable_time_step_smoothing" || value == L"min_fog_slicing_interval" ||
        value == L"hook_fog_slicing_guard" ||
        value == L"time_step_smoothing_frames" || value == L"time_step_smoothing" ||
        value == L"fix_cutscene_zero_dt" || value == L"fix_cutscene_scene_time_step") {
        return kCutsceneSection;
    }
    if (value == L"force_raw_mouse_input" || value == L"disable_camera_smoothing" ||
        value == L"gta_iv_car_camera" || value == L"gta_iv_bike_camera" ||
    value == L"disable_pursuit_camera" || value == L"fix_chase_aim_camera" ||
    value == L"hide_cop_radar_icons" ||
    value == L"modern_brake_reverse" ||
    value == L"no_wheel_turn_back" ||
        value == L"disable_controller_camera_autocenter" ||
        value == L"disable_onfoot_camera_autocenter" ||
        value == L"gtav_speed_shake" ||
        value == L"gtav_speed_shake_strength" ||
        value == L"gtav_speed_shake_start_speed_kmh" ||
        value == L"gtav_speed_shake_full_speed_kmh" ||
        value == L"speed_rumble" ||
        value == L"speed_rumble_start_kmh" ||
        value == L"speed_rumble_full_kmh" ||
        value == L"speed_rumble_strength_percent" ||
        value == L"controller_left_stick_deadzone" ||
        value == L"controller_right_stick_deadzone") {
        return kInputSection;
    }
    if (value == L"restore_original_fog" ||
        value == L"disable_dof" ||
        value == L"disable_cutscene_dof" ||
        value == L"restore_original_eye_reflections" ||
        value == L"restore_character_wetness" ||
        value == L"restore_character_sweat" ||
        value == L"wetness_full_time_seconds" ||
        value == L"wetness_fade_time_seconds" ||
        value == L"sweat_build_time_seconds" ||
        value == L"sweat_fade_time_seconds" ||
        value == L"sweat_onset_time_seconds" ||
        value == L"sweat_run_speed" ||
        value == L"sweat_combat_time_seconds" ||
        value == L"fix_high_fps_average_windows" ||
        value == L"fix_pedestrian_density_at_high_fps" ||
        value == L"improve_spherical_reflections" ||
        value == L"spherical_reflection_width") {
        return kGraphicsSection;
    }
    if (value == L"shadow_resolution" ||
        value == L"override_shadow_filter" ||
        value == L"headlight_shadows" ||
        value == L"siren_shadows" ||
        value == L"headlight_beam_light" ||
        value == L"headlight_beam_radius" ||
        value == L"headlight_beam_color_r" ||
        value == L"headlight_beam_color_g" ||
        value == L"headlight_beam_color_b" ||
        value == L"restore_headlight_shadows" ||
        value == L"restore_siren_shadows" ||
        value == L"headlight_shadow_light_type" ||
        value == L"siren_shadow_light_type") {
        return kShadowSection;
    }
    if (value == L"anisotropic_filtering" ||
        value == L"force_anisotropic_filtering") {
        return kTextureFilteringSection;
    }
    if (value.starts_with(L"override_") || value == L"prefer_max_refresh_rate" ||
        value == L"remove_hidden_120_fps_cap" || value == L"warn_low_res_buffer" ||
        value == L"custom_frame_limiter" || value == L"custom_frame_limiter_fps") {
        return kDisplaySection;
    }
    if (value.starts_with(L"smaa")) {
        return kAaSection;
    }
    if (value == L"fix_corrupt_save_handling" ||
        value == L"fix_thread_creation_failure" ||
        value == L"fix_qfile_ready_sleep_yield" ||
        value == L"fix_combat_list_selection" ||
        value == L"fix_online_probe_stall" ||
        value == L"fix_frame_counted_activity_timer" ||
        value == L"pool_headroom_mib") {
        return kStabilitySection;
    }
    if (value == L"restore_push_voice_lines" ||
        value == L"restore_combat_lines" ||
        value == L"restore_combat_lines_all_voices" ||
        value == L"driving_swear_lines" ||
        value == L"driving_swear_lines_free_roam_only" ||
        value == L"cop_pursuit_lines" ||
        value == L"cop_pursuit_lines_free_roam_only" ||
        value == L"swear_lines_free_roam_only" ||
        value == L"disable_driveby_slow_mo" ||
        value == L"disable_stunt_slow_mo" ||
        value == L"disable_stunt_sound_fx" ||
        value == L"disable_stunt_screen_fx" ||
        value == L"cops_persuit_lines" ||
        value == L"cop_pursuit_swear_lines" ||
        value == L"restore_police_voice_lines" ||
        value == L"restore_encourage_voice_lines" ||
        value == L"restore_wei_voice_lines" ||
        value == L"gta_vi_time_clock" ||
        value == L"gta_vi_time_clock_day_minutes") {
        return kGameplaySection;
    }
    if (value == L"diag_push_voice_line_events" ||
        value == L"diag_combat_line_events" ||
        value == L"diag_npc_voice_line_events" ||
        value == L"diag_wei_voice_line_events" ||
        value == L"diag_onfoot_yaw_probe_events" || value == L"diag_fov_probe" ||
    value == L"diag_blip_probe" ||
        value == L"diag_drivetrain_probe" ||
        value == L"diag_stimulus_events" ||
        value == L"push_voice_line_voice_id" ||
        value == L"push_voice_line_voice_id_sweep" ||
        value == L"push_voice_line_reaction_id" ||
        value == L"probe_action_requests" ||
        value == L"probe_slow_mo" ||
        value == L"probe_screen_filter" ||
        value == L"diag_light_submit_events" ||
        value == L"diag_census_shadow_consumers") {
        return kDebugSection;
    }
    if (value == L"stimulus_init_emitter_probe" ||
        value == L"summary_interval_ms" || value == L"max_verbose_events" ||
        value == L"max_unique_callbacks") {
        return kDiagnosticsSection;
    }
    return kAdvancedSection;
}

constexpr wchar_t kMissingIniValueSentinel[] =
    L"{SPatch-key-not-present-6C46D9C1}";

bool HasExactIniKey(const std::filesystem::path& path,
                    const wchar_t* section,
                    const wchar_t* key) {
    wchar_t buffer[256]{};
    const DWORD length = GetPrivateProfileStringW(
        section, key, kMissingIniValueSentinel, buffer,
        static_cast<DWORD>(std::size(buffer)), path.c_str());
    return length != std::size(kMissingIniValueSentinel) - 1 ||
           std::wstring_view(buffer, length) !=
               std::wstring_view(kMissingIniValueSentinel);
}

std::optional<std::wstring> ReadRawValue(const std::filesystem::path& path,
                                         const wchar_t* section,
                                         const wchar_t* key) {
    wchar_t buffer[256]{};
    const DWORD length = GetPrivateProfileStringW(
        section, key, kMissingIniValueSentinel, buffer,
        static_cast<DWORD>(std::size(buffer)), path.c_str());
    if (length >= std::size(buffer) - 1) {
        return std::nullopt;
    }
    if (length == std::size(kMissingIniValueSentinel) - 1 &&
        std::wstring_view(buffer, length) ==
            std::wstring_view(kMissingIniValueSentinel)) {
        return std::nullopt;
    }
    // A present empty value now remains an engaged optional so canonical
    // malformed input reaches its parser fallback instead of leaking through
    // to a lower-priority legacy alias.
    return std::wstring(buffer, length);
}

bool HasMigratedShenLongSettings(const std::filesystem::path& path) {
    const auto has_any = [&](const auto& sections, const auto& keys) {
        for (const wchar_t* section : sections) {
            for (const wchar_t* key : keys) {
                if (HasExactIniKey(path, section, key)) {
                    return true;
                }
            }
        }
        return false;
    };

    // v58 owns the [Tonemapping] AgX keys natively. They are no longer
    // treated as migrated ShenLong renderer keys: an older file carrying a
    // [Tonemapping] block parses through the alias table and its values are
    // preserved by the organized rewrite instead of being stripped.

    constexpr const wchar_t* shadow_sections[]{kShadowSection, kGraphicsSection, kSection};
    constexpr const wchar_t* shadow_keys[]{L"ShadowResolution", L"shadow_resolution"};
    if (has_any(shadow_sections, shadow_keys)) {
        return true;
    }

    constexpr const wchar_t* ao_sections[]{kAoSection, kGraphicsSection, kSection};
    constexpr const wchar_t* ao_keys[]{
        L"AmbientOcclusion", L"SDAO", L"GTAO", L"OriginalAOQuality", L"SSAO",
        L"SDAOQuality", L"SDAORadius", L"SDAOStrength",
        L"GTAOQuality", L"GTAORadius", L"GTAOStrength",
        L"GTAOLiteQuality", L"GTAOLiteRadius", L"GTAOLiteStrength",
        L"sdao_enable", L"gtao_enable", L"override_ssao", L"sdao_quality",
        L"sdao_radius", L"sdao_strength_percent", L"gtao_quality", L"gtao_radius",
        L"gtao_strength_percent", L"gtao_lite_quality", L"gtao_lite_radius",
        L"gtao_lite_strength_percent", L"hook_ao_stage", L"hook_ao_material",
        L"hook_ao_compute"};
    if (has_any(ao_sections, ao_keys)) {
        return true;
    }

    constexpr const wchar_t* gi_sections[]{kGiSection, kGraphicsSection, kSection};
    constexpr const wchar_t* gi_keys[]{
        L"GlobalIllumination", L"GIQuality", L"GIStrength", L"GIRadius",
        L"global_illumination", L"gi_quality", L"gi_strength_percent", L"gi_radius"};
    if (has_any(gi_sections, gi_keys)) {
        return true;
    }

    constexpr const wchar_t* pbr_sections[]{kPbrSection, kGraphicsSection, kSection};
    constexpr const wchar_t* pbr_keys[]{
        L"PhysicallyBasedRendering", L"PBR", L"physically_based_rendering", L"pbr"};
    if (has_any(pbr_sections, pbr_keys)) {
        return true;
    }

    constexpr const wchar_t* sss_sections[]{kSssSection, kGraphicsSection, kSection};
    constexpr const wchar_t* sss_keys[]{
        L"SubsurfaceScattering", L"StockHairBlur", L"SSSQuality", L"SSSStrength",
        L"SSSRadius", L"subsurface_scattering", L"stock_hair_blur", L"sss_quality",
        L"sss_strength_percent", L"sss_radius_percent"};
    if (has_any(sss_sections, sss_keys)) {
        return true;
    }

    constexpr const wchar_t* material_sections[]{
        kMaterialScatteringSection, kGraphicsSection, kSection};
    constexpr const wchar_t* material_keys[]{
        L"EyeScattering", L"HairScattering", L"TeethScattering",
        L"FoliageTransmission", L"WaterScattering", L"WaterVolumetricScattering",
        L"VolumetricScattering", L"WaterScatteringStrength", L"ScatteringStrength",
        L"WaterScatteringAnisotropy", L"ScatteringAnisotropy", L"eye_scattering",
        L"hair_scattering", L"teeth_scattering", L"foliage_transmission",
        L"water_scattering", L"water_volumetric_scattering", L"volumetric_scattering",
        L"water_sss", L"water_scattering_strength", L"scattering_strength",
        L"water_scattering_anisotropy", L"scattering_anisotropy"};
    if (has_any(material_sections, material_keys)) {
        return true;
    }

    constexpr const wchar_t* debug_sections[]{kDebugSection};
    constexpr const wchar_t* debug_keys[]{
        L"DumpShaders", L"CensusShadowConsumers"};
    return has_any(debug_sections, debug_keys);
}

int ParseAgxLookValue(std::optional<std::wstring> value, int fallback) {
    if (!value.has_value()) {
        return fallback;
    }
    return agx::ParseLookName(*value, fallback);
}

std::optional<std::wstring> ReadConfiguredValue(const std::filesystem::path& path,
                                                 const wchar_t* key) {
    if (const IniKeyAlias* alias = FindIniKeyAlias(key); alias != nullptr) {
        if (auto value = ReadRawValue(path, alias->section, alias->user_key); value.has_value()) {
            return value;
        }
        // Config v5 used the same end-user names but wrote every option under
        // [SPatch]. Check that canonical legacy location before snake_case
        // fallbacks so migration never silently resets a user's settings.
        if (std::wstring_view(alias->section) != std::wstring_view(kSection)) {
            if (auto value = ReadRawValue(path, kSection, alias->user_key); value.has_value()) {
                return value;
            }
        }
    }

    const wchar_t* section = SectionForKey(key);
    if (auto value = ReadRawValue(path, section, key); value.has_value()) {
        return value;
    }
    if (std::wstring_view(section) != std::wstring_view(kSection)) {
        return ReadRawValue(path, kSection, key);
    }
    return std::nullopt;
}

std::optional<std::wstring> ReadLoggingValue(
    const std::filesystem::path& path) {
    // v40 moves the one public logging switch to [Debug] Logging. Prefer the
    // canonical spelling, then preserve the former public and implementation
    // names long enough to migrate them out of the rewritten file.
    for (const auto& [section, key] : std::array{
             std::pair{kDebugSection, L"Logging"},
             std::pair{kSection, L"Logging"},
             std::pair{kDiagnosticsSection, L"EnableLogging"},
             std::pair{kSection, L"EnableLogging"},
             std::pair{kDebugSection, L"EnableLogging"},
             std::pair{kDiagnosticsSection, L"enable_logging"},
             std::pair{kSection, L"enable_logging"}}) {
        if (auto value = ReadRawValue(path, section, key); value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<std::wstring> ReadTextureFilteringValue(
    const std::filesystem::path& path,
    const wchar_t* canonical_key,
    const wchar_t* internal_key) {
    // v42 restores native sampler controls to SPatch. Prefer the dedicated
    // public section, then preserve the two historical locations and finally
    // the former implementation spellings during migration.
    for (const auto& [section, key] : std::array{
             std::pair{kTextureFilteringSection, canonical_key},
             std::pair{kGraphicsSection, canonical_key},
             std::pair{kSection, canonical_key},
             std::pair{kTextureFilteringSection, internal_key},
             std::pair{kGraphicsSection, internal_key},
             std::pair{kSection, internal_key}}) {
        if (auto value = ReadRawValue(path, section, key); value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

bool HasRetiredTextureFilteringLocation(
    const std::filesystem::path& path) {
    for (const auto& [section, key] : std::array{
             std::pair{kTextureFilteringSection, L"anisotropic_filtering"},
             std::pair{kTextureFilteringSection, L"force_anisotropic_filtering"},
             std::pair{kGraphicsSection, L"AnisotropicFiltering"},
             std::pair{kGraphicsSection, L"ForceAnisotropicFiltering"},
             std::pair{kGraphicsSection, L"anisotropic_filtering"},
             std::pair{kGraphicsSection, L"force_anisotropic_filtering"},
             std::pair{kSection, L"AnisotropicFiltering"},
             std::pair{kSection, L"ForceAnisotropicFiltering"},
             std::pair{kSection, L"anisotropic_filtering"},
             std::pair{kSection, L"force_anisotropic_filtering"}}) {
        if (HasExactIniKey(path, section, key)) {
            return true;
        }
    }
    return false;
}

bool HasRetiredCrashDumpLocation(const std::filesystem::path& path) {
    // Before v42 the public key was written under [SPatch]. Keep it readable,
    // but rewrite it once so WriteCrashDumps is literally the final INI key.
    return HasExactIniKey(path, kSection, L"WriteCrashDumps") ||
           HasExactIniKey(path, kSection, L"write_minidumps") ||
           HasExactIniKey(path, kDebugSection, L"write_minidumps");
}

bool HasRetiredLoggingSettings(const std::filesystem::path& path) {
    for (const auto& [section, key] : std::array{
             std::pair{kDiagnosticsSection, L"EnableLogging"},
             std::pair{kSection, L"EnableLogging"},
             std::pair{kDebugSection, L"EnableLogging"},
             std::pair{kDiagnosticsSection, L"enable_logging"},
             std::pair{kSection, L"enable_logging"}}) {
        if (ReadRawValue(path, section, key).has_value()) {
            return true;
        }
    }
    return false;
}

bool HasRetiredVisualDamageSettings(const std::filesystem::path& path) {
    for (const auto& [section, key] : std::array{
             std::pair{kGraphicsSection, L"RestoreVisualDamage"},
             std::pair{kSection, L"RestoreVisualDamage"},
             std::pair{kGraphicsSection, L"restore_character_visual_damage"},
             std::pair{kSection, L"restore_character_visual_damage"}}) {
        if (ReadRawValue(path, section, key).has_value()) {
            return true;
        }
    }
    return false;
}

bool HasRetiredPedDensitySettings(const std::filesystem::path& path) {
    // GTAIVPedDensityCurve and its strength/min/max keys were removed
    // outright. Detect every previously accepted spelling so a file carrying
    // them is rewritten once without residue.
    for (const auto& [section, key] : std::array{
             std::pair{kGraphicsSection, L"GTAIVPedDensityCurve"},
             std::pair{kSection, L"GTAIVPedDensityCurve"},
             std::pair{kGraphicsSection, L"gtaiv_ped_density_curve"},
             std::pair{kSection, L"gtaiv_ped_density_curve"},
             std::pair{kGraphicsSection, L"GTAIVPedDensityStrengthPercent"},
             std::pair{kSection, L"GTAIVPedDensityStrengthPercent"},
             std::pair{kGraphicsSection, L"gtaiv_ped_density_strength_percent"},
             std::pair{kSection, L"gtaiv_ped_density_strength_percent"},
             std::pair{kGraphicsSection, L"GTAIVPedDensityMinScalePercent"},
             std::pair{kSection, L"GTAIVPedDensityMinScalePercent"},
             std::pair{kGraphicsSection, L"gtaiv_ped_density_min_scale_percent"},
             std::pair{kSection, L"gtaiv_ped_density_min_scale_percent"},
             std::pair{kGraphicsSection, L"GTAIVPedDensityMaxScalePercent"},
             std::pair{kSection, L"GTAIVPedDensityMaxScalePercent"},
             std::pair{kGraphicsSection, L"gtaiv_ped_density_max_scale_percent"},
             std::pair{kSection, L"gtaiv_ped_density_max_scale_percent"}}) {
        if (ReadRawValue(path, section, key).has_value()) {
            return true;
        }
    }
    return false;
}

bool HasRetiredNfs2015Settings(const std::filesystem::path& path) {
    // NFS2015Camera and its 14 tuning keys were removed outright. Detect
    // every previously accepted spelling so a file carrying them is
    // rewritten once without residue.
    for (const auto& [section, key] : std::array{
             std::pair{kInputSection, L"NFS2015Camera"},
             std::pair{kSection, L"NFS2015Camera"},
             std::pair{kInputSection, L"nfs2015_camera"},
             std::pair{kSection, L"nfs2015_camera"},
             std::pair{kInputSection, L"NFS2015CameraDistancePercent"},
             std::pair{kSection, L"NFS2015CameraDistancePercent"},
             std::pair{kInputSection, L"nfs2015_camera_distance_percent"},
             std::pair{kSection, L"nfs2015_camera_distance_percent"},
             std::pair{kInputSection, L"NFS2015CameraHeightDropCm"},
             std::pair{kSection, L"NFS2015CameraHeightDropCm"},
             std::pair{kInputSection, L"nfs2015_camera_height_drop_cm"},
             std::pair{kSection, L"nfs2015_camera_height_drop_cm"},
             std::pair{kInputSection, L"NFS2015CameraSteeringSwingCm"},
             std::pair{kSection, L"NFS2015CameraSteeringSwingCm"},
             std::pair{kInputSection, L"nfs2015_camera_steering_swing_cm"},
             std::pair{kSection, L"nfs2015_camera_steering_swing_cm"},
             std::pair{kInputSection, L"NFS2015CameraNoiseCm"},
             std::pair{kSection, L"NFS2015CameraNoiseCm"},
             std::pair{kInputSection, L"nfs2015_camera_noise_cm"},
             std::pair{kSection, L"nfs2015_camera_noise_cm"},
             std::pair{kInputSection, L"NFS2015CameraYawLagPercent"},
             std::pair{kSection, L"NFS2015CameraYawLagPercent"},
             std::pair{kInputSection, L"nfs2015_camera_yaw_lag_percent"},
             std::pair{kSection, L"nfs2015_camera_yaw_lag_percent"},
             std::pair{kInputSection, L"NFS2015CameraSpeedDistancePercent"},
             std::pair{kSection, L"NFS2015CameraSpeedDistancePercent"},
             std::pair{kInputSection, L"nfs2015_camera_speed_distance_percent"},
             std::pair{kSection, L"nfs2015_camera_speed_distance_percent"},
             std::pair{kInputSection, L"NFS2015CameraLookAheadCm"},
             std::pair{kSection, L"NFS2015CameraLookAheadCm"},
             std::pair{kInputSection, L"nfs2015_camera_look_ahead_cm"},
             std::pair{kSection, L"nfs2015_camera_look_ahead_cm"},
             std::pair{kInputSection, L"NFS2015CameraDriftSwingCm"},
             std::pair{kSection, L"NFS2015CameraDriftSwingCm"},
             std::pair{kInputSection, L"nfs2015_camera_drift_swing_cm"},
             std::pair{kSection, L"nfs2015_camera_drift_swing_cm"},
             std::pair{kInputSection, L"NFS2015CameraRearCm"},
             std::pair{kSection, L"NFS2015CameraRearCm"},
             std::pair{kInputSection, L"nfs2015_camera_rear_cm"},
             std::pair{kSection, L"nfs2015_camera_rear_cm"},
             std::pair{kInputSection, L"NFS2015CameraHeightCm"},
             std::pair{kSection, L"NFS2015CameraHeightCm"},
             std::pair{kInputSection, L"nfs2015_camera_height_cm"},
             std::pair{kSection, L"nfs2015_camera_height_cm"},
             std::pair{kInputSection, L"NFS2015CameraRollPercent"},
             std::pair{kSection, L"NFS2015CameraRollPercent"},
             std::pair{kInputSection, L"nfs2015_camera_roll_percent"},
             std::pair{kSection, L"nfs2015_camera_roll_percent"},
             std::pair{kInputSection, L"NFS2015CameraPitchPercent"},
             std::pair{kSection, L"NFS2015CameraPitchPercent"},
             std::pair{kInputSection, L"nfs2015_camera_pitch_percent"},
             std::pair{kSection, L"nfs2015_camera_pitch_percent"},
             std::pair{kInputSection, L"NFS2015CameraHeightLimitPercent"},
             std::pair{kSection, L"NFS2015CameraHeightLimitPercent"},
             std::pair{kInputSection, L"nfs2015_camera_height_limit_percent"},
             std::pair{kSection, L"nfs2015_camera_height_limit_percent"}}) {
        if (ReadRawValue(path, section, key).has_value()) {
            return true;
        }
    }
    return false;
}


std::optional<std::wstring> ReadRendererValue(
    const std::filesystem::path& path,
    const wchar_t* canonical_key,
    const wchar_t* internal_key) {
    // Renderer/swap-chain overrides are retired. Detect every previously
    // accepted spelling so even a current-version file is rewritten once to
    // the stock native D3D11 contract.
    for (const auto& [section, key] : std::array{
             std::pair{kRendererSection, canonical_key},
             std::pair{kGraphicsSection, canonical_key},
             std::pair{kSection, canonical_key},
             std::pair{kGraphicsSection, internal_key},
             std::pair{kSection, internal_key}}) {
        if (auto value = ReadRawValue(path, section, key); value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

bool HasRetiredUnverifiedSettings(const std::filesystem::path& path) {
    const auto has_key = [&](const wchar_t* section,
                             const wchar_t* canonical_key,
                             const wchar_t* internal_key) {
        return ReadRawValue(path, section, canonical_key).has_value() ||
               ReadRawValue(path, kSection, canonical_key).has_value() ||
               ReadRawValue(path, section, internal_key).has_value() ||
               ReadRawValue(path, kSection, internal_key).has_value();
    };

    return has_key(kGraphicsSection, L"FixFrameRateTimers",
                   L"fix_frame_counted_activity_timer") ||
           has_key(kGraphicsSection, L"FixFrameRateSmoothing",
                   L"fix_frame_counted_smoothing") ||
           has_key(kStabilitySection, L"FixId3TagDivide",
                   L"fix_id3_tag_divide") ||
           has_key(kStabilitySection, L"FixPathfindingDivide",
                   L"fix_pathfinding_divide") ||
           has_key(kStabilitySection, L"FixCompressedMeshDivide",
                   L"fix_compressed_mesh_divide") ||
           has_key(kStabilitySection, L"FixPercentDivide",
                   L"fix_percent_divide") ||
           has_key(kStabilitySection, L"FixElementDivide",
                   L"fix_element_divide") ||
           has_key(kStabilitySection, L"FixObjectCountDivide",
                   L"fix_object_count_divide") ||
           has_key(kStabilitySection, L"FixElementScaleDivide",
                   L"fix_element_scale_divide") ||
           has_key(kStabilitySection, L"FixTableWalkDivide",
                   L"fix_table_walk_divide") ||
           has_key(kStabilitySection, L"FixElementLookupDivide",
                   L"fix_element_lookup_divide") ||
           has_key(kStabilitySection, L"FixCountIndexDivide",
                   L"fix_count_index_divide") ||
           has_key(kStabilitySection, L"FixSlotCountDivide",
                   L"fix_slot_count_divide") ||
           has_key(kStabilitySection, L"FixBucketSizeDivide",
                   L"fix_bucket_size_divide") ||
           has_key(kStabilitySection, L"FixHashMixDivide",
                   L"fix_hash_mix_divide") ||
           ReadRawValue(path, kAdvancedSection,
                        L"hook_dispatch_probe").has_value() ||
           ReadRawValue(path, kSection, L"hook_dispatch_probe").has_value();
}

std::optional<std::wstring> ReadOriginalFogAndNeonValue(
    const std::filesystem::path& path) {
    // v15 made the fog and neon result explicit. Prefer the canonical name,
    // then accept the v13 and v12 public names and the pre-v6 implementation
    // name from their grouped or catch-all locations.
    if (auto value = ReadRawValue(path, kGraphicsSection, L"RestoreOriginalFogAndNeon");
        value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kSection, L"RestoreOriginalFogAndNeon");
        value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kGraphicsSection, L"RestoreOriginalAtmosphere");
        value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kSection, L"RestoreOriginalAtmosphere");
        value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kGraphicsSection, L"RestoreOriginalFog");
        value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kSection, L"RestoreOriginalFog"); value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kGraphicsSection, L"restore_original_fog");
        value.has_value()) {
        return value;
    }
    return ReadRawValue(path, kSection, L"restore_original_fog");
}




std::optional<std::wstring> ReadOriginalShadowFilterValue(
    const std::filesystem::path& path) {
    if (auto value = ReadRawValue(path, kShadowSection, L"OriginalShadowFilter");
        value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kSection, L"OriginalShadowFilter");
        value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kGraphicsSection, L"ShadowFilter");
        value.has_value()) {
        return value;
    }
    if (auto value = ReadRawValue(path, kSection, L"ShadowFilter"); value.has_value()) {
        return value;
    }
    return ReadConfiguredValue(path, L"override_shadow_filter");
}

bool HasRetiredShadowSettings(const std::filesystem::path& path) {
    for (const auto& [section, key] : std::array{
             std::pair{kShadowSection, L"RestoreCharacterShadows"},
             std::pair{kGraphicsSection, L"RestoreCharacterShadows"},
             std::pair{kSection, L"RestoreCharacterShadows"},
             std::pair{kShadowSection, L"restore_character_shadow_resolution"},
             std::pair{kGraphicsSection, L"restore_character_shadow_resolution"},
             std::pair{kSection, L"restore_character_shadow_resolution"},
             std::pair{kShadowSection, L"CharacterShadowResolution"},
             std::pair{kGraphicsSection, L"CharacterShadowResolution"},
             std::pair{kSection, L"CharacterShadowResolution"},
             std::pair{kShadowSection, L"character_shadow_resolution"},
             std::pair{kGraphicsSection, L"character_shadow_resolution"},
             std::pair{kSection, L"character_shadow_resolution"},
             std::pair{kShadowSection, L"ShadowFilterScale"},
             std::pair{kSection, L"ShadowFilterScale"},
             std::pair{kShadowSection, L"shadow_filter_scale"},
             std::pair{kSection, L"shadow_filter_scale"}}) {
        if (ReadRawValue(path, section, key).has_value()) {
            return true;
        }
    }
    return false;
}

bool HasRetiredPcssSettings(const std::filesystem::path& path) {
    for (const wchar_t* section : {kShadowSection, kSection}) {
        for (const wchar_t* key : {
                 L"ShadowFiltering",
                 L"shadow_filtering",
                 L"PCSS",
                 L"PCSSQuality",
                 L"pcss_quality",
                 L"PCSSLightSize",
                 L"pcss_light_size_percent"}) {
            if (ReadRawValue(path, section, key).has_value()) {
                return true;
            }
        }
    }
    return false;
}

std::wstring TrimIniValue(std::wstring value) {
    const std::size_t comment = value.find_first_of(L";#");
    if (comment != std::wstring::npos) {
        value.resize(comment);
    }
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool WriteConfigText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!stream) {
        return false;
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream.good()) {
        return false;
    }
    // Check the buffered write before the caller atomically publishes the
    // temporary file.  Testing good() alone only observes the stream buffer;
    // a close-time flush failure could otherwise replace a valid INI with a
    // truncated one.
    stream.flush();
    if (!stream.good()) {
        return false;
    }
    stream.close();
    return !stream.fail();
}

bool PathExistsNoThrow(const std::filesystem::path& path) noexcept {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    return !error && exists;
}

std::optional<std::filesystem::path> LocalAppDataDirectory() {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required == 0) {
        return std::nullopt;
    }
    std::vector<wchar_t> buffer(required);
    const DWORD written = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size()) {
        return std::nullopt;
    }
    std::filesystem::path root(std::wstring(buffer.data(), written));
    if (!root.is_absolute()) {
        return std::nullopt;
    }
    return root.lexically_normal();
}

std::wstring NormalizedConfigPathKey(
    const std::filesystem::path& config_path) {
    std::error_code error;
    std::filesystem::path normalized =
        std::filesystem::absolute(config_path, error);
    if (error) {
        normalized = config_path;
    }
    std::wstring key = normalized.lexically_normal().native();
    std::replace(key.begin(), key.end(), L'/', L'\\');
    if (!key.empty()) {
        CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
    }
    return key;
}

std::filesystem::path BuildExternalBackupPath(
    const std::filesystem::path& config_path,
    const std::wstring& filename) {
    const auto local_app_data = LocalAppDataDirectory();
    if (!local_app_data.has_value()) {
        return {};
    }

    constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    std::uint64_t hash = kFnvOffset;
    const std::wstring key = NormalizedConfigPathKey(config_path);
    for (const wchar_t character : key) {
        const std::uint16_t unit = static_cast<std::uint16_t>(character);
        hash ^= static_cast<unsigned char>(unit & 0xFFU);
        hash *= kFnvPrime;
        hash ^= static_cast<unsigned char>((unit >> 8U) & 0xFFU);
        hash *= kFnvPrime;
    }

    std::wostringstream directory_name;
    directory_name << std::hex << std::setfill(L'0') << std::setw(16) << hash;
    return *local_app_data / L"SPatch" / L"ConfigBackups" /
           directory_name.str() / filename;
}

bool FilesHaveEqualBytes(const std::filesystem::path& first,
                         const std::filesystem::path& second) {
    std::error_code error;
    const auto first_size = std::filesystem::file_size(first, error);
    if (error) {
        return false;
    }
    const auto second_size = std::filesystem::file_size(second, error);
    if (error || first_size != second_size) {
        return false;
    }

    std::ifstream first_stream(first, std::ios::binary);
    std::ifstream second_stream(second, std::ios::binary);
    if (!first_stream || !second_stream) {
        return false;
    }
    std::array<char, 64 * 1024> first_buffer{};
    std::array<char, 64 * 1024> second_buffer{};
    while (first_stream && second_stream) {
        first_stream.read(first_buffer.data(), first_buffer.size());
        second_stream.read(second_buffer.data(), second_buffer.size());
        const auto first_count = first_stream.gcount();
        const auto second_count = second_stream.gcount();
        if (first_count != second_count ||
            !std::equal(first_buffer.begin(), first_buffer.begin() + first_count,
                        second_buffer.begin())) {
            return false;
        }
    }
    return first_stream.eof() && second_stream.eof();
}

bool WriteBackupCopy(const std::filesystem::path& source,
                     const std::filesystem::path& backup) {
    if (backup.empty()) {
        return false;
    }
    std::error_code directory_error;
    std::filesystem::create_directories(backup.parent_path(), directory_error);
    if (directory_error) {
        return false;
    }
    const unsigned long serial = g_config_write_serial.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path temp_path =
        backup.parent_path() /
        (backup.filename().wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
         std::to_wstring(serial) + L".tmp");
    if (!CopyFileW(source.c_str(), temp_path.c_str(), FALSE)) {
        std::error_code error;
        std::filesystem::remove(temp_path, error);
        return false;
    }
    if (MoveFileExW(temp_path.c_str(),
                    backup.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }

    std::error_code error;
    std::filesystem::remove(temp_path, error);
    return false;
}

bool RelocateLegacyInGameBackup(const std::filesystem::path& config_path) {
    const std::filesystem::path legacy_path(
        config_path.wstring() + L".previous.bak");
    if (!PathExistsNoThrow(legacy_path)) {
        return true;
    }
    const std::filesystem::path destination =
        LegacyConfigBackupPath(config_path);
    if (!WriteBackupCopy(legacy_path, destination) ||
        !FilesHaveEqualBytes(legacy_path, destination)) {
        return false;
    }
    if (DeleteFileW(legacy_path.c_str()) != FALSE) {
        return true;
    }
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}

// Versioned, user-facing template. All new files and migrations use concise
// SilentPatch-style names; legacy snake_case keys and old sections remain
// readable through the fallback parser below.
bool WriteOrganizedConfig(const std::filesystem::path& path, const Config& config) {
    const Config defaults{};
    std::ostringstream text;
    text << "; SPatch for Sleeping Dogs: Definitive Edition\n"
         << ";\n"
         << "; HOW TO CHANGE A SETTING\n"
         << ";   1. Change the number after the equals sign.\n"
         << ";   2. Save the file.\n"
         << ";\n"
         << "; Most settings are simple on and off switches: 1 turns the\n"
         << "; feature on, 0 turns it off. Settings that take a number\n"
         << "; say which values they accept. Anything marked \"restart\"\n"
         << "; only takes effect after you start the game again.\n"
         << "[SPatch]\n"
         << "; Leave this alone. It tells the mod which format the file uses.\n"
         << "ConfigVersion=" << kConfigVersion << "\n\n"
         << "; The main on and off switch for the whole mod.\n"
         << "Enabled=" << (config.enabled ? 1 : 0) << "\n";
    if (config.allow_unverified_build != defaults.allow_unverified_build) {
        text << "\n; Leave this at 0 unless someone tells you to change it.\n"
             << "; Setting it to 1 lets the mod run on a game version it was not\n"
             << "; made for, but many fixes switch themselves off because they\n"
             << "; cannot be applied safely.\n"
             << "AllowUnverifiedBuild=" << (config.allow_unverified_build ? 1 : 0) << "\n";
    }

    text << "\n[Cutscenes]\n"
         << "; Fix cutscenes stuttering and dropping frames for a moment.\n"
         << "FixCutsceneFPS="
         << ((config.fix_cutscene_zero_dt || config.fix_cutscene_scene_time_step) ? 1 : 0)
         << "\n\n"
         << "; 0 lets cutscenes play at whatever speed the game manages.\n"
         << "; You can also type a fixed speed from 15 to 1000.\n"
         << "; Only set a fixed speed if cutscenes still play at the wrong\n"
         << "; speed when the game is running well. Restart required.\n"
         << "CutsceneFPS=" << config.cutscene_fps << "\n\n"
         << "; Fix characters that appear twice in some cutscenes.\n"
         << "FixDuplicateCutsceneActorRestore="
         << (config.fix_nis_actor_restore_duplicates ? 1 : 0) << "\n\n"
         << "; FixCutsceneFPS repairs frames the game reports as zero-length,\n"
         << "; which is what makes cutscene motion smooth above 30 FPS.\n"
         << "FixCutsceneZeroDT=" << config.override_cutscene_zero_dt << "\n\n"
         << "; Rewrites the cutscene timeline itself. It smooths 30 FPS\n"
         << "; cutscene animation further, but it also pulls the timeline away\n"
         << "; from the game's own clock, which can deform props, chains and\n"
         << "; clothing during cutscenes. Off by default. Set to 1 only if\n"
         << "; cutscene motion still looks stepped with FixCutsceneFPS on.\n"
         << "FixCutsceneSceneTime="
         << (config.fix_cutscene_scene_time_step ? 1 : 0) << "\n\n"
         << "; -1 keeps the game setting. 0 turns smoothing off. 1 turns on\n"
         << "; the smooth frame timing the game normally uses.\n"
         << "TimeStepSmoothing=" << config.time_step_smoothing << "\n";

    text << "\n[Input]\n"
         << "; Use the game's direct mouse input, which is normally hidden.\n"
         << "; This makes aiming with the mouse feel more immediate.\n"
         << "ForceRawMouseInput=" << (config.force_raw_mouse_input ? 1 : 0) << "\n\n"
         << "; Take the slow drift out of the mouse camera, so it turns\n"
         << "; exactly where you point it. Gamepads are not affected.\n"
         << "DisableCameraSmoothing=" << (config.disable_camera_smoothing ? 1 : 0) << "\n\n"
         << "; Vehicle camera in the style of GTA IV. The view sits further\n"
         << "; to the right, follows the car loosely, and swings out when\n"
         << "; you use the handbrake. Works in cars, trucks and buses. Restart required.\n"
         << "GTAIVCarCamera=" << (config.gta_iv_car_camera ? 1 : 0) << "\n\n"
         << "; Stop the camera switching when the police start chasing you.\n"
         << "; 0 keeps the usual switch, 1 keeps your normal driving camera.\n"
         << "DisablePursuitCamera=" << (config.disable_pursuit_camera ? 1 : 0) << "\n";
    if (config.fix_chase_aim_camera != defaults.fix_chase_aim_camera) {
        text << "\n; Keeps the camera outside of the car when you aim a gun while\n"
             << "; the police are chasing you. Without it the chase camera sits\n"
             << "; inside the car body while aiming. 0 leaves the camera as is.\n"
             << "FixChaseAimCamera=" << (config.fix_chase_aim_camera ? 1 : 0) << "\n";
    }
    text << "\n; The same GTA IV style camera, but for bikes. Restart required.\n"
         << "GTAIVBikeCamera=" << (config.gta_iv_bike_camera ? 1 : 0) << "\n\n"
         << "; Stop the camera drifting back to the middle on its own when\n"
         << "; you use a gamepad in a vehicle. 0 keeps\n"
         << "; the normal behaviour. Restart required.\n"
         << "DisableControllerCameraAutocenter=" << (config.disable_controller_camera_autocenter ? 1 : 0) << "\n\n"
         << "; Experimental. Also stops the camera drifting back to the\n"
         << "; middle when you are on foot. Some gamepads can freeze when\n"
         << "; you turn, so set this back to 0 if that happens to you.\n"
         << "; Restart required.\n"
         << "DisableOnFootCameraAutocenter=" << (config.disable_onfoot_camera_autocenter ? 1 : 0) << "\n\n"
         << "; Adds the fast camera shake from GTA V at high speed. This\n"
         << "; works on its own, separately from the GTA IV camera above.\n"
         << "; Restart required.\n"
         << "GTAVSpeedShake=" << (config.gtav_speed_shake ? 1 : 0) << "\n\n"
         << "; How strong the shake is, as a percentage. 0 turns it off.\n"
         << "; Type a number from 0 to 300.\n"
         << "GTAVSpeedShakeStrength="
         << config.gtav_speed_shake_strength_percent << "\n\n"
         << "; Speed in km/h where the shake begins. Type a number from\n"
         << "; 0 to 400.\n"
         << "GTAVSpeedShakeStartSpeedKmh="
         << config.gtav_speed_shake_start_speed_kmh << "\n\n"
         << "; Speed in km/h where the shake reaches full strength. This\n"
         << "; must be higher than the speed above. Type a number from 1\n"
         << "; to 400.\n"
         << "GTAVSpeedShakeFullSpeedKmh="
         << config.gtav_speed_shake_full_speed_kmh << "\n\n"
         << "; Rumble in the controller that grows with the car's speed.\n"
         << "; 0 turns it off.\n"
         << "SpeedRumble=" << (config.speed_rumble ? 1 : 0) << "\n\n"
         << "; Speed in km/h where the rumble begins. It cannot go below 1.\n"
         << "SpeedRumbleStartKmh=" << config.speed_rumble_start_kmh << "\n\n"
         << "; Speed in km/h where the rumble is at its strongest. It is\n"
         << "; raised for you if it is not above the start speed.\n"
         << "SpeedRumbleFullKmh=" << config.speed_rumble_full_kmh << "\n\n"
         << "; How strong the rumble is, as a percentage of the motor's\n"
         << "; full power. Type a number from 0 to 100.\n"
         << "SpeedRumbleStrengthPercent=" << config.speed_rumble_strength_percent << "\n\n"
         << "; How far you must push the left stick before the game notices,\n"
         << "; as a percentage. -1 keeps the game setting, 0 reacts straight\n"
         << "; away, and 1 to 95 makes the stick less sensitive near the\n"
         << "; middle. Setting it to 0 can cause drift on some hardware.\n"
         << "LeftStickDeadzone=" << config.controller_left_stick_deadzone << "\n\n"
         << "; The same setting, but for the right stick. Type -1 to leave\n"
         << "; the controller's own setting alone, 0 to turn the filter off,\n"
         << "; or a number from 1 to 95 to set your own value. Setting it to 0 can cause drift on some hardware.\n"
         << "RightStickDeadzone=" << config.controller_right_stick_deadzone << "\n";

    text << "\n[Graphics]\n"
         << "; Restores the original fog values from the OG release and also fixes the missing glow from neon signs.\n"
         << "RestoreOriginalFogAndNeon=" << (config.restore_original_fog ? 1 : 0) << "\n\n"
         << "; Restores missing iris detail on some DE models.\n"
         << "RestoreOriginalEyeReflections="
         << (config.restore_original_eye_reflections ? 1 : 0) << "\n";
    if (config.restore_character_wetness != defaults.restore_character_wetness ||
        config.wetness_full_time_seconds != defaults.wetness_full_time_seconds ||
        config.wetness_fade_time_seconds != defaults.wetness_fade_time_seconds) {
        text << "\n; Let clothes and skin get properly wet in the rain, and dry again\n"
             << "; afterwards. Without this, some outfits never looked wet at all.\n"
             << "; Restart required.\n"
             << "RestoreWetness=" << (config.restore_character_wetness ? 1 : 0) << "\n"
             << "; How many seconds clothes stay soaking wet after you step out of\n"
             << "; water, from 0 to 3600.\n"
             << "WetnessFullTime=" << config.wetness_full_time_seconds << "\n"
             << "; How many seconds wet clothes then take to dry, from 0 to 3600.\n"
             << "WetnessFadeTime=" << config.wetness_fade_time_seconds << "\n";
    }
    text << "\n; Let Wei and other characters sweat after running or fighting.\n"
         << "; Driving does not count as running. Restart required.\n"
         << "RestoreSweat=" << (config.restore_character_sweat ? 1 : 0) << "\n\n"
         << "; Turn off the always on blur (dept of field) in the distance. 0 keeps it, 1 turns it off.\n"
         << "DisableDOF=" << (config.disable_dof ? 1 : 0) << "\n\n"
         << "; Turn off DOF during cutscenes only.\n"
         << "DisableCutsceneDOF=" << (config.disable_cutscene_dof ? 1 : 0) << "\n\n"
         << "; How many seconds of running or fighting it takes to sweat\n"
         << "; heavily, from 0 to 3600.\n"
         << "SweatBuildTime=" << config.sweat_build_time_seconds << "\n\n"
         << "; How many seconds sweat takes to dry once you stop, from 0 to 3600.\n"
         << "SweatFadeTime=" << config.sweat_fade_time_seconds << "\n\n"
         << "; How many seconds of running or fighting it takes before any\n"
         << "; sweat shows at all, from 0 to 3600.\n"
         << "SweatOnsetTime=" << config.sweat_onset_time_seconds << "\n\n"
         << "; How fast you have to be moving sideways to count as running,\n"
         << "; from 0 to 100.\n"
         << "SweatRunSpeed=" << config.sweat_run_speed << "\n\n"
         << "; How many seconds other people keep sweating after they are\n"
         << "; hit or punched, from 0 to 3600.\n"
         << "SweatCombatTime=" << config.sweat_combat_time_seconds << "\n\n"
         << "; Keeps crowd behaviour correct above 30 FPS.\n"
         << "FixPedestrianDensity="
         << (config.fix_pedestrian_density_at_high_fps ? 1 : 0) << "\n\n"
         << "; Stop the camera and car handling from behaving differently at very high FPS.\n"
         << "FixHighFPSAverages=" << (config.fix_high_fps_average_windows ? 1 : 0) << "\n\n"
         << "; Make reflections on shiny cars and surfaces sharper.\n"
         << "ImproveSphericalReflections="
         << (config.improve_spherical_reflections ? 1 : 0) << "\n\n"
         << "; 0 uses your current screen width. You can also type a width\n"
         << "; from 1280 to 4096.\n"
         << "SphericalReflectionWidth=" << config.spherical_reflection_width << "\n\n"
         << "; Show the true amount of graphics memory in use on the\n"
         << "; benchmark screen.\n"
         << "FixVRAMReporting=" << (config.fix_vram_capacity_reporting ? 1 : 0) << "\n";
    if (config.override_texture_detail_level != defaults.override_texture_detail_level ||
        config.override_motion_blur != defaults.override_motion_blur) {
        text << "\n; -1 keeps the game setting. 0 low, 1 medium, 2 high.\n"
             << "TextureDetail=" << config.override_texture_detail_level << "\n"
             << "; -1 keeps the game setting. 0 turns motion blur off, 1 normal,\n"
             << "; 2 high quality.\n"
             << "MotionBlur=" << config.override_motion_blur << "\n";
    }

    text << "\n[Tonemapping]\n"
         << "; Change how bright and colourful the game looks, it softens\n"
         << "; bright areas and stops colours going flat. This replaces the default basic tonemapping. 0 turns it off.\n"
         << "; Restart required.\n"
         << "AgX=" << (config.agx_enabled ? 1 : 0) << "\n\n"
         << "; Neutral gives a soft, unsaturated look. MediumHigh keeps more of\n"
         << "; the original game's contrast. Restart required.\n"
         << "AgXLook=" << (config.agx_look == agx::kAgxLookNeutral ? "Neutral" : "MediumHigh") << "\n\n"
         << "; 100 gives the full effect. 0 turns it off completely.\n"
         << "AgXStrength=" << config.agx_strength << "\n\n"
         << "; Overall brightness, from 25 to 400. 100 leaves it as it was.\n"
         << "AgXExposure=" << config.agx_exposure << "\n";
    if (config.agx_debug_bloom != defaults.agx_debug_bloom) {
        text << "\n; Leave this at 0. Anything above 0 replaces the whole screen\n"
             << "; with a test image taken from the game, so you will not be able\n"
             << "; to play. Only change it if someone asks you to, and use a\n"
             << "; separate launch for it. Restart required.\n"
             << "AgxDebugBloom=" << config.agx_debug_bloom << "\n";
    }

    text << "\n[TextureFiltering]\n"
         << "; Keeps textures sharp when you look at them from\n"
         << "; a distance. -1 keeps the game setting. You can also force 4, 8\n"
         << "; or 16. Higher numbers keep far away textures sharper. The vanilla game has this set at 4 with no possibility of changing it.\n"
         << "AnisotropicFiltering=" << config.anisotropic_filtering << "\n\n"
         << "; Also apply the setting above to textures the game would\n"
         << "; normally leave blurry.\n"
         << "ForceAnisotropicFiltering="
         << (config.force_anisotropic_filtering ? 1 : 0) << "\n";
    if (config.override_shadow_filter != defaults.override_shadow_filter ||
        config.shadow_resolution != defaults.shadow_resolution ||
        config.headlight_shadows != defaults.headlight_shadows ||
        config.siren_shadows != defaults.siren_shadows ||
        config.headlight_shadow_light_type != defaults.headlight_shadow_light_type ||
        config.siren_shadow_light_type != defaults.siren_shadow_light_type) {
        text << "\n[Shadows]\n"
             << "; Use the game's own shadow quality. -1 follows the option inside\n"
             << "; the game, 0 normal, 1 high.\n"
             << "OriginalShadowFilter=" << config.override_shadow_filter << "\n";
        if (config.shadow_resolution != defaults.shadow_resolution) {
            text << "\n; Shadow-map upscale applied by SPatch's shadow-scale\n"
                 << "; module (native D3D11 detours). 0 keeps the game's own\n"
                 << "; shadow-map sizes, 2048 floors the small classes to 2048,\n"
                 << "; and 4096 additionally doubles the native 2048 cascades to\n"
                 << "; 4096. Restart required.\n"
                 << "ShadowResolution=" << config.shadow_resolution << "\n";
        }
        if (config.headlight_shadows != defaults.headlight_shadows) {
            text << "\n; Give car headlight beams shadows through the game's\n"
                 << "; own spotlight shadow path. The original game never gave\n"
                 << "; vehicle lights shadows, so this is a new effect, not a\n"
                 << "; restored one. Set 0 to turn it back off.\n"
                 << "HeadlightShadows="
                 << (config.headlight_shadows ? 1 : 0) << "\n";
        }
        if (config.siren_shadows != defaults.siren_shadows) {
            text << "\n; The same shadow path for police lightbar lights.\n"
                 << "SirenShadows="
                 << (config.siren_shadows ? 1 : 0) << "\n";
        }
        if (config.headlight_beam_light != defaults.headlight_beam_light) {
            text << "\n; SPatch adds a real headlight beam light to your car\n"
                 << "; while you drive, rendered by the game's own lighting and\n"
                 << "; shadow system. The original game never did this - it is\n"
                 << "; a new effect. Set 0 to turn it off.\n"
                 << "HeadlightBeamLight="
                 << (config.headlight_beam_light ? 1 : 0) << "\n";
        }
        if (config.headlight_beam_radius != defaults.headlight_beam_radius) {
            text << "\n; How far the beam light reaches, in metres (4 to 40).\n"
                 << "HeadlightBeamRadius=" << config.headlight_beam_radius << "\n";
        }
        if (config.headlight_shadow_light_type != defaults.headlight_shadow_light_type) {
            text << "\n; Advanced: which internal light family the headlight\n"
                 << "; shadow switch above applies to. Leave this alone unless\n"
                 << "; someone tracking a problem asks you to change it.\n"
                 << "HeadlightShadowLightType="
                 << config.headlight_shadow_light_type << "\n";
        }
        if (config.siren_shadow_light_type != defaults.siren_shadow_light_type) {
            text << "\n; Advanced: which internal light family the siren shadow\n"
                 << "; switch above applies to. Leave this alone unless someone\n"
                 << "; tracking a problem asks you to change it.\n"
                 << "SirenShadowLightType=" << config.siren_shadow_light_type << "\n";
        }
    }

    text << "\n[Display]\n"
         << "; Fix a broken 1920x1880 resolution that can appear the first\n"
         << "; time you start the game.\n"
         << "FixFirstRunResolution=" << (config.fix_first_run_resolution ? 1 : 0) << "\n\n"
         << "; Use the fastest refresh rate your monitor offers for the\n"
         << "; resolution you picked.\n"
         << "UseMaximumRefreshRate=" << (config.prefer_max_refresh_rate ? 1 : 0) << "\n\n"
         << "; Remove an extra wait the game puts in when your frame rate\n"
         << "; limit is turned off, which otherwise holds the game near 120\n"
         << "; frames a second. Frame limiter settings and vertical sync are\n"
         << "; not changed by this.\n"
         << "Remove120FPSCap=" << (config.remove_hidden_120_fps_cap ? 1 : 0) << "\n\n"
         << "; -1 keeps the game setting, 0 windowed, 1 fullscreen.\n"
         << "Fullscreen=" << config.override_fullscreen << "\n\n"
         << "; Cap the frame rate to any number between 30 and 1000.\n"
         << "; 0 turns this off.\n"
         << "CustomFrameLimiterFPS=" << config.custom_frame_limiter_fps << "\n"
         << "; Turn the frame rate cap above on. This works the way\n"
         << "; Rivatuner does: the mod waits between frames, so the limit\n"
         << "; holds steady and stays smooth instead of stuttering. Works\n"
         << "; best with vertical sync off. Restart required.\n"
         << "CustomFrameLimiter=" << (config.custom_frame_limiter ? 1 : 0) << "\n";
    if (config.override_low_res_buffer != defaults.override_low_res_buffer) {
        text << "\n; -1 keeps the game setting, 0 turns it off, 1 turns it on.\n"
             << "; Turning it on makes the game slightly faster and slightly\n"
             << "; blurrier.\n"
             << "LowResolutionBuffer=" << config.override_low_res_buffer << "\n";
    }
    if (config.warn_low_res_buffer != defaults.warn_low_res_buffer) {
        text << "\n; Write a note in SPatch.log if the low resolution setting is\n"
             << "; still turned on.\n"
             << "WarnLowResolutionBuffer=" << (config.warn_low_res_buffer ? 1 : 0) << "\n";
    }

    text << "\n[Gameplay]\n"
         << "; Hide the police markers on the radar. 0 shows them, 1 hides them.\n"
         << "HideCopRadarIcons=" << (config.hide_cop_radar_icons ? 1 : 0)
         << "\n\n"
         << "; To go into reverse you must stop, let go of the brake, then press it again, as in many racing games.\n"
         << "ModernBrakeReverse=" << (config.modern_brake_reverse ? 1 : 0)
         << "\n";
    if (config.no_wheel_turn_back != defaults.no_wheel_turn_back) {
        text << "\n; Stop the front wheels straightening themselves when you get\n"
             << "; out of a vehicle. 0 lets the game straighten them, 1 leaves\n"
             << "; them turned as they were. Restart required.\n"
             << "NoWheelTurnBack=" << (config.no_wheel_turn_back ? 1 : 0) << "\n";
    }
    if (config.override_world_density != defaults.override_world_density) {
        text << "\n; -1 keeps the game setting. 0 to 4 pick how crowded the streets\n"
             << "; are, from empty to very busy.\n"
             << "WorldDensity=" << config.override_world_density << "\n";
    }
    if (config.override_rumble_enabled != defaults.override_rumble_enabled) {
        text << "\n; -1 keeps the game setting, 0 turns controller rumble off,\n"
             << "; 1 turns it on.\n"
             << "Rumble=" << config.override_rumble_enabled << "\n";
    }
    text << "\n; Wei will now shout when bumping into people.\n"
         << "RestorePushVoiceLines=" << (config.restore_push_voice_lines ? 1 : 0) << "\n\n"
         << "; Keep the feature above quiet while a mission is running.\n"
         << "PushVoiceLinesFreeRoamOnly="
         << (config.restore_push_voice_lines_free_roam_only ? 1 : 0) << "\n\n"
         << "; Wei will now shout when a fight starts and while he is\n"
         << "; fighting, he will also apologise when you hit NPCs outside a fight.\n"
         << "RestoreCombatLines=" << (config.restore_combat_lines ? 1 : 0) << "\n\n"
         << "; Keep the feature above quiet while a mission is running.\n"
         << "CombatLinesFreeRoamOnly="
         << (config.restore_combat_lines_free_roam_only ? 1 : 0) << "\n";
    if (config.restore_combat_lines_all_voices != defaults.restore_combat_lines_all_voices) {
        text << "\n; Let the struck character answer in their OWN voice instead of\n"
             << "; Wei's. Needs RestoreCombatLines=1. Restart required.\n"
             << "RestoreCombatLinesAllVoices="
             << (config.restore_combat_lines_all_voices ? 1 : 0) << "\n";
    }
    text << "\n; Wei will curse when he crashes into a car or barrier at speed\n"
         << "; while driving.\n"
         << "DrivingSwearLines="
         << (config.driving_swear_lines ? 1 : 0) << "\n\n"
         << "; Keep the driving curses quiet while a mission is running.\n"
         << "DrivingSwearLinesFreeRoamOnly="
         << (config.driving_swear_lines_free_roam_only ? 1 : 0) << "\n\n"
         << "; Wei will shout at the police while driving when they are\n"
         << "; chasing you and the wanted level is high.\n"
         << "CopPursuitLines="
         << (config.cop_pursuit_swear_lines ? 1 : 0) << "\n\n"
         << "; Keep the police shouts quiet while a mission is running.\n"
         << "CopPursuitLinesFreeRoamOnly="
         << (config.cop_pursuit_lines_free_roam_only ? 1 : 0) << "\n\n"
         << "; Keep both the driving curses and the police shouts quiet\n"
         << "; while a mission is running. Restart required.\n"
         << "SwearLinesFreeRoamOnly="
         << (config.swear_lines_free_roam_only ? 1 : 0) << "\n\n"
         << "; Stop the game going into slow motion when you aim from a\n"
         << "; vehicle. 0 leaves the slow motion on, 1 turns it off.\n"
         << "DisableDrivebySlowMo=" << (config.disable_driveby_slow_mo ? 1 : 0) << "\n\n"
         << "; Stop the game going into slow motion when you do a stunt.\n"
         << "; 0 leaves the slow motion on, 1 turns it off.\n"
         << "DisableStuntSlowMo=" << (config.disable_stunt_slow_mo ? 1 : 0) << "\n";
    if (config.disable_stunt_sound_fx != defaults.disable_stunt_sound_fx) {
        text << "\n; Stop the whoosh sound that plays when you do a stunt in slow\n"
             << "; motion. The stunt itself and its engine and tyre audio are\n"
             << "; not affected. 0 = off, 1 = muted.\n"
             << "DisableStuntSoundFx=" << (config.disable_stunt_sound_fx ? 1 : 0) << "\n";
    }
    text << "\n; Disables the unsatured screen FX that happens while doing a stunt. Restart required.\n"
         << "DisableStuntScreenFx=" << (config.disable_stunt_screen_fx ? 1 : 0) << "\n\n"
         << "; Police will shout when you shove or hit them, this is a rare occurence.\n"
         << "RestorePoliceVoiceLines=" << (config.restore_police_voice_lines ? 1 : 0) << "\n\n"
         << "; People in the crowd will cheer and shout during a\n"
         << "; fight, this is a rare occurence.\n"
         << "RestoreEncourageVoiceLines="
         << (config.restore_encourage_voice_lines ? 1 : 0) << "\n\n"
         << "; Hide the map while you are on foot, and show it again when\n"
         << "; you get in a vehicle, as in GTA 6.\n"
         << "DynamicRadar=" << (config.dynamic_radar ? 1 : 0) << "\n\n"
         << "; Make a whole day pass at the same speed all day long.\n"
         << "; Normally the game makes night pass faster than day. With\n"
         << "; this on, a full day lasts the number of real minutes set\n"
         << "; below. 0 turns it off, 1 turns it on. Restart required.\n"
         << "GTAVITimeClock=" << (config.gta_vi_time_clock ? 1 : 0) << "\n\n"
         << "; How many real minutes one full day should last, from 1 to\n"
         << "; 1440. 144 minutes means one in game minute every six real\n"
         << "; seconds, as in GTA 6.\n"
         << "GTAVITimeClockDayMinutes="
         << config.gta_vi_time_clock_day_minutes << "\n\n";

    text << "\n[Stability]\n"
         << "; Stop a crash caused by the fog changing too quickly.\n"
         << "FixFogSlicing=" << (config.hook_fog_slicing_guard ? 1 : 0) << "\n";
    if (config.min_fog_slicing_interval != defaults.min_fog_slicing_interval) {
        text << "; Smallest gap the game will allow between fog updates, from\n"
             << "; 1 to 4.\n"
             << "MinimumFogSlicingInterval=" << config.min_fog_slicing_interval << "\n";
    }
    if (config.pool_headroom_mib != defaults.pool_headroom_mib) {
        text << "\n; EXPERIMENTAL. How much memory the engine's Main Pool\n"
             << "; reserves up front, in megabytes. The stock game reserves\n"
             << "; 500. When a request does not fit, the game reserves a\n"
             << "; fresh region for it. When the pool cannot serve a request\n"
             << "; and cannot grow, the game reports an out of memory error\n"
             << "; and closes itself. A larger reservation gives the pool\n"
             << "; more room before that can happen, but it also commits\n"
             << "; more memory when the game starts. This has not been\n"
             << "; tested on a live install. Leave at 0 to keep the stock\n"
             << "; behaviour.\n"
             << "PoolHeadroomMiB=" << config.pool_headroom_mib << "\n";
    }
    text << "\n; Stop the menus breaking after the game has been running for a\n"
         << "; long time.\n"
         << "FixScaleformTimerOverflow=" << (config.fix_scaleform_qpc_clock ? 1 : 0) << "\n\n"
         << "; Fix a fault that stopped existing files being updated properly.\n"
         << "FixFileTimestampUpdates=" << (config.fix_file_timestamp_open_mode ? 1 : 0) << "\n\n"
         << "; Stop a crash when the game looks for sound file information\n"
         << "; that is missing.\n"
         << "FixAudioFileOpen=" << (config.fix_audio_file_open ? 1 : 0) << "\n\n"
         << "; Fix the wrong file size reported for very large files, four\n"
         << "; gigabytes and above.\n"
         << "FixLargeFileSizes=" << (config.fix_large_file_sizes ? 1 : 0) << "\n\n"
         << "; Stop a crash or hang while the game is shutting down its\n"
         << "; graphics memory.\n"
         << "FixVRAMPoolLock=" << (config.fix_vram_pool_lock ? 1 : 0) << "\n\n"
         << "; Stop crashes caused by damaged or incomplete game files, by\n"
         << "; refusing to load them.\n"
         << "FixResourceLoading=" << (config.fix_resource_loading ? 1 : 0) << "\n\n"
         << "; Stop a crash caused by very long names in the friends list.\n"
         << "FixContactListOverflow=" << (config.fix_contact_list_overflow ? 1 : 0) << "\n\n"
         << "; Stop a crash when loading a damaged save.\n"
         << "FixCorruptSaveCrash=" << (config.fix_corrupt_save_handling ? 1 : 0) << "\n\n"
         << "; Stop a crash when the game fails to start a new thread.\n"
         << "FixThreadCreationFailure="
         << (config.fix_thread_creation_failure ? 1 : 0) << "\n\n"
         << "; Stop the game burning one CPU core while it waits for a\n"
         << "; file read to finish in the background.\n"
         << "FixFileLoadCoreSpin="
         << (config.fix_qfile_ready_sleep_yield ? 1 : 0) << "\n\n"
         << "; Stop the game sorting a whole list of combat entries every\n"
         << "; frame when it only needs to remove one of them.\n"
         << "FixCombatListSortChurn="
         << (config.fix_combat_list_selection ? 1 : 0) << "\n\n"
         << "; Stop the game waiting on a network check for an online service\n"
         << "; that no longer exists. The game treats itself as offline, which\n"
         << "; is what you want when playing without an account.\n"
         << "FixOnlineProbeStall=" << (config.fix_online_probe_stall ? 1 : 0) << "\n\n"
         << "; Keep the camera and character movement feeling the same on a\n"
         << "; computer that runs at a high frame rate.\n"
         << "FixFrameCountedActivityTimer="
         << (config.fix_frame_counted_activity_timer ? 1 : 0) << "\n";

    text << "\n[AntiAliasing]\n"
         << "; Smooth out the rough edges you see around objects. This usually\n"
         << "; makes the game look cleaner without making it blurry. 0 turns\n"
         << "; it off. Restart required.\n"
         << "SMAA=" << ((config.hook_smaa_present && config.smaa_enable) ? 1 : 0) << "\n\n"
         << "; How strong the smoothing is: 0 low, 1 medium, 2 high, 3 ultra.\n"
         << "; Ultra looks the best.\n"
         << "SMAAPreset=" << config.smaa_preset << "\n\n"
         << "; 0 uses the setting above. If small bright icons or mission\n"
         << "; markers flicker, raise this a little, for example to 0.2.\n"
         << "; From 0 to 0.5.\n"
         << "SMAAEdgeThreshold=" << config.smaa_edge_threshold << "\n\n"
         << "; Stops small bright icons and mission markers from flickering when using SMAA.\n"
         << "IconStabilizer=" << (config.icon_stabilizer ? 1 : 0) << "\n";

    text << "\n[Debug]\n"
         << "; These are tools for finding problems. Leave them alone unless\n"
         << "; someone asks you to turn one on.\n"
         << ";\n"
         << "; Write a log file called SPatch.log in the same folder as this\n"
         << "; settings file. This is turned off by default. Turn it on if\n"
         << "; someone asks you to send them a log. Restart required.\n"
         << "Logging=" << (config.enable_logging ? 1 : 0) << "\n";
    if (config.diag_onfoot_yaw_probe_events != defaults.diag_onfoot_yaw_probe_events) {
        text << "\n; Leave at 0. Records details about turning the camera on foot\n"
             << "; with a gamepad. Only used when someone is tracking down a\n"
             << "; problem with gamepad turning.\n"
             << "OnFootYawProbeEvents=" << (config.diag_onfoot_yaw_probe_events ? 1 : 0) << "\n";
    }
    if (config.diag_fov_probe != defaults.diag_fov_probe) {
        text << "\n; Leave at 0. Records the field of view the game is using. Only\n"
             << "; used when someone is tracking down a problem with zoom.\n"
             << "FovProbe=" << (config.diag_fov_probe ? 1 : 0) << "\n";
    }
    if (config.diag_blip_probe != defaults.diag_blip_probe) {
        text << "\n; Leave at 0. Records the radar markers as they appear and\n"
             << "; disappear. Only used when someone is tracking down a problem\n"
             << "; with the police markers on the map.\n"
             << "BlipProbe=" << (config.diag_blip_probe ? 1 : 0) << "\n";
    }
    if (config.diag_drivetrain_probe != defaults.diag_drivetrain_probe) {
        text << "\n; Leave at 0. Records what the pedals and gearbox are doing when\n"
             << "; you change direction. Only used when someone is tracking down\n"
             << "; a problem with reversing.\n"
             << "DrivetrainProbe=" << (config.diag_drivetrain_probe ? 1 : 0) << "\n";
    }
    if (config.diag_stimulus_events != defaults.diag_stimulus_events) {
        text << "\n; Leave at 0. Counts which of the engine's 116 stimulus types\n"
             << "; actually reach a pedestrian, so a play session shows whether the\n"
             << "; shipped reactions to a visible weapon are emitted at all.\n"
             << "StimulusProbe=" << (config.diag_stimulus_events ? 1 : 0) << "\n";
    }
    if (config.diag_push_voice_line_events != defaults.diag_push_voice_line_events) {
        text << "\n; Leave at 0. Records the crowd lines that play when you push\n"
             << "; through people. Only used when someone is tracking down a\n"
             << "; problem with those voice lines.\n"
             << "PushVoiceLineEvents=" << (config.diag_push_voice_line_events ? 1 : 0) << "\n";
    }
    if (config.diag_light_submit_events != defaults.diag_light_submit_events) {
        text << "\n; Leave at 0. Records the engine's per-light lighting submits\n"
             << "; (light family, flags, chosen technique) so one night drive\n"
             << "; shows what the vehicle lights look like to the renderer.\n"
             << "LightSubmitEvents=" << (config.diag_light_submit_events ? 1 : 0) << "\n";
    }
    if (config.diag_census_shadow_consumers !=
        defaults.diag_census_shadow_consumers) {
        text << "\n; Leave at 0. Counts which drawing steps use each upgraded\n"
             << "; shadow map, so a soft shadow in one spot can be traced to\n"
             << "; its source. Only used when someone is tracking down a\n"
             << "; shadow problem.\n"
             << "CensusShadowConsumers="
             << (config.diag_census_shadow_consumers ? 1 : 0) << "\n";
    }
    if (config.push_voice_line_voice_id != defaults.push_voice_line_voice_id) {
        text << "\n; Leave at 0. Which voice to use for the crowd lines.\n"
             << "PushVoiceLineVoiceId=" << config.push_voice_line_voice_id << "\n";
    }
    if (config.push_voice_line_reaction_id != defaults.push_voice_line_reaction_id) {
        text << "\n; Leave at 0. Which crowd reaction to play.\n"
             << "PushVoiceLineReactionId=" << config.push_voice_line_reaction_id << "\n";
    }
    if (config.push_voice_line_voice_id_sweep != defaults.push_voice_line_voice_id_sweep) {
        text << "\n; Leave at 0. Tries every possible voice in turn and writes down\n"
             << "; which ones the game accepts.\n"
             << "PushVoiceLineVoiceIdSweep=" << (config.push_voice_line_voice_id_sweep ? 1 : 0) << "\n";
    }
    if (config.diag_combat_line_events != defaults.diag_combat_line_events) {
        text << "\n; Leave at 0. Records the fight lines that play and whether the\n"
             << "; game accepted them.\n"
             << "CombatLineEvents=" << (config.diag_combat_line_events ? 1 : 0) << "\n";
    }
    if (config.diag_npc_voice_line_events != defaults.diag_npc_voice_line_events) {
        text << "\n; Leave at 0. Records the police and crowd lines that play and\n"
             << "; whether the game accepted them.\n"
             << "NpcVoiceLineEvents=" << (config.diag_npc_voice_line_events ? 1 : 0) << "\n";
    }
    if (config.probe_slow_mo != defaults.probe_slow_mo) {
        text << "\n; Leave at 0. Records every time the game changes how fast the\n"
             << "; world moves, and which part of the game asked for it. Only\n"
             << "; used when someone is tracking down a slow motion problem.\n"
             << "SlowMoProbe=" << (config.probe_slow_mo ? 1 : 0) << "\n";
    }
    if (config.probe_screen_filter != defaults.probe_screen_filter) {
        text << "\n; Leave at 0. Records which script asks for a full-screen\n"
             << "; colour wash, and when. Only used when someone is tracking\n"
             << "; down a screen filter problem.\n"
             << "ScreenFilterProbe=" << (config.probe_screen_filter ? 1 : 0) << "\n";
    }
    if (config.probe_action_requests != defaults.probe_action_requests) {
        text << "\n; Leave at 0. Records the actions the game asks Wei to perform,\n"
             << "; such as crouching. Only used when someone is tracking down a\n"
             << "; problem with crouching.\n"
             << "ActionRequestProbe=" << (config.probe_action_requests ? 1 : 0) << "\n";
    }
    text << "\n; Write a small file if the game crashes, so the crash can be\n"
         << "; looked at later. Leave this on.\n"
         << "WriteCrashDumps=" << (config.write_minidumps ? 1 : 0);

    const unsigned long serial = g_config_write_serial.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path temp_path =
        path.parent_path() /
        (path.filename().wstring() + L".spatch." + std::to_wstring(GetCurrentProcessId()) +
         L"." + std::to_wstring(serial) + L".tmp");
    if (!WriteConfigText(temp_path, text.str())) {
        std::error_code error;
        std::filesystem::remove(temp_path, error);
        return false;
    }

    const BOOL replaced = MoveFileExW(temp_path.c_str(),
                                      path.c_str(),
                                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!replaced) {
        std::error_code error;
        std::filesystem::remove(temp_path, error);
        return false;
    }
    // The Win32 profile API caches section data per path.  Invalidate that
    // cache after an atomic replacement so the first post-migration read sees
    // the organized file rather than the legacy contents.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return true;
}

bool ParseBool(const std::optional<std::wstring>& raw, bool fallback) {
    if (!raw.has_value()) {
        return fallback;
    }
    const std::wstring value = TrimIniValue(*raw);
    if (value == L"1" || _wcsicmp(value.c_str(), L"true") == 0 ||
        _wcsicmp(value.c_str(), L"on") == 0 ||
        _wcsicmp(value.c_str(), L"yes") == 0) {
        return true;
    }
    if (value == L"0" || _wcsicmp(value.c_str(), L"false") == 0 ||
        _wcsicmp(value.c_str(), L"off") == 0 ||
        _wcsicmp(value.c_str(), L"no") == 0) {
        return false;
    }
    return fallback;
}

bool ParseBinaryBool(const std::optional<std::wstring>& raw, bool fallback) {
    if (!raw.has_value()) {
        return fallback;
    }
    const std::wstring value = TrimIniValue(*raw);
    if (value == L"1") {
        return true;
    }
    if (value == L"0") {
        return false;
    }
    return fallback;
}

int ParseInt(const std::optional<std::wstring>& raw, int fallback) {
    if (!raw.has_value()) {
        return fallback;
    }
    const std::wstring value = TrimIniValue(*raw);
    if (value.empty()) {
        return fallback;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const long long parsed = std::wcstoll(value.c_str(), &end, 10);
    if (errno == ERANGE || end == value.c_str() || *end != L'\0' ||
        parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) {
        return fallback;
    }
    return static_cast<int>(parsed);
}

float ParseFloat(const std::optional<std::wstring>& raw, float fallback) {
    if (!raw.has_value()) {
        return fallback;
    }
    const std::wstring value = TrimIniValue(*raw);
    if (value.empty()) {
        return fallback;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const float parsed = std::wcstof(value.c_str(), &end);
    if (errno == ERANGE || end == value.c_str() || *end != L'\0' ||
        !std::isfinite(parsed)) {
        return fallback;
    }
    return parsed;
}

bool ReadBool(const std::filesystem::path& path, const wchar_t* key, bool fallback) {
    return ParseBool(ReadConfiguredValue(path, key), fallback);
}

// v50 renames the Wei voice-line keys for clarity: RestoreWeiVoiceLines ->
// RestoreCombatLines and WeiVoiceLineEvents -> CombatLineEvents. Prefer the
// new spellings, then preserve the v49 names during migration so a user's
// enabled feature survives the rename.
bool ReadCombatLinesBool(const std::filesystem::path& path, bool fallback) {
    if (auto value = ReadRawValue(path, kGameplaySection, L"RestoreCombatLines")) {
        return ParseBool(value, fallback);
    }
    return ParseBool(
        ReadRawValue(path, kGameplaySection, L"RestoreWeiVoiceLines"), fallback);
}

bool ReadCombatLineEventsBool(const std::filesystem::path& path, bool fallback) {
    if (auto value = ReadRawValue(path, kDebugSection, L"CombatLineEvents")) {
        return ParseBool(value, fallback);
    }
    return ParseBool(
        ReadRawValue(path, kDebugSection, L"WeiVoiceLineEvents"), fallback);
}

// v70 renamed the round75 keys (Restore*Shadows -> the plain feature names);
// these helpers keep a v69 file's armed values during migration.
bool ReadHeadlightShadowsValue(const std::filesystem::path& path, bool fallback) {
    if (auto value = ReadRawValue(path, kShadowSection, L"HeadlightShadows")) {
        return ParseBool(value, fallback);
    }
    return ParseBool(
        ReadRawValue(path, kShadowSection, L"RestoreHeadlightShadows"), fallback);
}

bool ReadSirenShadowsValue(const std::filesystem::path& path, bool fallback) {
    if (auto value = ReadRawValue(path, kShadowSection, L"SirenShadows")) {
        return ParseBool(value, fallback);
    }
    return ParseBool(
        ReadRawValue(path, kShadowSection, L"RestoreSirenShadows"), fallback);
}

int ReadInt(const std::filesystem::path& path, const wchar_t* key, int fallback) {
    return ParseInt(ReadConfiguredValue(path, key), fallback);
}

float ReadFloat(const std::filesystem::path& path, const wchar_t* key, float fallback) {
    return ParseFloat(ReadConfiguredValue(path, key), fallback);
}

unsigned long ReadUInt(const std::filesystem::path& path,
                       const wchar_t* key,
                       unsigned long fallback) {
    const auto raw = ReadConfiguredValue(path, key);
    if (!raw.has_value()) {
        return fallback;
    }
    const std::wstring value = TrimIniValue(*raw);
    if (value.empty() || value.front() == L'-') {
        return fallback;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::wcstoull(value.c_str(), &end, 10);
    if (errno == ERANGE || end == value.c_str() || *end != L'\0' ||
        parsed > std::numeric_limits<unsigned long>::max()) {
        return fallback;
    }
    return static_cast<unsigned long>(parsed);
}

void RetireLegacyDeveloperControls(Config& config) {
    // Config v6 deliberately removes raw hook/probe/debug switches from the
    // end-user contract. Stable features derive their hook dependencies from
    // the public feature switches, so carrying these implementation controls
    // into the migrated file would recreate invalid combinations and the old
    // developer-facing INI. The untouched source remains in the versioned
    // external config-backup directory.
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
    config.hook_aa_probe = false;
    config.hook_aa_fx_probe = false;
    config.aa_variant_debug_keys = false;
    config.aa_aux_debug_keys = false;
    config.smaa_debug_keys = false;
    config.hook_post_material_submit = false;
    config.hook_character_regression_probe = false;
    config.hook_pass8_probe = false;
    config.aa_variant_mode = 0;
    config.aa_aux_mode = 0;
}

}  // namespace

std::filesystem::path ConfigBackupPath(
    const std::filesystem::path& config_path) {
    return BuildExternalBackupPath(
        config_path,
        L"SPatch-pre-v" + std::to_wstring(kConfigVersion) + L".ini");
}

std::filesystem::path LegacyConfigBackupPath(
    const std::filesystem::path& config_path) {
    return BuildExternalBackupPath(config_path, L"SPatch-legacy-previous.ini");
}

const char* ConfigPersistenceStatusName(ConfigPersistenceStatus status) noexcept {
    switch (status) {
        case ConfigPersistenceStatus::Unchanged:
            return "unchanged";
        case ConfigPersistenceStatus::Created:
            return "created";
        case ConfigPersistenceStatus::Migrated:
            return "migrated";
        case ConfigPersistenceStatus::CreateFailed:
            return "create_failed";
        case ConfigPersistenceStatus::BackupFailed:
            return "backup_failed";
        case ConfigPersistenceStatus::MigrationWriteFailed:
            return "migration_write_failed";
        case ConfigPersistenceStatus::SourceChanged:
            return "source_changed";
        case ConfigPersistenceStatus::SourceUnavailable:
            return "source_unavailable";
    }
    return "unknown";
}

EarlyConfig ReadEarlyConfig(const std::filesystem::path& path) {
    // The loader phase reads the file before the full load has run, so the
    // profile cache is dropped first: the cache is validated against the
    // file's timestamp, and a file edited in the same second as the launch
    // would otherwise be read from the cache.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());

    EarlyConfig early;
    early.enabled = ReadBool(path, L"enabled", early.enabled);
    early.pool_headroom_mib =
        ReadInt(path, L"pool_headroom_mib", early.pool_headroom_mib);
    // The same clamp the full load applies, so the two paths cannot read
    // the same file as two different values.
    if (early.pool_headroom_mib < kPoolHeadroomMinMiB) {
        early.pool_headroom_mib = kPoolHeadroomMinMiB;
    } else if (early.pool_headroom_mib > kPoolHeadroomMaxMiB) {
        early.pool_headroom_mib = kPoolHeadroomMaxMiB;
    }
    return early;
}

Config LoadConfig(const std::filesystem::path& path, ConfigLoadReport* report) {
    std::lock_guard<std::mutex> lock(g_config_write_mutex);
    ConfigLoadReport load_report{};
    load_report.source_file_existed = PathExistsNoThrow(path);
    if (!load_report.source_file_existed) {
        load_report.persistence = WriteOrganizedConfig(path, Config{})
                                      ? ConfigPersistenceStatus::Created
                                      : ConfigPersistenceStatus::CreateFailed;
    }

    // Callers may have edited the file with stdio or an atomic replacement
    // since the last profile-API read.  Drop the Win32 profile cache once per
    // load so tests and real migrations observe the bytes currently on disk.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());

    FileIdentity source_identity{};
    const bool source_identity_valid =
        ReadFileIdentity(path, source_identity);
    if (load_report.source_file_existed && !source_identity_valid) {
        load_report.persistence = ConfigPersistenceStatus::SourceUnavailable;
    }

    Config config;
    const int config_version = ReadInt(path, L"config_version", 0);
    load_report.source_version = config_version;
    config.enabled = ReadBool(path, L"enabled", config.enabled);
    if (ReadConfiguredValue(path, L"allow_unverified_build").has_value()) {
        config.allow_unverified_build =
            ReadBool(path, L"allow_unverified_build", config.allow_unverified_build);
    } else {
        // Config v5 and earlier used a name that incorrectly implied unsafe
        // fixed-address hooks would be enabled. Preserve the value while
        // migrating it to the truthful build-independent-only control.
        config.allow_unverified_build =
            ReadBool(path, L"allow_unsupported_build", config.allow_unverified_build);
    }
    config.write_minidumps =
        ParseBool(ReadConfiguredValue(path, L"write_minidumps"),
                  config.write_minidumps);
    config.enable_logging = ParseBool(ReadLoggingValue(path), config.enable_logging);
    config.hook_queue_ready = ReadBool(path, L"hook_queue_ready", config.hook_queue_ready);
    config.hook_task_dispatch =
        ReadBool(path, L"hook_task_dispatch", config.hook_task_dispatch);
    config.hook_wait_helper = ReadBool(path, L"hook_wait_helper", config.hook_wait_helper);
    config.hook_scaleform_time =
        ReadBool(path, L"hook_scaleform_time", config.hook_scaleform_time);
    config.hook_scaleform_init =
        ReadBool(path, L"hook_scaleform_init", config.hook_scaleform_init);
    config.hook_nis_timing =
        ReadBool(path, L"hook_nis_timing", config.hook_nis_timing);
    config.hook_nis_runtime =
        ReadBool(path, L"hook_nis_runtime", config.hook_nis_runtime);
    config.hook_nis_owner =
        ReadBool(path, L"hook_nis_owner", config.hook_nis_owner);
    config.hook_nis_actor_state =
        ReadBool(path, L"hook_nis_actor_state", config.hook_nis_actor_state);
    config.hook_twitch_probe =
        ReadBool(path, L"hook_twitch_probe", config.hook_twitch_probe);
    config.hook_frameflow =
        ReadBool(path, L"hook_frameflow", config.hook_frameflow);
    config.fix_cutscene_zero_dt =
        ReadBool(path, L"fix_cutscene_zero_dt", config.fix_cutscene_zero_dt);
    // The timeline rewrite is deliberately NOT read from the FixCutsceneFPS
    // master. It is the repair that deforms props, so it stays opt-in.
    config.override_cutscene_zero_dt =
        ReadInt(path, L"override_cutscene_zero_dt", config.override_cutscene_zero_dt);
    config.override_cutscene_scene_time =
        ReadInt(path, L"override_cutscene_scene_time", config.override_cutscene_scene_time);
    if (config.override_cutscene_zero_dt >= 0) {
        config.fix_cutscene_zero_dt = config.override_cutscene_zero_dt != 0;
    }
    // Anything that is not a positive value leaves the rewrite off. -1 is the
    // "follow the master" spelling from the first release of this key, and it
    // must not switch the rewrite back on now that the master no longer owns it.
    config.fix_cutscene_scene_time_step = config.override_cutscene_scene_time > 0;
    config.fix_nis_actor_restore_duplicates = ReadBool(
        path, L"fix_nis_actor_restore_duplicates", config.fix_nis_actor_restore_duplicates);
    const bool has_time_step_smoothing =
        ReadConfiguredValue(path, L"time_step_smoothing").has_value();
    if (has_time_step_smoothing) {
        config.time_step_smoothing =
            ReadInt(path, L"time_step_smoothing", config.time_step_smoothing);
    } else {
        const bool legacy_disable =
            ReadBool(path, L"disable_time_step_smoothing", false);
        const bool has_legacy_frames =
            ReadConfiguredValue(path, L"time_step_smoothing_frames").has_value();
        if (legacy_disable) {
            config.time_step_smoothing = 0;
        } else if (has_legacy_frames) {
            const int legacy_frames = ReadInt(path, L"time_step_smoothing_frames", 2);
            config.time_step_smoothing = legacy_frames < 0 ? -1 : (legacy_frames < 2 ? 0 : 1);
        }
    }
    config.hook_scenery_builders =
        ReadBool(path, L"hook_scenery_builders", config.hook_scenery_builders);
    config.hook_fog_slicing_guard =
        ReadBool(path, L"hook_fog_slicing_guard", config.hook_fog_slicing_guard);
    config.hook_aa_probe =
        ReadBool(path, L"hook_aa_probe", config.hook_aa_probe);
    config.hook_aa_fx_probe =
        ReadBool(path, L"hook_aa_fx_probe", config.hook_aa_fx_probe);
    config.aa_variant_debug_keys =
        ReadBool(path, L"aa_variant_debug_keys", config.aa_variant_debug_keys);
    config.aa_aux_debug_keys =
        ReadBool(path, L"aa_aux_debug_keys", config.aa_aux_debug_keys);
    config.hook_smaa_present =
        ReadBool(path, L"hook_smaa_present", config.hook_smaa_present);
    config.smaa_enable =
        ReadBool(path, L"smaa_enable", config.smaa_enable);
    config.smaa_debug_keys =
        ReadBool(path, L"smaa_debug_keys", config.smaa_debug_keys);
    // SMAA owns hook installation: the public SMAA switch drives hook
    // ownership. Stock-AA suppression always follows SMAA: stacking the
    // game's FXAA in front of SMAA was tried for the world-icon flicker and
    // re-broke the icons (SMAA re-processes and re-sharpens them), so there
    // is no user-facing override anymore. When SMAA is disabled, stock AA
    // always runs.
    config.hook_smaa_present = config.smaa_enable;
    config.smaa_disable_stock_aa = config.smaa_enable;
    config.hook_post_material_submit =
        ReadBool(path, L"hook_post_material_submit", config.hook_post_material_submit);
    config.hook_character_regression_probe =
        ReadBool(path, L"hook_character_regression_probe", config.hook_character_regression_probe);
    config.hook_pass8_probe =
        ReadBool(path, L"hook_pass8_probe", config.hook_pass8_probe);
    config.force_raw_mouse_input =
        ReadBool(path, L"force_raw_mouse_input", config.force_raw_mouse_input);
    config.disable_camera_smoothing =
        ReadBool(path, L"disable_camera_smoothing", config.disable_camera_smoothing);
    config.gta_iv_car_camera = ParseBinaryBool(
        ReadConfiguredValue(path, L"gta_iv_car_camera"),
        config.gta_iv_car_camera);
    config.gta_iv_bike_camera = ParseBinaryBool(
        ReadConfiguredValue(path, L"gta_iv_bike_camera"),
        config.gta_iv_bike_camera);
    config.disable_pursuit_camera = ParseBinaryBool(
        ReadConfiguredValue(path, L"disable_pursuit_camera"),
        config.disable_pursuit_camera);
    config.fix_chase_aim_camera = ParseBinaryBool(
        ReadConfiguredValue(path, L"fix_chase_aim_camera"),
        config.fix_chase_aim_camera);
    config.hide_cop_radar_icons = ParseBinaryBool(
        ReadConfiguredValue(path, L"hide_cop_radar_icons"),
        config.hide_cop_radar_icons);
    config.modern_brake_reverse = ParseBinaryBool(
        ReadConfiguredValue(path, L"modern_brake_reverse"),
        config.modern_brake_reverse);
    config.no_wheel_turn_back = ParseBinaryBool(
        ReadConfiguredValue(path, L"no_wheel_turn_back"),
        config.no_wheel_turn_back);
    config.disable_controller_camera_autocenter = ParseBinaryBool(
        ReadConfiguredValue(path, L"disable_controller_camera_autocenter"),
        config.disable_controller_camera_autocenter);
    config.disable_onfoot_camera_autocenter = ParseBinaryBool(
        ReadConfiguredValue(path, L"disable_onfoot_camera_autocenter"),
        config.disable_onfoot_camera_autocenter);
    config.gtav_speed_shake = ParseBinaryBool(
        ReadConfiguredValue(path, L"gtav_speed_shake"),
        config.gtav_speed_shake);
    config.gtav_speed_shake_strength_percent = ReadInt(
        path, L"gtav_speed_shake_strength",
        config.gtav_speed_shake_strength_percent);
    config.gtav_speed_shake_start_speed_kmh = ReadInt(
        path, L"gtav_speed_shake_start_speed_kmh",
        config.gtav_speed_shake_start_speed_kmh);
    config.gtav_speed_shake_full_speed_kmh = ReadInt(
        path, L"gtav_speed_shake_full_speed_kmh",
        config.gtav_speed_shake_full_speed_kmh);
    config.speed_rumble =
        ReadBool(path, L"speed_rumble", config.speed_rumble);
    config.speed_rumble_start_kmh =
        ReadInt(path, L"speed_rumble_start_kmh", config.speed_rumble_start_kmh);
    config.speed_rumble_full_kmh =
        ReadInt(path, L"speed_rumble_full_kmh", config.speed_rumble_full_kmh);
    config.speed_rumble_strength_percent =
        ReadInt(path, L"speed_rumble_strength_percent",
                config.speed_rumble_strength_percent);
    config.controller_left_stick_deadzone = ReadInt(
        path, L"controller_left_stick_deadzone", config.controller_left_stick_deadzone);
    config.controller_right_stick_deadzone = ReadInt(
        path, L"controller_right_stick_deadzone", config.controller_right_stick_deadzone);
    config.restore_original_fog =
        ParseBool(ReadOriginalFogAndNeonValue(path), config.restore_original_fog);
    config.disable_dof =
        ReadBool(path, L"disable_dof", config.disable_dof);
    config.disable_cutscene_dof =
        ReadBool(path, L"disable_cutscene_dof", config.disable_cutscene_dof);
    config.restore_original_eye_reflections = ReadBool(
        path, L"restore_original_eye_reflections", config.restore_original_eye_reflections);
    config.restore_character_wetness =
        ReadBool(path, L"restore_character_wetness", config.restore_character_wetness);
    config.restore_character_sweat =
        ReadBool(path, L"restore_character_sweat", config.restore_character_sweat);
    config.wetness_full_time_seconds =
        ReadInt(path, L"wetness_full_time_seconds", config.wetness_full_time_seconds);
    config.wetness_fade_time_seconds =
        ReadInt(path, L"wetness_fade_time_seconds", config.wetness_fade_time_seconds);
    config.sweat_build_time_seconds =
        ReadInt(path, L"sweat_build_time_seconds", config.sweat_build_time_seconds);
    config.sweat_fade_time_seconds =
        ReadInt(path, L"sweat_fade_time_seconds", config.sweat_fade_time_seconds);
    config.sweat_onset_time_seconds =
        ReadInt(path, L"sweat_onset_time_seconds", config.sweat_onset_time_seconds);
    config.sweat_run_speed =
        ReadFloat(path, L"sweat_run_speed", config.sweat_run_speed);
    config.sweat_combat_time_seconds =
        ReadInt(path, L"sweat_combat_time_seconds", config.sweat_combat_time_seconds);
    config.restore_push_voice_lines =
        ReadBool(path, L"restore_push_voice_lines", config.restore_push_voice_lines);
    config.restore_push_voice_lines_free_roam_only =
        ReadBool(path, L"restore_push_voice_lines_free_roam_only",
                 config.restore_push_voice_lines_free_roam_only);
    config.restore_combat_lines =
        ReadCombatLinesBool(path, config.restore_combat_lines);
    config.restore_combat_lines_free_roam_only =
        ReadBool(path, L"restore_combat_lines_free_roam_only",
                 config.restore_combat_lines_free_roam_only);
    config.restore_combat_lines_all_voices =
        ReadBool(path, L"restore_combat_lines_all_voices",
                 config.restore_combat_lines_all_voices);
    config.driving_swear_lines =
        ReadBool(path, L"driving_swear_lines", config.driving_swear_lines);
    config.driving_swear_lines_free_roam_only =
        ReadBool(path, L"driving_swear_lines_free_roam_only",
                 config.driving_swear_lines_free_roam_only);
    // Precedence: CopPursuitLines > CopsPersuitLines > CopPursuitSwearLines.
    config.cop_pursuit_swear_lines =
        ReadBool(path, L"cop_pursuit_lines", config.cop_pursuit_swear_lines);
    if (!ReadConfiguredValue(path, L"cop_pursuit_lines").has_value()) {
        config.cop_pursuit_swear_lines =
            ReadBool(path, L"cops_persuit_lines", config.cop_pursuit_swear_lines);
        if (!ReadConfiguredValue(path, L"cops_persuit_lines").has_value()) {
            config.cop_pursuit_swear_lines = ReadBool(
                path, L"cop_pursuit_swear_lines", config.cop_pursuit_swear_lines);
        }
    }
    config.cop_pursuit_lines_free_roam_only =
        ReadBool(path, L"cop_pursuit_lines_free_roam_only",
                 config.cop_pursuit_lines_free_roam_only);
    config.swear_lines_free_roam_only =
        ReadBool(path, L"swear_lines_free_roam_only",
                 config.swear_lines_free_roam_only);
    config.disable_driveby_slow_mo =
        ReadBool(path, L"disable_driveby_slow_mo", config.disable_driveby_slow_mo);
    config.disable_stunt_slow_mo =
        ReadBool(path, L"disable_stunt_slow_mo", config.disable_stunt_slow_mo);
    config.disable_stunt_sound_fx =
        ReadBool(path, L"disable_stunt_sound_fx", config.disable_stunt_sound_fx);
    config.disable_stunt_screen_fx =
        ReadBool(path, L"disable_stunt_screen_fx", config.disable_stunt_screen_fx);
    config.diag_push_voice_line_events =
        ReadBool(path, L"diag_push_voice_line_events", config.diag_push_voice_line_events);
    config.diag_combat_line_events =
        ReadCombatLineEventsBool(path, config.diag_combat_line_events);
    config.restore_police_voice_lines =
        ReadBool(path, L"restore_police_voice_lines", config.restore_police_voice_lines);
    config.restore_encourage_voice_lines = ReadBool(
        path, L"restore_encourage_voice_lines", config.restore_encourage_voice_lines);
    config.gta_vi_time_clock = ReadBool(
        path, L"gta_vi_time_clock", config.gta_vi_time_clock);
    config.gta_vi_time_clock_day_minutes = ReadInt(
        path, L"gta_vi_time_clock_day_minutes",
        config.gta_vi_time_clock_day_minutes);
    config.diag_npc_voice_line_events =
        ReadBool(path, L"diag_npc_voice_line_events", config.diag_npc_voice_line_events);
    config.diag_onfoot_yaw_probe_events = ReadBool(
        path, L"diag_onfoot_yaw_probe_events", config.diag_onfoot_yaw_probe_events);
    config.diag_fov_probe =
        ReadBool(path, L"diag_fov_probe", config.diag_fov_probe);
    config.diag_blip_probe =
        ReadBool(path, L"diag_blip_probe", config.diag_blip_probe);
    config.diag_drivetrain_probe =
        ReadBool(path, L"diag_drivetrain_probe",
                 config.diag_drivetrain_probe);
    config.diag_stimulus_events =
        ReadBool(path, L"diag_stimulus_events", config.diag_stimulus_events);
    config.stimulus_init_emitter_probe =
        ReadBool(path, L"stimulus_init_emitter_probe",
                 config.stimulus_init_emitter_probe);
    config.push_voice_line_voice_id =
        ReadUInt(path, L"push_voice_line_voice_id", config.push_voice_line_voice_id);
    config.push_voice_line_reaction_id =
        ReadUInt(path, L"push_voice_line_reaction_id", config.push_voice_line_reaction_id);
    config.push_voice_line_voice_id_sweep =
        ReadBool(path, L"push_voice_line_voice_id_sweep", config.push_voice_line_voice_id_sweep);
    config.headlight_shadows =
        ReadHeadlightShadowsValue(path, config.headlight_shadows);
    config.siren_shadows =
        ReadSirenShadowsValue(path, config.siren_shadows);
    config.headlight_beam_light =
        ReadBool(path, L"headlight_beam_light", config.headlight_beam_light);
    config.headlight_beam_radius =
        ReadFloat(path, L"headlight_beam_radius", config.headlight_beam_radius);
    config.headlight_beam_color_r =
        ReadFloat(path, L"headlight_beam_color_r", config.headlight_beam_color_r);
    config.headlight_beam_color_g =
        ReadFloat(path, L"headlight_beam_color_g", config.headlight_beam_color_g);
    config.headlight_beam_color_b =
        ReadFloat(path, L"headlight_beam_color_b", config.headlight_beam_color_b);
    config.headlight_beam_radius =
        std::max(kHeadlightBeamRadiusMin,
                 std::min(kHeadlightBeamRadiusMax,
                          config.headlight_beam_radius));
    config.headlight_beam_color_r =
        std::max(0.0f, std::min(kHeadlightBeamColorMax,
                                config.headlight_beam_color_r));
    config.headlight_beam_color_g =
        std::max(0.0f, std::min(kHeadlightBeamColorMax,
                                config.headlight_beam_color_g));
    config.headlight_beam_color_b =
        std::max(0.0f, std::min(kHeadlightBeamColorMax,
                                config.headlight_beam_color_b));
    config.headlight_shadow_light_type =
        ReadInt(path, L"headlight_shadow_light_type", config.headlight_shadow_light_type);
    config.siren_shadow_light_type =
        ReadInt(path, L"siren_shadow_light_type", config.siren_shadow_light_type);
    // An out-of-range light-type filter would silently match nothing, so
    // clamp both to the light byte's representable range at load time.
    config.headlight_shadow_light_type =
        std::max(kVehicleShadowLightTypeMin,
                 std::min(kVehicleShadowLightTypeMax,
                          config.headlight_shadow_light_type));
    config.siren_shadow_light_type =
        std::max(kVehicleShadowLightTypeMin,
                 std::min(kVehicleShadowLightTypeMax,
                          config.siren_shadow_light_type));
    config.diag_light_submit_events =
        ReadBool(path, L"diag_light_submit_events", config.diag_light_submit_events);
    config.diag_census_shadow_consumers =
        ReadBool(path, L"diag_census_shadow_consumers",
                 config.diag_census_shadow_consumers);
    const bool obsolete_shadow_settings = HasRetiredShadowSettings(path);
    config.fix_pedestrian_density_at_high_fps = ReadBool(
        path,
        L"fix_pedestrian_density_at_high_fps",
        config.fix_pedestrian_density_at_high_fps);
    config.fix_high_fps_average_windows = ReadBool(
        path, L"fix_high_fps_average_windows", config.fix_high_fps_average_windows);
    config.improve_spherical_reflections = ReadBool(
        path, L"improve_spherical_reflections", config.improve_spherical_reflections);
    config.fix_vram_capacity_reporting = ReadBool(
        path, L"fix_vram_capacity_reporting", config.fix_vram_capacity_reporting);
    config.remove_hidden_120_fps_cap =
        ReadBool(path, L"remove_hidden_120_fps_cap", config.remove_hidden_120_fps_cap);
    config.fix_first_run_resolution =
        ReadBool(path, L"fix_first_run_resolution", config.fix_first_run_resolution);
    config.fix_scaleform_qpc_clock =
        ReadBool(path, L"fix_scaleform_qpc_clock", config.fix_scaleform_qpc_clock);
    config.fix_file_timestamp_open_mode = ReadBool(
        path, L"fix_file_timestamp_open_mode", config.fix_file_timestamp_open_mode);
    config.fix_audio_file_open =
        ReadBool(path, L"fix_audio_file_open", config.fix_audio_file_open);
    config.fix_large_file_sizes =
        ReadBool(path, L"fix_large_file_sizes", config.fix_large_file_sizes);
    config.fix_vram_pool_lock =
        ReadBool(path, L"fix_vram_pool_lock", config.fix_vram_pool_lock);
    config.fix_resource_loading =
        ReadBool(path, L"fix_resource_loading", config.fix_resource_loading);
    config.fix_contact_list_overflow = ReadBool(
        path, L"fix_contact_list_overflow", config.fix_contact_list_overflow);
    config.fix_corrupt_save_handling =
        ReadBool(path, L"fix_corrupt_save_handling", config.fix_corrupt_save_handling);
    config.fix_thread_creation_failure =
        ReadBool(path, L"fix_thread_creation_failure", config.fix_thread_creation_failure);
    config.fix_qfile_ready_sleep_yield =
        ReadBool(path, L"fix_qfile_ready_sleep_yield",
                 config.fix_qfile_ready_sleep_yield);
    config.fix_combat_list_selection =
        ReadBool(path, L"fix_combat_list_selection",
                 config.fix_combat_list_selection);
    config.fix_online_probe_stall =
        ReadBool(path, L"fix_online_probe_stall", config.fix_online_probe_stall);
    config.fix_frame_counted_activity_timer =
        ReadBool(path, L"fix_frame_counted_activity_timer",
                 config.fix_frame_counted_activity_timer);
    config.warn_low_res_buffer =
        ReadBool(path, L"warn_low_res_buffer", config.warn_low_res_buffer);
    config.min_fog_slicing_interval =
        ReadInt(path, L"min_fog_slicing_interval", config.min_fog_slicing_interval);
    config.pool_headroom_mib =
        ReadInt(path, L"pool_headroom_mib", config.pool_headroom_mib);
    config.override_low_res_buffer =
        ReadInt(path, L"override_low_res_buffer", config.override_low_res_buffer);
    config.override_shadow_filter = ParseInt(
        ReadOriginalShadowFilterValue(path), config.override_shadow_filter);

    // ShadowScale reads this key
    // from SPatch.ini itself; the alias above keeps the writer preserving it.
    config.shadow_resolution =
        ParseInt(ReadConfiguredValue(path, L"shadow_resolution"),
                 config.shadow_resolution);
    config.custom_frame_limiter =
        ReadBool(path, L"custom_frame_limiter", config.custom_frame_limiter);
    config.custom_frame_limiter_fps =
        ReadFloat(path, L"custom_frame_limiter_fps",
                  config.custom_frame_limiter_fps);
    config.override_texture_detail_level =
        ReadInt(path, L"override_texture_detail_level", config.override_texture_detail_level);
    const auto anisotropic_filtering = ReadTextureFilteringValue(
        path, L"AnisotropicFiltering", L"anisotropic_filtering");
    if (anisotropic_filtering.has_value()) {
        const int parsed = ParseInt(anisotropic_filtering, -1);
        config.anisotropic_filtering =
            parsed == -1 || parsed == 4 || parsed == 8 || parsed == 16
                ? parsed
                : -1;
    }
    config.force_anisotropic_filtering = ParseBool(
        ReadTextureFilteringValue(path,
                                  L"ForceAnisotropicFiltering",
                                  L"force_anisotropic_filtering"),
        config.force_anisotropic_filtering);
    config.override_motion_blur =
        ReadInt(path, L"override_motion_blur", config.override_motion_blur);
    const bool obsolete_renderer_keys =
        ReadRendererValue(path, L"RendererBackend", L"renderer_backend").has_value() ||
        ReadRendererValue(path, L"SwapChainFlipModel",
                          L"swap_chain_flip_model").has_value() ||
        ReadRendererValue(path, L"SwapChainTearing",
                          L"swap_chain_tearing").has_value() ||
        ReadRendererValue(path, L"SwapChainFrameLatency",
                          L"swap_chain_frame_latency").has_value();
    const bool obsolete_unverified_keys =
        HasRetiredUnverifiedSettings(path);
    config.override_world_density =
        ReadInt(path, L"override_world_density", config.override_world_density);
    config.prefer_max_refresh_rate =
        ReadBool(path, L"prefer_max_refresh_rate", config.prefer_max_refresh_rate);
    config.override_fullscreen =
        ReadInt(path, L"override_fullscreen", config.override_fullscreen);
    config.override_rumble_enabled =
        ReadInt(path, L"override_rumble_enabled", config.override_rumble_enabled);
    config.spherical_reflection_width =
        ReadInt(path, L"spherical_reflection_width", config.spherical_reflection_width);
    config.aa_variant_mode =
        ReadInt(path, L"aa_variant_mode", config.aa_variant_mode);
    config.aa_aux_mode =
        ReadInt(path, L"aa_aux_mode", config.aa_aux_mode);
    config.smaa_preset =
        ReadInt(path, L"smaa_preset", config.smaa_preset);
    config.smaa_edge_threshold =
        ReadFloat(path, L"smaa_edge_threshold", config.smaa_edge_threshold);
    config.icon_stabilizer =
        ReadBool(path, L"icon_stabilizer", config.icon_stabilizer);
    config.hide_world_markers_probe =
        ReadBool(path, L"hide_world_markers_probe",
                 config.hide_world_markers_probe);
    config.probe_action_requests =
        ReadBool(path, L"probe_action_requests", config.probe_action_requests);
    config.probe_slow_mo =
        ReadBool(path, L"probe_slow_mo", config.probe_slow_mo);
    config.probe_screen_filter =
        ReadBool(path, L"probe_screen_filter", config.probe_screen_filter);
    config.dynamic_radar =
        ReadBool(path, L"dynamic_radar", config.dynamic_radar);
    config.agx_enabled =
        ReadBool(path, L"agx_enable", config.agx_enabled);
    config.agx_strength =
        ReadInt(path, L"agx_strength_percent", config.agx_strength);
    config.agx_exposure =
        ReadInt(path, L"agx_exposure_scale_percent", config.agx_exposure);
    config.agx_look = ParseAgxLookValue(
        ReadConfiguredValue(path, L"agx_look"), config.agx_look);
    config.agx_debug_bloom =
        ReadInt(path, L"agx_debug_bloom", config.agx_debug_bloom);
    config.summary_interval_ms =
        ReadUInt(path, L"summary_interval_ms", config.summary_interval_ms);
    config.max_verbose_events =
        ReadUInt(path, L"max_verbose_events", config.max_verbose_events);
    config.max_unique_callbacks =
        ReadUInt(path, L"max_unique_callbacks", config.max_unique_callbacks);
    config.cutscene_fps = ReadInt(path, L"cutscene_fps", config.cutscene_fps);
    if (config.summary_interval_ms > kMaxSummaryIntervalMs) {
        config.summary_interval_ms = kMaxSummaryIntervalMs;
    }
    if (config.max_verbose_events > kMaxVerboseEvents) {
        config.max_verbose_events = kMaxVerboseEvents;
    }
    if (config.max_unique_callbacks > kMaxUniqueCallbacks) {
        config.max_unique_callbacks = kMaxUniqueCallbacks;
    }

    if (config.wetness_full_time_seconds < kWetnessTimeMinSeconds) {
        config.wetness_full_time_seconds = kWetnessTimeMinSeconds;
    } else if (config.wetness_full_time_seconds > kWetnessTimeMaxSeconds) {
        config.wetness_full_time_seconds = kWetnessTimeMaxSeconds;
    }
    if (config.wetness_fade_time_seconds < kWetnessTimeMinSeconds) {
        config.wetness_fade_time_seconds = kWetnessTimeMinSeconds;
    } else if (config.wetness_fade_time_seconds > kWetnessTimeMaxSeconds) {
        config.wetness_fade_time_seconds = kWetnessTimeMaxSeconds;
    }
    if (config.sweat_build_time_seconds < kSweatTimeMinSeconds) {
        config.sweat_build_time_seconds = kSweatTimeMinSeconds;
    } else if (config.sweat_build_time_seconds > kSweatTimeMaxSeconds) {
        config.sweat_build_time_seconds = kSweatTimeMaxSeconds;
    }
    if (config.sweat_fade_time_seconds < kSweatTimeMinSeconds) {
        config.sweat_fade_time_seconds = kSweatTimeMinSeconds;
    } else if (config.sweat_fade_time_seconds > kSweatTimeMaxSeconds) {
        config.sweat_fade_time_seconds = kSweatTimeMaxSeconds;
    }
    if (config.sweat_onset_time_seconds < kSweatTimeMinSeconds) {
        config.sweat_onset_time_seconds = kSweatTimeMinSeconds;
    } else if (config.sweat_onset_time_seconds > kSweatTimeMaxSeconds) {
        config.sweat_onset_time_seconds = kSweatTimeMaxSeconds;
    }
    if (!std::isfinite(config.sweat_run_speed) ||
        config.sweat_run_speed < kSweatRunSpeedMin) {
        config.sweat_run_speed = kSweatRunSpeedMin;
    } else if (config.sweat_run_speed > kSweatRunSpeedMax) {
        config.sweat_run_speed = kSweatRunSpeedMax;
    }
    if (config.sweat_combat_time_seconds < kSweatTimeMinSeconds) {
        config.sweat_combat_time_seconds = kSweatTimeMinSeconds;
    } else if (config.sweat_combat_time_seconds > kSweatTimeMaxSeconds) {
        config.sweat_combat_time_seconds = kSweatTimeMaxSeconds;
    }

    const bool obsolete_pcss_keys = HasRetiredPcssSettings(path);
    const bool migrated_shenlong_keys = HasMigratedShenLongSettings(path);
    const bool obsolete_logging_keys = HasRetiredLoggingSettings(path);
    const bool obsolete_crash_dump_location =
        HasRetiredCrashDumpLocation(path);
    const bool obsolete_texture_filtering_location =
        HasRetiredTextureFilteringLocation(path);
    const bool legacy_in_game_backup = PathExistsNoThrow(
        std::filesystem::path(path.wstring() + L".previous.bak"));
    const bool obsolete_visual_damage_keys =
        HasRetiredVisualDamageSettings(path);
    const bool obsolete_ped_density_keys =
        HasRetiredPedDensitySettings(path);
    const bool obsolete_nfs2015_keys =
        HasRetiredNfs2015Settings(path);
    if (config_version < kConfigVersion ||
        obsolete_pcss_keys ||
        migrated_shenlong_keys ||
        obsolete_logging_keys ||
        obsolete_crash_dump_location ||
        obsolete_texture_filtering_location ||
        legacy_in_game_backup ||
        obsolete_visual_damage_keys ||
        obsolete_ped_density_keys ||
        obsolete_nfs2015_keys ||
        obsolete_renderer_keys ||
        obsolete_unverified_keys ||
        obsolete_shadow_settings) {
        RetireLegacyDeveloperControls(config);
    }

#if defined(SPATCH_FINAL_RELEASE)
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
    config.hook_aa_probe = false;
    config.hook_aa_fx_probe = false;
    config.aa_variant_debug_keys = false;
    config.aa_aux_debug_keys = false;
    config.smaa_debug_keys = false;
    config.hook_post_material_submit = false;
    config.hook_character_regression_probe = false;
    config.hook_pass8_probe = false;
    config.aa_variant_mode = 0;
    config.aa_aux_mode = 0;
    config.summary_interval_ms = 0;
    config.max_verbose_events = 0;
    config.max_unique_callbacks = 0;
    // Voice-line diagnostics are developer tuning aids and stay out of final
    // end-user builds entirely.
    config.diag_combat_line_events = false;
    config.diag_npc_voice_line_events = false;
    // Same for the stimulus census: it installs a detour on a per-ped,
    // per-stimulus path and exists only to answer a one-off measurement
    // question. Never active in a shipped build.
    config.diag_stimulus_events = false;
    // The emitter probe detours the engine's own Stimulus::Init entry and
    // exists only to answer a one-off measurement question, so it gets the
    // same treatment as the census above: never active in a shipped build.
    config.stimulus_init_emitter_probe = false;
#endif

    if (config.time_step_smoothing < -1) {
        config.time_step_smoothing = -1;
    } else if (config.time_step_smoothing > 1) {
        config.time_step_smoothing = 1;
    }

    config.controller_left_stick_deadzone =
        input::ClampDeadzonePercent(config.controller_left_stick_deadzone);
    config.controller_right_stick_deadzone =
        input::ClampDeadzonePercent(config.controller_right_stick_deadzone);

    config.gtav_speed_shake_strength_percent = std::clamp(
        config.gtav_speed_shake_strength_percent,
        kGTAVSpeedShakeStrengthMinPercent,
        kGTAVSpeedShakeStrengthMaxPercent);
    config.gtav_speed_shake_start_speed_kmh = std::clamp(
        config.gtav_speed_shake_start_speed_kmh,
        kGTAVSpeedShakeStartSpeedMinKmh,
        kGTAVSpeedShakeStartSpeedMaxKmh);
    config.gtav_speed_shake_full_speed_kmh = std::clamp(
        config.gtav_speed_shake_full_speed_kmh,
        kGTAVSpeedShakeFullSpeedMinKmh,
        kGTAVSpeedShakeFullSpeedMaxKmh);
    config.gta_vi_time_clock_day_minutes = std::clamp(
        config.gta_vi_time_clock_day_minutes,
        kGTAVITimeClockDayMinutesMin,
        kGTAVITimeClockDayMinutesMax);
    config.agx_strength = std::clamp(
        config.agx_strength, kAgxStrengthMinPercent, kAgxStrengthMaxPercent);
    config.agx_exposure = std::clamp(
        config.agx_exposure, kAgxExposureMinPercent, kAgxExposureMaxPercent);
    config.agx_look =
        config.agx_look == agx::kAgxLookNeutral ? agx::kAgxLookNeutral
                                                : agx::kAgxLookDefault;
    config.agx_debug_bloom = std::clamp(
        config.agx_debug_bloom, kAgxDebugBloomMin, kAgxDebugBloomMax);
    if (config.gtav_speed_shake_full_speed_kmh <=
        config.gtav_speed_shake_start_speed_kmh) {
        config.gtav_speed_shake_start_speed_kmh =
            kGTAVSpeedShakeStartSpeedDefaultKmh;
        config.gtav_speed_shake_full_speed_kmh =
            kGTAVSpeedShakeFullSpeedDefaultKmh;
    }

    if (config.min_fog_slicing_interval < 1) {
        config.min_fog_slicing_interval = 1;
    } else if (config.min_fog_slicing_interval > 4) {
        config.min_fog_slicing_interval = 4;
    }

    // The headroom knob is opt-in: 0 keeps the stock 500 MiB reservation.
    // Anything above the maximum is clamped rather than rejected so a
    // mistyped value cannot ask for an absurd reservation.
    if (config.pool_headroom_mib < kPoolHeadroomMinMiB) {
        config.pool_headroom_mib = kPoolHeadroomMinMiB;
    } else if (config.pool_headroom_mib > kPoolHeadroomMaxMiB) {
        config.pool_headroom_mib = kPoolHeadroomMaxMiB;
    }

    if (config.override_low_res_buffer < -1) {
        config.override_low_res_buffer = -1;
    } else if (config.override_low_res_buffer > 1) {
        config.override_low_res_buffer = 1;
    }

    if (config.override_shadow_filter < -1) {
        config.override_shadow_filter = -1;
    } else if (config.override_shadow_filter > 1) {
        config.override_shadow_filter = 1;
    }
    if (config.shadow_resolution < 0) {
        config.shadow_resolution = 0;
    } else if (config.shadow_resolution > 4096) {
        config.shadow_resolution = 4096;
    }

    config.custom_frame_limiter_fps =
        frame_limiter::ClampCustomFrameLimiterFps(
            config.custom_frame_limiter_fps);

    if (config.override_texture_detail_level < -1) {
        config.override_texture_detail_level = -1;
    } else if (config.override_texture_detail_level > 2) {
        config.override_texture_detail_level = 2;
    }

    if (config.override_motion_blur < -1) {
        config.override_motion_blur = -1;
    } else if (config.override_motion_blur > 2) {
        config.override_motion_blur = 2;
    }

    if (config.override_world_density < -1) {
        config.override_world_density = -1;
    } else if (config.override_world_density > 4) {
        config.override_world_density = 4;
    }

    if (config.override_fullscreen < -1) {
        config.override_fullscreen = -1;
    } else if (config.override_fullscreen > 1) {
        config.override_fullscreen = 1;
    }

    if (config.override_rumble_enabled < -1) {
        config.override_rumble_enabled = -1;
    } else if (config.override_rumble_enabled > 1) {
        config.override_rumble_enabled = 1;
    }

    if (config.spherical_reflection_width < kSphericalReflectionWidthAuto) {
        config.spherical_reflection_width = kSphericalReflectionWidthAuto;
    } else if (config.spherical_reflection_width > kSphericalReflectionWidthAuto &&
               config.spherical_reflection_width < kSphericalReflectionWidthMin) {
        config.spherical_reflection_width = kSphericalReflectionWidthMin;
    } else if (config.spherical_reflection_width > kSphericalReflectionWidthMax) {
        config.spherical_reflection_width = kSphericalReflectionWidthMax;
    }

    if (config.smaa_preset < 0) {
        config.smaa_preset = 0;
    } else if (config.smaa_preset > 3) {
        config.smaa_preset = 3;
    }
    // Out-of-range, negative, or malformed values fall back to the documented
    // default (0 = follow the preset threshold) instead of partial parsing.
    if (!(config.smaa_edge_threshold >= 0.0f) || config.smaa_edge_threshold > 0.5f) {
        config.smaa_edge_threshold = 0.0f;
    }

    config.speed_rumble_strength_percent =
        std::clamp(config.speed_rumble_strength_percent, 0, 100);
    if (config.speed_rumble_start_kmh < 1) {
        config.speed_rumble_start_kmh = 1;
    }
    if (config.speed_rumble_full_kmh <= config.speed_rumble_start_kmh) {
        config.speed_rumble_full_kmh = config.speed_rumble_start_kmh + 1;
    }

    if (config.aa_variant_mode < 0) {
        config.aa_variant_mode = 0;
    } else if (config.aa_variant_mode > 2) {
        config.aa_variant_mode = 2;
    }

    if (config.aa_aux_mode < 0) {
        config.aa_aux_mode = 0;
    } else if (config.aa_aux_mode > 2) {
        config.aa_aux_mode = 2;
    }

    if (config.cutscene_fps < kCutsceneFpsAuto) {
        config.cutscene_fps = kCutsceneFpsAuto;
    } else if (config.cutscene_fps > kCutsceneFpsMax) {
        config.cutscene_fps = kCutsceneFpsMax;
    } else if (config.cutscene_fps > kCutsceneFpsAuto &&
               config.cutscene_fps < kCutsceneFpsMin) {
        config.cutscene_fps = kCutsceneFpsMin;
    }

    if (load_report.persistence != ConfigPersistenceStatus::CreateFailed &&
        load_report.persistence != ConfigPersistenceStatus::SourceUnavailable &&
        (config_version < kConfigVersion ||
         obsolete_pcss_keys ||
         migrated_shenlong_keys ||
         obsolete_logging_keys ||
         obsolete_crash_dump_location ||
         obsolete_texture_filtering_location ||
         legacy_in_game_backup ||
         obsolete_visual_damage_keys ||
         obsolete_ped_density_keys ||
         obsolete_nfs2015_keys ||
         obsolete_renderer_keys ||
         obsolete_unverified_keys ||
         obsolete_shadow_settings)) {
        const auto backup_path = ConfigBackupPath(path);
        // Never rewrite a user's config unless the exact file being replaced
        // has first become the durable current backup.
        FileIdentity current_identity{};
        if (!source_identity_valid ||
            !ReadFileIdentity(path, current_identity) ||
            current_identity != source_identity) {
            load_report.persistence = ConfigPersistenceStatus::SourceChanged;
        } else if (!WriteBackupCopy(path, backup_path) ||
                   !FilesHaveEqualBytes(path, backup_path)) {
            load_report.persistence = ConfigPersistenceStatus::BackupFailed;
        } else if (!ReadFileIdentity(path, current_identity) ||
                   current_identity != source_identity) {
            load_report.persistence = ConfigPersistenceStatus::SourceChanged;
        } else if (!RelocateLegacyInGameBackup(path)) {
            load_report.persistence = ConfigPersistenceStatus::BackupFailed;
        } else if (!WriteOrganizedConfig(path, config)) {
            load_report.persistence = ConfigPersistenceStatus::MigrationWriteFailed;
        } else {
            load_report.persistence = ConfigPersistenceStatus::Migrated;
        }
    }

    if (report != nullptr) {
        *report = load_report;
    }
    return config;
}

bool WriteBoolValue(const std::filesystem::path& path, const wchar_t* key, bool value) {
    std::lock_guard<std::mutex> lock(g_config_write_mutex);
    if (key == nullptr || *key == L'\0' ||
        (!PathExistsNoThrow(path) && !WriteOrganizedConfig(path, Config{}))) {
        return false;
    }
    const IniKeyAlias* alias = FindIniKeyAlias(key);
    const wchar_t* section = alias != nullptr ? alias->section : SectionForKey(key);
    const wchar_t* output_key = alias != nullptr ? alias->user_key : key;
    return WritePrivateProfileStringW(section, output_key, value ? L"1" : L"0", path.c_str()) != 0;
}

bool WriteIntValue(const std::filesystem::path& path, const wchar_t* key, int value) {
    std::lock_guard<std::mutex> lock(g_config_write_mutex);
    if (key == nullptr || *key == L'\0' ||
        (!PathExistsNoThrow(path) && !WriteOrganizedConfig(path, Config{}))) {
        return false;
    }
    wchar_t buffer[32]{};
    _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, L"%d", value);
    const IniKeyAlias* alias = FindIniKeyAlias(key);
    const wchar_t* section = alias != nullptr ? alias->section : SectionForKey(key);
    const wchar_t* output_key = alias != nullptr ? alias->user_key : key;
    return WritePrivateProfileStringW(section, output_key, buffer, path.c_str()) != 0;
}

}  // namespace spatch
