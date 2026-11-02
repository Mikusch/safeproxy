#include "transmit.h"

#include "common.h"
#include "gamedata.h"
#include "reencode.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <vector>

#include <basehandle.h>
#include <const.h>
#include <eiface.h>
#include <iservernetworkable.h>

IServerGameEnts *g_pServerGameEnts;
CGlobalVars *gpGlobals;

SH_DECL_HOOK3_void(IServerGameEnts, CheckTransmit, SH_NOATTRIB, 0, CCheckTransmitInfo *, const unsigned short *, int);

namespace safeproxy {

TransmitStore g_TransmitStore;

namespace {

int s_CheckTransmitPostHookID;

int s_moveParentOffset;

int GetMoveParentIndex(int entity_index)
{
	// A SetTransmit hook can remove an entity mid-pass.
	IServerUnknown *unk = LiveUnknown(entity_index);
	if (!unk)
		return -1;
	CBaseHandle *pHandle = reinterpret_cast<CBaseHandle *>(reinterpret_cast<uint8_t *>(unk) + s_moveParentOffset);
	return gamehelpers->GetHandleEntity(*pHandle) ? pHandle->GetEntryIndex() : -1;
}

constexpr int kMoveParentUncomputed = -2;
int s_moveParentCacheTick = -1;
std::array<int, MAX_EDICTS> s_moveParentCache;

int GetMoveParentIndexCached(int entity_index)
{
	if (gpGlobals->tickcount != s_moveParentCacheTick)
	{
		s_moveParentCache.fill(kMoveParentUncomputed);
		s_moveParentCacheTick = gpGlobals->tickcount;
	}

	int &cached = s_moveParentCache[entity_index];
	if (cached == kMoveParentUncomputed)
		cached = GetMoveParentIndex(entity_index);
	return cached;
}

// Every CheckTransmit in a pass gets the same edict list.
std::bitset<MAX_EDICTS> s_snapshotEntities;
bool s_snapshotEntitiesPending;

// Transmit bits are only final after every CheckTransmit hook has run.
std::array<const CCheckTransmitInfo *, SM_MAXPLAYERS> s_transmitInfo;

// The info is CGameClient::m_PackInfo, and its m_pTransmitEdict points into the current frame.
void DropCurrentFrame(const CCheckTransmitInfo *info)
{
	uint8_t *base = reinterpret_cast<uint8_t *>(const_cast<CCheckTransmitInfo *>(info));
	uint8_t *&frame = *reinterpret_cast<uint8_t **>(base + g_Offsets.CGameClient_m_pCurrentFrame);
	if (frame && reinterpret_cast<const uint8_t *>(info->m_pTransmitEdict) == frame + g_Offsets.CClientFrame_transmit_entity)
		frame = nullptr;
}

struct SentEntities
{
	CBitVec<MAX_EDICTS> bits;
	int tick = -1;
};
std::array<SentEntities, SM_MAXPLAYERS> s_sent;

std::array<CBaseHandle, MAX_EDICTS> s_occupants;
std::array<int, MAX_EDICTS> s_occupiedSince;

// SV_ComputeClientPacks runs one CheckTransmit per client, then packs.
int s_checkTransmitsLeft;

void Hook_CheckTransmit_Post(CCheckTransmitInfo *pInfo, const unsigned short *pEdictIndices, int nEdicts)
{
	if (s_snapshotEntitiesPending)
	{
		for (int i = 0; i < nEdicts; ++i)
		{
			const int entity_index = pEdictIndices[i];
			s_snapshotEntities.set(entity_index);
			if (CBaseHandle occupant = LiveHandle(entity_index); occupant != s_occupants[entity_index])
			{
				s_occupants[entity_index] = occupant;
				s_occupiedSince[entity_index] = gpGlobals->tickcount;
			}
		}
		s_snapshotEntitiesPending = false;
	}

	int client = gamehelpers->IndexOfEdict(pInfo->m_pClientEnt);
	if (client >= 1 && client <= SM_MAXPLAYERS)
		s_transmitInfo[client - 1] = pInfo;

	// Only SourceTV and Replay get m_pTransmitAlways. Demos keep the real state.
	// TF2's CBaseObject::SetTransmit dereferences the recipient's entity.
	if (!g_TransmitStore.Empty() && !pInfo->m_pTransmitAlways && pInfo->m_pClientEnt->GetUnknown())
		g_TransmitStore.ApplyToTransmitInfo(pInfo, client - 1, pEdictIndices, nEdicts);

	// SetTransmit hooks can still raise state-change flags until the last CheckTransmit.
	if (s_checkTransmitsLeft > 0 && --s_checkTransmitsLeft == 0)
		NoteStateChanges();
	RETURN_META(MRES_IGNORED);
}

// Hide wins, otherwise the highest state.
TransmitState Compose(const Layers<TransmitState> &layers)
{
	TransmitState out = TransmitState::Normal;
	for (const auto &[owner, state] : layers.entries)
	{
		if (IsSuspended(owner))
			continue;
		if (state == TransmitState::Hide)
			return state;
		out = std::max(out, state);
	}
	return out;
}

bool IsUnderHide(int entity_index, int self, const std::bitset<MAX_EDICTS> &hidden, std::array<int8_t, MAX_EDICTS> &memo)
{
	if (entity_index <= 0 || entity_index == self)
		return false;
	int8_t &known = memo[entity_index];
	if (known < 0)
		known = hidden.test(entity_index) || IsUnderHide(GetMoveParentIndexCached(entity_index), self, hidden, memo);
	return known;
}

bool HasUnsentAncestor(int entity_index, int self, const CBitVec<MAX_EDICTS> &sent, std::array<int8_t, MAX_EDICTS> &memo)
{
	int8_t &known = memo[entity_index];
	if (known < 0)
	{
		const int parent = GetMoveParentIndexCached(entity_index);
		known = parent > 0 && parent != self && (!sent.Get(parent) || HasUnsentAncestor(parent, self, sent, memo));
	}
	return known;
}

// SH_MCALL skips the next hooked call, a moveparent's SetTransmit when this entity's vtable slot isn't hooked.
void CallOriginalSetTransmit(CBaseEntity *entity, CCheckTransmitInfo *pInfo)
{
	void **vfnptr = *reinterpret_cast<void ***>(entity) + g_Offsets.CBaseEntity_SetTransmit;
	void *original = SH_GLOB_SHPTR->GetOrigVfnPtrEntry(vfnptr);
	union
	{
		void (SourceHook::EmptyClass::*mfp)(CCheckTransmitInfo *, bool);
		struct
		{
			void *addr;
			intptr_t adjustor;
		} s;
	} u;
	u.s.addr = original ? original : *vfnptr;
	u.s.adjustor = 0;
	(reinterpret_cast<SourceHook::EmptyClass *>(entity)->*u.mfp)(pInfo, true);
}

} // anonymous namespace

void BeginTransmitPass(int client_count)
{
	s_snapshotEntities.reset();
	s_snapshotEntitiesPending = true;
	s_transmitInfo.fill(nullptr);
	s_checkTransmitsLeft = client_count;
}

bool IsSnapshotEntity(int entity_index)
{
	return s_snapshotEntities.test(entity_index);
}

bool IsTransmittedTo(int slot, int entity_index)
{
	const CCheckTransmitInfo *info = s_transmitInfo[slot];
	return info && info->m_pTransmitEdict->Get(entity_index);
}

void ForgetTransmitInfo(int slot)
{
	// A client disconnected by a prop callback still gets this snapshot from its freed frame. The engine skips a client without a frame.
	if (s_transmitInfo[slot] && IsEvaluatingHooks())
		DropCurrentFrame(s_transmitInfo[slot]);
	s_transmitInfo[slot] = nullptr;
}

void NoteTransmitted(int slot, int tick)
{
	if (const CCheckTransmitInfo *info = s_transmitInfo[slot])
	{
		info->m_pTransmitEdict->CopyTo(&s_sent[slot].bits);
		s_sent[slot].tick = tick;
	}
}

bool WasTransmitted(int slot, const CBaseHandle &entity)
{
	const int entity_index = entity.GetEntryIndex();
	const SentEntities &sent = s_sent[slot];
	if (entity_index >= MAX_EDICTS || sent.tick < 0 || !sent.bits.Get(entity_index))
		return false;
	return s_occupants[entity_index] == entity && s_occupiedSince[entity_index] <= sent.tick;
}

void TransmitStore::Set(const CBaseHandle &entity, int slot, TransmitState state, IPluginContext *owner)
{
	ClaimEntry(m_entries, entity).tiers.Set(slot, owner, state);
}

bool TransmitStore::Unset(const CBaseHandle &entity, int slot, IPluginContext *owner)
{
	auto it = FindLive(m_entries, entity);
	if (it == m_entries.end())
		return false;

	bool removed = it->second.tiers.Remove(slot, owner);
	if (it->second.tiers.Empty())
		m_entries.erase(it);
	return removed;
}

std::optional<TransmitState> TransmitStore::Query(const CBaseHandle &entity, int slot) const
{
	auto it = FindLive(m_entries, entity);
	if (it == m_entries.end())
		return std::nullopt;

	const Layers<TransmitState> *layers = it->second.tiers.Effective(slot);
	return layers ? std::optional<TransmitState>(Compose(*layers)) : std::nullopt;
}

bool TransmitStore::IsHidden(const CBaseHandle &entity, int slot) const
{
	const int entity_index = entity.GetEntryIndex();
	if (m_entries.empty() || entity_index >= MAX_EDICTS)
		return false;

	IGamePlayer *player = playerhelpers->GetGamePlayer(slot + 1);
	if (player && (player->IsSourceTV() || player->IsReplay()))
		return false;

	// Cached moveparents can be stale here, they're filled during the transmit pass.
	const int self = slot + 1;
	for (int parent = GetMoveParentIndex(self); parent > 0; parent = GetMoveParentIndex(parent))
	{
		if (parent == entity_index)
			return false;
	}

	for (int e = entity_index; e > 0 && e != self; e = GetMoveParentIndex(e))
	{
		auto it = FindLive(m_entries, LiveHandle(e));
		if (it == m_entries.end())
			continue;
		const Layers<TransmitState> *layers = it->second.tiers.Effective(slot);
		if (layers && Compose(*layers) == TransmitState::Hide)
			return true;
	}
	return false;
}

template <class Fn>
int TransmitStore::PruneEntities(Fn &&prune)
{
	// Don't count layers on removed entities.
	EraseStale(m_entries);
	int n = 0;
	for (auto it = m_entries.begin(); it != m_entries.end(); )
	{
		n += prune(it->second.tiers);
		it = it->second.tiers.Empty() ? m_entries.erase(it) : std::next(it);
	}
	return n;
}

int TransmitStore::ClearEntity(const CBaseHandle &entity, IPluginContext *owner)
{
	auto it = FindLive(m_entries, entity);
	if (it == m_entries.end())
		return 0;

	int n = it->second.tiers.RemoveOwner(owner);
	if (it->second.tiers.Empty())
		m_entries.erase(it);
	return n;
}

int TransmitStore::ClearSlot(int slot, IPluginContext *owner)
{
	return PruneEntities([&](Tiers<TransmitState> &tiers) { return tiers.Remove(slot, owner) ? 1 : 0; });
}

int TransmitStore::ClearOwner(IPluginContext *owner)
{
	return PruneEntities([&](Tiers<TransmitState> &tiers) { return tiers.RemoveOwner(owner); });
}

void TransmitStore::ApplyToTransmitInfo(CCheckTransmitInfo *pInfo, int slot, const unsigned short *edicts, int edict_count) const
{
	// Hides never reach or pass through the recipient's own player. A dormant local player or view model breaks prediction.
	const int self = slot + 1;

	std::bitset<MAX_EDICTS> hidden, forces_children, has_entry;
	// CheckTransmit only runs on the main thread.
	static std::vector<int> forced;
	forced.clear();
	for (const auto &[entity_index, ent] : m_entries)
	{
		const Layers<TransmitState> *layers = ent.tiers.Effective(slot);
		if (!layers)
			continue;
		has_entry.set(entity_index);

		const TransmitState state = Compose(*layers);
		if (state == TransmitState::Hide)
		{
			hidden.set(entity_index);
		}
		else if (state >= TransmitState::Force)
		{
			forced.push_back(entity_index);
			forces_children[entity_index] = state == TransmitState::ForceWithChildren;
		}
	}

	if (forces_children.any())
	{
		for (int i = 0; i < edict_count; ++i)
		{
			int entity_index = edicts[i];
			if (has_entry.test(entity_index))
				continue;

			for (int parent = GetMoveParentIndexCached(entity_index); parent > 0; parent = GetMoveParentIndexCached(parent))
			{
				if (forces_children.test(parent))
				{
					forced.push_back(entity_index);
					break;
				}
			}
		}
	}

	if (forced.empty() && hidden.none())
		return;

	// The recipient's own player is always sent.
	// A child sent without its moveparent is misplaced on the client.
	std::bitset<MAX_EDICTS> self_chain;
	for (int parent = GetMoveParentIndexCached(self); parent > 0; parent = GetMoveParentIndexCached(parent))
		self_chain.set(parent);

	std::array<int8_t, MAX_EDICTS> under_hide;
	if (hidden.any())
		under_hide.fill(-1);
	auto is_hidden = [&](int entity_index) {
		return hidden.any() && !self_chain.test(entity_index) && IsUnderHide(entity_index, self, hidden, under_hide);
	};

	if (!forced.empty())
	{
		CBitVec<MAX_EDICTS> before;
		pInfo->m_pTransmitEdict->CopyTo(&before);

		bool walked = false;
		for (int entity_index : forced)
		{
			// A transmit bit without a snapshot entry is a Host_Error in SV_WriteEnterPVS.
			// Walking a hidden entity would still mark the ancestors above the Hide.
			if (pInfo->m_pTransmitEdict->Get(entity_index) || !IsSnapshotEntity(entity_index) || is_hidden(entity_index))
				continue;

			IServerUnknown *unk = LiveUnknown(entity_index);
			if (!unk)
				continue;

			CallOriginalSetTransmit(unk->GetBaseEntity(), pInfo);
			walked = true;
		}

		// Ancestors' SetTransmit hooks still fire. One that blocks orphans the forced descendants.
		if (walked)
		{
			std::array<int8_t, MAX_EDICTS> orphaned;
			orphaned.fill(-1);
			for (int i = 0; i < edict_count; ++i)
			{
				int entity_index = edicts[i];
				if (pInfo->m_pTransmitEdict->Get(entity_index) && !before.Get(entity_index) && !self_chain.test(entity_index) && HasUnsentAncestor(entity_index, self, *pInfo->m_pTransmitEdict, orphaned))
					pInfo->m_pTransmitEdict->Clear(entity_index);
			}
		}
	}

	// Hides run last. SetTransmit also marks entities outside the moveparent chain, like weapons.
	if (hidden.none())
		return;

	for (int i = 0; i < edict_count; ++i)
	{
		int entity_index = edicts[i];
		if (pInfo->m_pTransmitEdict->Get(entity_index) && is_hidden(entity_index))
			pInfo->m_pTransmitEdict->Clear(entity_index);
	}
}

void TransmitStore::PurgeStale()
{
	EraseStale(m_entries);
}

void TransmitStore::OnClientDisconnected(int client)
{
	PruneEntities([&](Tiers<TransmitState> &tiers) { tiers.RemoveSlot(client - 1); return 0; });
	s_sent[client - 1].tick = -1;
}

void TransmitStore::Clear()
{
	m_entries.clear();
	s_moveParentCacheTick = -1;
	for (SentEntities &sent : s_sent)
		sent.tick = -1;
	s_occupants.fill(CBaseHandle());
}

bool InstallTransmitHook(char *error, size_t maxlen)
{
	sm_sendprop_info_t info;
	if (!gamehelpers->FindSendPropInfo("CBaseEntity", "moveparent", &info))
	{
		snprintf(error, maxlen, "Could not find SendProp CBaseEntity::moveparent");
		return false;
	}
	s_moveParentOffset = info.actual_offset;

	s_CheckTransmitPostHookID = SH_ADD_HOOK(IServerGameEnts, CheckTransmit, g_pServerGameEnts, SH_STATIC(Hook_CheckTransmit_Post), true);
	return true;
}

void UnloadTransmitHook()
{
	if (s_CheckTransmitPostHookID)
	{
		SH_REMOVE_HOOK_ID(s_CheckTransmitPostHookID);
		s_CheckTransmitPostHookID = 0;
	}
}

} // namespace safeproxy
