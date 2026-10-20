// hammer_id — SetSchemaHammerUniqueId skips m_sUniqueHammerID behind a jnz; patching that byte
// to a jmp makes every entity get its id (the CS2Fixes fix).
//
// The byte patch is declared in assets/gamedata.jsonc ("Patches" / SetHammerUniqueIdJmp, 1 byte
// "EB" written at the SetSchemaHammerUniqueId signature) and applied by s2sdk's patch manager
// when the plugin loads the game config — it saves the original bytes, verifies the write and
// restores on unload. s2sdk's own gamedata carries the same patch *commented out*, so nothing
// else applies it and there is no double patch. This fix therefore only reports what happened.

#include "../fixes.hpp"

namespace game_fixes {
namespace {

class CHammerIdFix final : public CFix {
public:
	explicit CHammerIdFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "hammer_id"; }

	bool Load() override {
		if (!m_env.gamedata.Signature("SetSchemaHammerUniqueId")) {
			LogFix(Name(), "SetSchemaHammerUniqueId not found (signature)", plg::Severity::Warning);
			return false;
		}
		if (__s2sdk_GetGameConfigPatch) {
			const plg::string patch = s2sdk::GetGameConfigPatch(m_env.gamedata.id, "SetHammerUniqueIdJmp");
			LogFix(Name(), Format("patched SetSchemaHammerUniqueId (jnz -> jmp); patch bytes now '%s'",
								  patch.c_str()));
		} else {
			LogFix(Name(), "patch applied by the game config (patch query unavailable)");
		}
		return true;
	}
};

} // namespace

std::unique_ptr<CFix> MakeHammerIdFix(Environment& env) {
	return std::make_unique<CHammerIdFix>(env);
}

} // namespace game_fixes
