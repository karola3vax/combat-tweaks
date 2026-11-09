#pragma once

// Anti-rush barrier: a grey outline around each configured box, and players stopped at it like a wall,
// until either team is down to one living player or the unlock time has passed.

#include <string>

class CSchemaSystemTypeScope;
class INetworkServerService;

namespace barrier
{

// False (with a reason) when CS2 changed something the barrier needs; the rest of the
// plugin keeps working without it.
bool Init(CSchemaSystemTypeScope *scope, const void *server_module_address, INetworkServerService *network_service,
	const std::string &config_path, std::string &error);
void Frame();
void Shutdown();

} // namespace barrier
