#include "barrier.h"

#include "anti_rush.h"
#include "center.h"
#include "chat.h"
#include "module.h"
#include "schema.h"

#include <ISmmPlugin.h>
#include <entity2/entityinstance.h>
#include <entity2/entitykeyvalues.h>
#include <engine2/gameshareddefs.h>
#include <entity2/entitysystem.h>
#include <gameentitysystem.h>
#include <engine2/inetworkgameserver.h>
#include <engine2/inetworkserverservice.h>
#include <tier0/convar.h>
#include <tier0/icvar.h>
#include <variant.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

extern ISmmAPI *g_SMAPI;

namespace barrier
{
namespace
{

constexpr float kPlayerHalfWidth = 16.0f; // the player's box sticks out this far from its origin
constexpr float kEjectRate = 10.0f;       // per unit of overlap, so a player touching the edge barely moves
constexpr const char *kPanelModel = "models/retakes/retakes_barrier.vmdl"; // the game's retake barrier
constexpr float kTouchHold = 0.5f;        // seconds a box stays lit after the last touch
constexpr std::uint8_t kRenderTransAlpha = 1;
constexpr std::uint8_t kRenderNone = 2;
constexpr int kGlowAlways = 3;            // m_iGlowType: through walls
constexpr const char *kWarningSound = "UIPanorama.buymenu_failure"; // the buy menu's "denied" buzz
constexpr const char *kHitSound = "Player.DamageBody.Onlooker"; // heard where the player stands
constexpr float kSoundLife = 2.0f; // seconds before a sound entity is removed
constexpr float kHitInterval = 0.5f;     // rush damage comes as one hit per this many seconds of pushing
constexpr float kZoneFloor = 16.0f;       // editor zones reach this far below the lower corner (stairs, slopes)
constexpr float kZoneHeight = 200.0f;     // ...and this far above the higher one, so jumping can't clear them

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

CEntityInstance *(*create_entity)(const char *, int) = nullptr;
void (*dispatch_spawn)(CEntityInstance *, CEntityKeyValues *) = nullptr;
void (*remove_entity)(CEntityInstance *) = nullptr;
void (*accept_input)(CEntityInstance *, const char *, CEntityInstance *, CEntityInstance *, variant_t *, int, void *) = nullptr;

struct Offsets
{
	std::ptrdiff_t team, pawn_alive, pawn, body, scene_node, origin, velocity, rules, round_start, warmup, color, render_mode, name, glow, glow_color, glow_team, glow_type, glow_range, glow_range_min, model_state,
		model_name, game_restart, health;
} f;

config::Settings::AntiRush settings;
std::string zones_path;
anti_rush::Zones all_zones;
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
std::vector<bool> lit;         // per box: drawn in the touched color
CEntityHandle rules_proxy;
int rules_search_frames = 0;

struct Rusher
{
	float pushing = 0.0f;      // seconds of walking into the barrier without a break
	bool caught = false;       // announced this round
	CEntityHandle relay, glow; // the model copies that show the glow
	float glow_until = 0.0f;
	float center_until = 0.0f; // the private note shows until then
	float damage_owed = 0.0f;  // seconds of pushing towards the next damage hit
};
std::array<Rusher, 64> rushers;
std::vector<std::pair<CEntityHandle, float>> sounds; // point_soundevents and when to remove them
bool set_game_restart = false; // this plugin turned m_bGameRestart on, for the note

// Editor: two corners per admin, and the preview box once both are set.
struct Draft
{
	int corners = 0;
	anti_rush::Vec first {}, second {};
	std::vector<CEntityHandle> preview;
};
std::array<Draft, 64> drafts;

float last_round_start = -1.0f;
float last_frame_time = 0.0f;
bool locked = false;
bool opened_this_round = false; // once open, the barrier stays open until the next round

std::string ReadFile(const std::string &path)
{
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

CGlobalVars *Globals()
{
	// INetworkGameServer is CNetworkGameServerBase's first base, so the pointer is the same.
	auto *server = network ? reinterpret_cast<INetworkGameServer *>(network->GetIGameServer()) : nullptr;
	return server ? server->GetGlobals() : nullptr;
}

void LoadZones()
{
	std::vector<std::string> errors;
	const std::string text = ReadFile(zones_path);
	all_zones = text.empty() ? anti_rush::Zones {} : anti_rush::ParseZones(text, errors);
	if (text.empty()) META_CONPRINTF("[COMBAT_TWEAKS] No %s yet; add zones in game with \"!ct zone\".\n", zones_path.c_str());
	for (const std::string &line : errors) META_CONPRINTF("[COMBAT_TWEAKS] zones.jsonc: %s\n", line.c_str());
}

bool SaveZones()
{
	const std::string temporary = zones_path + ".tmp";
	{
		std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
		file << anti_rush::FormatZones(all_zones);
		if (!file.flush()) return false;
	}
	std::remove(zones_path.c_str());
	return std::rename(temporary.c_str(), zones_path.c_str()) == 0; // never leaves a half-written file
}

CEntityInstance *Entity(const CEntityHandle &handle)
{
	CGameEntitySystem *entities = GameEntitySystem();
	return entities && handle.IsValid() ? entities->GetEntityInstance(handle) : nullptr;
}

void Remove(CEntityHandle &handle)
{
	if (CEntityInstance *entity = Entity(handle)) remove_entity(entity);
	handle = CEntityHandle();
}

void Paint(CEntityInstance *beam, const config::Rgba &rgba, bool notify)
{
	auto *color = &schema::Field<std::uint8_t>(beam, f.color); // r, g, b, a
	for (int i = 0; i < 4; ++i) color[i] = rgba[i];
	if (!notify) return;
	NetworkStateChangedData data(static_cast<uint32>(f.color));
	beam->NetworkStateChanged(data);
}

// The 4 sides of a box as the game's retake barrier panels, spawned the way its retake_barrier_spawn
// command does: origin at the bottom corner, turned towards the next corner, then width and height.
std::vector<CEntityHandle> SpawnBox(const anti_rush::Box &box, const config::Rgba &rgba)
{
	std::vector<CEntityHandle> handles;
	if (rgba[3] == 0) return handles; // alpha 0: invisible barrier
	const float corners[5][2] = {{box.min[0], box.min[1]}, {box.max[0], box.min[1]}, {box.max[0], box.max[1]},
		{box.min[0], box.max[1]}, {box.min[0], box.min[1]}};
	for (int side = 0; side < 4; ++side)
	{
		const float dx = corners[side + 1][0] - corners[side][0];
		const float dy = corners[side + 1][1] - corners[side][1];
		CEntityInstance *panel = create_entity("func_retakebarrier", -1);
		if (!panel) continue;
		Paint(panel, rgba, false);
		schema::Field<std::uint8_t>(panel, f.render_mode) = kRenderTransAlpha;
		auto *values = new CEntityKeyValues(); // the game frees it
		values->SetVector("origin", Vector(corners[side][0], corners[side][1], box.min[2]));
		values->SetQAngle("angles", QAngle(0.0f, std::atan2(dy, dx) * 180.0f / 3.14159265f, 0.0f));
		values->SetString("model", kPanelModel);
		values->SetString("solid", "0");
		values->SetFloat("barrier_width", std::sqrt(dx * dx + dy * dy));
		values->SetFloat("barrier_height", box.max[2] - box.min[2]);
		values->SetFloat("barrier_duration", 0.0f); // as the game's retake maps use it
		dispatch_spawn(panel, values);
		// The panel is solid whatever "solid" says, and players stopped by it never count as pushing.
		if (accept_input)
		{
			variant_t none;
			accept_input(panel, "DisableCollision", nullptr, nullptr, &none, 0, nullptr);
		}
		handles.push_back(panel->GetRefEHandle());
	}
	return handles;
}

void RemoveBeams()
{
	for (Beam &beam : beams) Remove(beam.handle);
	beams.clear();
}

void SpawnBeams()
{
	lit.assign(boxes.size(), false);
	for (std::size_t index = 0; index < boxes.size(); ++index)
		for (const CEntityHandle &handle : SpawnBox(boxes[index], settings.line_color)) beams.push_back({handle, index});
}

bool BeamsAlive()
{
	for (const Beam &beam : beams)
		if (!Entity(beam.handle)) return false;
	return true;
}

void SetBoxTouched(std::size_t box, bool touched)
{
	for (const Beam &beam : beams)
		if (CEntityInstance *entity = beam.box == box ? Entity(beam.handle) : nullptr)
			Paint(entity, touched ? settings.touched_color : settings.line_color, true);
}

void ClearPreview(Draft &draft)
{
	for (CEntityHandle &handle : draft.preview) Remove(handle);
	draft.preview.clear();
}

anti_rush::Box DraftBox(const Draft &draft)
{
	anti_rush::Box box = anti_rush::MakeBox(draft.first, draft.second);
	box.min[2] -= kZoneFloor;
	box.max[2] += kZoneHeight;
	return box;
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

CEntityInstance *Controller(int slot)
{
	CGameEntitySystem *entities = GameEntitySystem();
	CEntityInstance *controller = entities ? entities->GetPlayerController(CPlayerSlot(slot)) : nullptr;
	return controller && std::strcmp(controller->GetClassname(), "cs_player_controller") == 0 ? controller : nullptr;
}

// The living pawn of a slot and its feet position, or nullptr.
CEntityInstance *LivingPawn(int slot, anti_rush::Vec &position)
{
	CEntityInstance *controller = Controller(slot);
	if (!controller || !schema::Field<bool>(controller, f.pawn_alive)) return nullptr;
	CEntityInstance *pawn = Entity(schema::Field<CEntityHandle>(controller, f.pawn));
	void *body = pawn ? schema::Field<void *>(pawn, f.body) : nullptr;
	void *node = body ? schema::Field<void *>(body, f.scene_node) : nullptr;
	if (!node) return nullptr;
	const float *origin = &schema::Field<float>(node, f.origin);
	position = {origin[0], origin[1], origin[2]};
	return pawn;
}

// A prop_dynamic with the rusher's model. The relay copies the player's pose; the glow copy
// follows the relay and carries the glow, because CS2 draws no glow on player pawns (the way
// public ESP plugins do it).
void WriteGlow(CEntityInstance *prop, bool notify, const config::Rgba &color)
{
	void *glow = &schema::Field<std::byte>(prop, f.glow);
	auto *glow_color = &schema::Field<std::uint8_t>(glow, f.glow_color);
	for (int i = 0; i < 4; ++i) glow_color[i] = color[i];
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

CEntityInstance *SpawnModelCopy(const char *model, const config::Rgba *glow_color)
{
	const bool glowing = glow_color != nullptr;
	CEntityInstance *prop = create_entity("prop_dynamic", -1);
	if (!prop) return nullptr;
	prop->m_pEntity->m_flags = static_cast<EntityFlags_t>(prop->m_pEntity->m_flags & ~4u); // as the ESP plugins do
	auto *color = &schema::Field<std::uint8_t>(prop, f.color); // r, g, b, a
	if (glowing)
	{
		color[0] = color[1] = color[2] = 0;
		color[3] = 1; // all but invisible; only the glow shows
		WriteGlow(prop, false, *glow_color);
	}
	else
		schema::Field<std::uint8_t>(prop, f.render_mode) = kRenderNone;
	auto *values = new CEntityKeyValues(); // the game frees it
	values->SetString("model", model);
	values->SetString("spawnflags", "256");
	values->SetString("solid", "0"); // nobody may bump into it
	dispatch_spawn(prop, values);
	if (glowing) WriteGlow(prop, true, *glow_color); // spawning resets the glow, so set it again, as the ESP plugins do
	return prop;
}

void Follow(CEntityInstance *follower, CEntityInstance *target)
{
	variant_t value("!activator");
	accept_input(follower, "FollowEntity", target, follower, &value, 0, nullptr);
}

void StartGlow(Rusher &rusher, CEntityInstance *pawn, int team)
{
	void *body = schema::Field<void *>(pawn, f.body);
	void *node = body ? schema::Field<void *>(body, f.scene_node) : nullptr;
	const char *model = node ? schema::Field<CUtlSymbolLarge>(node, f.model_state + f.model_name).String() : nullptr;
	if (!model || !*model) return;
	CEntityInstance *relay = SpawnModelCopy(model, nullptr);
	CEntityInstance *glow = relay ? SpawnModelCopy(model, team == 3 ? &settings.glow_color_ct : &settings.glow_color_t) : nullptr;
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
	Remove(rusher.glow);
	Remove(rusher.relay);
}

void Announce(CEntityInstance *controller)
{
	std::string name = &schema::Field<char>(controller, f.name);
	chat::SendAll("rush_attempt", {{"name", name}}, schema::Field<std::uint8_t>(controller, f.team));
	META_CONPRINTF("[COMBAT_TWEAKS] Anti-rush: %s tried to rush.\n", name.c_str());
}

// The rusher's private note, sent every tick while it shows. Live rounds flash and drop center
// panels unless m_bGameRestart is set (ModSharp's FixHtmlFlashing), so it is set while a note
// shows, except during warmup or when the game itself has a restart pending.
void ShowNotes(void *rules, float now, float round_time)
{
	bool showing = false;
	for (int slot = 0; slot < 64; ++slot)
	{
		Rusher &rusher = rushers[slot];
		if (rusher.center_until <= now) continue;
		if (!locked || !Controller(slot))
		{
			rusher.center_until = 0.0f;
			continue;
		}
		showing = true;
		const int left = static_cast<int>(settings.unlock_seconds - round_time + 0.999f);
		std::string html = settings.unlock_seconds > 0
			? chat::Format(slot, "rush_center", {{"seconds", std::to_string(std::max(left, 0))}})
			: chat::Format(slot, "rush_center_wait");
		if (rusher.glow.IsValid()) html += chat::Format(slot, "rush_center_seen"); // while everyone sees them
		center::Send(slot, html);
	}
	if (!rules) return;
	bool &restart = schema::Field<bool>(rules, f.game_restart);
	if (showing && !restart && !schema::Field<bool>(rules, f.warmup))
	{
		restart = true;
		set_game_restart = true;
	}
	else if (!showing && set_game_restart)
	{
		restart = false;
		set_game_restart = false;
	}
}

// Runs the game's own "kill" console command as if the player had typed it: a suicide, scored as one.
// IVEngineServer2::ClientCommand only asks the player's game to run it, which it did not do.
bool Kill(int slot)
{
	if (!g_pCVar) return false;
	ConCommandRef kill = g_pCVar->FindConCommand("kill");
	if (!kill.IsValidRef()) return false;
	CCommand args;
	args.Tokenize("kill");
	g_pCVar->DispatchConCommand(kill, CCommandContext(CT_FIRST_SPLITSCREEN_CLIENT, CPlayerSlot(slot)), args);
	return true;
}

// Health off a player who keeps pushing into the barrier. A lethal amount kills them (a suicide),
// unless the config keeps them at 1 health.
// The game's own hit sound at a spot, through a short-lived point_soundevent.
void PlayHitSound(const anti_rush::Vec &position, float now)
{
	if (!accept_input) return;
	CEntityInstance *sound = create_entity("point_soundevent", -1);
	if (!sound) return;
	auto *values = new CEntityKeyValues(); // the game frees it
	values->SetVector("origin", Vector(position[0], position[1], position[2] + 40.0f)); // chest height
	values->SetString("soundName", kHitSound);
	dispatch_spawn(sound, values);
	variant_t none;
	accept_input(sound, "StartSound", nullptr, nullptr, &none, 0, nullptr);
	sounds.emplace_back(sound->GetRefEHandle(), now + kSoundLife);
}

void Hurt(int slot, CEntityInstance *pawn, Rusher &rusher, float dt, const anti_rush::Vec &position, float now)
{
	if (settings.damage_per_second <= 0) return;
	// Felt hits instead of 1 health per tick.
	rusher.damage_owed += dt;
	if (rusher.damage_owed < kHitInterval) return;
	rusher.damage_owed -= kHitInterval;
	const int damage = std::max(static_cast<int>(settings.damage_per_second * kHitInterval + 0.5f), 1);
	if (settings.damage_sound) PlayHitSound(position, now);
	int &health = schema::Field<int>(pawn, f.health);
	if (health - damage <= 0 && settings.damage_can_kill && Kill(slot)) return;
	health = std::max(health - damage, 1);
	NetworkStateChangedData data(static_cast<uint32>(f.health));
	pawn->NetworkStateChanged(data);
}

void SetLocked(bool now_locked, int alive_t, int alive_ct, float round_time)
{
	if (now_locked == locked) return;
	locked = now_locked;
	META_CONPRINTF("[COMBAT_TWEAKS] Anti-rush %s (T alive %d, CT alive %d, round time %.1f s).\n",
		locked ? "locked" : "unlocked", alive_t, alive_ct, round_time);
	if (!settings.round_messages) return;
	if (locked)
		chat::SendAll("barrier_locked");
	else
		chat::SendAll("barrier_unlocked");
}

} // namespace

bool Init(CSchemaSystemTypeScope *scope, const void *server_module_address, INetworkServerService *network_service,
	const std::string &path, std::string &error)
{
	network = network_service;
	zones_path = path;
	const auto segments = module::Segments(server_module_address);
	create_entity = reinterpret_cast<decltype(create_entity)>(module::FindUnique(segments, kCreateEntity));
	dispatch_spawn = reinterpret_cast<decltype(dispatch_spawn)>(module::FindUnique(segments, kDispatchSpawn));
	remove_entity = reinterpret_cast<decltype(remove_entity)>(module::FindUnique(segments, kRemoveEntity));
	accept_input = reinterpret_cast<decltype(accept_input)>(module::FindUnique(segments, kAcceptInput));
	if (!accept_input) META_CONPRINTF("[COMBAT_TWEAKS] Anti-rush glow is off: a CS2 update moved AcceptInput.\n");
	if (!create_entity || !dispatch_spawn || !remove_entity)
	{
		error = std::string("a CS2 update moved a function it needs (") + (!create_entity ? "CreateEntityByName " : "")
			+ (!dispatch_spawn ? "DispatchSpawn " : "") + (!remove_entity ? "UTIL_Remove" : "") + ")";
		create_entity = nullptr;
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
		{&f.color, "CBaseModelEntity", "m_clrRender"},
		{&f.render_mode, "CBaseModelEntity", "m_nRenderMode"},
		{&f.name, "CBasePlayerController", "m_iszPlayerName"},
		{&f.glow, "CBaseModelEntity", "m_Glow"},
		{&f.glow_color, "CGlowProperty", "m_glowColorOverride"},
		{&f.glow_team, "CGlowProperty", "m_iGlowTeam"},
		{&f.glow_type, "CGlowProperty", "m_iGlowType"},
		{&f.glow_range, "CGlowProperty", "m_nGlowRange"},
		{&f.glow_range_min, "CGlowProperty", "m_nGlowRangeMin"},
		{&f.model_state, "CSkeletonInstance", "m_modelState"},
		{&f.model_name, "CModelState", "m_ModelName"},
		{&f.game_restart, "CCSGameRules", "m_bGameRestart"},
		{&f.health, "CBaseEntity", "m_iHealth"},
	};
	for (const auto &field : fields)
	{
		*field.offset = schema::FieldOffset(scope, field.class_name, field.field);
		if (*field.offset >= 0) continue;
		error = std::string("CS2's schema no longer has ") + field.class_name + "::" + field.field;
		create_entity = nullptr;
		return false;
	}
	LoadZones();
	return true;
}

void Configure(const config::Settings::AntiRush &anti_rush)
{
	settings = anti_rush;
	if (!create_entity) return;
	RemoveBeams(); // redrawn with the new colors on the next frame
	if (!settings.glow)
		for (Rusher &rusher : rushers) StopGlow(rusher);
}

void ReloadZones()
{
	if (!create_entity) return;
	LoadZones();
	map.clear(); // the next frame picks this map's zones up again
}

void Frame()
{
	if (!create_entity) return;
	CGlobalVars *globals = Globals();
	CGameEntitySystem *entities = GameEntitySystem();
	if (!globals || !entities) return;

	const char *current_map = globals->mapname.ToCStr();
	if (!current_map) current_map = "";
	if (map != current_map)
	{
		const bool new_map = map.empty() || map != current_map;
		map = current_map;
		if (new_map) beams.clear(); // a map change removed them; a reload removed them itself
		RemoveBeams();
		rules_proxy = CEntityHandle();
		const auto found = all_zones.find(map);
		boxes = found == all_zones.end() ? std::vector<anti_rush::Box> {} : found->second;
		last_touch.assign(boxes.size(), -1000.0f);
		META_CONPRINTF("[COMBAT_TWEAKS] Map \"%s\": %d anti-rush zone(s).\n", map.c_str(), static_cast<int>(boxes.size()));
	}

	void *rules = GameRules();
	const float dt = std::min(std::max(globals->curtime - last_frame_time, 0.0f), 0.1f);
	last_frame_time = globals->curtime;
	const float round_start = rules ? schema::Field<float>(rules, f.round_start) : 0.0f;
	if (round_start != last_round_start) // a new round: everyone may be caught again, and the barrier closes again
	{
		last_round_start = round_start;
		opened_this_round = false;
		for (Rusher &rusher : rushers) rusher.caught = false, rusher.pushing = 0.0f;
	}
	for (Rusher &rusher : rushers) // glows end on time or with the rusher's death, locked or not
	{
		const int slot = static_cast<int>(&rusher - rushers.data());
		if (!rusher.glow.IsValid()) continue;
		CEntityInstance *controller = Controller(slot);
		const bool alive = controller && schema::Field<bool>(controller, f.pawn_alive);
		if (!alive || globals->curtime >= rusher.glow_until) StopGlow(rusher);
	}
	for (auto it = sounds.begin(); it != sounds.end();) // finished hit sounds
	{
		if (globals->curtime < it->second && Entity(it->first)) { ++it; continue; }
		Remove(it->first);
		it = sounds.erase(it);
	}
	for (Draft &draft : drafts) // previews vanish with the map
		if (draft.corners == 2 && !draft.preview.empty() && !Entity(draft.preview.front())) draft.preview.clear();

	int alive[4] = {};
	for (int slot = 0; slot < 64; ++slot)
	{
		CEntityInstance *controller = Controller(slot);
		if (!controller) continue;
		const std::uint8_t team = schema::Field<std::uint8_t>(controller, f.team);
		if (team < 4 && schema::Field<bool>(controller, f.pawn_alive)) ++alive[team];
	}
	const float round_time = rules ? globals->curtime - round_start : -1.0f;
	bool now_locked = settings.enabled && !boxes.empty() && rules && !schema::Field<bool>(rules, f.warmup)
		&& !opened_this_round
		&& anti_rush::Locked(round_time, alive[2], alive[3], settings.unlock_seconds, settings.unlock_players_left);
	if (locked && !now_locked && rules && !schema::Field<bool>(rules, f.warmup)) opened_this_round = true;
	SetLocked(now_locked, alive[2], alive[3], round_time);

	if (!locked)
	{
		if (!beams.empty()) RemoveBeams();
		ShowNotes(rules, globals->curtime, round_time);
		return;
	}
	if (beams.empty() || !BeamsAlive()) // first lock, a reload, or a round restart removed them
	{
		RemoveBeams();
		SpawnBeams();
	}
	for (int slot = 0; slot < 64; ++slot)
	{
		anti_rush::Vec position;
		CEntityInstance *pawn = LivingPawn(slot, position);
		if (!pawn) continue;
		Rusher &rusher = rushers[slot];
		bool pushing = false;
		bool touching = false;
		for (std::size_t index = 0; index < boxes.size(); ++index)
		{
			anti_rush::Box box = boxes[index];
			for (int axis = 0; axis < 2; ++axis)
			{
				box.min[axis] -= kPlayerHalfWidth;
				box.max[axis] += kPlayerHalfWidth;
			}
			if (!anti_rush::Inside(box, position)) continue;
			touching = true;
			float *velocity = &schema::Field<float>(pawn, f.velocity);
			pushing = anti_rush::MovingIn(box, position, {velocity[0], velocity[1], velocity[2]});
			// Red only for a push, the same thing the chat callout names, so players see why it lit up.
			if (pushing) last_touch[index] = globals->curtime;
			const anti_rush::Vec blocked =
				anti_rush::Block(box, position, {velocity[0], velocity[1], velocity[2]}, kEjectRate);
			for (int axis = 0; axis < 3; ++axis) velocity[axis] = blocked[axis];
			break;
		}
		rusher.pushing = pushing ? rusher.pushing + dt : 0.0f;
		// Pushing flickers from tick to tick against the wall; only leaving the barrier starts the count over.
		if (pushing) Hurt(slot, pawn, rusher, dt, position, globals->curtime);
		else if (!touching) rusher.damage_owed = 0.0f;
		// A caught rusher who keeps pushing keeps the note up; it then lingers for center_seconds.
		if (rusher.caught && pushing && settings.center_message) rusher.center_until = globals->curtime + settings.center_seconds;
		if (!settings.callout || rusher.caught || rusher.pushing < settings.callout_seconds) continue;
		rusher.caught = true;
		Announce(Controller(slot));
		if (settings.warning_sound) center::PlaySound(slot, kWarningSound);
		if (settings.center_message) rusher.center_until = globals->curtime + settings.center_seconds;
		if (settings.glow && accept_input)
		{
			StopGlow(rusher);
			StartGlow(rusher, pawn, schema::Field<std::uint8_t>(Controller(slot), f.team));
			rusher.glow_until = globals->curtime + settings.glow_seconds;
		}
	}
	ShowNotes(rules, globals->curtime, round_time);
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
	if (!create_entity) return;
	RemoveBeams();
	for (Rusher &rusher : rushers) StopGlow(rusher);
	for (Draft &draft : drafts) ClearPreview(draft);
	for (auto &sound : sounds) Remove(sound.first);
	sounds.clear();
	rushers = {};
	drafts = {};
	locked = false;
	create_entity = nullptr;
}

void Status(std::vector<std::string> &lines)
{
	if (!create_entity)
	{
		lines.push_back("Anti-rush: off (see the load message above for why)");
		return;
	}
	char line[256];
	std::snprintf(line, sizeof(line), "Anti-rush: %s; map %s has %d zone(s); barrier %s", settings.enabled ? "on" : "off",
		map.c_str(), static_cast<int>(boxes.size()), locked ? "closed" : "open");
	lines.push_back(line);
	std::snprintf(line, sizeof(line), "  Opens after %d s or at %d player(s) left; callout %s after %.1f s; glow %s (%.0f s)",
		settings.unlock_seconds, settings.unlock_players_left, settings.callout ? "on" : "off", settings.callout_seconds,
		!settings.glow ? "off" : accept_input ? "on" : "unavailable (AcceptInput not found)", settings.glow_seconds);
	lines.push_back(line);
}

Corner ZoneCorner(int slot)
{
	anti_rush::Vec position;
	if (!create_entity || slot < 0 || slot >= 64 || !LivingPawn(slot, position)) return Corner::Dead;
	Draft &draft = drafts[slot];
	ClearPreview(draft);
	if (draft.corners != 1)
	{
		draft.first = position;
		draft.corners = 1;
		return Corner::First;
	}
	draft.second = position;
	draft.corners = 2;
	draft.preview = SpawnBox(DraftBox(draft), settings.touched_color.at(3) ? settings.touched_color : config::Rgba {255, 0, 0, 255});
	return Corner::Second;
}

int ZoneSave(int slot)
{
	if (!create_entity || slot < 0 || slot >= 64 || drafts[slot].corners != 2 || map.empty()) return 0;
	Draft &draft = drafts[slot];
	all_zones[map].push_back(DraftBox(draft));
	ClearPreview(draft);
	draft = {};
	if (!SaveZones()) META_CONPRINTF("[COMBAT_TWEAKS] Could not write %s.\n", zones_path.c_str());
	const int number = static_cast<int>(all_zones[map].size());
	map.clear(); // reload this map's zones on the next frame
	return number;
}

void ZoneCancel(int slot)
{
	if (slot < 0 || slot >= 64) return;
	ClearPreview(drafts[slot]);
	drafts[slot] = {};
}

const std::vector<anti_rush::Box> &Zones()
{
	return boxes;
}

bool ZoneRemove(int number)
{
	auto found = all_zones.find(map);
	if (!create_entity || found == all_zones.end() || number < 1 || number > static_cast<int>(found->second.size())) return false;
	found->second.erase(found->second.begin() + (number - 1));
	if (!SaveZones()) META_CONPRINTF("[COMBAT_TWEAKS] Could not write %s.\n", zones_path.c_str());
	map.clear();
	return true;
}

const std::string &Map()
{
	return map;
}

} // namespace barrier
