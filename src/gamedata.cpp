#include "gamedata.h"

#include <cstdio>

namespace safeproxy {

namespace {

GamedataOffsets s_offsets;

bool ResolveGameData(IGameConfig *gameconf, char *error, size_t maxlen)
{
	struct SigSpec { const char *key; void **out; };
	SigSpec sigs[] = {
		{ "SendTable_Encode",                        reinterpret_cast<void **>(&s_offsets.SendTable_Encode)    },
		{ "SendTable_CalcDelta",                     reinterpret_cast<void **>(&s_offsets.SendTable_CalcDelta) },
		{ "SV_ComputeClientPacks",                   &s_offsets.SV_ComputeClientPacks                          },
		{ "CFrameSnapshotManager::GetPackedEntity",  &s_offsets.CFrameSnapshotManager_GetPackedEntity          },
		{ "CBaseServer::WriteDeltaEntities",         &s_offsets.CBaseServer_WriteDeltaEntities                 },
	};

	for (const auto &s : sigs)
	{
		if (!gameconf->GetMemSig(s.key, s.out))
		{
			snprintf(error, maxlen, "Missing gamedata signature '%s'", s.key);
			return false;
		}
		if (!*s.out)
		{
			snprintf(error, maxlen, "Gamedata signature '%s' did not match", s.key);
			return false;
		}
	}

	struct OffsetSpec { const char *key; int *out; };
	OffsetSpec offsets[] = {
		{ "CBaseEntity::ShouldTransmit",        &s_offsets.CBaseEntity_ShouldTransmit        },
		{ "CBaseEntity::SetTransmit",           &s_offsets.CBaseEntity_SetTransmit           },
		{ "PackedEntity::Size",                 &s_offsets.PackedEntity_Size                 },
		{ "PackedEntity::m_pData",              &s_offsets.PackedEntity_pData                },
		{ "PackedEntity::m_nBits",              &s_offsets.PackedEntity_nBits                },
		{ "PackedEntity::m_pChangeFrameList",   &s_offsets.PackedEntity_pChangeFrameList     },
		{ "CBaseClient::m_pBaseline",           &s_offsets.CBaseClient_m_pBaseline           },
		{ "CBaseClient::m_nBaselineUpdateTick", &s_offsets.CBaseClient_m_nBaselineUpdateTick },
		{ "CBaseClient::m_BaselinesSent",       &s_offsets.CBaseClient_m_BaselinesSent       },
		{ "CGameClient::m_pCurrentFrame",       &s_offsets.CGameClient_m_pCurrentFrame       },
		{ "CClientFrame::transmit_entity",      &s_offsets.CClientFrame_transmit_entity      },
	};

	for (const auto &o : offsets)
	{
		if (!gameconf->GetOffset(o.key, o.out))
		{
			snprintf(error, maxlen, "Missing gamedata offset '%s'", o.key);
			return false;
		}
	}
	return true;
}

} // anonymous namespace

const GamedataOffsets &g_Offsets = s_offsets;

bool LoadGameData(char *error, size_t maxlen)
{
	IGameConfig *gameconf = nullptr;
	char conf_error[255] = "";
	bool ok = gameconfs->LoadGameConfigFile("safeproxy", &gameconf, conf_error, sizeof(conf_error));
	if (ok)
		ok = ResolveGameData(gameconf, error, maxlen);
	else
		snprintf(error, maxlen, "Could not read safeproxy.txt gamedata: %s", conf_error);

	if (gameconf)
		gameconfs->CloseGameConfigFile(gameconf);
	return ok;
}

} // namespace safeproxy
