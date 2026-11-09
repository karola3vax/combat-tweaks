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
constexpr float kPushSpeed = 250.0f;
constexpr float kBeamWidth = 1.0f;
constexpr std::uint8_t kBeamAlpha = 96;          // faint, so the lines don't distract
constexpr std::uint8_t kRenderTransAlpha = 1;

// Checked against build 2000942: each matches exactly once.
#ifdef _WIN32
constexpr const char *kCreateEntity = "48 83 EC ? C6 44 24 ? 00 4C 8B C1";
constexpr const char *kDispatchSpawn = "48 89 5C 24 ? 57 48 83 EC ? 48 8B DA 48 8B F9 48 85 C9 0F 84 ? ? ? ? 48 85 D2";
constexpr const char *kRemoveEntity = "48 85 C9 74 ? 48 8B D1 48 8B 0D";
#else
constexpr const char *kCreateEntity = "48 8D 05 ? ? ? ? 55 48 89 FA";
constexpr const char *kDispatchSpawn = "48 85 FF 74 ? 55 48 89 E5 41 55 41 54 49 89 FC";
constexpr const char *kRemoveEntity = "48 89 FE 48 85 FF 74 ? 48 8D 05 ? ? ? ? 48";
#endif

struct Segment
{
	const std::uint8_t *start;
	std::size_t size;
};

CEntityInstance *(*create_entity)(const char *, int) = nullptr;
void (*dispatch_spawn)(CEntityInstance *, CEntityKeyValues *) = nullptr;
void (*remove_entity)(CEntityInstance *) = nullptr;

struct Offsets
{
	std::ptrdiff_t team, pawn_alive, pawn, body, scene_node, origin, velocity, rules, round_start, warmup, end_pos, width,
		end_width, color, render_mode;
} f;

anti_rush::Config config;
std::string config_path;
INetworkServerService *network = nullptr;
std::string map;
std::vector<anti_rush::Box> boxes; // for the current map
std::vector<CEntityHandle> beams;
CEntityHandle rules_proxy;
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
	for (const CEntityHandle &handle : beams)
		if (CEntityInstance *beam = entities ? entities->GetEntityInstance(handle) : nullptr) remove_entity(beam);
	beams.clear();
}

void SpawnBeams()
{
	for (const anti_rush::Box &box : boxes)
		for (auto edge : anti_rush::Edges(box))
		{
			// Only the floor outline: the full box was too distracting.
			if (edge[0][2] != box.min[2] || edge[1][2] != box.min[2]) continue;
			edge[0][2] = edge[1][2] = box.min[2] + 1.0f; // just above the floor, so it doesn't flicker into it
			CEntityInstance *beam = create_entity("beam", -1);
			if (!beam) continue;
			auto *end = &schema::Field<float>(beam, f.end_pos);
			for (int axis = 0; axis < 3; ++axis) end[axis] = edge[1][axis];
			schema::Field<float>(beam, f.width) = kBeamWidth;
			schema::Field<float>(beam, f.end_width) = kBeamWidth;
			auto *color = &schema::Field<std::uint8_t>(beam, f.color); // r, g, b, a
			color[0] = color[1] = color[2] = 120; // grey: glowing white pulsed with auto-exposure when shooting
			color[3] = kBeamAlpha;
			schema::Field<std::uint8_t>(beam, f.render_mode) = kRenderTransAlpha;
			auto *values = new CEntityKeyValues(); // the game frees it
			values->SetVector("origin", Vector(edge[0][0], edge[0][1], edge[0][2]));
			dispatch_spawn(beam, values);
			beams.push_back(beam->GetRefEHandle());
		}
}

bool BeamsAlive()
{
	CGameEntitySystem *entities = GameEntitySystem();
	for (const CEntityHandle &handle : beams)
		if (!entities || !entities->GetEntityInstance(handle)) return false;
	return !beams.empty();
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

} // namespace

bool Init(CSchemaSystemTypeScope *scope, const void *server_module_address, INetworkServerService *network_service,
	const std::string &path, std::string &error)
{
	network = network_service;
	const auto segments = ModuleSegments(server_module_address);
	create_entity = reinterpret_cast<decltype(create_entity)>(FindUnique(segments, kCreateEntity));
	dispatch_spawn = reinterpret_cast<decltype(dispatch_spawn)>(FindUnique(segments, kDispatchSpawn));
	remove_entity = reinterpret_cast<decltype(remove_entity)>(FindUnique(segments, kRemoveEntity));
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
		META_CONPRINTF("[NO_HIT_EFFECTS] Map \"%s\": %d anti-rush barrier(s).\n", map.c_str(), static_cast<int>(boxes.size()));
	}
	if (boxes.empty()) return;

	void *rules = GameRules();
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
		for (anti_rush::Box box : boxes)
		{
			for (int axis = 0; axis < 2; ++axis)
			{
				box.min[axis] -= kPlayerHalfWidth;
				box.max[axis] += kPlayerHalfWidth;
			}
			if (!anti_rush::Inside(box, position)) continue;
			float *velocity = &schema::Field<float>(pawn, f.velocity);
			const anti_rush::Vec pushed =
				anti_rush::PushOut(box, position, {velocity[0], velocity[1], velocity[2]}, kPushSpeed);
			for (int axis = 0; axis < 3; ++axis) velocity[axis] = pushed[axis];
			break;
		}
	}
}

void Shutdown()
{
	if (create_entity) RemoveBeams();
	create_entity = nullptr;
}

} // namespace barrier
