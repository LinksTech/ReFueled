# ReFueled

ReFueled is an experimental native Windows (x64) port of *Crash Team Racing Nitro-Fueled*
(PlayStation 4, CUSA13795, version 1.21), based on
[SacredFire](https://github.com/T3CHNOLOG1C/SacredFire) (Linux port), which builds on
[bbport](https://github.com/deadinside28/bloodborne_pc), which in turn uses parts of
[shadPS4](https://github.com/shadps4-emu/shadPS4). Chain: SacredFire -> bbport -> shadPS4.

> **Upstream no longer reachable:** since 2026-10-14 the SacredFire repository
> (`T3CHNOLOG1C/SacredFire`) returns "not found" on GitHub; it was not renamed or moved. This
> repository keeps SacredFire's full history up to its last public state (commit `f1f7935`, one docs commit after tag v0.2.1).
> The credits below are unchanged.

> **Status: very early, not playable.** The game does not start yet. Do not expect to be able
> to play anything.

## Status

| Milestone | Goal | Status |
|---|---|---|
| M1 | Load the executable at 0x800000000 and resolve imports | not reached |
| M2 | Run game init up to the first GPU submit | not reached |
| M3 | Main menu | not reached |
| M4 | Arcade race with sound, controller and saving | not reached |
| Mod loader | Loose-file mods, patches and plugins | planned (design draft only, no code) |

What exists so far: the Windows platform layer (`src/platform/`) and the Windows build
(MSYS2 CLANG64). The platform-layer tests pass, `sacredfire.exe` links, and a Vulkan window and
present test works. Milestones M1 to M4 have not been reached.

## No game files included

This repository contains no game code, assets or data from the game. You need your own legally
obtained dump of the game with the 1.21 update merged in. This repository does not explain how
to obtain one.

## Disclaimer

ReFueled is not affiliated with or endorsed by Sony Interactive Entertainment, Activision or
Beenox. *Crash Team Racing*, *Nitro-Fueled* and *PlayStation* and all other trademarks belong
to their owners and are named here only to identify the game this software is meant to run.

## Building

Windows: use the MSYS2 CLANG64 shell and run

```bash
bash build-windows.sh            # platform layer, runtime, GPU library, sacredfire.exe
bash build-windows.sh --test     # also build and run the platform-layer tests
```

A detailed guide (`docs/refueled/BUILD-WINDOWS.md`) will follow.

Linux: the upstream way is unchanged, see [docs/SacredFire-README.md](docs/SacredFire-README.md)
(`bash build.sh`).

## Project layout

- `src/` - loader and runtime (from SacredFire); `src/platform/` is the platform layer with the
  Linux and Windows implementations
- `gpu/` - renderer library: vendored shadPS4 video core and shader recompiler, with changes
- `build-windows.sh`, `build.sh` - Windows (MSYS2 CLANG64) and Linux build scripts
- `tests/` - tests; `tests/refueled/` covers the platform layer
- `docs/refueled/` - plan, interfaces, audits and logs of this port (in German)
- `docs/SacredFire-README.md` - the README of SacredFire (only relative links adjusted)

## Credits and licenses

ReFueled is based on [SacredFire](https://github.com/T3CHNOLOG1C/SacredFire), the Linux port
of *Crash Team Racing Nitro-Fueled* whose loader, runtime and renderer integration it builds
on. The upstream credits follow.

SacredFire is licensed under the **GNU GPL v2 or later** ([LICENSE](LICENSE)): it contains code
from bbport and shadPS4 (both GPL-2.0-or-later). Third-party components keep their licenses:

- [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28 (the loader, runtime
  and renderer this builds on)
- [shadPS4](https://github.com/shadps4-emu/shadPS4) video core and shader recompiler
- [sirit](https://github.com/shadps4-emu/sirit)
- [half](https://half.sourceforge.net/)
- [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) by FireBurn (MIT)
- AMD FidelityFX SDK (MIT)
- [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) (MIT)
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT)
- DejaVu fonts
- [miniz](https://github.com/richgel999/miniz) (MIT)
- Game patches by illusion and TroyWarez
  ([illusionyy/PS-Game-Patch](https://github.com/illusionyy/PS-Game-Patch))
- Import names from shadPS4's aerolib list

## License

GPL-2.0-or-later, see [LICENSE](LICENSE). Third-party components keep their own licenses, as
listed under Credits.

---

SOOOOOOOOOOOOOOOOOOOOOOON TM
