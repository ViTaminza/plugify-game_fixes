// game_fixes — the plugin's option readers and their defaults, kept out of the fix classes so the
// config contract can be pinned by tests (tests/core_logic_tests.cpp) without an engine.
//
// Every fix's ReadConfig() used to parse its block with pisexcompat's KeyValues; the plugin now
// reads the same file with its own KV1 reader (src/kv1.hpp) and these helpers, and the defaults /
// floors below are exactly the ones the fix classes carried before the pisexcompat removal:
//
//   keyvalues_flood   max_bytes 1500 (floor 256), max_per_second 10 (floor 1), burst 20 (floor 1),
//                     kick 0
//   team_switch_spam  max_attempts 15 (floor 1), window_seconds 10 (floor 0.5)
//   voice_flood       max_packets 64 (floor 16), max_bytes 4096 (floor 256), max_per_second 128
//                     (floor 64), cooldown 5 (floor 0), kick 1, kick_strikes 3 (floor 1)
//   slow_animation    reload_interval 1800 s (floor 60), max_map_hours 3 h, match_wait_minutes 120,
//                     warn_minutes 2 (the last three clamped by slowanim::ClampConfig)
//   steam_ban         clear_after_pass 1, whitelist = "Label" "SteamID64" pairs
//
// A fix block that is missing, or has no "enable" key, counts as disabled - exactly like the
// original loader.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kv1.hpp"
#include "slow_animation_logic.hpp"

namespace game_fixes::config {

// The plugin's config file name and directory (<configs>/game_fixes/), PLUGIN_STANDARD §3.
inline constexpr std::string_view kFileName = "game_fixes.ini";

// Every fix the plugin creates, in load order (plugin.cpp's Make*Factory calls, the shipped
// game_fixes.ini's blocks and the tests all follow this list).
inline constexpr std::array<const char*, 20> kFixNames = {{
	"beam_crash",
	"demo_record",
	"hammer_id",
	"slow_animation",
	"workshop_voice",
	"voice_fix",
	"steam_ban",
	"team_limit",
	"input_activator_crash",
	"sv_cheats",
	"rampbug",
	"water_jump",
	"keyvalues_flood",
	"team_switch_spam",
	"voice_flood",
	"blank_map",
	"player_ghost",
	"fake_messages",
	"cstv_bot_quota",
	"jump_spam",
}};

// The one block in the shipped file that is not a fix: s2sdk already pushes the player list to the
// Steam game server API, so the plugin only tells the operator it is not ported (plugin.cpp).
inline constexpr std::string_view kServerListPlayersBlock = "server_list_players";

// A fix block: whether it is enabled, and the block itself (nullptr when the block is missing).
struct FixBlock {
	bool enabled = false;
	const KvNode* options = nullptr;
};

FixBlock ReadFixBlock(const KvNode& root, std::string_view name);

// --- per-fix options ------------------------------------------------------------------------

struct SlowAnimationOptions {
	float reloadInterval = 1800.0f; // seconds between empty-server checks (floor 60)
	slowanim::Config watch;			// max_map_hours / match_wait_minutes / warn_minutes, clamped
};

SlowAnimationOptions ReadSlowAnimation(const KvNode& config);

struct SteamBanWhitelistEntry {
	std::string label;
	std::string steamId64;
};

struct SteamBanOptions {
	bool clearAfterPass = true;
	std::vector<SteamBanWhitelistEntry> whitelist;
};

SteamBanOptions ReadSteamBan(const KvNode& config);

struct KeyValuesFloodOptions {
	std::int32_t maxBytes = 1500;
	std::int32_t maxPerSecond = 10;
	std::int32_t burst = 20;
	bool kick = false;
};

KeyValuesFloodOptions ReadKeyValuesFlood(const KvNode& config);

struct VoiceFloodOptions {
	std::int32_t maxPackets = 64;
	std::int32_t maxBytes = 4096;
	std::int32_t maxPerSecond = 128;
	float cooldown = 5.0f;
	bool kick = true;
	std::int32_t kickStrikes = 3;
};

VoiceFloodOptions ReadVoiceFlood(const KvNode& config);

struct TeamSwitchSpamOptions {
	std::int32_t maxAttempts = 15;
	double windowSeconds = 10.0;
};

TeamSwitchSpamOptions ReadTeamSwitchSpam(const KvNode& config);

} // namespace game_fixes::config
