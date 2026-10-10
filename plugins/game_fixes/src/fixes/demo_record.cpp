// demo_record — with tv_enable 1 the GOTV client is never let go: its
// CServerSideClient::Disconnect is superseded and the session's maxplayers is raised by one on
// INetworkServerService::StartupServer *before* the engine reads it, so the GOTV slot is its own
// and the demo keeps recording across match restarts on the same map.
//
// Port notes:
//  * the maxplayers bump runs on the plugin's own StartupServer Pre hook
//    (OnStartupServerConfig), the one place where the configuration is still editable.
//  * the field offset (0x7C) is game-build dependent and lives in assets/gamedata.jsonc.
//  * the GOTV client has no s2sdk player state at the time it is disconnected, so the fix reads
//    the CServerSideClient's own m_bIsHLTV field at its gamedata offset.

#include "../fixes.hpp"

namespace game_fixes {
namespace {

class CDemoRecordFix final : public CFix {
public:
	explicit CDemoRecordFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "demo_record"; }

	bool Load() override;
	void Unload() override;

	// Forwarded from the plugin's StartupServer Pre hook (config is the raw GameSessionConfiguration_t).
	void OnStartupServerConfig(void* config) override;

private:
	static polyhook::ResultType DisconnectPre(polyhook::HookHandle hook, polyhook::ParametersHandle params,
											  int32_t count, polyhook::ReturnHandle ret,
											  polyhook::CallbackType type);

	bool IsTvEnabled() const {
		if (m_tvEnable == 0 || !__s2sdk_GetConVarBool) {
			return false;
		}
		return s2sdk::GetConVarBool(m_tvEnable);
	}

	static CDemoRecordFix* s_instance;

	uint64_t m_tvEnable = 0;
	hook::HookRef m_hook;
};

CDemoRecordFix* CDemoRecordFix::s_instance = nullptr;

polyhook::ResultType CDemoRecordFix::DisconnectPre(polyhook::HookHandle, polyhook::ParametersHandle params,
												   int32_t, polyhook::ReturnHandle, polyhook::CallbackType) {
	try {
		auto* fix = s_instance;
		if (!fix || fix->m_env.offIsHLTV < 0 || !fix->IsTvEnabled()) {
			return polyhook::ResultType::Ignored;
		}
		auto* client = polyhook::GetArgumentPointer(params, 0);
		if (!client) {
			return polyhook::ResultType::Ignored;
		}
		if (BoolAt(client, fix->m_env.offIsHLTV)) {
			LogFix(fix->Name(), "GOTV is enabled, blocking the GOTV client's disconnect");
			return polyhook::ResultType::Supercede;
		}
	} catch (...) {
	}
	return polyhook::ResultType::Ignored;
}

void CDemoRecordFix::OnStartupServerConfig(void* config) {
	if (!config || m_env.offSessionMaxPlayers < 0 || !IsTvEnabled()) {
		return;
	}
	const std::int32_t maxPlayers = Int32At(config, m_env.offSessionMaxPlayers);
	SetInt32At(config, m_env.offSessionMaxPlayers, maxPlayers + 1);
	LogFix(Name(), Format("GOTV is enabled, expanding the session's maxplayers (%d -> %d)", maxPlayers,
						  maxPlayers + 1));
}

bool CDemoRecordFix::Load() {
	if (!__s2sdk_FindConVar || !__s2sdk_GetConVarBool) {
		LogFix(Name(), "convar bindings are missing", plg::Severity::Warning);
		return false;
	}
	m_tvEnable = s2sdk::FindConVar("tv_enable");

	if (!m_env.vtableServerSideClient || m_env.idxDisconnect < 0 || m_env.offIsHLTV < 0) {
		LogFix(Name(), "CServerSideClient vtable or Disconnect index unavailable", plg::Severity::Warning);
		return false;
	}
	s_instance = this;
	const plg::vector<polyhook::DataType> args{polyhook::DataType::Pointer, polyhook::DataType::Int32,
											   polyhook::DataType::Pointer};
	m_hook = hook::InstallVFunc(m_env.vtableServerSideClient, m_env.idxDisconnect, polyhook::DataType::Void, args,
								&CDemoRecordFix::DisconnectPre, nullptr, "GameFixes_demo_Disconnect");
	if (!m_hook) {
		s_instance = nullptr;
		LogFix(Name(), "hooking CServerSideClient::Disconnect failed", plg::Severity::Warning);
		return false;
	}
	LogFix(Name(), Format("hooked CServerSideClient::Disconnect on vtable %p, tv_enable is %s",
						  m_env.vtableServerSideClient, IsTvEnabled() ? "on" : "off"));
	return true;
}

void CDemoRecordFix::Unload() {
	// The callback goes either way; the hook itself only if this plugin created it (s2sdk may
	// own the same vtable slot).
	hook::RemoveCallback(m_hook, polyhook::CallbackType::Pre, &CDemoRecordFix::DisconnectPre);
	hook::UnhookVFunc(m_hook, m_env.vtableServerSideClient, m_env.idxDisconnect);
	m_hook = {};
	s_instance = nullptr;
}

} // namespace

std::unique_ptr<CFix> MakeDemoRecordFix(Environment& env) {
	return std::make_unique<CDemoRecordFix>(env);
}

} // namespace game_fixes
