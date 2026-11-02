#ifndef _INCLUDE_SAFEPROXY_EXTENSION_H_
#define _INCLUDE_SAFEPROXY_EXTENSION_H_

#include "smsdk_ext.h"

#include <IPlayerHelpers.h>

class CSafeProxyExt
	: public SDKExtension
	, public SourceMod::IClientListener
	, public IPluginsListener
{
public:
	bool SDK_OnLoad(char *error, size_t maxlen, bool late) override;
	void SDK_OnUnload() override;
	void SDK_OnAllLoaded() override;
	void OnCoreMapEnd() override;

#if defined SMEXT_CONF_METAMOD
	bool SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late) override;
#endif

	bool InterceptClientConnect(int client, char *error, size_t maxlength) override;
	void OnClientDisconnecting(int client) override;
	void OnClientDisconnected(int client) override;

	void OnPluginPauseChange(IPlugin *plugin, bool paused) override;
	void OnPluginUnloaded(IPlugin *plugin) override;
};

extern CSafeProxyExt g_SafeProxyExt;

#endif // _INCLUDE_SAFEPROXY_EXTENSION_H_
