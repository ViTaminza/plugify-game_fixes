// slow_animation — the engine-free decision logic of the max_map_hours watch.
//
// The fix used to reload the map only while the server was empty. A production server always
// has someone on it, and a match that ends on mp_maxrounds / mp_timelimit and restarts on the
// same map (mp_match_end_restart 1) never changes the level either, so gpGlobals->curtime (a
// 32-bit float that only a level change resets) grows for hours and the game's animations,
// movement and lag compensation go sluggish. The reference fix (the Metamod fork's
// slow_animation.cpp, 2026-10-12) polls cs_gamerules and reloads with players on:
//
//   * once curtime passes max_map_hours: wait for warmup or for a match that is not the one
//     running when the watch armed (m_flGameStartTime changed or m_totalRoundsPlayed dropped --
//     a match restarted on the same map resets both), then reload;
//   * if no such moment comes within match_wait_minutes: warn in chat, wait warn_minutes, then
//     reload at the next round end (m_totalRoundsPlayed grew while m_gamePhase is not the match
//     ended phase); a game mode that never ends a round gets the reload ROUND_END_GRACE later.
//
// The match state is read from cs_gamerules by polling, not from game events: a match restart on
// the same map does not fire round_start, and cs_win_panel_match does not reach listeners.
//
// This header holds everything that decision needs and nothing engine-side, so tests/core_logic_tests
// drives it without s2sdk (ghost filtering, thresholds, new-match detection, fallback timing and
// the warning schedule).

#pragma once

namespace game_fixes {
namespace slowanim {

// CBasePlayerController::m_iConnected values (PlayerConnectedState, schema enum int32; the
// numbers are the engine's, see the reference sdk/CBasePlayerController.h).
enum class ConnectedState : int {
	NeverConnected = -1,
	Connected = 0,
	Connecting = 1,
	Reconnecting = 2,
	Disconnecting = 3,
	Disconnected = 4,
	Reserved = 5,
};

// A controller counts as a player while the player is in the game or on the way in. A player who
// is still loading must count: a level change at that moment breaks the join
// (CREATE_SERVER_FAILED). A controller left behind by a player who disconnected keeps existing
// until the map changes (the [NoChan] slots in `status`); counting those as players is why the
// old empty-server reload never fired on a server someone had visited.
inline bool CountsAsHuman(int connectedState) {
	return connectedState >= static_cast<int>(ConnectedState::Connected) &&
		   connectedState <= static_cast<int>(ConnectedState::Reconnecting);
}

// Nobody on the server for this long before an empty one is reloaded: an incoming player should
// not land right in the middle of a level change.
inline constexpr double kIdleBeforeReload = 300.0;

// How often the max_map_hours watch runs. Short enough to catch a round end inside
// mp_round_restart_delay; a few field reads per run.
inline constexpr double kWatchInterval = 2.0;

// m_gamePhase while the win panel of a finished match is up.
inline constexpr int kGamePhaseMatchEnded = 5;

// A game mode that never ends a round would never get the fallback reload, so this long after
// the warning it goes mid-round.
inline constexpr double kRoundEndGrace = 600.0;

// A level change with no StartupServer back within this long did not happen (a map name the
// engine would not take); it must not block later reloads.
inline constexpr double kReloadSettle = 300.0;

// After a forced reload, curtime is looked at once the new map has run this long. A level change
// that leaves curtime where it was would otherwise reload the map again and again.
inline constexpr double kCurtimeResetCheckDelay = 60.0;

// The three slow_animation options, clamped exactly like the reference's ReadConfig.
struct Config {
	float maxMapHours = 3.0f;
	float matchWaitMinutes = 120.0f;
	float warnMinutes = 2.0f;
};

inline Config ClampConfig(float maxMapHours, float matchWaitMinutes, float warnMinutes) {
	Config config;
	// 0 disables the forced reload; anything between 0 and 1 h would only flip the map around.
	config.maxMapHours = maxMapHours < 0.0f ? 0.0f : (maxMapHours > 0.0f && maxMapHours < 1.0f ? 1.0f : maxMapHours);
	config.matchWaitMinutes = matchWaitMinutes < 0.0f ? 0.0f : matchWaitMinutes;
	config.warnMinutes = warnMinutes < 0.0f ? 0.0f : warnMinutes;
	return config;
}

// Seconds of curtime after which a map with players on is due for a reload.
inline double CurtimeLimitSeconds(const Config& config) {
	return static_cast<double>(config.maxMapHours) * 3600.0;
}

// The CCSGameRules fields the watch reads (widths from the schema dump:
// m_bWarmupPeriod bool/1, m_flGameStartTime float32/4, m_totalRoundsPlayed int32/4,
// m_gamePhase int32/4).
struct RulesSnapshot {
	bool available = false;
	bool warmup = false;
	float gameStartTime = 0.0f;
	int totalRoundsPlayed = 0;
	int gamePhase = 0;

	// A match restarted on the same map resets the start time and the round count.
	bool IsNewMatchComparedTo(float pendingGameStart, int pendingRounds) const {
		return gameStartTime != pendingGameStart || totalRoundsPlayed < pendingRounds;
	}

	bool RoundEndedAfter(int roundsAtDeadline) const {
		return totalRoundsPlayed > roundsAtDeadline && gamePhase != kGamePhaseMatchEnded;
	}
};

enum class Action {
	None,
	ReloadWarmup,
	ReloadNewMatch,
	ReloadRoundEnd,
	ReloadGraceExpired,
};

enum class Notice {
	None,
	// curtime passed max_map_hours with players on: the watch armed.
	Armed,
	// match_wait_minutes ran out; the warning countdown started.
	FallbackWarning,
	// The warning ran out; the next round end (or the grace) will reload.
	DeadlineWarning,
	// curtime did not reset on the forced reload; the watch turned itself off.
	CurtimeNotReset,
};

struct Step {
	Action action = Action::None;
	Notice notice = Notice::None;
	bool forced = false; // the action is the max_map_hours reload (not the empty-server one)
};

// The max_map_hours state machine. The fix feeds it one sample per watch tick; the empty-server
// reload is separate and only calls OnReloadIssued().
class Watch {
public:
	// A new map started: nothing is pending, and a forced reload issued on the old map gets its
	// one chance to show that curtime reset.
	void OnNewMap(double now) {
		m_mapStart = now;
		m_reloadPending = false;
		m_fallback = false;
		m_deadlinePassed = false;
		m_reloadIssued = false;
		m_checkCurtimeReset = m_lastReloadForced;
		m_lastReloadForced = false;
	}

	// A level change was issued (from any path). Mirrors the reference: the reload is "forced"
	// when it came out of the max_map_hours watch, which is what arms the curtime-reset check.
	void OnReloadIssued(double now) {
		if (m_reloadIssued) {
			return;
		}
		m_reloadIssued = true;
		m_issuedAt = now;
		m_lastReloadForced = m_reloadPending;
		m_reloadPending = false;
		m_fallback = false;
		m_deadlinePassed = false;
	}

	bool Issued() const { return m_reloadIssued; }
	double IssuedAt() const { return m_issuedAt; }
	bool Pending() const { return m_reloadPending; }
	bool Fallback() const { return m_fallback; }
	bool Broken() const { return m_curtimeBroken; }

	// Forget a level change the engine never turned into a new map (a map name it would not
	// take), so later reloads are not blocked forever.
	bool Stale(double now) const { return m_reloadIssued && now - m_issuedAt >= kReloadSettle; }
	void ForgetIssued() {
		m_reloadIssued = false;
		m_lastReloadForced = false;
	}

	Step Tick(double now, float curtime, bool humans, const RulesSnapshot& rules, const Config& config) {
		Step step;

		if (m_curtimeBroken) {
			return step;
		}

		// After a forced reload the level change should have reset curtime. If it is still past the
		// limit on the new map, the level change did not reset it and reloading again would loop.
		if (m_checkCurtimeReset && now - m_mapStart >= kCurtimeResetCheckDelay) {
			m_checkCurtimeReset = false;
			if (static_cast<double>(curtime) >= CurtimeLimitSeconds(config)) {
				m_curtimeBroken = true;
				step.notice = Notice::CurtimeNotReset;
				return step;
			}
		}

		if (m_reloadIssued) {
			return step;
		}

		if (!m_reloadPending) {
			// curtime itself, not the time since StartupServer: it is the value that loses
			// precision, it does not move while the server hibernates, and it is right even when
			// the plugin was loaded mid-map. An empty server is the reload_interval check's
			// business.
			if (static_cast<double>(curtime) < CurtimeLimitSeconds(config) || !humans) {
				return step;
			}

			m_reloadPending = true;
			m_pendingSince = now;
			m_pendingGameStart = rules.available ? rules.gameStartTime : 0.0f;
			m_pendingRounds = rules.available ? rules.totalRoundsPlayed : 0;
			m_fallback = false;
			m_deadlinePassed = false;
			step.notice = Notice::Armed;
			return step;
		}

		if (rules.available) {
			// Warmup, or a match that is not the one that was running when the watch armed:
			// nobody plays for the score, reload now.
			if (rules.warmup) {
				step.action = Action::ReloadWarmup;
				step.forced = true;
				return step;
			}
			if (rules.IsNewMatchComparedTo(m_pendingGameStart, m_pendingRounds)) {
				step.action = Action::ReloadNewMatch;
				step.forced = true;
				return step;
			}
		}

		if (!m_fallback) {
			if (now - m_pendingSince < static_cast<double>(config.matchWaitMinutes) * 60.0) {
				return step;
			}
			// The match is not ending (mp_maxrounds 0 / 999, mp_timelimit 0, no cs_gamerules to
			// ask): the next round end it is, mid-match.
			m_fallback = true;
			m_deadline = now + static_cast<double>(config.warnMinutes) * 60.0;
			step.notice = Notice::FallbackWarning;
			return step;
		}

		if (!m_deadlinePassed) {
			if (now < m_deadline) {
				return step;
			}
			m_deadlinePassed = true;
			m_deadlineRounds = rules.available ? rules.totalRoundsPlayed : 0;
			step.notice = Notice::DeadlineWarning;
			return step;
		}

		// A round ended since the warning ran out; not the last one of a match, whose win panel
		// belongs to the match end flow (warmup or a map vote).
		if (rules.available && rules.RoundEndedAfter(m_deadlineRounds)) {
			step.action = Action::ReloadRoundEnd;
			step.forced = true;
			return step;
		}

		if (now - m_deadline >= kRoundEndGrace) {
			step.action = Action::ReloadGraceExpired;
			step.forced = true;
		}
		return step;
	}

private:
	double m_mapStart = 0.0;

	// curtime is past the limit and the reload is waiting, since m_pendingSince -- for warmup or
	// a new match (the match running then started at m_pendingGameStart, after m_pendingRounds
	// rounds), or (m_fallback) for a round end after the warning (m_deadline; m_deadlineRounds
	// rounds played by then).
	bool m_reloadPending = false;
	double m_pendingSince = 0.0;
	float m_pendingGameStart = 0.0f;
	int m_pendingRounds = 0;
	bool m_fallback = false;
	bool m_deadlinePassed = false;
	double m_deadline = 0.0;
	int m_deadlineRounds = 0;

	// A level change has been asked for; nothing asks again until OnNewMap.
	bool m_reloadIssued = false;
	double m_issuedAt = 0.0;
	// The last level change was the max_map_hours one; if curtime is still past the limit on the
	// new map, the level change did not reset it and reloading again would only loop.
	bool m_lastReloadForced = false;
	bool m_checkCurtimeReset = false;
	bool m_curtimeBroken = false;
};

} // namespace slowanim
} // namespace game_fixes
