// game_fixes — the map readiness gate of PITFALLS B1/B2, engine-free so the tests can drive every
// transition (tests/core_logic_tests.cpp).
//
// s2sdk dereferences a null client list, entity system or schema serializer map before the first
// map and in the window between OnMapEnd and the next OnMapStart (PITFALLS B2: a client query there
// reads an emptied client vector and segfaults; PITFALLS C8: a schema lookup walks a null
// serializer map). Every client, entity and schema call of this plugin therefore sits behind
// IsMapReady() (this gate), exactly as docs/PLUGIN_STANDARD.md §7 requires:
//
//   * OnPluginStart  -> Reset() + SetListenersRegistered(): a fresh start (including `plg reload`)
//     begins with the gate shut. Nothing is touched before the first map.
//   * OnMapStart     -> OnMapStart(): the map exists, but readiness is not latched here. The gate
//     opens on the *first OnPluginUpdate after OnMapStart* (§7 item 2), so no callback ordering
//     can slip a client call into the map-start window.
//   * OnPluginUpdate -> OnPluginUpdate(mapAvailable): latches the gate open. A plugin loaded (or
//     reloaded) into a running map never saw OnMapStart, so its first update opens the gate too -
//     but only when the map listeners are registered (without them nothing could ever close the
//     gate again), no OnMapEnd was seen since OnPluginStart, and a map name actually exists
//     (`mapAvailable`, so a plugin that starts before the very first map cannot open the gate on a
//     server that has no map yet). Returns true on the transition, so the caller can run the
//     once-per-map work.
//   * OnMapEnd       -> OnMapEnd(): only drops state (PITFALLS B2 - no client call may follow it).
//   * OnPluginEnd    -> Close(): the library is going away, a stale callback must not reach a
//     client.
//
// Gate() is the process-wide instance the plugin drives; IsMapReady() is what the fixes call in
// place of pisexcompat's old IsGameReady().

#pragma once

namespace game_fixes {

class MapGate {
public:
	// Whether s2sdk client/entity/schema calls are safe right now.
	[[nodiscard]] bool Ready() const { return m_ready; }

	// Whether an OnMapEnd arrived since OnPluginStart and no OnMapStart has followed it.
	[[nodiscard]] bool BetweenMaps() const { return m_betweenMaps; }

	// Whether the OnMapStart/OnMapEnd listeners were registered successfully.
	[[nodiscard]] bool ListenersRegistered() const { return m_listeners; }

	// OnPluginStart: a fresh start (including `plg reload`) begins with the gate shut.
	void Reset() { *this = MapGate(); }

	void SetListenersRegistered(bool registered) { m_listeners = registered; }

	// OnMapStart listener: the map exists; OnPluginUpdate() latches readiness on the first frame.
	void OnMapStart() {
		m_ready = false;
		m_betweenMaps = false;
	}

	// OnMapEnd listener: from here until the next OnMapStart every s2sdk client query can crash,
	// so this only clears state - it must not touch a client or an entity (PITFALLS B2).
	void OnMapEnd() {
		m_ready = false;
		m_betweenMaps = true;
	}

	// OnPluginUpdate: the latch described above. Returns true when the gate just opened.
	bool OnPluginUpdate(bool mapAvailable) {
		if (m_ready || m_betweenMaps || !m_listeners || !mapAvailable) {
			return false;
		}
		m_ready = true;
		return true;
	}

	// OnPluginEnd: nothing of this plugin may answer a callback after the library is unloaded.
	void Close() { m_ready = false; }

private:
	bool m_ready = false;
	bool m_betweenMaps = false;
	bool m_listeners = false;
};

// The plugin's process-wide gate (src/map_gate.cpp); driven only by src/plugin.cpp.
MapGate& Gate();

// True while a map is up: the one guard every client, entity and schema call checks.
bool IsMapReady();

} // namespace game_fixes
