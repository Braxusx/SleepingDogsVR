// SDVR <-> SPatch bridge.
// SPatch (c) xowny, GPLv3 - https://www.nexusmods.com/sleepingdogsdefinitiveedition/mods/148 - is built here as its own
// module, sdvr_spatch.dll (GPLv3, used with the author's permission), which the VR mod's dinput8.dll loads at start when the
// file is present. Its sources in this folder are unchanged apart from the hook into this file (config adjustment in
// Main.cpp) and the MinHook shim. This file decides which SPatch feature groups run in VR, from sdvr.ini [SPatch]; inside
// an enabled group SPatch.ini keeps deciding each feature.
#include <windows.h>
#include <string.h>
#include <stdint.h>
#include "Config.h"
// one line into the VR mod's log (sdvr.log) through the host's exported SdvrLog, when the host is there
static void SdvrSpatchLog(const char* text) {
    typedef void (*Log_t)(const char*);
    HMODULE h = GetModuleHandleW(L"dinput8.dll");
    Log_t f = h ? (Log_t)GetProcAddress(h, "SdvrLog") : nullptr;
    if (f) f(text); else OutputDebugStringA(text);
}
namespace {
int Key(const wchar_t* ini, const wchar_t* key, int def) { return static_cast<int>(GetPrivateProfileIntW(L"SPatch", key, def, ini)); }
}
// ---------------------------------------------------------------------------------------------------------------
// Guarded reads (build 098). SPatch protects its probing reads with __try / __except around a plain memory access
// (SafeRead and friends). MSVC honours that; clang (our toolchain) only keeps such a guard around CALLS and silently drops
// it around a plain access, and its option for the MSVC behaviour (-fasync-exceptions) crashes the compiler on files that
// also use C++ try. Result in builds 094-097: the first guarded read of a bad address (for example the speech manager
// constant module+0x441BE30) was an unhandled fault and the game shut itself down.
// This handler restores the intent: an access violation raised by an instruction INSIDE sdvr_spatch.dll is treated as a
// failed read - the instruction is skipped and what it would have loaded reads as zero. Faults anywhere else are not touched.
extern "C" {
#include "../../third_party/minhook/src/hde/hde64.h"
}
static uintptr_t g_selfLo, g_selfHi; static volatile LONG g_guardHits;
static LONG CALLBACK GuardVeh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord; uintptr_t ip = (uintptr_t)c->Rip;
    if (ip < g_selfLo || ip >= g_selfHi) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->NumberParameters >= 1 && ep->ExceptionRecord->ExceptionInformation[0] == 8) return EXCEPTION_CONTINUE_SEARCH;   // execute fault
    hde64s h; unsigned len = hde64_disasm((const void*)ip, &h);
    if ((h.flags & F_ERROR) || len == 0 || len > 15) return EXCEPTION_CONTINUE_SEARCH;
    bool mem = (h.flags & F_MODRM) && h.modrm_mod != 3;
    bool str = !h.opcode2 && h.opcode != 0x0F && (h.opcode == 0xA4 || h.opcode == 0xA5 || h.opcode == 0xAA || h.opcode == 0xAB || h.opcode == 0xAC || h.opcode == 0xAD || h.opcode == 0xA6 || h.opcode == 0xA7 || h.opcode == 0xAE || h.opcode == 0xAF);
    if (!mem && !str) return EXCEPTION_CONTINUE_SEARCH;
    if (mem) {
        unsigned reg = h.modrm_reg | (h.rex_r ? 8u : 0u);
        DWORD64* gpr = &c->Rax;   // Rax Rcx Rdx Rbx Rsp Rbp Rsi Rdi R8..R15 are contiguous in CONTEXT
        if (h.opcode == 0x8B || h.opcode == 0x63) { if (h.p_66) gpr[reg] &= ~0xFFFFull; else gpr[reg] = 0; }
        else if (h.opcode == 0x8A) gpr[reg] &= ~0xFFull;
        else if (h.opcode == 0x0F && (h.opcode2 == 0xB6 || h.opcode2 == 0xB7 || h.opcode2 == 0xBE || h.opcode2 == 0xBF)) gpr[reg] = 0;
        else if (h.opcode == 0x0F && (h.opcode2 == 0x10 || h.opcode2 == 0x28 || h.opcode2 == 0x6F || h.opcode2 == 0x12 || h.opcode2 == 0x16 || h.opcode2 == 0x6E || (h.opcode2 == 0x7E && h.p_rep == 0xF3)))
            memset(&(&c->Xmm0)[reg], 0, sizeof(M128A));
        // anything else (stores, compares, arithmetic on memory) is simply skipped
    }
    c->Rip += len;
    LONG n = InterlockedIncrement(&g_guardHits);
    if (n <= 12) {
        char line[200];
        wsprintfA(line, "spatch: guarded access failed at sdvr_spatch.dll+0x%x (address %p) - treated as a failed read (%ld)", (unsigned)(ip - g_selfLo),
                  ep->ExceptionRecord->NumberParameters >= 2 ? (void*)ep->ExceptionRecord->ExceptionInformation[1] : nullptr, n);
        SdvrSpatchLog(line);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void GuardInstall() {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&GuardInstall, &m) || !m) return;
    const IMAGE_DOS_HEADER* d = (const IMAGE_DOS_HEADER*)m; const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const BYTE*)m + d->e_lfanew);
    g_selfLo = (uintptr_t)m; g_selfHi = g_selfLo + nt->OptionalHeader.SizeOfImage;
    AddVectoredExceptionHandler(1, GuardVeh);
    SdvrSpatchLog("spatch: guarded-read handler installed");
}

namespace spatch {
void SdvrAdjustConfig(Config& c) {
    GuardInstall();
    wchar_t ini[MAX_PATH]; GetModuleFileNameW(nullptr, ini, MAX_PATH);
    wchar_t* s = wcsrchr(ini, L'\\'); if (s) wcscpy(s + 1, L"sdvr.ini");
    const int stability = Key(ini, L"Stability", 1), timing = Key(ini, L"Timing", 0), content = Key(ini, L"Content", 0),
              features = Key(ini, L"Features", 0), graphics = Key(ini, L"Graphics", 0);
    c.enabled = c.enabled && Key(ini, L"Enabled", 1) != 0;
    if (Key(ini, L"Log", 1)) c.enable_logging = true;   // SPatch.log next to the game exe
    // Stability: file / archive / resource / save / thread guards, VRAM fixes, pool headroom
    if (!stability) {
        c.hook_fog_slicing_guard = false; c.fix_scaleform_qpc_clock = false; c.fix_file_timestamp_open_mode = false;
        c.fix_audio_file_open = false; c.fix_large_file_sizes = false; c.fix_vram_pool_lock = false;
        c.fix_vram_capacity_reporting = false; c.fix_resource_loading = false; c.fix_contact_list_overflow = false;
        c.fix_corrupt_save_handling = false; c.fix_thread_creation_failure = false; c.write_minidumps = false;
        c.fix_qfile_ready_sleep_yield = false; c.fix_combat_list_selection = false; c.fix_online_probe_stall = false;
        c.fix_frame_counted_activity_timer = false; c.pool_headroom_mib = 0;
    }
    // Cutscene timing: the VR mod has its own smoothing on the same game function (NisSetPlayTime); SPatch's stays off
    // until the two are reconciled.
    c.fix_cutscene_zero_dt = false; c.fix_cutscene_scene_time_step = false; c.cutscene_fps = 0;
    c.override_cutscene_zero_dt = -1; c.override_cutscene_scene_time = -1;
    // Timing: high frame rate fixes
    if (!timing) {
        c.fix_pedestrian_density_at_high_fps = false; c.fix_high_fps_average_windows = false;
        c.remove_hidden_120_fps_cap = false;
    }
    c.time_step_smoothing = -1;   // the game's own frame-time smoothing setting is left as it is in VR
    // Content: restored character materials and voice lines
    if (!content) {
        c.restore_original_eye_reflections = false; c.restore_character_wetness = false; c.restore_character_sweat = false;
        c.fix_nis_actor_restore_duplicates = false;
        c.restore_push_voice_lines = false; c.restore_combat_lines = false; c.restore_combat_lines_all_voices = false;
        c.driving_swear_lines = false; c.cop_pursuit_swear_lines = false; c.restore_police_voice_lines = false;
        c.restore_encourage_voice_lines = false;
    }
    // Features: input, cameras, gameplay options
    if (!features) {
        c.force_raw_mouse_input = false; c.disable_camera_smoothing = false; c.gta_iv_car_camera = false;
        c.gta_iv_bike_camera = false; c.controller_left_stick_deadzone = -1; c.controller_right_stick_deadzone = -1;
        c.override_world_density = -1; c.override_rumble_enabled = -1;
        c.disable_driveby_slow_mo = false; c.disable_stunt_slow_mo = false; c.disable_stunt_sound_fx = false;
        c.disable_stunt_screen_fx = false; c.gta_vi_time_clock = false; c.disable_pursuit_camera = false;
        c.fix_chase_aim_camera = false; c.hide_cop_radar_icons = false; c.modern_brake_reverse = false;
        c.no_wheel_turn_back = false; c.disable_controller_camera_autocenter = false;
        c.disable_onfoot_camera_autocenter = false; c.speed_rumble = false; c.dynamic_radar = false;
    }
    // Test switches kept from the build 094-097 crash hunt (both default ON now). The crashes were a build problem on our
    // side: clang dropped SPatch's __try guards around direct memory reads (fixed with -Xclang -fasync-exceptions in build.sh).
    if (!Key(ini, L"VoiceLines", 1)) {
        c.restore_push_voice_lines = false; c.restore_combat_lines = false; c.restore_combat_lines_all_voices = false;
        c.driving_swear_lines = false; c.cop_pursuit_swear_lines = false; c.restore_police_voice_lines = false;
        c.restore_encourage_voice_lines = false;
    }
    if (!Key(ini, L"CombatListFix", 1)) c.fix_combat_list_selection = false;
    // Sweat is left out of the VR mod (user decision, 11 Nov 2026: small feature, not worth the risk). sdvr.ini [SPatch] Sweat=1
    // brings it back for testing only.
    if (!Key(ini, L"Sweat", 0)) c.restore_character_sweat = false;
    c.gtav_speed_shake = false;   // camera shake at speed: never in VR (the head is the camera)
    // Graphics: restorations and presentation changes - each needs checking per eye before it is allowed in VR
    if (!graphics) {
        c.restore_original_fog = false; c.improve_spherical_reflections = false; c.anisotropic_filtering = -1;
        c.force_anisotropic_filtering = false; c.override_shadow_filter = -1; c.override_texture_detail_level = -1;
        c.override_motion_blur = -1; c.override_low_res_buffer = -1;
        c.disable_dof = false; c.disable_cutscene_dof = false; c.headlight_shadows = false; c.siren_shadows = false;
        c.shadow_resolution = 0; c.headlight_beam_light = false; c.agx_enabled = false; c.icon_stabilizer = false;
    }
    // Not in VR: the headset owns resolution, refresh and pacing, and the eyes are composed by the VR mod, so SPatch's
    // display overrides, frame limiter and its present-time SMAA pass stay off.
    c.fix_first_run_resolution = false; c.prefer_max_refresh_rate = false; c.override_fullscreen = -1;
    c.custom_frame_limiter = false; c.custom_frame_limiter_fps = 0.f; c.warn_low_res_buffer = false;
    c.smaa_enable = false; c.hook_smaa_present = false; c.smaa_disable_stock_aa = false;
    char line[256];
    wsprintfA(line, "spatch: groups - stability %d, timing %d, content %d, features %d, graphics %d (enabled %d)", stability, timing, content, features, graphics, c.enabled ? 1 : 0);
    SdvrSpatchLog(line);
}
}  // namespace spatch
