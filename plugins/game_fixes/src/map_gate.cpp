#include "map_gate.hpp"

namespace game_fixes {
namespace {

MapGate g_gate;

} // namespace

MapGate& Gate() {
	return g_gate;
}

bool IsMapReady() {
	return g_gate.Ready();
}

} // namespace game_fixes
