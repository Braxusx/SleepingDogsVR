#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace spatch::character_surface {

struct ScalarSample {
    float value = 0.0f;
    bool present = false;
    bool finite = false;
};

struct Snapshot {
    ScalarSample component_wetness;
    ScalarSample component_sweat;
    ScalarSample staged_wetness;
    ScalarSample staged_sweat;
    // These are raw slots in the pre-bridge render block. Shader
    // disassembly proves that +0x38 is not a wetness input and +0x3C is an
    // unrelated material parameter; neither is causal surface telemetry.
    ScalarSample prebridge_slot_38;
    ScalarSample prebridge_slot_3c;

    [[nodiscard]] bool complete() const noexcept {
        return component_wetness.present && component_sweat.present &&
               staged_wetness.present && staged_sweat.present;
    }

    [[nodiscard]] bool all_finite() const noexcept {
        return complete() && component_wetness.finite && component_sweat.finite &&
               staged_wetness.finite && staged_sweat.finite;
    }

    [[nodiscard]] bool prebridge_complete() const noexcept {
        return prebridge_slot_38.present && prebridge_slot_3c.present;
    }

    [[nodiscard]] bool prebridge_all_finite() const noexcept {
        return prebridge_complete() && prebridge_slot_38.finite &&
               prebridge_slot_3c.finite;
    }
};

inline ScalarSample ReadScalar(const void* bytes,
                               std::size_t size,
                               std::size_t offset) noexcept {
    ScalarSample result{};
    if (bytes == nullptr || offset > size || sizeof(float) > size - offset) {
        return result;
    }

    std::memcpy(&result.value,
                static_cast<const std::uint8_t*>(bytes) + offset,
                sizeof(result.value));
    result.present = true;
    result.finite = std::isfinite(result.value);
    return result;
}

inline Snapshot CaptureSnapshot(const void* character_look,
                                std::size_t character_look_size,
                                const void* render_block,
                                std::size_t render_block_size,
                                const void* prebridge_block,
                                std::size_t prebridge_size) noexcept {
    Snapshot result{};
    result.component_wetness = ReadScalar(character_look, character_look_size, 0xB8);
    result.component_sweat = ReadScalar(character_look, character_look_size, 0xBC);
    result.staged_wetness = ReadScalar(render_block, render_block_size, 0x24);
    result.staged_sweat = ReadScalar(render_block, render_block_size, 0x28);
    result.prebridge_slot_38 =
        ReadScalar(prebridge_block, prebridge_size, 0x38);
    result.prebridge_slot_3c =
        ReadScalar(prebridge_block, prebridge_size, 0x3C);
    return result;
}

[[nodiscard]] inline bool IsNewMatchingWaterContact(
    std::uint64_t current_generation,
    std::uint64_t observed_generation,
    std::uintptr_t collision_component,
    std::uintptr_t sampled_component) noexcept {
    return current_generation != observed_generation && collision_component != 0 &&
           collision_component == sampled_component;
}

}  // namespace spatch::character_surface
