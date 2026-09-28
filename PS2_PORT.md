# OpenTESArena — PlayStation 2 port

A native PS2 homebrew build of OpenTESArena. The goal is the real OpenTESArena engine and game logic running on
the Emotion Engine, with desktop platform machinery (SDL2, Vulkan/software renderer, OpenAL/WildMIDI, Jolt Physics,
worker threads) replaced by PS2-native implementations built around the EE, GS, DMA, IOP and SPU2.

The desktop build is unchanged and still uses CMake. The PS2 build uses PS2Build (`ps2.yaml`).

> **Status:** early bring-up. The ELF builds and boots on PCSX2 through IOP setup, controller/audio drivers, logging,
> options and `Game::init()`, and stops at the Arena data check when no data is present. Rendering, input, audio
> and physics have **not yet been exercised with real Arena data** — see [Known limitations](#known-limitations).

---

## Building

### Install PS2Build

Follow <https://ps2.techwritescode.dev/>. Verified with:

```
ps2build v2026.09.24.1
toolchain v2026.09.10 (mips64r5900el-ps2-elf GCC 15.3.1), sdk-core v2026.09.15, sdk-world v2026.09.07.1
```

Optional (Claude Code): `claude mcp add ps2build -- ps2build mcp`.

### Build

From the repository root:

```
ps2build build
```

Output: `build/bin/opentesarena-ps2.elf`.

If the host runs out of memory compiling (many parallel `cc1plus` processes on large engine files), run
`ps2build generate` and then build with fewer jobs: `ninja -C build -j 6`.

### Package

```
bash ps2/tools/package.sh
```

Creates `build/OpenTESArena/` ready to copy to a USB stick.

### Required PS2Build packages

All resolved by name through `libs:` / `embed_irx:`; nothing is vendored or copied from other SDKs.

| Package | Use |
|---|---|
| `gskit`, `dmakit` | GS setup, framebuffers, DMA render queue, texture transfers |
| `pad` + `sio2man`, `padman` (IRX) | DualShock 2 |
| `audsrv` + `libsd`, `audsrv` (IRX) | SPU2 ADPCM voices |
| `filexio` + `iomanx`, `filexio` (IRX) | newlib file I/O through iomanX devices |
| `usbd_mini`, `bdm`, `bdmfs_fatfs`, `usbmass_bd_mini` (IRX) | USB mass storage — loaded only when booting from USB |
| `patches` | `sbv` patches for loading embedded IRX |
| `debug` | on-screen fatal error console |

---

## Install layout and Arena data

```
mass:/OpenTESArena/
  opentesarena-ps2.elf
  data/                  OpenTESArena's own data (meshes, ui, audio/music definitions, Clocks.txt, exe strings)
  data/ARENA/            <- your own copy of The Elder Scrolls: Arena (floppy version, contains A.EXE)
  data/ARENACD/          <- or the CD version (contains ACD.EXE)
  options/options-default.txt   PS2 defaults (ps2/dist/options)
  options/options-changes.txt   created on first run
  log/                   log files (one per boot, 10 kept)
```

**Arena's data is not distributed with OpenTESArena and must not be committed.** Arena is available free of charge
from Bethesda; copy your own install's files into `data/ARENA` (or `data/ARENACD`).

All paths are relative to the directory the ELF was launched from (derived from `argv[0]`), so the folder can live
anywhere: `mass:`, `host:` (PCSX2/ps2link), and in principle `cdrom0:`/HDD. Nothing is hardcoded to `mass0:`.

---

## Controller layout (DualShock 2)

Input goes through OpenTESArena's semantic input actions (`InputActionName`). Each pad button maps to an action and
is delivered through the binding the engine itself defines for that action in the currently active action map, so
the existing UI/listener code runs unchanged.

**Game world** (modern free-look interface is the PS2 default)

| Control | Action |
|---|---|
| Left stick | Move / strafe |
| Right stick | Look (deadzone, sensitivity; options `HorizontalSensitivity` / `VerticalSensitivity` / `InvertVerticalAxis` still apply) |
| Square | Attack. Hold Square and flick the right stick to choose the swing direction; a plain tap swings in a random direction (as in desktop modern mode). Bows fire on release. |
| Cross | Activate |
| Circle | Inspect |
| Triangle | Character sheet |
| R1 | Jump |
| L1 | Draw / sheathe weapon |
| L2 | Cast magic |
| R2 | Use item |
| Start | Pause menu |
| Select | Automap |
| D-pad Up / Down / Left / Right | World map / Camp / Logbook / Status |
| L3 / R3 | Player position / Steal |

**Menus**

| Control | Action |
|---|---|
| D-pad | Snap the pointer to the nearest button / list item in that direction (no precise analog aiming needed) |
| Left stick | Free pointer movement |
| Cross / Square | Left / right click |
| Circle | Back |
| Start | Accept |
| L1 / R1 | Scroll lists |
| Triangle | Backspace |

**Text entry** (character name) — temporary until an on-screen keyboard exists: D-pad Up/Down picks a letter,
Right or Cross adds it, Left or Triangle deletes, Start accepts.

---

## Architecture

```
OpenTESArena game/engine code (OpenTESArena/src, components/)   <- compiled unchanged except a few __PS2__ boundaries
        |
platform boundaries: RenderBackend, Window, Platform::, AudioManager, InputManager, JPH:: (physics), SDL data types
        |
ps2/  (PS2 implementations)
  platform/   Ps2Platform (boot, IOP/IRX, boot device, paths, timing, memory stats, fatal screen),
              Ps2EnginePlatform (Platform::), Ps2Window (Window), Ps2Video (display geometry)
  rendering/  Ps2GsRenderBackend (RenderBackend on the GS)
  audio/      Ps2AudioManager (AudioManager on SPU2 via audsrv), Ps2Adpcm (SPU ADPCM encoder)
  input/      Ps2Input (DualShock 2 -> input actions / pointer)
  physics/    JoltLite (PS2 physics backend implementing the JPH:: subset the engine uses)
  compat/     minimal SDL2 *data-type* subset + its PS2-backed functions (not an SDL port)
        |
EE / GS / DMA / IOP / SPU2 (PS2SDK via PS2Build)
```

### Platform substitution instead of scattered #ifs

Desktop-only implementation files are simply not compiled on PS2; `ps2.yaml` compiles PS2 implementations of the
same headers instead:

| Desktop file | PS2 replacement |
|---|---|
| `Rendering/Window.cpp` (SDL half) | `ps2/platform/Ps2Window.cpp` |
| `Rendering/Sdl2DSoft3DRenderBackend`, `SdlUiRenderer`, `SoftwareRenderer`, `VulkanRenderBackend` | `ps2/rendering/Ps2GsRenderBackend` |
| `Audio/AudioManager.cpp`, `Audio/WildMidi.cpp` | `ps2/audio/Ps2AudioManager.cpp` |
| `Utilities/Platform.cpp` | `ps2/platform/Ps2EnginePlatform.cpp` |
| Jolt Physics | `ps2/physics/JoltLite` |

### Engine source changes (all desktop-safe)

| File | Change |
|---|---|
| `Math/PlatformAbi.h` (new), `Vector2/3/4.h`, `Quaternion.h` | PS2 compiler workaround (see below). Expands to nothing on desktop. |
| `Rendering/WindowCommon.cpp` (new), `Window.cpp`, `CMakeLists.txt` | Platform-neutral window math moved verbatim out of the SDL file so PS2 can share it. |
| `Rendering/RenderMeshUtils.h/.cpp`, `RenderVoxelChunkManager.cpp`, `RenderEntityManager.cpp`, `Renderer.cpp` | Transform heaps upload only their used prefix (a pure speedup everywhere); `MAX_TRANSFORMS` is 2048 on PS2. |
| `Renderer.cpp` | Selects `Ps2GsRenderBackend` on PS2. `Span<int>` for index copy (`int32_t` is `long` on the EE). |
| `World/MeshLibrary.cpp` | `int` → `int32_t` index copy made explicit (same reason). |
| `Input/InputManager.h/.cpp` | Read-only `getInputActionMaps()`; PS2 hook calling `Ps2Input::update()` once per frame. |
| `Interface/GameWorldUiState.cpp` | PS2 hook: right-stick melee swing direction. |
| `Game/Game.cpp` | Frame limiter disabled on PS2 (GS vsync paces frames). |
| `Main.cpp` | `Ps2Platform::boot()` first; log file failure is non-fatal on PS2 (read-only media). |

### Compiler workaround: doubles in structs

The EE GCC (R5900, n32 ABI, `-msingle-float`) hits an internal compiler error ("maximum number of generated reload
insns") whenever a struct containing a `double` field is passed or returned by value: the n32 ABI assigns such
fields to 64-bit FPRs the single-precision R5900 FPU doesn't have. `Double2/3/4` and `Quaternion` are passed by value
everywhere, so on PS2 they get a user-provided copy constructor (`OTA_PLATFORM_ABI_COPYABLE`), which makes the C++
ABI pass them by reference. Float instantiations stay trivially copyable (C++20 conditionally trivial special
members). This is a toolchain bug and worth reporting upstream.

### Rendering — `Ps2GsRenderBackend` (gsKit + dmaKit)

**Why gsKit/dmaKit rather than ps2gl:** OpenTESArena's `RenderBackend` already hands the backend explicit vertex,
index and uniform buffers, textures, materials and ordered draw-call ranges. What's missing is only the rasterizer,
and the GS *is* one. gsKit gives thin, explicit control over GS registers, VRAM placement and the DMA queue, which
is what's needed to keep 8-bit textures + CLUTs, manage the 4 MiB of VRAM ourselves, and batch arbitrarily. ps2gl
would put an OpenGL 1.x state machine and its own texture/memory management between the engine and the GS for no
gain. The two are never used together.

- **No CPU framebuffer.** The EE transforms vertices (single precision), clips against the near plane plus a guard
  band, computes lighting, and writes REGLIST GIF packets (RGBAQ/ST/XYZ2, 24 bytes per vertex) directly into gsKit's
  double-buffered DMA queue. The GS rasterizes everything; perspective-correct STQ texturing and Z are native.
- **Batching.** Draw calls within each command-list range (sky, voxels, entities, weather...) are recorded, then
  sorted by texture / CLUT / GS state before emission, so a frame is a few dozen state changes rather than one per
  voxel face. Blended draws (ghosts) keep submission order. Per-frame scratch is bounded (24k vertices, 4k batches)
  and never allocated per frame.
- **8-bit textures stay 8-bit.** Arena textures are uploaded as PSMT8 with a 256-entry CT32 CLUT (1 KiB). Per-mesh
  lighting builds a CLUT from the palette and the matching row of Arena's light table, which reproduces the
  original colormap shading exactly for sprites and per-mesh-lit geometry. Per-pixel-lit geometry uses Gouraud
  modulation against the brightest row. Index 0 is transparent through the GS alpha test. CLUTs are keyed by
  palette *content* (the engine rewrites the palette every frame) and uploaded inline right before first use.
- **VRAM is a managed cache.** Layout: two 640×224 16-bit field buffers + 16-bit Z (840 KiB), 32 CLUT slots, then an
  8 KiB-page texture pool (~3.1 MiB) with LRU eviction and re-upload from the EE copy. Uploads go into the draw queue
  (`gsKit_texture_send_inline`), so eviction mid-frame is safe: earlier draws finish before the overwriting upload.
- **Video.** NTSC 640×448 interlaced (field rendering, 640×224 buffers), 4:3. Engine-facing "window" space is
  square-pixel 640×448, so Arena's 320×200 UI maps with the original CRT aspect (2× horizontal for readable text).
  PAL (640×512) is wired from the ROM region but secondary; progressive modes can be added in `Ps2Video` and
  `initContext`.
- Door texture-coordinate animation is applied per vertex (it's affine). Water/lava "screen-space" animation is
  approximated with a UV scroll.

### Physics — JoltLite

Real Jolt was not used: its desktop configuration (64 MiB temp allocator, 250k bodies, job threads) exceeds the whole
EE RAM, and it has no R5900 port. OpenTESArena's physics needs are small and Arena-shaped, so `ps2/physics/JoltLite`
implements just the `JPH::` subset the engine calls (~70 symbols). `Player`, `EntityChunkManager`,
`CollisionChunkManager` and `PhysicsContactListener` compile unchanged against it.

- **World:** the static compound bodies `CollisionChunkManager` builds from greedy-merged voxel boxes (walls, doors,
  sensors), with a per-compound 4-unit XZ grid for broadphase. Y-rotated boxes (diagonal walls) are handled exactly.
- **Actors:** vertical capsules, treated as rounded cylinders. Collisions are resolved with sub-stepped penetration
  push-out; the rounded bottom rides small ledges (step-up ≈ half the radius, like a Jolt capsule). Includes
  character ground support via a post-simulation probe, kinematic/dynamic/static motion types, locked DOFs, gravity
  factor and linear damping.
- **Sensors/contacts:** `OnContactAdded` fires once per overlap start (triggers, level transitions, projectiles vs
  walls and entities), using the engine's own `PhysicsLayer` filters.
- **Bounded:** fixed pools sized in `Collision/Physics.h` for PS2: 4096 bodies (~450 KiB), 512 tracked contacts, 120 Hz sub-steps (desktop Jolt uses 250k bodies / 64 MiB temp memory / 240 Hz).
- Ray selection already uses the engine's own DDA voxel ray caster and doesn't touch physics.

### Audio — `Ps2AudioManager` (audsrv / SPU2)

- **Sound effects:** Arena's 8-bit `.VOC` files are encoded once to SPU ADPCM on the EE (integer-only encoder), then
  uploaded to SPU2 RAM through audsrv on first use and played on hardware voices (up to 24) with per-voice
  volume/pan for 3D positioning. Nothing is decoded per frame and no PCM stays resident. The EE keeps an ADPCM cache
  capped at 768 KiB; SPU2 RAM is flushed and refilled on demand when full. Voice completion is time-based, so there
  are no per-frame IOP RPC polls.
- **Music (MIDI): not implemented yet.** Song selection and staging logic are kept, but playback is silent. The
  planned approach is a small EE-side sequencer rendering to the audsrv PCM stream with a compact instrument set
  produced by a host-side conversion tool, so users convert their own resources and nothing proprietary ships.
  No large SoundFont is made resident.

### Input — `Ps2Input`

See [Controller layout](#controller-layout-dualshock-2). libpad in DualShock mode, locked to analog. The pad is read
once per frame from the `InputManager::update()` hook. Events go through a fixed-capacity ring (no allocation).

### Filesystem

PS2Build's libstdc++ supports `std::filesystem`, and `fileXioInit()` routes newlib (`fopen`/`stat`/`opendir`) through
iomanX devices. The engine's `File`/`Directory`/`Path`/VFS code therefore runs unchanged. Host boots skip the IOP
reset and fileXio so PCSX2/ps2link `host:` keeps working. The VFS's case-insensitive lookup is unchanged.

### Memory budget (EE, 32 MiB)

| Item | Size |
|---|---|
| Kernel reserve | 1 MiB (ELF loads at 0x100000) |
| ELF (text+data+bss) | ~4.5 MiB (ends at ~5.5 MiB) |
| gsKit render queues | 2 × 512 KiB one-shot + 32 KiB persistent |
| Physics body pool | 4096 bodies ≈ 450 KiB |
| Voxel transform heaps (per chunk) | 2048 × (128 B Matrix4d + 64 B float copy) ≈ 400 KiB — first to optimize |
| Heap (everything else) | ~25 MiB available at boot |

Instrumentation: `Ps2Platform::sampleMemory()`/`logMemoryStats()` (heap arena, in-use, peak, unclaimed), plus
counters for GS VRAM used/peak, texture counts and bytes, per-frame upload bytes, audio cache bytes, draw calls and
triangles. `operator new` failure goes through a new-handler that logs memory stats and shows the fatal screen;
allocation failures never fail silently.

### Diagnostics

- TTY (EE stdout/stderr) plus a log file per boot in `log/`, covering IRX loading, boot device, base path, ROM
  region, pad state, GS/VRAM layout, audio init, and the engine's own logging.
- Fatal errors (`DebugCrash`, message boxes, OOM) re-initialize the GS into a text console and print the message,
  memory stats and boot info instead of leaving a black screen; START exits.

---

## Hardware test procedure

1. `ps2build build`, then `bash ps2/tools/package.sh`.
2. Copy `build/OpenTESArena/` to the root of a FAT32 USB stick, and your Arena data into `data/ARENA` (or `ARENACD`).
3. Launch `mass:/OpenTESArena/opentesarena-ps2.elf` from wLaunchELF (or another ELF loader) on a real console.
4. Watch `log/` on the stick afterwards, or TTY over ps2link/UDPTTY if available.
5. Check: boot → main menu readable on a 4:3 TV → new game → character creation fully by controller → city → a
   dungeon interior → combat → transitions. Watch the memory lines in the log for peak heap and VRAM.
6. PCSX2 is for quick iteration only (it hides DMA/timing/cache issues and is far faster than an EE at soft-float
   `double` math). Performance and correctness conclusions must come from real hardware.

---

## Known limitations

- **Not yet run with Arena data.** Everything after `Game::init()`'s data check is untested at runtime: GS
  rendering, UI, input mapping, collision, audio.
- Music is silent (see Audio).
- Screenshots return a black image (GS→EE readback not wired).
- Per-pixel lighting is Gouraud rather than Arena's exact light-table bands; fog light tables, puddle reflections,
  star brightness limiting and citizen palette remapping are approximated or not yet applied.
- PSMT8 uploads of odd-width textures may need row padding on real hardware (to verify).
- Text entry is a temporary D-pad letter picker.
- The engine still uses `double` widely. The EE emulates doubles in software, so gameplay code and the engine's
  own per-frame double math will be the main CPU cost. Convert hot paths (visibility, chunk updates, transform
  heaps) after measuring; don't blanket-convert world coordinates.
- `InputManager::update()` builds several small `std::vector`s per frame (desktop code). This is heap churn to
  remove after profiling.
- The HDD, CD/DVD boot paths and memory-card saving are not implemented (options are saved next to the ELF).
- Only NTSC is tested; PAL is wired but unverified; there's no 480p yet.
