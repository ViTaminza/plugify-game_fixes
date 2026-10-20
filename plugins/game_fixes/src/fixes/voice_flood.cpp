// voice_flood — the "server lagger": a client floods clc_VoiceData (hundreds of messages a tick,
// empty voice, thousands of packet_offsets) and the server lags copying each one to every
// listener. In CServerSideClient::ProcessVoiceData a message with more packet_offsets than
// max_packets (or more packet_offsets than it has voice bytes), or a client past max_per_second
// messages, is swallowed and the client's voice is dropped for cooldown seconds; a client caught
// kick_strikes times (each within a minute of the last) is kicked.
//
// The message is a protobuf object (CCLCMsg_VoiceData inside the CNetMessage wrapper). Its field
// offsets come from the fork's own build (DWARF and disassembly of its ProcessVoiceData handler)
// and are stored in assets/gamedata.jsonc; the string lengths are read the way the fork's compiled code
// reads them (untag the field pointer, follow it, the length sits 0x18 bytes before the data).

#include "../fixes.hpp"
#include "../scheduler.hpp"

#include <pisex/compat.hpp>
#include <pisex/keyvalues.hpp>

namespace game_fixes {
namespace {

// A strike this long after the previous one starts the count again.
constexpr double kStrikeMemory = 60.0;
// The fork reads std::string lengths at data - 0x18 (its own inlined protobuf accessor); the
// same constant is what the game's own protobuf build puts there.
constexpr std::intptr_t kStringLengthOffset = 0x18;

// Reads a protobuf `bytes`/`string` field's length out of the field storage at `location`,
// exactly like the engine's inlined size() does (untag, follow, length before the data).
std::int64_t ReadStringLength(const void* location) {
	const auto tagged = *reinterpret_cast<const std::uintptr_t*>(location);
	const auto pointer = reinterpret_cast<const std::uintptr_t*>(tagged & ~static_cast<std::uintptr_t>(3));
	if (pointer == nullptr) {
		return 0;
	}
	const auto data = *pointer;
	if (data <= static_cast<std::uintptr_t>(kStringLengthOffset)) {
		return 0;
	}
	return static_cast<std::int64_t>(*reinterpret_cast<const std::int32_t*>(data - kStringLengthOffset));
}

class CVoiceFloodFix final : public CFix {
public:
	explicit CVoiceFloodFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "voice_flood"; }

	void ReadConfig(const pisex::KeyValues& config) override {
		m_maxPackets = config.GetInt("max_packets", 64);
		m_maxBytes = config.GetInt("max_bytes", 4096);
		m_maxPerSecond = config.GetInt("max_per_second", 128);
		m_cooldown = config.GetFloat("cooldown", 5.0f);
		m_kick = config.GetBool("kick", true);
		m_kickStrikes = config.GetInt("kick_strikes", 3);
		if (m_maxPackets < 16) m_maxPackets = 16;
		if (m_maxBytes < 256) m_maxBytes = 256;
		if (m_maxPerSecond < 64) m_maxPerSecond = 64;
		if (m_cooldown < 0.0f) m_cooldown = 0.0f;
		if (m_kickStrikes < 1) m_kickStrikes = 1;
	}

	bool Load() override;
	void Unload() override;

private:
	struct ClientState {
		int userId = -1;
		double windowStart = 0.0;
		int inWindow = 0;
		double caughtUntil = 0.0;
		int strikes = 0;
		double lastStrike = 0.0;
	};

	static polyhook::ResultType ProcessVoiceDataPre(polyhook::HookHandle hook, polyhook::ParametersHandle params,
													int32_t count, polyhook::ReturnHandle ret,
													polyhook::CallbackType type);

	// Why a voice message is refused, or null when it may go through.
	const char* CheckVoiceData(const void* message) const;
	void Refuse(void* client, ClientState& state, const char* reason, double now);

	static CVoiceFloodFix* s_instance;

	ClientState m_clients[64];
	std::int32_t m_maxPackets = 64;
	std::int32_t m_maxBytes = 4096;
	std::int32_t m_maxPerSecond = 128;
	float m_cooldown = 5.0f;
	bool m_kick = true;
	std::int32_t m_kickStrikes = 3;
	hook::HookRef m_hook;
};

CVoiceFloodFix* CVoiceFloodFix::s_instance = nullptr;

const char* CVoiceFloodFix::CheckVoiceData(const void* message) const {
	const auto& env = m_env;
	if (env.offVoiceHasBits < 0 || env.offVoiceAudio < 0 || env.offVoicePacketOffsets < 0 ||
		env.offVoiceVoiceData < 0) {
		return nullptr; // no layout: the rate limit still applies
	}
	// has_audio: _has_bits_ bit 0 of CCLCMsg_VoiceData (field 1); a message without audio is
	// nothing to check.
	const auto hasBits = *reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(message) +
																 env.offVoiceHasBits);
	if ((hasBits & 1u) == 0) {
		return nullptr;
	}
	const auto* audio = *reinterpret_cast<const std::uint8_t* const*>(static_cast<const std::uint8_t*>(message) +
																	 env.offVoiceAudio);
	if (!audio) {
		return nullptr; // the protobuf default instance has no packets and no bytes
	}
	const auto packets =
		*reinterpret_cast<const std::int32_t*>(audio + env.offVoicePacketOffsets);
	if (packets > m_maxPackets) {
		return "too many packet_offsets";
	}
	const auto bytes = ReadStringLength(audio + env.offVoiceVoiceData);
	if (bytes > m_maxBytes) {
		return "voice_data over max_bytes";
	}
	if (packets > bytes) {
		return "more packet_offsets than voice_data bytes";
	}
	return nullptr;
}

polyhook::ResultType CVoiceFloodFix::ProcessVoiceDataPre(polyhook::HookHandle, polyhook::ParametersHandle params,
														 int32_t, polyhook::ReturnHandle ret,
														 polyhook::CallbackType) {
	try {
		auto* fix = s_instance;
		if (!fix) {
			return polyhook::ResultType::Ignored;
		}
		auto* client = polyhook::GetArgumentPointer(params, 0);
		auto* message = polyhook::GetArgumentPointer(params, 1);
		if (!client || !message || fix->m_env.offClientSlot < 0 || fix->m_env.offUserId < 0) {
			return polyhook::ResultType::Ignored;
		}
		const int slot = Int32At(client, fix->m_env.offClientSlot);
		if (slot < 0 || slot >= 64) {
			return polyhook::ResultType::Ignored;
		}
		ClientState& state = fix->m_clients[slot];

		const int userId = Int32At(client, fix->m_env.offUserId);
		if (state.userId != userId) {
			state = ClientState();
			state.userId = userId;
		}

		const double now = s2sdk::GetEngineTime();

		// Caught: everything is swallowed, well-formed messages it mixes in too, until the
		// cooldown runs out.
		if (state.caughtUntil != 0.0) {
			if (fix->m_cooldown == 0.0f || now < state.caughtUntil) {
				SetSupersededReturn(ret);
				return polyhook::ResultType::Supercede;
			}
			state.caughtUntil = 0.0;
			state.windowStart = now;
			state.inWindow = 0;
		}

		const char* reason = fix->CheckVoiceData(message);
		if (!reason) {
			if (now - state.windowStart >= 1.0) {
				state.windowStart = now;
				state.inWindow = 0;
			}
			if (++state.inWindow > fix->m_maxPerSecond) {
				reason = "voice messages over max_per_second";
			}
		}

		if (!reason) {
			return polyhook::ResultType::Ignored;
		}

		fix->Refuse(client, state, reason, now);

		// Swallowed: the original is skipped and the return is set to true, the value the fork's
		// Supersede hands the engine ("handled"), so a skipped call never returns a stale bool.
		SetSupersededReturn(ret);
		return polyhook::ResultType::Supercede;
	} catch (...) {
		return polyhook::ResultType::Ignored;
	}
}

void CVoiceFloodFix::Refuse(void* client, ClientState& state, const char* reason, double now) {
	state.caughtUntil = now + (m_cooldown > 0.0f ? m_cooldown : 1.0);

	if (now - state.lastStrike > kStrikeMemory) {
		state.strikes = 0;
	}
	++state.strikes;
	state.lastStrike = now;

	const int slot = Int32At(client, m_env.offClientSlot);
	const int userId = state.userId;
	const bool kick = m_kick && state.strikes >= m_kickStrikes;

	// The name/steam id are PlayerManager lookups; during shutdown they walk the emptied client
	// list (s2sdk does not validate that).
	std::string who = "(unknown)";
	if (__s2sdk_GetClientName && pisex::IsGameReady()) {
		try {
			who = std::string(std::string_view(s2sdk::GetClientName(slot)));
		} catch (...) {
		}
	}
	const auto steamId = (__s2sdk_GetClientSteamID64 && pisex::IsGameReady()) ? s2sdk::GetClientSteamID64(slot) : 0;

	LogFix(Name(), Format("%s (slot %d, %llu): %s (%d this second), strike %d/%d, %s", who.c_str(), slot,
						  static_cast<unsigned long long>(steamId), reason, state.inWindow, state.strikes,
						  m_kickStrikes,
						  kick ? "kicking"
							   : (m_cooldown > 0.0f ? "dropping their voice for a while" : "dropping their voice")));

	if (!kick) {
		return;
	}

	// Not from inside the client's own message processing: next frame, and only if a map is still
	// running and the slot holds the same client.
	const std::int32_t userIdOffset = m_env.offUserId;
	scheduler::NextFrame([slot, userId, userIdOffset]() {
		if (!__s2sdk_PlayerSlotToClientPtr || !__s2sdk_KickClient) {
			return;
		}
		// Map shutdown/changelevel empties the client list; the s2sdk kick path does not check.
		if (!pisex::IsGameReady()) {
			return;
		}
		void* current = s2sdk::PlayerSlotToClientPtr(slot);
		if (!current || Int32At(current, userIdOffset) != userId) {
			return;
		}
		try {
			s2sdk::KickClient(slot, s2sdk::NetworkDisconnectionReason::Kicked, plg::string("voice flood"));
		} catch (...) {
		}
	});
}

bool CVoiceFloodFix::Load() {
	if (!m_env.vtableServerSideClient || m_env.idxProcessVoiceData < 0) {
		LogFix(Name(), "CServerSideClient vtable or ProcessVoiceData index unavailable", plg::Severity::Warning);
		return false;
	}
	s_instance = this;
	const plg::vector<polyhook::DataType> args{polyhook::DataType::Pointer, polyhook::DataType::Pointer};
	m_hook = hook::InstallVFunc(m_env.vtableServerSideClient, m_env.idxProcessVoiceData, polyhook::DataType::Bool, args,
								&CVoiceFloodFix::ProcessVoiceDataPre, nullptr, "GameFixes_voice_flood");
	if (!m_hook) {
		s_instance = nullptr;
		LogFix(Name(), "hooking CServerSideClient::ProcessVoiceData failed", plg::Severity::Warning);
		return false;
	}
	LogFix(Name(), Format("hooked CServerSideClient::ProcessVoiceData on vtable %p (max_packets %d, max_bytes %d, "
						  "max_per_second %d, cooldown %.1f, kick %d after %d strike(s))",
						  m_env.vtableServerSideClient, m_maxPackets, m_maxBytes, m_maxPerSecond, m_cooldown, m_kick,
						  m_kickStrikes));
	return true;
}

void CVoiceFloodFix::Unload() {
	hook::RemoveCallback(m_hook, polyhook::CallbackType::Pre, &CVoiceFloodFix::ProcessVoiceDataPre);
	hook::UnhookVFunc(m_hook, m_env.vtableServerSideClient, m_env.idxProcessVoiceData);
	m_hook = {};
	s_instance = nullptr;
}

} // namespace

std::unique_ptr<CFix> MakeVoiceFloodFix(Environment& env) {
	return std::make_unique<CVoiceFloodFix>(env);
}

} // namespace game_fixes
