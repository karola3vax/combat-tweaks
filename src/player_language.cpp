#include "player_language.h"

#include "messages.h"

#include <ISmmPlugin.h>
#include <engine2/igameeventsystem.h>
#include <engine2/inetworkserverservice.h>
#include <engine2/ivengineserver2.h>
#include <engine2/networkgameserverbase.h>
#include <engine2/serversideclientbase.h>
#include <irecipientfilter.h>
#include <networksystem/inetworkmessages.h>

#include <array>
#include <chrono>
#include <mutex>
#include <random>

PLUGIN_GLOBALVARS();

namespace language
{

namespace
{

using Clock = std::chrono::steady_clock;

constexpr const char *kCvar = "cl_language";
constexpr int kValueIntact = 0;          // CCLCMsg_RespondCvarValue status: the value was read
constexpr auto kRetry = std::chrono::seconds(10); // a game still loading may not answer

class OnePlayer final : public IRecipientFilter
{
public:
	explicit OnePlayer(int slot)
	{
		recipients_.ClearAll();
		recipients_.Set(slot);
	}
	NetChannelBufType_t GetNetworkBufType() const override { return BUF_RELIABLE; }
	bool IsInitMessage() const override { return false; }
	const CPlayerBitVec &GetRecipients() const override { return recipients_; }
	CPlayerSlot GetPredictedPlayerSlot() const override { return CPlayerSlot(-1); }

private:
	CPlayerBitVec recipients_;
};

struct Player
{
	const CServerSideClientBase *client = nullptr; // whose game the open query went to
	int cookie = 0;                                // 0: no open query
	Clock::time_point asked {};
	std::string file; // "" until the game answers
};

INetworkServerService *network = nullptr;
INetworkMessages *net_messages = nullptr;
IGameEventSystem *events = nullptr;
IVEngineServer2 *engine = nullptr;
INetworkMessageInternal *get_cvar = nullptr;
void **hooked_vtable = nullptr;
std::mt19937 random_cookie {std::random_device {}()};

// The answer can arrive on a network thread, so players are shared under this lock.
std::mutex lock;
std::array<Player, ABSOLUTE_PLAYER_LIMIT> players;
bool active = false;

KHook::Return<bool> OnRespond(CServerSideClientBase *client, const CNetMessagePB<CCLCMsg_RespondCvarValue> &answer)
{
	if (answer.name() == kCvar && answer.cookie() != 0)
	{
		std::lock_guard guard(lock);
		// The cookie is random and the client must match, so one player cannot answer for another.
		for (Player &player : players)
		{
			if (!active || player.cookie != answer.cookie() || player.client != client) continue;
			player.cookie = 0;
			player.file = answer.status_code() == kValueIntact ? std::string(messages::LanguageFile(answer.value())) : "en";
			break;
		}
	}
	return {KHook::Action::Ignore, true};
}

// The engine's own handler for this message only returns true (checked on build 2000942).
KHook::Virtual<CServerSideClientBase, bool, const CNetMessagePB<CCLCMsg_RespondCvarValue> &> respond_hook(
	&CServerSideClientBase::ProcessRespondCvarValue, &OnRespond, nullptr);

CServerSideClientBase *Client(int slot)
{
	auto *server = network ? network->GetIGameServer() : nullptr;
	if (!server || slot >= server->m_Clients.Count()) return nullptr;
	return server->m_Clients[slot];
}

} // namespace

bool Init(INetworkServerService *network_service, INetworkMessages *messages_system, IGameEventSystem *event_system,
	IVEngineServer2 *engine_server, std::string &error)
{
	if (!network_service || !messages_system || !event_system || !engine_server)
	{
		error = "an engine interface is missing";
		return false;
	}
	network = network_service;
	net_messages = messages_system;
	events = event_system;
	engine = engine_server;
	std::lock_guard guard(lock);
	players = {};
	active = true;
	return true;
}

void Shutdown()
{
	{
		std::lock_guard guard(lock);
		active = false;
	}
	// KHook removes the vtable patch itself when the plugin library unloads.
	if (hooked_vtable) respond_hook.RemoveGlobal(reinterpret_cast<CServerSideClientBase *>(&hooked_vtable));
	hooked_vtable = nullptr;
	network = nullptr;
	get_cvar = nullptr;
}

void Query(int slot)
{
	if (!network || slot < 0 || slot >= ABSOLUTE_PLAYER_LIMIT) return;
	CServerSideClientBase *client = engine->GetPlayerNetInfo(CPlayerSlot(slot)) ? Client(slot) : nullptr; // bots have no net channel
	if (!client)
	{
		Forget(slot);
		return;
	}
	const auto now = Clock::now();
	{
		std::lock_guard guard(lock);
		Player &player = players[slot];
		if (player.client != client) player = {}; // someone else has the slot now
		if (!player.file.empty() || (player.cookie != 0 && now - player.asked < kRetry)) return;
	}
	if (!get_cvar) get_cvar = net_messages->FindNetworkMessagePartial("CSVCMsg_GetCvarValue");
	if (!get_cvar) return;
	if (!hooked_vtable)
	{
		// Every CServerSideClient shares this vtable, so hooking it once covers all players.
		hooked_vtable = *reinterpret_cast<void ***>(client);
		respond_hook.AddGlobal(client);
	}

	int cookie = 0;
	while (cookie == 0) cookie = static_cast<int>(random_cookie() & 0x7FFFFFFF);
	{
		std::lock_guard guard(lock);
		players[slot] = {client, cookie, now, {}};
	}
	CNetMessage *message = get_cvar->AllocateMessage();
	if (!message) return;
	auto *query = message->ToPB<CSVCMsg_GetCvarValue>();
	query->set_cookie(cookie);
	query->set_cvar_name(kCvar);
	OnePlayer filter(slot);
	events->PostEventAbstract(0, false, &filter, get_cvar, message, 0);
	net_messages->DeallocateNetMessageAbstract(get_cvar, message);
}

std::string Get(int slot)
{
	if (slot < 0 || slot >= ABSOLUTE_PLAYER_LIMIT) return "en";
	std::lock_guard guard(lock);
	return players[slot].file.empty() ? "en" : players[slot].file;
}

void Forget(int slot)
{
	if (slot < 0 || slot >= ABSOLUTE_PLAYER_LIMIT) return;
	std::lock_guard guard(lock);
	players[slot] = {};
}

} // namespace language
