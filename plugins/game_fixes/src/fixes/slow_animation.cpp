// slow_animation — curtime is a 32-bit float and loses precision after a day or two on one map
// (animations, movement and lag compensation turn sluggish). Every reload_interval seconds,
// while no human is connected, the current map is reloaded; the remaining mp_timelimit is
// carried over.
//
// The fork counted the CBasePlayerController entities and deliberately did *not* require
// IsConnected (a controller appears before the client finishes loading and reloading then breaks
// its connect). This port keeps that stance exactly, reading the clients themselves
// (CServerSideClient at the gamedata offsets): any client present at a slot that is neither a bot
// nor GOTV counts as a human, whatever its signon state — a client whose controller does not
// exist yet is still on its way in, and skipping an extra check is harmless while racing a join
// is not.

#include "../fixes.hpp"
#include "../scheduler.hpp"

#include <pisex/compat.hpp>
#include <pisex/keyvalues.hpp>

#include <cstdio>

namespace game_fixes {
namespace {

class CSlowAnimationFix final : public CFix {
public:
	explicit CSlowAnimationFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "slow_animation"; }

	void ReadConfig(const pisex::KeyValues& config) override {
		m_reloadInterval = config.GetFloat("reload_interval", 1800.0f);
		if (m_reloadInterval < 60.0f) {
			m_reloadInterval = 60.0f; // anything shorter would just keep flipping the map around
		}
	}

	bool Load() override;
	void Unload() override;
	void OnStartupServer(const char* mapName) override;

private:
	void OnReloadTimer();
	int CountHumanPlayers() const;

	// Seconds between empty-server checks ("reload_interval").
	float m_reloadInterval = 1800.0f;
	scheduler::Timer* m_reloadTimer = nullptr;

	uint64_t m_timelimitCvar = 0;

	// Map name and universal time from the last StartupServer.
	char m_map[256] = "";
	double m_mapStartTime = 0.0;
	// When a human was last seen: a pause before reloading so a joining player is not caught by
	// the level change ("join, leave, join" included).
	double m_lastHumanSeen = 0.0;

	// Remaining mp_timelimit (minutes) to restore after a reload; < 0 = none.
	float m_pendingTimelimit = -1.0f;
};

bool CSlowAnimationFix::Load() {
	if (!__s2sdk_FindConVar || !__s2sdk_GetConVarFloat || !__s2sdk_SetConVarFloat) {
		LogFix(Name(), "convar bindings are missing", plg::Severity::Warning);
		return false;
	}
	m_timelimitCvar = s2sdk::FindConVar("mp_timelimit");
	m_reloadTimer = scheduler::AddTimer(m_reloadInterval, [this]() { OnReloadTimer(); },
										scheduler::kTimerFlagRepeat);
	LogFix(Name(), Format("checking for an empty server every %.0f s", m_reloadInterval));
	return true;
}

void CSlowAnimationFix::Unload() {
	scheduler::KillTimer(m_reloadTimer);
	m_reloadTimer = nullptr;
}

void CSlowAnimationFix::OnStartupServer(const char* mapName) {
	std::snprintf(m_map, sizeof(m_map), "%s", (mapName && mapName[0]) ? mapName : "unknown");
	m_mapStartTime = scheduler::UniversalTime();
	if (m_lastHumanSeen <= 0.0) {
		m_lastHumanSeen = scheduler::UniversalTime();
	}

	LogFix(Name(), Format("map snapshot '%s'", m_map));

	if (m_pendingTimelimit < 0.0f) {
		return;
	}

	const float remaining = m_pendingTimelimit;
	m_pendingTimelimit = -1.0f;

	// The map's own cfg runs after StartupServer and would overwrite the value; one frame later
	// is past that.
	scheduler::NextFrame([this, remaining]() {
		if (m_timelimitCvar == 0) {
			return;
		}
		const float value = remaining > 0.0f ? remaining : 0.1f;
		LogFix(Name(), Format("restoring the remaining timelimit -> mp_timelimit %.1f", value));
		try {
			s2sdk::SetConVarFloat(m_timelimitCvar, value, false, false);
		} catch (...) {
		}
	});
}

void CSlowAnimationFix::OnReloadTimer() {
	if (!m_map[0]) {
		LogFix(Name(), "reload skipped: no map snapshot yet (loaded mid-map, waiting for the next StartupServer)");
		return;
	}

	const int players = CountHumanPlayers();
	if (players > 0) {
		m_lastHumanSeen = scheduler::UniversalTime();
		LogFix(Name(), Format("reload skipped: %d human player(s) on '%s', next check in %.0f s", players, m_map,
							  m_reloadInterval));
		return;
	}

	const double idleSeconds = scheduler::UniversalTime() - m_lastHumanSeen;
	if (m_lastHumanSeen > 0.0 && idleSeconds < 300.0) {
		LogFix(Name(), Format("reload skipped: server idle for %.0f s (waiting for 300 s), next check in %.0f s",
							  idleSeconds, m_reloadInterval));
		return;
	}

	const float elapsedMinutes =
		static_cast<float>(scheduler::UniversalTime() - m_mapStartTime) / 60.0f;
	const float timelimit = m_timelimitCvar != 0 ? s2sdk::GetConVarFloat(m_timelimitCvar) : 0.0f;

	if (timelimit > 0.0f) {
		m_pendingTimelimit = timelimit - elapsedMinutes;
		LogFix(Name(), Format("timelimit %.1f min, elapsed %.1f min -> restoring %.1f min after the reload",
							  timelimit, elapsedMinutes, m_pendingTimelimit));
	}

	try {
		if (s2sdk::IsMapValid(plg::string(m_map))) {
			LogFix(Name(), Format("server empty -> ChangeLevel('%s')", m_map));
			s2sdk::ServerCommand(plg::string("changelevel ") + m_map);
		} else {
			// Not a stock map -> a workshop one, which only changelevels by id.
			const std::string command = std::string("ds_workshop_changelevel ") + m_map;
			LogFix(Name(), "server empty -> '" + command + "'");
			s2sdk::ServerCommand(command);
		}
	} catch (...) {
	}
}

int CSlowAnimationFix::CountHumanPlayers() const {
	if (!__s2sdk_PlayerSlotToClientPtr || !__s2sdk_GetMaxClients) {
		return 0;
	}
	// No map running (right after load, or during shutdown/changelevel, when the client list is
	// torn down): report a player instead of touching anything, so the reload cannot fire in that
	// window. The next check sees the new map.
	if (!pisex::IsGameReady()) {
		return 1;
	}
	const int maxClients = s2sdk::GetMaxClients();
	int count = 0;
	for (int slot = 0; slot < maxClients; ++slot) {
		void* client = s2sdk::PlayerSlotToClientPtr(slot);
		if (!client) {
			continue;
		}
		if (m_env.offFakePlayer >= 0 && BoolAt(client, m_env.offFakePlayer)) {
			continue;
		}
		if (m_env.offIsHLTV >= 0 && BoolAt(client, m_env.offIsHLTV)) {
			continue;
		}
		// Deliberately no signon-state filter, matching the fork's controller count.
		++count;
	}
	return count;
}

} // namespace

std::unique_ptr<CFix> MakeSlowAnimationFix(Environment& env) {
	return std::make_unique<CSlowAnimationFix>(env);
}

} // namespace game_fixes
