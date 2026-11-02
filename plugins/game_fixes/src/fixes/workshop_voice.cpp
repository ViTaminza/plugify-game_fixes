// workshop_voice — clients key voice playback off the xuid in svc_VoiceData; on workshop maps it
// does not identify the speaker uniquely, streams collide and players stop hearing each other.
// CServerSideClient::SendNetMessage runs once per recipient, so every speaker gets a distinct
// xuid per listener, rewritten in place before the engine serializes the message. Seeds reset on
// every map change.
//
// The fork hooked the CServerSideClient vtable itself; s2sdk already has that hook (it drives its
// own OnSendNetMessage listener), so this fix registers *that* listener instead of adding a second
// hook — the callback runs in the same Pre window, once per recipient, and the s2sdk UserMessage
// gives the player slot, the svc_VoiceData id, the speaker entity and a writable xuid without any
// engine ABI work.

#include "../fixes.hpp"
#include "../map_gate.hpp"
#include "../scheduler.hpp"

#include <random>

namespace game_fixes {
namespace {

constexpr int kMaxSlots = 64;

// How long after a map start the registry lookup is made. The lookup cannot run in the
// StartupServer Pre callback (the engine registers its network messages later in that startup, and
// not at all at plugin load), so it is retried once the map is running.
constexpr float kRegistryRetryDelay = 1.0f;

class CWorkshopVoiceFix final : public CFix {
public:
	explicit CWorkshopVoiceFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "workshop_voice"; }

	bool Load() override;
	void Unload() override;
	void OnStartupServer(const char* mapName) override;
	void OnMapEnd() override;

private:
	static void OnSendNetMessage(int32_t playerSlot, void* userMessage);

	// True when the message is svc_VoiceData (its id: the gamedata constant, refreshed by the
	// registry lookup at every map start — at plugin load the engine has not registered its user
	// messages yet, so the lookup cannot be the only source).
	bool IsVoiceData(void* userMessage) const;

	// The registry lookup, made once the map is up (see kRegistryRetryDelay).
	void LookUpMessageId();

	uint64_t SeedForRecipient(int slot);

	static CWorkshopVoiceFix* s_instance;

	// Keyed by the slot of the client the voice packet is being sent *to*; the speaker's entity
	// index is added on top. Seeds are handed out 66 apart -- wider than any entity index -- so
	// two listeners' xuid windows can never overlap. Keying on the listener is what makes slot
	// reuse harmless: a new client in an old slot has nothing cached, so the mapping it inherits
	// is as good as a fresh one.
	std::uint64_t m_playerSeeds[kMaxSlots] = {};
	std::uint64_t m_seedCursor = 0;
	bool m_loggedFirstRewrite = false;

	// svc_VoiceData's message id; starts at the gamedata protocol constant and is refreshed by
	// the registry lookup at every map start (never a per-message lookup: the listener runs for
	// every outgoing message on the game thread).
	int32_t m_voiceDataId = 0;
	// The registry lookup is retried at every map start until it answers (it logs a warning when
	// the name is not registered); one successful lookup is enough for the process.
	bool m_registryChecked = false;
	// The one-shot lookup scheduled by the last OnStartupServer; killed on unload.
	scheduler::Timer* m_registryTimer = nullptr;
};

CWorkshopVoiceFix* CWorkshopVoiceFix::s_instance = nullptr;

bool CWorkshopVoiceFix::IsVoiceData(void* userMessage) const {
	if (m_voiceDataId <= 0 || !__s2sdk_UserMessageGetMessageID) {
		return false;
	}
	return s2sdk::UserMessageGetMessageID(userMessage) == m_voiceDataId;
}

void CWorkshopVoiceFix::OnSendNetMessage(int32_t playerSlot, void* userMessage) {
	try {
		auto* fix = s_instance;
		if (!fix || !userMessage || playerSlot < 0 || playerSlot >= kMaxSlots) {
			return;
		}
		if (!__s2sdk_PbGetInt32 || !__s2sdk_PbSetUInt64) {
			return;
		}
		if (!fix->IsVoiceData(userMessage)) {
			return;
		}

		int32_t speaker = 0;
		if (!s2sdk::PbGetInt32(userMessage, "entity", speaker)) {
			return;
		}

		const std::uint64_t xuid = fix->SeedForRecipient(playerSlot) + static_cast<std::uint64_t>(speaker);
		if (!s2sdk::PbSetUInt64(userMessage, "xuid", xuid)) {
			return;
		}

		if (!fix->m_loggedFirstRewrite) {
			fix->m_loggedFirstRewrite = true;
			LogFix(fix->Name(),
				   Format("rewriting svc_VoiceData xuid, first packet was entity %d -> slot %d", speaker, playerSlot));
		}
	} catch (...) {
	}
}

std::uint64_t CWorkshopVoiceFix::SeedForRecipient(int slot) {
	if (slot < 0 || slot >= kMaxSlots) {
		return 0;
	}
	if (m_playerSeeds[slot] == 0) {
		if (m_seedCursor == 0) {
			std::random_device rd;
			std::mt19937_64 gen(rd());
			m_seedCursor = gen() | 1;
		}
		m_seedCursor += 66;
		m_playerSeeds[slot] = m_seedCursor;
	}
	return m_playerSeeds[slot];
}

bool CWorkshopVoiceFix::Load() {
	if (!__s2sdk_OnSendNetMessage_Register || !__s2sdk_UserMessageGetMessageID || !__s2sdk_PbGetInt32 ||
		!__s2sdk_PbSetUInt64) {
		LogFix(Name(), "s2sdk OnSendNetMessage/UserMessage bindings are missing", plg::Severity::Warning);
		return false;
	}
	// The engine has not registered its user messages at plugin load (the lookup would fail and
	// warn), so this starts from the protocol constant in the gamedata and the registry replaces
	// it at the first map start.
	m_voiceDataId = m_env.offVoiceDataMessageId > 0 ? m_env.offVoiceDataMessageId : 47;
	s_instance = this;
	try {
		s2sdk::OnSendNetMessage_Register(&CWorkshopVoiceFix::OnSendNetMessage);
	} catch (...) {
		s_instance = nullptr;
		LogFix(Name(), "registering the OnSendNetMessage listener failed", plg::Severity::Warning);
		return false;
	}
	LogFix(Name(), Format("rewriting svc_VoiceData xuids through the s2sdk OnSendNetMessage listener "
						  "(message id %d; the registry lookup refines it once a map is up)",
						  m_voiceDataId));
	return true;
}

void CWorkshopVoiceFix::Unload() {
	scheduler::KillTimer(m_registryTimer);
	m_registryTimer = nullptr;
	if (__s2sdk_OnSendNetMessage_Unregister) {
		s2sdk::OnSendNetMessage_Unregister(&CWorkshopVoiceFix::OnSendNetMessage);
	}
	s_instance = nullptr;
}

void CWorkshopVoiceFix::OnStartupServer(const char* mapName) {
	(void)mapName;
	std::memset(m_playerSeeds, 0, sizeof(m_playerSeeds));
	m_loggedFirstRewrite = false;

	// The registry cannot be asked yet: this runs in the StartupServer Pre callback, before the
	// engine (and s2sdk) register their network messages. Retry once the map is live; a failed
	// lookup is not latched, so the next map start tries again.
	if (m_registryChecked) {
		return;
	}
	scheduler::KillTimer(m_registryTimer);
	m_registryTimer = scheduler::AddTimer(kRegistryRetryDelay, [this]() {
		m_registryTimer = nullptr;
		LookUpMessageId();
	});
}

void CWorkshopVoiceFix::OnMapEnd() {
	// The registry lookup is per-map delayed work. Dropping it here (before the plugin's final
	// scheduler sweep) keeps the member from pointing at a timer the sweep would delete, and the
	// next map's StartupServer arms it again (PITFALLS B2: no client is touched).
	scheduler::KillTimer(m_registryTimer);
	m_registryTimer = nullptr;
}

void CWorkshopVoiceFix::LookUpMessageId() {
	if (m_registryChecked || !__s2sdk_UserMessageFindMessageIdByName || !IsMapReady()) {
		return;
	}
	const int32_t registered = s2sdk::UserMessageFindMessageIdByName("svc_VoiceData");
	if (registered <= 0) {
		return; // still not registered: the next map start has another go
	}
	m_registryChecked = true;
	if (registered != m_voiceDataId) {
		LogFix(Name(), Format("svc_VoiceData message id from the registry: %d (was %d)", registered,
							  m_voiceDataId));
		m_voiceDataId = registered;
	}
}

} // namespace

std::unique_ptr<CFix> MakeWorkshopVoiceFix(Environment& env) {
	return std::make_unique<CWorkshopVoiceFix>(env);
}

} // namespace game_fixes
