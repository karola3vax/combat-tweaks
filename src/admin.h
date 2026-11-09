#pragma once

// The "ct" command: console ("ct ...") and chat ("!ct ..." shown, "/ct ..." hidden). Only the server
// console and the SteamID64s in the config may use it. Also "!rs" / "/rs" in chat, for every player, which
// resets their own score.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class CSchemaSystemTypeScope;

namespace admin
{

struct Hooks
{
	std::function<void()> reload;                            // re-read config, zones and messages
	std::function<void(std::vector<std::string> &)> status; // lines for "ct status"
};

bool Init(CSchemaSystemTypeScope *scope, Hooks hooks);
void Configure(const std::vector<std::uint64_t> &admins, bool reset_score);
void Shutdown();

} // namespace admin
