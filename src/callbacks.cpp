#include "callbacks.h"
#include "common.h"
#include "overrides.h"
#include "transmit.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <basehandle.h>
#include <const.h>
#include <dt_common.h>
#include <dt_send.h>
#include <edict.h>
#include <IForwardSys.h>
#include <iserverunknown.h>

namespace safeproxy {

namespace {

struct HookCall
{
	CBaseHandle entity;
	PropKey key;
	int tier;
	IPluginContext *owner;
	std::shared_ptr<PropHook> hook;
	int slot;
};

// Earlier callbacks can change the store.
const PropOverride *FindHook(const HookCall &call)
{
	const auto &entries = g_OverrideStore.Entries();
	auto ent_it = FindLive(entries, call.entity);
	if (ent_it == entries.end())
		return nullptr;
	auto prop_it = ent_it->second.by_prop.find(call.key);
	if (prop_it == ent_it->second.by_prop.end())
		return nullptr;
	const Layers<PropLayer> *layers = prop_it->second.tiers.Tier(call.tier);
	const PropLayer *layer = layers ? layers->Find(call.owner) : nullptr;
	return layer && layer->hook == call.hook ? &prop_it->second : nullptr;
}

bool Changed(IPluginFunction *fn)
{
	cell_t result = Pl_Continue;
	return fn->Execute(&result) == SP_ERROR_NONE && result == Pl_Changed;
}

std::optional<PropValue> EntityValue(IPluginFunction *fn, cell_t index)
{
	if (index == -1)
		return PropValue(std::in_place_type<uint32_t>, INVALID_EHANDLE_INDEX);

	CBaseEntity *pTarget = gamehelpers->ReferenceToEntity(index);
	const CBaseHandle *target = pTarget ? &reinterpret_cast<IServerUnknown *>(pTarget)->GetRefEHandle() : nullptr;
	// SendProxy_EHandleToInt sends only MAX_EDICT_BITS of the entry.
	if (!target || target->GetEntryIndex() >= MAX_EDICTS)
	{
		g_pSM->LogError(myself, "Prop callback %s returned invalid or non-networked entity %d", fn->DebugName(), index);
		return std::nullopt;
	}
	return PropValue(std::in_place_type<uint32_t>, static_cast<uint32_t>(target->ToInt()));
}

std::optional<PropValue> ColorValue(IPluginFunction *fn, const cell_t rgba[4])
{
	Rgba value;
	for (size_t i = 0; i < value.size(); ++i)
	{
		if (rgba[i] < 0 || rgba[i] > 255)
		{
			g_pSM->LogError(myself, "Prop callback %s returned color channel out of range (%d,%d,%d,%d)", fn->DebugName(), rgba[0], rgba[1], rgba[2], rgba[3]);
			return std::nullopt;
		}
		value[i] = static_cast<uint8_t>(rgba[i]);
	}
	return PropValue(std::in_place_type<Rgba>, value);
}

std::optional<PropValue> CallHook(const HookCall &call, const PropInfo &info, int entity_index, void *ent_base)
{
	const PropHook &hook = *call.hook;
	IPluginFunction *fn = hook.callback;
	const uint8_t *field = FieldAddress(ent_base, info, entity_index);
	if (!field)
		return std::nullopt;
	const cell_t client = call.slot + 1;

	fn->PushCell(entity_index);
	fn->PushString(hook.prop_name.c_str());

	switch (hook.value_type)
	{
	case PropType::Float:
	{
		float value;
		memcpy(&value, field, sizeof(value));
		fn->PushFloatByRef(&value);
		fn->PushCell(client);
		fn->PushCell(hook.element);
		if (!Changed(fn))
			return std::nullopt;
		return PropValue(std::in_place_type<float>, value);
	}
	case PropType::Vector:
	{
		Vec3 real;
		memcpy(real.data(), field, sizeof(real));
		cell_t value[3] = { sp_ftoc(real[0]), sp_ftoc(real[1]), sp_ftoc(real[2]) };
		fn->PushArray(value, 3, SM_PARAM_COPYBACK);
		fn->PushCell(client);
		fn->PushCell(hook.element);
		if (!Changed(fn))
			return std::nullopt;
		return PropValue(std::in_place_type<Vec3>, Vec3{ sp_ctof(value[0]), sp_ctof(value[1]), sp_ctof(value[2]) });
	}
	case PropType::String:
	{
		// Could be a char array or a string_t. The proxy handles both.
		DVariant real;
		info.prop->GetProxyFn()(info.prop, field - info.actual_offset, field, &real, std::max(info.array_element, 0), entity_index);
		char value[DT_MAX_STRING_BUFFERSIZE];
		snprintf(value, sizeof(value), "%s", real.m_pString ? real.m_pString : "");
		fn->PushStringEx(value, sizeof(value), SM_PARAM_STRING_UTF8 | SM_PARAM_STRING_COPY, SM_PARAM_COPYBACK);
		fn->PushCell(sizeof(value));
		fn->PushCell(client);
		fn->PushCell(hook.element);
		if (!Changed(fn))
			return std::nullopt;
		value[sizeof(value) - 1] = '\0';
		return PropValue(std::in_place_type<std::string>, value);
	}
	case PropType::EHandle:
	{
		CBaseHandle real;
		memcpy(&real, field, sizeof(real));
		edict_t *target = gamehelpers->GetHandleEntity(real);
		cell_t value = target ? gamehelpers->IndexOfEdict(target) : -1;
		fn->PushCellByRef(&value);
		fn->PushCell(client);
		fn->PushCell(hook.element);
		if (!Changed(fn))
			return std::nullopt;
		return EntityValue(fn, value);
	}
	case PropType::Color32:
	{
		cell_t value[4];
		for (int i = 0; i < 4; ++i)
			value[i] = field[i];
		fn->PushArray(value, 4, SM_PARAM_COPYBACK);
		fn->PushCell(client);
		fn->PushCell(hook.element);
		if (!Changed(fn))
			return std::nullopt;
		return ColorValue(fn, value);
	}
	default:
	{
		uint32_t real = 0;
		memcpy(&real, field, info.field_size);
		cell_t value = FieldBitsToInt(info, real);
		fn->PushCellByRef(&value);
		fn->PushCell(client);
		fn->PushCell(hook.element);
		if (!Changed(fn))
			return std::nullopt;
		return PropValue(std::in_place_type<int>, value);
	}
	}
}

} // anonymous namespace

void EvaluateHooks(const std::bitset<SM_MAXPLAYERS> &receiving)
{
	// Main thread only, and callbacks can't re-enter PostComputePacks.
	static std::vector<HookCall> calls;

	for (const auto &[entity_index, ent] : g_OverrideStore.Entries())
	{
		if (!ent.has_hooks || !IsSnapshotEntity(entity_index))
			continue;

		std::bitset<SM_MAXPLAYERS> sent;
		for (int slot = 0; slot < SM_MAXPLAYERS; ++slot)
		{
			if (receiving.test(slot) && IsTransmittedTo(slot, entity_index))
				sent.set(slot);
		}
		if (sent.none())
			continue;

		for (const auto &[key, po] : ent.by_prop)
		{
			auto add = [&](int tier, const Layers<PropLayer> &layers) {
				for (const auto &[owner, layer] : layers.entries)
				{
					if (!layer.hook || IsSuspended(owner))
						continue;
					if (tier != kSlotAllClients)
					{
						if (sent.test(tier))
							calls.push_back({ent.handle, key, tier, owner, layer.hook, tier});
						continue;
					}
					for (int slot = 0; slot < SM_MAXPLAYERS; ++slot)
					{
						if (sent.test(slot))
							calls.push_back({ent.handle, key, tier, owner, layer.hook, slot});
					}
				}
			};
			add(kSlotAllClients, po.tiers.all_clients);
			for (const auto &[tier, layers] : po.tiers.per_client)
				add(tier, layers);
		}
	}

	for (const HookCall &call : calls)
	{
		const PropOverride *po = FindHook(call);
		IServerUnknown *unk = po ? LiveUnknown(call.entity.GetEntryIndex()) : nullptr;
		if (!unk || IsSuspended(call.owner) || !IsTransmittedTo(call.slot, call.entity.GetEntryIndex()))
			continue;

		// The callback can invalidate po.
		const PropInfo info = po->info;
		std::optional<PropValue> value = CallHook(call, info, call.entity.GetEntryIndex(), unk);
		// A recipient the callback disconnected already had its results cleared.
		if (!IsTransmittedTo(call.slot, call.entity.GetEntryIndex()))
			continue;

		PropHook &hook = *call.hook;
		if (!value)
		{
			hook.by_slot.erase(call.slot);
			continue;
		}
		if (hook.mask == ~0u)
			NormalizeValue(info, *value);
		OverrideValue result{info.type, std::move(*value), info.field_size, hook.mask};
		if (hook.mask != ~0u)
			NarrowBits(result, hook.mask);
		hook.by_slot.insert_or_assign(call.slot, std::move(result));
	}
	calls.clear();
}

} // namespace safeproxy
