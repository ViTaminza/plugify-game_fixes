// Pure-logic tests for plugins/game_fixes:
//
//   * ban_map.hpp — the exact memory writes the steam_ban fix makes in the engine's ban map
//     (entry neutralization and the RemoveAll free-list rebuild). The container itself is the
//     engine's; what can be tested here is that the fix writes only inside the node slots, that
//     the free list it builds covers every slot exactly once, and that the header ends in the
//     engine's empty state. A stray write here corrupts live server memory, so the fixtures put
//     canaries around both the header and the node array.
//
//   * scheduler.cpp — the timer/next-frame pump the slow_animation reload and the deferred kicks
//     run on: a repeat timer fires on its interval (not per tick), KillTimer from inside a
//     callback stops it, NextFrame runs exactly once, Shutdown drops what is queued.
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

// ----------------------------------------------------------- load policy --------

void TestLoadPolicy() {
	using game_fixes::DecideLoad;
	using game_fixes::LoadDecision;

	// A fix that needs the schema may never be loaded before the offsets resolved: calling into
	// the schema system at plugin start segfaults live (CollectSchemaFields on a null class-info
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
	// The live-server case: index 93, serial 102949 -> 0xC912805D = -921534371, a valid handle the
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
	TestLoadPolicy();
	TestEncodedIndex();
	TestHandleValidity();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
