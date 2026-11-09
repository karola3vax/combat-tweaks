# Combat Tweaks

A Metamod:Source plugin for Counter-Strike 2 servers that makes fights cleaner and stops early rushes. It runs on its own: no CounterStrikeSharp or SwiftlyS2 needed.

## Features

- **No aimpunch when hit.** Getting shot doesn't kick your aim, with or without armor.
- **No slowdown when hit.** Getting shot doesn't slow you down. This happens on the server and the player's game together, so there's no stutter.
- **No ragdolls.** Dead bodies disappear at once.
- **Short decals.** Bullet holes, blood and scorch marks fade after about 3 seconds.
- **Anti-rush barriers.**
  - **Closing:** at the start of each round, a faint grey box blocks part of the map like a wall, and lights up red when someone walks into it.
  - **Opening:** it opens after a set time, or once a team is down to its last player.
  - **Rushers:** a player who keeps pushing into it is named in chat, glows in their team color for a few seconds (seen by everyone), and gets a private note in the middle of their screen.
- **In-game zone editor.** Admins draw barriers by standing at two corners. No coordinates to type.
- **9 languages.** Each player gets chat in their own game language: English, Turkish, Russian, Portuguese, Spanish, German, Chinese, Polish and French.
- **Settings and status.** Every feature can be switched off or tuned in a commented config file, and `ct status` shows what's running.
- **Windows and Linux.**

## Requirements

- A Counter-Strike 2 dedicated server
- Metamod:Source 2.0.0 build 1469 or newer

## Installation

1. Extract the release into `game/csgo/`.
2. Add your SteamID64 to `"admins"` in `game/csgo/addons/combat_tweaks/combat_tweaks.jsonc`.
3. Restart the server, then type `ct status` in the server console.

The release contains:

```text
addons/
|-- combat_tweaks/
|   |-- bin/combat_tweaks.dll
|   |-- bin/combat_tweaks.so
|   |-- combat_tweaks.jsonc      settings
|   |-- zones.jsonc              anti-rush barriers, by map
|   `-- translations/*.json      chat messages, one file per language
`-- metamod/combat_tweaks.vdf
```

**Updating:** replace only `bin/`, `translations/` and `metamod/combat_tweaks.vdf`. Keep your own `combat_tweaks.jsonc` and `zones.jsonc`.

## Commands

The `ct` command works three ways:
- **Server console:** `ct ...`, always allowed.
- **Player console:** `ct ...`, admins only.
- **Chat:** `!ct ...` (seen by everyone) or `/ct ...` (hidden), admins only.

| Command | What it does |
| --- | --- |
| `ct status` | Shows every feature, this map's barriers and any config problems |
| `ct zone` | Sets a barrier corner where you stand. Use it at one corner, then at the opposite corner, and the box is drawn |
| `ct zone save` | Saves the drawn box as a barrier for this map |
| `ct zone cancel` | Throws the drawn box away |
| `ct zone list` | Lists this map's barriers with their numbers |
| `ct zone remove <number>` | Deletes a barrier |
| `ct reload` | Reads the settings, barriers and messages again |

### Drawing a barrier

1. Walk to one corner of the area to block and type `!ct zone`.
2. Walk to the opposite corner and type `!ct zone` again. The box appears in red.
3. Type `!ct zone save`. The barrier works at once.

Barriers reach a little below the lower corner and 200 units above the higher one, so players can't jump over them. A barrier only stops players while it's closed, and it's never closed during warmup.

## Configuration

`addons/combat_tweaks/combat_tweaks.jsonc` is read at server start, on every map change and on `ct reload`. Any value it can't use is named in the console and replaced with the default.

| Setting | Default | Meaning |
| --- | --- | --- |
| `no_aimpunch` | `true` | Hits don't kick aim |
| `no_slowdown` | `true` | Hits don't slow players down |
| `hide_ragdolls` | `true` | Dead bodies disappear at once |
| `short_decals` | `true` | Bullet holes, blood and scorch marks fade after about 3 seconds |
| `anti_rush.enabled` | `true` | Use the barriers from `zones.jsonc` |
| `anti_rush.unlock_seconds` | `60` | Seconds after freeze time before the barrier opens (`0`: no timer) |
| `anti_rush.unlock_players_left` | `1` | Open as soon as a team has this many players alive, or fewer |
| `anti_rush.round_messages` | `true` | Tell players in chat when the barrier closes and opens |
| `anti_rush.line_color` | `[120, 120, 120, 160]` | Barrier lines, as `[red, green, blue, alpha]`. Alpha `0` hides them |
| `anti_rush.touched_color` | `[255, 0, 0, 255]` | Line color while someone is against the barrier |
| `anti_rush.callout` | `true` | Name players who keep walking into the barrier, once per round |
| `anti_rush.callout_seconds` | `0.5` | How long someone must push into it to be named |
| `anti_rush.glow` | `true` | Make named rushers glow, seen by everyone |
| `anti_rush.glow_seconds` | `3` | How long the glow lasts. The rusher's center note says so meanwhile |
| `anti_rush.glow_color_t` | `[240, 195, 105, 100]` | Glow color for terrorists; lower alpha is softer |
| `anti_rush.glow_color_ct` | `[135, 180, 235, 100]` | Glow color for counter-terrorists |
| `anti_rush.center_message` | `true` | Show the rusher a private note in the middle of their screen |
| `anti_rush.center_seconds` | `5` | How long that note stays after the rusher stops pushing |
| `admins` | `[]` | SteamID64s, in quotes, allowed to use `ct` in game |
| `update_check` | `true` | Print a console line when a newer release is out |

**Messages:** they're in `addons/combat_tweaks/translations/`, one file per language. They can be reworded or recolored with `{default}`, `{red}`, `{lightblue}`, `{yellow}`, `{lime}` and `{grey}`. Keep every `{placeholder}`, and keep lines short, because long lines wrap in the chat box. Players whose language has no file get English.

## After a CS2 update

- **Player fields:** read from CS2's own schema, so they keep working.
- **Barriers:** spawning the barrier lines and the rusher glow needs four game functions, found by byte pattern. If an update moves one, only the barrier, or only the glow, turns off with a console message, and the rest keeps working. `ct status` shows which.
- **Fixes:** a fixed release follows.

## Good to know

- **Visibility plugins:** plugins that hide players who can't be seen, such as anti-wallhack plugins, may also hide a rusher's glow behind walls.
- **Respawn modes:** with `mp_respawn_on_death_*`, bodies are removed when the player respawns, whatever `hide_ragdolls` is set to.
- **Lines when shooting:** barrier lines can look brighter or dimmer while shooting. This comes from the game's own exposure, not the plugin.

## Building

Dependencies are pinned in `build-dependencies.json` (Metamod:Source, s2sdk, hl2sdk-manifests, AMBuild) and fetched by the build scripts.

- **Windows** (Visual Studio 2022 C++ build tools, Python 3): `.\scripts\build-windows.ps1`
- **Linux** (Docker; builds in Valve's SteamRT3 "sniper" SDK image): `.\scripts\build-steamrt3.ps1`, or `scripts/build-linux.sh` on Linux with gcc, git and Python 3
- **Tests** for config, barriers, messages and version checks: `scripts/run-tests.sh` (g++ 11 or newer)

`build-steamrt3.ps1 --trace` builds a test version that logs every hit and keeps the normal slowdown on odd player slots, for comparison. Never run it on a live server.

## Credits

- The ragdoll and glow methods follow the public plugins by [oqyh (GoldKingZ)](https://github.com/oqyh).
- The game function patterns were checked against [CS2Fixes](https://github.com/Source2ZE/CS2Fixes) and [CounterStrikeSharp](https://github.com/roflmuffin/CounterStrikeSharp).
- JSON parsing uses [nlohmann/json](https://github.com/nlohmann/json) (MIT, `third_party/nlohmann/LICENSE.MIT`).

## License

[AGPL-3.0](LICENSE)
