#include "admin.h"

#include "barrier.h"
#include "chat.h"
#include "schema.h"

#include <ISmmPlugin.h>
#include <entity2/entityinstance.h>
#include <gameentitysystem.h>
#include <tier0/convar.h>
#include <tier0/icvar.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace admin
{
namespace
{

Hooks hooks;
std::vector<std::uint64_t> admins;
std::ptrdiff_t steam_id_offset = -1; // CBasePlayerController::m_steamID
bool ready = false;

bool Allowed(int slot)
{
	if (slot < 0) return true; // the server console
	CGameEntitySystem *entities = GameEntitySystem();
	CEntityInstance *controller = entities ? entities->GetPlayerController(CPlayerSlot(slot)) : nullptr;
	if (!controller) return false;
	const auto id = schema::Field<std::uint64_t>(controller, steam_id_offset);
	return id != 0 && std::find(admins.begin(), admins.end(), id) != admins.end();
}

std::string Point(const anti_rush::Vec &v)
{
	char text[64];
	std::snprintf(text, sizeof(text), "(%.0f, %.0f, %.0f)", v[0], v[1], v[2]);
	return text;
}

std::string Lower(std::string text)
{
	for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return text;
}

void Run(int slot, const std::vector<std::string> &words)
{
	if (!Allowed(slot))
	{
		chat::Send(slot, "no_access");
		return;
	}
	const std::string command = words.empty() ? "" : Lower(words[0]);
	const std::string sub = words.size() > 1 ? Lower(words[1]) : "";
	const std::string &map = barrier::Map();
	if (command == "status")
	{
		std::vector<std::string> lines;
		hooks.status(lines);
		for (const std::string &line : lines) chat::Console(slot, line);
		if (slot >= 0) chat::Send(slot, "status_in_console");
	}
	else if (command == "reload")
	{
		hooks.reload();
		chat::Send(slot, "reloaded");
	}
	else if (command == "zone" && sub.empty())
	{
		switch (barrier::ZoneCorner(slot))
		{
		case barrier::Corner::First: chat::Send(slot, "zone_corner1"); break;
		case barrier::Corner::Second: chat::Send(slot, "zone_corner2"); break;
		case barrier::Corner::Dead: chat::Send(slot, "must_be_alive"); break;
		}
	}
	else if (command == "zone" && sub == "save")
	{
		const int number = barrier::ZoneSave(slot);
		if (number) chat::Send(slot, "zone_saved", {{"number", std::to_string(number)}, {"map", map}});
		else chat::Send(slot, "zone_nothing");
	}
	else if (command == "zone" && sub == "cancel")
	{
		barrier::ZoneCancel(slot);
		chat::Send(slot, "zone_cancelled");
	}
	else if (command == "zone" && sub == "list")
	{
		const auto &zones = barrier::Zones();
		if (zones.empty()) chat::Send(slot, "zone_list_empty", {{"map", map}});
		for (std::size_t i = 0; i < zones.size(); ++i)
			chat::Send(slot, "zone_list_item",
				{{"number", std::to_string(i + 1)}, {"from", Point(zones[i].min)}, {"to", Point(zones[i].max)}});
	}
	else if (command == "zone" && sub == "remove" && words.size() > 2)
	{
		const std::string &argument = words[2];
		const int number = std::all_of(argument.begin(), argument.end(), ::isdigit) && argument.size() < 4 ? std::stoi(argument) : 0;
		if (barrier::ZoneRemove(number)) chat::Send(slot, "zone_removed", {{"number", argument}, {"map", map}});
		else chat::Send(slot, "zone_not_found", {{"number", argument}, {"map", map}});
	}
	else
		chat::Send(slot, "help");
}

std::vector<std::string> Words(const std::string &text)
{
	std::istringstream stream(text);
	std::vector<std::string> words;
	for (std::string word; stream >> word;) words.push_back(word);
	return words;
}

// "!ct zone save" in chat: shown to everyone. "/ct ..." is the same, but hidden.
class SayHook
{
public:
	KHook::Return<void> OnDispatch(ICvar *, ConCommandRef, const CCommandContext &context, const CCommand &args)
	{
		const int slot = context.GetPlayerSlot().Get();
		if (!ready || slot < 0 || args.ArgC() < 2) return {KHook::Action::Ignore};
		if (std::strcmp(args.Arg(0), "say") != 0 && std::strcmp(args.Arg(0), "say_team") != 0) return {KHook::Action::Ignore};
		std::vector<std::string> words = Words(args.ArgS());
		if (!words.empty() && words.front().size() > 1 && words.front().front() == '"') // "say" quotes the message
			words.front().erase(0, 1);
		if (!words.empty() && !words.back().empty() && words.back().back() == '"') words.back().pop_back();
		if (words.empty()) return {KHook::Action::Ignore};
		const std::string first = Lower(words.front());
		if (first != "!ct" && first != "/ct") return {KHook::Action::Ignore};
		words.erase(words.begin());
		Run(slot, words);
		return {first == "/ct" ? KHook::Action::Supersede : KHook::Action::Ignore};
	}
} say_hook;

KHook::Virtual<ICvar, void, ConCommandRef, const CCommandContext &, const CCommand &> dispatch_hook(
	&ICvar::DispatchConCommand, &say_hook, &SayHook::OnDispatch, nullptr);

} // namespace

CON_COMMAND_F(ct, "Combat Tweaks: status, zone, zone save, zone cancel, zone list, zone remove <number>, reload",
	FCVAR_RELEASE | FCVAR_CLIENT_CAN_EXECUTE)
{
	if (!ready) return;
	std::vector<std::string> words;
	for (int i = 1; i < args.ArgC(); ++i) words.emplace_back(args.Arg(i));
	Run(context.GetPlayerSlot().Get(), words);
}

bool Init(CSchemaSystemTypeScope *scope, Hooks given)
{
	hooks = std::move(given);
	steam_id_offset = schema::FieldOffset(scope, "CBasePlayerController", "m_steamID");
	if (steam_id_offset < 0) return false;
	ConVar_Register(FCVAR_RELEASE | FCVAR_GAMEDLL);
	dispatch_hook.Add(g_pCVar);
	ready = true;
	return true;
}

void Configure(const std::vector<std::uint64_t> &list)
{
	admins = list;
}

void Shutdown()
{
	if (!ready) return;
	ready = false;
	dispatch_hook.Remove(g_pCVar);
	ConVar_Unregister();
}

} // namespace admin
