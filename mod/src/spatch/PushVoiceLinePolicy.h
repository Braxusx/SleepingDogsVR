#pragma once

#include <algorithm>
#include <cstdint>

namespace spatch::push_voice_line {

// The restored push voice line reuses the game's own conversation dispatch,
// so the only new behavior is deciding WHEN the line may play. Ambient
// crowds keep pushing each other without the player, so a bare "someone was
// pushed" event would fire constantly. The gate keeps the restored line
// rare enough to feel intentional: a short cooldown between lines, a
// per-minute budget, and silence while the player is in combat (Wei has
// dedicated combat lines there).
inline constexpr double kCooldownSeconds = 1.5;
inline constexpr std::uint32_t kMaximumLinesPerMinute = 15;
inline constexpr std::uint32_t kHistoryLength = 15;
inline constexpr std::uint64_t kCooldownMs =
    static_cast<std::uint64_t>(kCooldownSeconds * 1000.0);
inline constexpr std::uint64_t kBudgetWindowMs = 60'000;

struct Decision {
    bool fire = false;
    bool suppressed_by_cooldown = false;
    bool suppressed_by_budget = false;
    bool suppressed_by_combat = false;
};

class PushVoiceLineGate {
public:
    // now_ms: monotonic milliseconds (GetTickCount64). in_combat: live
    // player combat state; false when unknown. A decision that fires
    // consumes one budget slot and starts a new cooldown.
    Decision Evaluate(std::uint64_t now_ms, bool in_combat,
                      std::uint64_t cooldown_ms = kCooldownMs) {
        Decision decision;
        if (in_combat) {
            decision.suppressed_by_combat = true;
            return decision;
        }
        if (last_fire_ms_ != 0 || count_ != 0) {
            if (now_ms < last_fire_ms_ ||
                now_ms - last_fire_ms_ < cooldown_ms) {
                decision.suppressed_by_cooldown = true;
                return decision;
            }
        }
        DropExpiredEntries(now_ms);
        if (count_ >= kMaximumLinesPerMinute) {
            decision.suppressed_by_budget = true;
            return decision;
        }
        for (std::uint32_t index = 0; index < kHistoryLength; ++index) {
            if (!history_used_[index]) {
                history_ms_[index] = now_ms;
                history_used_[index] = true;
                break;
            }
        }
        ++count_;
        last_fire_ms_ = now_ms;
        decision.fire = true;
        return decision;
    }

    void Reset() {
        last_fire_ms_ = 0;
        count_ = 0;
        std::fill(history_ms_, history_ms_ + kHistoryLength, 0);
        std::fill(history_used_, history_used_ + kHistoryLength, false);
    }

    [[nodiscard]] std::uint32_t recent_count() const { return count_; }

private:
    void DropExpiredEntries(std::uint64_t now_ms) {
        count_ = 0;
        for (std::uint32_t index = 0; index < kHistoryLength; ++index) {
            if (!history_used_[index]) {
                continue;
            }
            const std::uint64_t timestamp = history_ms_[index];
            if (now_ms >= timestamp &&
                now_ms - timestamp >= kBudgetWindowMs) {
                history_ms_[index] = 0;
                history_used_[index] = false;
                continue;
            }
            ++count_;
        }
    }

    std::uint64_t last_fire_ms_ = 0;
    std::uint32_t count_ = 0;
    std::uint64_t history_ms_[kHistoryLength]{};
    bool history_used_[kHistoryLength]{};
};

}  // namespace spatch::push_voice_line
