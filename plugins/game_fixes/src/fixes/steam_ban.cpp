// steam_ban — the engine's per-frame GC ban / competitive cooldown kick pass
// (GameSystem_Think_CheckSteamBan) walks CCSGameRules::sm_mapGcBanInformation and kicks every
// connected account it finds there. Accounts in the ini's whitelist are stripped out before the
// pass runs, so are competitive cooldowns while sv_kick_players_with_cooldown is below 2, and
// with clear_after_pass the map is cleared once the pass ran so a stale entry cannot hit the next
// player to join.
//
// How the map is treated here: the fork removed entries (CUtlRBTree::RemoveAt). This port never
// touches the tree's structure — it *neutralizes* an entry by overwriting its key (the account
// id) and reason with 0xFFFFFFFF, which the pass cannot match, and clears the whole map in Post
// with the fork's own RemoveAll writes (free list rebuild + root/count reset). That keeps the
// engine's red-black tree consistent whatever happens and is what makes the production
// configuration (clear_after_pass 1) behave exactly like the original.
//
// Map layout (offsets in assets/gamedata.jsonc) from the fork's own compiled code: node array at
// map+0x10 (stride 0x38, nodes: key at +0x10, reason at +0x18), allocation count at map+0x08,
// root at +0x18, element count at +0x1C, free-list head at +0x20, last allocation at +0x24.

#include "../fixes.hpp"
#include "../ban_map.hpp"

#include <pisex/keyvalues.hpp>

#include <cstdlib>
#include <unordered_set>
#include <vector>

namespace game_fixes {
namespace {

class CSteamBanFix final : public CFix {
public:
	explicit CSteamBanFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "steam_ban"; }

	void ReadConfig(const pisex::KeyValues& config) override {
		m_clearAfterPass = config.GetBool("clear_after_pass", true);

		const pisex::KeyValues* whitelist = config.FindKey("whitelist");
		if (!whitelist) {
			return;
		}
		for (const pisex::KeyValues* entry = whitelist->GetFirstSubKey(); entry; entry = entry->GetNextKey()) {
			const std::string_view steamId64 = entry->GetStringValue();
			const std::uint64_t id = std::strtoull(std::string(steamId64).c_str(), nullptr, 10);
			// SteamID64 -> account id (the low 32 bits); reject what cannot be a Steam account.
			if (id < 76561197960265728ull) {
				LogFix(Name(), Format("ignoring whitelist entry \"%s\" \"%s\": not a SteamID64", entry->GetName(),
									  std::string(steamId64).c_str()),
					   plg::Severity::Warning);
				continue;
			}
			m_whitelist.insert(static_cast<std::uint32_t>(id - 76561197960265728ull));
		}
	}

	bool Load() override;
	void Unload() override;

private:
	static polyhook::ResultType CheckSteamBanPre(polyhook::HookHandle hook, polyhook::ParametersHandle params,
												 int32_t count, polyhook::ReturnHandle ret,
												 polyhook::CallbackType type);
	static polyhook::ResultType CheckSteamBanPost(polyhook::HookHandle hook, polyhook::ParametersHandle params,
												  int32_t count, polyhook::ReturnHandle ret,
												  polyhook::CallbackType type);

	void StripEntries();
	void ClearMap();
	std::int32_t Count() const;

	static CSteamBanFix* s_instance;

	// The map's field offsets, read from the gamedata (see src/ban_map.hpp for the operations).
	banmap::Layout m_layout;

	// Steam32 account ids ("whitelist", given as SteamID64s) the pass must never see.
	std::unordered_set<std::uint32_t> m_whitelist;
	// "clear_after_pass": empty the map once the pass ran.
	bool m_clearAfterPass = true;
	hook::HookRef m_hook;
};

CSteamBanFix* CSteamBanFix::s_instance = nullptr;

std::int32_t CSteamBanFix::Count() const {
	return banmap::Count(m_env.banMap, m_layout);
}

polyhook::ResultType CSteamBanFix::CheckSteamBanPre(polyhook::HookHandle, polyhook::ParametersHandle,
													int32_t, polyhook::ReturnHandle, polyhook::CallbackType) {
	try {
		if (auto* fix = s_instance) {
			fix->StripEntries();
		}
	} catch (...) {
	}
	return polyhook::ResultType::Ignored;
}

polyhook::ResultType CSteamBanFix::CheckSteamBanPost(polyhook::HookHandle, polyhook::ParametersHandle,
													 int32_t, polyhook::ReturnHandle, polyhook::CallbackType) {
	try {
		auto* fix = s_instance;
		if (fix && fix->m_clearAfterPass && fix->Count() > 0) {
			fix->ClearMap();
		}
	} catch (...) {
	}
	return polyhook::ResultType::Ignored;
}

void CSteamBanFix::StripEntries() {
	// sv_kick_players_with_cooldown: below 2 the pass still acts on competitive cooldowns
	// (reasons 20, 22 and 23).
	bool dropCooldowns = false;
	if (__s2sdk_FindConVar && __s2sdk_GetConVarInt32) {
		static uint64_t cvar = 0; // resolved once
		if (cvar == 0) {
			cvar = s2sdk::FindConVar("sv_kick_players_with_cooldown");
		}
		if (cvar != 0) {
			dropCooldowns = s2sdk::GetConVarInt32(cvar) < 2;
		}
	}

	const int stripped = banmap::Neutralize(m_env.banMap, m_layout, m_whitelist, dropCooldowns);
	if (stripped > 0) {
		LogFix(Name(), Format("stripped %d entr%s from the GC ban map (%zu whitelisted account(s))", stripped,
							  stripped == 1 ? "y" : "ies", m_whitelist.size()));
	}
}

// The fork's RemoveAll: rebuild the free list over every slot and reset the tree.
void CSteamBanFix::ClearMap() {
	banmap::RemoveAll(m_env.banMap, m_layout);
	LogFix(Name(), "cleared the GC ban map after the pass");
}

bool CSteamBanFix::Load() {
	if (!m_env.fnCheckSteamBan) {
		LogFix(Name(), "GameSystem_Think_CheckSteamBan not found (signature)", plg::Severity::Warning);
		return false;
	}
	if (!m_env.banMap) {
		LogFix(Name(), "CCSGameRules::sm_mapGcBanInformation not found (signature)", plg::Severity::Warning);
		return false;
	}
	m_layout.array = m_env.gamedata.Offset("CCSGameRules_sm_mapGcBanInformation_Array", -1);
	m_layout.allocated = m_env.gamedata.Offset("CCSGameRules_sm_mapGcBanInformation_Allocated", -1);
	m_layout.root = m_env.gamedata.Offset("CCSGameRules_sm_mapGcBanInformation_Root", -1);
	m_layout.count = m_env.gamedata.Offset("CCSGameRules_sm_mapGcBanInformation_Count", -1);
	m_layout.firstFree = m_env.gamedata.Offset("CCSGameRules_sm_mapGcBanInformation_FirstFree", -1);
	m_layout.lastAlloc = m_env.gamedata.Offset("CCSGameRules_sm_mapGcBanInformation_LastAlloc", -1);
	m_layout.nodeStride = m_env.gamedata.Offset("CCSGameRules_sm_mapGcBanInformation_NodeStride", -1);
	m_layout.nodeKey = m_env.gamedata.Offset("CCSGameRules_sm_mapGcBanInformation_NodeKey", -1);
	m_layout.nodeReason = m_env.gamedata.Offset("CCSGameRules_sm_mapGcBanInformation_NodeReason", -1);
	if (!m_layout.Valid()) {
		LogFix(Name(), "ban map layout offsets are missing from the gamedata", plg::Severity::Warning);
		return false;
	}
	s_instance = this;
	const plg::vector<polyhook::DataType> args{};
	m_hook = hook::InstallDetour(m_env.fnCheckSteamBan, polyhook::DataType::Void, args,
								 &CSteamBanFix::CheckSteamBanPre, &CSteamBanFix::CheckSteamBanPost,
								 "GameFixes_CheckSteamBan");
	if (!m_hook) {
		s_instance = nullptr;
		LogFix(Name(), "hooking GameSystem_Think_CheckSteamBan failed", plg::Severity::Warning);
		return false;
	}
	LogFix(Name(), Format("hooked GameSystem_Think_CheckSteamBan (%p), ban map at %p, %zu whitelisted account(s), "
						  "clear after pass %s",
						  m_env.fnCheckSteamBan, m_env.banMap, m_whitelist.size(), m_clearAfterPass ? "on" : "off"));
	return true;
}

void CSteamBanFix::Unload() {
	hook::RemoveCallback(m_hook, polyhook::CallbackType::Pre, &CSteamBanFix::CheckSteamBanPre);
	hook::RemoveCallback(m_hook, polyhook::CallbackType::Post, &CSteamBanFix::CheckSteamBanPost);
	hook::UnhookDetour(m_hook, m_env.fnCheckSteamBan);
	m_hook = {};
	s_instance = nullptr;
}

} // namespace

std::unique_ptr<CFix> MakeSteamBanFix(Environment& env) {
	return std::make_unique<CSteamBanFix>(env);
}

} // namespace game_fixes
