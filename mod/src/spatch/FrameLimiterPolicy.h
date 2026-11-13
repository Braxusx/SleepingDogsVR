#pragma once

// Custom frame limiter policy (R6, 2026-10-12).
//
// A user-requested cap at an arbitrary frame rate, paced the way Rivatuner
// (RTSS) paces frames: the present call itself is delayed so each frame is
// displayed on an even schedule, instead of relying on the game's own
// coarse 0/1/2/4 limiter presets or on vertical sync. The scheduling math
// lives here as pure functions so unit tests can pin the behavior without
// a D3D device; the runtime half (atomic enable/fps state plus the wait in
// the Present detour) lives beside the SMAA present hook, which is the one
// code path in this mod that already sees every real frame.
//
// Scheduling model (RTSS-style):
//   * frame N+1 is targeted at previous + interval, never at "now" - so a
//     frame that finishes early waits, and frame times stay even;
//   * the long part of the wait sleeps on a high-resolution waitable timer,
//     the last ~2 ms spin on QueryPerformanceCounter for sub-millisecond
//     precision - same hybrid RTSS uses;
//   * when the game cannot keep the pace (now is a full interval past the
//     target) the schedule resyncs instead of accumulating debt, so a single
//     stall does not turn into a burst of zero-wait frames.

#include <cmath>
#include <limits>

namespace spatch::frame_limiter {

// The accepted INI range. 0 (or anything below 30) disables the limiter so a
// typo cannot silently pin the game to an unplayable rate; values above 1000
// exceed what the pacing can meaningfully hold.
inline constexpr float kMinFps = 30.0f;
inline constexpr float kMaxFps = 1000.0f;

// The final slice of each wait is spent spinning: timers wake with tens of
// microseconds of jitter, and the spin converts that into sub-millisecond
// evenness. 2 ms matches RTSS's compromise between precision and CPU burn.
inline constexpr double kSpinTailSeconds = 0.002;

namespace detail {

// constexpr replacements for std::isfinite (C++17 has no constexpr cmath):
// a float is finite when it equals itself (rules out NaN) and is not one of
// the two infinities (which compare false against their own negation).
[[nodiscard]] constexpr bool IsFiniteFloat(float value) noexcept {
    return value == value && value != std::numeric_limits<float>::infinity() &&
           value != -std::numeric_limits<float>::infinity();
}

}  // namespace detail

[[nodiscard]] constexpr float ClampCustomFrameLimiterFps(float fps) noexcept {
    if (!detail::IsFiniteFloat(fps) || fps < kMinFps) {
        return 0.0f;
    }
    if (fps > kMaxFps) {
        return kMaxFps;
    }
    return fps;
}

[[nodiscard]] constexpr bool FrameLimiterActive(bool enabled,
                                                float fps) noexcept {
    return enabled && fps > 0.0f;
}

[[nodiscard]] constexpr double FrameLimiterIntervalSeconds(
    float fps) noexcept {
    if (fps <= 0.0f) {
        return 0.0;
    }
    return 1.0 / static_cast<double>(fps);
}

struct FrameLimiterPlan {
    // How long the present should wait before submitting, in seconds. 0
    // means submit immediately.
    double wait_seconds = 0.0;
    // The previous-frame timestamp the NEXT plan must build on.
    double next_previous = 0.0;
    // False on the first observed frame (nothing to pace against yet) and
    // on resync frames that submit immediately.
    bool paced = false;
};

// Pure scheduler: given the present's timestamp, the previous frame's
// schedule anchor, and the interval, decide the wait and the next anchor.
[[nodiscard]] constexpr FrameLimiterPlan PlanFrame(
    double now_seconds,
    double previous_seconds,
    bool have_previous,
    double interval_seconds) noexcept {
    FrameLimiterPlan plan{};
    if (!(interval_seconds > 0.0) ||
        !detail::IsFiniteFloat(static_cast<float>(now_seconds)) ||
        !detail::IsFiniteFloat(static_cast<float>(previous_seconds))) {
        plan.next_previous = now_seconds;
        return plan;
    }
    if (!have_previous) {
        // First frame: pass through, anchor the schedule at it.
        plan.next_previous = now_seconds;
        return plan;
    }
    const double target = previous_seconds + interval_seconds;
    if (target <= now_seconds) {
        // At or past the target: submit now. Resync only when we fell a full
        // interval behind, so one slow frame does not accumulate debt while
        // a trivially early one stays on schedule.
        plan.next_previous =
            (now_seconds - previous_seconds > interval_seconds) ? now_seconds
                                                                : target;
        return plan;
    }
    const double remaining = target - now_seconds;
    plan.wait_seconds =
        remaining > kSpinTailSeconds ? remaining - kSpinTailSeconds : 0.0;
    plan.next_previous = target;
    plan.paced = true;
    return plan;
}

}  // namespace spatch::frame_limiter
