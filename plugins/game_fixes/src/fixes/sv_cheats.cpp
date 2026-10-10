// sv_cheats — turning sv_cheats off leaves anyone in noclip flying. Every such pawn goes back to
// MOVETYPE_WALK the moment the cvar flips to 0.
//
// The fork watched the cvar through ICvar's *global* change callback; s2sdk exposes the same
// callback as HookConVarChange (it fires from ICvar::CallGlobalChangeCallbacks), so the port uses
// it with the cvar's handle. Pawns are moved with s2sdk's SetClientMoveType, which is the
// engine's own CBaseEntity::SetMoveType (field write + network update, exactly the pair of writes
// the fork did by hand).

#include "../fixes.hpp"

#include <pisex/compat.hpp>

#include <cstdlib>

namespace game_fixes {
namespace {

constexpr std::int32_t kMoveTypeNoclip = 7; // MoveType_t::MOVETYPE_NOCLIP
constexpr std::int32_t kMoveTypeWalk = 2;   // MoveType_t::MOVETYPE_WALK

class CSvCheatsFix final : public CFix {
public:
	explicit CSvCheatsFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "sv_cheats"; }

	bool Load() override;
	void Unload() override;

private:
	static void OnConVarChanged(uint64_t conVar, const plg::string& newValue, const plg::string& oldValue);
	void OnSvCheatsDisabled();

	static CSvCheatsFix* s_instance;
	uint64_t m_svCheats = 0;
	bool m_hooked = false;
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
		if (text.empty() || text == "false") {
			fix->OnSvCheatsDisabled();
			return;
		}
		char* end = nullptr;
		const double value = std::strtod(text.data(), &end);
		if (end != text.data() && value == 0.0) {
			fix->OnSvCheatsDisabled();
		}
	} catch (...) {
	}
}

void CSvCheatsFix::OnSvCheatsDisabled() {
	if (!__s2sdk_GetMaxClients || !__s2sdk_GetClientMoveType || !__s2sdk_SetClientMoveType) {
		return;
	}
	// Cvar changes also happen during shutdown (the engine resets cvars at map end); client
	// queries then hit the emptied client list and crash inside s2sdk.
	if (!pisex::IsGameReady()) {
		return;
	}
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
		if (moveType != kMoveTypeNoclip) {
			continue;
		}
		try {
			s2sdk::SetClientMoveType(slot, static_cast<s2sdk::MoveType>(kMoveTypeWalk));
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
