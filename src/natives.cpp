#include "natives.h"
#include "overrides.h"
#include "reencode.h"
#include "transmit.h"

#include <memory>
#include <utility>

#include <basehandle.h>
#include <dt_send.h>
#include <iserverunknown.h>

namespace safeproxy {

namespace {

bool ReadRecipient(IPluginContext *pContext, cell_t recipient, int &slot)
{
	const int max_clients = playerhelpers->GetMaxClients();
	if (recipient != kAllClientsRecipient && (recipient < 1 || recipient > max_clients))
	{
		pContext->ThrowNativeError("Invalid recipient %d", recipient);
		return false;
	}
	slot = RecipientToSlot(recipient);
	return true;
}

// Overrides on an empty slot would be cleared when the next client connects.
bool RequireConnectedClient(IPluginContext *pContext, cell_t recipient)
{
	if (recipient != kAllClientsRecipient && !playerhelpers->GetGamePlayer(recipient)->IsConnected())
	{
		pContext->ThrowNativeError("Client %d is not connected", recipient);
		return false;
	}
	return true;
}

bool RequirePropsWritable(IPluginContext *pContext)
{
	if (!IsEncoding())
		return true;
	pContext->ThrowNativeError("Cannot change prop overrides while entities are being encoded");
	return false;
}

bool LookupEntityArg(cell_t param, CBaseHandle &entity)
{
	CBaseEntity *pEntity = gamehelpers->ReferenceToEntity(param);
	if (!pEntity)
		return false;
	entity = reinterpret_cast<IServerUnknown *>(pEntity)->GetRefEHandle();
	return true;
}

bool ResolveEntityArg(IPluginContext *pContext, cell_t param, CBaseHandle &entity)
{
	if (LookupEntityArg(param, entity))
		return true;
	pContext->ThrowNativeError("Entity %d (%d) is invalid", gamehelpers->ReferenceToIndex(param), param);
	return false;
}

struct PropArgs
{
	CBaseHandle entity;
	PropInfo info;
	cell_t recipient;
	int slot;
};

bool ReadPropArgs(IPluginContext *pContext, const cell_t *params, int recipient_param, int element_param, std::optional<PropType> required, PropArgs &out)
{
	if (!ResolveEntityArg(pContext, params[1], out.entity))
		return false;
	out.recipient = params[recipient_param];
	if (!ReadRecipient(pContext, out.recipient, out.slot))
		return false;

	char *prop_name;
	pContext->LocalToString(params[2], &prop_name);
	char err[256];
	if (!OverrideStore::Resolve(out.entity, prop_name, params[element_param], required, &out.info, err, sizeof(err)))
	{
		pContext->ThrowNativeError("%s", err);
		return false;
	}
	return true;
}

cell_t SetProp(IPluginContext *pContext, const cell_t *params, PropType type, PropValue value)
{
	PropArgs args;
	if (!RequirePropsWritable(pContext) || !ReadPropArgs(pContext, params, /*recipient=*/4, /*element=*/5, type, args) || !RequireConnectedClient(pContext, args.recipient))
		return 0;

	g_OverrideStore.Set(args.entity, args.info, args.slot, std::move(value), pContext);
	return 0;
}

// native void SafeProxy_SetProp(int entity, const char[] prop, int value, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_SetProp(IPluginContext *pContext, const cell_t *params)
{
	return SetProp(pContext, params, PropType::Int, PropValue(std::in_place_type<int>, params[3]));
}

// native void SafeProxy_SetPropFloat(int entity, const char[] prop, float value, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_SetPropFloat(IPluginContext *pContext, const cell_t *params)
{
	return SetProp(pContext, params, PropType::Float, PropValue(std::in_place_type<float>, sp_ctof(params[3])));
}

// native void SafeProxy_SetPropVector(int entity, const char[] prop, const float value[3], int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_SetPropVector(IPluginContext *pContext, const cell_t *params)
{
	cell_t *addr;
	pContext->LocalToPhysAddr(params[3], &addr);
	Vec3 value = { sp_ctof(addr[0]), sp_ctof(addr[1]), sp_ctof(addr[2]) };
	return SetProp(pContext, params, PropType::Vector, PropValue(std::in_place_type<Vec3>, value));
}

// native void SafeProxy_SetPropString(int entity, const char[] prop, const char[] value, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_SetPropString(IPluginContext *pContext, const cell_t *params)
{
	char *value;
	pContext->LocalToString(params[3], &value);
	return SetProp(pContext, params, PropType::String, PropValue(std::in_place_type<std::string>, value));
}

// native void SafeProxy_SetPropEnt(int entity, const char[] prop, int other, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_SetPropEnt(IPluginContext *pContext, const cell_t *params)
{
	uint32_t handle = INVALID_EHANDLE_INDEX;
	if (params[3] != -1)
	{
		CBaseEntity *pTarget = gamehelpers->ReferenceToEntity(params[3]);
		if (!pTarget)
			return pContext->ThrowNativeError("Target entity %d is invalid", params[3]);
		const CBaseHandle &target = reinterpret_cast<IServerUnknown *>(pTarget)->GetRefEHandle();
		// SendProxy_EHandleToInt sends only MAX_EDICT_BITS of the entry.
		if (target.GetEntryIndex() >= MAX_EDICTS)
			return pContext->ThrowNativeError("Target entity %d is not networked", params[3]);
		handle = static_cast<uint32_t>(target.ToInt());
	}
	return SetProp(pContext, params, PropType::EHandle, PropValue(std::in_place_type<uint32_t>, handle));
}

// native void SafeProxy_SetPropColor(int entity, const char[] prop, const int rgba[4], int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_SetPropColor(IPluginContext *pContext, const cell_t *params)
{
	cell_t *addr;
	pContext->LocalToPhysAddr(params[3], &addr);

	Rgba rgba;
	for (size_t i = 0; i < rgba.size(); ++i)
	{
		if (addr[i] < 0 || addr[i] > 255)
			return pContext->ThrowNativeError("Color channel out of range (%d,%d,%d,%d)", addr[0], addr[1], addr[2], addr[3]);
		rgba[i] = static_cast<uint8_t>(addr[i]);
	}
	return SetProp(pContext, params, PropType::Color32, PropValue(std::in_place_type<Rgba>, rgba));
}

// native void SafeProxy_SetPropReal(int entity, const char[] prop, int recipient, int element = 0);
cell_t Native_SetPropReal(IPluginContext *pContext, const cell_t *params)
{
	PropArgs args;
	if (!RequirePropsWritable(pContext) || !ReadPropArgs(pContext, params, /*recipient=*/3, /*element=*/4, std::nullopt, args) || !RequireConnectedClient(pContext, args.recipient))
		return 0;
	if (args.slot == kSlotAllClients)
		return pContext->ThrowNativeError("SafeProxy_SetPropReal only applies to client recipients");

	g_OverrideStore.Set(args.entity, args.info, args.slot, std::nullopt, pContext);
	return 0;
}

// native void SafeProxy_SetPropBits(int entity, const char[] prop, int bits, int mask, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_SetPropBits(IPluginContext *pContext, const cell_t *params)
{
	PropArgs args;
	if (!RequirePropsWritable(pContext) || !ReadPropArgs(pContext, params, /*recipient=*/5, /*element=*/6, PropType::Int, args) || !RequireConnectedClient(pContext, args.recipient))
		return 0;

	const uint32_t mask = static_cast<uint32_t>(params[4]);
	if (mask & FieldMask(args.info))
		g_OverrideStore.SetBits(args.entity, args.info, args.slot, static_cast<uint32_t>(params[3]), mask, pContext);
	return 0;
}

// StringToLocalUTF8 reads and can write before the buffer when maxlen is 1.
void WriteLocalString(IPluginContext *pContext, cell_t addr, cell_t maxlen, const char *source)
{
	if (maxlen > 1)
	{
		pContext->StringToLocalUTF8(addr, maxlen, source, nullptr);
	}
	else if (maxlen == 1)
	{
		char *dest;
		if (pContext->LocalToString(addr, &dest) == SP_ERROR_NONE)
			*dest = '\0';
	}
}

bool GetProp(IPluginContext *pContext, const cell_t *params, int recipient_param, int element_param, std::optional<PropType> type, PropArgs &args, EffectiveOverride &out)
{
	return ReadPropArgs(pContext, params, recipient_param, element_param, type, args) && g_OverrideStore.Query(args.entity, args.info, args.slot, out);
}

uint32_t SentBits(const PropArgs &args, const EffectiveOverride &eff)
{
	const int entity_index = args.entity.GetEntryIndex();
	const uint8_t *field = FieldAddress(gamehelpers->ReferenceToEntity(entity_index), args.info, entity_index);
	uint32_t real = 0;
	if (field)
		ReadRealBytes(field, args.info.field_size, &real);
	return (real & ~eff.mask) | eff.bits;
}

// native SafeProxyPropType SafeProxy_GetPropType(int entity, const char[] prop, int element = 0, char[] error = "", int maxlen = 0);
cell_t Native_GetPropType(IPluginContext *pContext, const cell_t *params)
{
	CBaseHandle entity;
	if (!ResolveEntityArg(pContext, params[1], entity))
		return 0;

	char *prop_name;
	pContext->LocalToString(params[2], &prop_name);
	PropInfo info;
	char err[256] = "";
	const bool ok = OverrideStore::Resolve(entity, prop_name, params[3], std::nullopt, &info, err, sizeof(err));
	WriteLocalString(pContext, params[4], params[5], err);
	// SafeProxyPropType is PropType + 1, with Unsupported at 0.
	return ok ? 1 + static_cast<cell_t>(info.type) : 0;
}

// native bool SafeProxy_HasProp(int entity, const char[] prop, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_HasProp(IPluginContext *pContext, const cell_t *params)
{
	PropArgs args;
	EffectiveOverride eff;
	return GetProp(pContext, params, /*recipient=*/3, /*element=*/4, std::nullopt, args, eff) ? 1 : 0;
}

// native bool SafeProxy_GetProp(int entity, const char[] prop, int &value, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_GetProp(IPluginContext *pContext, const cell_t *params)
{
	PropArgs args;
	EffectiveOverride eff;
	if (!GetProp(pContext, params, /*recipient=*/4, /*element=*/5, PropType::Int, args, eff))
		return 0;

	cell_t *addr;
	pContext->LocalToPhysAddr(params[3], &addr);
	*addr = FieldBitsToInt(args.info, SentBits(args, eff));
	return 1;
}

// native bool SafeProxy_GetPropFloat(int entity, const char[] prop, float &value, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_GetPropFloat(IPluginContext *pContext, const cell_t *params)
{
	PropArgs args;
	EffectiveOverride eff;
	if (!GetProp(pContext, params, /*recipient=*/4, /*element=*/5, PropType::Float, args, eff))
		return 0;

	cell_t *addr;
	pContext->LocalToPhysAddr(params[3], &addr);
	*addr = sp_ftoc(std::get<float>(eff.value->v));
	return 1;
}

// native bool SafeProxy_GetPropVector(int entity, const char[] prop, float value[3], int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_GetPropVector(IPluginContext *pContext, const cell_t *params)
{
	PropArgs args;
	EffectiveOverride eff;
	if (!GetProp(pContext, params, /*recipient=*/4, /*element=*/5, PropType::Vector, args, eff))
		return 0;

	cell_t *addr;
	pContext->LocalToPhysAddr(params[3], &addr);
	const auto &value = std::get<Vec3>(eff.value->v);
	for (size_t i = 0; i < value.size(); ++i)
		addr[i] = sp_ftoc(value[i]);
	return 1;
}

// native bool SafeProxy_GetPropString(int entity, const char[] prop, char[] buffer, int maxlen, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_GetPropString(IPluginContext *pContext, const cell_t *params)
{
	PropArgs args;
	EffectiveOverride eff;
	if (!GetProp(pContext, params, /*recipient=*/5, /*element=*/6, PropType::String, args, eff))
		return 0;

	WriteLocalString(pContext, params[3], params[4], eff.value->StringPtr());
	return 1;
}

// native bool SafeProxy_GetPropEnt(int entity, const char[] prop, int &other, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_GetPropEnt(IPluginContext *pContext, const cell_t *params)
{
	PropArgs args;
	EffectiveOverride eff;
	if (!GetProp(pContext, params, /*recipient=*/4, /*element=*/5, PropType::EHandle, args, eff))
		return 0;

	uint32_t raw = std::get<uint32_t>(eff.value->v);
	CBaseHandle handle(raw & ENT_ENTRY_MASK, raw >> NUM_SERIAL_NUM_SHIFT_BITS);
	edict_t *target = gamehelpers->GetHandleEntity(handle);

	cell_t *addr;
	pContext->LocalToPhysAddr(params[3], &addr);
	*addr = target ? gamehelpers->IndexOfEdict(target) : -1;
	return 1;
}

// native bool SafeProxy_GetPropColor(int entity, const char[] prop, int rgba[4], int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_GetPropColor(IPluginContext *pContext, const cell_t *params)
{
	PropArgs args;
	EffectiveOverride eff;
	if (!GetProp(pContext, params, /*recipient=*/4, /*element=*/5, PropType::Color32, args, eff))
		return 0;

	const uint32_t bits = SentBits(args, eff);

	cell_t *addr;
	pContext->LocalToPhysAddr(params[3], &addr);
	for (int i = 0; i < 4; ++i)
		addr[i] = (bits >> (8 * i)) & 0xFF;
	return 1;
}

// native bool SafeProxy_GetPropBits(int entity, const char[] prop, int &bits, int &mask, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_GetPropBits(IPluginContext *pContext, const cell_t *params)
{
	PropArgs args;
	EffectiveOverride eff;
	if (!GetProp(pContext, params, /*recipient=*/5, /*element=*/6, PropType::Int, args, eff))
		return 0;

	cell_t *bits, *mask;
	pContext->LocalToPhysAddr(params[3], &bits);
	pContext->LocalToPhysAddr(params[4], &mask);
	*bits = static_cast<cell_t>(eff.bits);
	*mask = static_cast<cell_t>(eff.mask);
	return 1;
}

// A removed or non-networked entity returns false instead of throwing.
bool ReadUnsetArgs(IPluginContext *pContext, const cell_t *params, int recipient_param, int element_param, std::optional<PropType> required, PropArgs &args)
{
	CBaseHandle entity;
	int slot;
	if (!RequirePropsWritable(pContext) || !ReadRecipient(pContext, params[recipient_param], slot) || !LookupEntityArg(params[1], entity) || !ResolveServerClass(entity.GetEntryIndex()))
		return false;
	return ReadPropArgs(pContext, params, recipient_param, element_param, required, args);
}

// native bool SafeProxy_UnsetProp(int entity, const char[] prop, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_UnsetProp(IPluginContext *pContext, const cell_t *params)
{
	PropArgs args;
	if (!ReadUnsetArgs(pContext, params, /*recipient=*/3, /*element=*/4, std::nullopt, args))
		return 0;
	return g_OverrideStore.Unset(args.entity, args.info, args.slot, pContext) ? 1 : 0;
}

// native bool SafeProxy_UnsetPropBits(int entity, const char[] prop, int mask, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_UnsetPropBits(IPluginContext *pContext, const cell_t *params)
{
	PropArgs args;
	if (!ReadUnsetArgs(pContext, params, /*recipient=*/4, /*element=*/5, PropType::Int, args))
		return 0;
	return g_OverrideStore.UnsetBits(args.entity, args.info, args.slot, static_cast<uint32_t>(params[3]), pContext) ? 1 : 0;
}

cell_t SetPropCallback(IPluginContext *pContext, const cell_t *params, int recipient_param, int element_param, PropType type, std::optional<uint32_t> bits_mask)
{
	PropArgs args;
	if (!RequirePropsWritable(pContext) || !ReadPropArgs(pContext, params, recipient_param, element_param, type, args) || !RequireConnectedClient(pContext, args.recipient))
		return 0;

	IPluginFunction *callback = pContext->GetFunctionById(static_cast<funcid_t>(params[3]));
	if (!callback)
		return pContext->ThrowNativeError("Invalid callback %x", params[3]);

	uint32_t mask = ~0u;
	if (bits_mask)
	{
		mask = *bits_mask & FieldMask(args.info);
		if (!mask)
		{
			g_OverrideStore.Unset(args.entity, args.info, args.slot, pContext);
			return 0;
		}
	}

	char *prop_name;
	pContext->LocalToString(params[2], &prop_name);

	auto hook = std::make_shared<PropHook>();
	hook->callback = callback;
	hook->value_type = type;
	hook->prop_name = prop_name;
	hook->element = params[element_param];
	hook->mask = mask;
	g_OverrideStore.SetHook(args.entity, args.info, args.slot, std::move(hook), pContext);
	return 0;
}

// native void SafeProxy_SetPropCallback(int entity, const char[] prop, SafeProxyCallback callback, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_SetPropCallback(IPluginContext *pContext, const cell_t *params)
{
	return SetPropCallback(pContext, params, /*recipient=*/4, /*element=*/5, PropType::Int, std::nullopt);
}

// native void SafeProxy_SetPropFloatCallback(int entity, const char[] prop, SafeProxyCallbackFloat callback, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_SetPropFloatCallback(IPluginContext *pContext, const cell_t *params)
{
	return SetPropCallback(pContext, params, /*recipient=*/4, /*element=*/5, PropType::Float, std::nullopt);
}

// native void SafeProxy_SetPropVectorCallback(int entity, const char[] prop, SafeProxyCallbackVector callback, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_SetPropVectorCallback(IPluginContext *pContext, const cell_t *params)
{
	return SetPropCallback(pContext, params, /*recipient=*/4, /*element=*/5, PropType::Vector, std::nullopt);
}

// native void SafeProxy_SetPropStringCallback(int entity, const char[] prop, SafeProxyCallbackString callback, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_SetPropStringCallback(IPluginContext *pContext, const cell_t *params)
{
	return SetPropCallback(pContext, params, /*recipient=*/4, /*element=*/5, PropType::String, std::nullopt);
}

// native void SafeProxy_SetPropEntCallback(int entity, const char[] prop, SafeProxyCallbackEnt callback, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_SetPropEntCallback(IPluginContext *pContext, const cell_t *params)
{
	return SetPropCallback(pContext, params, /*recipient=*/4, /*element=*/5, PropType::EHandle, std::nullopt);
}

// native void SafeProxy_SetPropColorCallback(int entity, const char[] prop, SafeProxyCallbackColor callback, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_SetPropColorCallback(IPluginContext *pContext, const cell_t *params)
{
	return SetPropCallback(pContext, params, /*recipient=*/4, /*element=*/5, PropType::Color32, std::nullopt);
}

// native void SafeProxy_SetPropBitsCallback(int entity, const char[] prop, SafeProxyCallback callback, int mask, int recipient = SAFEPROXY_ALL_CLIENTS, int element = 0);
cell_t Native_SetPropBitsCallback(IPluginContext *pContext, const cell_t *params)
{
	return SetPropCallback(pContext, params, /*recipient=*/5, /*element=*/6, PropType::Int, static_cast<uint32_t>(params[4]));
}

// native void SafeProxy_SetTransmit(int entity, SafeProxyTransmit state, int recipient = SAFEPROXY_ALL_CLIENTS);
cell_t Native_SetTransmit(IPluginContext *pContext, const cell_t *params)
{
	CBaseHandle entity;
	if (!ResolveEntityArg(pContext, params[1], entity))
		return 0;
	if (entity.GetEntryIndex() == 0)
		return pContext->ThrowNativeError("Cannot override transmit for worldspawn");
	if (!ResolveServerClass(entity.GetEntryIndex()))
		return pContext->ThrowNativeError("Entity %d (%d) is not networked", entity.GetEntryIndex(), params[1]);

	int slot;
	if (!ReadRecipient(pContext, params[3], slot) || !RequireConnectedClient(pContext, params[3]))
		return 0;

	const cell_t state = params[2];
	if (state < static_cast<cell_t>(TransmitState::Normal) || state > static_cast<cell_t>(TransmitState::ForceWithChildren))
		return pContext->ThrowNativeError("Invalid SafeProxyTransmit value %d", state);
	if (state == static_cast<cell_t>(TransmitState::Normal) && slot == kSlotAllClients)
		return pContext->ThrowNativeError("SafeProxyTransmit_Normal only applies to client recipients");

	g_TransmitStore.Set(entity, slot, static_cast<TransmitState>(state), pContext);
	return 0;
}

// native bool SafeProxy_GetTransmit(int entity, SafeProxyTransmit &state, int recipient = SAFEPROXY_ALL_CLIENTS);
cell_t Native_GetTransmit(IPluginContext *pContext, const cell_t *params)
{
	CBaseHandle entity;
	int slot;
	if (!ResolveEntityArg(pContext, params[1], entity) || !ReadRecipient(pContext, params[3], slot))
		return 0;

	auto ov = g_TransmitStore.Query(entity, slot);
	if (!ov)
		return 0;

	cell_t *addr;
	pContext->LocalToPhysAddr(params[2], &addr);
	*addr = static_cast<cell_t>(*ov);
	return 1;
}

// native bool SafeProxy_IsTransmitted(int entity, int client);
cell_t Native_IsTransmitted(IPluginContext *pContext, const cell_t *params)
{
	CBaseHandle entity;
	if (!ResolveEntityArg(pContext, params[1], entity))
		return 0;

	const cell_t client = params[2];
	const int max_clients = playerhelpers->GetMaxClients();
	if (client < 1 || client > max_clients)
		return pContext->ThrowNativeError("Client index %d is invalid", client);

	return WasTransmitted(client - 1, entity) ? 1 : 0;
}

// native bool SafeProxy_IsHidden(int entity, int client);
cell_t Native_IsHidden(IPluginContext *pContext, const cell_t *params)
{
	CBaseHandle entity;
	if (!ResolveEntityArg(pContext, params[1], entity))
		return 0;

	const cell_t client = params[2];
	const int max_clients = playerhelpers->GetMaxClients();
	if (client < 1 || client > max_clients)
		return pContext->ThrowNativeError("Client index %d is invalid", client);

	return g_TransmitStore.IsHidden(entity, client - 1) ? 1 : 0;
}

// native bool SafeProxy_UnsetTransmit(int entity, int recipient = SAFEPROXY_ALL_CLIENTS);
cell_t Native_UnsetTransmit(IPluginContext *pContext, const cell_t *params)
{
	CBaseHandle entity;
	int slot;
	if (!ReadRecipient(pContext, params[2], slot) || !LookupEntityArg(params[1], entity))
		return 0;

	return g_TransmitStore.Unset(entity, slot, pContext) ? 1 : 0;
}

// native int SafeProxy_ClearProps(int entity);
cell_t Native_ClearProps(IPluginContext *pContext, const cell_t *params)
{
	CBaseHandle entity;
	if (!RequirePropsWritable(pContext) || !LookupEntityArg(params[1], entity))
		return 0;

	return g_OverrideStore.ClearEntity(entity, pContext);
}

// native int SafeProxy_ClearTransmit(int entity);
cell_t Native_ClearTransmit(IPluginContext *pContext, const cell_t *params)
{
	CBaseHandle entity;
	if (!LookupEntityArg(params[1], entity))
		return 0;

	return g_TransmitStore.ClearEntity(entity, pContext);
}

// native int SafeProxy_ClearRecipient(int recipient);
cell_t Native_ClearRecipient(IPluginContext *pContext, const cell_t *params)
{
	int slot;
	if (!RequirePropsWritable(pContext) || !ReadRecipient(pContext, params[1], slot))
		return 0;

	return g_OverrideStore.ClearSlot(slot, pContext) + g_TransmitStore.ClearSlot(slot, pContext);
}

// native int SafeProxy_ClearAll();
cell_t Native_ClearAll(IPluginContext *pContext, const cell_t *params)
{
	if (!RequirePropsWritable(pContext))
		return 0;
	return g_OverrideStore.ClearOwner(pContext) + g_TransmitStore.ClearOwner(pContext);
}

} // anonymous namespace

const sp_nativeinfo_t g_Natives[] = {
	// Prop overrides
	{ "SafeProxy_SetProp",               Native_SetProp },
	{ "SafeProxy_SetPropFloat",          Native_SetPropFloat },
	{ "SafeProxy_SetPropVector",         Native_SetPropVector },
	{ "SafeProxy_SetPropString",         Native_SetPropString },
	{ "SafeProxy_SetPropEnt",            Native_SetPropEnt },
	{ "SafeProxy_SetPropColor",          Native_SetPropColor },
	{ "SafeProxy_SetPropReal",           Native_SetPropReal },
	{ "SafeProxy_SetPropBits",           Native_SetPropBits },
	{ "SafeProxy_GetPropType",           Native_GetPropType },
	{ "SafeProxy_HasProp",               Native_HasProp },
	{ "SafeProxy_GetProp",               Native_GetProp },
	{ "SafeProxy_GetPropFloat",          Native_GetPropFloat },
	{ "SafeProxy_GetPropVector",         Native_GetPropVector },
	{ "SafeProxy_GetPropString",         Native_GetPropString },
	{ "SafeProxy_GetPropEnt",            Native_GetPropEnt },
	{ "SafeProxy_GetPropColor",          Native_GetPropColor },
	{ "SafeProxy_GetPropBits",           Native_GetPropBits },
	{ "SafeProxy_UnsetProp",             Native_UnsetProp },
	{ "SafeProxy_UnsetPropBits",         Native_UnsetPropBits },

	// Prop callbacks
	{ "SafeProxy_SetPropCallback",       Native_SetPropCallback },
	{ "SafeProxy_SetPropFloatCallback",  Native_SetPropFloatCallback },
	{ "SafeProxy_SetPropVectorCallback", Native_SetPropVectorCallback },
	{ "SafeProxy_SetPropStringCallback", Native_SetPropStringCallback },
	{ "SafeProxy_SetPropEntCallback",    Native_SetPropEntCallback },
	{ "SafeProxy_SetPropColorCallback",  Native_SetPropColorCallback },
	{ "SafeProxy_SetPropBitsCallback",   Native_SetPropBitsCallback },

	// Transmit control
	{ "SafeProxy_SetTransmit",           Native_SetTransmit },
	{ "SafeProxy_GetTransmit",           Native_GetTransmit },
	{ "SafeProxy_IsTransmitted",         Native_IsTransmitted },
	{ "SafeProxy_IsHidden",              Native_IsHidden },
	{ "SafeProxy_UnsetTransmit",         Native_UnsetTransmit },

	// Clearing
	{ "SafeProxy_ClearProps",            Native_ClearProps },
	{ "SafeProxy_ClearTransmit",         Native_ClearTransmit },
	{ "SafeProxy_ClearRecipient",        Native_ClearRecipient },
	{ "SafeProxy_ClearAll",              Native_ClearAll },

	{ nullptr,                           nullptr },
};

} // namespace safeproxy
