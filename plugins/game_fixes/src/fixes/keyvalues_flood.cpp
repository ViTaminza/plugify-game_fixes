// keyvalues_flood — a client floods clc_CmdKeyValues with oversized payloads and the server
// spends the frame decoding them. CServerSideClient::ProcessCmdKeyValues is where the engine
// takes the message; payloads over max_bytes and messages past the per-second bucket (a token
// bucket with burst) are swallowed there. The byte cap also bounds how many nodes and how deep a
// tree the payload can hold, so no parser is needed. With "kick" the sender is removed too.
//
// The payload size is read straight out of the protobuf object the way the fork's compiled
// accessor does (see voice_flood.cpp for the untag/follow/length-before-data read); the field
// offset is in assets/gamedata.jsonc.

#include "../fixes.hpp"
#include "../scheduler.hpp"

#include <pisex/compat.hpp>
#include <pisex/keyvalues.hpp>

namespace game_fixes {
namespace {

constexpr std::intptr_t kStringLengthOffset = 0x18;

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

class CKeyValuesFloodFix final : public CFix {
public:
	explicit CKeyValuesFloodFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "keyvalues_flood"; }

	void ReadConfig(const pisex::KeyValues& config) override {
		m_maxBytes = config.GetInt("max_bytes", 1500);
		m_perSecond = config.GetInt("max_per_second", 10);
		m_burst = config.GetInt("burst", 20);
		m_kick = config.GetBool("kick", false);
		// Below these a normal client would start getting cut: the engine only sends userinfo
		// keyvalues, a handful of small ones per connect.
		if (m_maxBytes < 256) m_maxBytes = 256;
		if (m_perSecond < 1) m_perSecond = 1;
		if (m_burst < 1) m_burst = 1;
	}

	bool Load() override;
	void Unload() override;

private:
	struct ClientState {
		int userId = -1;
		double lastRefill = 0.0;
		double tokens = 0.0;
		bool caught = false;
	};

	static polyhook::ResultType ProcessCmdKeyValuesPre(polyhook::HookHandle hook, polyhook::ParametersHandle params,
													   int32_t count, polyhook::ReturnHandle ret,
													   polyhook::CallbackType type);

	void Refuse(void* client, ClientState& state, const char* reason);

	static CKeyValuesFloodFix* s_instance;

	ClientState m_clients[64];
	std::int32_t m_maxBytes = 1500;
	std::int32_t m_perSecond = 10;
	std::int32_t m_burst = 20;
	bool m_kick = false;
	hook::HookRef m_hook;
};

CKeyValuesFloodFix* CKeyValuesFloodFix::s_instance = nullptr;

polyhook::ResultType CKeyValuesFloodFix::ProcessCmdKeyValuesPre(polyhook::HookHandle, polyhook::ParametersHandle params,
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
			state.tokens = static_cast<double>(fix->m_burst);
			state.lastRefill = s2sdk::GetEngineTime();
		}

		const char* reason = nullptr;

		if (fix->m_env.offCmdKeyValuesData >= 0) {
			const auto bytes = ReadStringLength(static_cast<const std::uint8_t*>(message) +
												fix->m_env.offCmdKeyValuesData);
			if (bytes > fix->m_maxBytes) {
				reason = "keyvalues payload over max_bytes";
			}
		}

		if (!reason) {
			const double now = s2sdk::GetEngineTime();
			const double elapsed = now - state.lastRefill;
			if (elapsed > 0.0) {
				state.tokens += elapsed * static_cast<double>(fix->m_perSecond);
				if (state.tokens > static_cast<double>(fix->m_burst)) {
					state.tokens = static_cast<double>(fix->m_burst);
				}
				state.lastRefill = now;
			}
			if (state.tokens < 1.0) {
				reason = "keyvalues messages over max_per_second";
			} else {
				state.tokens -= 1.0;
			}
		}

		// Once caught, nothing more from this client is let through, even the well-formed
		// messages it mixes in, until it is gone.
		if (!reason && !state.caught) {
			return polyhook::ResultType::Ignored;
		}
		if (reason && !state.caught) {
			fix->Refuse(client, state, reason);
		}

		// Swallowed: the original is skipped and its return set to true (what the fork's
		// Supersede hands the engine), so a skipped call never returns a stale bool.
		SetSupersededReturn(ret);
		return polyhook::ResultType::Supercede;
	} catch (...) {
		return polyhook::ResultType::Ignored;
	}
}

void CKeyValuesFloodFix::Refuse(void* client, ClientState& state, const char* reason) {
	state.caught = true;

	const int slot = Int32At(client, m_env.offClientSlot);
	const int userId = state.userId;

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
	LogFix(Name(), Format("%s (slot %d, %llu): %s%s", who.c_str(), slot, static_cast<unsigned long long>(steamId),
						  reason, m_kick ? ", kicking" : ", dropping their keyvalues"));

	if (!m_kick) {
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
			s2sdk::KickClient(slot, s2sdk::NetworkDisconnectionReason::Kicked, plg::string("keyvalues flood"));
		} catch (...) {
		}
	});
}

bool CKeyValuesFloodFix::Load() {
	if (!m_env.vtableServerSideClient || m_env.idxProcessCmdKeyValues < 0) {
		LogFix(Name(), "CServerSideClient vtable or ProcessCmdKeyValues index unavailable", plg::Severity::Warning);
		return false;
	}
	s_instance = this;
	const plg::vector<polyhook::DataType> args{polyhook::DataType::Pointer, polyhook::DataType::Pointer};
	m_hook = hook::InstallVFunc(m_env.vtableServerSideClient, m_env.idxProcessCmdKeyValues, polyhook::DataType::Bool,
								args, &CKeyValuesFloodFix::ProcessCmdKeyValuesPre, nullptr, "GameFixes_keyvalues_flood");
	if (!m_hook) {
		s_instance = nullptr;
		LogFix(Name(), "hooking CServerSideClient::ProcessCmdKeyValues failed", plg::Severity::Warning);
		return false;
	}
	LogFix(Name(), Format("hooked CServerSideClient::ProcessCmdKeyValues on vtable %p (max_bytes %d, max_per_second %d, "
						  "burst %d, kick %d)",
						  m_env.vtableServerSideClient, m_maxBytes, m_perSecond, m_burst, m_kick));
	return true;
}

void CKeyValuesFloodFix::Unload() {
	hook::RemoveCallback(m_hook, polyhook::CallbackType::Pre, &CKeyValuesFloodFix::ProcessCmdKeyValuesPre);
	hook::UnhookVFunc(m_hook, m_env.vtableServerSideClient, m_env.idxProcessCmdKeyValues);
	m_hook = {};
	s_instance = nullptr;
}

} // namespace

std::unique_ptr<CFix> MakeKeyValuesFloodFix(Environment& env) {
	return std::make_unique<CKeyValuesFloodFix>(env);
}

} // namespace game_fixes
