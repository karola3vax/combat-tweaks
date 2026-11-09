#include "chat.h"

#include "messages.h"
#include "player_language.h"

#include <ISmmPlugin.h>
#include <engine2/igameeventsystem.h>
#include <engine2/inetworkserverservice.h>
#include <entity2/entityinstance.h>
#include <gameentitysystem.h>
#include <irecipientfilter.h>
#include <networksystem/inetworkmessages.h>
#include <engine2/ivengineserver2.h>

#include "usermessages.pb.h"

#include <chrono>
#include <cstring>

extern ISmmAPI *g_SMAPI;

namespace chat
{
namespace
{

constexpr int kHudPrintConsole = 2; // TextMsg dest
constexpr int kHudPrintTalk = 3;

INetworkMessages *network_messages = nullptr;
IGameEventSystem *events = nullptr;
messages::Messages texts;
std::string folder;
std::vector<std::string> load_errors;
bool ready = false;
std::chrono::steady_clock::time_point next_query {};

class OneRecipient final : public IRecipientFilter
{
public:
	explicit OneRecipient(int slot) { recipients_.Set(slot); }
	NetChannelBufType_t GetNetworkBufType() const override { return BUF_RELIABLE; }
	bool IsInitMessage() const override { return false; }
	const CPlayerBitVec &GetRecipients() const override { return recipients_; }
	CPlayerSlot GetPredictedPlayerSlot() const override { return CPlayerSlot(-1); }

private:
	CPlayerBitVec recipients_; // starts empty
};

bool IsPlayer(int slot)
{
	CGameEntitySystem *entities = GameEntitySystem();
	CEntityInstance *controller = entities ? entities->GetPlayerController(CPlayerSlot(slot)) : nullptr;
	return controller && std::strcmp(controller->GetClassname(), "cs_player_controller") == 0;
}

void Post(int slot, int destination, const std::string &text)
{
	if (!ready || slot < 0 || slot >= 64) return;
	INetworkMessageInternal *type = network_messages->FindNetworkMessagePartial("TextMsg");
	CNetMessage *data = type ? type->AllocateMessage() : nullptr;
	auto *message = data ? data->ToPB<CUserMessageTextMsg>() : nullptr;
	if (message)
	{
		message->set_dest(destination);
		for (const char *param : {text.c_str(), "", "", "", ""}) message->add_param(param);
		OneRecipient recipient(slot);
		events->PostEventAbstract(0, false, &recipient, type, data, 0);
	}
	if (data) network_messages->DeallocateNetMessageAbstract(type, data);
}

// For the server console: no color bytes and no leading space.
std::string Plain(std::string text)
{
	std::string out;
	for (char c : text)
		if (static_cast<unsigned char>(c) >= 0x20) out += c;
	const auto start = out.find_first_not_of(' ');
	return start == std::string::npos ? "" : out.substr(start);
}

const char *TeamColor(int team)
{
	return team == 2 ? "{yellow}" : team == 3 ? "{lightblue}" : "{default}";
}

void LoadTexts()
{
	load_errors = texts.Load(folder);
	for (const std::string &line : load_errors) META_CONPRINTF("[COMBAT_TWEAKS] translations: %s\n", line.c_str());
}

} // namespace

bool Init(const std::string &translations_dir)
{
	folder = translations_dir;
	LoadTexts();
	CreateInterfaceFn engine_factory = g_SMAPI->GetEngineFactory();
	network_messages = static_cast<INetworkMessages *>(engine_factory(NETWORKMESSAGES_INTERFACE_VERSION, nullptr));
	events = static_cast<IGameEventSystem *>(engine_factory(GAMEEVENTSYSTEM_INTERFACE_VERSION, nullptr));
	auto *network = static_cast<INetworkServerService *>(engine_factory(NETWORKSERVERSERVICE_INTERFACE_VERSION, nullptr));
	auto *engine = static_cast<IVEngineServer2 *>(engine_factory(SOURCE2ENGINETOSERVER_INTERFACE_VERSION, nullptr));
	ready = network_messages && events;
	std::string error;
	if (ready && !language::Init(network, network_messages, events, engine, error))
		META_CONPRINTF("[COMBAT_TWEAKS] Everyone gets English messages: %s.\n", error.c_str());
	return ready;
}

void Reload()
{
	if (!folder.empty()) LoadTexts();
}

void Frame()
{
	if (!ready) return;
	const auto now = std::chrono::steady_clock::now();
	if (now < next_query) return;
	next_query = now + std::chrono::seconds(1);
	for (int slot = 0; slot < 64; ++slot) language::Query(slot); // skips bots, known languages and empty slots
}

void Shutdown()
{
	if (!ready) return;
	language::Shutdown();
	ready = false;
}

void SendAll(const char *key, const Values &values, int team)
{
	Values with_team = values;
	with_team.emplace_back("team", TeamColor(team));
	for (int slot = 0; slot < 64; ++slot)
		if (IsPlayer(slot)) Post(slot, kHudPrintTalk, texts.Format(language::Get(slot), key, with_team));
}

void Send(int slot, const char *key, const Values &values)
{
	if (slot < 0) META_CONPRINTF("%s\n", Plain(texts.Format("en", key, values)).c_str());
	else Post(slot, kHudPrintTalk, texts.Format(language::Get(slot), key, values));
}

void Console(int slot, const std::string &line)
{
	if (slot < 0) META_CONPRINTF("%s\n", line.c_str());
	else Post(slot, kHudPrintConsole, line + "\n");
}

void Status(std::vector<std::string> &lines)
{
	lines.push_back("Chat: " + std::string(ready ? "on" : "off (this CS2 version changed how messages are sent)") + "; "
		+ std::to_string(texts.Texts().size()) + " language file(s)");
	for (const std::string &error : load_errors) lines.push_back("translations: " + error);
}

} // namespace chat
