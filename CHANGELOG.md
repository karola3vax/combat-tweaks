# Changelog

## 1.0.3

- Fixes server crashes on the CS2 update of November 23, 2026 (build 2000950). 1.0.2 does not work on this build.
- Nothing else changes.

## 1.0.2

- Built against Metamod:Source 2.0.0 build 1490 and the current s2sdk, which now has the network message layout of CS2 build 2000943. The plugin no longer carries its own copy of that change.
- It still loads on Metamod 2.0.0 build 1469 and newer; build 1490 or newer is recommended, because it fixes Metamod's own plugin-unload bugs.
- Nothing changes in game.

## 1.0.1

- Fixed for CS2 build 2000943, which changed how the game builds network messages. Chat messages, center-screen notes, warning sounds and the game language check use the new layout. 1.0.0 does not work on this build.
- A game setting the plugin cannot find no longer stops the plugin from loading. Only that setting is off, the plugin tries again every second, and `ct status` names it.
- No aimpunch and short decals work without CounterStrikeSharp. The plugin now finds the game's hidden developer settings itself; before, they were found only while CounterStrikeSharp was loaded, because it unhides them.

## 1.0.0

First public release.

- No aimpunch or slowdown when hit.
- No ragdolls.
- Short bullet, blood and scorch decals.
- Anti-rush barriers, drawn with the game's retake barrier walls, that block like a wall and open after a set time or when a team is down to its last player. Players who keep pushing into them are named in chat, glow in their team color, hear a warning sound, get a private note in the middle of their screen and lose health while they push on.
- `!rs` in chat resets a player's own score.
- Optional team colors: T and CT player models tinted in their team color (`team_colors`, off by default).
- In-game zone editor (`ct zone`), plus `ct status` and `ct reload`.
- Settings in `combat_tweaks.jsonc`, barriers in `zones.jsonc`.
- Chat in 9 languages, matched to each player's game language.
- A console note when a newer release is out.
