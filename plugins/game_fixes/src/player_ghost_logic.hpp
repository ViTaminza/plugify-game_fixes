// player_ghost — the pure part: which players are "ghosts" at round_freeze_end.
//
// A ghost is an alive player whose *pawn* sits on the spectator team (the controller was moved to
// spectate but the body survived). The reference checks IsAlive && PlayerPawn != null &&
// PlayerPawn.Team == Spectator; nothing else takes part in the decision.
//
// Kept engine-free so tests/core_logic_tests.cpp can pin it.

#pragma once

#include <cstdint>

namespace game_fixes::playerghost {

// CSTeam values (s2sdk enums.hpp:40).
constexpr std::int32_t kTeamNone = 0;
constexpr std::int32_t kTeamSpectator = 1;

constexpr bool IsGhostCandidate(bool inGame, bool alive, bool pawnValid, std::int32_t pawnTeam) {
	return inGame && alive && pawnValid && pawnTeam == kTeamSpectator;
}

} // namespace game_fixes::playergenost
