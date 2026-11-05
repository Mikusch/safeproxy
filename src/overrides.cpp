#include "overrides.h"
#include "reencode.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include <const.h>
#include <dt_common.h>
#include <dt_send.h>
#include <edict.h>
#include <iserverunknown.h>
#include <server_class.h>
#include <tier1/strtools.h>

const CStandardSendProxies *g_pStdProxies;

namespace safeproxy {

OverrideStore g_OverrideStore;

const char *OverrideValue::StringPtr() const
{
	return std::get<std::string>(v).c_str();
}

void OverrideValue::WriteTo(void *dst) const
{
	std::visit([&](const auto &val) {
		// Little-endian only. Narrow int fields take the low bytes.
		if constexpr (!std::is_same_v<std::decay_t<decltype(val)>, std::string>)
			memcpy(dst, &val, field_size);
	}, v);
}

uint32_t OverrideValue::Bits() const
{
	if (const int *i = std::get_if<int>(&v))
		return static_cast<uint32_t>(*i);
	uint32_t packed = 0;
	if (const auto *rgba = std::get_if<Rgba>(&v))
		memcpy(&packed, rgba->data(), sizeof(packed));
	return packed;
}

bool OverrideValue::SameAs(const OverrideValue &other) const
{
	if (type != other.type || mask != other.mask)
		return false;
	// An int and a color with the same bytes send the same field.
	if (IsBitwise(type))
		return Bits() == other.Bits();
	if (v.index() != other.v.index())
		return false;
	return std::visit([&](const auto &val) -> bool {
		using T = std::decay_t<decltype(val)>;
		const T &rhs = std::get<T>(other.v);
		if constexpr (std::is_same_v<T, std::string>)
			return val == rhs;
		else
			return memcmp(&val, &rhs, sizeof(T)) == 0;
	}, v);
}

size_t OverrideValue::Hash() const
{
	if (IsBitwise(type))
		return std::hash<uint32_t>()(Bits());
	return std::visit([](const auto &val) -> size_t {
		using T = std::decay_t<decltype(val)>;
		if constexpr (std::is_same_v<T, std::string>)
			return std::hash<std::string>()(val);
		else
			return std::hash<std::string_view>()(std::string_view(reinterpret_cast<const char *>(&val), sizeof(T)));
	}, v);
}

namespace {

// Same check as the engine's IsNonPointerModifyingProxy.
std::unordered_set<SendTableProxyFn> s_staticOffsetProxies;

void BuildStaticOffsetProxySet()
{
	s_staticOffsetProxies.clear();
	s_staticOffsetProxies.insert(g_pStdProxies->m_DataTableToDataTable);
#if SOURCE_ENGINE == SE_LEFT4DEAD2
	// hl2sdk-l4d2 declares m_SendLocalDataTable, which retail L4D2 doesn't have. The list pointer is in its place.
	CNonModifiedPointerProxy **head;
	memcpy(&head, &g_pStdProxies->m_SendLocalDataTable, sizeof(head));
#else
	s_staticOffsetProxies.insert(g_pStdProxies->m_SendLocalDataTable);
	CNonModifiedPointerProxy **head = g_pStdProxies->m_ppNonModifiedPointerProxies;
#endif
	if (head)
	{
		for (CNonModifiedPointerProxy *p = *head; p; p = p->m_pNext)
			s_staticOffsetProxies.insert(p->m_Fn);
	}
}

struct SendPropLookup
{
	SendProp *prop = nullptr;
	unsigned int offset = 0;
	std::array<Relocation, kMaxRelocations> relocations{};
	int relocation_count = 0;
	bool too_deep = false;
	bool in_vector = false;
};

struct ExcludedProp
{
	const char *table;
	const char *prop;
};

void CollectExcludes(SendTable *table, std::vector<ExcludedProp> &out)
{
	for (int i = 0; i < table->GetNumProps(); ++i)
	{
		SendProp *prop = table->GetProp(i);
		if (prop->IsExcludeProp())
			out.push_back({prop->GetExcludeDTName(), prop->GetName()});
		else if (SendTable *inner = prop->GetDataTable())
			CollectExcludes(inner, out);
	}
}

bool IsExcluded(const std::vector<ExcludedProp> &excludes, const SendTable *table, const char *prop_name)
{
	for (const ExcludedProp &e : excludes)
	{
		if (V_stricmp(e.table, table->GetName()) == 0 && V_stricmp(e.prop, prop_name) == 0)
			return true;
	}
	return false;
}

bool TableContainsName(SendTable *table, const char *name, bool want_table)
{
	for (int i = 0; i < table->GetNumProps(); ++i)
	{
		SendProp *prop = table->GetProp(i);
		const char *prop_name = prop->GetName();
		SendTable *inner = prop->GetDataTable();
		if (prop_name && strcmp(name, prop_name) == 0 && (!want_table || inner))
			return true;
		if (inner && TableContainsName(inner, name, want_table))
			return true;
	}
	return false;
}

void EnterTable(SendPropLookup &at, const SendProp *table)
{
	if (s_staticOffsetProxies.contains(table->GetDataTableProxyFn()))
	{
		at.offset += table->GetOffset();
		return;
	}
	// A CUtlVector element's proxy can't be called on its own, it assumes the engine has just grown the vector.
	// SendPropArray3 elements also name their array, but only CUtlVector ones carry extra data.
	if (table->GetParentArrayPropName() && table->GetExtraData())
		at.in_vector = true;
	if (at.relocation_count == kMaxRelocations)
	{
		at.too_deep = true;
		return;
	}
	at.relocations[at.relocation_count++] = {table, at.offset};
	at.offset = 0;
}

// Same first-match walk as SourceMod's FindSendPropInfo, but skips excluded props like the engine.
bool FindInSendTable(SendTable *table, const char *name, const std::vector<ExcludedProp> &excludes, const SendPropLookup &at, bool want_table, SendPropLookup *out, bool &saw_excluded)
{
	for (int i = 0; i < table->GetNumProps(); ++i)
	{
		SendProp *prop = table->GetProp(i);
		if (prop->IsExcludeProp() || prop->IsInsideArray())
			continue;

		const char *prop_name = prop->GetName();
		if (prop_name && IsExcluded(excludes, table, prop_name))
		{
			if (!saw_excluded)
			{
				SendTable *inner = prop->GetDataTable();
				saw_excluded = (strcmp(name, prop_name) == 0 && (!want_table || inner)) || (inner && TableContainsName(inner, name, want_table));
			}
			continue;
		}

		if (prop_name && strcmp(name, prop_name) == 0 && (!want_table || prop->GetDataTable()))
		{
			*out = at;
			out->prop = prop;
			out->offset += prop->GetOffset();
			return true;
		}

		if (SendTable *inner = prop->GetDataTable())
		{
			SendPropLookup inner_at = at;
			EnterTable(inner_at, prop);
			if (FindInSendTable(inner, name, excludes, inner_at, want_table, out, saw_excluded))
				return true;
		}
	}
	return false;
}

struct ClassLookups
{
	std::vector<ExcludedProp> excludes;
	std::unordered_map<std::string, SendPropLookup> by_name;
	std::unordered_map<const SendProp *, int> occurrences;
};
std::unordered_map<const SendTable *, ClassLookups> s_lookupCache;

int CountInTable(SendTable *table, const SendProp *target)
{
	int n = 0;
	for (int i = 0; i < table->GetNumProps(); ++i)
	{
		SendProp *prop = table->GetProp(i);
		if (prop == target)
			++n;
		else if (SendTable *inner = prop->GetDataTable())
			n += CountInTable(inner, target);
	}
	return n;
}

int CountOccurrences(ServerClass *sc, const SendProp *prop)
{
	auto [it, inserted] = s_lookupCache[sc->m_pTable].occurrences.try_emplace(prop);
	if (inserted)
		it->second = CountInTable(sc->m_pTable, prop);
	return it->second;
}

enum class LookupResult { Found, Missing, Excluded };

LookupResult LookupSendProp(ServerClass *sc, const char *name, SendPropLookup *out)
{
	auto [cache_it, inserted] = s_lookupCache.try_emplace(sc->m_pTable);
	ClassLookups &lookups = cache_it->second;
	if (inserted)
		CollectExcludes(sc->m_pTable, lookups.excludes);

	if (auto it = lookups.by_name.find(name); it != lookups.by_name.end())
	{
		*out = it->second;
		return LookupResult::Found;
	}

	// Match the whole name first for SENDINFO_STRUCTELEM props like "m_fog.colorPrimary".
	SendTable *table = sc->m_pTable;
	SendPropLookup found;
	std::string_view rest = name;
	for (;;)
	{
		bool saw_excluded = false;
		if (rest.empty())
			return LookupResult::Missing;
		if (FindInSendTable(table, std::string(rest).c_str(), lookups.excludes, found, false, &found, saw_excluded))
			break;

		const size_t dot = rest.find('.');
		if (dot == std::string_view::npos || dot == 0 || !FindInSendTable(table, std::string(rest.substr(0, dot)).c_str(), lookups.excludes, found, true, &found, saw_excluded))
			return saw_excluded ? LookupResult::Excluded : LookupResult::Missing;

		found.offset -= found.prop->GetOffset();
		EnterTable(found, found.prop);
		table = found.prop->GetDataTable();
		rest.remove_prefix(dot + 1);
	}

	*out = found;
	lookups.by_name.emplace(name, found);
	return LookupResult::Found;
}

struct PropClassification
{
	PropType type;
	int field_size;
};

struct TypedProxy
{
	SendPropType type;
	PropClassification classification;
};

// Transforming proxies aren't exported.
std::unordered_map<SendVarProxyFn, TypedProxy> s_probedProxies;

// A multimap because MSVC x64 folds SendProxy_FloatToFloat into SendProxy_Int32ToInt32.
std::unordered_multimap<SendVarProxyFn, TypedProxy> s_stdProxies;

void BuildStdProxyMap()
{
	s_stdProxies.clear();

	auto add = [](SendVarProxyFn fn, SendPropType wire, PropType type, int size) {
		if (fn)
			s_stdProxies.emplace(fn, TypedProxy{wire, {type, size}});
	};

	add(g_pStdProxies->m_Int8ToInt32,    DPT_Int,    PropType::Int,    sizeof(int8_t));
	add(g_pStdProxies->m_UInt8ToInt32,   DPT_Int,    PropType::Int,    sizeof(uint8_t));
	add(g_pStdProxies->m_Int16ToInt32,   DPT_Int,    PropType::Int,    sizeof(int16_t));
	add(g_pStdProxies->m_UInt16ToInt32,  DPT_Int,    PropType::Int,    sizeof(uint16_t));
	add(g_pStdProxies->m_Int32ToInt32,   DPT_Int,    PropType::Int,    sizeof(int32_t));
	add(g_pStdProxies->m_UInt32ToInt32,  DPT_Int,    PropType::Int,    sizeof(uint32_t));
	add(g_pStdProxies->m_FloatToFloat,   DPT_Float,  PropType::Float,  sizeof(float));
	add(g_pStdProxies->m_VectorToVector, DPT_Vector, PropType::Vector, sizeof(Vec3));
}

std::unordered_map<const SendProp *, PropClassification> s_probeProps;

// TF2 Tools swaps in proxies that read these to fire TF2_OnConditionAdded/Removed.
std::unordered_set<const SendProp *> s_watchedProps;

std::optional<PropClassification> ClassifyProp(SendProp *prop)
{
	if (auto it = s_probeProps.find(prop); it != s_probeProps.end())
		return it->second;

	// These props' own proxies don't matter, the re-encode patches their output.
	if (prop->GetType() == DPT_String)
		return PropClassification{PropType::String, 0};
	if (s_watchedProps.contains(prop))
		return PropClassification{PropType::Int, sizeof(int32_t)};

	SendVarProxyFn fn = prop->GetProxyFn();
	if (!fn)
		return std::nullopt;

	auto [first, last] = s_stdProxies.equal_range(fn);
	for (auto it = first; it != last; ++it)
	{
		if (it->second.type == prop->GetType())
			return it->second.classification;
	}
	if (auto it = s_probedProxies.find(fn); it != s_probedProxies.end() && it->second.type == prop->GetType())
		return it->second.classification;
	return std::nullopt;
}

struct ProxyProbe { const char *server_class; const char *prop_name; };

struct ProbeResult
{
	std::vector<SendProp *> props;
	SendVarProxyFn fn = nullptr;
};

const void *ModuleOf(SendVarProxyFn fn)
{
#if defined PLATFORM_WINDOWS
	HMODULE module = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(fn), &module);
	return module;
#else
	Dl_info info;
	return dladdr(reinterpret_cast<void *>(fn), &info) ? info.dli_fbase : nullptr;
#endif
}

const void *s_gameModule;

// Only proxies from the game's module vote. Another extension may hook many props with one shared proxy.
ProbeResult ProbeProxy(const std::vector<ProxyProbe> &probes, SendPropType type, const char *what)
{
	ProbeResult result;
	std::vector<SendVarProxyFn> fns;
	for (const ProxyProbe &probe : probes)
	{
		ServerClass *sc = gamehelpers->FindServerClass(probe.server_class);
		if (!sc)
			continue;
		SendPropLookup lookup;
		if (LookupSendProp(sc, probe.prop_name, &lookup) != LookupResult::Found || lookup.prop->GetType() != type)
		{
			g_pSM->LogError(myself, "Probe %s.%s (%s) is missing or changed", probe.server_class, probe.prop_name, what);
			continue;
		}
		result.props.push_back(lookup.prop);
		if (SendVarProxyFn fn = lookup.prop->GetProxyFn(); !s_stdProxies.contains(fn) && IsGameProxy(fn))
			fns.push_back(fn);
	}

	for (SendVarProxyFn fn : fns)
	{
		const size_t votes = std::count(fns.begin(), fns.end(), fn);
		if (votes >= 2 && votes * 2 > result.props.size())
		{
			result.fn = fn;
			break;
		}
	}
	return result;
}

} // anonymous namespace

void AutoDetectTransformingProxies()
{
	s_gameModule = ModuleOf(g_pStdProxies->m_Int32ToInt32);
	BuildStdProxyMap();
	BuildStaticOffsetProxySet();
	s_lookupCache.clear();
	s_probeProps.clear();

	struct ProbeSpec
	{
		const char *what;
		SendPropType type;
		PropClassification classification;
		std::vector<ProxyProbe> probes;
	};

	// The origin and angle proxies read the stomped field, except during MOVETYPE_STEP step simulation.
	static const ProbeSpec specs[] = {
		{ "EHandle", DPT_Int, {PropType::EHandle, sizeof(uint32_t)}, {
			{ "CBaseEntity", "m_hOwnerEntity" },
			{ "CBaseEntity", "m_hEffectEntity" },
			{ "CBaseEntity", "moveparent" },
		} },
		// Outside L4D2, CBaseEntity's m_clrRender doesn't use SendProxy_Color32ToInt.
		{ "Color32", DPT_Int, {PropType::Color32, sizeof(Rgba)}, {
			{ "CLightGlow", "m_clrRender" },
			{ "CSun", "m_clrRender" },
			{ "CSun", "m_clrOverlay" },
			{ "CFuncSmokeVolume", "m_Color1" },
			{ "CFuncSmokeVolume", "m_Color2" },
		} },
		{ "SendProxy_Angles", DPT_Vector, {PropType::Vector, sizeof(Vec3)}, {
			{ "CBaseEntity", "m_angRotation" },
			{ "CParticleSystem", "m_angRotation" },
			{ "CTFBaseRocket", "m_angRotation" },
			{ "CTFWeaponBaseGrenadeProj", "m_angRotation" },
			{ "CLightGlow", "m_angRotation" },
		} },
		{ "SendProxy_QAngles", DPT_Vector, {PropType::Vector, sizeof(Vec3)}, {
			{ "CTFPlayer", "m_angCustomModelRotation" },
			{ "CTEProjectedDecal", "m_angRotation" },
			{ "CTETFParticleEffect", "m_vecAngles" },
			{ "CTEEffectDispatch", "m_vAngles" },
			{ "CWeaponIFMBaseCamera", "m_angRenderAngles" },
		} },
		// L4D2 sends most origins relative to the entity's cell, which an origin override alone can't move.
		{ "SendProxy_Origin", DPT_Vector, {PropType::Vector, sizeof(Vec3)}, {
#if SOURCE_ENGINE != SE_LEFT4DEAD2
			{ "CBaseEntity", "m_vecOrigin" },
#endif
			{ "CParticleSystem", "m_vecOrigin" },
			{ "CColorCorrection", "m_vecOrigin" },
#if SOURCE_ENGINE != SE_LEFT4DEAD2
			{ "CCSPlayer", "m_vecOrigin" },
			{ "CCSPlayer", "csnonlocaldata.m_vecOrigin" },
#endif
			{ "CDODPlayer", "m_vecOrigin" },
			{ "CDODPlayer", "dodnonlocaldata.m_vecOrigin" },
		} },
		{ "SendProxy_OriginXY", DPT_VectorXY, {PropType::Vector, sizeof(Vec3)}, {
			{ "CTFPlayer", "m_vecOrigin" },
			{ "CTFPlayer", "tfnonlocaldata.m_vecOrigin" },
			{ "CHL2MP_Player", "m_vecOrigin" },
			{ "CHL2MP_Player", "hl2mpnonlocaldata.m_vecOrigin" },
#if SOURCE_ENGINE == SE_LEFT4DEAD2
			{ "CCSPlayer", "cslocaldata.m_vecOrigin" },
			{ "CTerrorPlayer", "cslocaldata.m_vecOrigin" },
#endif
		} },
		{ "SendProxy_OriginZ", DPT_Float, {PropType::Float, sizeof(float)}, {
			{ "CTFPlayer", "m_vecOrigin[2]" },
			{ "CTFPlayer", "tfnonlocaldata.m_vecOrigin[2]" },
			{ "CHL2MP_Player", "m_vecOrigin[2]" },
			{ "CHL2MP_Player", "hl2mpnonlocaldata.m_vecOrigin[2]" },
#if SOURCE_ENGINE == SE_LEFT4DEAD2
			{ "CCSPlayer", "cslocaldata.m_vecOrigin[2]" },
			{ "CTerrorPlayer", "cslocaldata.m_vecOrigin[2]" },
#endif
		} },
		{ "SendProxy_FuncRotatingOrigin", DPT_Vector, {PropType::Vector, sizeof(Vec3)}, {
			{ "CFuncRotating", "m_vecOrigin" },
		} },
		{ "SendProxy_AngleToFloat", DPT_Float, {PropType::Float, sizeof(float)}, {
			{ "CTFPlayer", "m_angEyeAngles[1]" },
			{ "CTFPlayer", "tfnonlocaldata.m_angEyeAngles[1]" },
			{ "CHL2MP_Player", "m_angEyeAngles[1]" },
			{ "CHL2MP_Player", "hl2mpnonlocaldata.m_angEyeAngles[1]" },
			{ "CCSPlayer", "m_angEyeAngles[0]" },
			{ "CCSPlayer", "m_angEyeAngles[1]" },
			{ "CDODPlayer", "m_angEyeAngles[0]" },
			{ "CDODPlayer", "m_angEyeAngles[1]" },
			{ "CTEFireBullets", "m_vecAngles[1]" },
			{ "CTEBreakModel", "m_angRotation[0]" },
			{ "CTEPhysicsProp", "m_angRotation[0]" },
		} },
		{ "SendProxy_FuncRotatingAngle", DPT_Float, {PropType::Float, sizeof(float)}, {
			{ "CTFAmmoPack", "m_angRotation[0]" },
			{ "CTFAmmoPack", "m_angRotation[1]" },
			{ "CTFAmmoPack", "m_angRotation[2]" },
			{ "CFuncRotating", "m_angRotation[0]" },
			{ "CFuncRotating", "m_angRotation[1]" },
			{ "CFuncRotating", "m_angRotation[2]" },
		} },
		{ "SendProxy_IntAddOne", DPT_Int, {PropType::Int, sizeof(int32_t)}, {
			{ "CBaseCombatWeapon", "m_iClip1" },
			{ "CBaseCombatWeapon", "m_iClip2" },
			{ "CSmokeStack", "m_iMaterialModel" },
			{ "CNPC_Manhack", "m_nEnginePitch1" },
			{ "CNPC_Manhack", "m_nEnginePitch2" },
		} },
		{ "SendProxy_Solid", DPT_Int, {PropType::Int, sizeof(uint8_t)}, {
			{ "CBaseEntity", "m_nSolidType" },
		} },
		{ "SendProxy_SolidFlags", DPT_Int, {PropType::Int, sizeof(uint16_t)}, {
			{ "CBaseEntity", "m_usSolidFlags" },
		} },
		// Sends the time as ticks past the network base, which the encode cache keys on.
		{ "SendProxy_SimulationTime", DPT_Int, {PropType::Float, sizeof(float)}, {
			{ "CBaseEntity", "m_flSimulationTime" },
		} },
		{ "SendProxy_FuncRotatingSimulationTime", DPT_Int, {PropType::Float, sizeof(float)}, {
			{ "CFuncRotating", "m_flSimulationTime" },
		} },
#if SOURCE_ENGINE == SE_LEFT4DEAD2
		// Other games now send m_fFlags with the standard int proxy.
		{ "SendProxy_CropFlagsToPlayerFlagBitsLength", DPT_Int, {PropType::Int, sizeof(int32_t)}, {
			{ "CBasePlayer", "m_fFlags" },
			{ "CBaseGrenade", "m_fFlags" },
		} },
		{ "SendProxy_InfectedAngle", DPT_Float, {PropType::Float, sizeof(float)}, {
			{ "Infected", "m_angRotation[0]" },
			{ "Infected", "m_angRotation[1]" },
			{ "Infected", "m_angRotation[2]" },
		} },
#endif
	};

	s_probedProxies.clear();
	for (const ProbeSpec &spec : specs)
	{
		ProbeResult probed = ProbeProxy(spec.probes, spec.type, spec.what);
		for (SendProp *prop : probed.props)
			s_probeProps.emplace(prop, spec.classification);
		if (probed.fn)
			s_probedProxies.try_emplace(probed.fn, TypedProxy{spec.type, spec.classification});
	}

	// Output patching only replaces the game's plain copy.
	s_watchedProps.clear();
	if (ServerClass *sc = gamehelpers->FindServerClass("CTFPlayer"))
	{
		for (const char *name : {"m_nPlayerCond", "_condition_bits", "m_nPlayerCondEx", "m_nPlayerCondEx2", "m_nPlayerCondEx3", "m_nPlayerCondEx4"})
		{
			SendPropLookup lookup;
			bool ok = LookupSendProp(sc, name, &lookup) == LookupResult::Found && lookup.prop->GetType() == DPT_Int;
			if (ok)
			{
				SendVarProxyFn fn = lookup.prop->GetProxyFn();
				ok = fn && (!IsGameProxy(fn) || fn == g_pStdProxies->m_Int32ToInt32 || fn == g_pStdProxies->m_UInt32ToInt32);
			}
			if (ok)
				s_watchedProps.insert(lookup.prop);
			else
				g_pSM->LogError(myself, "Condition prop CTFPlayer.%s is missing or changed", name);
		}
	}
}

bool IsGameProxy(SendVarProxyFn fn)
{
	return fn && s_gameModule && ModuleOf(fn) == s_gameModule;
}

namespace {

bool AreTypesCompatible(const PropClassification &classified, const SendProp *prop, PropType required)
{
	if (classified.type == required)
		return true;
	return IsBitwise(classified.type) && IsBitwise(required) && classified.field_size == 4 && prop->m_nBits == 32;
}

const char *DescribeType(PropType type)
{
	switch (type)
	{
	case PropType::Int:     return "an int";
	case PropType::Float:   return "a float";
	case PropType::Vector:  return "a vector";
	case PropType::String:  return "a string";
	case PropType::EHandle: return "an entity handle";
	case PropType::Color32: return "a color";
	}
	return "unsupported";
}

// Only DPT_Array elements have SPROP_INSIDEARRAY. SendPropArray3 elements are found by their parent array name.
bool IsArrayProp(SendProp *prop)
{
	if (prop->GetType() == DPT_Array)
		return true;
	if (prop->GetType() == DPT_DataTable)
	{
		SendTable *dt = prop->GetDataTable();
		if (dt && dt->GetNumProps() > 0 && dt->GetProp(0)->GetParentArrayPropName())
			return true;
	}
	return false;
}

bool ResolveProp(int entity_index, const char *prop_name, int element, std::optional<PropType> required, PropInfo *out, char *err, size_t errlen)
{
	ServerClass *sc = ResolveServerClass(entity_index);
	if (!sc)
	{
		snprintf(err, errlen, "Entity %d is invalid or not networked", entity_index);
		return false;
	}

	SendPropLookup raw;
	switch (LookupSendProp(sc, prop_name, &raw))
	{
	case LookupResult::Found:
		break;
	case LookupResult::Excluded:
		snprintf(err, errlen, "SendProp '%s' is excluded from class '%s'", prop_name, sc->GetName());
		return false;
	case LookupResult::Missing:
		snprintf(err, errlen, "Could not find SendProp '%s' on class '%s'", prop_name, sc->GetName());
		return false;
	}

	SendProp *prop = raw.prop;
	SendPropLookup at = raw;
	if (IsArrayProp(raw.prop))
	{
		bool legacy = raw.prop->GetType() == DPT_Array;
		SendTable *dt = legacy ? nullptr : raw.prop->GetDataTable();
		int num_elements = legacy ? raw.prop->GetNumElements() : dt->GetNumProps();
		if (element < 0 || element >= num_elements)
		{
			snprintf(err, errlen, "Array element %d out of range for '%s' on class '%s' (array has %d elements)", element, prop_name, sc->GetName(), num_elements);
			return false;
		}

		if (legacy)
		{
			prop = const_cast<SendProp *>(raw.prop->GetArrayProp());
			at.offset += prop->GetOffset() + element * raw.prop->GetElementStride();
		}
		else
		{
			at.offset -= raw.prop->GetOffset();
			EnterTable(at, raw.prop);
			prop = dt->GetProp(element);
			at.offset += prop->GetOffset();
		}
	}
	else if (element != 0)
	{
		snprintf(err, errlen, "Prop '%s' on class '%s' is not an array (element %d requested)", prop_name, sc->GetName(), element);
		return false;
	}

	auto classified = ClassifyProp(prop);
	if (!classified)
	{
		if (prop->GetType() == DPT_DataTable)
			snprintf(err, errlen, "Prop '%s' on class '%s' is a DataTable", prop_name, sc->GetName());
		else
			snprintf(err, errlen, "Prop '%s' on class '%s' has an unsupported send proxy", prop_name, sc->GetName());
		return false;
	}
	if (at.in_vector)
	{
		snprintf(err, errlen, "Prop '%s' on class '%s' is in a CUtlVector element", prop_name, sc->GetName());
		return false;
	}
	if (at.too_deep)
	{
		snprintf(err, errlen, "Prop '%s' on class '%s' is moved off the entity by too many DataTables", prop_name, sc->GetName());
		return false;
	}
	// KeyOf needs the last relocating table, or a string's SendProp, to appear only once.
	if (at.relocation_count > 0 && CountOccurrences(sc, classified->type == PropType::String ? prop : at.relocations[at.relocation_count - 1].table) != 1)
	{
		snprintf(err, errlen, "Prop '%s' on class '%s' is stored off the entity and appears more than once", prop_name, sc->GetName());
		return false;
	}
	if (required && !AreTypesCompatible(*classified, prop, *required))
	{
		snprintf(err, errlen, "Prop '%s' on class '%s' is %s", prop_name, sc->GetName(), DescribeType(classified->type));
		return false;
	}

	out->prop = prop;
	out->actual_offset = at.offset;
	out->type = classified->type;
	out->field_size = classified->field_size;
	out->array_element = raw.prop->GetType() == DPT_Array ? element : -1;
	out->relocations = at.relocations;
	out->relocation_count = at.relocation_count;
	out->watched = s_watchedProps.contains(prop);
	return true;
}

} // anonymous namespace

IServerUnknown *LiveUnknown(int entity_index)
{
	edict_t *edict = gamehelpers->EdictOfIndex(entity_index);
	if (!edict || edict->IsFree())
		return nullptr;
	return edict->GetUnknown();
}

ServerClass *ResolveServerClass(int entity_index)
{
	IServerUnknown *unk = LiveUnknown(entity_index);
	IServerNetworkable *net = unk ? unk->GetNetworkable() : nullptr;
	return net ? net->GetServerClass() : nullptr;
}

CBaseHandle LiveHandle(int entity_index)
{
	IServerUnknown *unk = LiveUnknown(entity_index);
	return unk ? unk->GetRefEHandle() : CBaseHandle();
}

namespace {

std::vector<IPluginContext *> s_suspended;
std::vector<std::pair<IPluginContext *, bool>> s_deferredSuspensions;

void ApplySuspension(IPluginContext *owner, bool suspended)
{
	std::erase(s_suspended, owner);
	if (suspended)
		s_suspended.push_back(owner);
}

} // anonymous namespace

bool IsSuspended(IPluginContext *owner)
{
	return std::find(s_suspended.begin(), s_suspended.end(), owner) != s_suspended.end();
}

void SetSuspended(IPluginContext *owner, bool suspended)
{
	if (IsEncoding())
		s_deferredSuspensions.emplace_back(owner, suspended);
	else
		ApplySuspension(owner, suspended);
}

void ApplyDeferredSuspensions()
{
	for (const auto &[owner, suspended] : s_deferredSuspensions)
		ApplySuspension(owner, suspended);
	s_deferredSuspensions.clear();
}

void ClearSuspensions()
{
	s_suspended.clear();
	s_deferredSuspensions.clear();
}

PropKey KeyOf(const PropInfo &info)
{
	if (info.type == PropType::String)
		return {info.prop, info.actual_offset, 0};
	const SendProp *table = info.Relocated() ? info.relocations[info.relocation_count - 1].table : nullptr;
	return {table, info.actual_offset, info.field_size};
}

uint8_t *FieldAddress(void *ent_base, const PropInfo &info, int entity_index)
{
	auto *base = static_cast<uint8_t *>(ent_base);
	for (int i = 0; i < info.relocation_count; ++i)
	{
		const Relocation &r = info.relocations[i];
		uint8_t *object = base + r.offset;
		CSendProxyRecipients recipients;
		recipients.SetAllRecipients();
		base = static_cast<uint8_t *>(r.table->GetDataTableProxyFn()(r.table, object, object + r.table->GetOffset(), &recipients, entity_index));
		if (!base)
			return nullptr;
	}
	return base + info.actual_offset;
}

const OverrideValue *PropHook::Result(int slot) const
{
	auto it = by_slot.find(slot);
	return it != by_slot.end() ? &it->second : nullptr;
}

bool PropHook::SameCall(const PropHook &other) const
{
	return callback == other.callback && value_type == other.value_type && element == other.element && mask == other.mask && prop_name == other.prop_name;
}

bool PropLayer::operator==(const PropLayer &other) const
{
	if (value != other.value || !hook != !other.hook)
		return false;
	return !hook || hook->SameCall(*other.hook);
}

bool EffectiveOverride::SameAs(const EffectiveOverride &other) const
{
	if (bits != other.bits || mask != other.mask || !value != !other.value)
		return false;
	return value == other.value || value->SameAs(*other.value);
}

size_t EffectiveOverride::Hash() const
{
	return value ? value->Hash() : std::hash<uint32_t>()(bits) * 31 ^ std::hash<uint32_t>()(mask);
}

EffectiveOverride ResolveEffective(const PropOverride &po, int slot)
{
	const bool bitwise = IsBitwise(po.info.type);
	EffectiveOverride out;
	auto apply = [&](const Layers<PropLayer> &layers) {
		for (const auto &[owner, layer] : layers.entries)
		{
			if (IsSuspended(owner))
				continue;
			if (layer.IsPassthrough())
			{
				out = {};
				continue;
			}

			const OverrideValue *value = layer.hook ? layer.hook->Result(slot) : &*layer.value;
			if (!value)
				continue;
			if (!bitwise)
			{
				out.value = value;
			}
			else
			{
				out.bits = (out.bits & ~value->mask) | (value->Bits() & value->mask);
				out.mask |= value->mask;
			}
		}
	};

	apply(po.tiers.all_clients);
	if (slot != kSlotAllClients)
	{
		if (const Layers<PropLayer> *own = po.tiers.Tier(slot))
			apply(*own);
	}

	// Drop bits past the field so equal overrides compare and hash equal.
	out.mask &= FieldMask(po.info);
	out.bits &= out.mask;
	return out;
}

void NormalizeValue(const PropInfo &info, PropValue &value)
{
	if (int *i = std::get_if<int>(&value); i && info.prop->m_nBits == 1)
		*i = *i != 0;
}

uint32_t FieldMask(const PropInfo &info)
{
	return info.prop->m_nBits == 1 ? 1u : info.field_size >= 4 ? ~0u : (1u << (8 * info.field_size)) - 1;
}

void NarrowBits(OverrideValue &value, uint32_t mask)
{
	value.v = PropValue(std::in_place_type<int>, static_cast<int>(value.Bits() & mask));
	value.mask = mask;
}

int FieldBitsToInt(const PropInfo &info, uint32_t bits)
{
	const int width = info.field_size * 8;
	if (width < 32)
	{
		const uint32_t field_mask = (1u << width) - 1;
		bits &= field_mask;
		if (info.prop->IsSigned() && (bits & (1u << (width - 1))))
			bits |= ~field_mask;
	}
	return static_cast<int>(bits);
}

bool OverrideStore::Resolve(const CBaseHandle &entity, const char *prop_name, int element, std::optional<PropType> required, PropInfo *out, char *err, size_t errlen)
{
	return ResolveProp(entity.GetEntryIndex(), prop_name, element, required, out, err, errlen);
}

EntityOverrides &OverrideStore::SetLayer(const CBaseHandle &entity, const PropInfo &info, int slot, PropLayer layer, IPluginContext *owner)
{
	auto &entry = ClaimEntry(m_entries, entity);
	auto &po = entry.by_prop[KeyOf(info)];
	po.info = info;
	const Layers<PropLayer> *tier = po.tiers.Tier(slot);
	const PropLayer *own = tier ? tier->Find(owner) : nullptr;
	const bool replaces_hook = own && own->hook;
	po.tiers.Set(slot, owner, std::move(layer));

	if (slot == kSlotAllClients)
		entry.has_all_clients_layers = true;
	else
		entry.per_client_slots.set(slot);
	if (replaces_hook)
		UpdateHookFlags(entry);
	return entry;
}

void OverrideStore::Set(const CBaseHandle &entity, const PropInfo &info, int slot, std::optional<PropValue> value, IPluginContext *owner)
{
	PropLayer layer;
	if (value)
	{
		NormalizeValue(info, *value);
		layer.value = OverrideValue{info.type, std::move(*value), info.field_size};
	}
	SetLayer(entity, info, slot, std::move(layer), owner);
}

PropLayer *OverrideStore::FindLayer(const CBaseHandle &entity, const PropInfo &info, int slot, IPluginContext *owner)
{
	auto ent_it = FindLive(m_entries, entity);
	if (ent_it == m_entries.end())
		return nullptr;
	auto prop_it = ent_it->second.by_prop.find(KeyOf(info));
	if (prop_it == ent_it->second.by_prop.end())
		return nullptr;
	Layers<PropLayer> *layers = prop_it->second.tiers.Tier(slot);
	return layers ? layers->Find(owner) : nullptr;
}

void OverrideStore::SetBits(const CBaseHandle &entity, const PropInfo &info, int slot, uint32_t bits, uint32_t mask, IPluginContext *owner)
{
	mask &= FieldMask(info);
	bits &= mask;
	if (const PropLayer *own = FindLayer(entity, info, slot, owner); own && own->value && IsBitwise(own->value->type))
	{
		bits |= own->value->Bits() & own->value->mask & ~mask;
		mask |= own->value->mask;
	}
	PropLayer layer;
	layer.value = OverrideValue{info.type, PropValue(std::in_place_type<int>, static_cast<int>(bits)), info.field_size, mask};
	SetLayer(entity, info, slot, std::move(layer), owner);
}

void OverrideStore::SetHook(const CBaseHandle &entity, const PropInfo &info, int slot, std::shared_ptr<PropHook> hook, IPluginContext *owner)
{
	UpdateHookFlags(SetLayer(entity, info, slot, PropLayer{std::nullopt, std::move(hook)}, owner));
}

bool OverrideStore::Unset(const CBaseHandle &entity, const PropInfo &info, int slot, IPluginContext *owner)
{
	auto ent_it = FindLive(m_entries, entity);
	if (ent_it == m_entries.end())
		return false;
	auto prop_it = ent_it->second.by_prop.find(KeyOf(info));
	if (prop_it == ent_it->second.by_prop.end())
		return false;

	if (!prop_it->second.tiers.Remove(slot, owner))
		return false;
	Compact(ent_it);
	return true;
}

bool OverrideStore::UnsetBits(const CBaseHandle &entity, const PropInfo &info, int slot, uint32_t mask, IPluginContext *owner)
{
	PropLayer *own = FindLayer(entity, info, slot, owner);
	if (!own || own->IsPassthrough() || !IsBitwise(own->hook ? own->hook->value_type : own->value->type))
		return false;

	const uint32_t own_mask = (own->hook ? own->hook->mask : own->value->mask) & FieldMask(info);
	if (!(own_mask & mask))
		return false;

	const uint32_t kept = own_mask & ~mask;
	if (!kept)
		return Unset(entity, info, slot, owner);
	if (own->hook)
	{
		own->hook->mask = kept;
		for (auto &[client_slot, result] : own->hook->by_slot)
			NarrowBits(result, kept);
	}
	else
	{
		NarrowBits(*own->value, kept);
	}
	return true;
}

bool OverrideStore::Query(const CBaseHandle &entity, const PropInfo &info, int slot, EffectiveOverride &out) const
{
	auto ent_it = FindLive(m_entries, entity);
	if (ent_it == m_entries.end())
		return false;
	auto prop_it = ent_it->second.by_prop.find(KeyOf(info));
	if (prop_it == ent_it->second.by_prop.end())
		return false;

	out = ResolveEffective(prop_it->second, slot);
	return out.Overrides();
}

void OverrideStore::Compact(EntityMap::iterator ent_it)
{
	auto &ent = ent_it->second;
	std::erase_if(ent.by_prop, [](const auto &kv) { return kv.second.tiers.Empty(); });
	if (ent.by_prop.empty())
	{
		m_entries.erase(ent_it);
		return;
	}

	ent.per_client_slots.reset();
	ent.has_all_clients_layers = false;
	for (const auto &[key, po] : ent.by_prop)
	{
		ent.has_all_clients_layers |= !po.tiers.all_clients.Empty();
		for (const auto &[slot, layers] : po.tiers.per_client)
			ent.per_client_slots.set(slot);
	}
	UpdateHookFlags(ent);
}

void OverrideStore::UpdateHookFlags(EntityOverrides &ent)
{
	ent.has_hooks = false;
	ent.has_all_clients_hooks = false;
	auto has_hook = [](const Layers<PropLayer> &layers) {
		return std::any_of(layers.entries.begin(), layers.entries.end(), [](const auto &entry) { return entry.second.hook != nullptr; });
	};
	for (const auto &[key, po] : ent.by_prop)
	{
		ent.has_all_clients_hooks |= has_hook(po.tiers.all_clients);
		for (const auto &[slot, layers] : po.tiers.per_client)
			ent.has_hooks |= has_hook(layers);
	}
	ent.has_hooks |= ent.has_all_clients_hooks;
}

template <class Fn>
int OverrideStore::PruneEntities(Fn &&prune_prop)
{
	// Don't count layers on removed entities.
	EraseStale(m_entries);
	int n = 0;
	for (auto it = m_entries.begin(); it != m_entries.end(); )
	{
		auto next = std::next(it);
		for (auto &[key, po] : it->second.by_prop)
			n += prune_prop(po);
		Compact(it);
		it = next;
	}
	return n;
}

int OverrideStore::ClearEntity(const CBaseHandle &entity, IPluginContext *owner)
{
	auto ent_it = FindLive(m_entries, entity);
	if (ent_it == m_entries.end())
		return 0;

	int n = 0;
	for (auto &[key, po] : ent_it->second.by_prop)
		n += po.tiers.RemoveOwner(owner);
	Compact(ent_it);
	return n;
}

int OverrideStore::ClearSlot(int slot, IPluginContext *owner)
{
	return PruneEntities([&](PropOverride &po) { return po.tiers.Remove(slot, owner) ? 1 : 0; });
}

int OverrideStore::ClearOwner(IPluginContext *owner)
{
	return PruneEntities([&](PropOverride &po) { return po.tiers.RemoveOwner(owner); });
}

void OverrideStore::PurgeStale()
{
	EraseStale(m_entries);
}

void OverrideStore::OnClientDisconnected(int client)
{
	PruneEntities([&](PropOverride &po) {
		po.tiers.RemoveSlot(client - 1);
		for (auto &[owner, layer] : po.tiers.all_clients.entries)
		{
			if (layer.hook)
				layer.hook->by_slot.erase(client - 1);
		}
		return 0;
	});
}

void OverrideStore::Clear()
{
	m_entries.clear();
}

} // namespace safeproxy
