#ifndef _INCLUDE_SAFEPROXY_REENCODE_H_
#define _INCLUDE_SAFEPROXY_REENCODE_H_

#include <cstddef>
#include <cstdint>

class CGameClient;
class CFrameSnapshot;
class PackedEntity;

namespace safeproxy {

bool InitReencode(char *error, size_t maxlen);
void ClearReencodeState();

void ClearReencodeSlot(int slot);

// Clients connected before the load can hold any baseline, including an earlier load's payloads.
void AssumeUnknownBaselines();

// True while the re-encode walks the override store. Its send proxies can reach plugin code.
bool IsEncoding();

// After this tick's snapshot is packed and before it's sent.
bool IsEvaluatingHooks();

// While the re-encode runs, entity memory holds one client group's overrides.
void ReadRealBytes(const void *addr, int size, void *out);

// Drops cached encodes of changed entities. Must run before packing clears the flags.
void NoteStateChanges();

void PreComputePacks(int clientCount);
void PostComputePacks(int clientCount, CGameClient **clients, CFrameSnapshot *snapshot);
PackedEntity *OnGetPackedEntity(CFrameSnapshot *pSnapshot, int entity, PackedEntity *original);

// Slot -1 leaves every pack untouched.
void PreWriteDelta(int slot, const void *baseline);

void OnBaselineUpdateSent(int slot, int tick, const uint32_t *baselines_sent);

void OnBaselineAccepted(int slot, int tick, const uint32_t *baselines_sent);

void OnBaselineAckFailed(int slot, const uint32_t *baselines_sent);

} // namespace safeproxy

#endif // _INCLUDE_SAFEPROXY_REENCODE_H_
