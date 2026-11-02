// jump_spam — the pure part: one jump per tick.
//
// A keybind macro makes the game run CheckJumpButton several times in one tick (subtick moves),
// each one counting as a jump. The reference cancels a jump-button check when the player already
// jumped on that move tick and records the tick after a successful OnJump.
//
// Kept engine-free so tests/core_logic_tests.cpp can pin the comparisons (the -1 initial value is
// deliberate: tick 0 is a real tick, so the first jump must not compare equal to it).

#pragma once

#include <cstdint>

namespace game_fixes::jumpspam {

constexpr std::int32_t kNoJumpTick = -1;

constexpr bool ShouldCancelJump(std::int32_t lastJumpTick, std::int32_t moveTick) {
	return lastJumpTick == moveTick;
}

constexpr std::int32_t RecordJump(std::int32_t moveTick) {
	return moveTick;
}

} // namespace game_fixes::jumpspam
