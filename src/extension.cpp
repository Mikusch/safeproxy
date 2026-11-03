#include "extension.h"
#include "gamedata.h"
#include "hooks.h"
#include "overrides.h"
#include "natives.h"
#include "reencode.h"
#include "transmit.h"

#include <eiface.h>
#include <icvar.h>
#include <server_class.h>

CSafeProxyExt g_SafeProxyExt;
SMEXT_LINK(&g_SafeProxyExt);

bool CSafeProxyExt::SDK_OnLoad(char *error, size_t maxlen, bool late)
{
	safeproxy::g_OverrideStore.Clear();
	safeproxy::g_TransmitStore.Clear();
	safeproxy::ClearReencodeState();
	safeproxy::ClearSuspensions();

	g_pStdProxies = gamedll->GetStandardSendProxies();

	bool ok = safeproxy::InitReencode(error, maxlen)
		&& safeproxy::LoadGameData(error, maxlen)
		&& safeproxy::InstallHooks(error, maxlen)
		&& safeproxy::InstallTransmitHook(error, maxlen);

	// SDK_OnUnload isn't called when SDK_OnLoad fails.
	if (!ok)
	{
		safeproxy::UnloadHooks();
		return false;
	}

	sharesys->AddNatives(myself, safeproxy::g_Natives);
	sharesys->RegisterLibrary(myself, "safeproxy");

	playerhelpers->AddClientListener(this);
	plsys->AddPluginsListener(this);
	safeproxy::AssumeUnknownBaselines();

	return true;
}

void CSafeProxyExt::SDK_OnUnload()
{
	plsys->RemovePluginsListener(this);
	playerhelpers->RemoveClientListener(this);

	safeproxy::UnloadTransmitHook();
	safeproxy::UnloadHooks();
}

void CSafeProxyExt::SDK_OnAllLoaded()
{
	safeproxy::AutoDetectTransformingProxies();
}

void CSafeProxyExt::OnCoreMapEnd()
{
	safeproxy::g_OverrideStore.Clear();
	safeproxy::g_TransmitStore.Clear();
	safeproxy::ClearReencodeState();
}

#if defined SMEXT_CONF_METAMOD
bool CSafeProxyExt::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	g_pServer = engine->GetIServer();

	GET_V_IFACE_CURRENT(GetServerFactory, g_pServerGameEnts, IServerGameEnts, INTERFACEVERSION_SERVERGAMEENTS);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);

	gpGlobals = ismm->GetCGlobals();

	return true;
}
#endif

// A client rejected in OnClientConnect gets no disconnect callbacks and can leave entries in its slot.
bool CSafeProxyExt::InterceptClientConnect(int client, char *error, size_t maxlength)
{
	safeproxy::g_OverrideStore.OnClientDisconnected(client);
	safeproxy::g_TransmitStore.OnClientDisconnected(client);
	return true;
}

void CSafeProxyExt::OnClientConnected(int client)
{
	char error[255];
	if (!safeproxy::HookBaselineAck(error, sizeof(error)))
		g_pSM->LogError(myself, "%s", error);
}

void CSafeProxyExt::OnClientDisconnecting(int client)
{
	safeproxy::ClearReencodeSlot(client - 1);
	safeproxy::ForgetTransmitInfo(client - 1);
}

// Plugins can still add entries during the game's ClientDisconnect.
void CSafeProxyExt::OnClientDisconnected(int client)
{
	// Mid-encode, leave it to InterceptClientConnect when the slot is reused.
	if (!safeproxy::IsEncoding())
		safeproxy::g_OverrideStore.OnClientDisconnected(client);
	safeproxy::g_TransmitStore.OnClientDisconnected(client);
}

// An unpause is still reported if the plugin failed in its own OnPluginPauseChange.
void CSafeProxyExt::OnPluginPauseChange(IPlugin *plugin, bool paused)
{
	safeproxy::SetSuspended(plugin->GetBaseContext(), plugin->GetStatus() != Plugin_Running);
}

void CSafeProxyExt::OnPluginUnloaded(IPlugin *plugin)
{
	safeproxy::g_OverrideStore.ClearOwner(plugin->GetBaseContext());
	safeproxy::g_TransmitStore.ClearOwner(plugin->GetBaseContext());
	safeproxy::SetSuspended(plugin->GetBaseContext(), false);
}
