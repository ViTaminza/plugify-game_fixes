// game_fixes — the map-gated schema-offset cache (PITFALLS C8).
//
// s2sdk's GetSchemaOffset builds its field list from the network serializer map, which does not
// exist before the first map is up and is torn down between OnMapEnd and the next OnMapStart.
// Calling it in either window segfaults inside CollectSchemaFields (the real crash was game_fixes
// calling it from OnPluginStart). The plugin no longer goes through pisexcompat, so the cache
// pisex::SchemaOffset used to provide lives here:
//
//   * with the map gate shut the cache answers -1 without consulting its own entries and without
//     calling s2sdk at all - neither a new lookup nor a remembered offset is handed out in a window
//     where the caller must not use it;
//   * only a *successful* lookup (offset >= 0) is remembered. GetSchemaOffset answers -1 for an
//     unknown field, and caching that -1 would hide the field for the whole session - a field that
//     appears later (or a lookup that ran a frame too early) is retried on the next call instead of
//     being poisoned;
//   * Clear() is called from OnPluginEnd, so `plg reload` resolves the fields again instead of
//     reusing offsets a previous instance happened to have found.
//
// The lookup is injected as a plain function pointer, so the tests drive the whole policy (gate
// first, failures retried, successes memoised, Clear() forgets) without an engine and without the
// s2sdk bindings.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace game_fixes {

class SchemaOffsetCache {
public:
	// The engine-side lookup: s2sdk::GetSchemaOffset, or -1 for "unknown".
	using OffsetLookup = std::int32_t (*)(const char* className, const char* fieldName);

	// >= 0 offset of className::fieldName, or -1 when no map is up / the field is unknown / the
	// caller passed an empty name. A cached hit never calls `lookup`.
	std::int32_t Offset(bool mapReady, const char* className, const char* fieldName, OffsetLookup lookup) {
		if (className == nullptr || fieldName == nullptr || *className == '\0' || *fieldName == '\0' ||
			lookup == nullptr) {
			return -1;
		}
		if (!mapReady) {
			// GetSchemaOffset dereferences the null serializer map before the first map and
			// between OnMapEnd and the next OnMapStart (PITFALLS B1/B2, C8). Even a remembered
			// offset is not handed out here: the caller uses -1 as "no schema read in this
			// window".
			return -1;
		}
		std::string key(className);
		key.push_back('\0');
		key.append(fieldName);
		if (const auto cached = m_offsets.find(key); cached != m_offsets.end()) {
			return cached->second;
		}
		const std::int32_t offset = lookup(className, fieldName);
		if (offset >= 0) {
			m_offsets.emplace(std::move(key), offset); // -1 stays retryable
		}
		return offset;
	}

	// Forgets every offset; the plugin calls this in OnPluginEnd (a reload re-resolves them).
	void Clear() { m_offsets.clear(); }

	// Bookkeeping for the tests.
	std::size_t CachedCount() const { return m_offsets.size(); }

private:
	std::unordered_map<std::string, std::int32_t> m_offsets; // "class\0field" -> offset >= 0
};

} // namespace game_fixes
