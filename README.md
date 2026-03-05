# VR Loading Screens

A Fallout 4 VR mod that replaces the default loading screens with custom concept art backgrounds and preserves the game's loading tips and level progress display as a floating VR overlay.

## Features

- **Custom background images** — Random DDS artwork shown as a VR overlay during loading (from `Data/Textures/LoadingScreens/`)
- **Tip & level display** — Game's loading tip text and level progress captured and shown as a transparent floating panel
- **Loading speed optimization** — Breaks the animation loop sleep so loading runs at full CPU speed
- **MCM VR settings** — Choose between three modes:
  - **Black (fastest)** — Plain black screen, no overlays
  - **Background only** — Random concept art image
  - **Background + Tips** — Art plus floating tip/level text (default)

## Requirements

- [Fallout 4 VR](https://store.steampowered.com/app/611660/Fallout_4_VR/)
- [F4SEVR](https://f4se.silverlock.org/) (Fallout 4 Script Extender VR)
- [MCM VR](https://www.nexusmods.com/fallout4/mods/21497) (Mod Configuration Menu VR) — for the in-game settings menu

## Installation

Install with your mod manager of choice (Mod Organizer 2 recommended). The mod folder structure:

```
Data/
  F4SE/Plugins/VRLoadingScreens.dll
  MCM/Config/VRLoadingScreens/config.json
  MCM/Config/VRLoadingScreens/settings.ini
  Textures/LoadingScreens/*.DDS
```

Add your own DDS images (DXT1/DXT5, recommended 2048x1024) to `Data/Textures/LoadingScreens/` for custom backgrounds.

## Building from Source

Requires:
- Visual Studio 2022
- CMake 3.23+
- vcpkg (`VCPKG_ROOT` environment variable set)
- [F4VRCommonFramework](https://github.com/rollingrock/F4VRCommonFramework) (set path in CMakeLists.txt)

```powershell
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Output DLL is copied to `package/Data/F4SE/Plugins/`.

## Technical Details

- Uses OpenVR IVROverlay API for VR overlay rendering
- Alpha-key shader strips black background from captured loading screen frame
- Frame capture crops top 55% of left eye to exclude VR controller models
- World-locked overlays positioned using HMD pose at capture time
- Deferred NOP applied inside VR Submit hook for safe animation loop exit

## License

MIT
