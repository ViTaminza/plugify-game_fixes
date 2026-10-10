// game_fixes — the plugin core: reads game_fixes.ini, resolves the gamedata (signatures, vtable
// and structure offsets, and applies the hammer_id byte patch through s2sdk's game config),
// installs the shared engine hooks and the per-fix ones, and pumps the timers/next-frame queue
// from OnPluginUpdate.
//
// The shared hooks are the ones several fixes want:
//   * INetworkServerService::StartupServer (Pre) — session configuration + per-map state resets,
//   * the four CCSPlayer_MovementServices functions — one detour each, dispatched to every
//     movement listener (rampbug and water_jump overlap on CategorizePosition).
// Everything that fails to resolve is logged and left off; the plugin itself always loads.

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>
#include <polyhook/polyhook.hpp>
#include <s2sdk/s2sdk.hpp>

#include <pisex/compat.hpp>
#include <pisex/keyvalues.hpp>
#include <pisex/paths.hpp>

#include "api.hpp"
#include "fixes.hpp"
#include "load_policy.hpp"
#include "scheduler.hpp"

namespace game_fixes {
namespace {

constexpr const char* kPluginName = "game_fixes";

// Order of the shared movement detours (the members and UninstallHooks both index by this).
enum MovementSlot : int {
	kMovementProcess = 0,
	kMovementTryPlayerMove,
	kMovementCategorizePosition,
	kMovementWaterMove,
	kMovementCount
};

// How often the schema offsets are retried before the waiting fixes are told to give up (one
// attempt per second).
constexpr int kSchemaAttemptsBeforeGivingUp = 60;

class GameFixesPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

private:
	// StartupServer Pre: the session configuration before the engine reads it, then the per-map
	// resets (the fork did both from its own Pre hook).
	static polyhook::ResultType StartupServerPre(polyhook::HookHandle hook, polyhook::ParametersHandle params,
												 int32_t count, polyhook::ReturnHandle ret,
												 polyhook::CallbackType type);

	static polyhook::ResultType ProcessMovementPre(polyhook::HookHandle hook, polyhook::ParametersHandle params,
												   int32_t count, polyhook::ReturnHandle ret,
												   polyhook::CallbackType type);
	static polyhook::ResultType ProcessMovementPost(polyhook::HookHandle hook, polyhook::ParametersHandle params,
													int32_t count, polyhook::ReturnHandle ret,
													polyhook::CallbackType type);
	static polyhook::ResultType TryPlayerMovePre(polyhook::HookHandle hook, polyhook::ParametersHandle params,
												 int32_t count, polyhook::ReturnHandle ret,
												 polyhook::CallbackType type);
	static polyhook::ResultType TryPlayerMovePost(polyhook::HookHandle hook, polyhook::ParametersHandle params,
												  int32_t count, polyhook::ReturnHandle ret,
												  polyhook::CallbackType type);
	static polyhook::ResultType CategorizePositionPre(polyhook::HookHandle hook, polyhook::ParametersHandle params,
													  int32_t count, polyhook::ReturnHandle ret,
													  polyhook::CallbackType type);
	static polyhook::ResultType WaterMovePre(polyhook::HookHandle hook, polyhook::ParametersHandle params,
											 int32_t count, polyhook::ReturnHandle ret,
											 polyhook::CallbackType type);

	bool LoadConfig();
	void LoadPendingSchemaFixes();
	bool InstallStartupServerHook();
	bool InstallMovementHooks();
	void UninstallHooks();

	static GameFixesPlugin* s_instance;

	Environment m_env;
	std::vector<std::unique_ptr<CFix>> m_fixes;
	std::vector<std::unique_ptr<CFix>> m_pendingSchema;
	std::vector<IMovementListener*> m_movement;
	hook::HookRef m_startupHook;
	// the four shared movement detours, indexed by MovementSlot; the address each was installed
	// on is recomputed from m_env on teardown
	hook::HookRef m_movementHooks[kMovementCount];
	bool m_movementHooked = false;
	// True between a map start (our StartupServer Pre callback) and the map-end callback. The
	// schema system may only be asked while a server is up - its cache walks the entity network
	// serializer info, which does not exist before the first server start and is torn down again
	// on shutdown/changelevel (that is what segfaulted inside CollectSchemaFields).
	bool m_mapRunning = false;
	bool m_schemaResolved = false;
	// Set once the retries are exhausted: the lookup caches its misses inside s2sdk and logs one
	// warning per miss, so it must not be retried forever.
	bool m_schemaGaveUp = false;
	int m_schemaAttempts = 0;
	// pump bookkeeping: OnPluginUpdate carries the frame delta, but the harness only reports
	// milliseconds since the last call, so a huge gap (server hitching) is clamped.
	std::chrono::milliseconds m_sinceSchemaCheck{0};
};

GameFixesPlugin* GameFixesPlugin::s_instance = nullptr;

// ---------------------------------------------------------------- hooks --------

polyhook::ResultType GameFixesPlugin::StartupServerPre(polyhook::HookHandle, polyhook::ParametersHandle params,
													   int32_t, polyhook::ReturnHandle, polyhook::CallbackType) {
	try {
		auto* plugin = s_instance;
		if (!plugin) {
			return polyhook::ResultType::Ignored;
		}
		plugin->m_mapRunning = true;
		void* config = polyhook::GetArgumentPointer(params, 1);
		const auto* mapName = static_cast<const char*>(polyhook::GetArgumentPointer(params, 3));
		for (auto& fix : plugin->m_fixes) {
			fix->OnStartupServerConfig(config);
		}
		for (auto& fix : plugin->m_fixes) {
			fix->OnStartupServer(mapName);
		}
	} catch (...) {
	}
	return polyhook::ResultType::Ignored;
}

polyhook::ResultType GameFixesPlugin::ProcessMovementPre(polyhook::HookHandle, polyhook::ParametersHandle params,
														 int32_t, polyhook::ReturnHandle, polyhook::CallbackType) {
	try {
		auto* plugin = s_instance;
		if (!plugin) {
			return polyhook::ResultType::Ignored;
		}
		void* services = polyhook::GetArgumentPointer(params, 0);
		void* move = polyhook::GetArgumentPointer(params, 1);
		auto result = polyhook::ResultType::Ignored;
		for (IMovementListener* listener : plugin->m_movement) {
			const auto current = listener->OnProcessMovement(services, move);
			if (static_cast<int>(current) > static_cast<int>(result)) {
				result = current;
			}
		}
		return result;
	} catch (...) {
		return polyhook::ResultType::Ignored;
	}
}

polyhook::ResultType GameFixesPlugin::ProcessMovementPost(polyhook::HookHandle, polyhook::ParametersHandle params,
														  int32_t, polyhook::ReturnHandle, polyhook::CallbackType) {
	try {
		auto* plugin = s_instance;
		if (!plugin) {
			return polyhook::ResultType::Ignored;
		}
		void* services = polyhook::GetArgumentPointer(params, 0);
		void* move = polyhook::GetArgumentPointer(params, 1);
		for (IMovementListener* listener : plugin->m_movement) {
			listener->OnProcessMovementPost(services, move);
		}
	} catch (...) {
	}
	return polyhook::ResultType::Ignored;
}

polyhook::ResultType GameFixesPlugin::TryPlayerMovePre(polyhook::HookHandle, polyhook::ParametersHandle params,
													   int32_t, polyhook::ReturnHandle, polyhook::CallbackType) {
	try {
		auto* plugin = s_instance;
		if (!plugin) {
			return polyhook::ResultType::Ignored;
		}
		void* services = polyhook::GetArgumentPointer(params, 0);
		void* move = polyhook::GetArgumentPointer(params, 1);
		void* firstDest = polyhook::GetArgumentPointer(params, 2);
		void* firstTrace = polyhook::GetArgumentPointer(params, 3);
		auto* isSurfing = static_cast<bool*>(polyhook::GetArgumentPointer(params, 4));
		auto result = polyhook::ResultType::Ignored;
		for (IMovementListener* listener : plugin->m_movement) {
			const auto current = listener->OnTryPlayerMove(services, move, firstDest, firstTrace, isSurfing);
			if (static_cast<int>(current) > static_cast<int>(result)) {
				result = current;
			}
		}
		return result;
	} catch (...) {
		return polyhook::ResultType::Ignored;
	}
}

polyhook::ResultType GameFixesPlugin::TryPlayerMovePost(polyhook::HookHandle, polyhook::ParametersHandle params,
														int32_t, polyhook::ReturnHandle, polyhook::CallbackType) {
	try {
		auto* plugin = s_instance;
		if (!plugin) {
			return polyhook::ResultType::Ignored;
		}
		void* services = polyhook::GetArgumentPointer(params, 0);
		void* move = polyhook::GetArgumentPointer(params, 1);
		void* firstDest = polyhook::GetArgumentPointer(params, 2);
		void* firstTrace = polyhook::GetArgumentPointer(params, 3);
		auto* isSurfing = static_cast<bool*>(polyhook::GetArgumentPointer(params, 4));
		for (IMovementListener* listener : plugin->m_movement) {
			listener->OnTryPlayerMovePost(services, move, firstDest, firstTrace, isSurfing);
		}
	} catch (...) {
	}
	return polyhook::ResultType::Ignored;
}

polyhook::ResultType GameFixesPlugin::CategorizePositionPre(polyhook::HookHandle, polyhook::ParametersHandle params,
															int32_t, polyhook::ReturnHandle,
															polyhook::CallbackType) {
	try {
		auto* plugin = s_instance;
		if (!plugin) {
			return polyhook::ResultType::Ignored;
		}
		void* services = polyhook::GetArgumentPointer(params, 0);
		void* move = polyhook::GetArgumentPointer(params, 1);
		const bool stayOnGround = polyhook::GetArgumentBool(params, 2);
		auto result = polyhook::ResultType::Ignored;
		for (IMovementListener* listener : plugin->m_movement) {
			const auto current = listener->OnCategorizePosition(services, move, stayOnGround);
			if (static_cast<int>(current) > static_cast<int>(result)) {
				result = current;
			}
		}
		return result;
	} catch (...) {
		return polyhook::ResultType::Ignored;
	}
}

polyhook::ResultType GameFixesPlugin::WaterMovePre(polyhook::HookHandle, polyhook::ParametersHandle params,
												   int32_t, polyhook::ReturnHandle, polyhook::CallbackType) {
	try {
		auto* plugin = s_instance;
		if (!plugin) {
			return polyhook::ResultType::Ignored;
		}
		void* services = polyhook::GetArgumentPointer(params, 0);
		void* move = polyhook::GetArgumentPointer(params, 1);
		auto result = polyhook::ResultType::Ignored;
		for (IMovementListener* listener : plugin->m_movement) {
			const auto current = listener->OnWaterMove(services, move);
			if (static_cast<int>(current) > static_cast<int>(result)) {
				result = current;
			}
		}
		return result;
	} catch (...) {
		return polyhook::ResultType::Ignored;
	}
}

// -------------------------------------------------------------- plugin ---------

bool GameFixesPlugin::LoadConfig() {
	const std::string path = pisex::ResolvePath("game_fixes.ini");
	if (path.empty()) {
		LogFix(kPluginName, "game_fixes.ini not found (looked next to the server and in the configs dir); "
							"every fix stays off",
			   plg::Severity::Error);
		return false;
	}
	pisex::KeyValues::AutoDelete config(kPluginName);
	if (!config->LoadFromFile(path)) {
		LogFix(kPluginName, "game_fixes.ini could not be parsed (" + path + "); every fix stays off",
			   plg::Severity::Error);
		return false;
	}
	LogFix(kPluginName, "config " + path);

	m_fixes.push_back(MakeBeamCrashFix(m_env));
	m_fixes.push_back(MakeDemoRecordFix(m_env));
	m_fixes.push_back(MakeHammerIdFix(m_env));
	m_fixes.push_back(MakeSlowAnimationFix(m_env));
	m_fixes.push_back(MakeWorkshopVoiceFix(m_env));
	m_fixes.push_back(MakeSteamBanFix(m_env));
	m_fixes.push_back(MakeTeamLimitFix(m_env));
	m_fixes.push_back(MakeInputActivatorCrashFix(m_env));
	m_fixes.push_back(MakeSvCheatsFix(m_env));
	m_fixes.push_back(MakeRampbugFix(m_env));
	m_fixes.push_back(MakeWaterJumpFix(m_env));
	m_fixes.push_back(MakeKeyValuesFloodFix(m_env));
	m_fixes.push_back(MakeTeamSwitchSpamFix(m_env));
	m_fixes.push_back(MakeVoiceFloodFix(m_env));

	std::vector<std::unique_ptr<CFix>> loaded;
	for (auto& fix : m_fixes) {
		const pisex::KeyValues* block = config->FindKey(fix->Name());
		const bool enabled = block != nullptr && block->GetBool("enable");
		// A fix that needs the schema must not even call into it here: s2sdk's schema cache walks
		// the entity network serializer info, which only exists once the server is up (a
		// GetSchemaOffset at plugin start segfaults inside CollectSchemaFields). Such a fix is put
		// aside *before* Load() and loaded from OnPluginUpdate when the offsets resolve
		// (src/load_policy.hpp, tested in tests/core_logic_tests.cpp).
		switch (DecideLoad(enabled, fix->NeedsSchema(), m_schemaResolved)) {
		case LoadDecision::Skip:
			LogFix(fix->Name(), "disabled");
			continue;
		case LoadDecision::DeferSchema:
			fix->ReadConfig(*block);
			LogFix(fix->Name(), "waiting for the schema offsets (loaded on the first update after a map starts)");
			m_pendingSchema.push_back(std::move(fix));
			continue;
		case LoadDecision::Load:
			break;
		}
		fix->ReadConfig(*block);
		if (!fix->Load()) {
			LogFix(fix->Name(), "NOT loaded", plg::Severity::Warning);
			fix->Unload();
			continue;
		}
		LogFix(fix->Name(), "loaded");
		if (IMovementListener* movement = fix->Movement()) {
			m_movement.push_back(movement);
		}
		loaded.push_back(std::move(fix));
	}
	m_fixes = std::move(loaded);

	// The fork's server_list_players has no counterpart here on purpose: s2sdk's player manager
	// already pushes the player list to the Steam game server API on its own fixed 5 s interval,
	// so porting the block (and its update_interval) would fight over the call.
	if (const pisex::KeyValues* block = config->FindKey("server_list_players"); block && block->GetBool("enable")) {
		LogFix("server_list_players",
			   "not ported: s2sdk already pushes the player list to Steam every 5 s (interval not configurable here)");
	}
	return !m_fixes.empty() || !m_pendingSchema.empty();
}

// Runs once the schema offsets resolved: loads every fix that was waiting for them.
void GameFixesPlugin::LoadPendingSchemaFixes() {
	auto pending = std::move(m_pendingSchema);
	m_pendingSchema.clear();
	for (auto& fix : pending) {
		if (!fix->Load()) {
			LogFix(fix->Name(), "NOT loaded (schema offsets are in, Load() still failed)", plg::Severity::Warning);
			fix->Unload();
			continue;
		}
		LogFix(fix->Name(), "loaded");
		if (IMovementListener* movement = fix->Movement()) {
			m_movement.push_back(movement);
		}
		m_fixes.push_back(std::move(fix));
	}
	if (!pending.empty() && !m_movementHooked) {
		InstallMovementHooks();
	}
}

bool GameFixesPlugin::InstallStartupServerHook() {
	if (!m_env.interfaceNetworkServerService || m_env.idxStartupServer < 0) {
		LogFix(kPluginName, "INetworkServerService or the StartupServer index is unavailable", plg::Severity::Warning);
		return false;
	}
	const plg::vector<polyhook::DataType> args{polyhook::DataType::Pointer, polyhook::DataType::Pointer,
											   polyhook::DataType::Pointer, polyhook::DataType::Pointer};
	// s2sdk hooks the same instance with a Post callback; polyhook shares the hook and this only
	// adds the Pre side.
	m_startupHook = hook::InstallVTable(m_env.interfaceNetworkServerService, m_env.idxStartupServer,
										polyhook::DataType::Void, args, &GameFixesPlugin::StartupServerPre, nullptr,
										"GameFixes_StartupServer");
	if (!m_startupHook) {
		LogFix(kPluginName, "adding the StartupServer Pre callback failed", plg::Severity::Warning);
	}
	return static_cast<bool>(m_startupHook);
}

bool GameFixesPlugin::InstallMovementHooks() {
	if (m_movement.empty()) {
		return true;
	}
	const plg::vector<polyhook::DataType> processArgs{polyhook::DataType::Pointer, polyhook::DataType::Pointer};
	const plg::vector<polyhook::DataType> moveArgs{polyhook::DataType::Pointer, polyhook::DataType::Pointer,
												   polyhook::DataType::Pointer, polyhook::DataType::Pointer,
												   polyhook::DataType::Pointer};
	const plg::vector<polyhook::DataType> categorizeArgs{polyhook::DataType::Pointer, polyhook::DataType::Pointer,
														 polyhook::DataType::Bool};
	bool ok = true;
	auto install = [&](int slot, void* address, const plg::vector<polyhook::DataType>& args,
					   hook::CallbackHandler pre, hook::CallbackHandler post, const char* name) {
		if (!address) {
			ok = false;
			return;
		}
		m_movementHooks[slot] = hook::InstallDetour(address, polyhook::DataType::Void, args, pre, post, name);
		if (!m_movementHooks[slot]) {
			ok = false;
		}
	};
	install(kMovementProcess, m_env.fnProcessMovement, processArgs, &GameFixesPlugin::ProcessMovementPre,
			&GameFixesPlugin::ProcessMovementPost, "GameFixes_ProcessMovement");
	install(kMovementTryPlayerMove, m_env.fnTryPlayerMove, moveArgs, &GameFixesPlugin::TryPlayerMovePre,
			&GameFixesPlugin::TryPlayerMovePost, "GameFixes_TryPlayerMove");
	install(kMovementCategorizePosition, m_env.fnCategorizePosition, categorizeArgs,
			&GameFixesPlugin::CategorizePositionPre, nullptr, "GameFixes_CategorizePosition");
	install(kMovementWaterMove, m_env.fnWaterMove, processArgs, &GameFixesPlugin::WaterMovePre, nullptr,
			"GameFixes_WaterMove");
	m_movementHooked = ok;
	if (!ok) {
		LogFix(kPluginName, "one or more movement detours could not be installed; rampbug/water_jump stay limited",
			   plg::Severity::Warning);
	}
	return ok;
}

void GameFixesPlugin::UninstallHooks() {
	// Every recorded hook is dropped independently: a partial install (one detour failed) must
	// still take the installed ones down, or they would keep callbacks pointing into this library
	// after it unloads. Unhooking only happens for hooks this plugin created and that no one else
	// has attached to.
	void* addresses[kMovementCount] = {m_env.fnProcessMovement, m_env.fnTryPlayerMove, m_env.fnCategorizePosition,
									   m_env.fnWaterMove};
	hook::CallbackHandler pres[kMovementCount] = {&GameFixesPlugin::ProcessMovementPre,
												  &GameFixesPlugin::TryPlayerMovePre,
												  &GameFixesPlugin::CategorizePositionPre,
												  &GameFixesPlugin::WaterMovePre};
	hook::CallbackHandler posts[kMovementCount] = {&GameFixesPlugin::ProcessMovementPost,
												   &GameFixesPlugin::TryPlayerMovePost, nullptr, nullptr};
	for (int i = 0; i < kMovementCount; ++i) {
		hook::RemoveCallback(m_movementHooks[i], polyhook::CallbackType::Pre, pres[i]);
		hook::RemoveCallback(m_movementHooks[i], polyhook::CallbackType::Post, posts[i]);
		hook::UnhookDetour(m_movementHooks[i], addresses[i]);
		m_movementHooks[i] = {};
	}
	m_movementHooked = false;
	if (m_startupHook) {
		hook::RemoveCallback(m_startupHook, polyhook::CallbackType::Pre, &GameFixesPlugin::StartupServerPre);
		hook::UnhookVTable(m_startupHook, m_env.interfaceNetworkServerService, m_env.idxStartupServer);
		m_startupHook = {};
	}
}

plg::PluginResult GameFixesPlugin::OnPluginStart() {
	try {
		s_instance = this;
		pisex::Init(kPluginName);
		if (!m_env.Resolve()) {
			// Without the gamedata nothing can be installed; the plugin still loads (returning
			// Failed would stop every other plugify plugin).
			LogFix(kPluginName, "gamedata could not be resolved; every fix stays off", plg::Severity::Error);
			return {};
		}
		// pisexcompat's map-end callback is driven by s2sdk's own map-end listener and touches no
		// client (it is registered here, before anything can run).
		pisex::RegisterMapEndCallback([this]() { m_mapRunning = false; });
		if (!LoadConfig()) {
			LogFix(kPluginName, "loaded, but no fix is active (see the config error above)", plg::Severity::Error);
			return {};
		}
		InstallStartupServerHook();
		InstallMovementHooks();
		LogFix(kPluginName, Format("ready (%zu fix(es) active)", m_fixes.size()));
	} catch (const std::exception& ex) {
		LogFix(kPluginName, std::string("OnPluginStart failed: ") + ex.what(), plg::Severity::Error);
	} catch (...) {
		LogFix(kPluginName, "OnPluginStart failed with an unknown exception", plg::Severity::Error);
	}
	return {};
}

plg::PluginResult GameFixesPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	try {
		// The schema offsets need the schema system, which walks the entity network serializer
		// info - it only exists while a map runs (querying it earlier segfaults inside
		// CollectSchemaFields, and it is torn down again during a changelevel). Retry once a
		// second while a map is up; the fixes waiting for the offsets load on the attempt that
		// resolves them.
		// Server-up evidence: our own StartupServer callback, or pisexcompat's map-running state
		// (true from the first pump on, cleared at map end) - the latter covers a plugin load that
		// happened after the current map had already started.
		const bool mapRunning = m_mapRunning || pisex::IsGameReady();
		if (!m_schemaResolved && !m_schemaGaveUp && mapRunning) {
			m_sinceSchemaCheck += dt;
			if (m_sinceSchemaCheck >= std::chrono::seconds(1)) {
				m_sinceSchemaCheck = std::chrono::milliseconds(0);
				ResolveSchemaOffsets(m_env);
				m_schemaResolved = SchemaOffsetsReady(m_env);
				if (m_schemaResolved) {
					LoadPendingSchemaFixes();
				} else if (++m_schemaAttempts >= kSchemaAttemptsBeforeGivingUp) {
					// Bounded: the lookup caches its miss inside s2sdk and warns on every retry,
					// so it is stopped here; the waiting fixes report a reason instead of silently
					// staying off.
					m_schemaGaveUp = true;
					LogFix(kPluginName, "the schema offsets did not resolve; the fixes that need them stay off",
						   plg::Severity::Warning);
					LoadPendingSchemaFixes();
				}
			}
		}

		// pisexcompat's own pump (deferred startup callbacks, its timer/next-frame queues); this
		// plugin drives its own scheduler but keeps the library in step like every other consumer.
		pisex::Poll();

		const double delta = std::chrono::duration<double>(dt).count();
		scheduler::Tick(delta > 0.0 ? delta : 0.015);
	} catch (const std::exception& ex) {
		LogFix(kPluginName, std::string("update failed: ") + ex.what(), plg::Severity::Error);
	} catch (...) {
		LogFix(kPluginName, "update failed with an unknown exception", plg::Severity::Error);
	}
	return {};
}

plg::PluginResult GameFixesPlugin::OnPluginEnd() {
	try {
		for (auto& fix : m_fixes) {
			fix->Unload();
		}
		m_fixes.clear();
		m_movement.clear();
		UninstallHooks();
		m_env.gamedata.Close(); // the fixes and hooks are gone; nothing reads the config anymore
		scheduler::Shutdown();
		pisex::Shutdown();
		s_instance = nullptr;
	} catch (...) {
	}
	return {};
}

} // namespace

} // namespace game_fixes

namespace {

game_fixes::GameFixesPlugin g_gameFixesPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_gameFixesPlugin)
