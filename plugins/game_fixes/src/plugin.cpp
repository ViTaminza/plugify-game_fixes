// game_fixes — the plugin core: reads game_fixes.ini (through the plugin's own KV1 reader and path
// resolution), resolves the gamedata (signatures, vtable and structure offsets, and applies the
// hammer_id byte patch through s2sdk's game config), installs the shared engine hooks and the
// per-fix ones, and pumps the timers/next-frame queue from OnPluginUpdate.
//
// The shared hooks are the ones several fixes want:
//   * INetworkServerService::StartupServer (Pre) — session configuration + per-map state resets,
//   * the four CCSPlayer_MovementServices functions — one detour each, dispatched to every
//     movement listener (rampbug and water_jump overlap on CategorizePosition).
// Everything that fails to resolve is logged and left off; the plugin itself always loads.
//
// The plugin is pure s2sdk (docs/PLUGIN_STANDARD.md §7): pisexcompat is gone, and the few things it
// provided live in this plugin now —
//   * the map gate (src/map_gate.hpp): the OnMapStart/OnMapEnd listeners are registered here, the
//     gate is latched open on the first OnPluginUpdate after a map starts (or after a mid-map
//     plugin load) and shut on OnMapEnd. No client, entity or schema call runs while it is shut
//     (PITFALLS B1/B2); this replaces pisex::IsGameReady()/RegisterMapEndCallback/Poll;
//   * the config reader (src/kv1.hpp) and the path resolution (src/paths.hpp) replace
//     pisex::KeyValues and pisex::ResolvePath;
//   * the timer/next-frame pump is this plugin's own scheduler (src/scheduler.cpp), pumped from
//     OnPluginUpdate; on OnMapEnd the per-map delayed work is dropped (scheduler::ClearDelayedWork)
//     so a deferred kick or restart meant for the old map can never run against the new one;
//   * chat output goes through src/output.hpp (a literal message to every player, no language or
//     phrase call - the server language is never switched).

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>
#include <polyhook/polyhook.hpp>
#include <s2sdk/s2sdk.hpp>

#include "api.hpp"
#include "config.hpp"
#include "fixes.hpp"
#include "kv1.hpp"
#include "load_policy.hpp"
#include "map_gate.hpp"
#include "paths.hpp"
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

// The host's config/data roots. Both are optional bindings; an empty string makes the path helpers
// fall back to their relative candidates.
std::string ConfigsDir() {
	try {
		if (plg::GetConfigsDir) {
			const plg::string value = plg::GetConfigsDir();
			return std::string(value.c_str(), value.size());
		}
	} catch (...) {
	}
	return {};
}

std::string DataDir() {
	try {
		if (plg::GetDataDir) {
			const plg::string value = plg::GetDataDir();
			return std::string(value.c_str(), value.size());
		}
	} catch (...) {
	}
	return {};
}

// PITFALLS B1/B3: the one s2sdk call that is safe before the first map. GetCurrentMap null-checks
// gpGlobals itself and answers an empty string then, which is exactly the proof the map gate needs
// before it may open.
bool HasMapName() {
	if (!__s2sdk_GetCurrentMap) {
		return false;
	}
	try {
		return !s2sdk::GetCurrentMap().empty();
	} catch (...) {
		return false;
	}
}

// --- map lifecycle listeners (PITFALLS B1/B2) ----------------------------------------------

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

	// OnMapStart runs when the new map exists, but readiness is latched by the first
	// OnPluginUpdate (docs/PLUGIN_STANDARD.md §7 item 2), never from inside the map-start callback
	// chain. OnMapEnd only drops state: the client list is already being torn down, so no client,
	// entity or schema call may run here (PITFALLS B2). Every fix is told first (it drops its own
	// per-map timer/latch), then the plugin's scheduler sweeps the remaining delayed work - a
	// next-frame kick or the blank_map restart belongs to the map that just ended.
	static void OnMapStartListener();
	static void OnMapEndListener();

	bool LoadConfig();
	void LoadPendingSchemaFixes();
	bool RegisterMapListeners();
	void UnregisterMapListeners();
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
	// The s2sdk OnMapStart/OnMapEnd listeners are registered (they drive the map gate).
	bool m_mapListeners = false;
	bool m_schemaResolved = false;
	// Set once the retries are exhausted: the lookup caches its misses inside s2sdk and logs one
	// warning per miss, so it must not be retried forever.
	bool m_schemaGaveUp = false;
	int m_schemaAttempts = 0;
	// pump bookkeeping: OnPluginUpdate carries the frame delta, but the harness only reports
	// milliseconds since the last call, so a huge gap (server hitching) is clamped by the check.
	std::chrono::milliseconds m_sinceSchemaCheck{0};
};

GameFixesPlugin* GameFixesPlugin::s_instance = nullptr;

// --------------------------------------------------------- map lifecycle --------

void GameFixesPlugin::OnMapStartListener() {
	try {
		Gate().OnMapStart();
	} catch (...) {
	}
}

void GameFixesPlugin::OnMapEndListener() {
	try {
		Gate().OnMapEnd();
		if (auto* plugin = s_instance) {
			for (auto& fix : plugin->m_fixes) {
				fix->OnMapEnd();
			}
		}
		// The final sweep: queued next-frame tasks and any one-shot timer a fix did not drop.
		scheduler::ClearDelayedWork();
	} catch (...) {
	}
}

// ---------------------------------------------------------------- hooks --------

polyhook::ResultType GameFixesPlugin::StartupServerPre(polyhook::HookHandle, polyhook::ParametersHandle params,
													   int32_t, polyhook::ReturnHandle, polyhook::CallbackType) {
	try {
		auto* plugin = s_instance;
		if (!plugin) {
			return polyhook::ResultType::Ignored;
		}
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
	const std::string path = ResolveConfigPath(config::kFileName, ConfigsDir(), DataDir());
	if (path.empty()) {
		LogFix(kPluginName, "game_fixes.ini path could not be resolved; every fix stays off",
			   plg::Severity::Error);
		return false;
	}
	const std::optional<KvNode> tree = LoadKv1File(path);
	if (!tree) {
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
	m_fixes.push_back(MakeVoiceFix(m_env));
	m_fixes.push_back(MakeSteamBanFix(m_env));
	m_fixes.push_back(MakeTeamLimitFix(m_env));
	m_fixes.push_back(MakeInputActivatorCrashFix(m_env));
	m_fixes.push_back(MakeSvCheatsFix(m_env));
	m_fixes.push_back(MakeRampbugFix(m_env));
	m_fixes.push_back(MakeWaterJumpFix(m_env));
	m_fixes.push_back(MakeKeyValuesFloodFix(m_env));
	m_fixes.push_back(MakeTeamSwitchSpamFix(m_env));
	m_fixes.push_back(MakeVoiceFloodFix(m_env));
	m_fixes.push_back(MakeBlankMapFix(m_env));
	m_fixes.push_back(MakePlayerGhostFix(m_env));
	m_fixes.push_back(MakeFakeMessagesFix(m_env));
	m_fixes.push_back(MakeCstvBotQuotaFix(m_env));
	m_fixes.push_back(MakeJumpSpamFix(m_env));

	// The factory names must be the ones the shipped config (and src/config.hpp's list) uses: a
	// mismatch would silently disable a fix, because its block would never be found.
	for (const auto& fix : m_fixes) {
		bool known = false;
		for (const char* name : config::kFixNames) {
			if (std::string_view(name) == fix->Name()) {
				known = true;
				break;
			}
		}
		if (!known) {
			LogFix(fix->Name(), "is not in the plugin's fix list; its config block would never be found",
				   plg::Severity::Error);
		}
	}

	std::vector<std::unique_ptr<CFix>> loaded;
	for (auto& fix : m_fixes) {
		const config::FixBlock block = config::ReadFixBlock(*tree, fix->Name());
		if (block.options == nullptr) {
			// A block that is missing, or that only carries "enable", has no options to read.
			LogFix(fix->Name(), "disabled");
			continue;
		}
		// A fix that needs the schema must not even call into it here: s2sdk's schema cache walks
		// the entity network serializer info, which only exists once the server is up (a
		// GetSchemaOffset at plugin start segfaults inside CollectSchemaFields). Such a fix is put
		// aside *before* Load() and loaded from OnPluginUpdate when the offsets resolve
		// (src/load_policy.hpp, tested in tests/core_logic_tests.cpp).
		switch (DecideLoad(block.enabled, fix->NeedsSchema(), m_schemaResolved)) {
		case LoadDecision::Skip:
			LogFix(fix->Name(), "disabled");
			continue;
		case LoadDecision::DeferSchema:
			fix->ReadConfig(*block.options);
			LogFix(fix->Name(), "waiting for the schema offsets (loaded on the first update after a map starts)");
			m_pendingSchema.push_back(std::move(fix));
			continue;
		case LoadDecision::Load:
			break;
		}
		fix->ReadConfig(*block.options);
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
	const config::FixBlock notPorted =
		config::ReadFixBlock(*tree, config::kServerListPlayersBlock);
	if (notPorted.enabled) {
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

bool GameFixesPlugin::RegisterMapListeners() {
	if (!__s2sdk_OnMapStart_Register || !__s2sdk_OnMapStart_Unregister || !__s2sdk_OnMapEnd_Register ||
		!__s2sdk_OnMapEnd_Unregister) {
		LogFix(kPluginName, "the s2sdk map-start/map-end listeners are unavailable; nothing that needs a map is touched",
			   plg::Severity::Warning);
		return false;
	}
	try {
		// OnMapEnd first: when the second registration fails, the half-registered pair can only
		// ever close the gate, never leave it open (the same order plugins/PlayersInfo uses).
		s2sdk::OnMapEnd_Register(&GameFixesPlugin::OnMapEndListener);
		s2sdk::OnMapStart_Register(&GameFixesPlugin::OnMapStartListener);
		m_mapListeners = true;
		Gate().SetListenersRegistered(true);
	} catch (...) {
		// A half-registered pair must leave nothing behind that could call into this library; both
		// unregisters are safe no-ops when the listener is not registered (PITFALLS A.7).
		m_mapListeners = false;
		Gate().SetListenersRegistered(false);
		try {
			s2sdk::OnMapStart_Unregister(&GameFixesPlugin::OnMapStartListener);
		} catch (...) {
		}
		try {
			s2sdk::OnMapEnd_Unregister(&GameFixesPlugin::OnMapEndListener);
		} catch (...) {
		}
	}
	if (!m_mapListeners) {
		LogFix(kPluginName, "registering the map-start/map-end listeners failed; nothing that needs a map is touched",
			   plg::Severity::Warning);
	}
	return m_mapListeners;
}

void GameFixesPlugin::UnregisterMapListeners() {
	m_mapListeners = false;
	Gate().SetListenersRegistered(false);
	// Symmetric teardown (PITFALLS A.7): both listeners are removed even when the plugin is
	// reloaded, so no callback points into this library after it unloads.
	if (__s2sdk_OnMapStart_Unregister) {
		try {
			s2sdk::OnMapStart_Unregister(&GameFixesPlugin::OnMapStartListener);
		} catch (...) {
		}
	}
	if (__s2sdk_OnMapEnd_Unregister) {
		try {
			s2sdk::OnMapEnd_Unregister(&GameFixesPlugin::OnMapEndListener);
		} catch (...) {
		}
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
	// has attached to (PITFALLS D.3: remove the callback first, then unhook by target only when
	// AreCallbacksRegistered() answers false - a shared hook belongs to its other users).
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
		// The plugin clock starts at 0; the first pump tick only establishes a baseline (the
		// host's first delta may cover its uptime, which used to fire every timer at boot).
		scheduler::Init();
		// PITFALLS B1/B2: a fresh start (including `plg reload`) begins with the gate shut; the
		// listeners are registered before anything can run, so the mid-map latch of the first
		// update is armed from the first frame on.
		Gate().Reset();
		RegisterMapListeners();
		if (!HasMapName()) {
			LogFix(kPluginName, "no map name yet (or the GetCurrentMap binding is missing); the plugin waits for "
								"the first map before touching clients, entities or the schema");
		}
		if (!m_env.Resolve()) {
			// Without the gamedata nothing can be installed; the plugin still loads (returning
			// Failed would stop every other plugify plugin).
			LogFix(kPluginName, "gamedata could not be resolved; every fix stays off", plg::Severity::Error);
			return {};
		}
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
		// PITFALLS B1/B2: latch the map gate. The first update after OnMapStart (or the first one
		// of a plugin loaded into a running map) is the "a map is up" evidence, and OnMapEnd
		// closes the gate again. The latch runs before any fix callback can fire from this pump;
		// once the gate is open the map-name query is not repeated (it only feeds the latch).
		if (!Gate().Ready()) {
			Gate().OnPluginUpdate(HasMapName());
		}

		// The schema offsets need the schema system, which walks the entity network serializer
		// info - it only exists while a map runs (querying it earlier segfaults inside
		// CollectSchemaFields, and it is torn down again during a changelevel). Retry once a
		// second while a map is up; the fixes waiting for the offsets load on the attempt that
		// resolves them.
		if (!m_schemaResolved && !m_schemaGaveUp && IsMapReady()) {
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
		// Reverse load order: a hook that two fixes share (voice_fix creates ProcessVoiceData,
		// voice_flood loaded later reuses it) is unhooked by its creator once the later-loaded user
		// has removed its callback. In load order the creator gives up on the still-shared hook and
		// loses its `created` flag, leaving a callback-less detour behind after `plg reload`
		// (PITFALLS A.7).
		for (auto it = m_fixes.rbegin(); it != m_fixes.rend(); ++it) {
			(*it)->Unload();
		}
		m_fixes.clear();
		m_pendingSchema.clear();
		m_movement.clear();
		UninstallHooks();
		// Symmetric teardown of the map listeners (PITFALLS A.7): registered in OnPluginStart,
		// removed here, so `plg reload` leaves no callback pointing into this library.
		UnregisterMapListeners();
		Gate().Close();
		m_env.gamedata.Close(); // the fixes and hooks are gone; nothing reads the config anymore
		scheduler::Shutdown();
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
