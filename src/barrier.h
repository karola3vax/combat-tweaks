#pragma once

// Anti-rush barrier: the game's retake barrier panels around each zone of the map, and players stopped at it like a wall,
// until either team is down to a few living players or the unlock time has passed. Also the in-game
// zone editor.

#include "anti_rush.h"
#include "config.h"

#include <string>
#include <vector>

class CSchemaSystemTypeScope;
class INetworkServerService;

namespace barrier
{

// False (with a reason) when CS2 changed something the barrier needs; the rest of the
// plugin keeps working without it.
bool Init(CSchemaSystemTypeScope *scope, const void *server_module_address, INetworkServerService *network_service,
	const std::string &zones_path, std::string &error);
void Configure(const config::Settings::AntiRush &settings);
void ReloadZones();
void Frame();
void Shutdown();

// One line per fact, for "ct status".
void Status(std::vector<std::string> &lines);

// Zone editor, for one admin.
enum class Corner
{
	First,  // first corner stored
	Second, // both stored; the box is shown
	Dead,   // the admin must be alive
};
Corner ZoneCorner(int slot);
int ZoneSave(int slot); // the new zone's number, or 0 when no box is ready
void ZoneCancel(int slot);
const std::vector<anti_rush::Box> &Zones();
bool ZoneRemove(int number);
const std::string &Map();

} // namespace barrier
