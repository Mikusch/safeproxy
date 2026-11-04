# SafeProxy

A SourceMod extension that allows overriding SendProp values and entity visibility per client, without changing the entities themselves.

It's designed as a safer and more performant alternative to [SendProxy Manager](https://forums.alliedmods.net/showthread.php?t=169795), hence the name.

> [!NOTE]
> You do **not** need to set `sv_parallel_packentities` or `sv_parallel_sendsnapshot` to `0` for this extension to work.

Supported Games:

* Team Fortress 2
* Counter-Strike: Source
* Half-Life 2 Deathmatch
* Day of Defeat: Source
* Left 4 Dead 2

The extension runs on both 64-bit and 32-bit servers.

## Installation

Download the release archive for your SourceMod version and operating system, and extract it into your game directory.

## Examples

The examples are written for Team Fortress 2, but the concepts apply to all supported games.

All examples assume you have included [`safeproxy.inc`](/include/safeproxy.inc).

### Overriding Values

Overrides take the same values as `SetEntProp`, and `SAFEPROXY_ALL_CLIENTS` (the default) overrides a prop for all clients.
`SafeProxy_SetPropReal` lets one client see the real value anyway.

This example shows players only their own ping on the scoreboard. The player resource keeps everyone's ping in an array indexed by client:

```sourcepawn
#include <sdktools>

public void OnClientPutInServer(int client)
{
	int resource = GetPlayerResourceEntity();
	if (resource == -1)
		return;

	SafeProxy_SetProp(resource, "m_iPing", 0, _, client);
	SafeProxy_SetPropReal(resource, "m_iPing", client, client);
}
```

### Overriding Single Bits

`SafeProxy_SetPropBits` overrides just the masked bits, so the other bits keep their real values and stay free for other plugins to override.

This example shows a bounty target's outline and marked-for-death skull only to its hunter, while the target's other conditions still show.
Forcing the target sends it to the hunter even when the game wouldn't, so the outline shows across the map. `SafeProxyTransmit_ForceWithChildren` also sends its cosmetics:

```sourcepawn
#include <tf2>

void SetBountyTarget(int hunter, int target)
{
	int bit = 1 << view_as<int>(TFCond_MarkedForDeath);
	SafeProxy_SetProp(target, "m_bGlowEnabled", true, hunter);
	SafeProxy_SetPropBits(target, "m_nPlayerCond", bit, bit, hunter);
	SafeProxy_SetTransmit(target, SafeProxyTransmit_ForceWithChildren, hunter);
}

void ClearBountyTarget(int hunter, int target)
{
	int bit = 1 << view_as<int>(TFCond_MarkedForDeath);
	SafeProxy_UnsetProp(target, "m_bGlowEnabled", hunter);
	SafeProxy_UnsetPropBits(target, "m_nPlayerCond", bit, hunter);
	SafeProxy_UnsetTransmit(target, hunter);
}
```

### Computing Values with a Callback

A callback supplies the value for each client whenever the entity is sent to them.
Returning anything but `Plugin_Changed` leaves the override out, and the real value or other plugins' overrides apply.

> [!TIP]
> Callbacks run on every snapshot for each client the entity is sent to, so keep them cheap.
> If a value only changes now and then, setting and unsetting a regular override when it changes, for example from a timer, costs far less.

This example outlines teammates below half health for medics:

```sourcepawn
#include <tf2_stocks>

public void OnClientPutInServer(int client)
{
	SafeProxy_SetPropCallback(client, "m_bGlowEnabled", Callback_GlowEnabled);
}

Action Callback_GlowEnabled(int player, const char[] prop, int &value, int client, int element)
{
	if (player == client || TF2_GetPlayerClass(client) != TFClass_Medic || GetClientTeam(client) != GetClientTeam(player))
		return Plugin_Continue;

	if (!IsPlayerAlive(player) || GetClientHealth(player) * 2 >= GetEntProp(player, Prop_Data, "m_iMaxHealth"))
		return Plugin_Continue;

	value = true;
	return Plugin_Changed;
}
```

### Hiding Entities

Hiding an entity also hides everything parented to it, like a player's weapons and cosmetics.
Transmit states don't affect sounds, so you can additionally filter them with `SafeProxy_IsHidden`.

This example adds a `!hide` command that allows hiding other players:

```sourcepawn
#include <sdktools>

bool g_bHidePlayers[MAXPLAYERS + 1];

public void OnPluginStart()
{
	RegConsoleCmd("sm_hide", Command_Hide);
	AddNormalSoundHook(OnNormalSound);
}

public void OnClientPutInServer(int client)
{
	g_bHidePlayers[client] = false;

	for (int viewer = 1; viewer <= MaxClients; viewer++)
	{
		if (g_bHidePlayers[viewer] && IsClientInGame(viewer))
			SafeProxy_SetTransmit(client, SafeProxyTransmit_Hide, viewer);
	}
}

Action Command_Hide(int client, int args)
{
	if (client == 0)
		return Plugin_Handled;

	g_bHidePlayers[client] = !g_bHidePlayers[client];

	for (int other = 1; other <= MaxClients; other++)
	{
		if (other == client || !IsClientInGame(other))
			continue;

		if (g_bHidePlayers[client])
			SafeProxy_SetTransmit(other, SafeProxyTransmit_Hide, client);
		else
			SafeProxy_UnsetTransmit(other, client);
	}

	ReplyToCommand(client, "Other players are now %s.", g_bHidePlayers[client] ? "hidden" : "visible");
	return Plugin_Handled;
}

Action OnNormalSound(int clients[MAXPLAYERS], int &numClients, char sample[PLATFORM_MAX_PATH], int &entity, int &channel, float &volume, int &level, int &pitch, int &flags, char soundEntry[PLATFORM_MAX_PATH], int &seed)
{
	if (!IsValidEntity(entity))
		return Plugin_Continue;

	int count = 0;
	for (int i = 0; i < numClients; i++)
	{
		if (!SafeProxy_IsHidden(entity, clients[i]))
			clients[count++] = clients[i];
	}

	if (count == numClients)
		return Plugin_Continue;

	numClients = count;
	return Plugin_Changed;
}
```

## Building

Requires [AMBuild](https://github.com/alliedmodders/ambuild).

```bash
git clone --recursive https://github.com/Mikusch/safeproxy
cd safeproxy
mkdir build && cd build
python3 ../configure.py \
  --sdk=tf2 \
  --mms-path=/path/to/metamod-source \
  --sm-path=/path/to/sourcemod \
  --sdk-root=/path/to/hl2sdks \
  --enable-optimize
ambuild
```
