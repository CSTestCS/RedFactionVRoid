# RedFactionVRoid

Use any **VRM avatar** as Mason's skin in **Red Faction Guerrilla Re-Mars-tered** (Steam).
The mod adds a **VRM SKIN** entry to the pause menu. Pick a `.vrm` file, and the avatar
replaces Mason. It is automatically scaled to Mason's height and follows his animations.

The mod is client-side only. It changes what your PC draws and nothing else. It does not
touch save files, game archives or the network.

> **Project status: read this first.** The VRM loading, auto-scaling, retargeting,
> rendering, pause-menu UI, input handling and installer are implemented and tested. This
> includes an end-to-end run under Wine's Direct3D 11 with a simulated game skeleton and
> camera (see *Testing*). **The mod has not yet been run against the real game.** To follow Mason,
> it needs the memory locations of his skeleton and the game camera in `rfg.exe`. Those
> depend on the game build and must be found once in game (see
> [docs/CALIBRATION.md](docs/CALIBRATION.md)). Until they are filled in, the menu and the
> avatar preview work, but the avatar is not drawn in the world.

## Installing

**Option A: installer (recommended)**

1. Download `RFVRoid_Installer.exe` from the latest build (GitHub **Actions** → *build* → artifacts, or **Releases**).
2. Close the game and run the installer. It finds the game in your Steam libraries (or asks
   for the folder) and copies the mod in.

**Option B: manual**

Copy `d3d11.dll` into the game folder, next to `rfg.exe`
(Steam → right-click the game → *Manage* → *Browse local files*). Optionally copy it a
second time as `dxgi.dll` (the installer does both; only one copy runs the mod).

If you already use a mod with one of these names (e.g. ReShade), the installer renames it to
`d3d11_chain.dll` / `dxgi_chain.dll`. RedFactionVRoid then loads it, so both keep working.
To uninstall, run the installer again and choose *No* (uninstall).

**Upgrading from the first version** (which installed only `dxgi.dll`): just run the new
installer. It replaces `dxgi.dll` and adds `d3d11.dll`.

### Is it working?

When the game starts, a **REDFACTIONVROID ACTIVE** banner appears at the top of the screen
for a few seconds. If you don't see it, open `RFVRoid.log` in the game folder:

| What the log shows | Meaning |
|---|---|
| No `RFVRoid.log` at all | The game didn't load the mod. Check that `d3d11.dll` is next to `rfg.exe`. On Steam Deck/Proton, add `WINEDLLOVERRIDES="d3d11,dxgi=n,b" %command%` to the launch options. |
| `Rendering hook is working` | Hooks are fine. Pause and press **V** / **Y**, or **F7**. |
| `WARNING: ... no frame has reached the mod` | The mod is loaded but can't draw. Please share the log. |
| `Key 0x76 pressed` | F7 was detected. |

## Using it

1. Put `.vrm` files (VRM 0.x or 1.0, e.g. from VRoid Studio or VRoid Hub) in the `vrm`
   folder inside the game folder. The installer creates it and opens it for you.
2. In game, press **Esc / Start** to pause, then choose **VRM SKIN**. You can click it, or
   press **Y** (controller) or **V** (keyboard). **F7** opens the menu at any time.
3. Choose an avatar. *MASON (NO SKIN)* turns the skin off. *BROWSE…* picks a file from
   anywhere on disk.

Your choice is remembered (`RFVRoid_state.ini`) and loaded automatically next time.

### Automatic sizing

The avatar's total height (feet to top of head/hair) is measured from its skinned mesh.
It is then scaled to Mason:

* `Mode=auto` (default): once Mason's skeleton is readable, the avatar's head-above-ankle
  span is matched to Mason's measured span. Until then, it uses `MasonHeight`.
* `Mode=height`: the avatar's total height is set to `MasonHeight` (default 1.83 m).
* `Multiplier` applies a final adjustment.

The menu shows the original and scaled heights.

## Configuration

`RFVRoid.ini` is created next to `rfg.exe` on first launch. Every setting is commented. The
main sections:

| Section | What it controls |
|---|---|
| `[General]` | enable/disable, menu hotkey, debug overlay |
| `[Scale]` | automatic sizing (see above) |
| `[Menu]` | where the VRM SKIN entry sits relative to the game's pause menu, fonts |
| `[Render]` | depth mode, lighting, foot grounding |
| `[GameSkeleton]`, `[BoneMap]`, `[GameCamera]`, `[GamePause]`, `[GameHide]` | where to find game data in memory (see calibration) |
| `[DrawFilter]` | which of the game's draw calls are Mason (hidden while a skin is active) |

Log file: `RFVRoid.log` in the game folder.

## How it works

```
rfg.exe ──loads──▶ d3d11.dll (this mod) ──forwards──▶ C:\Windows\System32\d3d11.dll
                        │   (also installed as dxgi.dll; only the first copy runs the mod)
                        ├─ wraps D3D11CreateDevice* / CreateDXGIFactory* → hooks the game's
                        │  real device and swap chain as they are created
                        │
                        ├─ hooks IDXGISwapChain::Present  → runs the mod once per frame
                        ├─ hooks ID3D11DeviceContext::DrawIndexed → skips Mason's draws
                        ├─ reads Mason's bones + camera from memory (RFVRoid.ini paths)
                        ├─ retargets the pose onto the VRM humanoid, scales it, and
                        │  draws it with GPU skinning + MToon-style toon shading
                        └─ draws the VRM SKIN entry / menu over the pause screen
```

* **VRM loading** (`src/vrm/loader.cpp`): glTF 2.0 binary parser with the `VRM` (0.x) and
  `VRMC_vrm` (1.0) humanoid extensions and MToon material parameters. Textures are decoded
  with Windows Imaging Component.
* **Retargeting** (`src/vrm/avatar.cpp`): direction-based. Each VRM bone is rotated so that it
  points where Mason's matching bone points. Elbow and knee bend planes, torso twist and head
  orientation are preserved. Because only joint positions are needed, Mason's and the
  avatar's bone axes and rest poses don't have to match.
* **Game binding** (`src/game/`): Cheat-Engine-style pointer paths with byte-signature
  scanning. Game patches can be handled by editing the .ini, without recompiling.
* **Proxy** (`src/win/proxy.cpp`, `proxy_exports.S`): every `d3d11.dll` / `dxgi.dll` export
  jumps to the real DLL (loaded on first use, never inside `DllMain`), so the game behaves
  exactly as before. Device and factory creation are wrapped to hook the game's own objects.
* **Input** (`src/win/input.cpp`): keys are read both from window messages and from the
  keyboard state every frame, so hotkeys work even if the game uses raw input or DirectInput.

### Hair, faces and lighting

* **Hair / skirt physics.** The spring bones and colliders authored in the VRM (VRM 0.x
  `secondaryAnimation` and VRM 1.0 `VRMC_springBone`) are simulated at 60 Hz
  (`src/vrm/spring.cpp`). Hair swings when Mason runs, turns or jumps. It is pushed out of
  the head and body colliders and freezes while the game is paused. `[Physics] Wind` adds a
  breeze.
* **Facial expressions.** The avatar blinks naturally. Choose its resting face in the VRM
  menu (**FACE**: neutral, happy, angry, sad, relaxed, surprised, or the avatar's own custom
  expressions). If Mason's health is configured (`[GameState] Health`), it winces when hit
  and looks worried at low health (`[Expressions]`).
* **Lighting.** With `Lighting=scene` (default), the avatar is lit from the game's own
  rendered image around Mason. Bright Mars daylight, dark tunnels, red alarm lights and
  explosions carry over onto the avatar, including the direction they come from. If the
  sun's direction/colour in memory are configured (`[GameLighting]`), toon shading also
  follows the time of day. `Lighting=fixed` restores a constant light.

Remaining limitations:
* No lip-sync to dialogue.
* The lighting is an estimate from the screen, not the game's own lighting pass; shadows
  cast by the world don't fall on the avatar.
* With `DepthMode=none`, scenery in front of Mason doesn't hide the avatar.

## Building

Requirements: CMake ≥ 3.16 and MinGW-w64 (GCC). Cross-compiling from Linux:

```sh
sudo apt install mingw-w64 cmake python3
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-win -j          # -> build-win/bin/d3d11.dll, RFVRoid_Installer.exe
```

On Windows, use an MSYS2 MinGW64 shell: `cmake -S . -B build -G Ninja && cmake --build build`.

Unit tests (VRM parsing, scaling, retargeting, pointer paths, config) run on the host:

```sh
cmake -S . -B build-host && cmake --build build-host -j && ctest --test-dir build-host --output-on-failure
```

`tools/make_test_vrm.py` generates the synthetic VRM 0.x / 1.0 test avatars.

### Testing

`rfv_smoke.exe` (built with the Windows targets) acts like a tiny D3D11 game. Like
`rfg.exe`, it imports `d3d11.dll`, so it loads the mod from its own folder. Run it from a
folder containing the mod as `d3d11.dll` (and optionally `dxgi.dll`) plus a `vrm` folder:

```sh
rfv_smoke.exe 150 menu.bmp menu     # opens the VRM menu with F7 and applies the first avatar
rfv_smoke.exe 90 pause.bmp pause    # presses Esc: the VRM SKIN pause entry appears
rfv_smoke.exe 0 x prepare-world     # writes RFVRoid.ini for the world test (run first)
rfv_smoke.exe 120 world.bmp world   # fake skeleton + camera in memory: avatar follows it
                                    # (RFV_SMOKE_BG=blue|orange|dark changes the scene colour)
```

The mod saves the requested frame as a BMP (`RFVROID_CAPTURE_FRAME` / `RFVROID_CAPTURE_PATH`).
On Linux it runs under Wine with `WINEDLLOVERRIDES="d3d11,dxgi=n,b"` and `xvfb-run`.

The shaders avoid structured and typed buffers so they also compile with Wine/Proton's HLSL
compiler (relevant for Steam Deck).
On Proton, add `WINEDLLOVERRIDES="d3d11,dxgi=n,b" %command%` to the game's launch options
so the mod's DLLs are used.

## Avatar licences

VRM files carry licence terms in their metadata (VRoid Hub shows them too). Only use avatars
whose licence allows this kind of personal use.
