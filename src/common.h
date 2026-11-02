#ifndef _INCLUDE_SAFEPROXY_COMMON_H_
#define _INCLUDE_SAFEPROXY_COMMON_H_

#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>

#include <basehandle.h>
#include <sp_vm_api.h>

class IServerUnknown;

namespace safeproxy {

using SourcePawn::IPluginContext;

// Matches SAFEPROXY_ALL_CLIENTS in the include.
constexpr int kAllClientsRecipient = -2;
constexpr int kSlotAllClients = -1;

constexpr int RecipientToSlot(int recipient)
{
	return recipient == kAllClientsRecipient ? kSlotAllClients : recipient - 1;
}

// A paused or failed plugin's layers stay stored but don't apply.
bool IsSuspended(IPluginContext *owner);
void SetSuspended(IPluginContext *owner, bool suspended);
void ApplyDeferredSuspensions();
void ClearSuspensions();

template <class T>
struct Layers
{
	std::vector<std::pair<IPluginContext *, T>> entries;

	bool Empty() const { return entries.empty(); }

	bool AnyUnsuspended() const
	{
		for (const auto &[owner, value] : entries)
		{
			if (!IsSuspended(owner))
				return true;
		}
		return false;
	}

	const T *Find(IPluginContext *owner) const
	{
		for (const auto &[layer_owner, value] : entries)
		{
			if (layer_owner == owner)
				return &value;
		}
		return nullptr;
	}

	// Editing in place keeps the layer's precedence.
	T *Find(IPluginContext *owner)
	{
		return const_cast<T *>(std::as_const(*this).Find(owner));
	}

	void Set(IPluginContext *owner, T value)
	{
		// A periodic refresh of the same value shouldn't retake precedence.
		if (const T *own = Find(owner); own && *own == value)
			return;
		Remove(owner);
		entries.emplace_back(owner, std::move(value));
	}

	bool Remove(IPluginContext *owner)
	{
		return std::erase_if(entries, [&](const auto &entry) { return entry.first == owner; }) != 0;
	}
};

template <class T>
struct Tiers
{
	Layers<T> all_clients;
	std::unordered_map<int, Layers<T>> per_client; // Never holds empty layers.

	bool Empty() const { return all_clients.Empty() && per_client.empty(); }

	const Layers<T> *Tier(int slot) const
	{
		if (slot == kSlotAllClients)
			return all_clients.Empty() ? nullptr : &all_clients;
		auto it = per_client.find(slot);
		return it == per_client.end() ? nullptr : &it->second;
	}

	Layers<T> *Tier(int slot)
	{
		return const_cast<Layers<T> *>(std::as_const(*this).Tier(slot));
	}

	const Layers<T> *Effective(int slot) const
	{
		if (const Layers<T> *own = Tier(slot); own && own->AnyUnsuspended())
			return own;
		return all_clients.AnyUnsuspended() ? &all_clients : nullptr;
	}

	void Set(int slot, IPluginContext *owner, T value)
	{
		(slot == kSlotAllClients ? all_clients : per_client[slot]).Set(owner, std::move(value));
	}

	bool Remove(int slot, IPluginContext *owner)
	{
		if (slot == kSlotAllClients)
			return all_clients.Remove(owner);
		auto it = per_client.find(slot);
		if (it == per_client.end() || !it->second.Remove(owner))
			return false;
		if (it->second.Empty())
			per_client.erase(it);
		return true;
	}

	int RemoveOwner(IPluginContext *owner)
	{
		int n = all_clients.Remove(owner) ? 1 : 0;
		for (auto it = per_client.begin(); it != per_client.end(); )
		{
			n += it->second.Remove(owner) ? 1 : 0;
			it = it->second.Empty() ? per_client.erase(it) : std::next(it);
		}
		return n;
	}

	void RemoveSlot(int slot) { per_client.erase(slot); }
};

// Null if the edict is free or has no entity, like a disconnected player's.
IServerUnknown *LiveUnknown(int entity_index);

CBaseHandle LiveHandle(int entity_index);

template <class EntityMap>
auto FindLive(EntityMap &entries, const CBaseHandle &entity)
{
	auto it = entries.find(entity.GetEntryIndex());
	return it != entries.end() && it->second.handle == entity ? it : entries.end();
}

template <class EntityMap>
auto &ClaimEntry(EntityMap &entries, const CBaseHandle &entity)
{
	auto &entry = entries[entity.GetEntryIndex()];
	if (entry.handle != entity)
	{
		entry = typename EntityMap::mapped_type{};
		entry.handle = entity;
	}
	return entry;
}

template <class EntityMap>
void EraseStale(EntityMap &entries)
{
	std::erase_if(entries, [](const auto &kv) { return kv.second.handle != LiveHandle(kv.first); });
}

} // namespace safeproxy

#endif // _INCLUDE_SAFEPROXY_COMMON_H_
