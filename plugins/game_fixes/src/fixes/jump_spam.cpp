// jump_spam — a keybind macro makes the game run CheckJumpButton several times for one move tick
// (the subtick moves of a single usercmd), and every one of those counts as a jump, giving speed
// nobody playing normally can match. The fix allows one jump-button check per move tick:
//
//   * CheckJumpButtonModern/Legacy Pre: if the player already jumped on this CMoveData tick,
//     cancel the original (the reference JumpSpamFix's CancelOriginal),
//   * OnJumpModern/Legacy Post: record the tick of the jump that actually happened.
//
// s2sdk has no movement GameHooks, so all four functions are signature-resolved detours; the
// signatures in assets/gamedata.jsonc were derived from the live libserver.so (the two OnJump
// ones are the CS2AC patterns, the two CheckJumpButton ones are the callers of those functions)
// and each matches exactly once. CMoveData::m_nTickCount is 0xD4 (212).
//
// Slot resolution: CheckJumpButton receives the movement-services pointer directly
// (PlayerServicesToPlayerSlot, like rampbug). OnJump receives an interior pointer into those
// services, so the slot comes from a thread-local set by our ProcessMovement listener — OnJump
// runs inside ProcessMovement on the same thread.
//
// Default off: on bhop/surf servers multiple jump inputs per tick are the point of the mode.

#include "../fixes.hpp"
#include "../jump_spam_logic.hpp"

namespace game_fixes {
namespace {

thread_local int g_currentSlot = -1;

class CJumpSpamFix final : public CFix, public IMovementListener {
public:
	explicit CJumpSpamFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "jump_spam"; }

	bool Load() override;
	void Unload() override;
	void OnStartupServer(const char* mapName) override;

	IMovementListener* Movement() override { return this; }

	polyhook::ResultType OnProcessMovement(void* services, void* move) override;
	polyhook::ResultType OnProcessMovementPost(void* services, void* move) override;

private:
	static polyhook::ResultType CheckJumpPre(polyhook::HookHandle hook, polyhook::ParametersHandle params,
											 int32_t count, polyhook::ReturnHandle ret, polyhook::CallbackType type);
	static polyhook::ResultType OnJumpPost(polyhook::HookHandle hook, polyhook::ParametersHandle params,
										   int32_t count, polyhook::ReturnHandle ret, polyhook::CallbackType type);

	static CJumpSpamFix* s_instance;
	hook::HookRef m_checkLegacy;
	hook::HookRef m_checkModern;
	hook::HookRef m_onJumpLegacy;
	hook::HookRef m_onJumpModern;
	std::int32_t m_lastJumpTick[64];
};

CJumpSpamFix* CJumpSpamFix::s_instance = nullptr;

polyhook::ResultType CJumpSpamFix::CheckJumpPre(polyhook::HookHandle, polyhook::ParametersHandle params, int32_t,
												polyhook::ReturnHandle ret, polyhook::CallbackType) {
	try {
		auto* fix = s_instance;
		if (!fix || fix->m_env.offMoveTickCount < 0 || !__s2sdk_PlayerServicesToPlayerSlot) {
			return polyhook::ResultType::Ignored;
		}
		void* services = polyhook::GetArgumentPointer(params, 0);
		void* move = polyhook::GetArgumentPointer(params, 1);
		if (!services || !move) {
			return polyhook::ResultType::Ignored;
		}
		const int slot = s2sdk::PlayerServicesToPlayerSlot(services);
		if (slot < 0 || slot >= 64) {
			return polyhook::ResultType::Ignored;
		}
		const std::int32_t tick = Int32At(move, fix->m_env.offMoveTickCount);
		if (!jumpspam::ShouldCancelJump(fix->m_lastJumpTick[slot], tick)) {
			return polyhook::ResultType::Ignored;
		}
		// CheckJumpButton returns whether the jump was taken; a cancelled check must not claim it
		// was.
		SetSupersededReturn(ret, false);
		return polyhook::ResultType::Supercede;
	} catch (...) {
		return polyhook::ResultType::Ignored;
	}
}

polyhook::ResultType CJumpSpamFix::OnJumpPost(polyhook::HookHandle, polyhook::ParametersHandle params, int32_t,
											  polyhook::ReturnHandle, polyhook::CallbackType) {
	try {
		auto* fix = s_instance;
		if (!fix || fix->m_env.offMoveTickCount < 0 || g_currentSlot < 0 || g_currentSlot >= 64) {
			return polyhook::ResultType::Ignored;
		}
		// Argument 1 is the CMoveData (argument 0 is the modern/legacy jump struct).
		void* move = polyhook::GetArgumentPointer(params, 1);
		if (!move) {
			return polyhook::ResultType::Ignored;
		}
		fix->m_lastJumpTick[g_currentSlot] = jumpspam::RecordJump(Int32At(move, fix->m_env.offMoveTickCount));
	} catch (...) {
	}
	return polyhook::ResultType::Ignored;
}

polyhook::ResultType CJumpSpamFix::OnProcessMovement(void* services, void*) {
	try {
		if (!__s2sdk_PlayerServicesToPlayerSlot || !services) {
			g_currentSlot = -1;
			return polyhook::ResultType::Ignored;
		}
		const int slot = s2sdk::PlayerServicesToPlayerSlot(services);
		g_currentSlot = (slot >= 0 && slot < 64) ? slot : -1;
	} catch (...) {
		g_currentSlot = -1;
	}
	return polyhook::ResultType::Ignored;
}

polyhook::ResultType CJumpSpamFix::OnProcessMovementPost(void*, void*) {
	g_currentSlot = -1;
	return polyhook::ResultType::Ignored;
}

bool CJumpSpamFix::Load() {
	if (!m_env.fnCheckJumpButtonLegacy || !m_env.fnCheckJumpButtonModern || !m_env.fnOnJumpLegacy ||
		!m_env.fnOnJumpModern) {
		LogFix(Name(), "jump signatures are missing from the gamedata", plg::Severity::Warning);
		return false;
	}
	if (m_env.offMoveTickCount < 0) {
		LogFix(Name(), "CMoveData::m_nTickCount offset is missing from the gamedata", plg::Severity::Warning);
		return false;
	}
	s_instance = this;
	for (std::int32_t& tick : m_lastJumpTick) {
		tick = jumpspam::kNoJumpTick;
	}
	const plg::vector<polyhook::DataType> args{polyhook::DataType::Pointer, polyhook::DataType::Pointer};
	m_checkLegacy = hook::InstallDetour(m_env.fnCheckJumpButtonLegacy, polyhook::DataType::Bool, args,
										&CJumpSpamFix::CheckJumpPre, nullptr, "GameFixes_check_jump_legacy");
	m_checkModern = hook::InstallDetour(m_env.fnCheckJumpButtonModern, polyhook::DataType::Bool, args,
										&CJumpSpamFix::CheckJumpPre, nullptr, "GameFixes_check_jump_modern");
	m_onJumpLegacy = hook::InstallDetour(m_env.fnOnJumpLegacy, polyhook::DataType::Bool, args, nullptr,
										 &CJumpSpamFix::OnJumpPost, "GameFixes_on_jump_legacy");
	m_onJumpModern = hook::InstallDetour(m_env.fnOnJumpModern, polyhook::DataType::Bool, args, nullptr,
										 &CJumpSpamFix::OnJumpPost, "GameFixes_on_jump_modern");
	if (!m_checkLegacy || !m_checkModern || !m_onJumpLegacy || !m_onJumpModern) {
		LogFix(Name(), "one or more jump detours could not be installed", plg::Severity::Warning);
		return false; // plugin calls Unload(), which removes whatever was installed
	}
	LogFix(Name(), Format("hooked jump functions (check %p/%p, onjump %p/%p, tick offset %d)",
						  m_env.fnCheckJumpButtonLegacy, m_env.fnCheckJumpButtonModern, m_env.fnOnJumpLegacy,
						  m_env.fnOnJumpModern, m_env.offMoveTickCount));
	return true;
}

void CJumpSpamFix::Unload() {
	hook::RemoveCallback(m_checkLegacy, polyhook::CallbackType::Pre, &CJumpSpamFix::CheckJumpPre);
	hook::UnhookDetour(m_checkLegacy, m_env.fnCheckJumpButtonLegacy);
	m_checkLegacy = {};
	hook::RemoveCallback(m_checkModern, polyhook::CallbackType::Pre, &CJumpSpamFix::CheckJumpPre);
	hook::UnhookDetour(m_checkModern, m_env.fnCheckJumpButtonModern);
	m_checkModern = {};
	hook::RemoveCallback(m_onJumpLegacy, polyhook::CallbackType::Post, &CJumpSpamFix::OnJumpPost);
	hook::UnhookDetour(m_onJumpLegacy, m_env.fnOnJumpLegacy);
	m_onJumpLegacy = {};
	hook::RemoveCallback(m_onJumpModern, polyhook::CallbackType::Post, &CJumpSpamFix::OnJumpPost);
	hook::UnhookDetour(m_onJumpModern, m_env.fnOnJumpModern);
	m_onJumpModern = {};
	g_currentSlot = -1;
	s_instance = nullptr;
}

void CJumpSpamFix::OnStartupServer(const char*) {
	for (std::int32_t& tick : m_lastJumpTick) {
		tick = jumpspam::kNoJumpTick;
	}
}

} // namespace

std::unique_ptr<CFix> MakeJumpSpamFix(Environment& env) {
	return std::make_unique<CJumpSpamFix>(env);
}

} // namespace game_fixes
