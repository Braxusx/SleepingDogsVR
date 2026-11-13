#pragma once

#include <algorithm>
#include <cmath>

namespace spatch::character_sweat {

// The engine's sweat value is a normalized material amount.  The policy is
// deliberately time-based so its behavior does not change with the render
// cadence or with a temporary 30 Hz engine update.
inline constexpr float kMaximumStepSeconds = 0.25f;
inline constexpr float kMinimumVisibleAmount = 0.001f;
inline constexpr float kDefaultBuildSeconds = 150.0f;
inline constexpr float kDefaultFadeSeconds = 120.0f;
inline constexpr float kDefaultOnsetSeconds = 30.0f;
inline constexpr float kDefaultRunSpeed = 2.5f;

struct Timing {
    float build_seconds = kDefaultBuildSeconds;
    float fade_seconds = kDefaultFadeSeconds;
    float onset_seconds = kDefaultOnsetSeconds;
};

struct State {
    float amount = 0.0f;
    float exertion_seconds = 0.0f;
};

struct StepResult {
    float amount = 0.0f;
    bool active = false;
};

struct ExertionDecision {
    bool velocity_candidate = false;
    bool running_exertion = false;
    bool combat_exertion = false;
    bool exerting = false;
};

inline float ClampAmount(float amount) noexcept {
    if (!std::isfinite(amount)) {
        return 0.0f;
    }
    return std::clamp(amount, 0.0f, 1.0f);
}

inline float SanitizeDelta(float delta_seconds) noexcept {
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0f) {
        return 0.0f;
    }
    return std::min(delta_seconds, kMaximumStepSeconds);
}

inline float SanitizeDuration(float seconds) noexcept {
    if (!std::isfinite(seconds) || seconds <= 0.0f) {
        return 0.0f;
    }
    return seconds;
}

inline StepResult Advance(State& state,
                          float delta_seconds,
                          bool exerting,
                          const Timing& timing = {}) noexcept {
    const float delta = SanitizeDelta(delta_seconds);
    const float build_seconds = SanitizeDuration(timing.build_seconds);
    const float fade_seconds = SanitizeDuration(timing.fade_seconds);
    const float onset_seconds = SanitizeDuration(timing.onset_seconds);
    state.amount = ClampAmount(state.amount);
    if (!std::isfinite(state.exertion_seconds) || state.exertion_seconds < 0.0f) {
        state.exertion_seconds = 0.0f;
    }

    if (delta > 0.0f) {
        if (exerting) {
            const float previous_exertion = state.exertion_seconds;
            state.exertion_seconds =
                std::min(onset_seconds, previous_exertion + delta);
            const float onset_remainder =
                std::max(0.0f, onset_seconds - previous_exertion);
            const float accumulation_delta = std::max(0.0f, delta - onset_remainder);
            if (accumulation_delta > 0.0f) {
                state.amount = build_seconds > 0.0f
                                   ? std::min(1.0f,
                                              state.amount + accumulation_delta / build_seconds)
                                   : 1.0f;
            }
        } else {
            state.exertion_seconds = 0.0f;
            state.amount = fade_seconds > 0.0f
                               ? std::max(0.0f, state.amount - delta / fade_seconds)
                               : 0.0f;
        }
    }

    // Do not discard the first small accumulation step at high refresh rates;
    // doing so would make the value unable to grow above zero at 125 Hz and
    // faster.  The visibility cutoff is only a dry-down terminal condition.
    if (!exerting && state.amount < kMinimumVisibleAmount) {
        state.amount = 0.0f;
    }
    return StepResult{state.amount, state.amount > 0.0f};
}

inline bool IsRunning(float horizontal_speed,
                      float threshold = kDefaultRunSpeed) noexcept {
    return std::isfinite(horizontal_speed) && std::isfinite(threshold) &&
           horizontal_speed > 0.0f &&
           horizontal_speed >= std::max(0.0f, threshold);
}

inline bool CanSafelyDecayOwnedAmount(bool owns_amount,
                                      float current_amount,
                                      float last_written_amount,
                                      float tolerance = 0.002f) noexcept {
    return owns_amount && std::isfinite(current_amount) &&
           std::isfinite(last_written_amount) && std::isfinite(tolerance) &&
           tolerance >= 0.0f &&
           std::fabs(current_amount - last_written_amount) <= tolerance;
}

inline ExertionDecision EvaluateExertion(bool velocity_valid,
                                         float horizontal_speed,
                                         float run_speed_threshold,
                                         bool vehicle_state_valid,
                                         bool in_vehicle,
                                         bool combat_valid,
                                         bool in_combat) noexcept {
    ExertionDecision result{};
    result.velocity_candidate =
        velocity_valid && IsRunning(horizontal_speed, run_speed_threshold);
    result.running_exertion = result.velocity_candidate &&
                              vehicle_state_valid && !in_vehicle;
    result.combat_exertion = combat_valid && in_combat;
    result.exerting = result.running_exertion || result.combat_exertion;
    return result;
}

inline bool IsExertionDecisionKnown(bool running_decision_valid,
                                    bool combat_decision_valid,
                                    const ExertionDecision& decision) noexcept {
    // OR is known true as soon as either independently valid term is true. It
    // is known false only when both terms are independently valid and false.
    return decision.running_exertion || decision.combat_exertion ||
           (running_decision_valid && combat_decision_valid);
}

}  // namespace spatch::character_sweat
