#ifndef _INCLUDE_SAFEPROXY_GAMEDATA_H_
#define _INCLUDE_SAFEPROXY_GAMEDATA_H_

#include "smsdk_ext.h"

#include <iclient.h>
#include <igameevents.h>

class SendTable;
class CSendProxyRecipients;
template <class T, class I> class CUtlMemory;
class bf_write;

namespace safeproxy {

typedef bool (*SendTable_Encode_t)(const SendTable *pTable, const void *pStruct, bf_write *pOut, int objectID, CUtlMemory<CSendProxyRecipients, int> *pRecipients, bool bNonZeroOnly);

typedef int (*SendTable_CalcDelta_t)(const SendTable *pTable, const void *pFromState, int nFromBits, const void *pToState, int nToBits, int *pDeltaProps, int nMaxDeltaProps, int objectID);

struct GamedataOffsets
{
	SendTable_Encode_t SendTable_Encode = nullptr;
	SendTable_CalcDelta_t SendTable_CalcDelta = nullptr;

	void *SV_ComputeClientPacks = nullptr;
	void *CFrameSnapshotManager_GetPackedEntity = nullptr;
	void *CBaseServer_WriteDeltaEntities = nullptr;

	int CBaseEntity_ShouldTransmit = -1;
	int CBaseEntity_SetTransmit = -1;

	int PackedEntity_Size = -1;
	int PackedEntity_pData = -1;
	int PackedEntity_nBits = -1;
	int PackedEntity_pChangeFrameList = -1;

	int CBaseClient_m_pBaseline = -1;
	int CBaseClient_m_nBaselineUpdateTick = -1;
	int CBaseClient_m_BaselinesSent = -1;

	int CGameClient_m_pCurrentFrame = -1;
	int CClientFrame_transmit_entity = -1;
};

extern const GamedataOffsets &g_Offsets;

// CGameClient starts with CBaseClient, whose bases are IGameEventListener2, IClient and IClientMessageHandler, in that order.
constexpr size_t kBaseClientIClientOffset = sizeof(IGameEventListener2);
constexpr size_t kBaseClientMessageHandlerOffset = kBaseClientIClientOffset + sizeof(IClient);

bool LoadGameData(char *error, size_t maxlen);

} // namespace safeproxy

#endif // _INCLUDE_SAFEPROXY_GAMEDATA_H_
