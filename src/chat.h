#pragma once

// Chat and console lines to players, each in their own game language (translations/*.json).

#include <string>
#include <utility>
#include <vector>

namespace chat
{

using Values = std::vector<std::pair<std::string, std::string>>;

// False when CS2 changed the interfaces messages need; then nothing is sent, the rest works.
bool Init(const std::string &translations_dir);
void Reload();
void Frame(); // asks new players for their language and takes the answers
void Shutdown();

// `team` colors the {team} token: 2 = T (yellow), 3 = CT (light blue).
void SendAll(const char *key, const Values &values = {}, int team = 0);
// Slot -1 is the server console (English, no colors).
void Send(int slot, const char *key, const Values &values = {});
// A text in the player's language (English for slot -1), for other places than chat.
std::string Format(int slot, const char *key, const Values &values = {});

// A plain line in a player's console, or the server console for slot -1.
void Console(int slot, const std::string &line);

// For "ct status".
void Status(std::vector<std::string> &lines);

} // namespace chat
