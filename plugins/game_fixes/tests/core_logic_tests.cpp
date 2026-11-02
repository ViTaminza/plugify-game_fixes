// Pure-logic tests for plugins/game_fixes:
//
//   * ban_map.hpp — the exact memory writes the steam_ban fix makes in the engine's ban map
//     (entry neutralization and the RemoveAll free-list rebuild). The container itself is the
//     engine's; what can be tested here is that the fix writes only inside the node slots, that
//     the free list it builds covers every slot exactly once, and that the header ends in the
//     engine's empty state. A stray write here corrupts the server's memory, so the fixtures put
//     canaries around both the header and the node array.
//
//   * scheduler.cpp — the timer/next-frame pump the slow_animation reload and the deferred kicks
//     run on: a repeat timer fires on its interval (not per tick), KillTimer from inside a
//     callback stops it, NextFrame runs exactly once, Shutdown drops what is queued, and the first
//     tick after Init does not add the host's uptime to the clock (the fork's boot-reload bug).
//
//   * slow_animation_logic.hpp — the max_map_hours watch: ghost controllers do not count as
//     players, the curtime threshold, new-match detection, the round-end fallback and its warning
//     schedule, and the curtime-reset check after a forced reload; plus the empty-server idle
//     pause and the short last-human sample that keeps it meaningful at the 30-minute check.
//
//   * hook::EncodedIndex — the Itanium ABI member-pointer encoding every vtable hook of this
//     plugin is keyed by (1 + 8 * index). A wrong value hooks the wrong function.
//
//   * map_gate.hpp — the PITFALLS B1/B2 readiness gate: shut before the first map, latched open by
//     the first update after OnMapStart (or by the first update of a plugin loaded mid-map),
//     closed by OnMapEnd and never re-opened by an update in the map-change window.
//
//   * kv1.hpp / config.hpp — the plugin's own KeyValues reader (comments, nested blocks, quoted and
//     unquoted values, the shim's GetInt/GetFloat/GetBool semantics, malformed files) and the
//     option readers of every configurable fix: every block of the shipped assets/game_fixes.ini
//     is read, the documented defaults and floors hold when a key is missing, and the enabled flags
//     match the shipped file.
//
//   * paths.hpp — the config lookup order that replaced pisex::ResolvePath.
//
//   * schema_cache.hpp — the map-gated, success-only schema offset cache that replaced
//     pisex::SchemaOffset: no lookup while the gate is shut, a miss stays retryable, Clear()
//     forgets.
//
//   * scheduler.cpp, additionally — the per-map delayed work dropped on OnMapEnd
//     (ClearDelayedWork) and the >= 1/64 s delay floor (PITFALLS C.9).

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unistd.h>
#include <string>
#include <unordered_set>
#include <vector>

#include "ban_map.hpp"
#include "blank_map_logic.hpp"
#include "config.hpp"
#include "cstv_logic.hpp"
#include "fake_messages_logic.hpp"
#include "jump_spam_logic.hpp"
#include "kv1.hpp"
#include "load_policy.hpp"
#include "map_gate.hpp"
#include "paths.hpp"
#include "player_ghost_logic.hpp"
#include "scheduler.hpp"
#include "schema_cache.hpp"
#include "slow_animation_logic.hpp"
#include "sv_cheats_logic.hpp"
#include "team_limit_logic.hpp"
#include "voice_fix_logic.hpp"

// api.hpp is needed for hook::EncodedIndex only; nothing out of it is called here, so the binding
// globals it declares are never referenced.
#include "api.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL %s\n", what.c_str());
	}
}

// ---------------------------------------------------------------- ban map --------

using game_fixes::banmap::Layout;

constexpr std::int32_t kCapacity = 6;
constexpr std::size_t kCanary = 64;
constexpr std::uint8_t kCanaryByte = 0xA5;

// A map header + node array with canaries on both sides, laid out exactly like the engine's
// (see ban_map.hpp) so the offsets in the layout can be read off it.
struct Fixture {
	std::vector<std::uint8_t> headerBefore; // canary
	std::vector<std::uint8_t> header;		// 0x28 bytes
	std::vector<std::uint8_t> nodesBefore;	// canary
	std::vector<std::uint8_t> nodes;		// kCapacity * 0x38
	std::vector<std::uint8_t> nodesAfter;	// canary

	Fixture() : headerBefore(kCanary, kCanaryByte), header(0x28, 0), nodesBefore(kCanary, kCanaryByte),
				nodes(kCapacity * 0x38, 0), nodesAfter(kCanary, kCanaryByte) {
		void* array = nodes.data();
		std::memcpy(header.data() + 0x10, &array, sizeof(array));
		Set(0x08, kCapacity); // capacity (the sweep bound)
		Set(0x18, -1);		  // root
		Set(0x1C, 0);		  // count
		Set(0x20, -1);		  // first free
	}

	[[nodiscard]] Layout layout() const {
		Layout l;
		l.array = 0x10;
		l.allocated = 0x08;
		l.root = 0x18;
		l.count = 0x1C;
		l.firstFree = 0x20;
		l.lastAlloc = 0x24;
		l.nodeStride = 0x38;
		l.nodeKey = 0x10;
		l.nodeReason = 0x18;
		return l;
	}

	void* map() { return header.data(); }

	void Set(std::int32_t offset, std::int32_t value) {
		std::memcpy(header.data() + offset, &value, sizeof(value));
	}
	[[nodiscard]] std::int32_t Get(std::int32_t offset) const {
		std::int32_t value = 0;
		std::memcpy(&value, header.data() + offset, sizeof(value));
		return value;
	}

	// Puts an entry in slot `index` (key at node+0x10, reason at node+0x18).
	void Entry(std::int32_t index, std::uint32_t key, std::uint32_t reason) {
		std::uint8_t* node = nodes.data() + static_cast<std::size_t>(index) * 0x38;
		std::memcpy(node + 0x10, &key, sizeof(key));
		std::memcpy(node + 0x18, &reason, sizeof(reason));
	}
	[[nodiscard]] std::uint32_t Key(std::int32_t index) const {
		std::uint32_t value = 0;
		std::memcpy(&value, nodes.data() + static_cast<std::size_t>(index) * 0x38 + 0x10, sizeof(value));
		return value;
	}
	[[nodiscard]] std::uint32_t Reason(std::int32_t index) const {
		std::uint32_t value = 0;
		std::memcpy(&value, nodes.data() + static_cast<std::size_t>(index) * 0x38 + 0x18, sizeof(value));
		return value;
	}
	[[nodiscard]] std::uint32_t Left(std::int32_t index) const {
		std::uint32_t value = 0;
		std::memcpy(&value, nodes.data() + static_cast<std::size_t>(index) * 0x38, sizeof(value));
		return value;
	}
	[[nodiscard]] std::uint32_t Right(std::int32_t index) const {
		std::uint32_t value = 0;
		std::memcpy(&value, nodes.data() + static_cast<std::size_t>(index) * 0x38 + 4, sizeof(value));
		return value;
	}
	void SetLeft(std::int32_t index, std::int32_t value) {
		const auto stored = static_cast<std::uint32_t>(value);
		std::memcpy(nodes.data() + static_cast<std::size_t>(index) * 0x38, &stored, sizeof(stored));
	}
	void SetRight(std::int32_t index, std::int32_t value) {
		const auto stored = static_cast<std::uint32_t>(value);
		std::memcpy(nodes.data() + static_cast<std::size_t>(index) * 0x38 + 4, &stored, sizeof(stored));
	}

	[[nodiscard]] bool HeaderCanaryIntact() const {
		for (const std::uint8_t byte : headerBefore) {
			if (byte != kCanaryByte) {
				return false;
			}
		}
		return true;
	}
	[[nodiscard]] bool NodeCanariesIntact() const {
		for (const std::uint8_t byte : nodesBefore) {
			if (byte != kCanaryByte) {
				return false;
			}
		}
		for (const std::uint8_t byte : nodesAfter) {
			if (byte != kCanaryByte) {
				return false;
			}
		}
		return true;
	}
};

void TestBanMapNeutralize() {
	Fixture fixture;
	const Layout layout = fixture.layout();
	const std::unordered_set<std::uint32_t> whitelist{1111u, 2222u};

	fixture.Entry(0, 1111u, 20u);  // whitelisted
	fixture.Entry(1, 3333u, 22u);  // cooldown
	fixture.Entry(2, 4444u, 3u);   // ordinary entry
	fixture.Entry(3, 2222u, 0u);   // whitelisted, different reason
	fixture.Set(0x1C, 4);

	const int stripped = game_fixes::banmap::Neutralize(fixture.map(), layout, whitelist, /*dropCooldowns=*/true);
	Check(stripped == 3, "neutralize strips whitelisted + cooldown entries");
	Check(fixture.Key(0) == game_fixes::banmap::kNeutral && fixture.Reason(0) == game_fixes::banmap::kNeutral,
		  "whitelisted entry is neutralized in both fields");
	Check(fixture.Key(1) == game_fixes::banmap::kNeutral && fixture.Reason(1) == game_fixes::banmap::kNeutral,
		  "cooldown entry is neutralized while the cvar is below 2");
	Check(fixture.Key(2) == 4444u && fixture.Reason(2) == 3u, "an ordinary entry is left alone");
	Check(fixture.Get(0x1C) == 4, "neutralizing does not change the element count");
	Check(static_cast<std::int32_t>(fixture.Left(0)) == 0 && fixture.Right(0) == 0,
		  "neutralizing does not touch the tree links");
	Check(fixture.HeaderCanaryIntact() && fixture.NodeCanariesIntact(), "no write outside the map");

	// A second pass must not count the same entries again.
	Check(game_fixes::banmap::Neutralize(fixture.map(), layout, whitelist, true) == 0,
		  "neutralizing twice strips nothing the second time");

	// Reason 20 is only stripped while the cvar is below 2.
	Fixture kept;
	kept.Entry(0, 3333u, 20u);
	Check(game_fixes::banmap::Neutralize(kept.map(), layout, whitelist, /*dropCooldowns=*/false) == 0,
		  "cooldowns are kept while sv_kick_players_with_cooldown is 2 or more");
	Check(kept.Key(0) == 3333u && kept.Reason(0) == 20u, "the cooldown entry is untouched then");

	// Degenerate inputs: no crash, nothing written.
	Check(game_fixes::banmap::Neutralize(nullptr, layout, whitelist, true) == 0, "neutralize tolerates a null map");
	Fixture empty;
	empty.Set(0x08, 0);
	Check(game_fixes::banmap::Neutralize(empty.map(), layout, whitelist, true) == 0,
		  "neutralize tolerates a map with no slots");
	Fixture noArray;
	Layout broken = layout;
	broken.array = -1;
	Check(game_fixes::banmap::Neutralize(noArray.map(), broken, whitelist, true) == 0,
		  "neutralize handles an invalid layout");
}

void TestBanMapRemoveAll() {
	Fixture fixture;
	const Layout layout = fixture.layout();

	fixture.Entry(0, 1111u, 20u);
	fixture.Entry(1, 3333u, 22u);
	fixture.Set(0x1C, 2);
	fixture.Set(0x18, 0); // pretend the tree has a root

	game_fixes::banmap::RemoveAll(fixture.map(), layout);

	Check(fixture.Get(0x18) == -1, "RemoveAll leaves the root empty (-1)");
	Check(fixture.Get(0x1C) == 0, "RemoveAll leaves the element count 0 (not -1)");

	// The free list must start at the header's head and visit every slot exactly once.
	const std::int32_t head = fixture.Get(0x20);
	Check(head >= 0 && head < kCapacity, "RemoveAll leaves a valid free-list head");
	int visited = 0;
	std::int32_t index = head;
	std::vector<bool> seen(static_cast<std::size_t>(kCapacity), false);
	while (index != -1) {
		Check(index >= 0 && index < kCapacity, "free-list index is inside the array");
		if (index < 0 || index >= kCapacity || seen[static_cast<std::size_t>(index)]) {
			break;
		}
		seen[static_cast<std::size_t>(index)] = true;
		Check(static_cast<std::int32_t>(fixture.Left(index)) == index, "a free slot points its left to itself");
		++visited;
		index = static_cast<std::int32_t>(fixture.Right(index));
	}
	Check(visited == kCapacity, "the free list covers every slot exactly once");
	Check(fixture.Get(0x20) == head, "the head stays the entry point of that list");
	Check(fixture.HeaderCanaryIntact() && fixture.NodeCanariesIntact(), "RemoveAll writes only inside the map");

	// Idempotent: running it again must not corrupt the list.
	game_fixes::banmap::RemoveAll(fixture.map(), layout);
	int again = 0;
	std::vector<bool> againSeen(static_cast<std::size_t>(kCapacity), false);
	for (std::int32_t i = fixture.Get(0x20); i != -1;) {
		if (i < 0 || i >= kCapacity || againSeen[static_cast<std::size_t>(i)]) {
			break;
		}
		againSeen[static_cast<std::size_t>(i)] = true;
		++again;
		i = static_cast<std::int32_t>(fixture.Right(i));
	}
	Check(again == kCapacity, "RemoveAll is idempotent");

	// A container that already had a free chain must end with one list, not a cycle.
	Fixture partial;
	partial.Set(0x20, 3); // free list head
	partial.Entry(0, 1111u, 20u);
	partial.Set(0x1C, 1);
	partial.Set(0x18, 0);
	// slots 1 and 3 carry the free marker (m_Left == index) and a chain, as a live one would
	partial.SetLeft(1, 1);
	partial.SetRight(1, -1);
	partial.SetLeft(3, 3);
	partial.SetRight(3, 1);
	game_fixes::banmap::RemoveAll(partial.map(), layout);
	int partialVisited = 0;
	std::vector<bool> partialSeen(static_cast<std::size_t>(kCapacity), false);
	for (std::int32_t i = partial.Get(0x20); i != -1;) {
		Check(i >= 0 && i < kCapacity, "rebuilt list index is inside the array");
		if (i < 0 || i >= kCapacity || partialSeen[static_cast<std::size_t>(i)]) {
			break;
		}
		partialSeen[static_cast<std::size_t>(i)] = true;
		++partialVisited;
		i = static_cast<std::int32_t>(partial.Right(i));
	}
	Check(partialVisited == kCapacity, "a pre-existing free chain does not survive into the rebuilt list");
	Check(partial.Get(0x1C) == 0 && partial.Get(0x18) == -1, "the header ends empty");
}

// ---------------------------------------------------------------- scheduler ------

void TestScheduler() {
	using namespace game_fixes::scheduler;

	// The first tick after Init has nothing to measure from: a delta that covers the host's whole
	// uptime must not push the clock forward and fire every timer at once (the reference
	// scheduler's bug: an empty-server map reload at each boot).
	Shutdown();
	Init();
	int bootFires = 0;
	AddTimer(1.0f, [&bootFires]() { ++bootFires; }, kTimerFlagRepeat);
	Tick(1000000.0);
	Check(bootFires == 0, "the first tick does not advance the clock by the host's uptime");
	Tick(0.6);
	Check(bootFires == 0, "a timer still waits for its interval to be measured");
	Tick(0.5);
	Check(bootFires == 1, "the timer fires once the measured time reaches its interval");

	Shutdown();
	Init();

	// NextFrame runs exactly once, on the next Tick.
	int frames = 0;
	NextFrame([&frames]() { ++frames; });
	Tick(0.015);
	Check(frames == 1, "NextFrame runs on the next tick");
	Tick(0.015);
	Check(frames == 1, "NextFrame does not run twice");

	// A repeat timer fires on its interval, not on every tick.
	int fires = 0;
	AddTimer(1.0f, [&fires]() { ++fires; }, kTimerFlagRepeat);
	for (int i = 0; i < 100; ++i) { // 100 * 0.05 s = 5 s
		Tick(0.05);
	}
	Check(fires >= 4 && fires <= 5, "a 1 s repeat timer fires once per second (got " + std::to_string(fires) + ")");

	// Killing a repeat timer stops it (its own counter, so the first timer above cannot blur the
	// numbers).
	Shutdown();
	Init();
	int killedFires = 0;
	Timer* selfKill = AddTimer(1.0f, [&killedFires]() { ++killedFires; }, kTimerFlagRepeat);
	Tick(0.0); // the first tick after Init only arms the clock
	Tick(1.0);
	Check(killedFires == 1, "a repeat timer fires after its interval");
	KillTimer(selfKill);
	Tick(1.0);
	Check(killedFires == 1, "KillTimer stops a repeat timer");

	// Killing from inside its own callback must not use the timer afterwards.
	Timer* dying = nullptr;
	int dyingFires = 0;
	dying = AddTimer(1.0f, [&]() { ++dyingFires; KillTimer(dying); }, kTimerFlagRepeat);
	Tick(1.0);
	Tick(1.0);
	Check(dyingFires == 1, "a timer that kills itself in its callback fires exactly once");

	// A one-shot timer fires once.
	int once = 0;
	AddTimer(0.5f, [&once]() { ++once; }, 0);
	for (int i = 0; i < 40; ++i) {
		Tick(0.05);
	}
	Check(once == 1, "a one-shot timer fires exactly once");

	// Shutdown drops what is queued: nothing may run after it.
	int afterShutdown = 0;
	NextFrame([&afterShutdown]() { ++afterShutdown; });
	Shutdown();
	Tick(0.05);
	Check(afterShutdown == 0, "Shutdown drops queued next-frame tasks");

	// A repeat timer is gone after Shutdown too.
	int postShutdown = 0;
	AddTimer(0.5f, [&postShutdown]() { ++postShutdown; }, kTimerFlagRepeat);
	Shutdown();
	Tick(1.0);
	Check(postShutdown == 0, "Shutdown removes repeat timers");

	Init();
}

// ------------------------------------------------------- slow_animation ---------

void TestSlowAnimationGhostFilter() {
	using namespace game_fixes::slowanim;

	Check(CountsAsHuman(static_cast<int>(ConnectedState::Connected)), "a connected controller is a player");
	Check(CountsAsHuman(static_cast<int>(ConnectedState::Connecting)),
		  "a connecting controller is a player (a level change would break the join)");
	Check(CountsAsHuman(static_cast<int>(ConnectedState::Reconnecting)),
		  "a reconnecting controller is a player");
	Check(!CountsAsHuman(static_cast<int>(ConnectedState::Disconnecting)),
		  "a disconnecting controller is not a reason to keep the map");
	Check(!CountsAsHuman(static_cast<int>(ConnectedState::Disconnected)),
		  "a leftover [NoChan] controller is a ghost, not a player");
	Check(!CountsAsHuman(static_cast<int>(ConnectedState::Reserved)), "a reserved controller is not a player");
	Check(!CountsAsHuman(static_cast<int>(ConnectedState::NeverConnected)),
		  "a controller that never connected is a ghost");
	Check(!CountsAsHuman(-2), "an unknown negative state is not a player");
}

void TestSlowAnimationConfig() {
	using namespace game_fixes::slowanim;

	const Config defaults = ClampConfig(3.0f, 120.0f, 2.0f);
	Check(defaults.maxMapHours == 3.0f && defaults.matchWaitMinutes == 120.0f && defaults.warnMinutes == 2.0f,
		  "the reference defaults are 3 h / 120 min / 2 min");

	const Config clamped = ClampConfig(-4.0f, -5.0f, -6.0f);
	Check(clamped.maxMapHours == 0.0f, "a negative max_map_hours becomes 0 (off)");
	Check(clamped.matchWaitMinutes == 0.0f && clamped.warnMinutes == 0.0f,
		  "negative wait/warn minutes become 0");

	Check(ClampConfig(0.25f, 120.0f, 2.0f).maxMapHours == 1.0f,
		  "a nonzero max_map_hours below 1 h is raised to 1 h");
	Check(ClampConfig(0.0f, 120.0f, 2.0f).maxMapHours == 0.0f,
		  "max_map_hours 0 stays 0: only the empty-server reload runs");
	Check(ClampConfig(8.5f, 10.0f, 1.5f).maxMapHours == 8.5f, "a value above 1 h is kept");
}

void TestSlowAnimationHumanStamp() {
	using namespace game_fixes::slowanim;

	// The short human sample: only a live map with a human on it refreshes the stamp (a sample
	// from a map change must not - the level change itself is not a reason to keep the pause).
	Check(RefreshLastHuman(1000.0, 1010.0, true, 2) == 1010.0,
		  "a human on a live map refreshes the last-human stamp");
	Check(RefreshLastHuman(1000.0, 1010.0, true, 0) == 1000.0, "an empty live map leaves the stamp alone");
	Check(RefreshLastHuman(1000.0, 1010.0, false, 1) == 1000.0,
		  "a sample taken while the map is torn down leaves the stamp alone");

	// The empty-server reload waits kIdleBeforeReload after the last human.
	Check(!EmptyLongEnough(1000.0 + kIdleBeforeReload - 1.0, 1000.0),
		  "the idle pause holds just under kIdleBeforeReload");
	Check(EmptyLongEnough(1000.0 + kIdleBeforeReload, 1000.0),
		  "the idle pause is over at kIdleBeforeReload");
	Check(EmptyLongEnough(1000.0, 0.0), "nobody was ever seen: nothing to wait for");

	// The regression the short sample exists for: a stamp refreshed only by the reload_interval
	// check (30 min by default) is already older than the pause when the next check comes around,
	// even if the last player left a second before it, so the empty server was reloaded with no
	// pause at all.
	const double reloadInterval = 1800.0;
	const double checkAt = 5000.0;
	const double lastPlayerLeftAt = checkAt + reloadInterval - 10.0;
	const double nextCheckAt = checkAt + reloadInterval;

	double unchecked = RefreshLastHuman(0.0, checkAt, true, 1);
	Check(EmptyLongEnough(nextCheckAt, unchecked),
		  "without the short sample the pause is already over at the next check");

	// With a sample every kHumanCheckInterval, the last sample with a human on is within one
	// interval of the disconnect, so the pause really runs.
	double sampled = unchecked;
	for (double t = checkAt; t <= lastPlayerLeftAt; t += kHumanCheckInterval) {
		sampled = RefreshLastHuman(sampled, t, true, 1);
	}
	Check(!EmptyLongEnough(nextCheckAt, sampled),
		  "with the short sample the check still sees the recent human and waits the pause");
	Check(EmptyLongEnough(lastPlayerLeftAt + kIdleBeforeReload, sampled),
		  "the reload happens once the pause has really passed");
}

void TestSlowAnimationWatch() {
	using namespace game_fixes::slowanim;

	const Config config = ClampConfig(3.0f, 120.0f, 2.0f);
	const float limit = static_cast<float>(CurtimeLimitSeconds(config));
	RulesSnapshot rules;
	rules.available = true;
	rules.gameStartTime = 5000.0f;
	rules.totalRoundsPlayed = 10;
	rules.gamePhase = 3;

	// Arm only with players on, at the exact threshold. An empty server belongs to reload_interval.
	{
		Watch watch;
		watch.OnNewMap(0.0);
		Step step = watch.Tick(100.0, limit - 1.0f, true, rules, config);
		Check(step.notice == Notice::None && step.action == Action::None, "below max_map_hours nothing happens");
		step = watch.Tick(101.0, limit, false, rules, config);
		Check(step.notice == Notice::None && !watch.Pending(),
			  "an empty server is left to the reload_interval check, even past max_map_hours");
		step = watch.Tick(102.0, limit, true, rules, config);
		Check(step.notice == Notice::Armed && watch.Pending(), "curtime at the limit with players on arms the watch");
		Check(!step.forced, "arming reports no action");
	}

	// Warmup and a new match reload immediately once the watch is armed.
	{
		Watch watch;
		watch.OnNewMap(0.0);
		watch.Tick(0.0, limit, true, rules, config);
		RulesSnapshot warmup = rules;
		warmup.warmup = true;
		Step step = watch.Tick(2.0, limit, true, warmup, config);
		Check(step.action == Action::ReloadWarmup && step.forced, "warmup gets the reload at once");

		Watch restartedWatch;
		restartedWatch.OnNewMap(0.0);
		restartedWatch.Tick(0.0, limit, true, rules, config);
		RulesSnapshot restarted = rules;
		restarted.gameStartTime = 6000.0f;
		restarted.totalRoundsPlayed = 0;
		step = restartedWatch.Tick(2.0, limit, true, restarted, config);
		Check(step.action == Action::ReloadNewMatch, "a changed m_flGameStartTime is a new match");

		Watch droppedWatch;
		droppedWatch.OnNewMap(0.0);
		droppedWatch.Tick(0.0, limit, true, rules, config);
		RulesSnapshot dropped = rules;
		dropped.totalRoundsPlayed = 9;
		step = droppedWatch.Tick(2.0, limit, true, dropped, config);
		Check(step.action == Action::ReloadNewMatch, "a drop of m_totalRoundsPlayed is a new match");

		Watch sameWatch;
		sameWatch.OnNewMap(0.0);
		sameWatch.Tick(0.0, limit, true, rules, config);
		RulesSnapshot same = rules;
		same.totalRoundsPlayed = 11;
		step = sameWatch.Tick(2.0, limit, true, same, config);
		Check(step.action == Action::None, "the running match is not a new one");
	}

	// Fallback: match_wait_minutes, then the warn_minutes warning, then a round end.
	{
		Watch watch;
		watch.OnNewMap(0.0);
		const double armedAt = 1000.0;
		Step step = watch.Tick(armedAt, limit, true, rules, config);
		Check(step.notice == Notice::Armed, "the watch arms at the threshold");

		step = watch.Tick(armedAt + 120.0 * 60.0 - 1.0, limit, true, rules, config);
		Check(step.notice == Notice::None, "no fallback before match_wait_minutes");
		step = watch.Tick(armedAt + 120.0 * 60.0, limit, true, rules, config);
		Check(step.notice == Notice::FallbackWarning && watch.Fallback(),
			  "match_wait_minutes exactly starts the fallback warning");

		step = watch.Tick(armedAt + 122.0 * 60.0 - 1.0, limit, true, rules, config);
		Check(step.notice == Notice::None, "the deadline waits for warn_minutes");
		step = watch.Tick(armedAt + 122.0 * 60.0, limit, true, rules, config);
		Check(step.notice == Notice::DeadlineWarning, "warn_minutes exactly ends the warning");

		// A round end: rounds played grew and the match is not on its win panel.
		RulesSnapshot lastRound = rules;
		lastRound.totalRoundsPlayed = 11;
		lastRound.gamePhase = kGamePhaseMatchEnded;
		step = watch.Tick(armedAt + 122.0 * 60.0 + 2.0, limit, true, lastRound, config);
		Check(step.action == Action::None, "the last round of a match (phase 5) does not trigger the fallback");

		RulesSnapshot roundEnd = rules;
		roundEnd.totalRoundsPlayed = 11;
		roundEnd.gamePhase = 1;
		step = watch.Tick(armedAt + 122.0 * 60.0 + 4.0, limit, true, roundEnd, config);
		Check(step.action == Action::ReloadRoundEnd && step.forced,
			  "a round end after the warning reloads mid-match");
	}

	// A game mode that never ends a round: ROUND_END_GRACE after the warning.
	{
		Watch watch;
		watch.OnNewMap(0.0);
		const double armedAt = 2000.0;
		watch.Tick(armedAt, limit, true, rules, config);
		watch.Tick(armedAt + 120.0 * 60.0, limit, true, rules, config);
		watch.Tick(armedAt + 122.0 * 60.0, limit, true, rules, config);

		const double deadline = armedAt + 122.0 * 60.0;
		Step step = watch.Tick(deadline + kRoundEndGrace - 1.0, limit, true, rules, config);
		Check(step.action == Action::None, "no round end and no grace yet: keep waiting");
		step = watch.Tick(deadline + kRoundEndGrace, limit, true, rules, config);
		Check(step.action == Action::ReloadGraceExpired && step.forced,
			  "ROUND_END_GRACE after the warning reloads anyway");
	}

	// A level change blocks further decisions until the new map, and a stale one is forgotten.
	{
		Watch watch;
		watch.OnNewMap(0.0);
		watch.Tick(10.0, limit, true, rules, config);
		watch.OnReloadIssued(10.0);
		Check(watch.Issued() && !watch.Pending(), "issuing a reload clears the pending watch");
		Step step = watch.Tick(12.0, limit, true, rules, config);
		Check(step.action == Action::None && step.notice == Notice::None, "nothing is decided while a reload is in flight");
		Check(!watch.Stale(10.0 + kReloadSettle - 1.0), "a fresh level change is not stale");
		Check(watch.Stale(10.0 + kReloadSettle), "a level change older than RELOAD_SETTLE is stale");
		watch.ForgetIssued();
		Check(!watch.Issued(), "a stale level change can be forgotten");
	}

	// After a forced reload, curtime must reset; if it did not, the watch turns itself off instead of
	// reloading in a loop.
	{
		Watch watch;
		watch.OnNewMap(0.0);
		watch.Tick(100.0, limit, true, rules, config);
		watch.OnReloadIssued(100.0);
		watch.OnNewMap(100.0);
		Step step = watch.Tick(159.0, 50000.0f, true, rules, config);
		Check(step.notice == Notice::Armed, "before the 60 s check the still-high curtime re-arms the watch");
		step = watch.Tick(160.0, 50000.0f, true, rules, config);
		Check(step.notice == Notice::CurtimeNotReset && watch.Broken(),
			  "curtime still past the limit after the forced reload disables max_map_hours");
		step = watch.Tick(1000.0, 50000.0f, true, rules, config);
		Check(step.action == Action::None && step.notice == Notice::None, "a broken watch stays quiet");
	}

	// No cs_gamerules: no warmup/new-match/round-end detection, but the grace still reloads.
	{
		Watch watch;
		watch.OnNewMap(0.0);
		const RulesSnapshot none;
		Step step = watch.Tick(0.0, limit, true, none, config);
		Check(step.notice == Notice::Armed, "the watch arms without cs_gamerules too");
		step = watch.Tick(120.0 * 60.0, limit, true, none, config);
		Check(step.notice == Notice::FallbackWarning, "the fallback starts without cs_gamerules");
		step = watch.Tick(122.0 * 60.0, limit, true, none, config);
		Check(step.notice == Notice::DeadlineWarning, "the warning ends without cs_gamerules");
		step = watch.Tick(122.0 * 60.0 + kRoundEndGrace, limit, true, none, config);
		Check(step.action == Action::ReloadGraceExpired, "without rules the grace reload still happens");
	}
}

// ------------------------------------------------------------- team limit ------

void TestTeamLimitFields() {
	using namespace game_fixes::teamlimit;

	// All six counters the reference raises; the two spawn-point counts are the ones the original
	// fork omitted. Every value is maxplayers and every width is the schema's 4 bytes.
	const auto writes = Writes(24);
	Check(writes.size() == 6, "team_limit writes six CCSGameRules fields");
	Check(kFieldSize == 4, "team_limit field width is int32 (sdk.json)");
	std::unordered_set<std::string> names;
	bool allMax = true;
	for (const FieldWrite& write : writes) {
		names.insert(write.field);
		allMax = allMax && write.value == 24;
	}
	Check(names.size() == 6, "team_limit field names are distinct");
	Check(allMax, "every team_limit field is raised to maxplayers");
	Check(names.count("m_iSpawnPointCount_Terrorist") == 1 && names.count("m_iSpawnPointCount_CT") == 1,
		  "team_limit includes both spawn-point counters");
	Check(names.count("m_iNumSpawnableTerrorist") == 1 && names.count("m_iNumSpawnableCT") == 1 &&
			  names.count("m_iMaxNumTerrorists") == 1 && names.count("m_iMaxNumCTs") == 1,
		  "team_limit keeps the four original counters");
}

// --------------------------------------------------------------- sv_cheats -----

void TestSvCheatsLogic() {
	using namespace game_fixes::svcheats;

	// V_atoi semantics: the leading number decides, and "false"/empty count as off.
	Check(IsOffValue(""), "an empty convar value is off");
	Check(IsOffValue("0"), "\"0\" is off");
	Check(IsOffValue("0.0"), "\"0.0\" is off");
	Check(IsOffValue("false"), "\"false\" is off");
	Check(IsOffValue("False") && IsOffValue("FALSE"), "\"False\"/\"FALSE\" are off too (bool.TryParse)");
	Check(!IsOffValue("1"), "\"1\" is on");
	Check(!IsOffValue("true"), "\"true\" is on");
	Check(!IsOffValue("2"), "\"2\" is on");
	Check(!IsOffValue("nonsense"), "a value without a number is not a zero");

	// Either field being NOCLIP means the pawn is still stuck; both WALK means nothing to do.
	Check(NeedsReset(7, 2), "m_MoveType=NOCLIP needs a reset");
	Check(NeedsReset(2, 7), "m_nActualMoveType=NOCLIP needs a reset even when m_MoveType is not");
	Check(NeedsReset(7, 7), "both NOCLIP needs a reset");
	Check(!NeedsReset(2, 2), "both WALK needs no reset");
}

// --------------------------------------------------------------- voice_fix -----

void TestVoiceFixLogic() {
	using namespace game_fixes::voicefix;

	Check(HasAudioField(1), "_has_bits_ bit 0 is the audio field");
	Check(!ShouldDrop(1), "a message with audio is kept");
	Check(ShouldDrop(0), "a message with no audio field is dropped");
	Check(ShouldDrop(2), "another field set without bit 0 is still no audio");
	Check(IsVoiceDataMessage(47, 47), "the voice message id matches itself");
	Check(!IsVoiceDataMessage(47, 46), "a different message id is not voice data");
	Check(!IsVoiceDataMessage(0, 0), "message id 0 is never voice data");
}

// ------------------------------------------------------------- player_ghost ---

void TestPlayerGhostLogic() {
	using namespace game_fixes::playerghost;

	Check(IsGhostCandidate(true, true, true, kTeamSpectator), "alive pawn on the spectator team is a ghost");
	Check(!IsGhostCandidate(true, true, true, 2), "a T pawn is not a ghost");
	Check(!IsGhostCandidate(true, true, true, kTeamNone), "a team-less pawn is not a ghost");
	Check(!IsGhostCandidate(true, false, true, kTeamSpectator), "a dead pawn is not a ghost");
	Check(!IsGhostCandidate(false, true, true, kTeamSpectator), "a client not in game is not touched");
	Check(!IsGhostCandidate(true, true, false, kTeamSpectator), "a missing pawn is not touched");
}

// ----------------------------------------------------------- fake_messages -----

void TestFakeMessagesLogic() {
	using namespace game_fixes::fakemessages;

	Check(IsChatCommand("say"), "say is chat");
	Check(IsChatCommand("SAY"), "command matching is case-insensitive");
	Check(IsChatCommand("say_team"), "say_team is chat");
	Check(IsChatCommand("Say_Team"), "say_team matching is case-insensitive");
	Check(!IsChatCommand("say_teamx"), "a longer command is not say_team");
	Check(!IsChatCommand("jointeam"), "jointeam is not chat and must pass");
	Check(!IsChatCommand(""), "an empty command is not chat");

	Check(SignonAllowsChat(5), "signon SPAWN may chat");
	Check(SignonAllowsChat(6), "signon FULL may chat");
	Check(!SignonAllowsChat(4), "signon PRESPAWN may not chat");
	Check(!SignonAllowsChat(0), "signon NONE may not chat");
}

// --------------------------------------------------------------- blank_map -----

void TestBlankMapLogic() {
	using namespace game_fixes::blankmap;

	Check(NeedsRestart("", "de_dust2"), "an empty nextlevel with a map name restarts");
	Check(!NeedsRestart("de_nuke", "de_dust2"), "a next map set means no restart");
	Check(!NeedsRestart("", ""), "without a map name there is nothing to restart");
	Check(RestartCommand("de_dust2", true) == "changelevel de_dust2", "a stock map uses changelevel");
	Check(RestartCommand("3070904001", false) == "ds_workshop_changelevel 3070904001",
		  "a workshop map uses ds_workshop_changelevel");
}

// ---------------------------------------------------------- cstv_bot_quota -----

void TestCstvLogic() {
	using namespace game_fixes::cstv;

	Check(ShouldRejectCandidate(1, true, true), "HLTV is rejected while the selector runs");
	Check(!ShouldRejectCandidate(0, true, true), "HLTV is not rejected outside bot selection");
	Check(!ShouldRejectCandidate(1, false, true), "a missing controller is never rejected");
	Check(!ShouldRejectCandidate(1, true, false), "a real bot is never rejected");
	Check(EnterSelection(0) == 1, "entering selection raises the depth");
	Check(LeaveSelection(1) == 0, "leaving selection lowers the depth");
	Check(LeaveSelection(0) == 0, "a stray leave cannot arm the predicate");
}

// --------------------------------------------------------------- jump_spam -----

void TestJumpSpamLogic() {
	using namespace game_fixes::jumpspam;

	Check(!ShouldCancelJump(kNoJumpTick, 0), "tick 0 is a real tick and does not match the initial -1");
	Check(ShouldCancelJump(5, 5), "the second jump check of the same tick is cancelled");
	Check(!ShouldCancelJump(4, 5), "a new tick may jump");
	Check(RecordJump(7) == 7, "a jump records its move tick");
}

// ----------------------------------------------------------- load policy --------

void TestLoadPolicy() {
	using game_fixes::DecideLoad;
	using game_fixes::LoadDecision;

	// A fix that needs the schema may never be loaded before the offsets resolved: calling into
	// the schema system at plugin start segfaults (CollectSchemaFields on a null class-info
	// node), which is what this decision table exists to prevent.
	Check(DecideLoad(/*enabled=*/false, /*needsSchema=*/false, /*schemaResolved=*/false) == LoadDecision::Skip,
		  "a disabled fix is skipped");
	Check(DecideLoad(false, true, true) == LoadDecision::Skip, "a disabled schema fix is skipped too");
	Check(DecideLoad(true, false, false) == LoadDecision::Load,
		  "a fix without schema needs loads before the offsets exist");
	Check(DecideLoad(true, true, false) == LoadDecision::DeferSchema,
		  "a schema fix waits for the offsets (never loads early)");
	Check(DecideLoad(true, true, true) == LoadDecision::Load, "a schema fix loads once the offsets are in");
	Check(DecideLoad(true, false, true) == LoadDecision::Load, "an ordinary fix loads once a map is up");
}

// ---------------------------------------------------------------- encoding ------

void TestEncodedIndex() {
	// Itanium ABI pointer-to-member-function: 1 + 8 * index. polyhook decodes exactly this.
	for (std::int32_t index : {0, 1, 5, 8, 16, 27, 35, 39, 63}) {
		const auto encoded = reinterpret_cast<std::uintptr_t>(game_fixes::hook::EncodedIndex(index));
		Check(encoded == 1u + 8u * static_cast<std::uintptr_t>(index),
			  "EncodedIndex(" + std::to_string(index) + ") is 1 + 8*index");
	}
}

// ---------------------------------------------------------------- handles ------

void TestHandleValidity() {
	// A real case: index 93, serial 102949 -> 0xC912805D = -921534371, a valid handle the
	// game hands out once its serial counter has passed 65535 (bit 31 set).
	Check(game_fixes::IsValidHandle(-921534371), "-921534371 (0xC912805D) is a valid handle");
	Check(game_fixes::IsValidHandle(0x7FFFFFFF), "0x7FFFFFFF is a valid handle");
	Check(game_fixes::IsValidHandle(1), "handle 1 is valid");
	Check(!game_fixes::IsValidHandle(0), "0 (the world / \"none\") is not a valid handle");
	Check(!game_fixes::IsValidHandle(-1), "-1 (INVALID_EHANDLE_INDEX) is not a valid handle");
}


// ---------------------------------------------------------------- KV1 reader ----

void TestKv1() {
	using game_fixes::KvNode;
	using game_fixes::ParseKv1;

	const std::string_view text =
		"\xEF\xBB\xBF// leading comment\n"
		"\"game_fixes\"\n"
		"{\n"
		"\t\"one\"\t\t\"1\"\n"
		"\t\"nested\"\n"
		"\t{\n"
		"\t\t\"deep\" \"42\"\n"
		"\t}\n"
		"\t\"block_only\"\n"
		"\t{\n"
		"\t}\n"
		"\t\"unquoted\" value // trailing comment\n"
		"\t\"quoted\" \"a \\\"quoted\\\" value\"\n"
		"\t\"float\" \"2.5\"\n"
		"\t\"yes\" \"YES\"\n"
		"}\n";
	const auto root = ParseKv1(text);
	Check(root.has_value(), "a well-formed KV1 document parses (with a BOM and comments)");
	if (!root) {
		return;
	}
	Check(root->name == "game_fixes", "the single top-level block becomes the parsed root");
	Check(root->Children().size() == 7, "every child node of the root block is kept");
	const KvNode* one = root->Find("one");
	Check(one != nullptr && one->GetInt({}) == 1, "GetInt reads the node's own value");
	Check(root->Find("ONE") == one, "Find is case-insensitive");
	Check(root->Find("nested") != nullptr && root->Find("nested")->Find("deep") != nullptr &&
			  root->Find("nested")->Find("deep")->GetInt({}) == 42,
		  "nested blocks are parsed");
	Check(root->Find("unquoted") != nullptr && root->Find("unquoted")->GetString({}) == "value",
		  "an unquoted value ends at a trailing comment");
	Check(root->Find("quoted") != nullptr && root->Find("quoted")->GetString({}) == "a \"quoted\" value",
		  "escapes inside a quoted string are decoded");
	Check(root->Find("float") != nullptr && root->Find("float")->GetFloat({}) == 2.5f,
		  "GetFloat reads a float value");
	Check(root->Find("float")->GetInt({}) == 2, "GetInt keeps Valve's atoi fallback on \"2.5\"");
	Check(root->Find("yes")->GetBool({}), "\"YES\" is true for GetBool");
	Check(!root->Find("block_only")->GetBool({}, false), "a block with no value reads the bool default");
	Check(root->Find("missing") == nullptr, "an unknown key is not found");
	Check(root->GetInt("missing", 7) == 7, "a missing key reads the int default");
	Check(root->GetBool("one"), "a numeric value is read as a bool");
	Check(root->GetString("missing", "def") == "def", "a missing key reads the string default");

	// The bool spellings the shim accepted.
	const auto bools = ParseKv1("\"b\" { \"on\" \"on\" \"off\" \"OFF\" \"yes\" \"yes\" \"no\" \"no\" "
								"\"num\" \"1\" \"str\" \"abc\" }");
	Check(bools.has_value(), "the bool fixture parses");
	if (bools) {
		Check(bools->GetBool("on") && bools->GetBool("off", true) == false, "on/off spellings");
		Check(bools->GetBool("yes") && !bools->GetBool("no", true), "yes/no spellings");
		Check(bools->GetBool("num") && !bools->GetBool("str"), "numbers decide, text does not");
	}

	// Malformed documents are rejected as a whole: a half-read tree must never be used.
	Check(!ParseKv1("\"a\" \"b\" }").has_value(), "a stray top-level '}' is malformed");
	Check(!ParseKv1("\"a\" { \"b\" \"c\"").has_value(), "a missing '}' is malformed");
	Check(!ParseKv1("\"a\" \"unterminated").has_value(), "an unterminated string is malformed");
	Check(ParseKv1("").has_value() && ParseKv1("")->Children().empty(),
		  "an empty file parses to an empty tree");
}

// --------------------------------------------------------------- config --------
// The path of the shipped config: the tests parse the real file, so a broken block or a changed
// default fails the suite. CMake passes it explicitly; the __FILE__ fallback keeps a manual /tmp
// build working.
std::string TestConfigPath() {
#ifdef GAME_FIXES_TEST_CONFIG
	return GAME_FIXES_TEST_CONFIG;
#else
	const std::string file = __FILE__;
	const std::size_t tests = file.find("/tests/");
	return (tests == std::string::npos ? std::string(".") : file.substr(0, tests)) + "/assets/game_fixes.ini";
#endif
}

void TestConfigReaders() {
	using namespace game_fixes;

	const auto tree = LoadKv1File(TestConfigPath());
	Check(tree.has_value(), "the shipped game_fixes.ini parses with the plugin's own KV1 reader");
	if (!tree) {
		return;
	}
	Check(tree->Children().size() == config::kFixNames.size() + 1,
		  "the shipped config has one block per fix plus server_list_players");

	for (const char* name : config::kFixNames) {
		const KvNode* block = tree->Find(name);
		Check(block != nullptr, std::string("the shipped config has the block for ") + name);
		if (block == nullptr) {
			continue;
		}
		Check(block->Find("enable") != nullptr, std::string(name) + " has an \"enable\" key");
		const config::FixBlock fix = config::ReadFixBlock(*tree, name);
		Check(fix.options == block, std::string("ReadFixBlock finds the block of ") + name);
		const bool shouldBeOn = std::string_view(name) != "cstv_bot_quota" && std::string_view(name) != "jump_spam";
		Check(fix.enabled == shouldBeOn,
			  std::string(name) + (shouldBeOn ? " is enabled by default" : " is off by default"));
	}
	Check(config::ReadFixBlock(*tree, config::kServerListPlayersBlock).enabled,
		  "the shipped config enables server_list_players (the plugin logs that it is not ported)");
	Check(config::ReadFixBlock(*tree, "no_such_fix").options == nullptr,
		  "ReadFixBlock reports a missing block");
	Check(!config::ReadFixBlock(*tree, "no_such_fix").enabled, "a missing block is disabled");

	// The documented values of the shipped file.
	const config::FixBlock slow = config::ReadFixBlock(*tree, "slow_animation");
	if (slow.options != nullptr) {
		const config::SlowAnimationOptions options = config::ReadSlowAnimation(*slow.options);
		Check(options.reloadInterval == 1800.0f, "slow_animation reload_interval is 1800 s in the shipped file");
		Check(options.watch.maxMapHours == 3.0f, "slow_animation max_map_hours is 3 in the shipped file");
		Check(options.watch.matchWaitMinutes == 120.0f, "slow_animation match_wait_minutes is 120");
		Check(options.watch.warnMinutes == 2.0f, "slow_animation warn_minutes is 2");
	}
	const config::FixBlock keyvalues = config::ReadFixBlock(*tree, "keyvalues_flood");
	if (keyvalues.options != nullptr) {
		const config::KeyValuesFloodOptions options = config::ReadKeyValuesFlood(*keyvalues.options);
		Check(options.maxBytes == 1500 && options.maxPerSecond == 10 && options.burst == 20 && !options.kick,
			  "keyvalues_flood keeps its shipped 1500/10/20/kick 0");
	}
	const config::FixBlock voice = config::ReadFixBlock(*tree, "voice_flood");
	if (voice.options != nullptr) {
		const config::VoiceFloodOptions options = config::ReadVoiceFlood(*voice.options);
		Check(options.maxPackets == 64 && options.maxBytes == 4096 && options.maxPerSecond == 128 &&
				  options.cooldown == 5.0f && options.kick && options.kickStrikes == 3,
			  "voice_flood keeps its shipped 64/4096/128/5/kick 1/3");
	}
	const config::FixBlock teamSwitch = config::ReadFixBlock(*tree, "team_switch_spam");
	if (teamSwitch.options != nullptr) {
		const config::TeamSwitchSpamOptions options = config::ReadTeamSwitchSpam(*teamSwitch.options);
		Check(options.maxAttempts == 15 && options.windowSeconds == 10.0,
			  "team_switch_spam keeps its shipped 15 attempts / 10 s");
	}
	const config::FixBlock steamBan = config::ReadFixBlock(*tree, "steam_ban");
	if (steamBan.options != nullptr) {
		const config::SteamBanOptions options = config::ReadSteamBan(*steamBan.options);
		Check(options.clearAfterPass, "steam_ban ships clear_after_pass 1");
		Check(options.whitelist.empty(), "the shipped steam_ban whitelist is empty");
	}

	// Missing keys read the built-in defaults the fix classes always had.
	const KvNode empty;
	const config::SlowAnimationOptions slowDefaults = config::ReadSlowAnimation(empty);
	Check(slowDefaults.reloadInterval == 1800.0f && slowDefaults.watch.maxMapHours == 3.0f &&
			  slowDefaults.watch.matchWaitMinutes == 120.0f && slowDefaults.watch.warnMinutes == 2.0f,
		  "a missing slow_animation block reads 1800 s / 3 h / 120 min / 2 min");
	const config::KeyValuesFloodOptions keyvaluesDefaults = config::ReadKeyValuesFlood(empty);
	Check(keyvaluesDefaults.maxBytes == 1500 && keyvaluesDefaults.maxPerSecond == 10 &&
			  keyvaluesDefaults.burst == 20 && !keyvaluesDefaults.kick,
		  "a missing keyvalues_flood block reads 1500/10/20/kick 0");
	const config::VoiceFloodOptions voiceDefaults = config::ReadVoiceFlood(empty);
	Check(voiceDefaults.maxPackets == 64 && voiceDefaults.maxBytes == 4096 && voiceDefaults.maxPerSecond == 128 &&
			  voiceDefaults.cooldown == 5.0f && voiceDefaults.kick && voiceDefaults.kickStrikes == 3,
		  "a missing voice_flood block reads 64/4096/128/5/kick 1/3");
	const config::TeamSwitchSpamOptions teamSwitchDefaults = config::ReadTeamSwitchSpam(empty);
	Check(teamSwitchDefaults.maxAttempts == 15 && teamSwitchDefaults.windowSeconds == 10.0,
		  "a missing team_switch_spam block reads 15 attempts / 10 s");
	const config::SteamBanOptions steamBanDefaults = config::ReadSteamBan(empty);
	Check(steamBanDefaults.clearAfterPass && steamBanDefaults.whitelist.empty(),
		  "a missing steam_ban block reads clear_after_pass 1 and no whitelist");

	// The floors the fix classes applied are kept (and a NaN never reaches the scheduler).
	const auto clamped = ParseKv1("\"opt\" { \"reload_interval\" \"1\" \"max_map_hours\" \"0.25\" "
								  "\"match_wait_minutes\" \"-5\" \"warn_minutes\" \"-6\" }");
	Check(clamped.has_value(), "the clamp fixture parses");
	if (clamped) {
		const config::SlowAnimationOptions options = config::ReadSlowAnimation(*clamped);
		Check(options.reloadInterval == 60.0f, "reload_interval is floored at 60 s");
		Check(options.watch.maxMapHours == 1.0f, "max_map_hours in (0, 1) is raised to 1 h");
		Check(options.watch.matchWaitMinutes == 0.0f && options.watch.warnMinutes == 0.0f,
			  "negative wait/warn minutes become 0");
	}
	const auto clampedKeyValues = ParseKv1("\"kf\" { \"max_bytes\" \"10\" \"max_per_second\" \"0\" "
										   "\"burst\" \"0\" \"kick\" \"1\" }");
	if (clampedKeyValues) {
		const config::KeyValuesFloodOptions options = config::ReadKeyValuesFlood(*clampedKeyValues);
		Check(options.maxBytes == 256 && options.maxPerSecond == 1 && options.burst == 1 && options.kick,
			  "keyvalues_flood floors are 256 bytes / 1 per second / burst 1, kick is read");
	}
	const auto clampedVoice = ParseKv1("\"vf\" { \"max_packets\" \"1\" \"max_bytes\" \"1\" "
									   "\"max_per_second\" \"1\" \"cooldown\" \"-3\" \"kick\" \"0\" "
									   "\"kick_strikes\" \"0\" }");
	if (clampedVoice) {
		const config::VoiceFloodOptions options = config::ReadVoiceFlood(*clampedVoice);
		Check(options.maxPackets == 16 && options.maxBytes == 256 && options.maxPerSecond == 64 &&
				  options.cooldown == 0.0f && !options.kick && options.kickStrikes == 1,
			  "voice_flood floors are 16 packets / 256 bytes / 64 per second / cooldown 0 / 1 strike");
	}
	const auto clampedTeamSwitch = ParseKv1("\"t\" { \"max_attempts\" \"0\" \"window_seconds\" \"0\" }");
	if (clampedTeamSwitch) {
		const config::TeamSwitchSpamOptions options = config::ReadTeamSwitchSpam(*clampedTeamSwitch);
		Check(options.maxAttempts == 1 && options.windowSeconds == 0.5,
			  "team_switch_spam floors are 1 attempt / 0.5 s");
	}
	const auto whitelist = ParseKv1("\"steam_ban\" { \"clear_after_pass\" \"0\" \"whitelist\" { "
									"\"Slynx\" \"76561198000000000\" \"bad\" \"nope\" } }");
	if (whitelist) {
		const config::SteamBanOptions options = config::ReadSteamBan(*whitelist);
		Check(!options.clearAfterPass, "clear_after_pass 0 is read");
		Check(options.whitelist.size() == 2, "every whitelist child is read");
		if (options.whitelist.size() == 2) {
			Check(options.whitelist[0].label == "Slynx" &&
					  options.whitelist[0].steamId64 == "76561198000000000",
				  "a whitelist entry keeps its label and SteamID64");
			Check(options.whitelist[1].label == "bad" && options.whitelist[1].steamId64 == "nope",
				  "a non-SteamID64 entry is read and rejected by the fix (the reader does not filter)");
		}
	}
}

// ---------------------------------------------------------------- paths --------

void TestPaths() {
	using namespace game_fixes;

	Check(JoinPath("a", "b") == "a/b", "JoinPath joins two fragments");
	Check(JoinPath("a/", "b") == "a/b", "JoinPath does not double the separator");
	Check(JoinPath("a", "/b") == "a/b", "JoinPath strips a leading separator of the second part");
	Check(JoinPath("", "b") == "b", "JoinPath of an empty left side is the right side");
	Check(JoinPath("a", "") == "a", "JoinPath of an empty right side is the left side");

	const std::vector<std::string> candidates = ConfigCandidates("game_fixes.ini", "/cfg", "/data");
	Check(candidates.size() == 4, "the config has four lookup candidates");
	if (candidates.size() == 4) {
		Check(candidates[0] == "game_fixes.ini", "the bare file name is tried first (working directory)");
		Check(candidates[1] == "/cfg/game_fixes/game_fixes.ini",
			  "the packaged <configs>/game_fixes/game_fixes.ini is second");
		Check(candidates[2] == "/cfg/game_fixes/translations/game_fixes.ini",
			  "the shim's translations directory is third");
		Check(candidates[3] == "/data/game_fixes/game_fixes.ini", "the data directory is last");
	}
	const std::vector<std::string> relative = ConfigCandidates("game_fixes.ini", "", "");
	Check(relative.size() == 4 && relative[1] == "game_fixes/game_fixes.ini",
		  "without host directories the candidates stay relative");
	Check(ConfigCandidates("", "/cfg", "/data").empty(), "an empty file name has no candidates");

	// ResolveConfigPath: the packaged location is the fallback for the error log, an existing file
	// wins over it (in either the configs or the data directory).
	const std::filesystem::path root =
		std::filesystem::temp_directory_path() / ("game_fixes_paths_tests_" + std::to_string(::getpid()));
	std::error_code error;
	std::filesystem::remove_all(root, error);
	std::filesystem::create_directories(root / "configs" / "game_fixes", error);
	const std::string configsDir = (root / "configs").string();
	const std::string dataDir = (root / "data").string();

	const std::string fallback = ResolveConfigPath("game_fixes.ini", configsDir, dataDir);
	Check(fallback == JoinPath(JoinPath(configsDir, "game_fixes"), "game_fixes.ini"),
		  "with no file on disk the packaged path is returned (for the log line)");

	const std::string packaged = JoinPath(JoinPath(configsDir, "game_fixes"), "game_fixes.ini");
	{
		std::ofstream file(packaged);
		file << "// placeholder\n";
	}
	Check(FileExists(packaged), "the temp config file exists");
	Check(ResolveConfigPath("game_fixes.ini", configsDir, dataDir) == packaged,
		  "an existing packaged config is found");
	Check(ResolveConfigPath(packaged, "/nonexistent", "/nonexistent") == packaged,
		  "an absolute path that exists is used as given");
	std::filesystem::remove_all(root, error);
}

// -------------------------------------------------------------- map gate ------

void TestMapGate() {
	using game_fixes::MapGate;

	MapGate gate;
	Check(!gate.Ready() && !gate.BetweenMaps() && !gate.ListenersRegistered(),
		  "a fresh gate is shut and has no listeners");
	Check(!gate.OnPluginUpdate(true), "without listeners an update cannot open the gate");
	Check(!gate.Ready(), "the gate stays shut without listeners");

	gate.SetListenersRegistered(true);
	Check(!gate.OnPluginUpdate(false), "an update without a map name does not open the gate");
	Check(!gate.Ready(), "the gate is still shut after an update without a map");

	// A plugin loaded (or reloaded) into a running map never saw OnMapStart: its first update is
	// the "a map is up" evidence.
	Check(gate.OnPluginUpdate(true), "the first update with a map name opens the gate (mid-map load)");
	Check(gate.Ready(), "the gate is open after that update");
	Check(!gate.OnPluginUpdate(true), "a later update does not report another transition");

	gate.OnMapEnd();
	Check(!gate.Ready() && gate.BetweenMaps(), "OnMapEnd shuts the gate and marks the map-change window");
	Check(!gate.OnPluginUpdate(true), "no update re-opens the gate between maps");
	Check(!gate.Ready(), "the gate stays shut between maps");
	gate.OnMapStart();
	Check(!gate.Ready() && !gate.BetweenMaps(), "OnMapStart does not latch readiness itself");
	Check(gate.OnPluginUpdate(true), "the first update after OnMapStart opens the gate");
	Check(gate.Ready(), "the gate is open on the new map");

	gate.Close();
	Check(!gate.Ready(), "Close shuts the gate for unload");

	// A server that never has a map: Start + updates without a map name never open it.
	MapGate noMap;
	noMap.SetListenersRegistered(true);
	noMap.OnMapStart();
	Check(!noMap.OnPluginUpdate(false), "without a map name the gate never opens");
	Check(!noMap.Ready(), "a map-less server keeps every client call off");

	// A failed listener registration leaves the gate shut forever (fail closed).
	MapGate noListeners;
	noListeners.OnMapStart();
	Check(!noListeners.OnPluginUpdate(true), "the gate needs registered listeners");
	Check(!noListeners.Ready(), "the gate stays shut when the listeners could not be registered");

	// Reset (= OnPluginStart / `plg reload`) forgets everything, including a previously open gate.
	MapGate reloaded;
	reloaded.SetListenersRegistered(true);
	reloaded.OnPluginUpdate(true);
	reloaded.Reset();
	Check(!reloaded.Ready() && !reloaded.BetweenMaps() && !reloaded.ListenersRegistered(),
		  "Reset shuts a fresh instance completely");
}

// ----------------------------------------------------------- schema cache -----

int g_lookupCalls = 0;

std::int32_t FakeSchemaLookup(const char* className, const char* fieldName) {
	++g_lookupCalls;
	if (className == nullptr || fieldName == nullptr) {
		return -1;
	}
	if (std::string_view(className) == "Missing") {
		return -1;
	}
	return 123;
}

void TestSchemaCache() {
	using game_fixes::SchemaOffsetCache;

	SchemaOffsetCache cache;
	g_lookupCalls = 0;
	Check(cache.Offset(false, "CBaseEntity", "m_iHealth", &FakeSchemaLookup) == -1,
		  "a shut map gate answers -1");
	Check(g_lookupCalls == 0, "a shut map gate never calls the schema lookup");
	Check(cache.Offset(true, "CBaseEntity", "m_iHealth", &FakeSchemaLookup) == 123,
		  "an open gate resolves the offset");
	Check(g_lookupCalls == 1, "the lookup ran once");
	Check(cache.Offset(true, "CBaseEntity", "m_iHealth", &FakeSchemaLookup) == 123,
		  "a cached offset is served again");
	Check(g_lookupCalls == 1, "a cached hit does not call the lookup again");
	Check(cache.CachedCount() == 1, "one successful offset is cached");
	Check(cache.Offset(true, "Missing", "m_X", &FakeSchemaLookup) == -1, "an unknown field answers -1");
	Check(cache.Offset(true, "Missing", "m_X", &FakeSchemaLookup) == -1, "an unknown field is retried");
	Check(g_lookupCalls == 3, "a failed lookup is not cached (only < 0 hits stay retryable)");
	Check(cache.CachedCount() == 1, "a failed lookup does not grow the cache");
	Check(cache.Offset(true, "", "m_X", &FakeSchemaLookup) == -1, "an empty class name answers -1");
	Check(cache.Offset(true, "CBaseEntity", "", &FakeSchemaLookup) == -1, "an empty field name answers -1");
	Check(cache.Offset(true, "CBaseEntity", "m_iHealth", nullptr) == -1, "a missing lookup answers -1");
	Check(cache.Offset(false, "CBaseEntity", "m_iHealth", &FakeSchemaLookup) == -1,
		  "the shut gate hides even a cached offset");
	Check(cache.Offset(true, "CBaseEntity", "m_iHealth", &FakeSchemaLookup) == 123,
		  "the offset is still remembered when the gate reopens");
	Check(g_lookupCalls == 3, "the cached offset survived the shut gate");
	cache.Clear();
	Check(cache.CachedCount() == 0, "Clear forgets every offset (OnPluginEnd)");
	Check(cache.Offset(true, "CBaseEntity", "m_iHealth", &FakeSchemaLookup) == 123 && g_lookupCalls == 4,
		  "after Clear the field is resolved again");
}

// ------------------------------------------- scheduler: delayed work, floor ----

void TestSchedulerDelayedWorkAndFloor() {
	using namespace game_fixes::scheduler;

	// PITFALLS C.9: no delay below one tick reaches a timer, whatever the caller passed.
	Shutdown();
	Init();
	const float minimum = static_cast<float>(kMinTimerInterval);
	Timer* zero = AddTimer(0.0f, []() {}, kTimerFlagRepeat);
	Timer* negative = AddTimer(-5.0f, []() {}, kTimerFlagRepeat);
	Timer* notANumber = AddTimer(std::numeric_limits<float>::quiet_NaN(), []() {}, 0);
	Check(zero->interval == minimum, "a 0-delay timer is clamped to one 64-tick");
	Check(negative->interval == minimum, "a negative delay is clamped to one 64-tick");
	Check(notANumber->interval == minimum, "a NaN delay is clamped to one 64-tick");

	// OnMapEnd drops the per-map delayed work: a deferred kick / the blank_map restart must not run
	// on the new map, while the periodic checks (repeat timers) keep running.
	Shutdown();
	Init();
	Tick(0.0); // the first tick after Init only arms the clock
	int onceFires = 0;
	int repeatFires = 0;
	int frameFires = 0;
	AddTimer(0.02f, [&onceFires]() { ++onceFires; }, 0);
	Timer* repeat = AddTimer(0.02f, [&repeatFires]() { ++repeatFires; }, kTimerFlagRepeat);
	NextFrame([&frameFires]() { ++frameFires; });
	ClearDelayedWork();
	for (int i = 0; i < 20; ++i) {
		Tick(0.02);
	}
	Check(onceFires == 0, "ClearDelayedWork drops one-shot timers (they belong to the old map)");
	Check(frameFires == 0, "ClearDelayedWork drops queued next-frame work");
	Check(repeatFires >= 3 && repeatFires <= 5, "ClearDelayedWork keeps repeating timers (they check the map gate)");
	KillTimer(repeat);

	// The next-frame queue works again after the map end (the new map can schedule its own work).
	int afterMapEnd = 0;
	NextFrame([&afterMapEnd]() { ++afterMapEnd; });
	Tick(0.02);
	Check(afterMapEnd == 1, "a next-frame task queued after a map end runs");

	Shutdown();
	Init();
}

} // namespace

int main() {
	std::printf("game_fixes core logic tests\n");
	TestBanMapNeutralize();
	TestBanMapRemoveAll();
	TestScheduler();
	TestSchedulerDelayedWorkAndFloor();
	TestKv1();
	TestConfigReaders();
	TestPaths();
	TestMapGate();
	TestSchemaCache();
	TestSlowAnimationGhostFilter();
	TestSlowAnimationConfig();
	TestSlowAnimationHumanStamp();
	TestSlowAnimationWatch();
	TestTeamLimitFields();
	TestSvCheatsLogic();
	TestVoiceFixLogic();
	TestPlayerGhostLogic();
	TestFakeMessagesLogic();
	TestBlankMapLogic();
	TestCstvLogic();
	TestJumpSpamLogic();
	TestLoadPolicy();
	TestEncodedIndex();
	TestHandleValidity();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
