#include "reencode.h"
#include "callbacks.h"
#include "common.h"
#include "overrides.h"
#include "gamedata.h"
#include "hooks.h"
#include "transmit.h"

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <const.h>
#include <edict.h>
#include <iclient.h>
#include <icvar.h>
#include <tier1/convar.h>
#include <server_class.h>
#include <dt_common.h>
#include <dt_send.h>
#include <bitbuf.h>

namespace safeproxy {

namespace {

// sv_parallel_sendsnapshot sends from worker threads.
thread_local int t_currentClientSlot = -1;

thread_local const void *t_baselineSnapshot = nullptr;

// The engine reads the "to" frame first. In replay mode it's older than this tick's snapshot.
thread_local const CFrameSnapshot *t_toSnapshot = nullptr;

// The engine reads an entering entity's "to" pack, then the "from" pack at that index, then its baseline.
thread_local const PackedEntity *t_lastToPack = nullptr;
thread_local int t_lastToEntity = -1;

// From engine/changeframelist.h.
class IChangeFrameList
{
public:
	virtual void Release() = 0;
	virtual int GetNumProps() = 0;
	virtual void SetChangeTick(const int *pPropIndices, int nPropIndices, const int iTick) = 0;
	virtual int GetPropsChangedAfterTick(int iTick, int *iOutProps, int nMaxOutProps) = 0;
	virtual IChangeFrameList *Copy() = 0;

protected:
	virtual ~IChangeFrameList() {}
};

struct Payload
{
	std::vector<uint32_t> storage;

	// Sorted flat indices of props that differ from the engine's pack.
	std::vector<int> overridden;

	int PaddedBits() const { return static_cast<int>(storage.size()) * 32; }
};

using PayloadPtr = std::shared_ptr<const Payload>;

// An empty baseline makes the engine send every prop, which decodes the same against whatever the client holds.
const Payload kNoProps{{0}};

struct EntityState
{
	// Null when the client's latest frame carried the engine's own pack.
	PayloadPtr payload;
	const ServerClass *server_class = nullptr;
	int updated_tick = INT_MIN;

	// (prop index, tick its override last started, changed or ended), ascending by prop.
	std::vector<std::pair<int, int>> changed;
};

// Makes the engine also resend the props in EntityState::changed.
class SentChangeFrameList final : public IChangeFrameList
{
public:
	IChangeFrameList *real = nullptr;
	const std::vector<std::pair<int, int>> *changed = nullptr;

	void Release() override {}
	int GetNumProps() override { return real->GetNumProps(); }
	void SetChangeTick(const int *, int, const int) override {}
	IChangeFrameList *Copy() override { return real->Copy(); }

	int GetPropsChangedAfterTick(int iTick, int *iOutProps, int nMaxOutProps) override
	{
		int n = real->GetPropsChangedAfterTick(iTick, iOutProps, nMaxOutProps);
		if (n < 0)
			return n;

		m_merged.clear();
		const int num_props = real->GetNumProps();
		const int *real_it = iOutProps, *real_end = iOutProps + n;
		for (const auto &[prop, tick] : *changed)
		{
			if (tick <= iTick || prop >= num_props)
				continue;
			while (real_it != real_end && *real_it < prop)
				m_merged.push_back(*real_it++);
			if (real_it != real_end && *real_it == prop)
				++real_it;
			m_merged.push_back(prop);
		}
		if (m_merged.empty())
			return n;

		m_merged.insert(m_merged.end(), real_it, real_end);
		n = std::min(static_cast<int>(m_merged.size()), nMaxOutProps);
		std::copy_n(m_merged.begin(), n, iOutProps);
		return n;
	}

private:
	std::vector<int> m_merged;
};

struct SlotState
{
	std::unordered_map<int, EntityState> entities;
	// Same keys as entities, for GetPackedEntity's fast path.
	std::bitset<MAX_EDICTS> tracked;

	// Ticks of the frames the engine allocated for this client, sent or not.
	std::deque<int> frame_ticks;

	std::unordered_map<int, PayloadPtr> baseline;
	int pending_baseline_tick = -1;
	std::unordered_map<int, PayloadPtr> pending_baseline;

	// Entities the client may hold an instance baseline for. A server-forced full update frees them only on the server.
	std::bitset<MAX_EDICTS> may_hold_baseline;
	std::bitset<MAX_EDICTS> baseline_unknown;

	// Scratch for the packs served to this client. One thread sends to a client at a time.
	std::vector<unsigned char> to_pack;
	std::vector<unsigned char> baseline_pack;
	SentChangeFrameList change_frames;
};

std::array<SlotState, SM_MAXPLAYERS> s_slots;

const CFrameSnapshot *s_currentSnapshot;
int s_currentTick = -1;

bool s_encoding;
bool s_evaluatingHooks;

std::optional<size_t> HashEffectiveOverrides(const EntityOverrides &ent, int slot)
{
	std::optional<size_t> h;
	for (const auto &[key, po] : ent.by_prop)
	{
		const EffectiveOverride eff = ResolveEffective(po, slot);
		if (eff.Overrides())
			h = h.value_or(0) * 31 + (PropKeyHash()(key) ^ eff.Hash());
	}
	return h;
}

bool HasSameEffectiveOverrides(const EntityOverrides &ent, int slot_a, int slot_b)
{
	for (const auto &[key, po] : ent.by_prop)
	{
		if (!ResolveEffective(po, slot_a).SameAs(ResolveEffective(po, slot_b)))
			return false;
	}
	return true;
}

struct EncodeGroup
{
	size_t hash;
	int representative;
	std::bitset<SM_MAXPLAYERS> slots;
};

struct CachedOverride
{
	std::optional<OverrideValue> value;
	uint32_t bits, mask;

	bool Matches(const EffectiveOverride &eff) const
	{
		if (bits != eff.bits || mask != eff.mask || value.has_value() != (eff.value != nullptr))
			return false;
		return !value || value->SameAs(*eff.value);
	}
};

// payload is null if the encode matched the engine's pack.
struct CachedEncode
{
	size_t hash;
	std::vector<std::pair<PropKey, CachedOverride>> values;
	PayloadPtr payload;
	int encoded_tick;
};

// Dropped on state changes. An override can bring in props the engine's encode leaves out.
// Changes the engine doesn't flag are picked up when an entry expires.
struct EntityEncodeCache
{
	const ServerClass *server_class = nullptr;
	// SPROP_ENCODED_AGAINST_TICKCOUNT props encode against it, and it moves without a state change.
	int network_base = INT_MIN;
	std::vector<uint32_t> engine_data;
	int engine_bits = -1;
	std::vector<CachedEncode> encodes;
};

std::unordered_map<int, EntityEncodeCache> s_encodeCache;

constexpr float kCachedEncodeLifetime = 0.5f;

bool MatchesCachedEncode(const CachedEncode &cached, size_t hash, const EntityOverrides &ent, int slot)
{
	if (cached.hash != hash)
		return false;

	size_t i = 0;
	for (const auto &[key, po] : ent.by_prop)
	{
		const EffectiveOverride eff = ResolveEffective(po, slot);
		if (!eff.Overrides())
			continue;
		if (i == cached.values.size() || !(cached.values[i].first == key) || !cached.values[i].second.Matches(eff))
			return false;
		++i;
	}
	return i == cached.values.size();
}

std::optional<CachedEncode> MakeCachedEncode(size_t hash, const EntityOverrides &ent, int slot, PayloadPtr payload, int tick)
{
	CachedEncode cached{hash, {}, std::move(payload), tick};
	for (const auto &[key, po] : ent.by_prop)
	{
		const EffectiveOverride eff = ResolveEffective(po, slot);
		if (!eff.Overrides())
			continue;
		// An EHandle's encode also depends on the target existing.
		if (eff.value && eff.value->type == PropType::EHandle)
			return std::nullopt;
		std::optional<OverrideValue> value;
		if (eff.value)
			value = *eff.value;
		cached.values.emplace_back(key, CachedOverride{std::move(value), eff.bits, eff.mask});
	}
	return cached;
}

// Strings swap the proxy, a longer value could overflow a char[] field of unknown size.
struct StringOverride
{
	SendProp *prop;
	int element;
	const char *value;
	SendVarProxyFn original;
};

const std::vector<StringOverride> *s_stringOverrides = nullptr;

void SafeProxyStringProxy(const SendProp *pProp, const void *pStructBase, const void *pData, DVariant *pOut, int iElement, int objectID)
{
	SendVarProxyFn original = nullptr;
	for (const StringOverride &s : *s_stringOverrides)
	{
		if (s.prop != pProp)
			continue;
		original = s.original;
		if (s.element < 0 || s.element == iElement)
		{
			pOut->m_pString = s.value;
			return;
		}
	}
	original(pProp, pStructBase, pData, pOut, iElement, objectID);
}

struct ScopedStringProxies
{
	std::vector<StringOverride> overrides;

	ScopedStringProxies() { s_stringOverrides = &overrides; }

	void Add(const PropInfo &info, const char *value)
	{
		// Swapping twice would save SafeProxyStringProxy as the original.
		auto swapped = std::find_if(overrides.begin(), overrides.end(), [&](const StringOverride &s) { return s.prop == info.prop; });
		SendVarProxyFn original = swapped != overrides.end() ? swapped->original : info.prop->GetProxyFn();
		if (swapped == overrides.end())
			info.prop->SetProxyFn(&SafeProxyStringProxy);
		overrides.push_back({info.prop, info.array_element, value, original});
	}

	~ScopedStringProxies()
	{
		s_stringOverrides = nullptr;
		for (const StringOverride &s : overrides)
			s.prop->SetProxyFn(s.original);
	}
};

struct SavedProp
{
	void *addr;
	int size;
	uint8_t bytes[sizeof(Vec3)]; // The largest stomped field is a Vector/QAngle.
};

void ApplyOverridesToEntityMemory(void *ent_base, int entity_index, const EntityOverrides &ent, int slot, std::vector<SavedProp> &out_saved, ScopedStringProxies &out_proxies)
{
	static std::vector<std::pair<const PropOverride *, EffectiveOverride>> stomps;
	out_saved.clear();
	stomps.clear();

	for (const auto &[key, po] : ent.by_prop)
	{
		const EffectiveOverride eff = ResolveEffective(po, slot);
		if (!eff.Overrides())
			continue;

		if (eff.value && eff.value->IsString())
			out_proxies.Add(po.info, eff.value->StringPtr());
		else
			stomps.emplace_back(&po, eff);
	}

	// Write narrower props last, so a vector element wins over its vector.
	std::sort(stomps.begin(), stomps.end(), [](const auto &a, const auto &b) { return a.first->info.field_size > b.first->info.field_size; });

	for (const auto &[po, eff] : stomps)
	{
		uint8_t *addr = FieldAddress(ent_base, po->info, entity_index);
		if (!addr)
			continue;
		SavedProp &saved = out_saved.emplace_back();
		saved.addr = addr;
		saved.size = po->info.field_size;
		memcpy(saved.bytes, addr, saved.size);

		if (eff.value)
		{
			eff.value->WriteTo(addr);
		}
		else
		{
			uint32_t sent = 0;
			memcpy(&sent, addr, saved.size);
			sent = (sent & ~eff.mask) | eff.bits;
			memcpy(addr, &sent, saved.size);
		}
	}
}

template <class F>
void ForEachSetBit(const uint32_t *bits, F &&f)
{
	for (int w = 0; w < MAX_EDICTS / 32; ++w)
	{
		for (uint32_t word = bits[w]; word; word &= word - 1)
			f(w * 32 + std::countr_zero(word));
	}
}

template <class T>
T ReadField(const void *base, int offset)
{
	T value;
	memcpy(&value, reinterpret_cast<const uint8_t *>(base) + offset, sizeof(value));
	return value;
}

PackedEntity *RedirectPackedEntity(std::vector<unsigned char> &buf, const PackedEntity *original, const Payload *payload, IChangeFrameList *change_frames)
{
	const int size = g_Offsets.PackedEntity_Size;
	if (static_cast<int>(buf.size()) < size)
		buf.resize(size);
	memcpy(buf.data(), original, size);

	if (payload)
	{
		const void *pData = payload->storage.data();
		int nBits = payload->PaddedBits();
		memcpy(buf.data() + g_Offsets.PackedEntity_pData, &pData, sizeof(pData));
		memcpy(buf.data() + g_Offsets.PackedEntity_nBits, &nBits, sizeof(nBits));
	}
	if (change_frames)
		memcpy(buf.data() + g_Offsets.PackedEntity_pChangeFrameList, &change_frames, sizeof(change_frames));
	return reinterpret_cast<PackedEntity *>(buf.data());
}

// CalcDelta only reports props in its "to" buffer, and either side can be missing a data table.
void DiffPacks(const SendTable *table, const void *a, int a_bits, const void *b, int b_bits, int entity, std::vector<int> &out)
{
	static int a_to_b[MAX_DATATABLE_PROPS];
	static int b_to_a[MAX_DATATABLE_PROPS];
	int n_ab = g_Offsets.SendTable_CalcDelta(table, a, a_bits, b, b_bits, a_to_b, MAX_DATATABLE_PROPS, entity);
	int n_ba = g_Offsets.SendTable_CalcDelta(table, b, b_bits, a, a_bits, b_to_a, MAX_DATATABLE_PROPS, entity);

	out.clear();
	std::set_union(a_to_b, a_to_b + std::max(n_ab, 0), b_to_a, b_to_a + std::max(n_ba, 0), std::back_inserter(out));
}

struct PayloadDiff
{
	PayloadPtr from, to;
	std::vector<int> props;
};

const std::vector<int> &DiffPayloads(std::vector<PayloadDiff> &memo, const PayloadPtr &from, const PayloadPtr &to, const ServerClass *sc, int entity)
{
	for (const PayloadDiff &d : memo)
	{
		if (d.from == from && d.to == to)
			return d.props;
	}

	PayloadDiff &d = memo.emplace_back(PayloadDiff{from, to, {}});

	static std::vector<int> both, moved;
	both.clear();
	std::set_intersection(from->overridden.begin(), from->overridden.end(), to->overridden.begin(), to->overridden.end(), std::back_inserter(both));
	if (both.empty() || from->storage == to->storage)
		return d.props;

	DiffPacks(sc->m_pTable, from->storage.data(), from->PaddedBits(), to->storage.data(), to->PaddedBits(), entity, moved);
	std::set_intersection(both.begin(), both.end(), moved.begin(), moved.end(), std::back_inserter(d.props));
	return d.props;
}

void StampChanges(EntityState &st, const std::vector<int> &props, int tick)
{
	if (props.empty())
		return;

	static std::vector<std::pair<int, int>> merged;
	merged.clear();

	auto it = st.changed.begin(), end = st.changed.end();
	for (int prop : props)
	{
		while (it != end && it->first < prop)
			merged.push_back(*it++);
		if (it != end && it->first == prop)
			++it;
		merged.emplace_back(prop, tick);
	}
	merged.insert(merged.end(), it, end);
	st.changed.swap(merged);
}

void Transition(EntityState &st, const ServerClass *sc, const PayloadPtr &payload, int entity, int tick, std::vector<PayloadDiff> *memo)
{
	st.updated_tick = tick;
	if (st.server_class == sc && st.payload == payload)
		return;
	// A different class at this index is a new entity. The engine sends it as a create, so the old stamps don't apply.
	if (st.server_class != sc)
	{
		st.payload.reset();
		st.server_class = sc;
		st.changed.clear();
	}

	static const std::vector<int> kNone;
	const std::vector<int> &before = st.payload ? st.payload->overridden : kNone;
	const std::vector<int> &after = payload ? payload->overridden : kNone;

	static std::vector<int> changed, merged;
	changed.clear();
	std::set_symmetric_difference(before.begin(), before.end(), after.begin(), after.end(), std::back_inserter(changed));
	if (st.payload && payload)
	{
		const std::vector<int> &moved = DiffPayloads(*memo, st.payload, payload, sc, entity);
		merged.clear();
		std::set_union(changed.begin(), changed.end(), moved.begin(), moved.end(), std::back_inserter(merged));
		changed.swap(merged);
	}

	StampChanges(st, changed, tick);
	st.payload = payload;
}

// The engine keeps up to max(MAX_CLIENT_FRAMES, sv_maxreplay / interval) frames per client.
constexpr int kEngineMinClientFrames = 128;
ConVar *s_maxReplay;

int MaxClientFrames()
{
	return std::max(kEngineMinClientFrames, static_cast<int>(s_maxReplay->GetFloat() / gpGlobals->interval_per_tick));
}

void NoteClientFrame(SlotState &s, int tick)
{
	s.frame_ticks.push_back(tick);
	// The engine also drops at most one frame per new one. After sv_maxreplay is lowered it can still delta from older frames.
	if (static_cast<int>(s.frame_ticks.size()) > MaxClientFrames())
		s.frame_ticks.pop_front();
}

void FinishSlotFrame(SlotState &s, int tick)
{
	// Acks only name frames the engine still keeps. Once MaxClientFrames() frames are kept, the client has every change up to the oldest.
	const int oldest = static_cast<int>(s.frame_ticks.size()) >= MaxClientFrames() ? s.frame_ticks.front() : INT_MIN;
	for (auto it = s.entities.begin(); it != s.entities.end(); )
	{
		EntityState &st = it->second;
		if (st.updated_tick != tick)
			Transition(st, ResolveServerClass(it->first), nullptr, it->first, tick, nullptr);

		std::erase_if(st.changed, [&](const auto &change) { return change.second <= oldest; });
		if (!st.payload && st.changed.empty())
		{
			s.tracked.reset(it->first);
			it = s.entities.erase(it);
		}
		else
		{
			++it;
		}
	}
}

std::vector<SavedProp> s_saved;

bool EncodeWithOverrides(ServerClass *sc, int entity_index, const EntityOverrides &ent_ov, int slot, const void *engine_data, int engine_bits, PayloadPtr &out)
{
	constexpr size_t kScratchWords = PAD_NUMBER(MAX_PACKEDENTITY_DATA, sizeof(uint32_t)) / sizeof(uint32_t);
	static uint32_t scratch_storage[kScratchWords];
	static std::vector<int> overridden;

	void *ent_base = LiveUnknown(entity_index);
	if (!ent_base)
		return false;

	ScopedStringProxies str_proxies;
	ApplyOverridesToEntityMemory(ent_base, entity_index, ent_ov, slot, s_saved, str_proxies);

	bf_write buf("SafeProxy", reinterpret_cast<char *>(scratch_storage), static_cast<int>(sizeof(scratch_storage)));

	bool ok = g_Offsets.SendTable_Encode(sc->m_pTable, ent_base, &buf, entity_index, nullptr, false);

	// Disconnecting a player from one of its send proxies deletes it.
	if (LiveUnknown(entity_index) != ent_base)
	{
		s_saved.clear();
		return false;
	}

	// Restore in reverse so overlapping saves unwind to the original bytes.
	for (auto it = s_saved.rbegin(); it != s_saved.rend(); ++it)
		memcpy(it->addr, it->bytes, it->size);
	s_saved.clear();

	// SendTable_Encode checks for overflow before writing its last bits.
	if (!ok || buf.IsOverflowed())
	{
		if (buf.IsOverflowed())
			g_pSM->LogError(myself, "Overrides exceed the packed entity size for entity %d (%s)", entity_index, sc->GetName());
		else
			g_pSM->LogError(myself, "SendTable_Encode failed for entity %d (%s)", entity_index, sc->GetName());
		return false;
	}

	const int bits = buf.GetNumBitsWritten();
	const int words = PAD_NUMBER(BitByte(bits), sizeof(uint32_t)) / sizeof(uint32_t);
	// Clear bits left over from earlier encodes, so equal encodes compare equal.
	if (bits & 31)
		scratch_storage[words - 1] &= (1u << (bits & 31)) - 1;

	DiffPacks(sc->m_pTable, engine_data, engine_bits, scratch_storage, PAD_NUMBER(bits, 32), entity_index, overridden);
	if (overridden.empty())
	{
		out.reset();
		return true;
	}

	auto payload = std::make_shared<Payload>();
	payload->storage.assign(scratch_storage, scratch_storage + words);
	payload->overridden = overridden;
	out = std::move(payload);
	return true;
}

void EncodeOverrides(CFrameSnapshot *snapshot, const std::bitset<SM_MAXPLAYERS> &receiving, int tick)
{
	// PostComputePacks only runs on the main thread.
	static std::vector<EncodeGroup> groups;
	static std::vector<CachedEncode> fresh;
	static std::vector<PayloadDiff> memo;

	const int max_age = static_cast<int>(kCachedEncodeLifetime / gpGlobals->interval_per_tick);

	for (const auto &[entity_index, ent_ov] : g_OverrideStore.Entries())
	{
		// Only snapshot entities that some client transmits are packed.
		if (!IsSnapshotEntity(entity_index))
			continue;
		const PackedEntity *engine_pack = EngineGetPackedEntity(snapshot, entity_index);
		if (!engine_pack)
			continue;
		const void *engine_data = ReadField<const void *>(engine_pack, g_Offsets.PackedEntity_pData);
		const int engine_bits = ReadField<int>(engine_pack, g_Offsets.PackedEntity_nBits);
		// FLAG_IS_COMPRESSED is the sign bit. Only the HLTV relay's client state compresses packs.
		if (engine_bits < 0)
			continue;

		ServerClass *sc = ResolveServerClass(entity_index);
		if (!sc)
			continue;

		const auto slot_mask = ent_ov.has_all_clients_layers ? receiving : ent_ov.per_client_slots & receiving;

		groups.clear();
		// Clients without layers of their own all get the all-clients layers. Resolve them once.
		bool shared_resolved = false;
		int shared_group = -1;
		for (int slot = 0; slot < SM_MAXPLAYERS; ++slot)
		{
			if (!slot_mask.test(slot) || !IsTransmittedTo(slot, entity_index))
				continue;

			const bool shared = !ent_ov.per_client_slots.test(slot) && !ent_ov.has_all_clients_hooks;
			if (shared && shared_resolved)
			{
				if (shared_group >= 0)
					groups[shared_group].slots.set(slot);
				continue;
			}
			shared_resolved |= shared;

			const std::optional<size_t> hash = HashEffectiveOverrides(ent_ov, slot);
			if (!hash)
				continue;
			auto group = std::find_if(groups.begin(), groups.end(), [&](const EncodeGroup &g) {
				return g.hash == *hash && HasSameEffectiveOverrides(ent_ov, slot, g.representative);
			});
			if (group == groups.end())
				group = groups.insert(groups.end(), EncodeGroup{*hash, slot, {}});
			group->slots.set(slot);
			if (shared)
				shared_group = static_cast<int>(group - groups.begin());
		}
		if (groups.empty())
			continue;

		EntityEncodeCache &cache = s_encodeCache[entity_index];
		const int network_base = gpGlobals->GetNetworkBase(tick, entity_index);
		const int engine_words = PAD_NUMBER(engine_bits, 32) / 32;
		if (cache.server_class != sc || cache.network_base != network_base || cache.engine_bits != engine_bits
			|| memcmp(cache.engine_data.data(), engine_data, engine_words * sizeof(uint32_t)) != 0)
		{
			cache.server_class = sc;
			cache.network_base = network_base;
			cache.engine_bits = engine_bits;
			cache.engine_data.assign(static_cast<const uint32_t *>(engine_data), static_cast<const uint32_t *>(engine_data) + engine_words);
			cache.encodes.clear();
		}
		// Keep entries a client still holds. Not every client gets a snapshot every tick.
		std::erase_if(cache.encodes, [&](const CachedEncode &c) {
			return tick - c.encoded_tick > max_age || (c.payload && c.payload.use_count() == 1);
		});

		fresh.clear();
		memo.clear();
		for (const EncodeGroup &g : groups)
		{
			PayloadPtr payload;
			auto hit = std::find_if(cache.encodes.begin(), cache.encodes.end(), [&](const CachedEncode &c) {
				return MatchesCachedEncode(c, g.hash, ent_ov, g.representative);
			});
			if (hit != cache.encodes.end())
			{
				payload = hit->payload;
			}
			else
			{
				if (!EncodeWithOverrides(sc, entity_index, ent_ov, g.representative, engine_data, engine_bits, payload))
					continue;
				if (auto cached = MakeCachedEncode(g.hash, ent_ov, g.representative, payload, tick))
					fresh.push_back(std::move(*cached));
			}
			if (!payload)
				continue;

			for (int slot = 0; slot < SM_MAXPLAYERS; ++slot)
			{
				if (!g.slots.test(slot))
					continue;
				SlotState &s = s_slots[slot];
				Transition(s.entities[entity_index], sc, payload, entity_index, tick, &memo);
				s.tracked.set(entity_index);
			}
		}

		std::move(fresh.begin(), fresh.end(), std::back_inserter(cache.encodes));
	}
}

} // anonymous namespace

bool InitReencode(char *error, size_t maxlen)
{
	s_maxReplay = g_pCVar->FindVar("sv_maxreplay");
	if (!s_maxReplay)
	{
		snprintf(error, maxlen, "Could not find ConVar 'sv_maxreplay'");
		return false;
	}
	return true;
}

void ClearReencodeState()
{
	for (auto &s : s_slots)
		s = SlotState{};
	s_encodeCache.clear();
	s_currentSnapshot = nullptr;
	s_currentTick = -1;
}

void ClearReencodeSlot(int slot)
{
	s_slots[slot] = SlotState{};
}

void AssumeUnknownBaselines()
{
	for (int client = 1; client <= playerhelpers->GetMaxClients(); ++client)
	{
		IGamePlayer *player = playerhelpers->GetGamePlayer(client);
		if (player && player->IsConnected() && !player->IsFakeClient())
		{
			s_slots[client - 1].may_hold_baseline.set();
			s_slots[client - 1].baseline_unknown.set();
		}
	}
}

bool IsEncoding()
{
	return s_encoding;
}

bool IsEvaluatingHooks()
{
	return s_evaluatingHooks;
}

void ReadRealBytes(const void *addr, int size, void *out)
{
	memcpy(out, addr, size);

	// The earliest save of a byte holds its real value.
	const auto *lo = static_cast<const uint8_t *>(addr);
	for (auto it = s_saved.rbegin(); it != s_saved.rend(); ++it)
	{
		const auto *saved_lo = static_cast<const uint8_t *>(it->addr);
		const uint8_t *begin = std::max(lo, saved_lo);
		const uint8_t *end = std::min(lo + size, saved_lo + it->size);
		if (begin < end)
			memcpy(static_cast<uint8_t *>(out) + (begin - lo), it->bytes + (begin - saved_lo), end - begin);
	}
}

void NoteStateChanges()
{
	std::erase_if(s_encodeCache, [](const auto &entry) {
		edict_t *edict = gamehelpers->EdictOfIndex(entry.first);
		return edict && edict->HasStateChanged();
	});
}

void PreComputePacks(int clientCount)
{
	// Entries outlive their entity until here. Packing looks them up by index.
	g_OverrideStore.PurgeStale();
	g_TransmitStore.PurgeStale();

	std::erase_if(s_encodeCache, [](const auto &entry) { return !g_OverrideStore.Entries().contains(entry.first); });

	BeginTransmitPass(clientCount);
}

void PostComputePacks(int clientCount, CGameClient **clients, CFrameSnapshot *snapshot)
{
	const int tick = gpGlobals->tickcount;

	std::bitset<SM_MAXPLAYERS> receiving;
	for (int i = 0; i < clientCount; ++i)
	{
		IClient *iclient = reinterpret_cast<IClient *>(reinterpret_cast<uintptr_t>(clients[i]) + kBaseClientIClientOffset);
		int slot = iclient->GetPlayerSlot();
		if (iclient->IsFakeClient())
		{
			IGamePlayer *player = playerhelpers->GetGamePlayer(slot + 1);
			if (player && (player->IsSourceTV() || player->IsReplay()))
				NoteTransmitted(slot, tick);
			continue;
		}
		receiving.set(slot);
		NoteClientFrame(s_slots[slot], tick);
		NoteTransmitted(slot, tick);
	}

	s_evaluatingHooks = true;
	EvaluateHooks(receiving);
	s_evaluatingHooks = false;

	if (!g_OverrideStore.Empty())
	{
		s_encoding = true;
		EncodeOverrides(snapshot, receiving, tick);
		s_encoding = false;
		ApplyDeferredSuspensions();
	}

	for (int slot = 0; slot < SM_MAXPLAYERS; ++slot)
	{
		if (receiving.test(slot))
			FinishSlotFrame(s_slots[slot], tick);
	}

	// Published last, so a send that runs before the slot states are complete gets the engine's packs.
	s_currentSnapshot = snapshot;
	s_currentTick = tick;
}

PackedEntity *OnGetPackedEntity(CFrameSnapshot *pSnapshot, int entity, PackedEntity *original)
{
	if (t_currentClientSlot < 0)
		return original;

	SlotState &s = s_slots[t_currentClientSlot];

	// Entering the PVS deltas from the baseline, so serve the one this client adopted.
	if (pSnapshot == t_baselineSnapshot)
	{
		if (!original)
		{
			if (!s.may_hold_baseline.test(entity) || t_lastToEntity != entity)
				return original;
			// The engine only uses a baseline pack with the entering entity's server class.
			return RedirectPackedEntity(s.baseline_pack, t_lastToPack, &kNoProps, nullptr);
		}
		if (s.baseline_unknown.test(entity))
			return RedirectPackedEntity(s.baseline_pack, original, &kNoProps, nullptr);
		auto it = s.baseline.find(entity);
		if (it == s.baseline.end())
			return original;
		return RedirectPackedEntity(s.baseline_pack, original, it->second.get(), nullptr);
	}

	if (!t_toSnapshot)
		t_toSnapshot = pSnapshot;
	if (!original)
		return original;
	if (pSnapshot == t_toSnapshot)
	{
		t_lastToPack = original;
		t_lastToEntity = entity;
	}

	// With the change frame list below, the engine only reads the "to" pack's data.
	if (pSnapshot != s_currentSnapshot)
		return original;

	if (!s.tracked.test(entity)) [[likely]]
		return original;

	const EntityState &st = s.entities.find(entity)->second;
	IChangeFrameList *real = ReadField<IChangeFrameList *>(original, g_Offsets.PackedEntity_pChangeFrameList);

	// Game-server packs always have a change frame list.
	if (!real)
		return st.payload ? RedirectPackedEntity(s.to_pack, original, st.payload.get(), nullptr) : original;

	s.change_frames.real = real;
	s.change_frames.changed = &st.changed;
	return RedirectPackedEntity(s.to_pack, original, st.payload.get(), &s.change_frames);
}

void PreWriteDelta(int slot, const void *baseline)
{
	t_currentClientSlot = slot;
	t_baselineSnapshot = baseline;
	t_toSnapshot = nullptr;
	t_lastToEntity = -1;
}

void OnBaselineUpdateSent(int slot, int tick, const uint32_t *baselines_sent)
{
	SlotState &s = s_slots[slot];
	s.pending_baseline_tick = tick;
	s.pending_baseline.clear();
	// The client stores these as soon as the packet arrives, accepted or not.
	ForEachSetBit(baselines_sent, [&](int entity) { s.may_hold_baseline.set(entity); });

	// The baseline entities were written from this tick's "to" packs.
	// A replay-mode client is sent an older, unredirected frame, so it adopts the engine's packs.
	if (tick != s_currentTick)
		return;
	ForEachSetBit(baselines_sent, [&](int entity) {
		if (!s.tracked.test(entity))
			return;
		const EntityState &st = s.entities.find(entity)->second;
		if (st.payload)
			s.pending_baseline.emplace(entity, st.payload);
	});
}

void OnBaselineAccepted(int slot, int tick, const uint32_t *baselines_sent)
{
	SlotState &s = s_slots[slot];
	// An update sent before the load stays unknown.
	if (tick != s.pending_baseline_tick)
		return;

	ForEachSetBit(baselines_sent, [&](int entity) {
		if (auto it = s.pending_baseline.find(entity); it != s.pending_baseline.end())
			s.baseline[entity] = it->second;
		else
			s.baseline.erase(entity);
		s.baseline_unknown.reset(entity);
	});

	s.pending_baseline_tick = -1;
	s.pending_baseline.clear();
}

void OnBaselineAckFailed(int slot, const uint32_t *baselines_sent)
{
	SlotState &s = s_slots[slot];
	ForEachSetBit(baselines_sent, [&](int entity) { s.baseline_unknown.set(entity); });
}

} // namespace safeproxy
