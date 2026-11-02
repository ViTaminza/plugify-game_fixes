// sv_cheats — the pure part: how a convar value string is read as "off", and which pawns need to
// be taken out of noclip.
//
// The engine hands the callback the new value as text. The fork read it with V_atoi, i.e. the
// leading number decides ("0", "0.0", "false" and an empty value are all off). Kept engine-free
// so tests/core_logic_tests.cpp can pin both decisions.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace game_fixes::svcheats {

constexpr std::int32_t kMoveTypeNoclip = 7; // MoveType_t::MOVETYPE_NOCLIP
constexpr std::int32_t kMoveTypeWalk = 2;	// MoveType_t::MOVETYPE_WALK

// The new value of sv_cheats means "off".
inline bool IsOffValue(std::string_view text) {
	if (text.size() == 5) {
		// "false", in any case (bool.TryParse accepts any case).
		static constexpr char kFalse[] = "false";
		bool same = true;
		for (std::size_t i = 0; i < text.size(); ++i) {
			const char c = (text[i] >= 'A' && text[i] <= 'Z') ? static_cast<char>(text[i] - 'A' + 'a') : text[i];
			same = same && c == kFalse[i];
		}
		if (same) {
			return true;
		}
	}
	if (text.empty()) {
		return true;
	}
	// strtod needs a NUL-terminated buffer; string_view does not guarantee one.
	const std::string copy(text);
	char* end = nullptr;
	const double value = std::strtod(copy.c_str(), &end);
	if (end == copy.c_str() || value != 0.0) {
		return false;
	}
	return true;
}

// A pawn is stuck in noclip when either the move type the game runs or the actual move type the
// movement code last applied says NOCLIP (the reference and the fork check both).
constexpr bool NeedsReset(std::int32_t moveType, std::int32_t actualMoveType) {
	return moveType == kMoveTypeNoclip || actualMoveType == kMoveTypeNoclip;
}

} // namespace game_fixes::svcheats
