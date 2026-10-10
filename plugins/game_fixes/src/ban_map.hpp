// The GC ban map (CCSGameRules::sm_mapGcBanInformation) as the steam_ban fix treats it.
//
// The container is a CUtlOrderedMap<uint32, CGcBanInformation_t> the engine owns; this file holds
// the only operations the fix performs on it, kept free of s2sdk so tests/core_logic_tests.cpp
// can drive them against a synthetic map. Everything is plain memory access at the offsets the
// fork's own build uses (see assets/gamedata.jsonc and README.md).
//
// Layout (verified against the fork's compiled handlers and cross-checked with the sibling
// gamebanfix plugin, which maintains the same map):
//
//   map + 0x08  int32  node slot count (the sweep bound the engine's own RemoveAll uses)
//   map + 0x10  ptr    node array
//   map + 0x18  int32  RB root index (-1 = empty)
//   map + 0x1C  int32  element count
//   map + 0x20  int32  free-list head (-1 = none)
//   node stride 0x38; node + 0x00 int32 m_Left (own index while free)
//                        + 0x04 int32 m_Right (next free node while free)
//                        + 0x10 uint32 key (Steam account id)
//                        + 0x18 uint32 reason (CGcBanInformation_t::m_uiReason)

#pragma once

#include <cstdint>
#include <cstring>
#include <unordered_set>

namespace game_fixes::banmap {

struct Layout {
	std::int32_t array = -1;
	std::int32_t allocated = -1;
	std::int32_t root = -1;
	std::int32_t count = -1;
	std::int32_t firstFree = -1;
	std::int32_t lastAlloc = -1;
	std::int32_t nodeStride = -1;
	std::int32_t nodeKey = -1;
	std::int32_t nodeReason = -1;

	[[nodiscard]] bool Valid() const {
		return array >= 0 && allocated >= 0 && root >= 0 && count >= 0 && firstFree >= 0 && lastAlloc >= 0 &&
			   nodeStride > 0 && nodeKey >= 0 && nodeReason >= 0;
	}
};

// An entry the pass can never match: the account id is the 32-bit invalid handle and the reason
// is not a real one. Written over both fields, because the pass reads both.
constexpr std::uint32_t kNeutral = 0xFFFFFFFFu;

// The competitive-cooldown reasons the pass acts on while sv_kick_players_with_cooldown < 2.
inline constexpr std::uint32_t kCooldownReasons[] = {20u, 22u, 23u};

// Reads a 32-bit field of the map header.
inline std::int32_t Int32At(const void* map, std::int32_t offset) {
	std::int32_t value = 0;
	std::memcpy(&value, static_cast<const std::uint8_t*>(map) + offset, sizeof(value));
	return value;
}

inline void SetInt32At(void* map, std::int32_t offset, std::int32_t value) {
	std::memcpy(static_cast<std::uint8_t*>(map) + offset, &value, sizeof(value));
}

// The map element count (what the pass tests before doing anything).
inline std::int32_t Count(void* map, const Layout& layout) {
	if (!map || layout.count < 0) {
		return 0;
	}
	return Int32At(map, layout.count);
}

inline std::uint8_t* NodeAt(void* map, const Layout& layout, std::int32_t index) {
	const void* nodes = nullptr;
	std::memcpy(&nodes, static_cast<const std::uint8_t*>(map) + layout.array, sizeof(nodes));
	if (!nodes) {
		return nullptr;
	}
	return static_cast<std::uint8_t*>(const_cast<void*>(nodes)) + static_cast<std::ptrdiff_t>(index) * layout.nodeStride;
}

inline std::uint32_t U32At(const std::uint8_t* base, std::int32_t offset) {
	std::uint32_t value = 0;
	std::memcpy(&value, base + offset, sizeof(value));
	return value;
}

inline void SetU32At(std::uint8_t* base, std::int32_t offset, std::uint32_t value) {
	std::memcpy(base + offset, &value, sizeof(value));
}

// Neutralizes the whitelisted accounts and, when the cvar is below 2, the competitive-cooldown
// entries: the pass matches on the key and acts on the reason, so both are overwritten and the
// entry can no longer kick anybody. The tree itself is never touched (the fork removed entries
// with RemoveAt; neutralizing keeps the structure consistent no matter what). Returns the number
// of entries neutralized.
inline int Neutralize(void* map, const Layout& layout, const std::unordered_set<std::uint32_t>& whitelist,
					  bool dropCooldowns) {
	if (!map || !layout.Valid()) {
		return 0;
	}
	// An empty tree has nothing the pass could act on - it reads this very count before it walks
	// anything, and after RemoveAll the map stays empty until a new ban arrives. The hook runs
	// every frame, so skip the per-slot hashing of stale keys in that state (the slots keep their
	// old keys after RemoveAll, they are just no longer in the tree).
	if (Int32At(map, layout.count) <= 0) {
		return 0;
	}
	const std::int32_t allocated = Int32At(map, layout.allocated);
	if (allocated <= 0) {
		return 0;
	}
	int stripped = 0;
	for (std::int32_t i = 0; i < allocated; ++i) {
		std::uint8_t* node = NodeAt(map, layout, i);
		if (!node) {
			return stripped;
		}
		const std::uint32_t key = U32At(node, layout.nodeKey);
		const std::uint32_t reason = U32At(node, layout.nodeReason);
		if (key == kNeutral && reason == kNeutral) {
			continue; // already neutralized
		}
		bool match = whitelist.contains(key);
		if (!match && dropCooldowns) {
			for (const std::uint32_t cooldown : kCooldownReasons) {
				if (reason == cooldown) {
					match = true;
					break;
				}
			}
		}
		if (!match) {
			continue;
		}
		SetU32At(node, layout.nodeKey, kNeutral);
		SetU32At(node, layout.nodeReason, kNeutral);
		++stripped;
	}
	return stripped;
}

// The engine's RemoveAll as the fork's compiled code runs it: every slot is pushed onto the free
// list, then m_Root = -1 and m_NumElements = 0 in one 8-byte store (the register's top half is
// zero, so the count lands as 0, not -1).
inline void RemoveAll(void* map, const Layout& layout) {
	if (!map || !layout.Valid()) {
		return;
	}
	const std::int32_t allocated = Int32At(map, layout.allocated);
	if (allocated <= 0) {
		return;
	}
	// The list is rebuilt from scratch: starting from the map's current head would chain the new
	// links onto the old ones and turn the list into a cycle (the old head's slot has just been
	// relinked too).
	std::int32_t firstFree = -1;
	for (std::int32_t i = 0; i < allocated; ++i) {
		std::uint8_t* node = NodeAt(map, layout, i);
		if (!node) {
			return;
		}
		// Every slot is relinked, unconditionally: skipping "already free-looking" slots (m_Left
		// == index, which is also what a never-used slot holds in slot 0) would leave that slot
		// pointing at itself and turn the list into a cycle.
		SetU32At(node, 4, static_cast<std::uint32_t>(firstFree)); // m_Right = the previous head
		SetU32At(node, 0, static_cast<std::uint32_t>(i));		  // m_Left = own index (free marker)
		firstFree = i;
	}
	SetInt32At(map, layout.firstFree, firstFree);
	// m_Root = -1 and m_NumElements = 0 (the engine writes them as one 8-byte store; the fields
	// are adjacent in its layout, and writing them apart lands the same bytes).
	SetInt32At(map, layout.root, -1);
	SetInt32At(map, layout.count, 0);
}

} // namespace game_fixes::banmap
