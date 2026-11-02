// sv_cheats — turning sv_cheats off leaves anyone in noclip flying. Every such pawn goes back to
// MOVETYPE_WALK the moment the cvar flips to 0.
//
// The fork watched the cvar through ICvar's *global* change callback; s2sdk exposes the same
// callback as HookConVarChange (it fires from ICvar::CallGlobalChangeCallbacks), so the port uses
// it with the cvar's handle.
//
// Both move-type fields are handled, exactly like the reference SvCheatsFix and the C++ fork
// (ref-gf/GameFixes-base/src/fixes/sv_cheats.cpp:75-83): a pawn is reset when m_MoveType OR
// m_nActualMoveType is NOCLIP. m_MoveType goes through s2sdk's SetClientMoveType — the engine's
// own CBaseEntity::SetMoveType, which is also what networks the change (m_MoveType is the
// networked one). m_nActualMoveType is 1 byte and networked:false (sdk.json offset 1500, size 1),
// so it is written raw through the schema offset with changeState=false; there is no state to
// network. The offset is resolved lazily on the first sv_cheats-off while a map runs, never at
// plugin start (GetSchemaOffset before a map exists segfaults).

#include "../fixes.hpp"
#include "../map_gate.hpp"
#include "../sv_cheats_logic.hpp"

namespace game_fixes {
namespace {

using svcheats::kMoveTypeWalk;

class CSvCheatsFix final : public CFix {
public:
	explicit CSvCheatsFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "sv_cheats"; }

	bool Load() override;
	void Unload() override;
	void OnStartupServer(const char* mapName) override;

private:
	static void OnConVarChanged(uint64_t conVar, const plg::string& newValue, const plg::string& oldValue);
	void OnSvCheatsDisabled();
	void ResolveActualMoveTypeOffset();

	static CSvCheatsFix* s_instance;
	uint64_t m_svCheats = 0;
	bool m_hooked = false;
	// CBaseEntity::m_nActualMoveType (1 byte, networked:false). Resolved once while a map runs.
	std::int32_t m_offActualMoveType = -1;
	bool m_actualResolveTried = false;
	bool m_loggedActualFailure = false;
};

CSvCheatsFix* CSvCheatsFix::s_instance = nullptr;

void CSvCheatsFix::OnConVarChanged(uint64_t, const plg::string& newValue, const plg::string&) {
	try {
		auto* fix = s_instance;
		if (!fix) {
			return;
		}
		// The callback carries the engine's new value string; the fork read it with V_atoi, i.e.
		// "0"/"0.0"/"false" and a leading digit's numeric value decide.
		const std::string_view text(newValue.c_str(), newValue.size());
		if (svcheats::IsOffValue(text)) {
			fix->OnSvCheatsDisabled();
		}
	} catch (...) {
	}
}

void CSvCheatsFix::ResolveActualMoveTypeOffset() {
	if (m_actualResolveTried || !__s2sdk_GetSchemaOffset) {
		return;
	}
	m_actualResolveTried = true;
	m_offActualMoveType = s2sdk::GetSchemaOffset(plg::string("CBaseEntity"), plg::string("m_nActualMoveType"));
	if (m_offActualMoveType < 0 && !m_loggedActualFailure) {
		m_loggedActualFailure = true;
		LogFix(Name(), "CBaseEntity::m_nActualMoveType not in the schema; only m_MoveType can be reset",
			   plg::Severity::Warning);
	}
}

void CSvCheatsFix::OnSvCheatsDisabled() {
	if (!__s2sdk_GetMaxClients || !__s2sdk_GetClientMoveType || !__s2sdk_SetClientMoveType) {
		return;
	}
	// Cvar changes also happen during shutdown (the engine resets cvars at map end); client
	// queries then hit the emptied client list and crash inside s2sdk.
	if (!IsMapReady()) {
		return;
	}
	ResolveActualMoveTypeOffset();

	const int maxClients = s2sdk::GetMaxClients();
	int fixed = 0;
	for (int slot = 0; slot < maxClients; ++slot) {
		// PlayerSlotToClientPtr is the one silent, Count-checked client lookup s2sdk has; the
		// move-type calls and IsClientInGame go through utils::GetClientBySlot, which does an
		// unchecked Element(slot) and dies on an emptied list. After this check both are safe for
		// the slot, and IsClientInGame keeps a client without a pawn (still connecting) from
		// reaching helpers::GetController2, which would warn about the missing pawn.
		if (!__s2sdk_PlayerSlotToClientPtr || !s2sdk::PlayerSlotToClientPtr(slot)) {
			continue;
		}
		if (__s2sdk_IsClientInGame && !s2sdk::IsClientInGame(slot)) {
			continue;
		}
		const auto moveType = static_cast<std::int32_t>(s2sdk::GetClientMoveType(slot));

		// The actual move type is what the movement code last applied; if only it says NOCLIP the
		// pawn is still stuck. Read it through the pawn's handle (a handle-based read, and the
		// handle is validated first).
		std::int32_t actualMoveType = moveType;
		int handle = -1;
		if (m_offActualMoveType >= 0 && __s2sdk_GetClientPawn && __s2sdk_EntPointerToEntHandle &&
			__s2sdk_GetEntData) {
			if (void* pawn = s2sdk::GetClientPawn(slot)) {
				handle = s2sdk::EntPointerToEntHandle(pawn);
				if (IsValidHandle(handle)) {
					actualMoveType =
						static_cast<std::int32_t>(s2sdk::GetEntData(handle, m_offActualMoveType, 1));
				}
			}
		}

		if (!svcheats::NeedsReset(moveType, actualMoveType)) {
			continue;
		}
		try {
			// The engine setter also performs the networked move-type update (m_MoveType is the
			// networked field).
			s2sdk::SetClientMoveType(slot, static_cast<s2sdk::MoveType>(kMoveTypeWalk));
			// m_nActualMoveType is not networked (sdk.json), so the raw write needs no state change;
			// it is written explicitly because the engine setter is not documented to touch it.
			if (m_offActualMoveType >= 0 && IsValidHandle(handle) && __s2sdk_SetEntData) {
				s2sdk::SetEntData(handle, m_offActualMoveType, kMoveTypeWalk, 1, false, -1);
			}
			++fixed;
		} catch (...) {
		}
	}
	if (fixed) {
		LogFix(Name(), Format("sv_cheats went off, %d pawn(s) taken out of noclip", fixed));
	}
}

bool CSvCheatsFix::Load() {
	if (!__s2sdk_FindConVar || !__s2sdk_HookConVarChange) {
		LogFix(Name(), "convar bindings are missing", plg::Severity::Warning);
		return false;
	}
	m_svCheats = s2sdk::FindConVar("sv_cheats");
	if (m_svCheats == 0) {
		LogFix(Name(), "sv_cheats not found", plg::Severity::Warning);
		return false;
	}
	s_instance = this;
	try {
		s2sdk::HookConVarChange(m_svCheats, &CSvCheatsFix::OnConVarChanged);
		m_hooked = true;
	} catch (...) {
		m_hooked = false;
	}
	if (!m_hooked) {
		s_instance = nullptr;
		LogFix(Name(), "hooking sv_cheats changes failed", plg::Severity::Warning);
		return false;
	}
	LogFix(Name(), "watching sv_cheats");
	return true;
}

void CSvCheatsFix::OnStartupServer(const char*) {
	// A map start is the next chance to resolve the actual-move-type offset (and the failure log
	// is once per map, not once per process).
	if (m_offActualMoveType < 0) {
		m_actualResolveTried = false;
		m_loggedActualFailure = false;
	}
}

void CSvCheatsFix::Unload() {
	if (m_hooked && __s2sdk_UnhookConVarChange) {
		s2sdk::UnhookConVarChange(m_svCheats, &CSvCheatsFix::OnConVarChanged);
	}
	m_hooked = false;
	s_instance = nullptr;
}

} // namespace

std::unique_ptr<CFix> MakeSvCheatsFix(Environment& env) {
	return std::make_unique<CSvCheatsFix>(env);
}

} // namespace game_fixes
