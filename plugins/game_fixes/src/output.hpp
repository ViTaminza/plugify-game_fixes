// game_fixes — the plugin's one chat output helper (replaces pisex::PrintToChatAll).
//
// slow_animation announces the reload in chat ("Карта ... идёт уже ... ч"). The message is a
// literal string formatted by the fix itself; nothing here goes through a phrases/localization API
// and no language is ever set, so the server's language is never switched by this plugin.
//
// Rules kept from the pisexcompat wrapper it replaces:
//   * nothing is printed while the map gate is shut - s2sdk's print helpers walk client state,
//     which is null before the first map and torn down between OnMapEnd and the next OnMapStart;
//   * a missing s2sdk binding is a no-op (fail soft; the log line still records the message);
//   * an exception from a plg::string allocation must not escape into the game thread.

#pragma once

#include <string_view>

#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include "map_gate.hpp"

namespace game_fixes {

inline void PrintToChatAll(std::string_view message) {
	if (message.empty() || !IsMapReady() || __s2sdk_PrintToChatAll == nullptr) {
		return;
	}
	try {
		s2sdk::PrintToChatAll(plg::string(message));
	} catch (...) {
	}
}

} // namespace game_fixes
