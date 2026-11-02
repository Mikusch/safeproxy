#ifndef _INCLUDE_SAFEPROXY_TRANSMIT_H_
#define _INCLUDE_SAFEPROXY_TRANSMIT_H_

#include "smsdk_ext.h"
#include "common.h"

#include <cstdint>
#include <optional>
#include <unordered_map>

class CCheckTransmitInfo;
class CGlobalVars;
class IServerGameEnts;

extern IServerGameEnts *g_pServerGameEnts;
extern CGlobalVars *gpGlobals;

namespace safeproxy {

// Matches SafeProxyTransmit in the include.
enum class TransmitState : uint8_t
{
	Normal = 0,
	Hide = 1,
	Force = 2,
	ForceWithChildren = 3,
};

struct EntityTransmitOverrides
{
	CBaseHandle handle;
	Tiers<TransmitState> tiers;
};

class TransmitStore
{
public:
	void Set(const CBaseHandle &entity, int slot, TransmitState state, IPluginContext *owner);
	bool Unset(const CBaseHandle &entity, int slot, IPluginContext *owner);

	std::optional<TransmitState> Query(const CBaseHandle &entity, int slot) const;
	bool IsHidden(const CBaseHandle &entity, int slot) const;

	int ClearEntity(const CBaseHandle &entity, IPluginContext *owner);
	int ClearSlot(int slot, IPluginContext *owner);
	int ClearOwner(IPluginContext *owner);

	bool Empty() const { return m_entries.empty(); }

	void ApplyToTransmitInfo(CCheckTransmitInfo *pInfo, int slot, const unsigned short *edicts, int edict_count) const;

	void PurgeStale();

	void OnClientDisconnected(int client);
	void Clear();

private:
	using EntityMap = std::unordered_map<int, EntityTransmitOverrides>;

	template <class Fn>
	int PruneEntities(Fn &&prune);

	EntityMap m_entries;
};

extern TransmitStore g_TransmitStore;

void BeginTransmitPass(int client_count);
bool IsSnapshotEntity(int entity_index);
bool IsTransmittedTo(int slot, int entity_index);

// Disconnecting frees the client frames its transmit info points into.
void ForgetTransmitInfo(int slot);

void NoteTransmitted(int slot, int tick);
bool WasTransmitted(int slot, const CBaseHandle &entity);

bool InstallTransmitHook(char *error, size_t maxlen);
void UnloadTransmitHook();

} // namespace safeproxy

#endif // _INCLUDE_SAFEPROXY_TRANSMIT_H_
