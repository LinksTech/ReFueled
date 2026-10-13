# Welle B – erster CLANG64-Build (2026-10-13)

Verhaltensabweichung Linux: keine (alle Fix-Pakete einzeln geprüft, siehe deren Logs)

Toolchain: MSYS2 (winget, C:\msys64), clang 23.1.3, cmake 4.4.4, ninja 1.13.2, Vulkan 1.4.363, SDL3 3.4.18,
Python 3.14.8 – wie TOOLCHAIN.md §2. Zähl-Build `build-windows.sh --keep-going --test` (vorher: 83406d5~,
Stand nach Welle A + DBG ungemergt), nachher `build-windows.sh --test` ohne `--keep-going` grün (170eddc).

## Fehler je Paket (Zähl-Build vorher → nachher)

| Paket (owners.txt) | Fehler vorher | Einheiten vorher | nachher | behoben durch | ausgeführt von |
|---|---|---|---|---|---|
| F5 (fsr-vulkan, sirit) | 56 | 24 | 0 | erzeugter Kompat-Header per `-include`, nur WIN32 | B-F5, Sonnet |
| F5-toolchain (libc++-Header) | 16 | 0 | 0 | Ursache texture_cache.h `atomic_ref<const>` (G3a) | B-GPU, Sonnet |
| G3a / G3b | 0 | 3 / 6 | 0 | an texture_cache.h und `PipelineCompiler` gescheitert | B-GPU, Sonnet |
| V1 | 0 | 1 | 0 | wie G3a | B-GPU, Sonnet |
| V2 | 3 | 5 | 0 | bitset `_Find_*` (libc++-Zweig), `GetGpuCommandProcessorThreadId` | B-GPU, Sonnet |
| G3c | 2 | 4 | 0 | wait_trace `MaxFrames`; window.cpp Gap-Include | B-SHIM, Sonnet; Opus |
| Opus-unbekannt (Overlay-Asm) | 1 | 0 | 0 | `.pushsection/.popsection` statt `.previous` (COFF) | B-SHIM, Sonnet |
| R5 | 37 | 8 | 0 | `-D_GNU_SOURCE=` (6); rand_s über Gap; runtime_mutex-Wand; json2-Atomic | Opus; B-GAP, B-RT (Sonnet) |
| R1 | 7 | 1 | 0 | `-D_GNU_SOURCE=`, `-Wno-unused-but-set-global`, Atomics | Opus; B-RT, Sonnet |
| F6 | 6 | 1 | 0 | gmtime_r/localtime_r in der Gap-Schicht | B-GAP, Sonnet |
| K5 | 2 | 1 | 0 | `-D_GNU_SOURCE=`, Atomic | Opus; B-RT, Sonnet |
| R2c | 2 | 2 | 0 | `-D_GNU_SOURCE=` | Opus |
| R4 | 1 | 1 | 0 | strcasestr in der Gap-Schicht | B-GAP, Sonnet |
| LH | 1 | 1 | 0 | test_lowheap unter Windows mit pf_vm_alloc_fixed | B-RT, Sonnet |
| K2, K3-Opus, K7, IF7-Opus | 1 / 1 / 1 / 1 | 1 / 1 / 1 / 2 | 0 | `_GNU_SOURCE`, probe.c-Guard, PfContext-Init im Test, `_Thread_local` | Opus |
| K6 (test_thread, Laufzeit) | 0 | 1 | 0 | Test legte den TLS-Slot nicht an; CPU-Zeit-Prüfung gegen 15,6-ms-Takt | Opus |
| link (test_time) | 2 | 0 | 0 | Folge von IF7 | Opus |
| link sacredfire.exe (neu nach Runde 1) | 3 | – | 0 | shell32/ws2_32/mswsock, kernel32 vor mincore (CreateFileMapping2) | Opus |
| **gesamt** | **140** (+3 Link) | **64** | **0** | | |

Plattformtests: vorher 6 von 11 grün, nachher 11 von 11 (`tests: 11 run, 0 failed`).
Fix-Runden: eine Runde mit 5 parallelen Sonnet-Paketen (B-F5, B-GPU, B-SHIM, B-GAP, B-RT) plus Opus;
Abbruchkriterium D1 nicht berührt (GPU-Bibliothek in einer Runde übersetzbar).

## Vulkan-Rauchtest
`sacredfire.exe --vulkan-only`: `Vulkan: NVIDIA GeForce RTX 5080; command submission + 4096-byte readback PASS`,
rc=0. D1 (Absturz beim ersten Vulkan-Init) nicht eingetreten: CLANG64 bleibt.

## Offen
- M1 wartet auf Dump 1.21 (`game_check.py` meldet für 1.00 missing_update).
- T1 (bestehende tests/test_*.c) und M3-Strang (Fenster, Vulkan-Init mit Testbild) laufen als nächste Pakete.
- Linkzeile: Systembibliotheken jetzt von Hand; besser aus der CMake-Konfiguration (eigenes Exe-Ziel), wenn
  weitere Bibliotheken hinzukommen.
