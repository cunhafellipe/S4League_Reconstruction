# S4League Reconstruction

Clean-room C++ reconstruction of an early S4 League client, focused on reproducing the original runtime behavior from client data and reverse-engineering notes.

## Current state

- Direct3D 9 renderer with maps, characters, textures and skinned meshes.
- Character movement, collision, jumping, wall jumping and directional dodge.
- Lua-driven actor and weapon animation states, transitions and attack timing.
- Weapon attachment data, editable bone overrides and dual-part weapons.
- Lua weapon effects and sounds with native XAudio2 playback.
- Per-map OGG playlists read from each map's `[BGM]` section.
- Default 60 FPS cap; `F2` cycles 60 / 144 / uncapped.

This is an active reconstruction. Some weapon states, effects, collisions and animation transitions still need accuracy work.

## Required game resources

Game resources are proprietary and are intentionally not included in this repository.

Extract the files from a compatible S4 League client and place the complete extraction root beside `S4Client.exe`, named `resources`:

```text
build/Release/
  S4Client.exe
  resources/
    resources/
      bgm/
      effects/
      mapinfo/
      model/
      script/
      sound/
      xui/
    xml/
      weapon_attach.x7
```

The outer folder must be named exactly `resources`. Do not commit extracted assets.

## Build

Requirements:

- Windows 10 or newer
- Visual Studio with Desktop development with C++
- Windows 10/11 SDK
- Win32/x86 toolchain

Open `s4client.sln` and build `Release | Win32`, or use MSBuild:

```powershell
msbuild s4client.sln /m /p:Configuration=Release /p:Platform=Win32
```

Run the client with `Run.bat` from the repository root.

## Controls

- `WASD`: move
- `Space`: jump; press shortly after `A` or `D` to dodge
- Mouse: camera and weapon actions
- `Esc`: map selector
- `F1`: toggle weapon-bone editing mode
- `F2`: cycle FPS cap
- `F3`: animation tuning panel
- `F4`: weapon selector
- `F5`: pause/resume map music
- `F6` / `F7`: next/previous map track

## Repository policy

Extracted game resources, logs, debug symbols, IDA databases and local reverse-engineering notes are ignored. Only this README is tracked as project documentation.
The repository keeps a minimal runnable `build/Release` package (`S4Client.exe`, the required D3DX DLL, Lua shim, weapon loadout and bone overrides); extracted resources, logs and debug symbols remain excluded.
