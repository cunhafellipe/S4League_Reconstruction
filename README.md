
# S4League Client Source Code Reconstruction

An ongoing reverse engineering project focused on reconstructing the original S4 League game client source code in C++.

## Project Goals
- Reconstruct original client functionality
- Analyze and document game client internals
- Rebuild client systems from reverse engineering research
- Document the original client architecture

## Keywords
S4League Source Code, S4 League Client Source,
S4League Reverse Engineering, S4League Client Reconstruction


Clean-room C++ reconstruction of an S4 League client, focused on reproducing the original runtime behavior from client data (Lua scripts, `.x7` tables, `.xui` layouts, `.scn` models, `.seq` effects) and reverse-engineering notes.

A prebuilt client is included in `build/Release/`. To play you only need to add the game resources (see below).

## Quick start

1. Copy the game resources into `build/Release/resources/` (layout below).
2. Run `Run.bat` (or `build/Release/S4Client.exe`).

The client starts offline (`"online": { "enabled": false }` in `config.json`).

## Required game resources

Game resources are proprietary and are intentionally **not** included in this repository.

Extract the files from a compatible S4 League client and place the extraction root beside `S4Client.exe`, named `resources`:

```text
build/Release/
  S4Client.exe            (included)
  d3dx9_29.dll            (included)
  config.json             (included)
  weapon_bone_overrides.json (included)
  third_party/
    lua_weapon_shim.lua   (included)
  resources/              (you provide this)
    resources/
      bgm/
      effects/
      gui/
      image/
      mapinfo/
      model/
      script/
      sound/
      xui/
    xml/
      weapon_attach.x7
```

The outer folder must be named exactly `resources`. Do not commit extracted assets.

## config.json

```json
{
    "weapons": [ { "scn": "sword.scn", "states": "PlasmaSword" } ],
    "online": { "enabled": false, "server": "127.0.0.1", "port": 4100, "nickname": "player" },
    "resource_mode": "normal",
    "music_enabled": false,
    "music_track": "",
    "graphics_quality": "high",
    "resolution": "1600x900",
    "display": "windowed",
    "aspect": "auto"
}
```

- `weapons`: up to 3 slots (keys `1`/`2`/`3`); `scn` is a file in `resources/resources/model/weapon/`, `states` the `NormalState_<X>` suffix of the actor-state Lua. The `F4` menu writes this list.
- `online`: address of a multiplayer relay server (WebSocket on `port`, UDP on `port + 1`). The relay server is not part of this repository; with `enabled: false` the client plays offline.
- `resource_mode`: `normal` reads the vanilla tree (`resources/`). `full` reads an extended resource tree with the same layout (`xml/` + `resources/`), found automatically next to `resources/` (the folder with the richest script folder) or given by an optional `"full_dir"` key; it falls back to `normal` when none is found. The scripts of either set work whether they are plain text or compiled Lua.
- `music_enabled` / `music_track`: the music is **off** by default. `ESC > Settings` (Music, Track), `F5` and `F6` change it and every change is saved here; `music_track` is a file of the `Bgm` folder (`"Bgm/x.ogg"`), empty = the map's own playlist.

## Version 2.2

- **Vanilla resources**: the data scripts are executed instead of text-scanned, so the compiled (`LuaQ`) scripts of an unmodified client work, and `resource_mode` switches the whole resource tree (models, xml, sounds, UI, scripts).
- **Actor states run natively**: every `actorstates_*.lua` is executed with a recorder standing in for the client's bindings; animations, sounds, effects, weapon-change forms (shape change) and ghost trails come from the scripts.
- **Combat from the attack scripts**: every `CreateCAttack` shape and fire time, critical / depends-on rules, stun, blow, uppercut (the attacker rises), push, knock-down / stand-up / death on the victim, camera shake.
- **Maps**: animated map objects and effects, sky pass, GPU buffers for the static geometry (much higher FPS).
- **Other players' weapons** are holstered like ours; holstered guns hang straight on the hip.
- **Tools**: clones (`C` leaves a copy of you with its pose and effects, `V` releases them; they can be hit), `PrtScr` saves `screenshots/*.png`, `s4_crash.log` / `s4_crash.dmp` on a crash, `loaded resources.log` (every model / texture / effect / sound with its memory), unused textures and sounds are released, the client is large-address-aware (4 GB).

## What works

**Rendering and world**
- Direct3D 9 renderer: maps with lightmaps, characters, costumes, skinned meshes, weapons.
- Character movement, collision, jumping, wall jumping, falling and directional dodge.
- Per-map OGG playlists from each map's `[BGM]` section; XAudio2 sound effects.

**Animation**
- Lua-driven actor states, transitions, blend in/out and attack timing (`SetAnim`, `SetBlendingAnim`, lower/upper body split).
- Per-weapon idle, run, attack, jump-attack and reload animations.
- Gun aim blending by camera pitch (client `CCustomAnimBlender` rule), per-direction fire poses with cross-fades when the walking direction changes.

**Weapons**
- Weapon attachment from `weapon_attach.x7`, scene tree linked like the client, dual-part weapons, worn weapons.
- Melee combos, strong/charged swings, right-click actions and jump attacks (any left click in the air starts the jump attack, as in `UseWeapon1_Jump`).
- Guns: fire rate, magazine, reload (`R`), per-weapon crosshair and ammo arc from the HUD layout, shots through the crosshair, every ray and spread pattern defined in the weapon Lua (`MAKERAY`, `AddProjectileOffset`), shots from the weapon's fire node.
- Point-blank wall shots push the shooter back with the attack's `AddPushCondition` (shotguns more than revolvers).

**Effects (`.seq`)**
- Particle emitters with the client rules: one burst vs continuous emission, keyframed size/colour/rotation, render types (camera-facing, velocity-stretched, upright, flat), texture animation.
- Effect meshes with their own timing, fade in/out and per-mesh opacity tracks.
- Beam nodes (`CActLightning`) for gun tracers such as the smash rifle rail.
- Weapon blast, wall-hit and player-hit sequences from the attack attributes.

**Online (with a relay server)**
- Players see each other's costumes, weapons, movement and animations.
- Shots are replicated: muzzle blast, bullets/beams and impacts are drawn on every client.
- Melee and bullet hits, HP (starts at 100) shown at the bottom and over each player's head, respawn at 0 HP.

## Known gaps

- Default bullet tracer for guns without a beam sequence (`CGunTracerNode`) is approximated.
- Not applied yet from the Lua: movement calls of some weapons (Iron Boots boost, `SetDelayDrop`, `SetTimeToStaySky`, `SetJUMP_POWER`), the SP system, guard rules, distance drop-off of damage, explosive projectiles (rockets, bombs, mines). Blow flight, down time and shake length are approximations.
- Effect particle size uses half of the keyframe size (matches the original on screen; the exact scale in the effect node chain is still being located).
- Other players' weapons: rigid pieces only (skinned pieces and some special weapons are not drawn on them yet).
- Some weapon skins and effects still need accuracy work.

## Build

Requirements:

- Windows 10 or newer
- Visual Studio with Desktop development with C++
- Windows 10/11 SDK
- Win32/x86 toolchain

Open `s4client.sln` and build `Release | Win32`, or:

```powershell
.\build.ps1 Release
```

The output goes to `build/Release/S4Client.exe`. Run it with `Run.bat` from the repository root.

## Controls

- `WASD`: move; double `W`: sprint
- `Space`: jump; dodge with `Space` after `A` / `D`
- Left mouse: attack / fire; right mouse: weapon action
- `R`: reload; `Z`: switch shoulder camera
- `1` / `2` / `3`: weapon slots
- `Esc`: menu / map selector
- `F1`: weapon-bone editing mode
- `F2`: cycle FPS cap (60 / 144 / uncapped)
- `F4`: inventory / weapon selector
- `F5` / `F6`: music on / off, next track (`Shift+F6` previous); saved in `config.json`
- `C`: leave a clone where you stand (as many as you want); `V`: release them
- `PrtScr`: screenshot to `screenshots/`
- `[` / `]` (gun holstered): turn the holstered grip side

## Repository policy

Only the client source, project files and a minimal runnable `build/Release` package (`S4Client.exe`, the D3DX DLL, `config.json`, the Lua shim and the bone overrides) are tracked. Extracted game resources, logs, debug symbols and local tools are excluded.
