// team_limit — the game caps how many players may join each team from the map's spawn points,
// so a join gets refused as "team full" on maps with few spawns. On every round_start,
// CCSGameRules' spawnable/max T and CT counts are raised to maxplayers.
//
// The fork hooked the game's event manager (round_start) and cached the rules object per map;
// this port uses s2sdk's round_start hook and s2sdk's g_pGameRules directly (s2sdk already
// tracks the cs_gamerules entity). The four fields are written through the schema with
// changeState=false: the schema lists all four as "networked": false (CCSGameRules
// m_iNumSpawnableTerrorist 0xD94, m_iNumSpawnableCT 0xD98, m_iMaxNumTerrorists 0xE8C,
// m_iMaxNumCTs 0xE90), so changeState=true could only produce s2sdk's "cannot be send over
// network" warning every round start. The fork's NetworkStateChanged calls were no-ops for these
// fields for the same reason; the join-limit check reads the value directly, so the cap raise is
// unchanged.

#include "../fixes.hpp"

namespace game_fixes {
namespace {

struct Field {
	const char* name;
};

constexpr Field kFields[] = {
	{"m_iNumSpawnableTerrorist"},
	{"m_iNumSpawnableCT"},
	{"m_iMaxNumTerrorists"},
	{"m_iMaxNumCTs"},
};

class CTeamLimitFix final : public CFix {
public:
	explicit CTeamLimitFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "team_limit"; }

	bool Load() override;
	void Unload() override;

private:
	static s2sdk::ResultType OnRoundStart(const plg::string& name, void* event, bool dontBroadcast);

	static CTeamLimitFix* s_instance;
	bool m_hooked = false;
};

CTeamLimitFix* CTeamLimitFix::s_instance = nullptr;

s2sdk::ResultType CTeamLimitFix::OnRoundStart(const plg::string&, void*, bool) {
	try {
		if (!s_instance || !__s2sdk_GetGameRules || !__s2sdk_GetMaxClients || !__s2sdk_SetEntSchema2) {
			return s2sdk::ResultType::Continue;
		}
		void* rules = s2sdk::GetGameRules();
		if (!rules) {
			return s2sdk::ResultType::Continue;
		}
		const int maxPlayers = s2sdk::GetMaxClients();
		for (const Field& field : kFields) {
			// changeState=false: none of the four fields is networked (see the file header), so a
			// state change is impossible and s2sdk would log "cannot be send over network" every
			// round start.
			s2sdk::SetEntSchema2(rules, "CCSGameRules", field.name, maxPlayers, false, 0); // element 0: not an array (entity-schemas guide)
		}
	} catch (...) {
	}
	return s2sdk::ResultType::Continue;
}

bool CTeamLimitFix::Load() {
	if (!__s2sdk_HookEvent || !__s2sdk_GetGameRules || !__s2sdk_SetEntSchema2) {
		LogFix(Name(), "event/schema bindings are missing", plg::Severity::Warning);
		return false;
	}
	s_instance = this;
	try {
		const s2sdk::EventHookError error =
			s2sdk::HookEvent("round_start", &CTeamLimitFix::OnRoundStart, s2sdk::HookMode::Pre);
		m_hooked = error == s2sdk::EventHookError::Okay;
	} catch (...) {
		m_hooked = false;
	}
	if (!m_hooked) {
		s_instance = nullptr;
		LogFix(Name(), "hooking round_start failed", plg::Severity::Warning);
		return false;
	}
	LogFix(Name(), "listening for round_start");
	return true;
}

void CTeamLimitFix::Unload() {
	if (m_hooked && __s2sdk_UnhookEvent) {
		s2sdk::UnhookEvent("round_start", &CTeamLimitFix::OnRoundStart, s2sdk::HookMode::Pre);
	}
	m_hooked = false;
	s_instance = nullptr;
}

} // namespace

std::unique_ptr<CFix> MakeTeamLimitFix(Environment& env) {
	return std::make_unique<CTeamLimitFix>(env);
}

} // namespace game_fixes
