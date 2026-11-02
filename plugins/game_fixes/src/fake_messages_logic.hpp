// fake_messages — the pure part: what counts as chat, and when a client may chat.
//
// Advertising bots connect just long enough to post a message without ever joining the game. The
// reference lets chat through only once the client is in game: PlayerData.InGame is set from
// signonState >= SIGNONSTATE_SPAWN and by OnClientPutInServer.
//
// Kept engine-free so tests/core_logic_tests.cpp can pin the name matching and the signon rule.

#pragma once

#include <cstdint>
#include <string_view>

namespace game_fixes::fakemessages {

// SignonState_t values used here (s2sdk enums.hpp:426).
constexpr std::int32_t kSignonSpawn = 5;

constexpr char ToLowerAscii(char c) {
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (std::size_t i = 0; i < left.size(); ++i) {
		if (ToLowerAscii(left[i]) != ToLowerAscii(right[i])) {
			return false;
		}
	}
	return true;
}

// Only the two chat commands are blocked; every other client command is untouched.
inline bool IsChatCommand(std::string_view command) {
	return EqualsIgnoreCase(command, "say") || EqualsIgnoreCase(command, "say_team");
}

// A client may chat once its signon state has reached SPAWN (the reference's IsPlayerPutInServer).
constexpr bool SignonAllowsChat(std::int32_t signonState) {
	return signonState >= kSignonSpawn;
}

} // namespace game_fixes::fakemessages
