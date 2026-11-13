#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace spatch::gtav_speed_shake {

// Shipped GTA V third-person vehicle-camera metadata. Oscillator phases are
// expressed in cycles and frequencies in hertz.
inline constexpr float kMinimumForwardSpeedMetersPerSecond = 25.0f;
inline constexpr float kMaximumForwardSpeedMetersPerSecond = 45.0f;
inline constexpr float kStockStrength = 1.0f;
inline constexpr float kDefaultStrength = 1.85f;
inline constexpr float kMinimumStrength = 0.0f;
inline constexpr float kMaximumStrength = 3.0f;
inline constexpr float kSpeedResponseSpringConstant = 10.0f;
inline constexpr float kAttackDurationSeconds = 1.0f;
inline constexpr float kSustainEnvelope = 0.225f;

// Hook-facing values are expressed in the same units as the policy. Invalid
// thresholds fall back as a pair so the interpolation span can never collapse
// or reverse. Finite strength values are clamped to the supported range.
struct Tuning {
    float strength = kDefaultStrength;
    float start_speed_mps = kMinimumForwardSpeedMetersPerSecond;
    float full_speed_mps = kMaximumForwardSpeedMetersPerSecond;

    constexpr bool operator==(const Tuning&) const = default;
};

inline constexpr Tuning kDefaultTuning{};
inline constexpr Tuning kStockTuning{
    kStockStrength,
    kMinimumForwardSpeedMetersPerSecond,
    kMaximumForwardSpeedMetersPerSecond,
};

inline constexpr bool kHasRelativeYTranslation = false;
inline constexpr bool kHasYawRotation = false;
inline constexpr bool kHasFovEffect = false;
inline constexpr bool kHasMotionBlurEffect = false;
inline constexpr bool kHasControllerRumble = false;

struct SineOscillator {
    float amplitude = 0.0f;
    float frequency_hz = 0.0f;
    float phase_cycles = 0.0f;

    constexpr bool operator==(const SineOscillator&) const = default;
};

inline constexpr std::array<SineOscillator, 3> kRelativeXOscillators{{
    {0.04f, 1.2444f, 0.0f},
    {0.07f, 0.8712f, 0.0f},
    {0.03f, 2.0568f, 0.0f},
}};

inline constexpr std::array<SineOscillator, 6> kRelativeZOscillators{{
    {0.10f, 0.9768f, 0.75f},
    {0.15f, 0.528f, 0.75f},
    {0.07f, 1.2f, 0.75f},
    {0.03f, 6.76f, 0.0f},
    {0.02f, 3.36f, 0.0f},
    {0.02f, 4.63968f, 0.0f},
}};

inline constexpr std::array<SineOscillator, 2> kRotationXOscillators{{
    {0.01f, 2.111f, 0.0f},
    {0.01f, 1.333f, 0.0f},
}};

inline constexpr std::array<SineOscillator, 1> kRotationYOscillators{{
    {0.00666f, 3.111f, 0.0f},
}};

template <std::size_t Count>
[[nodiscard]] constexpr float SumAmplitudes(
    const std::array<SineOscillator, Count>& oscillators) noexcept {
    float sum = 0.0f;
    for (const SineOscillator& oscillator : oscillators) {
        sum += oscillator.amplitude;
    }
    return sum;
}

inline constexpr float kMaximumRelativeXMeters =
    SumAmplitudes(kRelativeXOscillators) * kSustainEnvelope;
inline constexpr float kMaximumRelativeZMeters =
    SumAmplitudes(kRelativeZOscillators) * kSustainEnvelope;
inline constexpr float kMaximumRotationXRadians =
    SumAmplitudes(kRotationXOscillators) * kSustainEnvelope;
inline constexpr float kMaximumRotationYRadians =
    SumAmplitudes(kRotationYOscillators) * kSustainEnvelope;
inline constexpr float kMaximumSupportedRelativeXMeters =
    kMaximumRelativeXMeters * kMaximumStrength;
inline constexpr float kMaximumSupportedRelativeZMeters =
    kMaximumRelativeZMeters * kMaximumStrength;
inline constexpr float kMaximumSupportedRotationXRadians =
    kMaximumRotationXRadians * kMaximumStrength;
inline constexpr float kMaximumSupportedRotationYRadians =
    kMaximumRotationYRadians * kMaximumStrength;

struct Offsets {
    float relative_x_meters = 0.0f;
    float relative_z_meters = 0.0f;
    float rotation_x_radians = 0.0f;
    float rotation_y_radians = 0.0f;

    constexpr bool operator==(const Offsets&) const = default;
};

// This state is intentionally independent of engine objects. Hooks can own one
// instance per active camera/subject and sample it repeatedly without advancing
// it from nested desired-pose calls.
struct State {
    double phase_time_seconds = 0.0;
    float attack_elapsed_seconds = 0.0f;
    float target_speed_factor = 0.0f;
    float speed_factor = 0.0f;
    bool attack_started = false;

    constexpr bool operator==(const State&) const = default;
};

struct Sample {
    Offsets offsets{};
    float target_speed_factor = 0.0f;
    float speed_factor = 0.0f;
    float envelope = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr bool operator==(const Vec3&) const = default;
};

struct Pose {
    Vec3 eye{};
    Vec3 look{};
    Vec3 up{};

    constexpr bool operator==(const Pose&) const = default;
};

inline constexpr double kPi =
    3.141592653589793238462643383279502884;
inline constexpr double kTwoPi = 2.0 * kPi;

[[nodiscard]] inline float ClampUnitFinite(float value) noexcept {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, 0.0f, 1.0f);
}

[[nodiscard]] inline Tuning NormalizeTuning(Tuning tuning) noexcept {
    Tuning effective = kDefaultTuning;
    if (std::isfinite(tuning.strength)) {
        effective.strength = std::clamp(
            tuning.strength, kMinimumStrength, kMaximumStrength);
    }

    if (std::isfinite(tuning.start_speed_mps) &&
        std::isfinite(tuning.full_speed_mps) &&
        tuning.start_speed_mps >= 0.0f &&
        tuning.full_speed_mps > tuning.start_speed_mps) {
        effective.start_speed_mps = tuning.start_speed_mps;
        effective.full_speed_mps = tuning.full_speed_mps;
    }
    return effective;
}

[[nodiscard]] inline Offsets MaximumChannelOffsets(
    Tuning tuning = kDefaultTuning) noexcept {
    const float strength = NormalizeTuning(tuning).strength;
    return Offsets{
        kMaximumRelativeXMeters * strength,
        kMaximumRelativeZMeters * strength,
        kMaximumRotationXRadians * strength,
        kMaximumRotationYRadians * strength,
    };
}

[[nodiscard]] inline float SlowInOut(float normalized_value) noexcept {
    const float clamped = ClampUnitFinite(normalized_value);
    if (clamped <= 0.0f) {
        return 0.0f;
    }
    if (clamped >= 1.0f) {
        return 1.0f;
    }

    const double eased =
        0.5 * (1.0 - std::cos(kPi * static_cast<double>(clamped)));
    if (!std::isfinite(eased)) {
        return 0.0f;
    }
    return std::clamp(static_cast<float>(eased), 0.0f, 1.0f);
}

[[nodiscard]] inline float ResolveSpeedTarget(
    float signed_forward_speed_meters_per_second,
    Tuning tuning = kDefaultTuning) noexcept {
    if (!std::isfinite(signed_forward_speed_meters_per_second)) {
        return 0.0f;
    }
    const Tuning effective = NormalizeTuning(tuning);
    if (signed_forward_speed_meters_per_second <=
        effective.start_speed_mps) {
        return 0.0f;
    }
    if (signed_forward_speed_meters_per_second >=
        effective.full_speed_mps) {
        return 1.0f;
    }

    const float normalized =
        (signed_forward_speed_meters_per_second -
         effective.start_speed_mps) /
        (effective.full_speed_mps - effective.start_speed_mps);
    return SlowInOut(normalized);
}

[[nodiscard]] inline float StepSpeedResponse(float previous_factor,
                                             float target_factor,
                                             float delta_seconds) noexcept {
    const float previous = ClampUnitFinite(previous_factor);
    const float target = ClampUnitFinite(target_factor);
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0f) {
        return previous;
    }

    const double retention = std::exp(
        -std::sqrt(static_cast<double>(kSpeedResponseSpringConstant)) *
        static_cast<double>(delta_seconds));
    const double next =
        static_cast<double>(target) +
        (static_cast<double>(previous) - static_cast<double>(target)) *
            retention;
    if (!std::isfinite(next)) {
        return target;
    }
    return std::clamp(static_cast<float>(next), 0.0f, 1.0f);
}

[[nodiscard]] inline float ResolveAttackEnvelope(
    float attack_elapsed_seconds) noexcept {
    if (!std::isfinite(attack_elapsed_seconds) ||
        attack_elapsed_seconds <= 0.0f) {
        return 0.0f;
    }
    if (attack_elapsed_seconds >= kAttackDurationSeconds) {
        return kSustainEnvelope;
    }
    return kSustainEnvelope *
           (attack_elapsed_seconds / kAttackDurationSeconds);
}

template <std::size_t Count>
[[nodiscard]] inline double SampleOscillators(
    const std::array<SineOscillator, Count>& oscillators,
    double phase_time_seconds) noexcept {
    if (!std::isfinite(phase_time_seconds) || phase_time_seconds < 0.0) {
        phase_time_seconds = 0.0;
    }

    double sum = 0.0;
    for (const SineOscillator& oscillator : oscillators) {
        const double cycles =
            static_cast<double>(oscillator.frequency_hz) *
                phase_time_seconds +
            static_cast<double>(oscillator.phase_cycles);
        if (!std::isfinite(cycles)) {
            return 0.0;
        }
        const double bounded_cycles = std::remainder(cycles, 1.0);
        const double contribution =
            static_cast<double>(oscillator.amplitude) *
            std::sin(kTwoPi * bounded_cycles);
        if (!std::isfinite(contribution)) {
            return 0.0;
        }
        sum += contribution;
    }
    return std::isfinite(sum) ? sum : 0.0;
}

[[nodiscard]] inline float ClampOffset(double value,
                                       float absolute_limit) noexcept {
    if (!std::isfinite(value) || !std::isfinite(absolute_limit) ||
        absolute_limit <= 0.0f) {
        return 0.0f;
    }
    return static_cast<float>(std::clamp(
        value,
        -static_cast<double>(absolute_limit),
        static_cast<double>(absolute_limit)));
}

[[nodiscard]] inline float ScaleStockChannel(float stock_value,
                                             float strength,
                                             float stock_limit) noexcept {
    if (!std::isfinite(stock_value) || !std::isfinite(strength) ||
        !std::isfinite(stock_limit) || strength <= 0.0f ||
        stock_limit <= 0.0f) {
        return 0.0f;
    }
    if (strength == kStockStrength) {
        // Keep the 100% path bit-for-bit equivalent to the original profile
        // calculation: no additional floating-point operation is performed.
        return stock_value;
    }
    return ClampOffset(
        static_cast<double>(stock_value) * static_cast<double>(strength),
        stock_limit * strength);
}

[[nodiscard]] inline Offsets SampleProfile(double phase_time_seconds,
                                           float speed_factor,
                                           float envelope,
                                           Tuning tuning =
                                               kDefaultTuning) noexcept {
    const Tuning effective = NormalizeTuning(tuning);
    const double stock_scale =
        static_cast<double>(ClampUnitFinite(speed_factor)) *
        static_cast<double>(std::clamp(
            std::isfinite(envelope) ? envelope : 0.0f,
            0.0f,
            kSustainEnvelope));
    if (!(stock_scale > 0.0) || effective.strength <= 0.0f) {
        // Phase lives in State and advances independently. Quiet frames need
        // no twelve-oscillator trigonometric sample.
        return {};
    }

    const float stock_relative_x = ClampOffset(
        SampleOscillators(kRelativeXOscillators, phase_time_seconds) *
            stock_scale,
        kMaximumRelativeXMeters);
    const float stock_relative_z = ClampOffset(
        SampleOscillators(kRelativeZOscillators, phase_time_seconds) *
            stock_scale,
        kMaximumRelativeZMeters);
    const float stock_rotation_x = ClampOffset(
        SampleOscillators(kRotationXOscillators, phase_time_seconds) *
            stock_scale,
        kMaximumRotationXRadians);
    const float stock_rotation_y = ClampOffset(
        SampleOscillators(kRotationYOscillators, phase_time_seconds) *
            stock_scale,
        kMaximumRotationYRadians);

    return Offsets{
        ScaleStockChannel(stock_relative_x,
                          effective.strength,
                          kMaximumRelativeXMeters),
        ScaleStockChannel(stock_relative_z,
                          effective.strength,
                          kMaximumRelativeZMeters),
        ScaleStockChannel(stock_rotation_x,
                          effective.strength,
                          kMaximumRotationXRadians),
        ScaleStockChannel(stock_rotation_y,
                          effective.strength,
                          kMaximumRotationYRadians),
    };
}

[[nodiscard]] inline bool IsFinite(const Vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

[[nodiscard]] inline Vec3 Add(const Vec3& left, const Vec3& right) noexcept {
    return Vec3{left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] inline Vec3 Subtract(const Vec3& left,
                                   const Vec3& right) noexcept {
    return Vec3{left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] inline Vec3 Scale(const Vec3& value, float scale) noexcept {
    return Vec3{value.x * scale, value.y * scale, value.z * scale};
}

[[nodiscard]] inline double Dot(const Vec3& left, const Vec3& right) noexcept {
    return static_cast<double>(left.x) * static_cast<double>(right.x) +
           static_cast<double>(left.y) * static_cast<double>(right.y) +
           static_cast<double>(left.z) * static_cast<double>(right.z);
}

[[nodiscard]] inline Vec3 Cross(const Vec3& left, const Vec3& right) noexcept {
    return Vec3{
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

[[nodiscard]] inline bool TryNormalize(const Vec3& value,
                                       Vec3& normalized,
                                       float* length_out = nullptr) noexcept {
    if (!IsFinite(value)) {
        return false;
    }
    const double length_squared = Dot(value, value);
    if (!std::isfinite(length_squared) || length_squared <= 0.00000001) {
        return false;
    }
    const double length = std::sqrt(length_squared);
    if (!std::isfinite(length) || length <= 0.0001 ||
        length > static_cast<double>(std::numeric_limits<float>::max())) {
        return false;
    }
    const float inverse_length = static_cast<float>(1.0 / length);
    const Vec3 candidate = Scale(value, inverse_length);
    if (!IsFinite(candidate)) {
        return false;
    }
    normalized = candidate;
    if (length_out != nullptr) {
        *length_out = static_cast<float>(length);
    }
    return true;
}

[[nodiscard]] inline Vec3 RotateAroundUnitAxis(const Vec3& value,
                                               const Vec3& unit_axis,
                                               float angle_radians) noexcept {
    const double angle = static_cast<double>(angle_radians);
    const float cosine = static_cast<float>(std::cos(angle));
    const float sine = static_cast<float>(std::sin(angle));
    const float projection = static_cast<float>(Dot(unit_axis, value));
    return Add(
        Add(Scale(value, cosine), Scale(Cross(unit_axis, value), sine)),
        Scale(unit_axis, projection * (1.0f - cosine)));
}

[[nodiscard]] inline bool IsEffectivelyZero(const Offsets& offsets) noexcept {
    constexpr float kEpsilon = 0.00000001f;
    return std::abs(offsets.relative_x_meters) <= kEpsilon &&
           std::abs(offsets.relative_z_meters) <= kEpsilon &&
           std::abs(offsets.rotation_x_radians) <= kEpsilon &&
           std::abs(offsets.rotation_y_radians) <= kEpsilon;
}

// Translates eye and look together along the camera's orthonormal right/up
// axes, then pitches around local right and rolls around the post-pitch forward
// axis. No yaw is introduced, and eye-to-look distance is preserved. The output
// is not changed when the input basis/sample is invalid or effectively zero.
[[nodiscard]] inline bool TryApplyToPose(const Pose& input,
                                         const Sample& sample,
                                         Pose& output) noexcept {
    const Offsets& offsets = sample.offsets;
    if (!IsFinite(input.eye) || !IsFinite(input.look) || !IsFinite(input.up) ||
        !std::isfinite(offsets.relative_x_meters) ||
        !std::isfinite(offsets.relative_z_meters) ||
        !std::isfinite(offsets.rotation_x_radians) ||
        !std::isfinite(offsets.rotation_y_radians) ||
        IsEffectivelyZero(offsets)) {
        return false;
    }

    Vec3 forward{};
    float eye_to_look_distance = 0.0f;
    if (!TryNormalize(Subtract(input.look, input.eye),
                      forward,
                      &eye_to_look_distance)) {
        return false;
    }

    // Remove any forward component from the supplied up vector before using it
    // for translation or rotation. This preserves the camera's handedness while
    // preventing accumulated non-orthogonality from scaling the shake.
    const Vec3 projected_up = Subtract(
        input.up,
        Scale(forward, static_cast<float>(Dot(input.up, forward))));
    Vec3 up{};
    if (!TryNormalize(projected_up, up)) {
        return false;
    }
    Vec3 right{};
    if (!TryNormalize(Cross(forward, up), right)) {
        return false;
    }
    if (!TryNormalize(Cross(right, forward), up)) {
        return false;
    }

    const Vec3 translation = Add(
        Scale(right, offsets.relative_x_meters),
        Scale(up, offsets.relative_z_meters));
    const Vec3 translated_eye = Add(input.eye, translation);

    Vec3 pitched_forward = RotateAroundUnitAxis(
        forward, right, offsets.rotation_x_radians);
    Vec3 pitched_up = RotateAroundUnitAxis(
        up, right, offsets.rotation_x_radians);
    if (!TryNormalize(pitched_forward, pitched_forward)) {
        return false;
    }
    pitched_up = Subtract(
        pitched_up,
        Scale(pitched_forward,
              static_cast<float>(Dot(pitched_up, pitched_forward))));
    if (!TryNormalize(pitched_up, pitched_up)) {
        return false;
    }

    Vec3 rolled_up = RotateAroundUnitAxis(
        pitched_up, pitched_forward, offsets.rotation_y_radians);
    rolled_up = Subtract(
        rolled_up,
        Scale(pitched_forward,
              static_cast<float>(Dot(rolled_up, pitched_forward))));
    if (!TryNormalize(rolled_up, rolled_up)) {
        return false;
    }

    const Vec3 translated_look = Add(
        translated_eye,
        Scale(pitched_forward, eye_to_look_distance));
    const Pose candidate{translated_eye, translated_look, rolled_up};
    if (!IsFinite(candidate.eye) || !IsFinite(candidate.look) ||
        !IsFinite(candidate.up)) {
        return false;
    }

    output = candidate;
    return true;
}

[[nodiscard]] inline Sample CurrentSample(
    const State& state,
    Tuning tuning = kDefaultTuning) noexcept {
    const float target = ClampUnitFinite(state.target_speed_factor);
    const float speed = ClampUnitFinite(state.speed_factor);
    const float envelope =
        ResolveAttackEnvelope(state.attack_elapsed_seconds);
    return Sample{
        SampleProfile(state.phase_time_seconds, speed, envelope, tuning),
        target,
        speed,
        envelope,
    };
}

// Returns false without touching state for invalid or nested/non-advancing
// frame deltas. A valid frame always advances phase and attack, including while
// the signed forward speed is below the activation threshold.
[[nodiscard]] inline bool Advance(State& state,
                                  float signed_forward_speed_meters_per_second,
                                  float delta_seconds,
                                  Tuning tuning = kDefaultTuning) noexcept {
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0f) {
        return false;
    }

    State next = state;
    if (!std::isfinite(next.phase_time_seconds) ||
        next.phase_time_seconds < 0.0) {
        next.phase_time_seconds = 0.0;
    }
    next.attack_elapsed_seconds = std::clamp(
        std::isfinite(next.attack_elapsed_seconds)
            ? next.attack_elapsed_seconds
            : 0.0f,
        0.0f,
        kAttackDurationSeconds);
    next.speed_factor = ClampUnitFinite(next.speed_factor);

    next.phase_time_seconds += static_cast<double>(delta_seconds);
    if (!std::isfinite(next.phase_time_seconds)) {
        next.phase_time_seconds = 0.0;
    }

    if (next.attack_started) {
        const double attack_elapsed =
            static_cast<double>(next.attack_elapsed_seconds) +
            static_cast<double>(delta_seconds);
        next.attack_elapsed_seconds = static_cast<float>(std::clamp(
            attack_elapsed,
            0.0,
            static_cast<double>(kAttackDurationSeconds)));
    } else {
        // GTA creates the shaker during the follow-camera update. Its first
        // same-frame envelope sample begins attack at elapsed time zero, even
        // though oscillator phase and the speed spring have advanced.
        next.attack_started = true;
    }
    next.target_speed_factor = ResolveSpeedTarget(
        signed_forward_speed_meters_per_second,
        tuning);
    next.speed_factor = StepSpeedResponse(
        next.speed_factor,
        next.target_speed_factor,
        delta_seconds);

    state = next;
    return true;
}

inline void Reset(State& state) noexcept {
    state = {};
}

[[nodiscard]] constexpr bool ShouldPreserveRuntimeStateWithoutAdvance(
    unsigned int update_depth,
    float delta_seconds) noexcept {
    return update_depth != 1 || !(delta_seconds > 0.0f);
}

}  // namespace spatch::gtav_speed_shake
