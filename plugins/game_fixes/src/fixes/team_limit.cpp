// team_limit — the game caps how many players may join each team from the map's spawn points,
// so a join gets refused as "team full" on maps with few spawns. On every round_start (post, so
// the game's own round-start handling has already run), CCSGameRules' spawnable/max T and CT
// counts and both spawn-point counts are raised to maxplayers.
//
// The fork hooked the game's event manager (round_start) and cached the rules object per map;
// this port uses s2sdk's round_start hook and s2sdk's g_pGameRules directly (s2sdk already
// tracks the cs_gamerules entity).
//
// The six fields are resolved through the schema by name (GetSchemaOffset) and written raw with
// SetEntData2, width 4 (sdk.json: m_iNumSpawnableTerrorist 3476, m_iNumSpawnableCT 3480,
// m_iSpawnPointCount_Terrorist 3716, m_iSpawnPointCount_CT 3720, m_iMaxNumTerrorists 3724,
// m_iMaxNumCTs 3728 — all int32 and networked:false). changeState=false for the same reason: a
// network state change on a non-networked field is impossible, s2sdk would only log "cannot be
// send over network" every round start, and the join-limit check reads the value directly.
// The offsets are resolved on the first round_start (a map is running by then; GetSchemaOffset
// before a map exists segfaults), not at plugin start.

#include "../fixes.hpp"
#include "../map_gate.hpp"
#include "../team_limit_logic.hpp"

namespace game_fixes {
namespace {

class CTeamLimitFix final : public CFix {
public:
	explicit CTeamLimitFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "team_limit"; }

	bool Load() override;
	void Unload() override;
	void OnStartupServer(const char* mapName) override;

private:
	static s2sdk::ResultType OnRoundStart(const plg::string& name, void* event, bool dontBroadcast);

	void ResolveOffsets();
	void WriteFields(void* rules, std::int32_t maxPlayers);

	static CTeamLimitFix* s_instance;
	bool m_hooked = false;
	// Resolved once; -1 means "not there (yet)". m_resolveTried stops repeated schema lookups
	// (s2sdk caches and logs every miss).
	bool m_resolveTried = false;
	std::int32_t m_offsets[teamlimit::kFieldCount] = {-1, -1, -1, -1, -1, -1};
	// One failure log per map, not per round.
	bool m_loggedFailure = false;
};

CTeamLimitFix* CTeamLimitFix::s_instance = nullptr;

void CTeamLimitFix::ResolveOffsets() {
	if (m_resolveTried || !__s2sdk_GetSchemaOffset) {
		return;
	}
	m_resolveTried = true;
	bool complete = true;
	for (std::size_t i = 0; i < teamlimit::kFieldCount; ++i) {
		m_offsets[i] = s2sdk::GetSchemaOffset(plg::string("CCSGameRules"), plg::string(teamlimit::Writes(0)[i].field));
		if (m_offsets[i] < 0) {
			complete = false;
		}
	}
	if (!complete && !m_loggedFailure) {
		m_loggedFailure = true;
		LogFix(Name(), "CCSGameRules team-limit fields are missing from the schema; using the by-name setter",
			   plg::Severity::Warning);
	}
}

void CTeamLimitFix::WriteFields(void* rules, std::int32_t maxPlayers) {
	if (!rules) {
		return;
	}
	// Raw offset writes are the normal path: the widths are fixed by the schema (4 bytes).
	if (__s2sdk_SetEntData2) {
		const auto writes = teamlimit::Writes(maxPlayers);
		bool complete = true;
		for (std::int32_t offset : m_offsets) {
			if (offset < 0) {
				complete = false;
				break;
			}
		}
		if (complete) {
			for (std::size_t i = 0; i < writes.size(); ++i) {
				s2sdk::SetEntData2(rules, m_offsets[i], writes[i].value, teamlimit::kFieldSize, false, -1);
			}
			return;
		}
	}
	// Fallback for a build where one of the fields did not resolve: s2sdk's by-name setter. It
	// runs on the round_start path (a map is up), so the schema is safe to use here.
	if (!__s2sdk_SetEntSchema2) {
		return;
	}
	for (const teamlimit::FieldWrite& write : teamlimit::Writes(maxPlayers)) {
		s2sdk::SetEntSchema2(rules, "CCSGameRules", write.field, write.value, false, 0);
	}
}

s2sdk::ResultType CTeamLimitFix::OnRoundStart(const plg::string&, void*, bool) {
	try {
		// Events keep arriving during a changelevel (PITFALLS C.13): s2sdk's cached g_pGameRules is
		// stale there and the schema may only be touched with a map up (the same guard the sibling
		// event fixes use). The round_start that matters happens with a live map.
		if (!s_instance || !IsMapReady() || !__s2sdk_GetGameRules || !__s2sdk_GetMaxClients ||
			!__s2sdk_SetEntData2) {
			return s2sdk::ResultType::Continue;
		}
		void* rules = s2sdk::GetGameRules();
		if (!rules) {
			return s2sdk::ResultType::Continue;
		}
		s_instance->ResolveOffsets();
		s_instance->WriteFields(rules, s2sdk::GetMaxClients());
	} catch (...) {
	}
	return s2sdk::ResultType::Continue;
}

bool CTeamLimitFix::Load() {
	if (!__s2sdk_HookEvent || !__s2sdk_GetGameRules || !__s2sdk_SetEntData2) {
		LogFix(Name(), "event/schema bindings are missing", plg::Severity::Warning);
		return false;
	}
	s_instance = this;
	try {
		// Post: the game's own round_start handling must not run after our write, or it could put
		// the map's spawn-derived counts back.
		const s2sdk::EventHookError error =
			s2sdk::HookEvent("round_start", &CTeamLimitFix::OnRoundStart, s2sdk::HookMode::Post);
		m_hooked = error == s2sdk::EventHookError::Okay;
	} catch (...) {
		m_hooked = false;
	}
	if (!m_hooked) {
		s_instance = nullptr;
		LogFix(Name(), "hooking round_start failed", plg::Severity::Warning);
		return false;
	}
	LogFix(Name(), "listening for round_start (post)");
	return true;
}

void CTeamLimitFix::Unload() {
	if (m_hooked && __s2sdk_UnhookEvent) {
		s2sdk::UnhookEvent("round_start", &CTeamLimitFix::OnRoundStart, s2sdk::HookMode::Post);
	}
	m_hooked = false;
	s_instance = nullptr;
}

void CTeamLimitFix::OnStartupServer(const char*) {
	m_loggedFailure = false;
	// Offsets stay valid for the process once resolved; a map change does not invalidate them.
}

} // namespace

std::unique_ptr<CFix> MakeTeamLimitFix(Environment& env) {
	return std::make_unique<CTeamLimitFix>(env);
}

} // namespace game_fixes
