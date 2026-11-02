// voice_fix — the client side of the reference VoiceFix (Source2ZE/CS2VoiceFix): a client voice
// message that carries no `audio` field is malformed for what the engine and the other clients
// expect, and the reference drops it outright. Some clients send those packets, and the listener
// on the other end can then stop hearing the speaker.
//
// s2sdk has no client-to-server net-message hook, so this uses the same
// CServerSideClient::ProcessVoiceData vtable hook as voice_flood: the callback gets the raw
// CCLCMsg_VoiceData object and `_has_bits_` bit 0 is the protobuf's "audio present" flag. The
// hook is shared through polyhook (FindVirtual2 + AddCallback), so voice_flood and voice_fix can
// both be attached, in any enable combination. The flood policy is untouched.
//
// The protobuf offsets come from assets/gamedata.jsonc (verified against the live build). This
// fix only needs the has-bits offset; the audio pointer offset is voice_flood's business.

#include "../fixes.hpp"
#include "../voice_fix_logic.hpp"

namespace game_fixes {
namespace {

class CVoiceFix final : public CFix {
public:
	explicit CVoiceFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "voice_fix"; }

	bool Load() override;
	void Unload() override;

private:
	static polyhook::ResultType ProcessVoiceDataPre(polyhook::HookHandle hook, polyhook::ParametersHandle params,
													int32_t count, polyhook::ReturnHandle ret,
													polyhook::CallbackType type);

	static CVoiceFix* s_instance;
	hook::HookRef m_hook;
};

CVoiceFix* CVoiceFix::s_instance = nullptr;

polyhook::ResultType CVoiceFix::ProcessVoiceDataPre(polyhook::HookHandle, polyhook::ParametersHandle params, int32_t,
													polyhook::ReturnHandle ret, polyhook::CallbackType) {
	try {
		auto* fix = s_instance;
		if (!fix || fix->m_env.offVoiceHasBits < 0) {
			return polyhook::ResultType::Ignored;
		}
		// Argument 1 is the CCLCMsg_VoiceData object (argument 0 is the CServerSideClient).
		const auto* message = static_cast<const std::uint8_t*>(polyhook::GetArgumentPointer(params, 1));
		if (!message) {
			return polyhook::ResultType::Ignored;
		}
		const auto hasBits =
			*reinterpret_cast<const std::uint32_t*>(message + fix->m_env.offVoiceHasBits);
		if (!voicefix::ShouldDrop(hasBits)) {
			return polyhook::ResultType::Ignored;
		}
		// ProcessVoiceData returns bool; "handled" (true) is what the engine reads for a message
		// the hook wanted gone (the same value voice_flood hands back when it swallows one).
		SetSupersededReturn(ret, true);
		return polyhook::ResultType::Supercede;
	} catch (...) {
		return polyhook::ResultType::Ignored;
	}
}

bool CVoiceFix::Load() {
	if (!m_env.vtableServerSideClient || m_env.idxProcessVoiceData < 0) {
		LogFix(Name(), "CServerSideClient vtable or ProcessVoiceData index unavailable", plg::Severity::Warning);
		return false;
	}
	if (m_env.offVoiceHasBits < 0) {
		LogFix(Name(), "CCLCMsg_VoiceData has-bits offset is missing from the gamedata", plg::Severity::Warning);
		return false;
	}
	s_instance = this;
	const plg::vector<polyhook::DataType> args{polyhook::DataType::Pointer, polyhook::DataType::Pointer};
	m_hook = hook::InstallVFunc(m_env.vtableServerSideClient, m_env.idxProcessVoiceData, polyhook::DataType::Bool, args,
								&CVoiceFix::ProcessVoiceDataPre, nullptr, "GameFixes_voice_fix");
	if (!m_hook) {
		s_instance = nullptr;
		LogFix(Name(), "hooking CServerSideClient::ProcessVoiceData failed", plg::Severity::Warning);
		return false;
	}
	LogFix(Name(), "dropping client voice messages without an audio field");
	return true;
}

void CVoiceFix::Unload() {
	hook::RemoveCallback(m_hook, polyhook::CallbackType::Pre, &CVoiceFix::ProcessVoiceDataPre);
	hook::UnhookVFunc(m_hook, m_env.vtableServerSideClient, m_env.idxProcessVoiceData);
	m_hook = {};
	s_instance = nullptr;
}

} // namespace

std::unique_ptr<CFix> MakeVoiceFix(Environment& env) {
	return std::make_unique<CVoiceFix>(env);
}

} // namespace game_fixes
