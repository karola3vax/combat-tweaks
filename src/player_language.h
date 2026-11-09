#pragma once

// Each player's CS2 language. CS2 does not send cl_language with a player's info, so the server
// asks each player's game for it (CSVCMsg_GetCvarValue) and reads the answer
// (CCLCMsg_RespondCvarValue, through a KHook on CServerSideClientBase::ProcessRespondCvarValue).

#include <string>

class INetworkServerService;
class INetworkMessages;
class IGameEventSystem;
class IVEngineServer2;

namespace language
{

// False, with the reason, when an interface is missing; then Get always says "en".
bool Init(INetworkServerService *network, INetworkMessages *messages, IGameEventSystem *events, IVEngineServer2 *engine,
	std::string &error);
void Shutdown();

// Asks the player's game, unless it already answered or was asked in the last 10 seconds, so it
// is safe to call for every slot each second. A slot with no connected human forgets its language.
// Game thread only.
void Query(int slot);
// The translation file code ("tr", "pt", ...); "en" until the player's game answers.
std::string Get(int slot);
// On disconnect.
void Forget(int slot);

} // namespace language
