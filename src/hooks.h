#ifndef _INCLUDE_SAFEPROXY_HOOKS_H_
#define _INCLUDE_SAFEPROXY_HOOKS_H_

#include "smsdk_ext.h"

class IServer;
class IServerGameClients;
class CFrameSnapshot;
class PackedEntity;

extern IServer *g_pServer;
extern IServerGameClients *g_pServerGameClients;

namespace safeproxy {

bool InstallHooks(char *error, size_t maxlen);

void UnloadHooks();

// CFrameSnapshotManager::GetPackedEntity without the redirect.
PackedEntity *EngineGetPackedEntity(CFrameSnapshot *snapshot, int entity);

} // namespace safeproxy

#endif // _INCLUDE_SAFEPROXY_HOOKS_H_
