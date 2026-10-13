# ReFueled – Welle B: Start, sobald MSYS2 CLANG64 installiert ist

Stand 2026-10-13. Auslöser: `C:\msys64\clang64\bin\clang.exe` existiert und die `pacman`-Zeile aus
TOOLCHAIN.md §2 ist durch. Ablauf ohne Rückfrage (Planänderung 2026-10-13, Punkt 3).

## 1. Sofort (Opus, seriell, je wenige Minuten)
1. Toolchain-Check in der CLANG64-Shell (`C:\msys64\usr\bin\bash.exe -lc` mit `MSYSTEM=CLANG64`,
   TMP/TEMP auf `C:\CTR Reload\tmp\refueled\B`): `clang --version`, `pkg-config --modversion vulkan sdl3`,
   `cmake --version`, `ninja --version`, `llvm-windres --version`. Fehlendes Paket → der Projektleitung melden.
2. `bash build-windows.sh --no-gpu --test`: Plattformschicht + alle `tests/refueled/test_*.c`
   (Abnahme F3, F6, F7, K1, K4, K6, K7, LH, R2, R3, IF-7–IF-9 unter clang). Erwartete Fix-Runde:
   MinGW-Header-Unterschiede, die der MSVC-Zwischenlauf nicht sehen konnte (Gap-Schicht, .S/COFF,
   .seh_*-Direktiven, pthread-Typen). Fehler je Besitzer-Paket an den jeweiligen Agenten.
3. Runtime-Objekte (Stufe 2 von build-windows.sh): Kompilierfehler in K2/K3/K5/R1/R2c/R4/R5a je an
   den Besitzer; Opus repariert probe.c (K3) selbst.

## 2. Parallel, sobald Schritt 2 grün ist (Sonnet)
F5-Abnahme zuerst (ein Agent): `cmake -S gpu -B out/win/gpu …` laut build-windows.sh Stufe 4 bis
"configure ok". Dann G2/G3 in fünf Teilpaketen (audit/GPU-MINGW.md §5), jedes mit eigenem
Worktree und nur seinem Ordner; Abbruchkriterium D1 gilt je Teilpaket (zwei Fix-Runden):

| ID | Ordner (besessen) | Erwartung | Umfang |
|---|---|---|---|
| G2a | `gpu/shadps4/common/**` | path_util (shlobj-Guard), ntapi.h-Typkollisionen | S |
| G2b | `gpu/shadps4/shader_recompiler/**` | kaum Plattformcode; Risiko fmt 12 / boost 1.92 / libc++ | S–M |
| G3a | `gpu/shadps4/core/**`, `video_core/amdgpu/**` (ohne liverpool.cpp), `video_core/texture_cache/**`, `video_core/*.cpp` (ohne page_manager) | wenig | S |
| G3b | `gpu/shadps4/video_core/renderer_vulkan/**` (ohne V2/V3-Dateien) | viel Code, wenig Plattform | M |
| G3c | `gpu/shim/**` Kompilat (S1/S2/S3/V3-Dateien) + `fault_manager.cpp` | Shim-C++ unter MinGW | S–M |

Gemeinsame Regel: Änderungen in `gpu/shadps4` minimal, jede Zeile `// refueled:`; Fixes, die
mehrere Teilpakete betreffen (z. B. ein gemeinsamer Header), gehen an Opus.

## 3. Danach
- F3b: Link von `out/win/sacredfire.exe`; `sacredfire.exe --vulkan-only` (Vulkan-Rauchtest).
- T1: bestehende `tests/test_*.c` unter Windows (test_pad: setenv/mkstemp/TMP-Pfade).
- M1 (sobald Dump 1.21 und `game_check.py` sauber): `run-windows.sh` bis `--list-imports`;
  Belege: "Mapped … bytes" mit Image bei 0x800000000, Import-Zusammenfassung gleich der
  Linux-Referenz (Referenzzahlen aus einem früheren Linux-Lauf bzw. CI).
