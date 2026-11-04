#include "hooks.h"
#include "gamedata.h"
#include "reencode.h"

#include <cstdint>
#include <cstdio>

#include <CDetour/detours.h>
#include <eiface.h>
#include <iclient.h>
#include <inetmsghandler.h>

IServer *g_pServer;
IServerGameClients *g_pServerGameClients;

class CBaseClient;
class CClientFrame;
class bf_write;

SH_DECL_HOOK1(IClientMessageHandler, ProcessBaselineAck, SH_NOATTRIB, 0, bool, CLC_BaselineAck *);

namespace safeproxy {

namespace {

CDetour *s_SVComputeClientPacks_detour;
CDetour *s_GetPackedEntity_detour;
CDetour *s_WriteDeltaEntities_detour;
CDetour *s_GetReplayDelay_detour;

bool s_BaselineAckHookTried;
int s_BaselineAckHookID;
int s_BaselineAckPostHookID;

uint8_t *BaseClientOf(IClientMessageHandler *handler)
{
	return reinterpret_cast<uint8_t *>(handler) - kBaseClientMessageHandlerOffset;
}

int BaselineUpdateTick(const uint8_t *base)
{
	return *reinterpret_cast<const int *>(base + g_Offsets.CBaseClient_m_nBaselineUpdateTick);
}

// Acks are processed between sends, never during one.
int s_ackedUpdateTick = -1;

bool Hook_ProcessBaselineAck(CLC_BaselineAck *msg)
{
	s_ackedUpdateTick = BaselineUpdateTick(BaseClientOf(META_IFACEPTR(IClientMessageHandler)));
	RETURN_META_VALUE(MRES_IGNORED, true);
}

bool Hook_ProcessBaselineAck_Post(CLC_BaselineAck *msg)
{
	if (s_ackedUpdateTick < 0)
		RETURN_META_VALUE(MRES_IGNORED, true);

	uint8_t *base = BaseClientOf(META_IFACEPTR(IClientMessageHandler));
	IClient *iclient = reinterpret_cast<IClient *>(base + kBaseClientIClientOffset);
	const uint32_t *baselines_sent = reinterpret_cast<const uint32_t *>(base + g_Offsets.CBaseClient_m_BaselinesSent);
	// Only an accepted ack clears m_nBaselineUpdateTick inside the call.
	if (BaselineUpdateTick(base) < 0)
		OnBaselineAccepted(iclient->GetPlayerSlot(), s_ackedUpdateTick, baselines_sent);
	// A failed ack can leave part of the update in the engine's baseline.
	else if (!META_RESULT_ORIG_RET(bool))
		OnBaselineAckFailed(iclient->GetPlayerSlot(), baselines_sent);
	RETURN_META_VALUE(MRES_IGNORED, true);
}

void UnloadBaselineAckHook()
{
	int *hook_ids[] = {
		&s_BaselineAckPostHookID,
		&s_BaselineAckHookID,
	};

	for (int *hook_id : hook_ids)
	{
		if (*hook_id)
		{
			SH_REMOVE_HOOK_ID(*hook_id);
			*hook_id = 0;
		}
	}
}

// Every CGameClient shares the hooked vtable.
void InstallBaselineAckHook(CGameClient *client)
{
	s_BaselineAckHookTried = true;

	IClientMessageHandler *handler = reinterpret_cast<IClientMessageHandler *>(reinterpret_cast<uint8_t *>(client) + kBaseClientMessageHandlerOffset);
	s_BaselineAckHookID = SH_ADD_VPHOOK(IClientMessageHandler, ProcessBaselineAck, handler, SH_STATIC(Hook_ProcessBaselineAck), false);
	s_BaselineAckPostHookID = SH_ADD_VPHOOK(IClientMessageHandler, ProcessBaselineAck, handler, SH_STATIC(Hook_ProcessBaselineAck_Post), true);
	if (!s_BaselineAckHookID || !s_BaselineAckPostHookID)
	{
		UnloadBaselineAckHook();
		g_pSM->LogError(myself, "Could not hook IClientMessageHandler::ProcessBaselineAck");
	}
}

DETOUR_DECL_STATIC3(Detour_SV_ComputeClientPacks, void, int, clientCount, CGameClient **, clients, CFrameSnapshot *, snapshot)
{
	// Packing runs before this frame's sends, so the hook is in place before any baseline update.
	if (!s_BaselineAckHookTried && clientCount > 0)
		InstallBaselineAckHook(clients[0]);

	PreComputePacks(clientCount);
	DETOUR_STATIC_CALL(Detour_SV_ComputeClientPacks)(clientCount, clients, snapshot);
	PostComputePacks(clientCount, clients, snapshot);
}

DETOUR_DECL_MEMBER2(Detour_GetPackedEntity, PackedEntity *, CFrameSnapshot *, pSnapshot, int, entity)
{
	PackedEntity *original = DETOUR_MEMBER_CALL(Detour_GetPackedEntity)(pSnapshot, entity);
	return OnGetPackedEntity(pSnapshot, entity, original);
}

// sv_parallel_sendsnapshot runs this on several threads, and SourceHook isn't thread-safe.
DETOUR_DECL_MEMBER4(Detour_WriteDeltaEntities, void, CBaseClient *, client, CClientFrame *, to, CClientFrame *, from, bf_write &, pBuf)
{
	// CHLTVServer and CReplayServer inherit this function, and their clients' slots overlap the game server's.
	if (reinterpret_cast<void *>(this) != g_pServer)
	{
		DETOUR_MEMBER_CALL(Detour_WriteDeltaEntities)(client, to, from, pBuf);
		return;
	}

	uint8_t *base = reinterpret_cast<uint8_t *>(client);
	IClient *iclient = reinterpret_cast<IClient *>(base + kBaseClientIClientOffset);
	// SourceTV and Replay demo writers call this with their bot. Demos keep the real state.
	int slot = iclient->IsFakeClient() ? -1 : iclient->GetPlayerSlot();

	const void *baseline = *reinterpret_cast<void **>(base + g_Offsets.CBaseClient_m_pBaseline);
	const int &baseline_update_tick = *reinterpret_cast<int *>(base + g_Offsets.CBaseClient_m_nBaselineUpdateTick);
	const uint32_t *baselines_sent = reinterpret_cast<const uint32_t *>(base + g_Offsets.CBaseClient_m_BaselinesSent);
	const int pending_before = baseline_update_tick;

	PreWriteDelta(slot, baseline);
	DETOUR_MEMBER_CALL(Detour_WriteDeltaEntities)(client, to, from, pBuf);
	PreWriteDelta(-1, nullptr);

	// The call sets m_nBaselineUpdateTick when it sends a baseline update.
	if (slot >= 0 && pending_before < 0 && baseline_update_tick >= 0)
		OnBaselineUpdateSent(slot, baseline_update_tick, baselines_sent);
}

// sv_parallel_sendsnapshot runs this on several threads. A client disconnected mid-snapshot has a null edict, which the game reads as the world.
DETOUR_DECL_MEMBER2(Detour_GetReplayDelay, int, edict_t *, player, int &, entity)
{
	if (!player)
		return 0;
	return DETOUR_MEMBER_CALL(Detour_GetReplayDelay)(player, entity);
}

} // anonymous namespace

PackedEntity *EngineGetPackedEntity(CFrameSnapshot *snapshot, int entity)
{
	// The engine's GetPackedEntity never reads its this pointer.
	static Detour_GetPackedEntityClass manager;
	return (manager.*Detour_GetPackedEntityClass::Detour_GetPackedEntity_Actual)(snapshot, entity);
}

bool InstallHooks(char *error, size_t maxlen)
{
	s_SVComputeClientPacks_detour = DETOUR_CREATE_STATIC(Detour_SV_ComputeClientPacks, g_Offsets.SV_ComputeClientPacks);
	if (!s_SVComputeClientPacks_detour)
	{
		snprintf(error, maxlen, "Could not create SV_ComputeClientPacks detour");
		return false;
	}

	s_GetPackedEntity_detour = DETOUR_CREATE_MEMBER(Detour_GetPackedEntity, g_Offsets.CFrameSnapshotManager_GetPackedEntity);
	if (!s_GetPackedEntity_detour)
	{
		snprintf(error, maxlen, "Could not create CFrameSnapshotManager::GetPackedEntity detour");
		return false;
	}

	s_WriteDeltaEntities_detour = DETOUR_CREATE_MEMBER(Detour_WriteDeltaEntities, g_Offsets.CBaseServer_WriteDeltaEntities);
	if (!s_WriteDeltaEntities_detour)
	{
		snprintf(error, maxlen, "Could not create CBaseServer::WriteDeltaEntities detour");
		return false;
	}

	void *get_replay_delay = SH_GET_ORIG_VFNPTR_ENTRY(g_pServerGameClients, &IServerGameClients::GetReplayDelay);
	s_GetReplayDelay_detour = DETOUR_CREATE_MEMBER(Detour_GetReplayDelay, get_replay_delay);
	if (!s_GetReplayDelay_detour)
	{
		snprintf(error, maxlen, "Could not create IServerGameClients::GetReplayDelay detour");
		return false;
	}

	s_SVComputeClientPacks_detour->EnableDetour();
	s_GetPackedEntity_detour->EnableDetour();
	s_WriteDeltaEntities_detour->EnableDetour();
	s_GetReplayDelay_detour->EnableDetour();
	return true;
}

void UnloadHooks()
{
	UnloadBaselineAckHook();
	s_BaselineAckHookTried = false;

	CDetour **detours[] = {
		&s_GetReplayDelay_detour,
		&s_WriteDeltaEntities_detour,
		&s_GetPackedEntity_detour,
		&s_SVComputeClientPacks_detour,
	};

	for (CDetour **detour : detours)
	{
		if (*detour)
		{
			(*detour)->Destroy();
			*detour = nullptr;
		}
	}
}

} // namespace safeproxy
