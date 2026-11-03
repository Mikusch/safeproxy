#include "hooks.h"
#include "gamedata.h"
#include "reencode.h"

#include <cstdint>
#include <cstdio>

#include <CDetour/detours.h>
#include <eiface.h>
#include <iclient.h>

IServer *g_pServer;
IServerGameClients *g_pServerGameClients;

class CBaseClient;
class CClientFrame;
class bf_write;

namespace safeproxy {

namespace {

CDetour *s_SVComputeClientPacks_detour;
CDetour *s_GetPackedEntity_detour;
CDetour *s_WriteDeltaEntities_detour;
CDetour *s_GetReplayDelay_detour;

DETOUR_DECL_STATIC3(Detour_SV_ComputeClientPacks, void, int, clientCount, CGameClient **, clients, CFrameSnapshot *, snapshot)
{
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
	IClient *iclient = reinterpret_cast<IClient *>(base + g_Offsets.CBaseClient_IClientOffset);
	// SourceTV and Replay demo writers call this with their bot. Demos keep the real state.
	int slot = iclient->IsFakeClient() ? -1 : iclient->GetPlayerSlot();

	const void *baseline = *reinterpret_cast<void **>(base + g_Offsets.CBaseClient_m_pBaseline);
	const int &baseline_update_tick = *reinterpret_cast<int *>(base + g_Offsets.CBaseClient_m_nBaselineUpdateTick);
	const uint32_t *baselines_sent = reinterpret_cast<const uint32_t *>(base + g_Offsets.CBaseClient_m_BaselinesSent);
	const int pending_before = baseline_update_tick;

	if (slot >= 0)
		SettleBaselineUpdate(slot, pending_before, baseline, baselines_sent);

	PreWriteDelta(slot, baseline);
	DETOUR_MEMBER_CALL(Detour_WriteDeltaEntities)(client, to, from, pBuf);
	PreWriteDelta(-1, nullptr);

	// The call sets m_nBaselineUpdateTick when it sends a baseline update.
	if (slot >= 0 && pending_before < 0 && baseline_update_tick >= 0)
		OnBaselineUpdateSent(slot, baseline_update_tick, baselines_sent);
}

// Send threads call this too. A client disconnected mid-snapshot has a null edict, which the game reads as the world.
DETOUR_DECL_MEMBER2(Detour_GetReplayDelay, int, edict_t *, player, int &, entity)
{
	if (!player)
		return 0;
	return DETOUR_MEMBER_CALL(Detour_GetReplayDelay)(player, entity);
}

} // anonymous namespace

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
