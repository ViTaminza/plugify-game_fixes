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
//     schedule, and the curtime-reset check after a forced reload.
//
//   * hook::EncodedIndex — the Itanium ABI member-pointer encoding every vtable hook of this
//     plugin is keyed by (1 + 8 * index). A wrong value hooks the wrong function.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include "ban_map.hpp"
#include "load_policy.hpp"
#include "scheduler.hpp"
#include "slow_animation_logic.hpp"

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

} // namespace

int main() {
	std::printf("game_fixes core logic tests\n");
	TestBanMapNeutralize();
	TestBanMapRemoveAll();
	TestScheduler();
	TestSlowAnimationGhostFilter();
	TestSlowAnimationConfig();
	TestSlowAnimationWatch();
	TestLoadPolicy();
	TestEncodedIndex();
	TestHandleValidity();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
