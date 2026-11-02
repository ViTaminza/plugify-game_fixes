// fake_messages — advertising bots connect just long enough to post a chat line, without ever
// joining the game or appearing on the scoreboard. Chat is only let through from clients whose
// signon state has reached SPAWN (the reference FakeMessagesFix's InGame latch); every other
// client command is untouched, and the server / GOTV (slot -1) always passes.
//
// s2sdk has no chat hook, but every client string command is dispatched through
// IServerGameClients::ClientCommand and ICvar::DispatchConCommand, and the global command listener
// (empty name, Pre) sees all of them. Returning Handled makes s2sdk Supercede the dispatch, so a
// blocked `say` never reaches the chat relay.
//
// State: signon updates come from OnClientSignonStateChanged and OnClientPutInServer (both s2sdk
// listeners); a player who was already in game when the plugin loaded (or across a changelevel)
// is seeded from CServerSideClient::m_nSignonState with a 1 s repeat timer that stops after the
// first successful pass. No client is queried before the map is up (the map gate, src/map_gate.hpp).

#include "../fake_messages_logic.hpp"
#include "../fixes.hpp"
#include "../map_gate.hpp"
#include "../scheduler.hpp"

namespace game_fixes {
namespace {

constexpr int kMaxSlots = 64;

class CFakeMessagesFix final : public CFix {
public:
	explicit CFakeMessagesFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "fake_messages"; }

	bool Load() override;
	void Unload() override;
	void OnStartupServer(const char* mapName) override;

private:
	static s2sdk::ResultType OnGameCommand(int32_t slot, s2sdk::ConCommandContext context,
										   const plg::vector<plg::string>& args);
	static void OnSignonStateChanged(int32_t slot, s2sdk::SignonState state, int32_t spawnCount);
	static void OnClientPutInServer(int32_t slot);
	static void OnClientDisconnect(int32_t slot, s2sdk::NetworkDisconnectionReason reason);

	void ArmSeed();
	void SeedFromSignon();

	static CFakeMessagesFix* s_instance;
	bool m_listenerRegistered = false;
	// One flag per client listener: each is unregistered independently, so a failure in the middle
	// of the set can never leave a callback pointing into this library (PITFALLS A.7).
	bool m_signonListener = false;
	bool m_putInServerListener = false;
	bool m_disconnectListener = false;
	bool m_inGame[kMaxSlots] = {};
	scheduler::Timer* m_seedTimer = nullptr;
	bool m_loggedBlock = false;
};

CFakeMessagesFix* CFakeMessagesFix::s_instance = nullptr;

s2sdk::ResultType CFakeMessagesFix::OnGameCommand(int32_t slot, s2sdk::ConCommandContext,
												  const plg::vector<plg::string>& args) {
	try {
		auto* fix = s_instance;
		if (!fix || slot < 0 || slot >= kMaxSlots || args.empty()) {
			return s2sdk::ResultType::Continue;
		}
		if (!fakemessages::IsChatCommand(std::string_view(args[0].c_str(), args[0].size()))) {
			return s2sdk::ResultType::Continue;
		}
		if (fix->m_inGame[slot]) {
			return s2sdk::ResultType::Continue;
		}
		if (!fix->m_loggedBlock) {
			fix->m_loggedBlock = true;
			LogFix(fix->Name(), Format("blocked chat from slot %d (client has not joined the game)", slot));
		}
		// Handled (not Stop): the global listener sets the result, s2sdk then supersedes the
		// command dispatch, and the chat is never relayed.
		return s2sdk::ResultType::Handled;
	} catch (...) {
		return s2sdk::ResultType::Continue;
	}
}

void CFakeMessagesFix::OnSignonStateChanged(int32_t slot, s2sdk::SignonState state, int32_t) {
	try {
		auto* fix = s_instance;
		if (!fix || slot < 0 || slot >= kMaxSlots) {
			return;
		}
		fix->m_inGame[slot] = fakemessages::SignonAllowsChat(static_cast<std::int32_t>(state));
	} catch (...) {
	}
}

void CFakeMessagesFix::OnClientPutInServer(int32_t slot) {
	try {
		auto* fix = s_instance;
		if (fix && slot >= 0 && slot < kMaxSlots) {
			fix->m_inGame[slot] = true;
		}
	} catch (...) {
	}
}

void CFakeMessagesFix::OnClientDisconnect(int32_t slot, s2sdk::NetworkDisconnectionReason) {
	try {
		auto* fix = s_instance;
		if (fix && slot >= 0 && slot < kMaxSlots) {
			fix->m_inGame[slot] = false;
		}
	} catch (...) {
	}
}

void CFakeMessagesFix::ArmSeed() {
	if (m_seedTimer) {
		return;
	}
	// Timers with interval 0 are forbidden (the engine watchdog); 1 s is also enough for a bot
	// that only connects to post a line, because the signon listener reports it before then.
	m_seedTimer = scheduler::AddTimer(1.0f, [this]() { SeedFromSignon(); }, scheduler::kTimerFlagRepeat);
}

void CFakeMessagesFix::SeedFromSignon() {
	try {
		// No client queries before the map is up (s2sdk walks a null/emptied client list).
		if (!IsMapReady()) {
			return;
		}
		if (__s2sdk_PlayerSlotToClientPtr && __s2sdk_GetMaxClients && m_env.offSignonState >= 0) {
			const int maxClients = s2sdk::GetMaxClients();
			for (int slot = 0; slot < kMaxSlots; ++slot) {
				void* client = slot < maxClients ? s2sdk::PlayerSlotToClientPtr(slot) : nullptr;
				m_inGame[slot] =
					client != nullptr &&
					fakemessages::SignonAllowsChat(Int32At(client, m_env.offSignonState));
			}
		}
		if (m_seedTimer) {
			scheduler::KillTimer(m_seedTimer);
			m_seedTimer = nullptr;
		}
	} catch (...) {
	}
}

bool CFakeMessagesFix::Load() {
	if (!__s2sdk_AddCommandListener || !__s2sdk_RemoveCommandListener || !__s2sdk_PlayerSlotToClientPtr) {
		LogFix(Name(), "command listener/player bindings are missing", plg::Severity::Warning);
		return false;
	}
	if (m_env.offSignonState < 0) {
		LogFix(Name(), "CServerSideClientBase m_nSignonState offset is missing from the gamedata",
			   plg::Severity::Warning);
		return false;
	}
	s_instance = this;
	try {
		// Empty name = global listener: sees every dispatched client command, has no name that a
		// reload could collide with, and does not depend on `say` being a registered ConCommand.
		m_listenerRegistered = s2sdk::AddCommandListener(plg::string(), &CFakeMessagesFix::OnGameCommand,
														 s2sdk::HookMode::Pre);
	} catch (...) {
		m_listenerRegistered = false;
	}
	if (!m_listenerRegistered) {
		s_instance = nullptr;
		LogFix(Name(), "registering the command listener failed", plg::Severity::Warning);
		return false;
	}
	const bool haveClientListeners =
		__s2sdk_OnClientSignonStateChanged_Register && __s2sdk_OnClientSignonStateChanged_Unregister &&
		__s2sdk_OnClientPutInServer_Register && __s2sdk_OnClientPutInServer_Unregister &&
		__s2sdk_OnClientDisconnect_Register && __s2sdk_OnClientDisconnect_Unregister;
	if (haveClientListeners) {
		try {
			s2sdk::OnClientSignonStateChanged_Register(&CFakeMessagesFix::OnSignonStateChanged);
			m_signonListener = true;
		} catch (...) {
		}
		try {
			s2sdk::OnClientPutInServer_Register(&CFakeMessagesFix::OnClientPutInServer);
			m_putInServerListener = true;
		} catch (...) {
		}
		try {
			s2sdk::OnClientDisconnect_Register(&CFakeMessagesFix::OnClientDisconnect);
			m_disconnectListener = true;
		} catch (...) {
		}
	}
	if (!m_signonListener || !m_putInServerListener || !m_disconnectListener) {
		// Fail soft, but the signon latch is what makes the block correct; without it only the
		// periodic seed (stopped after the first pass) would keep the state fresh.
		LogFix(Name(), "client signon listeners are missing; using the periodic signon seed only",
			   plg::Severity::Warning);
	}
	ArmSeed();
	LogFix(Name(), Format("blocking chat from clients below signon %d", fakemessages::kSignonSpawn));
	return true;
}

void CFakeMessagesFix::Unload() {
	if (m_seedTimer) {
		scheduler::KillTimer(m_seedTimer);
		m_seedTimer = nullptr;
	}
	if (m_signonListener) {
		m_signonListener = false;
		if (__s2sdk_OnClientSignonStateChanged_Unregister) {
			try {
				s2sdk::OnClientSignonStateChanged_Unregister(&CFakeMessagesFix::OnSignonStateChanged);
			} catch (...) {
			}
		}
	}
	if (m_putInServerListener) {
		m_putInServerListener = false;
		if (__s2sdk_OnClientPutInServer_Unregister) {
			try {
				s2sdk::OnClientPutInServer_Unregister(&CFakeMessagesFix::OnClientPutInServer);
			} catch (...) {
			}
		}
	}
	if (m_disconnectListener) {
		m_disconnectListener = false;
		if (__s2sdk_OnClientDisconnect_Unregister) {
			try {
				s2sdk::OnClientDisconnect_Unregister(&CFakeMessagesFix::OnClientDisconnect);
			} catch (...) {
			}
		}
	}
	if (m_listenerRegistered) {
		try {
			s2sdk::RemoveCommandListener(plg::string(), &CFakeMessagesFix::OnGameCommand, s2sdk::HookMode::Pre);
		} catch (...) {
		}
		m_listenerRegistered = false;
	}
	s_instance = nullptr;
}

void CFakeMessagesFix::OnStartupServer(const char*) {
	// New map: re-seed the latch after the server is up; players may already be connected when
	// this plugin loaded mid-map. The signon listeners keep it current afterwards.
	m_loggedBlock = false;
	ArmSeed();
}

} // namespace

std::unique_ptr<CFix> MakeFakeMessagesFix(Environment& env) {
	return std::make_unique<CFakeMessagesFix>(env);
}

} // namespace game_fixes
