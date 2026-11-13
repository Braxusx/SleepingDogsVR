#pragma once

#include <cstdint>

namespace spatch::wei_voice_line {

// Which restored voice line (if any) a single HealthApplyDamage event calls
// for. The families are the round69 v1 catalog: combat_pre_posture on
// combat entry, combat_taunt_cold / combat_taunt_angry while dealing damage
// during combat, hit_apology on player-dealt light hits outside combat.
enum class Action {
  None,
  CombatPrePosture,
  CombatTauntCold,
  CombatTauntAngry,
  HitApology,
};

// HEURISTIC (labeled honestly): the apology threshold is not derived from
// engine code. The game exposes no "accidental strike" event, so a light
// player-dealt hit outside combat is approximated by its damage value. The
// round68 probe sessions logged no damage values for zero-damage bumps, and
// the HealthApplyDamage detour sees damage as an integer, so 5 (the smallest
// plausible non-trivial hit) is the v1 default. Expect tuning after a live
// session (G6); this constant is the single knob.
inline constexpr int kHitApologyMaxDamage = 5;

// TAUNT PACING (tuned 2026-08-29 on user feedback, first live session):
// taunts arrived every 2-6 s in sustained combat because they ride the shared
// 1.5 s Wei gate. The user asked for them "a tiny bit less often" in active
// combat, so taunts get their own minimum gap ON TOP of the shared gate; it
// is checked BEFORE the gate so a gapped taunt consumes no gate slot, and
// apologies / entry lines are unaffected.
inline constexpr std::uint64_t kTauntCooldownMs = 4'000;

// True when a taunt may play now. last_taunt_ms == 0 means no taunt has fired
// yet (GetTickCount64 is monotonic and effectively never 0).
inline bool TauntReady(std::uint64_t now_ms, std::uint64_t last_taunt_ms) {
  return last_taunt_ms == 0 ||
         now_ms - last_taunt_ms >= kTauntCooldownMs;
}

// Pure trigger classifier for the restored Wei voice lines.
//
// Inputs:
//   combat_entry    - the player's combat state flipped from not-in-combat to
//                     in-combat on this event (observed at damage events only;
//                     the delay is documented in the design note).
//   combat_known    - the live combat-state read succeeded. When it did not,
//                     neither taunts nor apologies may fire: an unknown state
//                     is NOT treated as "outside combat" here, because a
//                     mid-fight light hit would then masquerade as an apology.
//   in_combat       - the player's live combat state (meaningful only when
//                     combat_known).
//   player_attacker - the damaged character was struck by the player.
//   damage          - the damage value carried by the event (0 for bumps).
//   taunt_counter   - alternates the cold/angry taunt pick; the caller
//                     advances it only when a taunt actually plays.
//
// Priority on a single event: combat entry wins over taunts (the entry line
// and a taunt must not both play on the same hit), taunts win over apologies
// (combat damage never apologizes). Entry lines do not require
// player_attacker: the event only reveals that the player's own combat state
// flipped, and any damage event after the flip may carry that news.
inline Action SelectAction(bool combat_entry, bool combat_known, bool in_combat,
                           bool player_attacker, int damage,
                           std::uint32_t taunt_counter) {
  if (combat_known && in_combat) {
    if (combat_entry) {
      return Action::CombatPrePosture;
    }
    if (player_attacker && damage > 0) {
      return (taunt_counter % 2u) == 0u ? Action::CombatTauntCold
                                        : Action::CombatTauntAngry;
    }
    return Action::None;
  }
  if (combat_known && player_attacker && damage > 0 &&
      damage <= kHitApologyMaxDamage) {
    return Action::HitApology;
  }
  return Action::None;
}

} // namespace spatch::wei_voice_line
