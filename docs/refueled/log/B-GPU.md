# B-GPU – Fix-Paket Welle B (G3a / G3b / V2)

Ausgefuehrt von Sonnet, zu pruefen von Opus. Ziel: video_core/** und core/** kompilieren mit clang 23 + libc++ (CLANG64).

Verhaltensabweichung Linux: keine

## Fehlerzahl (Zaehl-Build, out/win/census/summary.md)

| Paket | vorher | nachher |
|---|---|---|
| G3a | 0 Fehler, 3 Einheiten fehlgeschlagen | 0 / 0 |
| G3b | 0 Fehler, 6 Einheiten fehlgeschlagen | 0 / 0 |
| V2 | 3 Fehler, 5 Einheiten fehlgeschlagen | 0 / 0 |

Fehler gesamt 78 -> 59 (Rest: F5 = fsr-vulkan/sirit 56, G3c = gpu/shim 2 (window.cpp strcasestr, bbport_wait_trace.cpp size), Opus-unbekannt 1 (`<inline asm>: unknown directive`, bbport_overlay.cpp)). Alle Einheiten unter gpu/shadps4/video_core/** und core/** uebersetzen; renderdoc.cpp ist nicht im CMake-Ziel. Eine Fix-Runde reichte.

## Aenderungen (alt <-> neu, Linux-Gleichheit)

| Stelle | alt | neu | Linux-Gleichheit |
|---|---|---|---|
| texture_cache/texture_cache.h:152 (IsUpToDate) | `std::atomic_ref<const u32>(reinterpret_cast<const u32&>(image.flags)).load(acquire)` | `std::atomic_ref<u32>(const_cast<u32&>(reinterpret_cast<const u32&>(image.flags))).load(acquire)` | gleiches Objekt, gleiche acquire-Ladung, nur const abgeworfen (es wird nur geladen); gleicher Maschinencode |
| renderer_vulkan/vk_rasterizer.cpp:~914 | `for (block = blocks._Find_first(); block < size(); block = blocks._Find_next(block))` | unter `#if defined(_LIBCPP_VERSION)`: Schleife ueber `test(block)`; `#else` die Originalzeilen unveraendert | Linux nimmt den `#else`-Zweig, unveraendert; libc++-Ersatz liefert dieselben gesetzten Bits aufsteigend |
| amdgpu/liverpool.h:~198 | `GetGpuCommandProcessorThreadId()` nur `#ifdef __linux__` | fuer alle Plattformen deklariert, gleicher Rumpf `return gpu_tid;` | Linux: gleiche Funktion; |
| amdgpu/liverpool.h:~346 | `u32 gpu_tid;` nur Linux | `u32 gpu_tid{};` alle Plattformen | Linux: nur Nullinitialisierung eines vor Process() ohnehin nicht gelesenen Members (Layout gleich) |
| amdgpu/liverpool.cpp:~133 | `#ifdef __linux__ gpu_tid = gettid(); #endif` | Linux-Zweig unveraendert, neuer `#else`: `gpu_tid = pf_thread_id()` | Windows-only Zusatz; pf_thread_id war in der Datei schon eingebunden |
| renderer_vulkan/vk_pipeline_cache.cpp | Klasse `PipelineCompiler` nach dem PipelineCache-Konstruktor | Klasse unveraendert vor den Konstruktor verschoben (Kommentarzeile `// refueled:`) | reine Umordnung innerhalb der Uebersetzungseinheit; libc++ braucht den vollstaendigen Typ fuer `unique_ptr` im Konstruktor (Aufraeumpfad) |

Hinweis zur gpu_tid-Entscheidung (fuer Opus): unter Windows ist `pf_thread_id()` die id, die auch page_manager.cpp an `IsGpuSideThreadId` uebergibt, falls dieser Pfad dort eine Thread-id derselben Art liefert; das ist im Paket V2/Fault-Handler zu bestaetigen (page_manager.cpp:595 `ptid`).

## Befehle

```
MSYSTEM=CLANG64 TMP/TEMP=C:\CTR Reload\tmp\refueled\B-GPU
C:\msys64\usr\bin\bash.exe -lc "cd /c/nitro-ps4-wt/B-GPU && bash build-windows.sh --keep-going --skip-libpf --skip-common --skip-atrac9 --skip-resource --skip-link"
```
