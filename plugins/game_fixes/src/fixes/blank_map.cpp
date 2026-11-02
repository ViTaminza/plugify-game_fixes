// blank_map — when a match ends with no next map set, nextlevel_changed carries an empty
// nextlevel and the server can drop into an empty, unplayable state that needs a manual restart.
// The fix restarts the current map instead: `changelevel <map>` for a stock map,
// `ds_workshop_changelevel <map>` for a workshop one (only addressable by its id, which is what
// the current map name is on a workshop server).
//
// The command is deferred one frame: it is issued from a game-event Pre hook, and a changelevel
// started inside the event dispatch is exactly the kind of re-entrancy the engine and the event
// manager do not expect. The pending flag makes it one restart per map; it is also cleared on the
// next map start (OnStartupServer) and in Unload. The map gate (src/map_gate.hpp) gates both the event and the
// deferred command, so an event that arrives while the server is shutting down is ignored.

#include "../blank_map_logic.hpp"
#include "../fixes.hpp"
#include "../map_gate.hpp"
#include "../scheduler.hpp"

#include <string>

namespace game_fixes {
namespace {

class CBlankMapFix final : public CFix {
public:
	explicit CBlankMapFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "blank_map"; }

	bool Load() override;
	void Unload() override;
	void OnStartupServer(const char* mapName) override;

private:
	static s2sdk::ResultType OnNextLevelChanged(const plg::string& name, void* event, bool dontBroadcast);
	static void OnDeferredRestart(std::string mapName);

	static CBlankMapFix* s_instance;
	bool m_hooked = false;
	bool m_pending = false;
};

CBlankMapFix* CBlankMapFix::s_instance = nullptr;

s2sdk::ResultType CBlankMapFix::OnNextLevelChanged(const plg::string&, void* event, bool) {
	try {
		auto* fix = s_instance;
		if (!fix || !event || !IsMapReady()) {
			return s2sdk::ResultType::Continue;
		}
		if (fix->m_pending) {
			return s2sdk::ResultType::Continue; // one restart per map
		}
		if (!__s2sdk_GetEventString || !__s2sdk_GetCurrentMap) {
			return s2sdk::ResultType::Continue;
		}
		const plg::string nextLevel = s2sdk::GetEventString(event, "nextlevel");
		const std::string_view next(nextLevel.c_str(), nextLevel.size());
		const plg::string current = s2sdk::GetCurrentMap();
		const std::string_view map(current.c_str(), current.size());
		if (!blankmap::NeedsRestart(next, map)) {
			return s2sdk::ResultType::Continue;
		}
		fix->m_pending = true;
		LogFix(fix->Name(), Format("no next map after the match end, restarting \"%s\" next frame",
								   std::string(map).c_str()));
		scheduler::NextFrame([name = std::string(map)]() { OnDeferredRestart(name); });
	} catch (...) {
	}
	return s2sdk::ResultType::Continue;
}

void CBlankMapFix::OnDeferredRestart(std::string mapName) {
	try {
		auto* fix = s_instance;
		if (!fix) {
			return; // unloaded before the frame ran
		}
		fix->m_pending = false;
		// Map end / shutdown empties the server; issuing a changelevel then would be wrong.
		if (!IsMapReady() || mapName.empty()) {
			return;
		}
		if (!__s2sdk_IsMapValid || !__s2sdk_ServerCommand) {
			return;
		}
		const std::string command =
			blankmap::RestartCommand(mapName, s2sdk::IsMapValid(plg::string(mapName)));
		s2sdk::ServerCommand(plg::string(command));
		LogFix(fix->Name(), "executed " + command);
	} catch (...) {
	}
}

bool CBlankMapFix::Load() {
	if (!__s2sdk_HookEvent || !__s2sdk_GetEventString || !__s2sdk_GetCurrentMap || !__s2sdk_IsMapValid ||
		!__s2sdk_ServerCommand) {
		LogFix(Name(), "event/engine bindings are missing", plg::Severity::Warning);
		return false;
	}
	s_instance = this;
	try {
		const s2sdk::EventHookError error =
			s2sdk::HookEvent("nextlevel_changed", &CBlankMapFix::OnNextLevelChanged, s2sdk::HookMode::Pre);
		m_hooked = error == s2sdk::EventHookError::Okay;
	} catch (...) {
		m_hooked = false;
	}
	if (!m_hooked) {
		s_instance = nullptr;
		LogFix(Name(), "hooking nextlevel_changed failed", plg::Severity::Warning);
		return false;
	}
	LogFix(Name(), "watching nextlevel_changed");
	return true;
}

void CBlankMapFix::Unload() {
	if (m_hooked && __s2sdk_UnhookEvent) {
		s2sdk::UnhookEvent("nextlevel_changed", &CBlankMapFix::OnNextLevelChanged, s2sdk::HookMode::Pre);
	}
	m_hooked = false;
	m_pending = false;
	s_instance = nullptr;
}

void CBlankMapFix::OnStartupServer(const char*) {
	// A new map is running: a restart scheduled for the previous one is never delivered.
	m_pending = false;
}

} // namespace

std::unique_ptr<CFix> MakeBlankMapFix(Environment& env) {
	return std::make_unique<CBlankMapFix>(env);
}

} // namespace game_fixes
