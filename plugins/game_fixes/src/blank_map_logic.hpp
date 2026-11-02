// blank_map — the pure part: when nextlevel_changed needs a restart, and which command to run.
//
// The reference: if the event's nextlevel is empty, restart the current map — `changelevel` for a
// stock map, `ds_workshop_changelevel` for a workshop map (the workshop one is only addressable by
// its id, which is what GlobalsVars.MapName holds there).
//
// Kept engine-free so tests/core_logic_tests.cpp can pin both decisions.

#pragma once

#include <string>
#include <string_view>

namespace game_fixes::blankmap {

// An empty nextlevel is the blank-map state; a map name is required to be able to restart.
inline bool NeedsRestart(std::string_view nextLevel, std::string_view currentMap) {
	return nextLevel.empty() && !currentMap.empty();
}

inline std::string RestartCommand(std::string_view mapName, bool mapIsValid) {
	return (mapIsValid ? std::string("changelevel ") : std::string("ds_workshop_changelevel ")) +
		   std::string(mapName);
}

} // namespace game_fixes::blankmap
