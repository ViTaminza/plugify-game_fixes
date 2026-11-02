// team_limit — the pure part: which CCSGameRules fields the fix raises and to what.
//
// Kept engine-free so tests/core_logic_tests.cpp can pin the field list. All six fields are
// 4-byte ints and non-networked (sdk.json: m_iNumSpawnable* 3476/3480, m_iSpawnPointCount_*
// 3716/3720, m_iMaxNum* 3724/3728), so they are written with SetEntData2(..., size 4,
// changeState=false): a network state change on a non-networked field is impossible and s2sdk
// would only log "cannot be send over network" every round start.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace game_fixes::teamlimit {

struct FieldWrite {
	const char* field;
	std::int32_t value;
};

// The six counters the reference TeamLimitFix and the fork raise to maxplayers. The two spawn
// point counts are what the original fork omitted.
inline constexpr std::size_t kFieldCount = 6;

inline constexpr std::array<FieldWrite, kFieldCount> Writes(std::int32_t maxPlayers) {
	return {{{"m_iNumSpawnableTerrorist", maxPlayers},
			 {"m_iNumSpawnableCT", maxPlayers},
			 {"m_iSpawnPointCount_Terrorist", maxPlayers},
			 {"m_iSpawnPointCount_CT", maxPlayers},
			 {"m_iMaxNumTerrorists", maxPlayers},
			 {"m_iMaxNumCTs", maxPlayers}}};
}

// Width of every field above (sdk.json size 4). Kept next to the names so a write can never use
// the wrong width.
inline constexpr std::int32_t kFieldSize = 4;

} // namespace game_fixes::teamlimit
