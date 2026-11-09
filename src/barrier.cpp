#include "barrier.h"

#include "anti_rush.h"
#include "schema.h"

#include <ISmmPlugin.h>
#include <entity2/entityinstance.h>
#include <entity2/entitykeyvalues.h>
#include <engine2/gameshareddefs.h>
#include <entity2/entitysystem.h>
#include <gameentitysystem.h>
#include <engine2/inetworkgameserver.h>
#include <engine2/inetworkserverservice.h>
#include <engine2/igameeventsystem.h>
#include <networksystem/inetworkmessages.h>
#include <variant.h>

#include "usermessages.pb.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <link.h>
#endif

extern ISmmAPI *g_SMAPI;

namespace barrier
{
namespace
{

constexpr float kPlayerHalfWidth = 16.0f; // the player's box sticks out this far from its origin
constexpr float kEjectRate = 10.0f; // per unit of overlap, so a player touching the edge barely moves
constexpr float kBeamWidth = 1.0f;
constexpr std::uint8_t kBeamAlpha = 160; // visible but not distracting
constexpr std::uint8_t kGrey = 120;         // grey: glowing white pulsed with auto-exposure when shooting
constexpr float kTouchHold = 0.5f;          // seconds the box stays lit after the last touch
constexpr std::uint8_t kRenderTransAlpha = 1;
constexpr float kRushSeconds = 0.5f; // walking into the barrier this long counts as rushing
constexpr float kGlowSeconds = 5.0f; // how long a rusher glows
constexpr int kGlowAlways = 3;       // m_iGlowType: through walls
constexpr int kHudPrintTalk = 3;     // TextMsg dest: chat
constexpr std::uint8_t kRenderNone = 2;

// Checked against build 2000942: each matches exactly once.
#ifdef _WIN32
constexpr const char *kCreateEntity = "48 83 EC ? C6 44 24 ? 00 4C 8B C1";
constexpr const char *kDispatchSpawn = "48 89 5C 24 ? 57 48 83 EC ? 48 8B DA 48 8B F9 48 85 C9 0F 84 ? ? ? ? 48 85 D2";
constexpr const char *kRemoveEntity = "48 85 C9 74 ? 48 8B D1 48 8B 0D";
constexpr const char *kAcceptInput = "48 89 5C 24 ? 48 89 6C 24 ? 56 57 41 56 48 81 EC ? ? ? ? 4D 8B F0 48 8B F1 48 8B 0D ? ? ? ? 4C 8B C2 48 8D 94 24 ? ? ? ?";
#else
constexpr const char *kCreateEntity = "48 8D 05 ? ? ? ? 55 48 89 FA";
constexpr const char *kDispatchSpawn = "48 85 FF 74 ? 55 48 89 E5 41 55 41 54 49 89 FC";
constexpr const char *kRemoveEntity = "48 89 FE 48 85 FF 74 ? 48 8D 05 ? ? ? ? 48";
constexpr const char *kAcceptInput = "55 48 89 E5 41 57 49 89 FF 41 56 48 8D BD ? ? ? ? 41 55 4C 8D AD ? ? ? ? 41 54 4D 89 C4 53";
#endif

struct Segment
{
	const std::uint8_t *start;
	std::size_t size;
};

CEntityInstance *(*create_entity)(const char *, int) = nullptr;
void (*dispatch_spawn)(CEntityInstance *, CEntityKeyValues *) = nullptr;
void (*remove_entity)(CEntityInstance *) = nullptr;
void (*accept_input)(CEntityInstance *, const char *, CEntityInstance *, CEntityInstance *, variant_t *, int, void *) = nullptr;

struct Offsets
{
	std::ptrdiff_t team, pawn_alive, pawn, body, scene_node, origin, velocity, rules, round_start, warmup, end_pos, width,
		end_width, color, render_mode, name, glow, glow_color, glow_team, glow_type, glow_start, glow_time, glow_range,
		glow_range_min, model_state, model_name;
} f;

anti_rush::Config config;
std::string config_path;
INetworkServerService *network = nullptr;
std::string map;
std::vector<anti_rush::Box> boxes; // for the current map
struct Beam
{
	CEntityHandle handle;
	std::size_t box;
};
std::vector<Beam> beams;
std::vector<float> last_touch; // curtime of the latest touch, per box
std::vector<bool> lit;         // per box: drawn red because a player is against it
CEntityHandle rules_proxy;
INetworkMessages *messages = nullptr;
IGameEventSystem *events = nullptr;

struct Rusher
{
	float pushing = 0.0f; // seconds of walking into the barrier without a break
	bool caught = false;  // announced this round
	CEntityHandle relay, glow; // the model copies that show the glow
	float glow_until = 0.0f;
};
std::array<Rusher, 64> rushers;
float last_round_start = -1.0f;
float last_frame_time = 0.0f;
int rules_search_frames = 0;

std::vector<Segment> ModuleSegments(const void *address)
{
	std::vector<Segment> segments;
#ifdef _WIN32
	HMODULE module = nullptr;
	MODULEINFO info {};
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			static_cast<const char *>(address), &module)
		&& GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)))
		segments.push_back({static_cast<const std::uint8_t *>(info.lpBaseOfDll), info.SizeOfImage});
#else
	struct Search
	{
		std::uintptr_t address;
		std::vector<Segment> *result;
	} search {reinterpret_cast<std::uintptr_t>(address), &segments};
	dl_iterate_phdr(
		[](dl_phdr_info *info, size_t, void *data) -> int {
			auto *search = static_cast<Search *>(data);
			std::vector<Segment> found;
			bool contains = false;
			for (int i = 0; i < info->dlpi_phnum; ++i)
			{
				const ElfW(Phdr) &header = info->dlpi_phdr[i];
				if (header.p_type != PT_LOAD || header.p_memsz == 0) continue;
				const std::uintptr_t start = info->dlpi_addr + header.p_vaddr;
				found.push_back({reinterpret_cast<const std::uint8_t *>(start), static_cast<std::size_t>(header.p_memsz)});
				contains |= search->address >= start && search->address < start + header.p_memsz;
			}
			if (!contains) return 0;
			*search->result = std::move(found);
			return 1;
		},
		&search);
#endif
	return segments;
}

// The one place in the module matching the pattern ("?" is any byte), or nullptr.
void *FindUnique(const std::vector<Segment> &segments, const char *pattern)
{
	std::vector<std::optional<std::uint8_t>> bytes;
	for (const char *at = pattern; *at;)
	{
		if (*at == ' ') ++at;
		else if (*at == '?') bytes.emplace_back(), ++at;
		else
		{
			bytes.emplace_back(static_cast<std::uint8_t>(std::strtoul(at, nullptr, 16)));
			at += 2;
		}
	}
	void *found = nullptr;
	int count = 0;
	for (const Segment &segment : segments)
	{
		if (bytes.size() > segment.size) continue;
		for (const std::uint8_t *at = segment.start; at + bytes.size() <= segment.start + segment.size && count < 2; ++at)
		{
			std::size_t i = 0;
			while (i < bytes.size() && (!bytes[i] || at[i] == *bytes[i])) ++i;
			if (i == bytes.size()) found = const_cast<std::uint8_t *>(at), ++count;
		}
	}
	return count == 1 ? found : nullptr;
}

std::string ReadFile(const std::string &path)
{
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void LoadConfig()
{
	std::vector<std::string> errors;
	const std::string text = ReadFile(config_path);
	config = anti_rush::ParseConfig(text, &errors);
	if (text.empty()) META_CONPRINTF("[NO_HIT_EFFECTS] Could not read %s; no anti-rush barriers.\n", config_path.c_str());
	for (const std::string &line : errors)
		META_CONPRINTF("[NO_HIT_EFFECTS] Skipped bad line in %s, %s\n", config_path.c_str(), line.c_str());
}

void RemoveBeams()
{
	CGameEntitySystem *entities = GameEntitySystem();
	for (const Beam &beam : beams)
		if (CEntityInstance *entity = entities ? entities->GetEntityInstance(beam.handle) : nullptr) remove_entity(entity);
	beams.clear();
}

void SpawnBeams()
{
	lit.assign(boxes.size(), false);
	for (std::size_t index = 0; index < boxes.size(); ++index)
		for (auto edge : anti_rush::Edges(boxes[index]))
		{
			for (auto &point : edge)
				if (point[2] == boxes[index].min[2]) point[2] += 1.0f; // just above the floor, so it doesn't flicker into it
			CEntityInstance *beam = create_entity("beam", -1);
			if (!beam) continue;
			auto *end = &schema::Field<float>(beam, f.end_pos);
			for (int axis = 0; axis < 3; ++axis) end[axis] = edge[1][axis];
			schema::Field<float>(beam, f.width) = kBeamWidth;
			schema::Field<float>(beam, f.end_width) = kBeamWidth;
			auto *color = &schema::Field<std::uint8_t>(beam, f.color); // r, g, b, a
			color[0] = color[1] = color[2] = kGrey;
			color[3] = kBeamAlpha;
			schema::Field<std::uint8_t>(beam, f.render_mode) = kRenderTransAlpha;
			auto *values = new CEntityKeyValues(); // the game frees it
			values->SetVector("origin", Vector(edge[0][0], edge[0][1], edge[0][2]));
			dispatch_spawn(beam, values);
			beams.push_back({beam->GetRefEHandle(), index});
		}
}

bool BeamsAlive()
{
	CGameEntitySystem *entities = GameEntitySystem();
	for (const Beam &beam : beams)
		if (!entities || !entities->GetEntityInstance(beam.handle)) return false;
	return !beams.empty();
}

// Recolors every beam of a box: full red while a player is against it, faint grey otherwise.
void SetBoxTouched(std::size_t box, bool touched)
{
	CGameEntitySystem *entities = GameEntitySystem();
	for (const Beam &beam : beams)
	{
		CEntityInstance *entity = beam.box == box && entities ? entities->GetEntityInstance(beam.handle) : nullptr;
		if (!entity) continue;
		auto *color = &schema::Field<std::uint8_t>(entity, f.color); // r, g, b, a
		color[0] = touched ? 255 : kGrey;
		color[1] = color[2] = touched ? 0 : kGrey;
		color[3] = touched ? 255 : kBeamAlpha;
		NetworkStateChangedData data(static_cast<uint32>(f.color));
		entity->NetworkStateChanged(data);
	}
}

void *GameRules()
{
	CGameEntitySystem *entities = GameEntitySystem();
	if (!entities) return nullptr;
	CEntityInstance *proxy = entities->GetEntityInstance(rules_proxy);
	// A full search once a second at most, until the map's cs_gamerules exists.
	if (!proxy && --rules_search_frames <= 0)
	{
		rules_search_frames = 64;
		for (int index = 0; index < MAX_TOTAL_ENTITIES && !proxy; ++index)
		{
			CEntityInstance *entity = entities->GetEntityInstance(CEntityIndex(index));
			if (entity && std::strcmp(entity->GetClassname(), "cs_gamerules") == 0) proxy = entity;
		}
		if (proxy) rules_proxy = proxy->GetRefEHandle();
	}
	return proxy ? schema::Field<void *>(proxy, f.rules) : nullptr;
}

// A prop_dynamic with the rusher's model. The relay copies the player's pose; the glow copy
// follows the relay and carries the glow, because CS2 draws no glow on player pawns (the way
// public ESP plugins do it).
void WriteGlow(CEntityInstance *prop, bool notify)
{
	void *glow = &schema::Field<std::byte>(prop, f.glow);
	auto *glow_color = &schema::Field<std::uint8_t>(glow, f.glow_color);
	glow_color[0] = 255;
	glow_color[1] = glow_color[2] = 0;
	glow_color[3] = 255;
	schema::Field<int>(glow, f.glow_team) = -1; // everyone sees it
	schema::Field<int>(glow, f.glow_type) = kGlowAlways;
	schema::Field<int>(glow, f.glow_range) = 5000;
	schema::Field<int>(glow, f.glow_range_min) = 100;
	if (!notify) return;
	// The glow property is a struct inside the entity: the change path is its offset, then the field's.
	for (std::ptrdiff_t field : {f.glow_color, f.glow_team, f.glow_type, f.glow_range, f.glow_range_min})
	{
		NetworkStateChangedData data({static_cast<uint32>(f.glow), static_cast<uint32>(field)});
		prop->NetworkStateChanged(data);
	}
}

CEntityInstance *SpawnModelCopy(const char *model, bool glowing)
{
	CEntityInstance *prop = create_entity("prop_dynamic", -1);
	if (!prop) return nullptr;
	prop->m_pEntity->m_flags = static_cast<EntityFlags_t>(prop->m_pEntity->m_flags & ~4u); // as the ESP plugins do
	auto *color = &schema::Field<std::uint8_t>(prop, f.color); // r, g, b, a
	if (glowing)
	{
		color[0] = color[1] = color[2] = 0;
		color[3] = 1; // all but invisible; only the glow shows
		WriteGlow(prop, false);
	}
	else
		schema::Field<std::uint8_t>(prop, f.render_mode) = kRenderNone;
	auto *values = new CEntityKeyValues(); // the game frees it
	values->SetString("model", model);
	values->SetString("spawnflags", "256");
	values->SetString("solid", "0"); // nobody may bump into it
	dispatch_spawn(prop, values);
	if (glowing) WriteGlow(prop, true); // spawning may reset the glow, so set it again, as the ESP plugins do
	return prop;
}

void Follow(CEntityInstance *follower, CEntityInstance *target)
{
	variant_t value("!activator");
	accept_input(follower, "FollowEntity", target, follower, &value, 0, nullptr);
}

void StartGlow(Rusher &rusher, CEntityInstance *pawn)
{
	void *body = schema::Field<void *>(pawn, f.body);
	void *node = body ? schema::Field<void *>(body, f.scene_node) : nullptr;
	const char *model = node ? schema::Field<CUtlSymbolLarge>(node, f.model_state + f.model_name).String() : nullptr;
	if (!model || !*model) return;
	CEntityInstance *relay = SpawnModelCopy(model, false);
	CEntityInstance *glow = relay ? SpawnModelCopy(model, true) : nullptr;
	if (!glow)
	{
		if (relay) remove_entity(relay);
		return;
	}
	Follow(relay, pawn);
	Follow(glow, relay);
	rusher.relay = relay->GetRefEHandle();
	rusher.glow = glow->GetRefEHandle();
}

void StopGlow(Rusher &rusher)
{
	CGameEntitySystem *entities = GameEntitySystem();
	for (CEntityHandle *handle : {&rusher.glow, &rusher.relay})
	{
		if (CEntityInstance *prop = entities && handle->IsValid() ? entities->GetEntityInstance(*handle) : nullptr)
			remove_entity(prop);
		*handle = CEntityHandle();
	}
}

// [ANTI-RUSH] in red (CS2 ignores a color code at the very start, hence the space), the name in its team's color, the rest in white.
void Announce(CEntityInstance *controller, std::uint8_t team)
{
	if (!messages || !events) return;
	std::string name = &schema::Field<char>(controller, f.name);
	for (char &c : name)
		if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' '; // color codes in names would break the line
	const std::string text = std::string(" \x07[ANTI-RUSH]\x01 Player ") + (team == 3 ? "\x0B" : "\x09") /* CT light blue, T yellow */ + name
		+ "\x01 tried to rush in a blocked zone.";
	INetworkMessageInternal *type = messages->FindNetworkMessagePartial("TextMsg");
	CNetMessage *data = type ? type->AllocateMessage() : nullptr;
	auto *message = data ? data->ToPB<CUserMessageTextMsg>() : nullptr;
	if (message)
	{
		message->set_dest(kHudPrintTalk);
		for (const char *param : {text.c_str(), "", "", "", ""}) message->add_param(param);
		events->PostEventAbstract(0, false, -1, nullptr, type, data, 0, BUF_RELIABLE); // -1, null: every client
	}
	if (data) messages->DeallocateNetMessageAbstract(type, data);
	META_CONPRINTF("[NO_HIT_EFFECTS] Anti-rush: %s tried to rush.\n", name.c_str());
}

} // namespace

bool Init(CSchemaSystemTypeScope *scope, const void *server_module_address, INetworkServerService *network_service,
	const std::string &path, std::string &error)
{
	network = network_service;
	messages = static_cast<INetworkMessages *>(g_SMAPI->GetEngineFactory()(NETWORKMESSAGES_INTERFACE_VERSION, nullptr));
	events = static_cast<IGameEventSystem *>(g_SMAPI->GetEngineFactory()(GAMEEVENTSYSTEM_INTERFACE_VERSION, nullptr));
	if (!messages || !events) META_CONPRINTF("[NO_HIT_EFFECTS] Anti-rush cannot send chat messages on this CS2 version.\n");
	const auto segments = ModuleSegments(server_module_address);
	create_entity = reinterpret_cast<decltype(create_entity)>(FindUnique(segments, kCreateEntity));
	dispatch_spawn = reinterpret_cast<decltype(dispatch_spawn)>(FindUnique(segments, kDispatchSpawn));
	remove_entity = reinterpret_cast<decltype(remove_entity)>(FindUnique(segments, kRemoveEntity));
	accept_input = reinterpret_cast<decltype(accept_input)>(FindUnique(segments, kAcceptInput));
	if (!accept_input) META_CONPRINTF("[NO_HIT_EFFECTS] Anti-rush glow is off: a CS2 update moved AcceptInput.\n");
	if (!create_entity || !dispatch_spawn || !remove_entity)
	{
		error = std::string("a CS2 update moved a function it needs (") + (!create_entity ? "CreateEntityByName " : "")
			+ (!dispatch_spawn ? "DispatchSpawn " : "") + (!remove_entity ? "UTIL_Remove" : "") + ")";
		return false;
	}
	const struct
	{
		std::ptrdiff_t *offset;
		const char *class_name;
		const char *field;
	} fields[] = {
		{&f.team, "CBaseEntity", "m_iTeamNum"},
		{&f.pawn_alive, "CCSPlayerController", "m_bPawnIsAlive"},
		{&f.pawn, "CCSPlayerController", "m_hPlayerPawn"},
		{&f.body, "CBaseEntity", "m_CBodyComponent"},
		{&f.scene_node, "CBodyComponent", "m_pSceneNode"},
		{&f.origin, "CGameSceneNode", "m_vecAbsOrigin"},
		{&f.velocity, "CBaseEntity", "m_vecAbsVelocity"},
		{&f.rules, "CCSGameRulesProxy", "m_pGameRules"},
		{&f.round_start, "CCSGameRules", "m_fRoundStartTime"},
		{&f.warmup, "CCSGameRules", "m_bWarmupPeriod"},
		{&f.end_pos, "CBeam", "m_vecEndPos"},
		{&f.width, "CBeam", "m_fWidth"},
		{&f.end_width, "CBeam", "m_fEndWidth"},
		{&f.color, "CBaseModelEntity", "m_clrRender"},
		{&f.render_mode, "CBaseModelEntity", "m_nRenderMode"},
		{&f.name, "CBasePlayerController", "m_iszPlayerName"},
		{&f.glow, "CBaseModelEntity", "m_Glow"},
		{&f.glow_color, "CGlowProperty", "m_glowColorOverride"},
		{&f.glow_team, "CGlowProperty", "m_iGlowTeam"},
		{&f.glow_type, "CGlowProperty", "m_iGlowType"},
		{&f.glow_start, "CGlowProperty", "m_flGlowStartTime"},
		{&f.glow_time, "CGlowProperty", "m_flGlowTime"},
		{&f.glow_range, "CGlowProperty", "m_nGlowRange"},
		{&f.glow_range_min, "CGlowProperty", "m_nGlowRangeMin"},
		{&f.model_state, "CSkeletonInstance", "m_modelState"},
		{&f.model_name, "CModelState", "m_ModelName"},
	};
	for (const auto &field : fields)
	{
		*field.offset = schema::FieldOffset(scope, field.class_name, field.field);
		if (*field.offset >= 0) continue;
		error = std::string("CS2's schema no longer has ") + field.class_name + "::" + field.field;
		return false;
	}
	config_path = path;
	LoadConfig();
	return true;
}

void Frame()
{
	if (!create_entity) return;
	// INetworkGameServer is CNetworkGameServerBase's first base, so the pointer is the same.
	auto *server = network ? reinterpret_cast<INetworkGameServer *>(network->GetIGameServer()) : nullptr;
	CGlobalVars *globals = server ? server->GetGlobals() : nullptr;
	CGameEntitySystem *entities = GameEntitySystem();
	if (!globals || !entities) return;

	const char *current_map = globals->mapname.ToCStr();
	if (!current_map) current_map = "";
	if (map != current_map)
	{
		map = current_map;
		beams.clear(); // the map change removed them
		rules_proxy = CEntityHandle();
		LoadConfig();
		const auto found = config.boxes.find(map);
		boxes = found == config.boxes.end() ? std::vector<anti_rush::Box> {} : found->second;
		last_touch.assign(boxes.size(), -1000.0f);
		META_CONPRINTF("[NO_HIT_EFFECTS] Map \"%s\": %d anti-rush barrier(s).\n", map.c_str(), static_cast<int>(boxes.size()));
	}
	if (boxes.empty()) return;

	void *rules = GameRules();
	const float dt = std::min(std::max(globals->curtime - last_frame_time, 0.0f), 0.1f);
	last_frame_time = globals->curtime;
	const float round_start = rules ? schema::Field<float>(rules, f.round_start) : 0.0f;
	if (round_start != last_round_start) // a new round: everyone may be caught again
	{
		last_round_start = round_start;
		for (Rusher &rusher : rushers) rusher.caught = false, rusher.pushing = 0.0f;
	}
	for (int slot = 0; slot < 64; ++slot) // glows end on time or with the rusher's death, locked or not
	{
		Rusher &rusher = rushers[slot];
		if (!rusher.glow.IsValid()) continue;
		CEntityInstance *controller = entities->GetPlayerController(CPlayerSlot(slot));
		const bool alive = controller && schema::Field<bool>(controller, f.pawn_alive);
		if (!alive || globals->curtime >= rusher.glow_until) StopGlow(rusher);
	}
	int alive[4] = {};
	for (int slot = 0; slot < 64; ++slot)
	{
		CEntityInstance *controller = entities->GetPlayerController(CPlayerSlot(slot));
		if (!controller || std::strcmp(controller->GetClassname(), "cs_player_controller") != 0) continue;
		const std::uint8_t team = schema::Field<std::uint8_t>(controller, f.team);
		if (team < 4 && schema::Field<bool>(controller, f.pawn_alive)) ++alive[team];
	}
	const bool locked = rules && !schema::Field<bool>(rules, f.warmup)
		&& anti_rush::Locked(globals->curtime - schema::Field<float>(rules, f.round_start), alive[2], alive[3],
			config.unlock_seconds);
	static bool was_locked = false;
	if (locked != was_locked)
	{
		was_locked = locked;
		META_CONPRINTF("[NO_HIT_EFFECTS] Anti-rush %s (T alive %d, CT alive %d, round time %.1f s).\n",
			locked ? "locked" : "unlocked", alive[2], alive[3],
			rules ? globals->curtime - schema::Field<float>(rules, f.round_start) : -1.0f);
	}

	if (!locked)
	{
		if (!beams.empty()) RemoveBeams();
		return;
	}
	if (!BeamsAlive()) // first lock, or a round restart removed them
	{
		RemoveBeams();
		SpawnBeams();
	}
	for (int slot = 0; slot < 64; ++slot)
	{
		CEntityInstance *controller = entities->GetPlayerController(CPlayerSlot(slot));
		if (!controller || std::strcmp(controller->GetClassname(), "cs_player_controller") != 0
			|| !schema::Field<bool>(controller, f.pawn_alive))
			continue;
		CEntityInstance *pawn = entities->GetEntityInstance(schema::Field<CEntityHandle>(controller, f.pawn));
		void *body = pawn ? schema::Field<void *>(pawn, f.body) : nullptr;
		void *node = body ? schema::Field<void *>(body, f.scene_node) : nullptr;
		if (!node) continue;
		const float *origin = &schema::Field<float>(node, f.origin);
		const anti_rush::Vec position {origin[0], origin[1], origin[2]};
		Rusher &rusher = rushers[slot];
		bool pushing = false;
		for (std::size_t index = 0; index < boxes.size(); ++index)
		{
			anti_rush::Box box = boxes[index];
			for (int axis = 0; axis < 2; ++axis)
			{
				box.min[axis] -= kPlayerHalfWidth;
				box.max[axis] += kPlayerHalfWidth;
			}
			if (!anti_rush::Inside(box, position)) continue;
			last_touch[index] = globals->curtime;
			float *velocity = &schema::Field<float>(pawn, f.velocity);
			pushing = anti_rush::MovingIn(box, position, {velocity[0], velocity[1], velocity[2]});
			const anti_rush::Vec pushed =
				anti_rush::Block(box, position, {velocity[0], velocity[1], velocity[2]}, kEjectRate);
			for (int axis = 0; axis < 3; ++axis) velocity[axis] = pushed[axis];
			break;
		}
		rusher.pushing = pushing ? rusher.pushing + dt : 0.0f;
		if (rusher.caught || rusher.pushing < kRushSeconds) continue;
		rusher.caught = true;
		Announce(controller, schema::Field<std::uint8_t>(controller, f.team));
		if (accept_input) StartGlow(rusher, pawn);
		rusher.glow_until = globals->curtime + kGlowSeconds;
	}
	for (std::size_t index = 0; index < boxes.size() && index < lit.size(); ++index)
	{
		const bool touched = globals->curtime - last_touch[index] < kTouchHold;
		if (touched == lit[index]) continue;
		lit[index] = touched;
		SetBoxTouched(index, touched);
	}
}

void Shutdown()
{
	if (create_entity) RemoveBeams();
	if (remove_entity)
		for (Rusher &rusher : rushers) StopGlow(rusher);
	rushers = {};
	create_entity = nullptr;
}

} // namespace barrier
