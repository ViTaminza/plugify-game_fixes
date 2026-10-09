// beam_crash — CBeam::SetBeamOrigin / CBeam::SetBeamEndPos loop forever on a beam without a
// parent (a map can crash the server with one). The fork supersedes that path in Pre and makes
// the one assignment the original would have made; parented beams go through the game code
// untouched. Ported to polyhook detours over the two signature-resolved functions; Supercede in
// a Pre callback skips the original (the fork's Post handler's assignment is done in Pre here —
// it is the only side effect the skipped path would have had).

#include "../fixes.hpp"

namespace game_fixes {
namespace {

class CBeamCrashFix final : public CFix {
public:
	explicit CBeamCrashFix(Environment& env) : CFix(env) {}

	const char* Name() const override { return "beam_crash"; }

	bool Load() override;
	void Unload() override;

	// The parent check reads CBaseEntity::m_CBodyComponent -> CBodyComponent::m_pSceneNode ->
	// CGameSceneNode::m_pParent, so the schema offsets must be there before this loads.
	bool NeedsSchema() const override { return true; }

private:
	static polyhook::ResultType SetBeamOriginPre(polyhook::HookHandle hook, polyhook::ParametersHandle params,
												 int32_t count, polyhook::ReturnHandle ret,
												 polyhook::CallbackType type);
	static polyhook::ResultType SetBeamEndPosPre(polyhook::HookHandle hook, polyhook::ParametersHandle params,
												 int32_t count, polyhook::ReturnHandle ret,
												 polyhook::CallbackType type);

	// scene node of a beam, or null when the entity does not have its component chain yet.
	// CBaseEntity::m_CBodyComponent and CBodyComponent::m_pSceneNode are stored-pointer ("ptr")
	// fields: the objects are the pointers STORED in the fields (the original's
	// m_CBodyComponent()->m_pSceneNode()), not the field addresses GetEntDataPtr2 returns, so
	// each hop reads the pointer value with PtrAt.
	void* SceneNodeOf(void* beam) const {
		if (m_env.offEntityBodyComponent < 0 || m_env.offBodySceneNode < 0) {
			return nullptr;
		}
		void* body = PtrAt(beam, m_env.offEntityBodyComponent);
		if (!body) {
			return nullptr;
		}
		return PtrAt(body, m_env.offBodySceneNode);
	}

	static CBeamCrashFix* s_instance;

	hook::HookRef m_originHook;
	hook::HookRef m_endPosHook;
};

CBeamCrashFix* CBeamCrashFix::s_instance = nullptr;

// Pre: supersede when the beam has no parent (the game's unparented path loops); the assignment
// that path would have made is done here instead.
polyhook::ResultType CBeamCrashFix::SetBeamOriginPre(polyhook::HookHandle, polyhook::ParametersHandle params,
													 int32_t, polyhook::ReturnHandle,
													 polyhook::CallbackType) {
	try {
		auto* fix = s_instance;
		if (!fix) {
			return polyhook::ResultType::Ignored;
		}
		auto* beam = polyhook::GetArgumentPointer(params, 0);
		if (!beam) {
			return polyhook::ResultType::Ignored;
		}
		void* sceneNode = fix->SceneNodeOf(beam);
		if (!sceneNode || fix->m_env.offSceneNodeParent < 0) {
			return polyhook::ResultType::Ignored; // let the game handle what we cannot inspect
		}
		if (PtrAt(sceneNode, fix->m_env.offSceneNodeParent) != nullptr) {
			return polyhook::ResultType::Ignored; // parented: game code is fine
		}
		// The position argument is only read past this point: a parented beam - the common case,
		// the game updates every beam of the map each frame - leaves the callback without touching
		// it (each polyhook::GetArgumentPointer is a cross-plugin call).
		const auto* position = static_cast<const plg::vec3*>(polyhook::GetArgumentPointer(params, 1));
		if (!position) {
			return polyhook::ResultType::Ignored;
		}
		if (fix->m_env.offSceneNodeAbsOrigin >= 0 && __s2sdk_SetEntDataVector3D2) {
			s2sdk::SetEntDataVector3D2(sceneNode, fix->m_env.offSceneNodeAbsOrigin, *position, false, -1);
		}
		return polyhook::ResultType::Supercede;
	} catch (...) {
		return polyhook::ResultType::Ignored;
	}
}

polyhook::ResultType CBeamCrashFix::SetBeamEndPosPre(polyhook::HookHandle, polyhook::ParametersHandle params,
													 int32_t, polyhook::ReturnHandle,
													 polyhook::CallbackType) {
	try {
		auto* fix = s_instance;
		if (!fix) {
			return polyhook::ResultType::Ignored;
		}
		auto* beam = polyhook::GetArgumentPointer(params, 0);
		if (!beam) {
			return polyhook::ResultType::Ignored;
		}
		void* sceneNode = fix->SceneNodeOf(beam);
		if (!sceneNode || fix->m_env.offSceneNodeParent < 0) {
			return polyhook::ResultType::Ignored;
		}
		if (PtrAt(sceneNode, fix->m_env.offSceneNodeParent) != nullptr) {
			return polyhook::ResultType::Ignored;
		}
		// Read only past the parent check, like SetBeamOriginPre.
		const auto* position = static_cast<const plg::vec3*>(polyhook::GetArgumentPointer(params, 1));
		if (!position) {
			return polyhook::ResultType::Ignored;
		}
		if (__s2sdk_SetEntSchemaVector3D2) {
			s2sdk::SetEntSchemaVector3D2(beam, "CBeam", "m_vecEndPos", *position, false, -1);
		}
		return polyhook::ResultType::Supercede;
	} catch (...) {
		return polyhook::ResultType::Ignored;
	}
}

bool CBeamCrashFix::Load() {
	if (!m_env.fnSetBeamOrigin || !m_env.fnSetBeamEndPos) {
		LogFix(Name(), "CBeam::SetBeamOrigin/SetBeamEndPos not found (signature)", plg::Severity::Warning);
		return false;
	}
	if (m_env.offSceneNodeParent < 0) {
		LogFix(Name(), "CGameSceneNode::m_pParent is not in the schema; the fix cannot tell parented beams apart",
			   plg::Severity::Warning);
		return false;
	}
	s_instance = this;
	const plg::vector<polyhook::DataType> args{polyhook::DataType::Pointer, polyhook::DataType::Pointer};
	m_originHook = hook::InstallDetour(m_env.fnSetBeamOrigin, polyhook::DataType::Void, args,
									   &CBeamCrashFix::SetBeamOriginPre, nullptr, "GameFixes_beam_SetBeamOrigin");
	if (!m_originHook) {
		s_instance = nullptr;
		LogFix(Name(), "hooking CBeam::SetBeamOrigin failed (polyhook)", plg::Severity::Warning);
		return false;
	}
	m_endPosHook = hook::InstallDetour(m_env.fnSetBeamEndPos, polyhook::DataType::Void, args,
									   &CBeamCrashFix::SetBeamEndPosPre, nullptr, "GameFixes_beam_SetBeamEndPos");
	if (!m_endPosHook) {
		// Half-installed: drop the first hook again (Unload also runs on a failed Load).
		LogFix(Name(), "hooking CBeam::SetBeamEndPos failed (polyhook); SetBeamOrigin is unhooked again",
			   plg::Severity::Warning);
		Unload();
		return false;
	}
	LogFix(Name(), Format("hooked CBeam::SetBeamOrigin (%p) and CBeam::SetBeamEndPos (%p)",
						  m_env.fnSetBeamOrigin, m_env.fnSetBeamEndPos));
	return true;
}

void CBeamCrashFix::Unload() {
	s_instance = nullptr;
	// The callbacks are removed either way; the hooks themselves only when this plugin created
	// them and no one else is attached (a reload of this plugin would otherwise leave a callback
	// pointing into this library).
	hook::RemoveCallback(m_originHook, polyhook::CallbackType::Pre, &CBeamCrashFix::SetBeamOriginPre);
	hook::UnhookDetour(m_originHook, m_env.fnSetBeamOrigin);
	m_originHook = {};
	hook::RemoveCallback(m_endPosHook, polyhook::CallbackType::Pre, &CBeamCrashFix::SetBeamEndPosPre);
	hook::UnhookDetour(m_endPosHook, m_env.fnSetBeamEndPos);
	m_endPosHook = {};
}

} // namespace

std::unique_ptr<CFix> MakeBeamCrashFix(Environment& env) {
	return std::make_unique<CBeamCrashFix>(env);
}

} // namespace game_fixes
