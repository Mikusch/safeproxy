#ifndef _INCLUDE_SAFEPROXY_OVERRIDES_H_
#define _INCLUDE_SAFEPROXY_OVERRIDES_H_

#include "smsdk_ext.h"
#include "common.h"

#include <array>
#include <bitset>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

class SendProp;
class ServerClass;
class CStandardSendProxies;
typedef void (*SendVarProxyFn)(const SendProp *pProp, const void *pStructBase, const void *pData, class DVariant *pOut, int iElement, int objectID);

extern const CStandardSendProxies *g_pStdProxies;

namespace safeproxy {

void AutoDetectTransformingProxies();

ServerClass *ResolveServerClass(int entity_index);

enum class PropType : uint8_t
{
	Int,
	Float,
	Vector,
	String,
	EHandle,
	Color32,
};

inline bool IsBitwise(PropType type) { return type == PropType::Int || type == PropType::Color32; }

using Vec3 = std::array<float, 3>;
using Rgba = std::array<uint8_t, 4>;
static_assert(sizeof(Vec3) == sizeof(float) * 3 && sizeof(Rgba) == 4);

using PropValue = std::variant<
	int,
	float,
	Vec3,
	std::string,
	uint32_t, // EHandle (CBaseHandle::m_Index)
	Rgba
>;

// A DataTable on a prop's path whose proxy moves its data off the entity, like the gamerules table.
struct Relocation
{
	const SendProp *table = nullptr;
	unsigned int offset = 0;
};

constexpr int kMaxRelocations = 4;

struct PropInfo
{
	SendProp *prop = nullptr;
	unsigned int actual_offset = 0; // From the entity base, or the last relocation's base.
	PropType type = PropType::Int;
	int field_size = 0;
	int array_element = -1; // Only set for DPT_Array elements (they share one SendProp).
	std::array<Relocation, kMaxRelocations> relocations{};
	int relocation_count = 0;

	bool Relocated() const { return relocation_count > 0; }
};

// Calls the relocating DataTable proxies like the encoder. Null if one of them skips its table.
uint8_t *FieldAddress(void *ent_base, const PropInfo &info, int entity_index);

struct OverrideValue
{
	PropType type = PropType::Int;
	PropValue v;
	int field_size = 0;
	uint32_t mask = ~0u; // Bitwise props: the field bits this layer sets.

	bool IsString() const { return type == PropType::String; }

	const char *StringPtr() const;

	void WriteTo(void *dst) const;

	uint32_t Bits() const;

	// Compares bytes so -0.0f and 0.0f aren't equal.
	bool SameAs(const OverrideValue &other) const;
	bool operator==(const OverrideValue &other) const { return SameAs(other); }
	size_t Hash() const;
};

// Several SendProps can send one field, but a string override swaps its SendProp's proxy.
struct PropKey
{
	const SendProp *prop;
	unsigned int offset;
	int field_size;
	bool operator==(const PropKey &) const = default;
};

struct PropKeyHash
{
	size_t operator()(const PropKey &k) const
	{
		return std::hash<const void *>()(k.prop) ^ (static_cast<size_t>(k.offset) * 0x9E3779B9u) ^ (static_cast<size_t>(k.field_size) << 24);
	}
};

PropKey KeyOf(const PropInfo &info);

// Nonzero becomes 1 on 1-bit props, like SetEntProp. Otherwise 2 would be sent as 0.
void NormalizeValue(const PropInfo &info, PropValue &value);

int FieldBitsToInt(const PropInfo &info, uint32_t bits);

uint32_t FieldMask(const PropInfo &info);

void NarrowBits(OverrideValue &value, uint32_t mask);

struct PropHook
{
	IPluginFunction *callback = nullptr;
	PropType value_type = PropType::Int; // Can differ from the prop's type on 32-bit int and color props.
	std::string prop_name;
	int element = 0;
	uint32_t mask = ~0u;

	// What the callback last returned for each client slot, kept only while it returns Plugin_Changed.
	std::unordered_map<int, OverrideValue> by_slot;

	const OverrideValue *Result(int slot) const;
	bool SameCall(const PropHook &other) const;
};

struct PropLayer
{
	std::optional<OverrideValue> value;
	std::shared_ptr<PropHook> hook;

	bool IsPassthrough() const { return !value && !hook; }
	bool operator==(const PropLayer &other) const;
};

struct PropOverride
{
	PropInfo info;
	Tiers<PropLayer> tiers;
};

struct EffectiveOverride
{
	const OverrideValue *value = nullptr;
	uint32_t bits = 0;
	uint32_t mask = 0;

	bool Overrides() const { return value || mask; }
	bool SameAs(const EffectiveOverride &other) const;
	size_t Hash() const;
};

// The all-clients tier applies first, then the slot's own, each in the order its layers last changed.
EffectiveOverride ResolveEffective(const PropOverride &po, int slot);

struct EntityOverrides
{
	CBaseHandle handle;
	std::unordered_map<PropKey, PropOverride, PropKeyHash> by_prop;

	std::bitset<SM_MAXPLAYERS> per_client_slots;
	bool has_all_clients_layers = false;
	bool has_hooks = false;
	bool has_all_clients_hooks = false; // All-clients hooks can give clients without their own layers different values.
};

class OverrideStore
{
public:
	using EntityMap = std::unordered_map<int, EntityOverrides>;

	static bool Resolve(const CBaseHandle &entity, const char *prop_name, int element, std::optional<PropType> required, PropInfo *out, char *err, size_t errlen);

	// A nullopt value sets a passthrough.
	void Set(const CBaseHandle &entity, const PropInfo &info, int slot, std::optional<PropValue> value, IPluginContext *owner);
	// Merges into the owner's value layer, and replaces a hook or passthrough. The mask needs a field bit.
	void SetBits(const CBaseHandle &entity, const PropInfo &info, int slot, uint32_t bits, uint32_t mask, IPluginContext *owner);
	void SetHook(const CBaseHandle &entity, const PropInfo &info, int slot, std::shared_ptr<PropHook> hook, IPluginContext *owner);
	bool Unset(const CBaseHandle &entity, const PropInfo &info, int slot, IPluginContext *owner);
	// Narrows the owner's value or hook layer, or removes it when no bits are left. Passthroughs are kept.
	bool UnsetBits(const CBaseHandle &entity, const PropInfo &info, int slot, uint32_t mask, IPluginContext *owner);

	bool Query(const CBaseHandle &entity, const PropInfo &info, int slot, EffectiveOverride &out) const;

	int ClearEntity(const CBaseHandle &entity, IPluginContext *owner);
	int ClearSlot(int slot, IPluginContext *owner);
	int ClearOwner(IPluginContext *owner);

	void PurgeStale();

	void OnClientDisconnected(int client);
	void Clear();
	bool Empty() const { return m_entries.empty(); }

	const EntityMap &Entries() const { return m_entries; }

private:
	template <class Fn>
	int PruneEntities(Fn &&prune_prop);
	void Compact(EntityMap::iterator ent_it);
	static void UpdateHookFlags(EntityOverrides &ent);
	PropLayer *FindLayer(const CBaseHandle &entity, const PropInfo &info, int slot, IPluginContext *owner);
	EntityOverrides &SetLayer(const CBaseHandle &entity, const PropInfo &info, int slot, PropLayer layer, IPluginContext *owner);

	EntityMap m_entries;
};

extern OverrideStore g_OverrideStore;

} // namespace safeproxy

#endif // _INCLUDE_SAFEPROXY_OVERRIDES_H_
