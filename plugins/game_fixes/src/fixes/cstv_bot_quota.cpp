// cstv_bot_quota — automatic bot quota management (bot_quota_mode = fill) can pick the in-game
// SourceTV client for removal: the selector sees a pawn-less HLTV controller and mistakes it for
// a bot, which interrupts demo recording (and later recordings cannot start normally).
//
// The fix is the reference CstvBotQuotaFix: detour the native "select and kick a bot" function
// and its shared TeamCandidate predicate. TeamCandidate returns false for an HLTV controller ONLY
// while the selector is on the stack (thread-local depth > 0), so the iterator skips SourceTV and
// continues to an ordinary bot. Everywhere else the predicate runs untouched, bot settings are
// never changed, and the fix is only meaningful while a map is running.
//
// Two engine signatures (verified unique against the live libserver.so) and one schema field are
// required; anything missing leaves the fix off with a log (it must never take the server down
// inside bot selection).

#include "../cstv_logic.hpp"
#include "../fixes.hpp"
#include "../map_gate.hpp"
#include "../scheduler.hpp"

namespace game_fixes {
namespace {

// Selection depth is per thread: the bot quota can run on the game thread only, but the predicate
// is shared and must not filter on any other caller's thread.
thread_local int g_selectionDepth = 0;

class CCstvBotQuotaFix final : public CFix {
public:
	explicit CCstvBotQuotaFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "cstv_bot_quota"; }

	bool Load() override;
	void Unload() override;
	void OnStartupServer(const char* mapName) override;
	void OnMapEnd() override;

private:
	static polyhook::ResultType SelectAndKickBotPre(polyhook::HookHandle hook, polyhook::ParametersHandle params,
													int32_t count, polyhook::ReturnHandle ret,
													polyhook::CallbackType type);
	static polyhook::ResultType SelectAndKickBotPost(polyhook::HookHandle hook, polyhook::ParametersHandle params,
													 int32_t count, polyhook::ReturnHandle ret,
													 polyhook::CallbackType type);
	static polyhook::ResultType TeamCandidatePre(polyhook::HookHandle hook, polyhook::ParametersHandle params,
												 int32_t count, polyhook::ReturnHandle ret,
												 polyhook::CallbackType type);

	void ArmOffsetResolver();
	void ResolveHltvOffset();

	static CCstvBotQuotaFix* s_instance;
	hook::HookRef m_selectHook;
	hook::HookRef m_candidateHook;
	std::int32_t m_offIsHLTV = -1;
	scheduler::Timer* m_resolveTimer = nullptr;
	int m_resolveAttempts = 0;
	bool m_reportedSkip = false;
	bool m_loggedOffsetFailure = false;
};

CCstvBotQuotaFix* CCstvBotQuotaFix::s_instance = nullptr;

polyhook::ResultType CCstvBotQuotaFix::SelectAndKickBotPre(polyhook::HookHandle, polyhook::ParametersHandle,
														   int32_t, polyhook::ReturnHandle, polyhook::CallbackType) {
	g_selectionDepth = cstv::EnterSelection(g_selectionDepth);
	return polyhook::ResultType::Ignored;
}

polyhook::ResultType CCstvBotQuotaFix::SelectAndKickBotPost(polyhook::HookHandle, polyhook::ParametersHandle,
															int32_t, polyhook::ReturnHandle, polyhook::CallbackType) {
	g_selectionDepth = cstv::LeaveSelection(g_selectionDepth);
	return polyhook::ResultType::Ignored;
}

polyhook::ResultType CCstvBotQuotaFix::TeamCandidatePre(polyhook::HookHandle, polyhook::ParametersHandle params,
														int32_t, polyhook::ReturnHandle ret,
														polyhook::CallbackType) {
	try {
		auto* fix = s_instance;
		if (!fix || fix->m_offIsHLTV < 0 || g_selectionDepth <= 0) {
			return polyhook::ResultType::Ignored;
		}
		if (!IsMapReady() || !__s2sdk_EntPointerToEntHandle || !__s2sdk_GetEntData) {
			return polyhook::ResultType::Ignored;
		}
		// Argument 1 is the controller the iterator is looking at (0 = the filter, 2 = the pawn).
		void* controller = polyhook::GetArgumentPointer(params, 1);
		if (!controller) {
			return polyhook::ResultType::Ignored;
		}
		const int handle = s2sdk::EntPointerToEntHandle(controller);
		if (!IsValidHandle(handle)) {
			return polyhook::ResultType::Ignored;
		}
		const bool isHltv = s2sdk::GetEntData(handle, fix->m_offIsHLTV, 1) != 0;
		if (!cstv::ShouldRejectCandidate(g_selectionDepth, true, isHltv)) {
			return polyhook::ResultType::Ignored;
		}
		if (!fix->m_reportedSkip) {
			fix->m_reportedSkip = true;
			LogFix(fix->Name(), "skipped the SourceTV controller during native bot removal (first time this map)");
		}
		// The predicate reports "not a candidate", so the selector keeps looking for a real bot.
		SetSupersededReturn(ret, false);
		return polyhook::ResultType::Supercede;
	} catch (...) {
		// Never let an exception cross back into the engine's bot selection.
		return polyhook::ResultType::Ignored;
	}
}

void CCstvBotQuotaFix::ArmOffsetResolver() {
	if (m_offIsHLTV >= 0 || m_resolveTimer) {
		return;
	}
	// GetSchemaOffset is only safe while a map is running; the timer gives it a few seconds to
	// become answerable instead of calling it from OnPluginStart (which segfaults).
	m_resolveTimer = scheduler::AddTimer(2.0f, [this]() { ResolveHltvOffset(); }, scheduler::kTimerFlagRepeat);
}

void CCstvBotQuotaFix::ResolveHltvOffset() {
	try {
		if (!IsMapReady()) {
			return;
		}
		if (!__s2sdk_GetSchemaOffset) {
			return;
		}
		m_offIsHLTV = s2sdk::GetSchemaOffset(plg::string("CBasePlayerController"), plg::string("m_bIsHLTV"));
		if (m_offIsHLTV >= 0) {
			if (m_resolveTimer) {
				scheduler::KillTimer(m_resolveTimer);
				m_resolveTimer = nullptr;
			}
			LogFix(Name(), Format("m_bIsHLTV resolved at offset %d", m_offIsHLTV));
			return;
		}
		if (++m_resolveAttempts >= 15) {
			// s2sdk caches the miss and logs on every lookup; stop retrying after ~30 s and stay
			// inactive (the next map start resets the attempts and tries again).
			if (!m_loggedOffsetFailure) {
				m_loggedOffsetFailure = true;
				LogFix(Name(), "CBasePlayerController::m_bIsHLTV not in the schema; the fix stays inactive",
					   plg::Severity::Warning);
			}
			if (m_resolveTimer) {
				scheduler::KillTimer(m_resolveTimer);
				m_resolveTimer = nullptr;
			}
		}
	} catch (...) {
	}
}

bool CCstvBotQuotaFix::Load() {
	if (!m_env.fnCstvSelectAndKickBot || !m_env.fnCstvTeamCandidate) {
		LogFix(Name(), "CSTV quota signatures are missing from the gamedata", plg::Severity::Warning);
		return false;
	}
	s_instance = this;
	const plg::vector<polyhook::DataType> selectArgs{polyhook::DataType::Int32};
	m_selectHook = hook::InstallDetour(m_env.fnCstvSelectAndKickBot, polyhook::DataType::Bool, selectArgs,
									   &CCstvBotQuotaFix::SelectAndKickBotPre, &CCstvBotQuotaFix::SelectAndKickBotPost,
									   "GameFixes_cstv_select");
	if (!m_selectHook) {
		s_instance = nullptr;
		LogFix(Name(), "hooking SelectAndKickBot failed", plg::Severity::Warning);
		return false;
	}
	const plg::vector<polyhook::DataType> candidateArgs{polyhook::DataType::Pointer, polyhook::DataType::Pointer,
														polyhook::DataType::Pointer};
	m_candidateHook = hook::InstallDetour(m_env.fnCstvTeamCandidate, polyhook::DataType::Bool, candidateArgs,
										  &CCstvBotQuotaFix::TeamCandidatePre, nullptr, "GameFixes_cstv_candidate");
	if (!m_candidateHook) {
		// Keep the state consistent: Load() failure leads the plugin to call Unload().
		s_instance = nullptr;
		LogFix(Name(), "hooking TeamCandidate failed", plg::Severity::Warning);
		return false;
	}
	ArmOffsetResolver();
	LogFix(Name(), Format("hooked CSTV bot selection (%p / %p)", m_env.fnCstvSelectAndKickBot,
						  m_env.fnCstvTeamCandidate));
	return true;
}

void CCstvBotQuotaFix::Unload() {
	if (m_resolveTimer) {
		scheduler::KillTimer(m_resolveTimer);
		m_resolveTimer = nullptr;
	}
	hook::RemoveCallback(m_selectHook, polyhook::CallbackType::Pre, &CCstvBotQuotaFix::SelectAndKickBotPre);
	hook::RemoveCallback(m_selectHook, polyhook::CallbackType::Post, &CCstvBotQuotaFix::SelectAndKickBotPost);
	hook::UnhookDetour(m_selectHook, m_env.fnCstvSelectAndKickBot);
	m_selectHook = {};
	hook::RemoveCallback(m_candidateHook, polyhook::CallbackType::Pre, &CCstvBotQuotaFix::TeamCandidatePre);
	hook::UnhookDetour(m_candidateHook, m_env.fnCstvTeamCandidate);
	m_candidateHook = {};
	g_selectionDepth = 0;
	s_instance = nullptr;
}

void CCstvBotQuotaFix::OnMapEnd() {
	// A selection cannot span a map change; resetting the depth keeps a Post that never ran (an
	// exception unwound through the engine) from leaving the predicate armed on the new map.
	g_selectionDepth = 0;
}

void CCstvBotQuotaFix::OnStartupServer(const char*) {
	m_reportedSkip = false;
	m_resolveAttempts = 0;
	m_loggedOffsetFailure = false;
	// A selector whose Post never ran (an exception unwound through the engine) must not leave the
	// predicate armed into the next map.
	g_selectionDepth = 0;
	ArmOffsetResolver();
}

} // namespace

std::unique_ptr<CFix> MakeCstvBotQuotaFix(Environment& env) {
	return std::make_unique<CCstvBotQuotaFix>(env);
}

} // namespace game_fixes
