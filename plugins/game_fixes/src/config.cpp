#include "config.hpp"

namespace game_fixes::config {

FixBlock ReadFixBlock(const KvNode& root, std::string_view name) {
	FixBlock block;
	if (name.empty()) {
		return block;
	}
	block.options = root.Find(name);
	if (block.options == nullptr) {
		return block;
	}
	block.enabled = block.options->GetBool("enable");
	return block;
}

SlowAnimationOptions ReadSlowAnimation(const KvNode& config) {
	SlowAnimationOptions options;
	options.reloadInterval = config.GetFloat("reload_interval", 1800.0f);
	// Anything shorter than a minute would only keep flipping the map around; the negated compare
	// also catches a NaN, which must never reach the scheduler.
	if (!(options.reloadInterval >= 60.0f)) {
		options.reloadInterval = 60.0f;
	}
	options.watch = slowanim::ClampConfig(config.GetFloat("max_map_hours", 3.0f),
										  config.GetFloat("match_wait_minutes", 120.0f),
										  config.GetFloat("warn_minutes", 2.0f));
	return options;
}

SteamBanOptions ReadSteamBan(const KvNode& config) {
	SteamBanOptions options;
	options.clearAfterPass = config.GetBool("clear_after_pass", true);
	if (const KvNode* whitelist = config.Find("whitelist")) {
		for (const KvNode& entry : whitelist->Children()) {
			options.whitelist.push_back({entry.name, std::string(entry.GetString({}))});
		}
	}
	return options;
}

KeyValuesFloodOptions ReadKeyValuesFlood(const KvNode& config) {
	KeyValuesFloodOptions options;
	options.maxBytes = config.GetInt("max_bytes", 1500);
	options.maxPerSecond = config.GetInt("max_per_second", 10);
	options.burst = config.GetInt("burst", 20);
	options.kick = config.GetBool("kick", false);
	// Below these a normal client would start getting cut: the engine only sends userinfo
	// keyvalues, a handful of small ones per connect.
	if (options.maxBytes < 256) {
		options.maxBytes = 256;
	}
	if (options.maxPerSecond < 1) {
		options.maxPerSecond = 1;
	}
	if (options.burst < 1) {
		options.burst = 1;
	}
	return options;
}

VoiceFloodOptions ReadVoiceFlood(const KvNode& config) {
	VoiceFloodOptions options;
	options.maxPackets = config.GetInt("max_packets", 64);
	options.maxBytes = config.GetInt("max_bytes", 4096);
	options.maxPerSecond = config.GetInt("max_per_second", 128);
	options.cooldown = config.GetFloat("cooldown", 5.0f);
	options.kick = config.GetBool("kick", true);
	options.kickStrikes = config.GetInt("kick_strikes", 3);
	if (options.maxPackets < 16) {
		options.maxPackets = 16;
	}
	if (options.maxBytes < 256) {
		options.maxBytes = 256;
	}
	if (options.maxPerSecond < 64) {
		options.maxPerSecond = 64;
	}
	if (!(options.cooldown >= 0.0f)) {
		options.cooldown = 0.0f;
	}
	if (options.kickStrikes < 1) {
		options.kickStrikes = 1;
	}
	return options;
}

TeamSwitchSpamOptions ReadTeamSwitchSpam(const KvNode& config) {
	TeamSwitchSpamOptions options;
	options.maxAttempts = config.GetInt("max_attempts", 15);
	options.windowSeconds = static_cast<double>(config.GetFloat("window_seconds", 10.0f));
	if (options.maxAttempts < 1) {
		options.maxAttempts = 1;
	}
	if (!(options.windowSeconds >= 0.5)) {
		options.windowSeconds = 0.5;
	}
	return options;
}

} // namespace game_fixes::config
