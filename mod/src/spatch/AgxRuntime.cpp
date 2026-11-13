// Native AgX tonemapping port for SPatch.
//
// Sleeping Dogs reconstructs scene-linear color in one exact pixel shader
// (identity: size 1852, DXBC checksum, CRC32 0x67843125 - see AgxPolicy.h)
// immediately before the HUD. This runtime installs a MinHook detour on
// ID3D11Device::CreatePixelShader (vtable slot 15) and substitutes the
// compiled AgX replacement for that exact bytecode, leaving every other
// shader and the game's pipeline requests untouched.
//
// The mechanism is the SPatch-native analogue of the proven ShenLong
// add-on: same identity contract, same macro-driven compile (one variant at
// startup), same driver validation via a throwaway CreatePixelShader call.
// The embedding host changed from ReShade's pipeline-create event to a
// native vtable detour because the ReShade runtime is not available to
// SPatch.
//
// Fail-open doctrine: AgX disabled, device missing, compile failure, driver
// rejection, or any identity mismatch each leave the stock shader active.
// The detour is never installed when AgX is disabled; once installed it is
// retained for process lifetime because the bundled MinHook fork has no
// safe disable-only API (frees the trampoline) - same doctrine SMAA
// follows (SmaaRuntime.cpp).

#include "AgxPolicy.h"
#include "AgxRuntime.h"
#include "AgxShaderSource.inl"
#include "HookTargetGuard.h"
#include "Logger.h"
#include "SystemLibrary.h"

#include <Windows.h>
#include <MinHook.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace spatch::agx {
namespace {

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID,
                                      SIZE_T,
                                      LPCSTR,
                                      const D3D_SHADER_MACRO*,
                                      ID3DInclude*,
                                      LPCSTR,
                                      LPCSTR,
                                      UINT,
                                      UINT,
                                      ID3DBlob**,
                                      ID3DBlob**);

using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE ID3D11Device::*)(
    const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);

// ID3D11Device::CreatePixelShader is vtable slot 15 (after IUnknown 0-2,
// CreateBuffer 3, CreateTexture1D/2D/3D 4-6, CreateShaderResourceView 7,
// CreateUnorderedAccessView 8, CreateRenderTargetView 9,
// CreateDepthStencilView 10, CreateInputLayout 11, CreateVertexShader 12,
// CreateGeometryShader 13, CreateGeometryShaderWithStream 14).
constexpr std::size_t kCreatePixelShaderSlot = 15;

std::atomic<bool> g_enabled{false};
std::atomic<int> g_strength_percent{100};
std::atomic<int> g_exposure_percent{100};
std::atomic<int> g_look{1};
std::atomic<int> g_debug_bloom{0};

std::uintptr_t g_device_slot = 0;

std::mutex g_mutex;
bool g_hook_installed = false;
bool g_compile_attempted = false;
// Published under g_mutex, read in the detour; atomic so the pointer
// publication below pairs with an acquire read instead of racing.
std::atomic<bool> g_shader_ready{false};
ComPtr<ID3D11PixelShader> g_replacement_shader;
// The device the cached replacement was compiled against. D3D11 objects
// cannot be bound across devices, so the detour compares identity and fails
// open (then recompiles once) if the game's device was recreated (TDR or
// driver reset). The replacement shader keeps its own device alive, so a raw
// identity comparison is stable.
std::atomic<ID3D11Device*> g_shader_device{nullptr};
std::atomic<int> g_recompile_attempts{0};
// The MinHook trampoline back into the original ID3D11Device method. Set
// once under g_mutex by MaybeInstallHooks before the detour is armed and
// retained for process lifetime (the bundled fork frees trampolines on
// removal).
CreatePixelShaderFn g_original_create_pixel_shader = nullptr;

// Guards our own driver-validation CreatePixelShader call inside the
// detour. Thread-local so a concurrent game create on another thread is
// still substituted normally; our own validation must pass through so it
// can never observe its own replacement (which would hide a driver
// rejection of the replacement bytecode).
thread_local int g_validation_depth = 0;

HMODULE g_d3dcompiler_module = nullptr;
D3DCompileFn g_d3d_compile = nullptr;

template <typename T>
T* ReadSlot(std::uintptr_t slot) noexcept {
    if (slot == 0) {
        return nullptr;
    }
    T* value = nullptr;
    __try {
        value = *reinterpret_cast<T**>(slot);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return nullptr;
    }
    return value;
}

template <typename T>
T* RetainSlotValue(std::uintptr_t slot) noexcept {
    T* value = ReadSlot<T>(slot);
    if (value != nullptr) {
        value->AddRef();
    }
    return value;
}

bool CompileReplacement(ID3D11Device* device) {
    if (device == nullptr) {
        return false;
    }
    if (g_d3d_compile == nullptr) {
        if (g_d3dcompiler_module == nullptr) {
            g_d3dcompiler_module = LoadSystemLibrary(L"D3DCompiler_47.dll");
        }
        if (g_d3dcompiler_module != nullptr) {
            g_d3d_compile = reinterpret_cast<D3DCompileFn>(
                GetProcAddress(g_d3dcompiler_module, "D3DCompile"));
        }
        if (g_d3d_compile == nullptr) {
            log::Error("agx_d3dcompiler_load_fail");
            return false;
        }
    }

    const CompileVariant variant = SelectCompileVariant(
        g_strength_percent.load(std::memory_order_acquire),
        g_exposure_percent.load(std::memory_order_acquire),
        g_look.load(std::memory_order_acquire),
        g_debug_bloom.load(std::memory_order_acquire));
    std::array<char, 32> strength{};
    std::array<char, 32> exposure{};
    sprintf_s(strength.data(), strength.size(), "%.9g", variant.strength);
    sprintf_s(exposure.data(), exposure.size(), "%.9g", variant.exposure);
    const char* full_strength = variant.full_strength ? "1" : "0";
    const char* look = variant.look == 0 ? "0" : "1";
    std::array<char, 32> debug_bloom{};
    sprintf_s(debug_bloom.data(), debug_bloom.size(), "%d", variant.debug_bloom);
    const std::array<D3D_SHADER_MACRO, 6> macros = {{
        {"SPATCH_AGX_EXPOSURE", exposure.data()},
        {"SPATCH_AGX_STRENGTH", strength.data()},
        {"SPATCH_AGX_FULL_STRENGTH", full_strength},
        {"SPATCH_AGX_LOOK", look},
        {"SPATCH_AGX_DEBUG_BLOOM", debug_bloom.data()},
        {nullptr, nullptr},
    }};

    constexpr UINT compile_flags = D3DCOMPILE_ENABLE_STRICTNESS |
        D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3 |
        D3DCOMPILE_WARNINGS_ARE_ERRORS;
    ComPtr<ID3DBlob> errors;
    ComPtr<ID3DBlob> bytecode;
    const HRESULT compile_result = g_d3d_compile(
        kAgxFinalPreHudShader,
        sizeof(kAgxFinalPreHudShader) - 1,
        "SPatchTonemapping.hlsl",
        macros.data(),
        nullptr,
        "main",
        "ps_4_0",
        compile_flags,
        0,
        bytecode.GetAddressOf(),
        errors.ReleaseAndGetAddressOf());
    if (FAILED(compile_result) || bytecode == nullptr) {
        if (errors != nullptr) {
            log::ErrorF("agx_compile_fail hr=0x%08X error=%s",
                        static_cast<unsigned int>(compile_result),
                        static_cast<const char*>(errors->GetBufferPointer()));
        } else {
            log::ErrorF("agx_compile_fail hr=0x%08X",
                        static_cast<unsigned int>(compile_result));
        }
        return false;
    }

    // Validate the driver path before arming the substitution. The call
    // runs under the validation-passthrough guard so the detour (if already
    // installed) cannot substitute this request; the throwaway shader object
    // is released immediately.
    ++g_validation_depth;
    ComPtr<ID3D11PixelShader> validation_shader;
    const HRESULT validation_result = device->CreatePixelShader(
        bytecode->GetBufferPointer(),
        bytecode->GetBufferSize(),
        nullptr,
        validation_shader.ReleaseAndGetAddressOf());
    --g_validation_depth;
    if (FAILED(validation_result) || validation_shader == nullptr) {
        log::ErrorF("agx_driver_reject hr=0x%08X; retaining native tone mapper",
                    static_cast<unsigned int>(validation_result));
        return false;
    }

    // The validated bytecode becomes the cached substitution object.
    ComPtr<ID3D11PixelShader> replacement;
    ++g_validation_depth;
    const HRESULT create_result = device->CreatePixelShader(
        bytecode->GetBufferPointer(),
        bytecode->GetBufferSize(),
        nullptr,
        replacement.GetAddressOf());
    --g_validation_depth;
    if (FAILED(create_result) || replacement == nullptr) {
        log::ErrorF("agx_replacement_create_fail hr=0x%08X",
                    static_cast<unsigned int>(create_result));
        return false;
    }
    g_replacement_shader = std::move(replacement);
    g_shader_device.store(device, std::memory_order_release);
    // The debug mode is the one field that tells the two runs apart: a
    // non-zero value here means this launch replaced the game frame with a
    // view of texHDRBloom, so nothing observed in it says anything about how
    // the stunt screen looks.
    log::InfoF("agx_shader_ready variant=1 size=%lu look=%s strength=%s "
               "debug_bloom=%d",
               static_cast<unsigned long>(bytecode->GetBufferSize()),
               look,
               strength.data(),
               variant.debug_bloom);
    return true;
}

HRESULT STDMETHODCALLTYPE DetourCreatePixelShader(
    ID3D11Device* device,
    const void* shader_bytecode,
    SIZE_T bytecode_length,
    ID3D11ClassLinkage* class_linkage,
    ID3D11PixelShader** pixel_shader) {
    // The MinHook original is captured under g_mutex before the detour can
    // be armed; once armed it is retained for process lifetime (fork
    // doctrine), so a plain read is stable from this point on.
    const CreatePixelShaderFn original = g_original_create_pixel_shader;

    if (!g_shader_ready.load(std::memory_order_acquire) ||
        g_replacement_shader == nullptr || g_validation_depth > 0) {
        // Fail-open: pass through un-substituted. This covers our own
        // validation creates and any create that races the arming.
        return original != nullptr
                   ? (device->*original)(shader_bytecode, bytecode_length,
                                         class_linkage, pixel_shader)
                   : E_FAIL;
    }

    if (device != g_shader_device.load(std::memory_order_relaxed)) {
        // The game's device was recreated (TDR / driver reset). The cached
        // shader belongs to the dead device and must not be handed out cross-
        // device; recompile once or twice against the live device, failing
        // open (native tonemapper) for this and subsequent creates until it
        // succeeds. Manual lock/unlock: this function contains __try, so no
        // object with a destructor may be live in it.
        if (g_mutex.try_lock()) {
            if (device != g_shader_device.load(std::memory_order_relaxed) &&
                g_recompile_attempts.load(std::memory_order_relaxed) < 3) {
                g_recompile_attempts.fetch_add(1, std::memory_order_relaxed);
                if (CompileReplacement(device)) {
                    log::Info("agx_recompiled_after_device_change");
                } else {
                    log::Error("agx_recompile_after_device_change_failed; "
                               "native tone mapper active for this device");
                }
            }
            g_mutex.unlock();
        }
        return original != nullptr
                   ? (device->*original)(shader_bytecode, bytecode_length,
                                         class_linkage, pixel_shader)
                   : E_FAIL;
    }

    __try {
        if (pixel_shader != nullptr &&
            HasExpectedDxbcIdentity(
                static_cast<const std::uint8_t*>(shader_bytecode),
                bytecode_length)) {
            // Hand out a fresh reference to the cached replacement; the
            // game releases its own reference, so each accepted request
            // balances the AddRef here.
            ID3D11PixelShader* replacement = g_replacement_shader.Get();
            replacement->AddRef();
            *pixel_shader = replacement;
            log::Info("agx_substituted final pre-HUD pixel shader");
            return S_OK;
        }
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        // The identity read failed; fall through to the stock path.
    }

    return (device->*original)(shader_bytecode, bytecode_length,
                               class_linkage, pixel_shader);
}

}  // namespace

void SetDeviceSlot(std::uintptr_t device_slot) {
    g_device_slot = device_slot;
}

void Configure(bool enabled,
               int strength_percent,
               int exposure_percent,
               int look,
               int debug_bloom) {
    g_enabled.store(enabled, std::memory_order_release);
    g_strength_percent.store(ClampAgxStrengthPercent(strength_percent),
                             std::memory_order_release);
    g_exposure_percent.store(ClampAgxExposurePercent(exposure_percent),
                             std::memory_order_release);
    g_look.store(ClampAgxLook(look), std::memory_order_release);
    g_debug_bloom.store(ClampAgxDebugBloom(debug_bloom),
                        std::memory_order_release);
}

// SEH-only helper: reads the CreatePixelShader vtable entry without any
// C++ object unwinding so MaybeInstallHooks can keep its ComPtr owner.
void* ReadCreatePixelShaderTarget(ID3D11Device* device) noexcept {
    __try {
        void* const* vtable = *reinterpret_cast<void** const*>(device);
        return vtable[kCreatePixelShaderSlot];
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return nullptr;
    }
}

void MaybeInstallHooks() {
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_hook_installed) {
        return;
    }

    // Hold a COM reference for the complete discovery/compile transaction so
    // a device teardown cannot free the vtable between the read and the
    // MinHook install (same retention discipline as SMAA).
    ComPtr<ID3D11Device> device_owner;
    device_owner.Attach(RetainSlotValue<ID3D11Device>(g_device_slot));
    ID3D11Device* const device = device_owner.Get();
    if (device == nullptr) {
        return;
    }

    void* const target = ReadCreatePixelShaderTarget(device);
    if (target == nullptr) {
        return;
    }

    hook_guard::Guard target_guard;
    hook_guard::Result guard_result;
    if (!target_guard.InitializeForAddress(
            reinterpret_cast<const void*>(target), &guard_result)) {
        log::ErrorF("agx_hook_target_rejected reason=owner_module status=%s "
                    "pe_status=%s win32_error=%u target=0x%p",
                    hook_guard::StatusName(guard_result.status),
                    hook_guard::PeStatusName(guard_result.pe_status),
                    guard_result.win32_error,
                    reinterpret_cast<const void*>(target));
        return;
    }
    const hook_guard::Result verify_result = target_guard.Verify(
        reinterpret_cast<const void*>(target),
        hook_guard::kMinHookTargetVerificationBytes);
    if (!verify_result.verified()) {
        log::ErrorF("agx_hook_target_rejected reason=unexpected_bytes status=%s "
                    "rva=0x%08X mismatch_offset=%zu win32_error=%u target=0x%p",
                    hook_guard::StatusName(verify_result.status),
                    verify_result.target_rva,
                    verify_result.mismatch_offset,
                    verify_result.win32_error,
                    reinterpret_cast<const void*>(target));
        return;
    }

    // Compile once, immediately before arming the detour. A compile or
    // driver failure leaves the native shader completely untouched.
    if (!g_compile_attempted) {
        g_compile_attempted = true;
        if (!CompileReplacement(device)) {
            log::Error("agx_inactive (compile failed - falling back to engine tonemap)");
            return;
        }
        g_shader_ready = true;
    }
    if (!g_shader_ready) {
        return;
    }

    // The bundled MinHook fork installs the detour immediately from
    // MH_CreateHook and exposes no disable-only operation; the trampoline
    // is retained for process lifetime (SmaaRuntime.cpp doctrine). A failed
    // create below is never retried from the per-frame path: without the
    // original pointer a pass-through detour would still be safe, but a
    // half-armed state is harder to reason about than an inactive one.
    const MH_STATUS status = MH_CreateHook(
        reinterpret_cast<void*>(target),
        reinterpret_cast<void*>(&DetourCreatePixelShader),
        reinterpret_cast<void**>(&g_original_create_pixel_shader));
    if (status != MH_OK) {
        log::ErrorF("agx_hook_create_fail status=%d target=0x%p",
                    static_cast<int>(status),
                    reinterpret_cast<const void*>(target));
        g_shader_ready = false;
        return;
    }

    g_hook_installed = true;
    log::InfoF("agx_hook_installed target=0x%p identity_size=%zu identity_crc=0x%08X",
               reinterpret_cast<const void*>(target),
               kFinalPreHudPixelShaderSize,
               kFinalPreHudPixelShaderHash);
}

}  // namespace spatch::agx
