#pragma once

#include <filesystem>

namespace spatch {

struct Config {
    // Core
    bool enabled = true;
    // Permits build-independent settings on an unverified executable. Fixed
    // address hooks and executable patches remain disabled in this mode.
    bool allow_unverified_build = false;
    bool write_minidumps = true;
    // File logging is opt-in in user releases. Crash dumps remain independent
    // so a user can keep the low-overhead crash reporter without a live log.
    bool enable_logging = false;

    // Runtime probes and internal dependency hooks.  The cutscene fix turns
    // on its required timing hooks automatically; these switches only add
    // optional telemetry when enabled by an advanced user.
    bool hook_queue_ready = false;
    bool hook_task_dispatch = false;
    bool hook_wait_helper = false;
    bool hook_scaleform_time = false;
    bool hook_scaleform_init = false;
    bool hook_nis_timing = false;
    bool hook_nis_runtime = false;
    bool hook_nis_owner = false;
    bool hook_nis_actor_state = false;
    bool hook_twitch_probe = false;
    bool hook_frameflow = false;
    bool hook_scenery_builders = false;
    bool hook_fog_slicing_guard = true;
    bool hook_aa_probe = false;
    bool hook_aa_fx_probe = false;
    bool aa_variant_debug_keys = false;
    bool aa_aux_debug_keys = false;
    bool hook_smaa_present = true;
    // Derived, not user-facing: the game's own FXAA always yields to a live
    // SMAA replacement (stacking was tried and re-broke the world icons).
    bool smaa_disable_stock_aa = true;
    bool hook_post_material_submit = false;
    bool hook_character_regression_probe = false;
    // Doc-295 combined probe: WndProc message/dedupe counters, the vestigial
    // online-service URL probe wall-time, and the doc-292 activity-timer
    // advance rate. Observe-only telemetry for the legacy researched layout.
    bool hook_pass8_probe = false;
    // Logs the game's own conversation dispatches (push reactions and their
    // speech-guard values) so one short play session shows whether the
    // push-voice-line chain is reachable on a live install.
    bool diag_push_voice_line_events = false;
    // Logs the restored combat voice-line invocations (which family fired
    // and whether the engine accepted it) so a play session shows the
    // trigger coverage on a live install. Suppressed in final end-user
    // builds.
    bool diag_combat_line_events = false;
    // Logs the restored NPC voice-line invocations (the police and crowd
    // encouragement families; which fired and whether the engine accepted
    // it) so a play session shows the trigger coverage on a live install.
    // Suppressed in final end-user builds.
    bool diag_npc_voice_line_events = false;
    // Per-hit samples from the on-foot yaw-approach site (return-RVA site,
    // approach args, occupancy read, OS-level stick sample) so a play
    // session proves whether manual stick turning bypasses the approach
    // before any hold is enabled. Like the vehicle-camera probe records it
    // stays available in final end-user builds when Logging=1, because the
    // on-foot hold verdict can only be captured on the deployed build.
    bool diag_onfoot_yaw_probe_events = false;
    // FOV probe: record which float on the camera component (and on the vehicle
    // subject) moves within the plausible field-of-view range, so the live FOV
    // field can be identified at runtime. Aim from a car while this runs: the FOV
    // value steps.
    bool diag_fov_probe = false;
    // Blip probe: record calls to the script blip API that controls blip-type
    // visibility. Sleeping Dogs' own scripts flip a blip type visible/invisible as
    // the wanted state changes, so the police type identifies itself by observation.
    bool diag_blip_probe = false;
    // Drivetrain probe: record the analog driver output (pedals) and the
    // transmission output (reverse/gear bytes) around the reverse-engage
    // moment, to identify what drives the brake-to-reverse transition before
    // gating it. Drive forward, brake to a stop into reverse, release,
    // re-press, while this is on.
    bool diag_drivetrain_probe = false;
    // Stimulus census: counts which of the engine's 116 declared stimulus
    // types actually reach a ped's StimulusReceiverComponent, so one play
    // session shows whether the shipped weapon-fear reactions (DRAW_WEAPON,
    // GUN_POINTED_AT_YOU, SCARED_PEDESTRIAN) are emitted at all. Observe-only:
    // it changes no game behaviour and writes only to SPatch.log.
    bool diag_stimulus_events = false;
    // Logs the game's per-light deferred submits (light family, packed flag
    // byte, chosen technique family) so one night-drive session shows which
    // light types exist, which already carry the engine's own shadow flag,
    // and what vehicle headlights and sirens look like at the submit layer.
    // Observe-only; writes to SPatch.log.
    bool diag_light_submit_events = false;
    // Shadow-scale consumer census (read by the ShadowScale module from
    // SPatch.ini itself); kept here so the config writer preserves the key.
    bool diag_census_shadow_consumers = false;
    // Captures the StimulusEmitterComponent* that the engine hands to
    // Stimulus::Init and reports its TypeUID. This is the ONLY mechanism that
    // can reach the emitter: it registers with the central StimulusManager
    // update list, never with a ped, so a SimObject component census cannot
    // find it. Observe-only and legacy_researched only -- the RVA is not
    // mapped for the later Steam layout.
    bool stimulus_init_emitter_probe = false;
    // Voice and reaction ids passed to the engine's speak-by-name entry.
    //
    // Which values are correct was NOT settled by static reasoning: the diagnostic
    // log's field labels were swapped between builds, so the same {0,5} pair appeared
    // as "reaction=0 voice=5" in one build and "voice=0 reaction=5" in another.
    // Defaults come from kPushVoiceIdDefault / kPushReactionIdDefault (5 and 5), which
    // match what the engine's own PUSHED dispatches carry. Both are overridable so a
    // single live run can determine the right pair empirically.
    unsigned int push_voice_line_voice_id = 0;
    unsigned int push_voice_line_reaction_id = 5;
    // Sweep a list of candidate voice ids at runtime (one per bump) so a single
    // session identifies which value the engine accepts, instead of us inferring
    // it from logs whose field labels were swapped between builds.
    bool push_voice_line_voice_id_sweep = false;

    // Stable fixes / restorations
    bool fix_cutscene_zero_dt = true;
    // Rebuilding the NIS timeline smooths 30 Hz cutscene animation at high
    // frame rates, but it re-bases the timeline the director samples away from
    // the engine's own clock. Object tracks then sample ahead of the bones they
    // are attached to, which deforms props (chains, cloth, clothing) while a
    // cutscene plays. Off by default; the frame-flow repair already smooths the
    // camera and does not displace the timeline.
    bool fix_cutscene_scene_time_step = false;
    // Per-repair overrides for the two halves of FixCutsceneFPS. They are
    // independent repairs with different failure modes, and one of them can
    // deform props at high frame rates, so each needs its own off switch.
    // -1 follows FixCutsceneFPS, 0 disables that repair, 1 forces it on.
    int override_cutscene_zero_dt = -1;
    int override_cutscene_scene_time = -1;
    bool fix_nis_actor_restore_duplicates = true;
    bool smaa_enable = true;
    bool smaa_debug_keys = false;
    bool restore_original_fog = true;
    bool restore_original_eye_reflections = true;
    bool restore_character_wetness = true;
    bool restore_character_sweat = true;
    // Disable the engine's real-time depth-of-field pass (the distance
    // "far blur"). The stage queues its DepthOfField_PrePass / CalcNearCoC /
    // DepthOfField_FarBlur draws through one dispatcher; when this option is
    // on the dispatcher returns before queuing anything, so no DOF blur is
    // rendered anywhere. Off by default (stock look).
    bool disable_dof = false;
    // Cutscene DOF control. The same dispatcher serves NIS (non-interactive
    // scene) calls - its mode argument selects the DEPTHOFFIELDNIS shader -
    // so cutscene DOF can be kept or killed independently of the gameplay
    // pass. Off by default (stock look).
    bool disable_cutscene_dof = false;
    // Wei's "moving!" lines when pushing through pedestrians still exist in
    // the DE's audio data and speech code but the game never triggers them.
    // The fix waits for the game's own push reactions and then plays Wei's
    // line through the game's speech chain. Off by default until confirmed
    // on a live install.
    bool restore_push_voice_lines = true;
    // Keep the restored push lines quiet while a mission is running (the
    // game's UI mode global reads 2 = mission-scoped gameplay). Off by
    // default.
    bool restore_push_voice_lines_free_roam_only = true;
    // Wei's shipped-but-unwired combat and social voice lines (pre-fight,
    // combat taunts, low-damage apologies outside combat) exist in the DE's
    // audio banks but the game never triggers them. The fix fires them through
    // the same proven speech path as the push voice line. Off by default
    // until confirmed on a live install.
    bool restore_combat_lines = true;
    // Disable the restored combat voice lines while a mission is running
    // (the game's UI mode global reads 2 = mission-scoped gameplay; 0 = the
    // free-roam state this restore feature was tuned for).
    bool restore_combat_lines_free_roam_only = true;
    // Speak the combat_pre_posture line in the STRUCK character's own voice
    // instead of always in Wei's. That family ships for 66 voices (400 takes),
    // not just Wei, and the by-name speech entry composes the name from the
    // target component's own voice tag - so this simply hands it the victim's
    // component. Requires restore_combat_lines; without it there is no line to
    // redirect. Off by default: it changes who is heard, and it is a wider
    // behavior change than the Wei-only restore.
    bool restore_combat_lines_all_voices = false;
    // Wei.swear_cops is a full 12-take swearing family in the DE's audio
    // banks that gameplay never plays. When enabled, a hard vehicle impact
    // while Wei is driving (detected as a large one-frame forward-speed drop
    // in the chase camera update) fires one swear line through the same
    // proven speech path as RestoreCombatLines. This is a NEW behavior, not
    // a restoration - the game never intended crash barks. Off by default
    // until confirmed on a live install.
    bool driving_swear_lines = true;
    // Suppress the driving swear lines while a mission is running (the game's
    // UI mode global reads 2 = mission-scoped gameplay). Off by default.
    bool driving_swear_lines_free_roam_only = true;
    // Police-specific Wei.swear_cops takes on a verified rising cop-heat
    // transition while driving. Separate from generic impact barks.
    // Public key: CopPursuitLines.
    bool cop_pursuit_swear_lines = true;
    // Suppress the cop-pursuit lines while a mission is running (UI mode == 2).
    // Off by default.
    bool cop_pursuit_lines_free_roam_only = true;
    // Combined switch: suppress BOTH the driving swear lines and the
    // cop-pursuit lines while a mission is running (UI mode == 2). OR-ed with
    // the two per-feature keys above. Off by default.
    bool swear_lines_free_roam_only = true;
    // Neutralise the engine's slow-motion for two specific features. Both
    // work by overriding the time-dilation request at the engine's apply
    // point (RVA 0x301340). The observed request factors are 0.5 for the
    // drive-by aim ramp and 0.3 for the stunt hit (live capture 2026-09-15).
    bool disable_driveby_slow_mo = false;
    bool disable_stunt_slow_mo = false;
    // Suppress the stunt swoosh. Exactly ONE event id is dropped, and only
    // while the engine's applied time scale is below 1.0, i.e. only inside a
    // slow-motion window: 0xE488B9D7, posted through 0x00144230 on the first
    // frame of every stunt. The operator confirmed it as the swoosh on
    // 2026-09-28 - it was the first id of BOTH stunt windows in that session's
    // capture, and the earlier raw-probe capture put it 1 ms before the scale
    // dropped to 0.3. It stays unnamed: the name-to-id resolver is a table
    // lookup rather than a hash, and this id does not route through it.
    // Everything else passes through untouched, including the ~780 Hz
    // engine/tyre parameters, so the stunt keeps its engine and tyre audio.
    // The pass-by whoosh pair - 0x46A60BB3 (Play_vehicle_whooshes) and
    // 0xBDE53B23 (whoosh_rel_speed) - is deliberately NOT suppressed: play
    // sessions showed it firing during ordinary driving, 35 to 59 s before any
    // stunt window opened, so muting it was never what this option asked for.
    bool disable_stunt_sound_fx = false;
    // Suppress the stunt screen desaturation. v2: the effect is requested by
    // the gameplay scripts through the full-screen-filter API - the
    // `enable_screen_filter` / `enable_fullscreen_filter` Squish handlers that
    // funnel into the engine applier at RVA 0x005D830 (legacy layout). The
    // hook sees every enabling request with its filter id and drops only ids
    // pinned in kStuntScreenFilterBlockedIds. The dead camera-blend detour this
    // replaces was removed per the desaturation-driver artifact's own decision
    // rule (see artifacts/reverse-engineering/
    // 2026-09-28-stunt-screen-fx-desaturation-driver.md).
    //
    // STATE, and the user must be told this plainly: the block list is
    // currently EMPTY (`std::array<unsigned int, 0>`), so with this option ON
    // the hook installs and logs every enabling request but suppresses
    // NOTHING. The filter id is a NAME HASH, not an index: the Squish handlers
    // `enable_screen_filter` / `enable_fullscreen_filter` resolve the name with
    // FUN_140180880 - CRC, poly 0x04C11DB7 reflected, no final XOR - before
    // funnelling into the applier. That is a DIFFERENT resolver from the sound
    // system's FNV-1 at 0xA41FD0; do not conflate them. An earlier note here
    // claimed a table lookup on the strength of five hash families scoring
    // 0/5, which was an artifact of testing the wrong algorithm entirely. The
    // CRC reading is from the 2026-09-30 decompile and is NOT yet confirmed
    // against a runtime log line, so it must not be used to pre-pin an id.
    // Until one live stunt pins it via `screen_filter_probe id=...`, this
    // option is a discovery pass, not a working suppressor. Do not document it
    // to the user as working.
    bool disable_stunt_screen_fx = false;
    // The DE ships police voice lines (cops shouting when shoved or struck)
    // and crowd encouragement shouts (spoken by the NPC being hit) in its
    // audio banks but never triggers them. Each fix fires its family through
    // the same proven speech path as RestorePushVoiceLines, on the speaking
    // NPC's own voice component; the engine filters voices that do not own
    // the family. Off by default until confirmed on a live install.
    bool restore_police_voice_lines = true;
    bool restore_encourage_voice_lines = true;
    // Flattened GTA VI-style day-night clock. When enabled, one full
    // in-game day passes in exactly gta_vi_time_clock_day_minutes real
    // minutes and the stock 1.33x faster night progression is cancelled.
    // On by default on mapped layouts; safe-compat mode fail-closes it.
    bool gta_vi_time_clock = true;
    // Target length of one full in-game day in real minutes. Clamped at
    // load time; the shipped default is the GTA VI design point.
    int gta_vi_time_clock_day_minutes = 144;
    bool fix_pedestrian_density_at_high_fps = true;
    // (REMOVED) GTA IV Popcycle-style pedestrian density (GTAIVPedDensityCurve
    // and its strength/min/max keys): removed outright. The curve never ran
    // against live crowds and its write rode the pedestrian-spawn seam; old
    // INIs carrying the keys are rewritten once via HasRetiredPedDensitySettings.
    bool fix_high_fps_average_windows = true;
    bool improve_spherical_reflections = true;
    bool remove_hidden_120_fps_cap = true;
    bool fix_first_run_resolution = true;
    bool fix_scaleform_qpc_clock = true;
    bool fix_file_timestamp_open_mode = true;
    bool fix_audio_file_open = true;
    bool fix_large_file_sizes = true;
    bool fix_vram_pool_lock = true;
    bool fix_vram_capacity_reporting = true;
    bool fix_resource_loading = true;
    bool fix_contact_list_overflow = true;
    bool fix_corrupt_save_handling = true;
    bool fix_thread_creation_failure = true;
    // The async file-load poller spins with Sleep(0) while a read is in
    // flight (burns a core); the engine's other Sleep sites all yield with
    // Sleep(1). Internal churn fix, RE sweep THREAD-01.
    bool fix_qfile_ready_sleep_yield = true;
    // The per-frame combat list removal pass quicksorts the whole array to
    // take one max-key entry; the selection reproduces the removal walk's
    // call sequence without the sort. Internal churn fix, RE sweep SIM-01.
    bool fix_combat_list_selection = true;
    // The Eidos online-service domain used by the boot connectivity probe
    // has been shut down; the stock blocking WinINet request can only fail
    // after a network timeout (measured 1387 ms per boot, doc 296). The fix
    // returns the stock "service unreachable" result without wininet.
    bool fix_online_probe_stall = true;
    // The character/camera activity accumulator at +0x1E5C refreshes with a
    // fixed 0.1 per call (event-counted; advance 0.1 even at dt=0, docs
    // 292/296/297). The fix scales the pulse by the 60 Hz reference so
    // transition pacing matches the design point at any call rate.
    bool fix_frame_counted_activity_timer = true;
    bool warn_low_res_buffer = true;

    // Input
    // The game already registers Windows Raw Input for the mouse. This forces
    // its hidden raw-input option instead of installing a second input stack.
    bool force_raw_mouse_input = true;
    // FollowCamera otherwise drains accumulated mouse delta over time before
    // applying yaw and pitch. Controller look is not temporally smoothed.
    bool disable_camera_smoothing = true;
    // Independent opt-in vehicle-camera behavior. Unsupported executable
    // profiles must force both off until their native mappings are verified.
    bool gta_iv_car_camera = false;
    bool gta_iv_bike_camera = false;
    // Suppresses the dedicated camera behavior that engages while the police
    // pursue the player. The base selector picks the Flee parameters block
    // ahead of Drive; with this on, the Drive block is used instead, so the
    // camera keeps its normal road behavior during a chase.
    bool disable_pursuit_camera = false;
    // Fixes the chase camera pulling inside the car body when the player aims
    // a gun during a police pursuit. The pursuit branch's aim parameters hold
    // the camera ~1.8 m from the car (inside the body, measured from a live
    // capture); the normal driving branch's aim parameters hold ~3.2 m
    // (outside). With this on, the camera is pushed out to that known-good
    // distance while aiming during a pursuit, and the game's own collision
    // still runs on the result. Cars only.
    bool fix_chase_aim_camera = true;
    // Hide the police (cop) icons on the radar/minimap. The game applies a blip
    // symbol per entity and the police ones are four known symbols
    // (cop_symbol, cop_foot_symbol, cop_foot_patrol_symbol,
    // cop_vehicle_patrol_symbol); with this on those applications are skipped so
    // only the cop icons disappear.
    bool hide_cop_radar_icons = false;
    // Racing-game reverse: while braking to a stop from forward motion, the
    // transmission must not engage reverse; the brake has to be released and
    // pressed again. Implemented as a per-vehicle latch in the driver stage
    // plus a reverse-output veto in the transmission shift stage.
    bool modern_brake_reverse = false;
    // Keep the road wheels at their current angle when the player exits a
    // vehicle instead of letting the game straighten them. Implemented as a
    // hold of the last driven steering angle in the steering callback, player
    // vehicle only, frozen only while the car is parked.
    bool no_wheel_turn_back = false;
    // (REMOVED) Need for Speed (2015) style vehicle chase camera and its 14
    // tuning keys: removed outright. Old INIs carrying the keys are rewritten
    // once via HasRetiredNfs2015Settings.
    bool disable_controller_camera_autocenter = false;
    // EXPERIMENTAL on-foot hold behind the same angular-approach site.
    // Latest-Steam and safe compatibility mode force this off like the
    // vehicle sibling above. Revert condition: stick yaw freeze.
    bool disable_onfoot_camera_autocenter = false;
    // Stock GTA V high-speed vehicle shake. This remains independent of the
    // two GTA IV-like camera controls above.
    bool gtav_speed_shake = false;
    // End-user tuning for the stock GTA V profile. Speeds use km/h in the
    // INI; the runtime converts them at the camera-policy boundary.
    int gtav_speed_shake_strength_percent = 180;
    int gtav_speed_shake_start_speed_kmh = 70;
    int gtav_speed_shake_full_speed_kmh = 150;
    // Controller rumble that rises with vehicle speed. Off by default.
    bool speed_rumble = true;
    int speed_rumble_start_kmh = 70;
    int speed_rumble_full_kmh = 150;
    int speed_rumble_strength_percent = 1;
    // -1 preserves the stock radial filter. Zero exposes the direct XInput or
    // DirectInput stick values; positive values are radial percentages.
    int controller_left_stick_deadzone = -1;
    int controller_right_stick_deadzone = -1;

    // Numeric tuning
    // The stock setter uses interval * 8 as an unsigned divisor; only 1-4
    // produce a valid, non-stalled slicing cadence.
    int min_fog_slicing_interval = 1;
    // EXPERIMENTAL. Megabytes of memory the engine's Main Pool reserves up
    // front. The stock build reserves 500 MiB in one VirtualAlloc; when a
    // request does not fit, the allocator reserves a fresh region sized to
    // that request. When the pool can serve no request and cannot grow the
    // game reports "out of memory" and traps. Raising the initial
    // reservation gives the pool more contiguous room before it has to
    // grow. 0 keeps the stock 500 MiB reservation. This changes how much
    // memory the game commits at startup and is not validated on a live
    // install; it is a diagnostic knob, not a fix.
    int pool_headroom_mib = 0;
    // -1 keeps the game value, 0 disables the stock adaptive smoother, and 1
    // enables it. The game always uses a maximum of eight samples; this is not
    // an arbitrary frame-count control.
    int time_step_smoothing = 0;
    int override_low_res_buffer = 0;
    int override_shadow_filter = -1;
    // Gives car headlight beams shadows through the engine's own per-light
    // spotlight shadow path (DRSPOTLIGHTSHADOWS). The original game never
    // applied that path to vehicle lights, so this is an enhancement, not a
    // restoration of shipped behavior. Off by default until a live session
    // confirms the visual.
    bool headlight_shadows = false;
    // Same mechanism for the police lightbar / siren lights. Independent of
    // HeadlightShadows.
    bool siren_shadows = false;
    // Shadow-map resolution upscale driven by the ShadowScale module (native
    // D3D11 detours). 0 keeps the
    // game's native shadow-map sizes; 2048 floors the small classes to 2048;
    // 4096 additionally doubles the native 2048 cascades to 4096. Parsed for
    // the ShadowScale module (which reads SPatch.ini itself); kept here so
    // the config writer preserves the key across rewrites.
    int shadow_resolution = 0;
    // SPatch-authored headlight beam: feeds one real spot light into the
    // engine's deferred light gather while you drive, so the beam renders
    // with the same light-card and shadow path world lamps use. This is the
    // working successor to the flag-based keys above - vehicle lights are
    // not deferred lights (round75/76). Off by default.
    bool headlight_beam_light = false;
    // Beam light radius in metres (4..40).
    float headlight_beam_radius = 14.0f;
    // Beam color, linear 0..1.
    float headlight_beam_color_r = 0.95f;
    float headlight_beam_color_g = 0.97f;
    float headlight_beam_color_b = 1.0f;
    // Light-record type byte (light + 0x68) each shadow force matches. The
    // default 5 is the dedicated vehicle-effect submit family observed
    // statically (round75); change only with LightSubmitEvents evidence.
    int headlight_shadow_light_type = 5;
    int siren_shadow_light_type = 5;
    // R6: RTSS-style custom frame limiter. fps <= 0 (after clamping) means
    // off; the INI key order is FPS value first, enable switch second.
    bool custom_frame_limiter = false;
    float custom_frame_limiter_fps = 0.0f;
    int override_texture_detail_level = -1;
    // -1 preserves the game setting. Supported forced values are the native
    // sampler-writer exponents represented as 4x, 8x, and 16x.
    int anisotropic_filtering = 16;
    // Promote the exact stock trilinear filter-selector branch.
    bool force_anisotropic_filtering = true;
    // -1 keeps the game setting, 0 disables motion blur, 1 is the stock
    // normal tier, and 2 is the stock high-quality tier.
    int override_motion_blur = -1;
    int override_world_density = -1;
    int override_rumble_enabled = -1;
    // 0 follows the active display width. Positive values are clamped to the
    // safe 1280..4096 range; the spherical target is always kept at 2:1.
    int spherical_reflection_width = 0;
    bool prefer_max_refresh_rate = true;
    int override_fullscreen = -1;
    int aa_variant_mode = 0;
    int aa_aux_mode = 0;
    int smaa_preset = 3;
    // Edge-detection sensitivity override for the SMAA replacement. 0 follows
    // the canonical threshold of the selected preset. Raises the minimum
    // luma delta that counts as an edge; translucent pulsing world icons and
    // mission markers flicker when their soft edges straddle the preset
    // threshold.
    float smaa_edge_threshold = 0.0f;
    // Non-FXAA fix for the world-icon flicker: after the SMAA neighborhood
    // blend, a contrast-gated tent pass smooths only the high-frequency band
    // (small pulsing world icons and mission markers); flat regions and
    // SMAA's own blends pass through untouched.
    bool icon_stabilizer = true;
    // Dynamic radar: hide the minimap while on foot (unless a mission is
    // running), show it in vehicles. Driven through the game's own HUD
    // visibility path.
    bool dynamic_radar = false;
    // World-marker probe: installs call-counting detours on the candidate
    // world-icon renderer functions (they call the original; only logging).
    // Evidence-gathering for the future HideWorldMarkers option.
    bool hide_world_markers_probe = false;
    // Crouch-restoration probe: one-shot dump of the runtime eActionRequest
    // enum table (name arrays + ids) ~15s after launch, plus a log of the
    // first _submit_action_request calls the game itself makes (cops crouch
    // while defending). Evidence for wiring the player crouch toggle.
    bool probe_action_requests = false;
    // Diagnostic: log every change of the engine's applied simulation time
    // scale (RVA 0x203C958) together with the requesting component, so the
    // slow-motion feature that asked for it can be identified from a live
    // session. Off by default; writes to SPatch.log only.
    bool probe_slow_mo = false;
    // Diagnostic: log every script full-screen colour-wash request (filter id,
    // direction, priority) through the engine's single filter funnel, so the
    // filter id behind a wash the player wants gone can be identified from a
    // live session. Off by default; writes to SPatch.log only.
    bool probe_screen_filter = false;
    // Native AgX tonemapping (v58): replaces only the exact known final
    // pre-HUD pixel shader. Look: 0 = Neutral, 1 = MediumHigh (the
    // stock-matched default). Strength and exposure are percentages.
    bool agx_enabled = true;
    int agx_strength = 100;
    int agx_exposure = 100;
    int agx_look = 1;
    // AgXDebugBloom is a DIAGNOSTIC, not a fix. It selects the AgX replacement
    // shader's compile-time visualisation of the engine's texHDRBloom:
    // 0 (default) is the shipped image, 1 shows the raw bloom colour, 2 shows
    // the bloom's chroma spread as one greyscale scalar (run this one first),
    // and 3 shows the bloom's .a HDR term alone. 1-3 REPLACE the whole frame
    // with that view, so the game is not on screen while they are set: a
    // debug run and an ordinary run are two separate launches, and nothing
    // observed in a debug run says anything about how the stunt screen looks.
    // Supported range 0-3; out-of-range values are clamped at load time. It
    // suppresses nothing and fixes nothing. See
    // artifacts/reverse-engineering/2026-09-28-stunt-screen-fx-render-path-agx.md
    // section 9.1.
    int agx_debug_bloom = 0;
    // Time in seconds spent fully wet after water contact, followed by the
    // duration of the linear dry-down. Both are end-user configurable.
    int wetness_full_time_seconds = 30;
    int wetness_fade_time_seconds = 270;
    // Sweat is accumulated only while a character is running or in combat.
    // The values are deliberately independent of render cadence and apply to
    // Wei and NPCs alike.
    int sweat_build_time_seconds = 150;
    int sweat_fade_time_seconds = 120;
    int sweat_onset_time_seconds = 30;
    float sweat_run_speed = 2.5f;
    int sweat_combat_time_seconds = 15;
    unsigned long summary_interval_ms = 0;
    unsigned long max_verbose_events = 0;
    unsigned long max_unique_callbacks = 0;

    // Cutscene timing
    // 0 follows the live game cadence (recommended). A positive value tells
    // the correction logic which externally selected game cadence to expect.
    // This is deliberately not a renderer/physics limiter.
    int cutscene_fps = 0;
};

inline constexpr int kCutsceneFpsAuto = 0;
inline constexpr int kCutsceneFpsMin = 15;
inline constexpr int kCutsceneFpsMax = 1000;

inline constexpr int kSphericalReflectionWidthAuto = 0;
inline constexpr int kSphericalReflectionWidthMin = 1280;
inline constexpr int kSphericalReflectionWidthMax = 4096;
inline constexpr int kWetnessTimeMinSeconds = 0;
inline constexpr int kWetnessTimeMaxSeconds = 3600;
inline constexpr int kSweatTimeMinSeconds = 0;
inline constexpr int kSweatTimeMaxSeconds = 3600;
inline constexpr float kSweatRunSpeedMin = 0.0f;
inline constexpr float kSweatRunSpeedMax = 100.0f;
inline constexpr int kGTAVSpeedShakeStrengthDefaultPercent = 180;
inline constexpr int kGTAVSpeedShakeStrengthMinPercent = 0;
inline constexpr int kGTAVSpeedShakeStrengthMaxPercent = 300;
inline constexpr int kGTAVSpeedShakeStartSpeedDefaultKmh = 70;
inline constexpr int kGTAVSpeedShakeStartSpeedMinKmh = 0;
inline constexpr int kGTAVSpeedShakeStartSpeedMaxKmh = 400;
inline constexpr int kGTAVSpeedShakeFullSpeedDefaultKmh = 150;
inline constexpr int kGTAVSpeedShakeFullSpeedMinKmh = 1;
inline constexpr int kGTAVSpeedShakeFullSpeedMaxKmh = 400;
inline constexpr int kGTAVITimeClockDayMinutesDefault = 144;
inline constexpr int kPoolHeadroomDefaultMiB = 0;
inline constexpr int kPoolHeadroomMinMiB = 0;
// The reservation size is patched as a 32-bit immediate, so 4096 MiB would
// overflow to zero and ask the game for an empty arena. 2048 MiB is the
// practical ceiling: it stays well inside the immediate's range and keeps
// the committed region to a size a 64-bit game can still reserve and commit
// on a normal machine.
inline constexpr int kPoolHeadroomMaxMiB = 2048;
inline constexpr int kGTAVITimeClockDayMinutesMin = 1;
inline constexpr int kGTAVITimeClockDayMinutesMax = 1440;
inline constexpr int kAgxStrengthDefaultPercent = 100;
inline constexpr int kAgxStrengthMinPercent = 0;
inline constexpr int kAgxStrengthMaxPercent = 100;
inline constexpr int kAgxExposureDefaultPercent = 100;
inline constexpr int kAgxExposureMinPercent = 25;
inline constexpr int kAgxExposureMaxPercent = 400;
inline constexpr int kAgxLookDefault = 1;
inline constexpr int kAgxDebugBloomMin = 0;
inline constexpr int kAgxDebugBloomMax = 3;
inline constexpr int kVehicleShadowLightTypeDefault = 5;
inline constexpr int kVehicleShadowLightTypeMin = 0;
inline constexpr int kVehicleShadowLightTypeMax = 255;
inline constexpr int kConfigVersion = 81;
inline constexpr float kHeadlightBeamRadiusDefault = 14.0f;
inline constexpr float kHeadlightBeamRadiusMin = 4.0f;
inline constexpr float kHeadlightBeamRadiusMax = 40.0f;
inline constexpr float kHeadlightBeamColorMax = 1.0f;

enum class ConfigPersistenceStatus {
    Unchanged,
    Created,
    Migrated,
    CreateFailed,
    BackupFailed,
    MigrationWriteFailed,
    SourceChanged,
    SourceUnavailable,
};

struct ConfigLoadReport {
    int source_version = 0;
    bool source_file_existed = false;
    ConfigPersistenceStatus persistence = ConfigPersistenceStatus::Unchanged;

    [[nodiscard]] bool persistence_succeeded() const noexcept {
        return persistence == ConfigPersistenceStatus::Unchanged ||
               persistence == ConfigPersistenceStatus::Created ||
               persistence == ConfigPersistenceStatus::Migrated;
    }
};

const char* ConfigPersistenceStatusName(ConfigPersistenceStatus status) noexcept;
// Migration backups live outside the game directory. The path is stable for
// one config path and target schema version so installers and tests can find
// the exact pre-migration bytes without leaving .bak files beside the game.
std::filesystem::path ConfigBackupPath(const std::filesystem::path& config_path);
std::filesystem::path LegacyConfigBackupPath(
    const std::filesystem::path& config_path);
Config LoadConfig(const std::filesystem::path& path, ConfigLoadReport* report = nullptr);
bool WriteBoolValue(const std::filesystem::path& path, const wchar_t* key, bool value);

// The configuration values the loader phase needs.
//
// The loader phase runs before the full configuration load, because the
// game has to be patched before it starts running. It reads only the
// values that decide whether to patch, and it writes nothing: the loader
// lock is held at that point, and a configuration rewrite under it would
// hold the whole process up for as long as the write takes.
struct EarlyConfig {
    bool enabled = true;
    int pool_headroom_mib = kPoolHeadroomDefaultMiB;
};
EarlyConfig ReadEarlyConfig(const std::filesystem::path& path);
bool WriteIntValue(const std::filesystem::path& path, const wchar_t* key, int value);

}  // namespace spatch
