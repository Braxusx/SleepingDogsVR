#pragma once

#include <array>
#include <cstdint>
#include <cstring>

namespace spatch::onfoot_camera {

inline constexpr std::uintptr_t kControllerLookActionsRva = 0x023E43C8;
inline constexpr std::size_t kControllerActionActiveOffset = 0x4C;
inline constexpr std::uintptr_t OrbitOptionCallRva(bool latest) noexcept {
    return latest ? 0x003DAAE2 : 0x003DAC42;
}
inline constexpr std::uintptr_t OrbitOptionGetterRva(bool latest) noexcept {
    return latest ? 0x004A6860 : 0x004A6720;
}
inline constexpr std::array<std::uint8_t, 16> OrbitOptionSignature(bool latest) noexcept {
    return {0xE8, static_cast<std::uint8_t>(latest ? 0x79 : 0xD9),
            static_cast<std::uint8_t>(latest ? 0xBD : 0xBA), 0x0C, 0x00,
            0x84, 0xC0, 0x74, 0x7D, 0x80, 0xBF, 0xA8, 0x21, 0, 0, 0};
}

// Flags at FollowCameraComponent::Update's native mouse-boom option call.
// Keep the original option result, retention selector, and motion selector
// independent. The selector also controls the native eye commit after clamp.
inline constexpr std::uint32_t kOptionEnabled = 1;
inline constexpr std::uint32_t kRetainBoom = 2;
inline constexpr std::uint32_t kRefreshBoom = 4;

[[nodiscard]] constexpr std::uint32_t SelectOrbitFlags(
    std::uint32_t native_flags, bool enabled, bool input_allowed,
    bool controller_action_readable, bool controller_action_active) noexcept {
    if (!enabled || !input_allowed || !controller_action_readable ||
        (native_flags & kRetainBoom) != 0) {
        return native_flags;
    }
    return kOptionEnabled | kRetainBoom |
           (controller_action_active ? kRefreshBoom : 0u);
}

// Call-site adapter, not an ordinary Win64 function: intentionally
// changes R12B/R14B for the enclosing native block. All other nonvolatile
// registers remain untouched. The original GetBool still runs exactly once.
//
// At entry, the native caller's RSP is our RSP+8. Its controller index lives
// at +78h, hence +A8h after our 28h allocation. RBP is the native frame pointer;
// [RBP+98h] is its existing allow-input flag. Callback ABI:
// uint32_t(void* component, uint32_t native_flags, int index, bool allowed).
inline constexpr std::array<std::uint8_t, 96> kOrbitRelayTemplate{
    0x48, 0x83, 0xEC, 0x28,                         // sub rsp,28h
    0x48, 0xB8,                                     // mov rax,GetBool
    0, 0, 0, 0, 0, 0, 0, 0,
    0xFF, 0xD0,                                     // call rax
    0x0F, 0xB6, 0xD0,                               // movzx edx,al
    0x41, 0x0F, 0xB6, 0xC4,                         // movzx eax,r12b
    0xD1, 0xE0, 0x09, 0xC2,                         // shl eax,1; or edx,eax
    0x41, 0x0F, 0xB6, 0xC6,                         // movzx eax,r14b
    0xC1, 0xE0, 0x02, 0x09, 0xC2,                   // shl eax,2; or edx,eax
    0x48, 0x89, 0xF9,                               // mov rcx,rdi
    0x44, 0x8B, 0x84, 0x24, 0xA8, 0, 0, 0,         // mov r8d,[rsp+A8h]
    0x44, 0x0F, 0xB6, 0x8D, 0x98, 0, 0, 0,         // movzx r9d,[rbp+98h]
    0x48, 0xB8,                                     // mov rax,callback
    0, 0, 0, 0, 0, 0, 0, 0,
    0xFF, 0xD0,                                     // call rax
    0x89, 0xC2, 0xD1, 0xEA,                         // mov edx,eax; shr edx,1
    0x80, 0xE2, 0x01, 0x41, 0x88, 0xD4,             // and dl,1; mov r12b,dl
    0x89, 0xC2, 0xC1, 0xEA, 0x02,                   // mov edx,eax; shr edx,2
    0x80, 0xE2, 0x01, 0x41, 0x88, 0xD6,             // and dl,1; mov r14b,dl
    0x83, 0xE0, 0x01,                               // and eax,1
    0x48, 0x83, 0xC4, 0x28, 0xC3,                   // add rsp,28h; ret
};

// UNWIND_INFO: version 1, four-byte prologue, one UWOP_ALLOC_SMALL (28h).
inline constexpr std::array<std::uint8_t, 8> kOrbitRelayUnwind{
    1, 4, 1, 0, 4, 0x42, 0, 0,
};

[[nodiscard]] inline auto BuildOrbitRelay(std::uintptr_t get_bool,
                                          std::uintptr_t callback) noexcept {
    auto bytes = kOrbitRelayTemplate;
    std::memcpy(bytes.data() + 6, &get_bool, sizeof(get_bool));
    std::memcpy(bytes.data() + 57, &callback, sizeof(callback));
    return bytes;
}

}  // namespace spatch::onfoot_camera
