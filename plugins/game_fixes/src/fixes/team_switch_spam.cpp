// team_switch_spam — a client spamming jointeam / spectate makes the server walk the team-change
// path (and its plugins) every time. Past max_attempts in window_seconds the client is kicked.
//
// The fork counted the raw clc_StringCmd messages in CServerSideClient::ProcessStringCmd. This
// port counts the *dispatched* jointeam/spectate commands instead: identifying them inside the
// protobuf message would need the message ABI, while s2sdk's command manager sees the dispatch of
// every client command. jointeam/spectate are NOT ConCommands in CS2 (the engine parses them in
// its ClientCommand path), so they are hooked through the GLOBAL command listener (empty name,
// filtered by name here) - the same mechanism pisexcompat uses for its game commands: a named
// AddConsoleCommand would shadow the engine's command (the client then gets "not marked
// FCVAR_CLIENT_CAN_EXECUTE") and a named hook could not be re-created after `plg reload` because
// the name stays taken. Unrecognized command spam — which the engine's own command rate limiter
// already covers — is the one case the fork saw and this port does not.

#include "../fixes.hpp"
#include "../scheduler.hpp"

#include <pisex/compat.hpp>
#include <pisex/keyvalues.hpp>

namespace game_fixes {
namespace {

// The two commands the fix counts (the engine's lookup is case-insensitive, so this compare is
// too; both names are eight characters, so the size check pins the full token).
bool IsRateLimitedCommand(std::string_view name) {
	return (name.size() == 8 && EqualsIgnoreCaseN(name, "jointeam", 8)) ||
		   (name.size() == 8 && EqualsIgnoreCaseN(name, "spectate", 8));
}

class CTeamSwitchSpamFix final : public CFix {
public:
	explicit CTeamSwitchSpamFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "team_switch_spam"; }

	void ReadConfig(const pisex::KeyValues& config) override {
		m_maxAttempts = config.GetInt("max_attempts", 15);
		m_windowSeconds = static_cast<double>(config.GetFloat("window_seconds", 10.0f));
		if (m_maxAttempts < 1) m_maxAttempts = 1;
		if (m_windowSeconds < 0.5) m_windowSeconds = 0.5;
	}

	bool Load() override;
	void Unload() override;

private:
	struct ClientState {
		int userId = -1;
		double windowStart = 0.0;
		int inWindow = 0;
		bool kicked = false;
	};

	// Callback of the global command listener: fires for every dispatched client command.
	static s2sdk::ResultType OnGameCommand(int32_t caller, s2sdk::ConCommandContext context,
										   const plg::vector<plg::string>& args);
	void Count(void* client);
	void Refuse(void* client, ClientState& state);

	static CTeamSwitchSpamFix* s_instance;

	ClientState m_clients[64];
	std::int32_t m_maxAttempts = 15;
	double m_windowSeconds = 10.0;
	bool m_listenerRegistered = false;
};

CTeamSwitchSpamFix* CTeamSwitchSpamFix::s_instance = nullptr;

s2sdk::ResultType CTeamSwitchSpamFix::OnGameCommand(int32_t caller, s2sdk::ConCommandContext,
													const plg::vector<plg::string>& args) {
	try {
		auto* fix = s_instance;
		if (fix && caller >= 0 && !args.empty() && __s2sdk_PlayerSlotToClientPtr) {
			const std::string_view name(args[0].c_str());
			if (IsRateLimitedCommand(name)) {
				fix->Count(s2sdk::PlayerSlotToClientPtr(caller));
			}
		}
	} catch (...) {
	}
	// The command itself still goes through: this is a rate limit, not a filter, and the kick is
	// the whole punishment.
	return s2sdk::ResultType::Continue;
}

void CTeamSwitchSpamFix::Count(void* client) {
	if (!client || m_env.offClientSlot < 0 || m_env.offUserId < 0) {
		return;
	}
	const int slot = Int32At(client, m_env.offClientSlot);
	if (slot < 0 || slot >= 64) {
		return;
	}
	ClientState& state = m_clients[slot];

	const int userId = Int32At(client, m_env.offUserId);
	if (state.userId != userId) {
		state = ClientState();
		state.userId = userId;
	}

	const double now = s2sdk::GetEngineTime();
	if (state.windowStart <= 0.0 || now - state.windowStart >= m_windowSeconds) {
		state.windowStart = now;
		state.inWindow = 0;
	}

	if (++state.inWindow > m_maxAttempts && !state.kicked) {
		Refuse(client, state);
	}
}

void CTeamSwitchSpamFix::Refuse(void* client, ClientState& state) {
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
	LogFix(Name(), Format("%s (slot %d, %llu): more than %d team commands in %.1fs, kicking", who.c_str(), slot,
						  static_cast<unsigned long long>(steamId), m_maxAttempts, m_windowSeconds));

	// Once kicked, its window is restarted so a client that reconnects into the same slot does
	// not inherit the count; a client flooding inside the same frame must not schedule the kick
	// (or log it) again.
	state.kicked = true;
	state.inWindow = 0;
	state.windowStart = s2sdk::GetEngineTime();

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
			s2sdk::KickClient(slot, s2sdk::NetworkDisconnectionReason::Kicked, plg::string("team switch spam"));
		} catch (...) {
		}
	});
}

bool CTeamSwitchSpamFix::Load() {
	if (!__s2sdk_AddCommandListener || !__s2sdk_RemoveCommandListener || !__s2sdk_PlayerSlotToClientPtr) {
		LogFix(Name(), "command listener/player bindings are missing", plg::Severity::Warning);
		return false;
	}
	if (m_env.offClientSlot < 0 || m_env.offUserId < 0) {
		LogFix(Name(), "client slot/userid offsets are missing", plg::Severity::Warning);
		return false;
	}
	s_instance = this;
	try {
		// The global listener (empty name): receives every client command, the trampoline filters
		// jointeam/spectate. It has no name to collide with, so a `plg reload` can always re-add it.
		m_listenerRegistered = s2sdk::AddCommandListener(plg::string(), &CTeamSwitchSpamFix::OnGameCommand,
														 s2sdk::HookMode::Pre);
	} catch (...) {
		m_listenerRegistered = false;
	}
	if (!m_listenerRegistered) {
		s_instance = nullptr;
		LogFix(Name(), "adding the global client-command listener failed", plg::Severity::Warning);
		return false;
	}
	LogFix(Name(), Format("counting jointeam/spectate through the global client-command listener (max %d team commands in %.1fs)",
						  m_maxAttempts, m_windowSeconds));
	return true;
}

void CTeamSwitchSpamFix::Unload() {
	if (m_listenerRegistered && __s2sdk_RemoveCommandListener) {
		s2sdk::RemoveCommandListener(plg::string(), &CTeamSwitchSpamFix::OnGameCommand, s2sdk::HookMode::Pre);
	}
	m_listenerRegistered = false;
	s_instance = nullptr;
}

} // namespace

std::unique_ptr<CFix> MakeTeamSwitchSpamFix(Environment& env) {
	return std::make_unique<CTeamSwitchSpamFix>(env);
}

} // namespace game_fixes
