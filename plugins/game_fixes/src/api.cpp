#include "api.hpp"

namespace game_fixes {

namespace hook {

namespace {

bool AddCallbacks(polyhook::HookHandle hook, CallbackHandler pre, CallbackHandler post) {
	if (hook == nullptr) {
		return false;
	}
	bool ok = true;
	if (pre != nullptr) {
		ok = polyhook::AddCallback(hook, polyhook::CallbackType::Pre, pre) && ok;
	}
	if (post != nullptr) {
		ok = polyhook::AddCallback(hook, polyhook::CallbackType::Post, post) && ok;
	}
	return ok;
}

} // namespace

HookRef InstallVFunc(void* vtableAddress, std::int32_t index, polyhook::DataType returnType,
					 const plg::vector<polyhook::DataType>& arguments, CallbackHandler pre,
					 CallbackHandler post, const char* name) {
	HookRef ref;
	if (!vtableAddress || index < 0) {
		return ref;
	}
	try {
		// Reuse an existing hook for the same (vtable, index) — s2sdk already hooks
		// CServerSideClient::SendNetMessage and ProcessRespondCvarValue this way.
		ref.handle = polyhook::FindVirtual2(vtableAddress, EncodedIndex(index));
		if (ref.handle == nullptr) {
			ref.handle = polyhook::HookVirtualFunc2(vtableAddress, EncodedIndex(index), returnType,
													arguments, -1, plg::string(name));
			ref.created = ref.handle != nullptr;
		}
		if (!AddCallbacks(ref.handle, pre, post)) {
			ref.handle = nullptr;
			ref.created = false;
		}
	} catch (...) {
		ref.handle = nullptr;
		ref.created = false;
	}
	return ref;
}

HookRef InstallVTable(void* object, std::int32_t index, polyhook::DataType returnType,
					  const plg::vector<polyhook::DataType>& arguments, CallbackHandler pre,
					  CallbackHandler post, const char* name) {
	HookRef ref;
	if (!object || index < 0) {
		return ref;
	}
	try {
		ref.handle = polyhook::FindVirtual(object, index);
		if (ref.handle == nullptr) {
			ref.handle = polyhook::HookVirtualTable(object, index, returnType, arguments, -1, plg::string(name));
			ref.created = ref.handle != nullptr;
		}
		if (!AddCallbacks(ref.handle, pre, post)) {
			ref.handle = nullptr;
			ref.created = false;
		}
	} catch (...) {
		ref.handle = nullptr;
		ref.created = false;
	}
	return ref;
}

HookRef InstallDetour(void* address, polyhook::DataType returnType,
					  const plg::vector<polyhook::DataType>& arguments, CallbackHandler pre,
					  CallbackHandler post, const char* name) {
	HookRef ref;
	if (!address) {
		return ref;
	}
	try {
		ref.handle = polyhook::FindDetour(address);
		if (ref.handle == nullptr) {
			ref.handle = polyhook::HookDetour(address, returnType, arguments, -1, plg::string(name));
			ref.created = ref.handle != nullptr;
		}
		if (!AddCallbacks(ref.handle, pre, post)) {
			ref.handle = nullptr;
			ref.created = false;
		}
	} catch (...) {
		ref.handle = nullptr;
		ref.created = false;
	}
	return ref;
}

void RemoveCallback(const HookRef& ref, polyhook::CallbackType type, CallbackHandler callback) {
	if (ref.handle == nullptr || callback == nullptr || !__polyhook_RemoveCallback) {
		return;
	}
	try {
		polyhook::RemoveCallback(ref.handle, type, callback);
	} catch (...) {
	}
}

// Unhooking is only safe when this plugin created the hook and nothing else is attached to it
// (another plugin may have added a callback to the same hook after it was created).
bool CanUnhook(const HookRef& ref) {
	if (!ref.created || !ref.handle || !__polyhook_AreCallbacksRegistered) {
		return false;
	}
	try {
		return !polyhook::AreCallbacksRegistered(ref.handle);
	} catch (...) {
		return false;
	}
}

void UnhookVFunc(const HookRef& ref, void* vtableAddress, std::int32_t index) {
	if (!CanUnhook(ref) || !vtableAddress || index < 0 || !__polyhook_UnhookVirtualFunc2) {
		return;
	}
	try {
		polyhook::UnhookVirtualFunc2(vtableAddress, EncodedIndex(index));
	} catch (...) {
	}
}

void UnhookVTable(const HookRef& ref, void* object, std::int32_t index) {
	if (!CanUnhook(ref) || !object || index < 0 || !__polyhook_UnhookVirtualTable) {
		return;
	}
	try {
		polyhook::UnhookVirtualTable(object, index);
	} catch (...) {
	}
}

void UnhookDetour(const HookRef& ref, void* address) {
	if (!CanUnhook(ref) || !address || !__polyhook_UnhookDetour) {
		return;
	}
	try {
		polyhook::UnhookDetour(address);
	} catch (...) {
	}
}

} // namespace hook

bool Environment::Resolve() {
	if (!gamedata.Load()) {
		Log("game_fixes: gamedata (assets/gamedata.jsonc) could not be loaded", plg::Severity::Error);
		return false;
	}

	vtableServerSideClient = gamedata.VTable("CServerSideClient");
	vtableTraceFilter = gamedata.VTable("CTraceFilter");
	vtableNavPhysics = gamedata.VTable("CNavPhysicsInterface");
	if (__s2sdk_QueryInterface) {
		interfaceNetworkServerService = s2sdk::QueryInterface("engine2", "NetworkServerService_001");
	}

	fnSetBeamOrigin = gamedata.Signature("CBeam_SetBeamOrigin");
	fnSetBeamEndPos = gamedata.Signature("CBeam_SetBeamEndPos");
	fnAcceptInput = gamedata.Signature("CEntityIdentity_AcceptInput");
	fnProcessMovement = gamedata.Signature("CCSPlayer_MovementServices_ProcessMovement");
	fnTryPlayerMove = gamedata.Signature("CCSPlayer_MovementServices_TryPlayerMove");
	fnCategorizePosition = gamedata.Signature("CCSPlayer_MovementServices_CategorizePosition");
	fnWaterMove = gamedata.Signature("CCSPlayer_MovementServices_WaterMove");
	fnCheckSteamBan = gamedata.Signature("GameSystem_Think_CheckSteamBan");

	// lea reg, [rip + sm_mapGcBanInformation]: the operand is the map itself.
	if (void* ref = gamedata.Signature("CCSGameRules_sm_mapGcBanInformation")) {
		const std::int32_t disp = Int32At(ref, 3);
		banMap = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(ref) + 7 + static_cast<std::intptr_t>(disp));
	}

	idxStartupServer = gamedata.Offset("INetworkServerService_StartupServer_Index", -1);
	idxDisconnect = gamedata.Offset("CServerSideClient_Disconnect_Index", -1);
	idxSendNetMessage = gamedata.Offset("CServerSideClient_SendNetMessage_Index", -1);
	idxProcessStringCmd = gamedata.Offset("CServerSideClient_ProcessStringCmd_Index", -1);
	idxProcessCmdKeyValues = gamedata.Offset("CServerSideClient_ProcessCmdKeyValues_Index", -1);
	idxProcessVoiceData = gamedata.Offset("CServerSideClient_ProcessVoiceData_Index", -1);
	idxNavTraceShape = gamedata.Offset("INavPhysicsInterface_Nav_TraceShape_Index", -1);
	offClientSlot = gamedata.Offset("CServerSideClientBase_m_nClientSlot", -1);
	offSignonState = gamedata.Offset("CServerSideClientBase_m_nSignonState", -1);
	offFakePlayer = gamedata.Offset("CServerSideClientBase_m_bFakePlayer", -1);
	offUserId = gamedata.Offset("CServerSideClientBase_m_UserID", -1);
	offIsHLTV = gamedata.Offset("CServerSideClientBase_m_bIsHLTV", -1);
	signonStateConnected = gamedata.Offset("SIGNONSTATE_CONNECTED", -1);
	offVoiceDataMessageId = gamedata.Offset("SVC_Messages_svc_VoiceData", 47);
	offSessionMaxPlayers = gamedata.Offset("GameSessionConfiguration_maxPlayers", -1);
	offMoveVelocity = gamedata.Offset("CMoveData_m_vecVelocity", -1);
	offMoveAbsOrigin = gamedata.Offset("CMoveData_m_vecAbsOrigin", -1);
	offVoiceHasBits = gamedata.Offset("CCLCMsg_VoiceData_hasBits", -1);
	offVoiceAudio = gamedata.Offset("CCLCMsg_VoiceData_audio", -1);
	offVoicePacketOffsets = gamedata.Offset("CMsgVoiceAudio_packetOffsets", -1);
	offVoiceVoiceData = gamedata.Offset("CMsgVoiceAudio_voiceData", -1);
	offCmdKeyValuesData = gamedata.Offset("CCLCMsg_CmdKeyValues_data", -1);
	return true;
}

void ResolveSchemaOffsets(Environment& env) {
	auto offset = [](const char* className, const char* member) -> std::int32_t {
		if (!__s2sdk_GetSchemaOffset) {
			return -1;
		}
		return s2sdk::GetSchemaOffset(plg::string(className), plg::string(member));
	};
	// Resolved once the schema system can answer; -1 means "not there, try again later".
	if (env.offEntityBodyComponent < 0) {
		env.offEntityBodyComponent = offset("CBaseEntity", "m_CBodyComponent");
	}
	if (env.offBodySceneNode < 0) {
		env.offBodySceneNode = offset("CBodyComponent", "m_pSceneNode");
	}
	if (env.offSceneNodeParent < 0) {
		env.offSceneNodeParent = offset("CGameSceneNode", "m_pParent");
	}
	if (env.offSceneNodeAbsOrigin < 0) {
		env.offSceneNodeAbsOrigin = offset("CGameSceneNode", "m_vecAbsOrigin");
	}
	if (env.offEntityOwnerHandle < 0) {
		env.offEntityOwnerHandle = offset("CBaseEntity", "m_hOwnerEntity");
	}
	if (env.offEntityCollision < 0) {
		env.offEntityCollision = offset("CBaseEntity", "m_pCollision");
	}
	if (env.offCollisionAttribute < 0) {
		env.offCollisionAttribute = offset("CCollisionProperty", "m_collisionAttribute");
	}
	if (env.offAttributeHierarchyId < 0) {
		env.offAttributeHierarchyId = offset("VPhysicsCollisionAttribute_t", "m_nHierarchyId");
	}
	if (env.offAttributeInteractsWith < 0) {
		env.offAttributeInteractsWith = offset("VPhysicsCollisionAttribute_t", "m_nInteractsWith");
	}
}

bool SchemaOffsetsReady(const Environment& env) {
	return env.offEntityBodyComponent >= 0 && env.offBodySceneNode >= 0 && env.offSceneNodeParent >= 0 &&
		   env.offSceneNodeAbsOrigin >= 0 && env.offEntityOwnerHandle >= 0 && env.offEntityCollision >= 0 &&
		   env.offCollisionAttribute >= 0 && env.offAttributeHierarchyId >= 0 &&
		   env.offAttributeInteractsWith >= 0;
}

} // namespace game_fixes
