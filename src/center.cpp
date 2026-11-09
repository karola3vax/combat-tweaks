#include "center.h"

#include "module.h"
#include "recipient.h"

#include <ISmmPlugin.h>
#include <engine2/igameeventsystem.h>
#include <igameevents.h>
#include <tier0/generichash.h>
#include <networksystem/inetworkmessages.h>

#include "gameevents.pb.h"

extern ISmmAPI *g_SMAPI;

namespace center
{
namespace
{

IGameEventManager2 *manager = nullptr;
INetworkMessages *network_messages = nullptr;
IGameEventSystem *events = nullptr;
void *vtable_holder = nullptr; // stands in for an object, so the hook can be placed on the class's vtable

class Catcher
{
public:
	KHook::Return<int> OnLoadEventsFromFile(IGameEventManager2 *self, const char *, bool)
	{
		manager = self;
		return {KHook::Action::Ignore};
	}
} catcher;

KHook::Virtual<IGameEventManager2, int, const char *, bool> load_events_hook(
	&IGameEventManager2::LoadEventsFromFile, &catcher, &Catcher::OnLoadEventsFromFile, nullptr);

} // namespace

bool Init(const void *server_module_address)
{
	CreateInterfaceFn engine_factory = g_SMAPI->GetEngineFactory();
	network_messages = static_cast<INetworkMessages *>(engine_factory(NETWORKMESSAGES_INTERFACE_VERSION, nullptr));
	events = static_cast<IGameEventSystem *>(engine_factory(GAMEEVENTSYSTEM_INTERFACE_VERSION, nullptr));
	vtable_holder = module::FindVtable(module::Segments(server_module_address), "CGameEventManager");
	if (!network_messages || !events || !vtable_holder) return false;
	// The game keeps one global event manager; with it found, this works at once, even after a late load.
	manager = static_cast<IGameEventManager2 *>(module::FindStaticObject(module::Segments(server_module_address), vtable_holder));
	META_CONPRINTF("[COMBAT_TWEAKS] Event manager %s.\n", manager ? "found" : "not found yet; waiting for the game to load its events");
	load_events_hook.Add(reinterpret_cast<IGameEventManager2 *>(&vtable_holder));
	return true;
}

bool Ready()
{
	return manager != nullptr;
}

void Send(int slot, const std::string &html)
{
	if (!manager || slot < 0 || slot >= 64) return;
	// Not forced: with nobody listening the game returns nothing and nothing is sent.
	IGameEvent *event = manager->CreateEvent("show_survival_respawn_status");
	if (!event) return;
	INetworkMessageInternal *type = network_messages->FindNetworkMessageById(GE_Source1LegacyGameEvent);
	CNetMessage *data = type ? type->AllocateMessage() : nullptr;
	if (data)
	{
		event->SetString("loc_token", html.c_str());
		event->SetInt("duration", 1);
		event->SetInt("userid", -1);
		manager->SerializeEvent(event, data->ToPB<CMsgSource1LegacyGameEvent>());
		OneRecipient recipient(slot);
		events->PostEventAbstract(-1, false, &recipient, type, data, 0);
		network_messages->DeallocateNetMessageAbstract(type, data);
	}
	manager->FreeEvent(event);
}

void PlaySound(int slot, const char *sound_event)
{
	if (!network_messages || !events || slot < 0 || slot >= 64) return;
	INetworkMessageInternal *type = network_messages->FindNetworkMessageById(GE_SosStartSoundEvent);
	CNetMessage *data = type ? type->AllocateMessage() : nullptr;
	auto *message = data ? data->ToPB<CMsgSosStartSoundEvent>() : nullptr;
	if (message)
	{
		static int guid = 0x43540000; // our own range, so it never matches a sound the game started
		message->set_soundevent_guid(++guid);
		// The game names sound events by this hash (seed "SRC2").
		message->set_soundevent_hash(MurmurHash2LowerCase(sound_event, 0x53524332u));
		OneRecipient recipient(slot);
		events->PostEventAbstract(-1, false, &recipient, type, data, 0);
	}
	if (data) network_messages->DeallocateNetMessageAbstract(type, data);
}

void Shutdown()
{
	if (vtable_holder) load_events_hook.Remove(reinterpret_cast<IGameEventManager2 *>(&vtable_holder));
	vtable_holder = nullptr;
	manager = nullptr;
}

} // namespace center
