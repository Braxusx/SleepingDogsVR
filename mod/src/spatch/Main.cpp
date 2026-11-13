#include "BootstrapPolicy.h"
#include "Config.h"
#include "DisplaySettings.h"
#include "Hooks.h"
#include "Logger.h"
#include "PoolDiagnostics.h"
#include "ShadowScale.h"
#include "PoolHeadroom.h"
#include "SystemLibrary.h"
#include "VersionGuard.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace spatch { void SdvrAdjustConfig(Config& config); }   // SDVR bridge (SdvrBridge.cpp)
namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE,
                                          DWORD,
                                          HANDLE,
                                          MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

HMODULE g_module = nullptr;

spatch::Config g_config{};
std::filesystem::path g_module_directory;
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_previous_exception_filter = nullptr;
std::atomic<bool> g_crash_filter_ready = false;
HMODULE g_dbghelp_module = nullptr;
MiniDumpWriteDumpFn g_mini_dump_write_dump = nullptr;
std::atomic<bool> g_minidumps_enabled = false;
std::atomic_flag g_dump_in_progress = ATOMIC_FLAG_INIT;
std::atomic<unsigned long> g_dump_serial = 0;
std::atomic<bool> g_detaching = false;
constexpr std::size_t kDumpPathCapacity = 32768;
std::array<wchar_t, kDumpPathCapacity> g_dump_path_buffer{};

// The crash-time pool report's own buffers. They are file-scope statics
// rather than stack locals because the crash filter runs on the faulting
// thread, where a large stack frame can turn a crash into a stack
// overflow. The report is written at most once per crash, under the
// dump-in-progress guard, so the buffers are never used twice at once.
constexpr std::size_t kPoolReportNameCapacity = 1024;
constexpr std::size_t kPoolReportCapacity = 8192;
std::array<wchar_t, kPoolReportNameCapacity> g_pool_report_name{};
std::array<char, kPoolReportCapacity> g_pool_report_buffer{};

// The game's executable base, cached at bootstrap.
//
// The crash handler must not call GetModuleHandleW: that takes the
// loader lock, and a fault on a thread that died inside a loader
// operation would make the crash handler block instead of reporting. The
// base never changes for the life of the process, so it is read once and
// published here.
std::atomic<std::uintptr_t> g_game_module_base{0};

void AbortNeverStartedBootstrapThread(HANDLE thread) noexcept {
    if (thread == nullptr) {
        return;
    }
    // The handle is created suspended and ResumeThread has never succeeded,
    // so no user code, TLS, or CRT cleanup can have run. Terminating this
    // never-started bootstrap object is the only way to avoid a detached start
    // address after DllMain rejects the load.
#pragma warning(suppress : 6258)
    (void)TerminateThread(thread, ERROR_DLL_INIT_FAILED);
}

std::wstring GetModulePath(HMODULE module) {
    try {
        std::wstring buffer(MAX_PATH, L'\0');

        for (;;) {
            const DWORD length =
                GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0) {
                return {};
            }

            if (length < buffer.size()) {
                buffer.resize(length);
                return buffer;
            }
            // Windows paths are bounded well below this value in normal use;
            // keep a corrupt/hostile loader result from growing the bootstrap
            // string without limit.
            if (buffer.size() >= 32768 / 2) {
                return {};
            }
            buffer.resize(buffer.size() * 2);
        }
    } catch (...) {
        return {};
    }
}

bool FormatTimestampedDumpPath(unsigned long serial) {
    SYSTEMTIME system_time{};
    GetLocalTime(&system_time);

    const int written = _snwprintf_s(g_dump_path_buffer.data(),
                                     g_dump_path_buffer.size(),
                                     _TRUNCATE,
                                     L"%ls\\SPatch-%04u%02u%02u-%02u%02u%02u-%03u-p%lu-%lu.dmp",
                                     g_module_directory.c_str(),
                                     system_time.wYear,
                                     system_time.wMonth,
                                     system_time.wDay,
                                     system_time.wHour,
                                     system_time.wMinute,
                                     system_time.wSecond,
                                     system_time.wMilliseconds,
                                     GetCurrentProcessId(),
                                     serial);
    return written > 0;
}

// Writes the engine's Main Pool state next to the crash dump. The pool
// object lives in the game's uninitialized .data tail, which the minidump
// does not capture, so the dump on its own cannot say whether the pool
// ran out of room. This file is the only place that answer survives the
// crash.
//
// The path mirrors the dump path with a .pool.txt suffix, so the two
// files sit together and are picked up together.
//
// The file is written with the Win32 API rather than the C++ stream
// layer. A crashing process may have died holding a lock inside the
// runtime's iostream machinery, and a crash handler that then blocks on
// that lock would hang instead of producing the report.
void WritePoolCrashReport(
    std::uintptr_t fault_address,
    unsigned long fault_code) {
    try {
        // The base is read from the cache, not from GetModuleHandleW: that
        // call takes the loader lock, which a faulting thread may already
        // be holding.
        const std::uintptr_t game_base =
            g_game_module_base.load(std::memory_order_acquire);
        if (game_base == 0) {
            return;
        }

        // Everything below uses plain data and fixed buffers. A crashing
        // process may have a damaged heap, and the crash handler must not
        // ask the allocator for memory.
        spatch::pool_diagnostics::PoolSample state{};
        if (!spatch::pool_diagnostics::ReadPoolSample(game_base, &state)) {
            return;
        }

        std::array<spatch::pool_diagnostics::PoolSample,
                   spatch::pool_diagnostics::kPoolHistoryDepth>
            history{};
        const std::size_t history_count = spatch::pool_diagnostics::CopyHistory(
            history.data(), history.size());

        // The report path is the dump path with a different suffix. It is
        // built in a fixed buffer: a std::wstring would ask the allocator
        // for memory and take the CRT heap lock, and a crashing process
        // may have died holding either.
        if (!spatch::pool_diagnostics::BuildPoolReportPath(
                g_dump_path_buffer.data(),
                g_pool_report_name.data(),
                g_pool_report_name.size())) {
            return;
        }

        const spatch::pool_diagnostics::CrashContext crash =
            spatch::pool_diagnostics::DescribeCrash(
                game_base, fault_address, fault_code);

        const std::size_t report_size = spatch::pool_diagnostics::FormatPoolReportInto(
            g_pool_report_buffer.data(),
            g_pool_report_buffer.size(),
            state,
            history_count > 0 ? history.data() : nullptr,
            history_count,
            crash);

        HANDLE file = CreateFileW(g_pool_report_name.data(),
                                  GENERIC_WRITE,
                                  FILE_SHARE_READ,
                                  nullptr,
                                  CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }
        DWORD written = 0;
        WriteFile(file,
                  g_pool_report_buffer.data(),
                  static_cast<DWORD>(report_size),
                  &written,
                  nullptr);
        CloseHandle(file);
    } catch (...) {
        // A crash handler must never fault or throw. The pool state is a
        // diagnostic, not a correctness requirement.
    }
}

std::string Sha256Hex(
    const std::array<unsigned char, 32>& hash) {
    std::ostringstream stream;
    stream << std::hex << std::uppercase << std::setfill('0');
    for (const unsigned char byte : hash) {
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return stream.str();
}

void LogModuleIdentity(
    const std::filesystem::path& module_path,
    bool logging_enabled) noexcept {
    if (!logging_enabled) {
        return;
    }
    try {
        std::error_code size_error;
        const std::uintmax_t file_size =
            std::filesystem::file_size(module_path, size_error);
        std::array<unsigned char, 32> sha256{};
        const bool hash_computed =
            spatch::ComputeSha256FromPath(module_path, sha256);
        // Already the ASI's first log line, and already the strongest provenance
        // available: the DEPLOYED file's own sha256, which is directly
        // comparable to a build hash. A separate `spatch_build` line was added
        // and then merged here, but it was redundant -- the deployed sha256
        // answers "which binary produced this session" on its own, and did so
        // correctly for every session in the log. The apparent ambiguity was a
        // defect in a throwaway parser, not in the ASI.
        spatch::log::InfoF(
            "module_identity path=%s file_size=%s sha256=%s",
            spatch::log::ToUtf8(module_path.wstring()).c_str(),
            size_error ? "unavailable" : std::to_string(file_size).c_str(),
            hash_computed ? Sha256Hex(sha256).c_str() : "unavailable");
    } catch (...) {
        spatch::log::Warn(
            "module_identity unavailable because diagnostics formatting failed");
    }
}

LONG WINAPI TopLevelExceptionFilter(EXCEPTION_POINTERS* exception_pointers);

LONG ContinueExceptionSearch(EXCEPTION_POINTERS* exception_pointers) {
    if (!g_crash_filter_ready.load(std::memory_order_acquire)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const LPTOP_LEVEL_EXCEPTION_FILTER previous =
        g_previous_exception_filter.load(std::memory_order_acquire);
    if (previous != nullptr && previous != &TopLevelExceptionFilter) {
        return previous(exception_pointers);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI TopLevelExceptionFilter(EXCEPTION_POINTERS* exception_pointers) {
    if (!g_minidumps_enabled.load(std::memory_order_acquire) ||
        g_mini_dump_write_dump == nullptr || g_module_directory.empty()) {
        return ContinueExceptionSearch(exception_pointers);
    }
    if (g_dump_in_progress.test_and_set(std::memory_order_acquire)) {
        return ContinueExceptionSearch(exception_pointers);
    }

    HANDLE file = INVALID_HANDLE_VALUE;
    constexpr unsigned int kMaximumNameAttempts = 32;
    for (unsigned int attempt = 0; attempt < kMaximumNameAttempts; ++attempt) {
        const unsigned long serial =
            g_dump_serial.fetch_add(1, std::memory_order_relaxed);
        if (!FormatTimestampedDumpPath(serial)) {
            break;
        }
        file = CreateFileW(g_dump_path_buffer.data(),
                           GENERIC_WRITE,
                           0,
                           nullptr,
                           CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
        const DWORD create_error = GetLastError();
        if (file != INVALID_HANDLE_VALUE ||
            (create_error != ERROR_FILE_EXISTS &&
             create_error != ERROR_ALREADY_EXISTS)) {
            break;
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        g_dump_in_progress.clear(std::memory_order_release);
        return ContinueExceptionSearch(exception_pointers);
    }

    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = exception_pointers;
    info.ClientPointers = FALSE;

    // Sample the engine's Main Pool before the dump is written. The pool
    // object lives in the game's uninitialized .data tail, so the minidump
    // does not contain it: without this line the one piece of state that
    // says whether the pool ran out of room is lost with the crash. The
    // read is SEH-guarded inside the diagnostics module, so a torn heap
    // here cannot fault the crash handler.
    //
    // The faulting address is passed in because it is what settles
    // whether the pool ran out of room: the Main Pool's allocator has
    // exactly one place where it reports an out-of-memory failure and
    // stops, and a fault there means the pool could not serve the
    // request.
    std::uintptr_t fault_address = 0;
    unsigned long fault_code = 0;
    if (exception_pointers != nullptr &&
        exception_pointers->ExceptionRecord != nullptr) {
        fault_address = reinterpret_cast<std::uintptr_t>(
            exception_pointers->ExceptionRecord->ExceptionAddress);
        fault_code = static_cast<unsigned long>(
            exception_pointers->ExceptionRecord->ExceptionCode);
    }
    WritePoolCrashReport(fault_address, fault_code);

    const BOOL dump_written = g_mini_dump_write_dump(GetCurrentProcess(),
                                                     GetCurrentProcessId(),
                                                     file,
                                                     MiniDumpNormal,
                                                     &info,
                                                     nullptr,
                                                     nullptr);
    CloseHandle(file);
    if (!dump_written) {
        DeleteFileW(g_dump_path_buffer.data());
    }
    g_dump_in_progress.clear(std::memory_order_release);
    return ContinueExceptionSearch(exception_pointers);
}

// The headroom attempt made from the loader phase, and whether one was
// made at all.
//
// The attempt happens in DllMain, before the game has started running,
// and the bootstrap thread reads the result afterwards. The result is
// plain data, and the thread that reads it was created after the attempt
// finished, so it needs no lock of its own.
spatch::pool_headroom::HeadroomPatchResult g_early_headroom{};
std::atomic<bool> g_early_headroom_attempted{false};

// Writes the headroom patch from the loader phase.
//
// The mod is loaded by the Simple ASI Loader, which the game executable
// imports statically, and that loader loads every .asi file in the game
// directory from its own DllMain. The mod's DllMain therefore runs while
// the loader still holds its lock: before the game's entry point, and so
// before the game can create its Main Pool. A patch written from here
// always lands in time. The bootstrap thread is too late on its own: the
// first live run of this feature measured the pool already created by the
// time the bootstrap thread reached the patch.
//
// The work is deliberately small and bounded, because the loader lock is
// held while it runs and everything done under it delays the whole game
// start: the configuration is read for the one value the option needs,
// the game executable is hashed so the researched RVAs are known to mean
// what they mean, and the patch is written. Nothing else happens here.
void AttemptEarlyPoolHeadroom() noexcept {
    try {
        const std::filesystem::path module_path = GetModulePath(g_module);
        if (!spatch::IsUsableBootstrapModulePath(module_path)) {
            return;
        }

        const std::filesystem::path config_path =
            module_path.parent_path() / "SPatch.ini";
        const spatch::EarlyConfig early = spatch::ReadEarlyConfig(config_path);
        if (!early.enabled || early.pool_headroom_mib <= 0) {
            // The option is off, or the mod is disabled. There is nothing
            // to patch and the game starts without the delay.
            return;
        }

        // The game executable's base and path. A module that is already
        // loaded is looked up without taking the loader lock, and this
        // thread already holds it.
        const std::uintptr_t game_base =
            reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
        const std::filesystem::path game_exe = GetModulePath(nullptr);
        if (game_base == 0 || game_exe.empty()) {
            return;
        }

        g_early_headroom = spatch::pool_headroom::ApplyPoolHeadroomInLoaderPhase(
            game_exe, game_base, early.pool_headroom_mib);
        g_early_headroom_attempted.store(true, std::memory_order_release);
    } catch (...) {
        // The loader phase must never fail the load. The bootstrap thread
        // still attempts the patch later, and the receipt then says the
        // patch landed too late.
    }
}

DWORD BootstrapMain() {
    if (g_detaching.load()) {
        return 0;
    }

    const std::filesystem::path module_path = GetModulePath(g_module);
    if (!spatch::IsUsableBootstrapModulePath(module_path)) {
        OutputDebugStringA(
            "SPatch bootstrap aborted: module path is empty or not absolute\n");
        return ERROR_BAD_PATHNAME;
    }
    g_module_directory = module_path.parent_path();

    const std::filesystem::path config_path = g_module_directory / "SPatch.ini";
    spatch::ConfigLoadReport config_report{};
    g_config = spatch::LoadConfig(config_path, &config_report);
    spatch::SdvrAdjustConfig(g_config);   // SDVR: feature groups for VR (SdvrBridge.cpp)

    const std::filesystem::path log_path = g_module_directory / "SPatch.log";
    if (!spatch::log::Initialize(log_path, g_config.enable_logging)) {
        // Diagnostics must never become a prerequisite for the patch itself.
        OutputDebugStringA("SPatch could not open SPatch.log; continuing without file logging\n");
    }

    // ShadowScale: arm the d3d11 device-creation hooks before anything can
    // race the game's device creation. Self-gated: no-op unless
    // [Shadows] ShadowResolution is set in SPatch.ini.
    spatch::shadow_scale::StartDeviceWatch();

    LogModuleIdentity(module_path, g_config.enable_logging);
    spatch::log::Info("SPatch bootstrap starting");

    spatch::log::InfoF(
        "config core enabled=%d allow_unverified=%d crash_dumps=%d logging=%d "
        "source_version=%d schema_version=%d persistence=%s",
        g_config.enabled ? 1 : 0,
        g_config.allow_unverified_build ? 1 : 0,
        g_config.write_minidumps ? 1 : 0,
        g_config.enable_logging ? 1 : 0,
        config_report.source_version,
        spatch::kConfigVersion,
        spatch::ConfigPersistenceStatusName(config_report.persistence));
    if (!config_report.persistence_succeeded()) {
        spatch::log::WarnF("configuration persistence failed status=%s path=%s; "
                           "using the values that could be read without replacing the file",
                           spatch::ConfigPersistenceStatusName(config_report.persistence),
                           spatch::log::ToUtf8(config_path.wstring()).c_str());
    }
    // The stunt options were the only ones never recorded at startup. With no
    // line naming them, a capture where the operator's toggle was off is
    // indistinguishable from one where the hook silently failed to work, which
    // is the whole reason the previous three attempts were unreadable. The
    // live gates read their option state from this line.
    spatch::log::InfoF(
        "requested_config gameplay disable_driveby_slow_mo=%d disable_stunt_slow_mo=%d "
        "disable_stunt_screen_fx=%d disable_stunt_sound_fx=%d slow_mo_probe=%d "
        "screen_filter_probe=%d",
        g_config.disable_driveby_slow_mo ? 1 : 0,
        g_config.disable_stunt_slow_mo ? 1 : 0,
        g_config.disable_stunt_screen_fx ? 1 : 0,
        g_config.disable_stunt_sound_fx ? 1 : 0,
        g_config.probe_slow_mo ? 1 : 0,
        g_config.probe_screen_filter ? 1 : 0);
    spatch::log::InfoF(
        "requested_config timing cutscene_fix=%d cutscene_fps=%d smoothing=%d fog_guard=%d",
        (g_config.fix_cutscene_zero_dt || g_config.fix_cutscene_scene_time_step) ? 1 : 0,
        g_config.cutscene_fps,
        g_config.time_step_smoothing,
        g_config.hook_fog_slicing_guard ? 1 : 0);
    spatch::log::InfoF(
        "requested_config input gta_iv_car_camera=%d gta_iv_bike_camera=%d "
        "disable_controller_camera_autocenter=%d "
        "disable_onfoot_camera_autocenter=%d "
        "fix_chase_aim_camera=%d "
        "gtav_speed_shake=%d gtav_speed_shake_strength_percent=%d "
        "gtav_speed_shake_start_speed_kmh=%d "
        "gtav_speed_shake_full_speed_kmh=%d",
        g_config.gta_iv_car_camera ? 1 : 0,
        g_config.gta_iv_bike_camera ? 1 : 0,
        g_config.disable_controller_camera_autocenter ? 1 : 0,
        g_config.disable_onfoot_camera_autocenter ? 1 : 0,
        g_config.fix_chase_aim_camera ? 1 : 0,
        g_config.gtav_speed_shake ? 1 : 0,
        g_config.gtav_speed_shake_strength_percent,
        g_config.gtav_speed_shake_start_speed_kmh,
        g_config.gtav_speed_shake_full_speed_kmh);
    spatch::log::InfoF(
        "requested_config display maximum_refresh=%d fullscreen=%d remove_120_cap=%d "
        "custom_limiter=%d custom_limiter_fps=%.1f",
        g_config.prefer_max_refresh_rate ? 1 : 0,
        g_config.override_fullscreen,
        g_config.remove_hidden_120_fps_cap ? 1 : 0,
        g_config.custom_frame_limiter ? 1 : 0,
        static_cast<double>(g_config.custom_frame_limiter_fps));
    spatch::log::InfoF(
        "requested_config texture_filtering anisotropy=%d force_verified_trilinear=%d",
        g_config.anisotropic_filtering,
        g_config.force_anisotropic_filtering ? 1 : 0);
    spatch::log::InfoF(
        "requested_config tonemapping agx=%d look=%s strength=%d exposure=%d",
        g_config.agx_enabled ? 1 : 0,
        g_config.agx_look == 0 ? "Neutral" : "MediumHigh",
        g_config.agx_strength,
        g_config.agx_exposure);

    spatch::log::InfoF(
        "requested_engine_fix_config pedestrian_density=%d spherical_reflections=%d spherical_width=%d "
        "original_fog_and_neon=%d original_eye_reflections=%d character_wetness=%d "
        "character_sweat=%d "
        "high_fps_averages=%d remove_120_fps_cap=%d "
        "corrupt_save_guard=%d thread_failure_guard=%d",
        g_config.fix_pedestrian_density_at_high_fps ? 1 : 0,
        g_config.improve_spherical_reflections ? 1 : 0,
        g_config.spherical_reflection_width,
        g_config.restore_original_fog ? 1 : 0,
        g_config.restore_original_eye_reflections ? 1 : 0,
        g_config.restore_character_wetness ? 1 : 0,
        g_config.restore_character_sweat ? 1 : 0,
        g_config.fix_high_fps_average_windows ? 1 : 0,
        g_config.remove_hidden_120_fps_cap ? 1 : 0,
        g_config.fix_corrupt_save_handling ? 1 : 0,
        g_config.fix_thread_creation_failure ? 1 : 0);
    spatch::log::InfoF(
        "requested_engine_guard_config first_run_resolution=%d scaleform_clock=%d "
        "file_timestamp=%d audio_open=%d large_files=%d vram_pool_lock=%d "
        "vram_reporting=%d resource_loading=%d",
        g_config.fix_first_run_resolution ? 1 : 0,
        g_config.fix_scaleform_qpc_clock ? 1 : 0,
        g_config.fix_file_timestamp_open_mode ? 1 : 0,
        g_config.fix_audio_file_open ? 1 : 0,
        g_config.fix_large_file_sizes ? 1 : 0,
        g_config.fix_vram_pool_lock ? 1 : 0,
        g_config.fix_vram_capacity_reporting ? 1 : 0,
        g_config.fix_resource_loading ? 1 : 0);

    spatch::log::InfoF("requested_cutscene_timing target_fps=%d mode=%s zero_dt_fix=%d scene_time_fix=%d",
                       g_config.cutscene_fps,
                       g_config.cutscene_fps == 0 ? "auto" : "configured",
                       g_config.fix_cutscene_zero_dt ? 1 : 0,
                       g_config.fix_cutscene_scene_time_step ? 1 : 0);

    if (!g_config.enabled) {
        spatch::log::Warn("SPatch disabled by config");
        return 0;
    }

    if (g_config.write_minidumps) {
        g_dbghelp_module = spatch::LoadSystemLibrary(L"dbghelp.dll");
        if (g_dbghelp_module != nullptr) {
            g_mini_dump_write_dump = reinterpret_cast<MiniDumpWriteDumpFn>(
                GetProcAddress(g_dbghelp_module, "MiniDumpWriteDump"));
        }
        if (g_mini_dump_write_dump != nullptr) {
            // The callback becomes visible inside SetUnhandledExceptionFilter.
            // Keep it in a disabled no-chain state until the returned previous
            // filter and all dump dependencies have been safely published.
            g_crash_filter_ready.store(false, std::memory_order_relaxed);
            const LPTOP_LEVEL_EXCEPTION_FILTER previous =
                SetUnhandledExceptionFilter(&TopLevelExceptionFilter);
            g_previous_exception_filter.store(previous, std::memory_order_release);
            g_crash_filter_ready.store(true, std::memory_order_release);
            g_minidumps_enabled.store(true, std::memory_order_release);
        } else {
            spatch::log::Warn("MiniDumpWriteDump unavailable; crash dumps disabled");
            if (g_dbghelp_module != nullptr) {
                FreeLibrary(g_dbghelp_module);
                g_dbghelp_module = nullptr;
            }
        }
    }

    const spatch::BuildCheckResult build = spatch::InspectLoadedGame();
    spatch::log::Info(build.summary);

    // The crash handler reads the game's base from here rather than
    // asking for it, because asking takes the loader lock.
    g_game_module_base.store(
        reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr)),
        std::memory_order_release);

    if (g_detaching.load()) {
        return 0;
    }

    // EXPERIMENTAL pool headroom. The game creates its Main Pool itself,
    // so this only has an effect when the patch lands before that
    // creation. The patch is written from the loader phase, before the
    // game starts running, so it always lands in time. The result
    // reports which of the two happened, and the crash-time pool report
    // shows the reservation the pool actually ended up with, so the
    // option is never a guess.
    {
        spatch::pool_headroom::HeadroomPatchResult headroom{};
        const bool early_attempted =
            g_early_headroom_attempted.load(std::memory_order_acquire);
        if (early_attempted && g_early_headroom.applied) {
            // The loader phase already wrote the patch, before the game
            // had started running. Writing it again here would be a
            // second attempt at a patch that is already in place.
            headroom = g_early_headroom;
            spatch::log::InfoF(
                "pool_headroom reserved_mib=%d applied from the loader "
                "phase, before the game started",
                headroom.requested_mib);
        } else {
            // The fallback for a mod that was loaded after the game had
            // already started. The patch can only land in time here when
            // the game has not created the pool yet.
            const std::uintptr_t game_base =
                reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
            headroom = spatch::pool_headroom::ApplyPoolHeadroom(
                g_config, game_base, build.build_id, build.hook_layout_supported);
            if (headroom.applied && !headroom.pool_already_created) {
                spatch::log::InfoF(
                    "pool_headroom reserved_mib=%d applied before pool creation",
                    headroom.requested_mib);
            }
        }

        // The reservation the pool really ended up with. The game creates
        // the pool in its first moments, so the read is polled for a
        // bounded time: the wait must not hold the bootstrap up when the
        // pool never appears.
        {
            const std::uintptr_t game_base =
                g_game_module_base.load(std::memory_order_acquire);
            headroom.effective_mib_after_creation =
                spatch::pool_diagnostics::WaitForPoolCreation(game_base, 250);
            headroom.pool_creation_observed =
                headroom.effective_mib_after_creation >= 0;
            spatch::log::InfoF(
                "pool_reservation_observed mib=%d observed=%d",
                headroom.effective_mib_after_creation,
                headroom.pool_creation_observed ? 1 : 0);
        }

        // The option is experimental, so the user has to be able to read
        // what it did without turning on logging. The receipt is written
        // next to SPatch.ini whenever the option asked for something, so
        // the result is never left to a log line the user may not have
        // enabled.
        if (headroom.requested_mib > 0) {
            const std::filesystem::path receipt_path =
                g_module_directory / "SPatch-pool-headroom.txt";
            if (!spatch::pool_headroom::WritePoolHeadroomReceipt(
                    receipt_path, headroom)) {
                spatch::log::Warn(
                    "could not write the pool headroom receipt next to "
                    "SPatch.ini");
            } else {
                spatch::log::InfoF(
                    "pool headroom receipt written to %ls",
                    receipt_path.c_str());
            }
        }
    }

    // Sample the pool while the game runs, so the crash report can show
    // how the pool moved before it failed rather than only its final
    // state. The sampler only reads.
    {
        const std::uintptr_t game_base =
            reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
        spatch::pool_diagnostics::StartSampler(game_base, 15000);
    }

    const spatch::BootstrapPrepareResult prepare =
        spatch::PrepareBootstrapHooks(g_config, build, g_detaching.load());
    g_config = prepare.effective_config;

    if (prepare.status == spatch::BootstrapPrepareStatus::DetachedBeforeHooks) {
        return 0;
    }

    if (prepare.status == spatch::BootstrapPrepareStatus::UnsupportedBuild) {
        spatch::log::Warn("unsupported game build; hooks not installed");
        spatch::log::Info("hook_result requested=0 installed=0 reason=unsupported_build");
        return 0;
    }

    const bool hooks_requested =
        prepare.status ==
        spatch::BootstrapPrepareStatus::ReadyForHookInitialization;
    spatch::log::InfoF(
        "effective_profile build_id=%s hooks_requested=%d safe_compatibility=%d "
        "latest_steam=%d smaa=%d character_sweat=%d "
        "duplicate_actor_restore=%d gta_iv_car_camera=%d "
        "gta_iv_bike_camera=%d disable_controller_camera_autocenter=%d "
        "disable_onfoot_camera_autocenter=%d gtav_speed_shake=%d "
        "gtav_speed_shake_strength_percent=%d "
        "gtav_speed_shake_start_speed_kmh=%d "
        "gtav_speed_shake_full_speed_kmh=%d rumble=%d",
        build.build_id.c_str(),
        hooks_requested ? 1 : 0,
        prepare.safe_compatibility_mode ? 1 : 0,
        prepare.latest_steam_profile ? 1 : 0,
        (g_config.hook_smaa_present && g_config.smaa_enable) ? 1 : 0,
        g_config.restore_character_sweat ? 1 : 0,
        g_config.fix_nis_actor_restore_duplicates ? 1 : 0,
        g_config.gta_iv_car_camera ? 1 : 0,
        g_config.gta_iv_bike_camera ? 1 : 0,
        g_config.disable_controller_camera_autocenter ? 1 : 0,
        g_config.disable_onfoot_camera_autocenter ? 1 : 0,
        g_config.gtav_speed_shake ? 1 : 0,
        g_config.gtav_speed_shake_strength_percent,
        g_config.gtav_speed_shake_start_speed_kmh,
        g_config.gtav_speed_shake_full_speed_kmh,
        g_config.override_rumble_enabled);

    // Do not touch user settings until the executable policy has accepted the
    // build. Safe compatibility mode may apply only these build-independent
    // XML settings; fixed-address behavior has already been disabled above.
    const std::filesystem::path display_settings_path =
        spatch::ResolveDisplaySettingsPath(g_module_directory);
    if (spatch::ShouldApplyDisplaySettings(prepare.status)) {
        spatch::log::InfoF("display_settings selected_path=%s",
                           spatch::log::ToUtf8(display_settings_path.c_str()).c_str());
        spatch::ApplyDisplaySettingsPatches(display_settings_path, g_config);
    }

    if (prepare.safe_compatibility_mode) {
        spatch::log::WarnF("safe compatibility mode active build_id=%s; hook-based features disabled until this executable is remapped",
                           build.build_id.c_str());
        spatch::log::Info(
            "hook_result requested=0 installed=0 mode=settings_only");
        spatch::log::Info("SPatch initialized in settings-only mode");
        return 0;
    }

    if (prepare.latest_steam_profile) {
        spatch::log::WarnF(
            "latest_steam compatibility profile active; smaa=%d character_sweat=%d "
            "duplicate_actor_restore=%d gta_iv_car_camera=%d "
            "gta_iv_bike_camera=%d disable_controller_camera_autocenter=%d "
            "disable_onfoot_camera_autocenter=%d gtav_speed_shake=%d "
            "gtav_speed_shake_strength_percent=%d "
            "gtav_speed_shake_start_speed_kmh=%d "
            "gtav_speed_shake_full_speed_kmh=%d rumble=%d",
            (g_config.hook_smaa_present && g_config.smaa_enable) ? 1 : 0,
            g_config.restore_character_sweat ? 1 : 0,
            g_config.fix_nis_actor_restore_duplicates ? 1 : 0,
            g_config.gta_iv_car_camera ? 1 : 0,
            g_config.gta_iv_bike_camera ? 1 : 0,
            g_config.disable_controller_camera_autocenter ? 1 : 0,
            g_config.disable_onfoot_camera_autocenter ? 1 : 0,
            g_config.gtav_speed_shake ? 1 : 0,
            g_config.gtav_speed_shake_strength_percent,
            g_config.gtav_speed_shake_start_speed_kmh,
            g_config.gtav_speed_shake_full_speed_kmh,
            g_config.override_rumble_enabled);
    }

    if (g_detaching.load()) {
        return 0;
    }

    const spatch::BootstrapFinalizeStatus finalize =
        spatch::FinalizeBootstrapHooks(
            spatch::hooks::Initialize(g_config,
                                      config_path,
                                      display_settings_path,
                                      build.build_id,
                                      build.hook_layout_supported));
    if (finalize == spatch::BootstrapFinalizeStatus::HookInitializationFailed) {
        spatch::log::Error("hook initialization failed");
        spatch::log::Info("hook_result requested=1 installed=0");
        spatch::hooks::Shutdown();
        return 0;
    }

    spatch::log::Info("hook_result requested=1 installed=1");
    // No provenance line here: module_identity, written at the top of this run,
    // already carries the deployed file's sha256. See the artifact's Provenance
    // section for why a second marker was tried and removed.
    spatch::log::Info("SPatch initialized");
    return 0;
}

DWORD WINAPI BootstrapThread(void*) noexcept {
    try {
        return BootstrapMain();
    } catch (const std::exception& error) {
        try {
            spatch::hooks::Shutdown();
        } catch (...) {
        }
        spatch::log::ErrorF("bootstrap aborted by C++ exception: %s", error.what());
        OutputDebugStringA("SPatch bootstrap aborted by C++ exception\n");
    } catch (...) {
        try {
            spatch::hooks::Shutdown();
        } catch (...) {
        }
        spatch::log::Error("bootstrap aborted by unknown C++ exception");
        OutputDebugStringA("SPatch bootstrap aborted by unknown C++ exception\n");
    }
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        g_detaching.store(false, std::memory_order_relaxed);

        // The pool headroom patch has to be written before the game
        // creates its Main Pool, and the game creates the pool in the
        // first moments after its entry point. This DllMain runs while the
        // loader still holds its lock, because the mod is loaded from the
        // ASI loader's own DllMain, so the game has not started running
        // yet and the patch always lands in time.
        AttemptEarlyPoolHeadroom();

        // Create the worker before pinning so a resource failure can reject
        // the DLL load cleanly. It stays suspended until the process-lifetime
        // module pin is established and all globals above are published.
        HANDLE thread =
            CreateThread(nullptr, 0, &BootstrapThread, nullptr, CREATE_SUSPENDED, nullptr);
        if (thread == nullptr) {
            return FALSE;
        }

        // Every installed hook points back into this image. Pinning makes the
        // documented process-lifetime contract enforceable and prevents a
        // third-party FreeLibrary call from unmapping active detours.
        HMODULE pinned_module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(module),
                               &pinned_module)) {
            AbortNeverStartedBootstrapThread(thread);
            CloseHandle(thread);
            return FALSE;
        }
        if (ResumeThread(thread) == static_cast<DWORD>(-1)) {
            AbortNeverStartedBootstrapThread(thread);
            CloseHandle(thread);
            return FALSE;
        }
        CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_detaching.store(true);
        if (reserved != nullptr) {
            // The process is terminating. The operating system has already
            // ended the other threads, so waiting for the sampler would
            // hang on a thread that no longer runs. The thread must still
            // be detached: the CRT runs its destructor after DllMain
            // returns, and a destroyed-but-never-joined thread terminates
            // the process right there.
            spatch::pool_diagnostics::DetachSamplerForProcessExit();
        } else {
            // The module is being unloaded, so the sampler thread must not
            // still be running code inside this module.
            spatch::pool_diagnostics::StopSampler();
        }
    }

    return TRUE;
}
