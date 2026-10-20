// slow_animation — curtime is a 32-bit float (tickcount * interval) and the longer one map runs
// the coarser it gets: about 1 ms steps at 2.3 h, 2 ms at 4.6 h, 4 ms at 9 h, half a 64-tick
// interval at a day. Animations, movement and lag compensation turn sluggish. Only a level change
// resets it — a match that ends on mp_maxrounds or mp_timelimit and restarts on the same map
// (mp_match_end_restart 1) does not. So the map is reloaded:
//
//   * reload_interval — every that many seconds, a server with no connected human (and none for
//     300 s before that) has its map reloaded; the remaining mp_timelimit is carried over;
//   * max_map_hours — with players on, once curtime is past that many hours the reload waits for
//     a moment nobody plays for the score: warmup, or a new match on the same map (a map vote or
//     mapcycle that changes the map at the match end does the job itself). A match that does not
//     end within match_wait_minutes gets the reload at a round end instead, announced
//     warn_minutes ahead (ROUND_END_GRACE later if no round ends either). A mid-match reload
//     carries the remaining mp_timelimit, counted from the match start.
//
// The match state is read from cs_gamerules every WATCH_INTERVAL, not from game events: a match
// restarted on the same map goes straight into warmup without a round_start, and
// cs_win_panel_match does not reach listeners. The decision logic itself is engine-free
// (../slow_animation_logic.hpp, tested in tests/core_logic_tests.cpp); this file only feeds it
// the rules snapshot and the player count and carries out the reload.
//
// Ghost controllers: a controller left behind by a player who disconnected stays until the map
// changes (the [NoChan] slots in `status`). Counting those as players kept the old fix from ever
// reloading a server somebody had visited, so only Connected/Connecting/Reconnecting controllers
// count. A player who is still loading counts: a level change at that moment breaks the join
// ("CREATE_SERVER_FAILED" on the client).
//
// Schema access follows PITFALLS C: the CCSGameRules object is a separate allocation reached
// through the cs_gamerules proxy's m_pGameRules pointer (the verified plugins/round_end_sounds
// pattern), every field is read raw at its schema offset and width, and every pointer is
// validated in the same tick before it is used. The offsets are resolved lazily once a map runs.

#include "../fixes.hpp"
#include "../scheduler.hpp"
#include "../slow_animation_logic.hpp"

#include <pisex/compat.hpp>
#include <pisex/keyvalues.hpp>
#include <pisex/output.hpp>
#include <pisex/schema.hpp>
#include <pisex/timers.hpp>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

namespace game_fixes {
namespace {

class CSlowAnimationFix final : public CFix {
public:
	explicit CSlowAnimationFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "slow_animation"; }

	void ReadConfig(const pisex::KeyValues& config) override;
	bool Load() override;
	void Unload() override;
	void OnStartupServer(const char* mapName) override;

private:
	// What one pass over the player controllers found.
	struct PlayerCounts {
		// In the game or on the way in; CountPlayers() lists the states.
		int humans = 0;
		// Controllers with no client behind them: left over from a disconnect, a reservation, a
		// client that never got in. Not a reason to keep the map.
		int ghosts = 0;
	};

	PlayerCounts CountPlayers() const;

	// The reload_interval check: the diagnostics line, then the empty-server reload.
	void OnReloadTimer();
	// The max_map_hours watch: arms the reload, waits for warmup or a new match, and falls back to
	// a round end when the match does not end in time.
	void OnWatchTimer();

	// The one place a level change is issued from; why goes to the log. midMatch: the match goes on
	// after the reload, so the remaining mp_timelimit is carried over.
	void ReloadMap(const char* why, bool midMatch);
	// Forgets a level change that was asked for but never became a new map.
	void ForgetStaleReload();
	// Map name from the game server when the plugin was loaded mid-map; false while there is no
	// map at all.
	bool EnsureMapSnapshot();
	void LogStatus(const char* why);
	// Prints to every player through the chat (pisexcompat/PrintToChatAll), and logs it.
	void Announce(const char* format, ...) const;

	// Resolves the schema offsets once a map is up (never at plugin start: GetSchemaOffset walks
	// the entity network serializer info, which does not exist then).
	void ResolveSchemaOffsets();
	// The CCSGameRules object behind the cs_gamerules proxy, read fresh and validated this tick.
	void* GetGameRules();
	slowanim::RulesSnapshot ReadRules();
	static float CurTime();

	// Seconds between empty-server checks ("reload_interval").
	float m_reloadInterval = 1800.0f;
	// The max_map_hours options; clamped exactly like the reference's ReadConfig.
	slowanim::Config m_watchConfig;
	// The max_map_hours state machine (engine-free, see ../slow_animation_logic.hpp).
	slowanim::Watch m_watch;

	scheduler::Timer* m_reloadTimer = nullptr;
	pisex::CTimer* m_watchTimer = nullptr;

	uint64_t m_timelimitCvar = 0;

	// Schema offsets (CCSGameRulesProxy::m_pGameRules, the four CCSGameRules fields and
	// CBasePlayerController::m_iConnected); -1 until resolved, kept -1 when the field is missing.
	std::int32_t m_offProxyGameRules = -1;
	std::int32_t m_offWarmup = -1;
	std::int32_t m_offGameStart = -1;
	std::int32_t m_offTotalRounds = -1;
	std::int32_t m_offGamePhase = -1;
	std::int32_t m_offControllerConnected = -1;
	bool m_offsetsResolved = false;

	// Map name and universal time from the last StartupServer (or from the game server when the
	// plugin was loaded mid-map).
	char m_map[256] = "";
	double m_mapStartTime = 0.0;
	// When a human was last seen: a pause before reloading so a joining player is not caught by
	// the level change ("join, leave, join" included).
	double m_lastHumanSeen = 0.0;

	// Remaining mp_timelimit (minutes) to restore after a reload; < 0 = none.
	float m_pendingTimelimit = -1.0f;
};

void CSlowAnimationFix::ReadConfig(const pisex::KeyValues& config) {
	m_reloadInterval = config.GetFloat("reload_interval", 1800.0f);
	if (m_reloadInterval < 60.0f) {
		m_reloadInterval = 60.0f; // anything shorter would just keep flipping the map around
	}

	m_watchConfig = slowanim::ClampConfig(config.GetFloat("max_map_hours", 3.0f),
										  config.GetFloat("match_wait_minutes", 120.0f),
										  config.GetFloat("warn_minutes", 2.0f));
}

bool CSlowAnimationFix::Load() {
	if (!__s2sdk_FindConVar || !__s2sdk_GetConVarFloat || !__s2sdk_SetConVarFloat) {
		LogFix(Name(), "convar bindings are missing", plg::Severity::Warning);
		return false;
	}
	m_timelimitCvar = s2sdk::FindConVar("mp_timelimit");
	m_reloadTimer = scheduler::AddTimer(m_reloadInterval, [this]() { OnReloadTimer(); },
										scheduler::kTimerFlagRepeat);
	LogFix(Name(), Format("checking for an empty server every %.0f s", m_reloadInterval));

	if (m_watchConfig.maxMapHours > 0.0f) {
		// pisexcompat's timer pump: the delay is floored at one tick, a fire that falls into a map
		// change is postponed to the first ready frame, and the callback re-validates the map
		// state below before touching anything.
		m_watchTimer = pisex::CreateTimer(static_cast<float>(slowanim::kWatchInterval), [this]() -> float {
			OnWatchTimer();
			return static_cast<float>(slowanim::kWatchInterval);
		});
		if (m_watchTimer) {
			LogFix(Name(), Format("with players on: once curtime passes %.1f h, reload at the next warmup or "
								  "new match (or at a round end after %.0f min, warning %.0f min ahead)",
								  m_watchConfig.maxMapHours, m_watchConfig.matchWaitMinutes,
								  m_watchConfig.warnMinutes));
		} else {
			LogFix(Name(), "max_map_hours is on, but the watch timer could not be created (s2sdk timer "
						   "bindings are missing) - only the empty-server reload runs",
				   plg::Severity::Warning);
		}
	} else {
		LogFix(Name(), "max_map_hours is 0: a map players are on is never reloaded");
	}
	return true;
}

void CSlowAnimationFix::Unload() {
	scheduler::KillTimer(m_reloadTimer);
	m_reloadTimer = nullptr;
	if (m_watchTimer) {
		pisex::RemoveTimer(m_watchTimer);
		m_watchTimer = nullptr;
	}
}

void CSlowAnimationFix::OnStartupServer(const char* mapName) {
	const char* name = (mapName && mapName[0]) ? mapName : "unknown";

	// A timelimit carried over belongs to the map that was reloaded; a map the engine picked
	// instead (an admin, a vote) starts fresh.
	const std::size_t nameLength = std::strlen(name);
	const bool sameMap = m_map[0] != '\0' && std::strlen(m_map) == nameLength &&
						 EqualsIgnoreCaseN(m_map, name, nameLength);
	const float pending = sameMap ? m_pendingTimelimit : -1.0f;
	m_pendingTimelimit = -1.0f;

	std::snprintf(m_map, sizeof(m_map), "%s", name);
	m_mapStartTime = scheduler::UniversalTime();
	if (m_lastHumanSeen <= 0.0) {
		m_lastHumanSeen = m_mapStartTime;
	}

	// New map, new cs_gamerules, nothing pending.
	m_watch.OnNewMap(m_mapStartTime);

	LogFix(Name(), Format("map snapshot '%s'", m_map));

	// The map's own cfg runs after StartupServer and would overwrite the value; one frame later is
	// past that.
	scheduler::NextFrame([this, pending]() {
		if (!pisex::IsGameReady()) {
			return;
		}
		LogStatus("new map");
		if (pending < 0.0f || m_timelimitCvar == 0) {
			return;
		}
		const float value = pending > 0.0f ? pending : 0.1f;
		LogFix(Name(), Format("restoring the remaining timelimit -> mp_timelimit %.1f", value));
		try {
			s2sdk::SetConVarFloat(m_timelimitCvar, value, false, false);
		} catch (...) {
		}
	});
}

bool CSlowAnimationFix::EnsureMapSnapshot() {
	if (m_map[0] != '\0') {
		return true;
	}

	// Loaded mid-map (plugin reload on a running server): no StartupServer to name the map, so ask
	// the game server. Its map age is unknown, but max_map_hours goes by curtime and does not need
	// it.
	const std::string_view map = pisex::CurrentMap();
	if (map.empty()) {
		return false;
	}
	const std::size_t length = map.size() < sizeof(m_map) - 1 ? map.size() : sizeof(m_map) - 1;
	std::memcpy(m_map, map.data(), length);
	m_map[length] = '\0';

	m_mapStartTime = scheduler::UniversalTime();
	if (m_lastHumanSeen <= 0.0) {
		m_lastHumanSeen = m_mapStartTime;
	}

	LogStatus("loaded mid-map, map name from the game server");
	return true;
}

void CSlowAnimationFix::OnReloadTimer() {
	// The scheduler keeps wall-clock time through a level change, so this callback can run while
	// the old entities and globals are being torn down. Do not inspect the client list, globals, or
	// gamerules until pisexcompat confirms that the new map is live.
	if (!pisex::IsGameReady()) {
		return;
	}
	if (!EnsureMapSnapshot()) {
		LogFix(Name(), "reload skipped: no map name yet");
		return;
	}

	ForgetStaleReload();

	const double now = scheduler::UniversalTime();
	if (m_watch.Issued()) {
		LogFix(Name(), Format("reload skipped: one was issued %.0f s ago and no new map has started yet",
							  now - m_watch.IssuedAt()));
		return;
	}

	ResolveSchemaOffsets();
	LogStatus("check");

	const PlayerCounts players = CountPlayers();
	if (players.humans > 0) {
		m_lastHumanSeen = now;
		LogFix(Name(), Format("reload skipped: %d human player(s) on '%s', next check in %.0f s",
							  players.humans, m_map, m_reloadInterval));
		return;
	}

	// Nobody is on right now, but someone was just here: do not reload until the pause has passed,
	// or an incoming player lands exactly in a level change. Also protects "join, leave, join".
	const double idleSeconds = now - m_lastHumanSeen;
	if (m_lastHumanSeen > 0.0 && idleSeconds < slowanim::kIdleBeforeReload) {
		LogFix(Name(), Format("reload skipped: server idle for %.0f s (waiting for %.0f s), next check in %.0f s",
							  idleSeconds, slowanim::kIdleBeforeReload, m_reloadInterval));
		return;
	}

	ReloadMap("server empty", true);
}

void CSlowAnimationFix::OnWatchTimer() {
	try {
		if (m_watchConfig.maxMapHours <= 0.0f || !m_watchTimer || !pisex::IsGameReady()) {
			return;
		}
		ResolveSchemaOffsets();
		if (!EnsureMapSnapshot()) {
			return;
		}

		ForgetStaleReload();
		if (m_watch.Issued()) {
			return;
		}

		const double now = scheduler::UniversalTime();
		const float curtime = CurTime();
		const PlayerCounts players = CountPlayers();
		const slowanim::RulesSnapshot rules = ReadRules();

		const slowanim::Step step = m_watch.Tick(now, curtime, players.humans > 0, rules, m_watchConfig);

		switch (step.notice) {
		case slowanim::Notice::Armed:
			LogStatus("max_map_hours");
			LogFix(Name(), Format("curtime is past %.1f h -- reloading at the next warmup or new match "
								  "(round end fallback in %.0f min)",
								  m_watchConfig.maxMapHours, m_watchConfig.matchWaitMinutes));
			Announce("[Server] Карта %s идёт уже %.1f ч, от этого начинаются слоумо и кривые анимации. "
					 "После окончания матча карта перезапустится (около минуты).",
					 m_map, static_cast<double>(curtime) / 3600.0);
			break;
		case slowanim::Notice::FallbackWarning:
			LogFix(Name(), Format("no new match within %.0f min -- reloading at the first round end after %.0f min",
								  m_watchConfig.matchWaitMinutes, m_watchConfig.warnMinutes));
			Announce("[Server] Чтобы убрать слоумо, карта перезапустится в конце раунда, через %.0f мин "
					 "или чуть позже (около минуты).",
					 m_watchConfig.warnMinutes);
			break;
		case slowanim::Notice::DeadlineWarning:
			LogFix(Name(), Format("the warning is over -- reloading at the next round end (or in %.0f s)",
								  slowanim::kRoundEndGrace));
			Announce("[Server] Карта перезапустится в конце этого раунда.");
			break;
		case slowanim::Notice::CurtimeNotReset:
			LogStatus("after the forced reload");
			LogFix(Name(), "curtime did not reset on the level change -- max_map_hours is off until the plugin "
						   "is reloaded",
				   plg::Severity::Warning);
			return;
		case slowanim::Notice::None:
			break;
		}

		switch (step.action) {
		case slowanim::Action::ReloadWarmup:
			ReloadMap("warmup", false);
			break;
		case slowanim::Action::ReloadNewMatch:
			ReloadMap("a new match started", false);
			break;
		case slowanim::Action::ReloadRoundEnd:
			ReloadMap("round ended", true);
			break;
		case slowanim::Action::ReloadGraceExpired:
			ReloadMap("no round ended after the warning", true);
			break;
		case slowanim::Action::None:
			break;
		}
	} catch (...) {
	}
}

void CSlowAnimationFix::ForgetStaleReload() {
	const double now = scheduler::UniversalTime();
	if (!m_watch.Stale(now)) {
		return;
	}
	LogFix(Name(), Format("the level change issued %.0f s ago never started a new map -- forgetting it",
						  now - m_watch.IssuedAt()));
	m_watch.ForgetIssued();
}

void CSlowAnimationFix::ReloadMap(const char* why, bool midMatch) {
	if (m_watch.Issued()) {
		return;
	}

	const double now = scheduler::UniversalTime();
	const bool forced = m_watch.Pending();
	m_watch.OnReloadIssued(now);

	const float timelimit = m_timelimitCvar != 0 ? s2sdk::GetConVarFloat(m_timelimitCvar) : 0.0f;
	if (midMatch && timelimit > 0.0f) {
		// Elapsed since the match started (the game's own m_flGameStartTime, so matches restarted
		// on this map and mp_restartgame are counted right); since the map started if the rules
		// are not there to ask.
		float elapsedMinutes = static_cast<float>(now - m_mapStartTime) / 60.0f;
		const slowanim::RulesSnapshot rules = ReadRules();
		const float curtime = CurTime();
		if (rules.available && rules.gameStartTime > 0.0f && rules.gameStartTime <= curtime) {
			elapsedMinutes = (curtime - rules.gameStartTime) / 60.0f;
		}
		m_pendingTimelimit = timelimit - elapsedMinutes;
		LogFix(Name(), Format("timelimit %.1f min, elapsed %.1f min of this match -> restoring %.1f min "
							  "after the reload",
							  timelimit, elapsedMinutes, m_pendingTimelimit));
	}

	if (forced) {
		Announce("[Server] Перезапуск карты против слоумо...");
	}

	try {
		if (s2sdk::IsMapValid(plg::string(m_map))) {
			LogFix(Name(), Format("%s -> ChangeLevel('%s')", why, m_map));
			s2sdk::ServerCommand(plg::string("changelevel ") + m_map);
		} else {
			// Not a stock map -> a workshop one, which only changelevels by id.
			const std::string command = std::string("ds_workshop_changelevel ") + m_map;
			LogFix(Name(), std::string(why) + " -> '" + command + "'");
			s2sdk::ServerCommand(command);
		}
	} catch (...) {
	}
}

CSlowAnimationFix::PlayerCounts CSlowAnimationFix::CountPlayers() const {
	PlayerCounts counts;
	if (!__s2sdk_PlayerSlotToClientPtr || !__s2sdk_GetMaxClients) {
		return counts;
	}
	// No map running (right after load, or during shutdown/changelevel, when the client list is
	// torn down): report a player instead of touching anything, so no reload can fire in that
	// window. The next check sees the new map.
	if (!pisex::IsGameReady()) {
		counts.humans = 1;
		return counts;
	}

	const int maxClients = s2sdk::GetMaxClients();
	const bool haveControllerState = m_offControllerConnected >= 0 && __s2sdk_PlayerSlotToEntPointer &&
									 __s2sdk_EntPointerToEntHandle && __s2sdk_IsValidEntHandle &&
									 __s2sdk_GetEntData;
	for (int slot = 0; slot < maxClients; ++slot) {
		void* client = s2sdk::PlayerSlotToClientPtr(slot);
		// Bots and GOTV are not players. Both flags live on the client, so a ghost controller (no
		// client) cannot be one of these.
		if (client != nullptr) {
			if (m_env.offFakePlayer >= 0 && BoolAt(client, m_env.offFakePlayer)) {
				continue;
			}
			if (m_env.offIsHLTV >= 0 && BoolAt(client, m_env.offIsHLTV)) {
				continue;
			}
		}

		if (haveControllerState) {
			void* controller = s2sdk::PlayerSlotToEntPointer(slot);
			if (controller == nullptr) {
				// A client without a controller yet is still on its way in; never race its join.
				if (client != nullptr) {
					++counts.humans;
				}
				continue;
			}
			const std::int32_t handle = s2sdk::EntPointerToEntHandle(controller);
			if (!IsValidHandle(handle) || !s2sdk::IsValidEntHandle(handle)) {
				continue;
			}
			const int state = static_cast<int>(s2sdk::GetEntData(handle, m_offControllerConnected, 4));
			if (slowanim::CountsAsHuman(state)) {
				++counts.humans;
			} else {
				++counts.ghosts;
			}
			continue;
		}

		// No schema offset (older s2sdk): the old behaviour, any client at the slot is a human.
		if (client != nullptr) {
			++counts.humans;
		}
	}
	return counts;
}

void CSlowAnimationFix::ResolveSchemaOffsets() {
	if (m_offsetsResolved || !pisex::IsGameReady()) {
		return;
	}
	m_offProxyGameRules = pisex::SchemaOffset("CCSGameRulesProxy", "m_pGameRules");
	m_offWarmup = pisex::SchemaOffset("CCSGameRules", "m_bWarmupPeriod");
	m_offGameStart = pisex::SchemaOffset("CCSGameRules", "m_flGameStartTime");
	m_offTotalRounds = pisex::SchemaOffset("CCSGameRules", "m_totalRoundsPlayed");
	m_offGamePhase = pisex::SchemaOffset("CCSGameRules", "m_gamePhase");
	m_offControllerConnected = pisex::SchemaOffset("CBasePlayerController", "m_iConnected");
	m_offsetsResolved = true;

	if (m_offWarmup < 0 || m_offGameStart < 0 || m_offTotalRounds < 0 || m_offGamePhase < 0) {
		LogFix(Name(), "the CCSGameRules fields were not all found in the schema - warmup/new-match/round-end "
					   "detection is off, the max_map_hours grace still applies",
			   plg::Severity::Warning);
	}
	if (m_offControllerConnected < 0) {
		LogFix(Name(), "CBasePlayerController::m_iConnected was not found - every client counts as a human "
					   "(leftover controllers cannot be filtered)",
			   plg::Severity::Warning);
	}
}

void* CSlowAnimationFix::GetGameRules() {
	if (!pisex::IsGameReady()) {
		return nullptr;
	}

	// The authoritative rules pointer, in this tick: the proxy's m_pGameRules field. The proxy is
	// an entity, so its handle is validated here, right before the pointer is read; the rules
	// object itself is a separate allocation and is only used in this same tick (PITFALLS C.3).
	void* proxy = __s2sdk_GetGameRulesProxy ? s2sdk::GetGameRulesProxy() : nullptr;
	if (proxy != nullptr && m_offProxyGameRules >= 0 && __s2sdk_EntPointerToEntHandle &&
		__s2sdk_IsValidEntHandle && __s2sdk_GetEntData2) {
		const std::int32_t handle = s2sdk::EntPointerToEntHandle(proxy);
		if (IsValidHandle(handle) && s2sdk::IsValidEntHandle(handle)) {
			try {
				const std::int64_t value =
					s2sdk::GetEntData2(proxy, m_offProxyGameRules, static_cast<std::int32_t>(sizeof(void*)));
				if (value > 0) {
					return reinterpret_cast<void*>(static_cast<std::intptr_t>(value));
				}
			} catch (...) {
			}
		}
	}

	// Fallback for a build where the field offset could not be resolved: s2sdk's cached global.
	if (__s2sdk_GetGameRules) {
		try {
			return s2sdk::GetGameRules();
		} catch (...) {
		}
	}
	return nullptr;
}

slowanim::RulesSnapshot CSlowAnimationFix::ReadRules() {
	slowanim::RulesSnapshot rules;
	void* object = GetGameRules();
	if (object == nullptr) {
		return rules;
	}
	if (m_offWarmup < 0 || m_offGameStart < 0 || m_offTotalRounds < 0 || m_offGamePhase < 0 ||
		!__s2sdk_GetEntData2 || !__s2sdk_GetEntDataFloat2) {
		return rules;
	}
	try {
		rules.available = true;
		// Widths from the schema dump: bool/1, float32/4, int32/4, int32/4.
		rules.warmup = s2sdk::GetEntData2(object, m_offWarmup, 1) != 0;
		rules.gameStartTime = static_cast<float>(s2sdk::GetEntDataFloat2(object, m_offGameStart, 4));
		rules.totalRoundsPlayed = static_cast<int>(s2sdk::GetEntData2(object, m_offTotalRounds, 4));
		rules.gamePhase = static_cast<int>(s2sdk::GetEntData2(object, m_offGamePhase, 4));
	} catch (...) {
		rules = slowanim::RulesSnapshot();
	}
	return rules;
}

float CSlowAnimationFix::CurTime() {
	// The snapshot is refreshed at the start of every pisex::Poll(); the watch timer fires from
	// inside that Poll, and the reload timer runs right after it, so this is the current curtime.
	return pisex::GetCGlobalVars()->curtime;
}

void CSlowAnimationFix::LogStatus(const char* why) {
	const pisex::GlobalVars* globals = pisex::GetCGlobalVars();
	const float curtime = globals->curtime;
	// The gap to the next representable float is the step every animation, movement and lag
	// compensation timestamp is quantised to.
	const float stepMs =
		(std::nextafterf(curtime, std::numeric_limits<float>::infinity()) - curtime) * 1000.0f;

	const PlayerCounts players = CountPlayers();
	const slowanim::RulesSnapshot rules = ReadRules();

	char rulesText[96] = "no cs_gamerules";
	if (rules.available) {
		std::snprintf(rulesText, sizeof(rulesText), "phase %d%s, %d round(s) played", rules.gamePhase,
					  rules.warmup ? " (warmup)" : "", rules.totalRoundsPlayed);
	}

	LogFix(Name(),
		   Format("%s: map '%s' for %.1f min, curtime %.1f s (%.2f h, float step %.2f ms), tickcount %d, %s, "
				  "%d human(s), %d leftover controller(s)%s",
				  why, m_map, (scheduler::UniversalTime() - m_mapStartTime) / 60.0, curtime, curtime / 3600.0f,
				  stepMs, globals->tickcount, rulesText, players.humans, players.ghosts,
				  m_watch.Pending() ? (m_watch.Fallback() ? ", reload due at a round end" : ", reload due at warmup / a new match")
									: ""));
}

void CSlowAnimationFix::Announce(const char* format, ...) const {
	char message[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);

	pisex::PrintToChatAll("%s", message);
	LogFix(Name(), std::string("announced: ") + message);
}

} // namespace

std::unique_ptr<CFix> MakeSlowAnimationFix(Environment& env) {
	return std::make_unique<CSlowAnimationFix>(env);
}

} // namespace game_fixes
