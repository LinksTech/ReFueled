# Paket-Log F3b (Sonnet): build-windows.sh baut sacredfire.exe

Status: **ungetestet bis CLANG64** (nicht installiert). Geprueft nur: `bash -n build-windows.sh`
gruen; `sacredfire.manifest` ist wohlgeformtes XML (PowerShell `[xml]`).

## Dateien
`build-windows.sh`, `src/platform/WIN32_SOURCES.txt` (20 Eintraege: probe.c, 18x runtime*.c,
vulkan_smoke.c, sortiert), `packaging/windows/sacredfire.manifest`, `packaging/windows/sacredfire.rc`.
Keine Linux-wirksame Stelle geaendert (build.sh unberuehrt), daher keine Alt/Neu-Tabelle noetig.

## Stufen (Ausgabe in out/win/)
1. libpf.a: `pf_*_win32.c`, `pf_*_win32.S`, `pf_common.c`, `pf_lowheap.c` (Glob; `.S` und
   `pf_posix_gap_win32.c` sind damit automatisch dabei). Flags wie F3.
2. Gemeinsame Quellen aus WIN32_SOURCES.txt nach `out/win/obj/common/*.o` (NICHT mehr in libpf.a,
   Abweichung von F3, wo sie ins Archiv gingen); Flags wie libpf plus `pkg-config --cflags vulkan sdl3`.
3. libatrac9.a: `-std=c99 -O2 -g -gcodeview -w`, ohne Gap-Include, inkrementell wie build.sh
   (`-gcodeview` zusaetzlich, damit der PDB die Symbole kennt).
4. GPU: `tools/deps.sh` nur wenn `vk_mem_alloc.h` fehlt, `tools/deps-windows.sh` immer
   (idempotent), Submodule wie build.sh, `fsr_patches_apply` wortgleich, dann
   `env -u VULKAN_SDK CMAKE_PREFIX_PATH=out/deps/prefix cmake -S gpu -B out/win/gpu -G Ninja
   -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO=off -DBB_LTO=${BB_LTO:-OFF}
   -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++` und `ninja -C out/win/gpu bbgpu`
   (Log `out/win/gpu-build.log`, bei Fehler letzte 40 Zeilen ohne Fortschrittszeilen).
5. `llvm-windres` (im Verzeichnis packaging/windows, damit `1 24 "sacredfire.manifest"` relativ
   aufgeloest wird) -> `out/win/sacredfire.res.o`.
6. Link mit `clang++`: Objekte aus 2, libpf.a, libatrac9.a, Ressource, Archivgruppe, dann
   `pkg-config --libs vulkan sdl3`, `-lpthread -lm`; `-fuse-ld=lld --disable-high-entropy-va
   --disable-dynamicbase --pdb=out/win/sacredfire.pdb` -> sacredfire.exe; danach out/win/VERSION
   wie build.sh.
7. `--test`: nur tests/refueled/test_*.c gegen libpf.a (wie F3).
8. `--no-gpu` ueberspringt 4 und 6. Zusaetzlich `--skip-libpf|common|atrac9|gpu|resource|link`.

## Entscheidungen / Risiken (bitte pruefen)
- **Statische Abhaengigkeiten von libbbgpu.a:** CMake buendelt sie nicht. Das Skript legt alle
  `*.a` aus `out/win/gpu` (sirit, FSR-Vulkan, ...) und `out/deps/prefix/lib` (miniz) in eine
  `--start-group`-Gruppe, ergaenzt `-lZydis -lZycore` falls kein Zydis-Archiv darunter ist, und
  haengt `pkg-config --libs fmt libxxhash libavformat libavcodec libavutil libswscale
  libswresample` an (die Pakete, die gpu/CMakeLists.txt per find_package/pkg_check_modules sucht).
  Das ist geraten, nicht geprueft; ein sauberer Weg waere ein CMake-Ziel `sacredfire` in
  gpu/CMakeLists.txt (gehoert nicht zu diesem Paket). Fehlende Symbole beim ersten Lauf
  wahrscheinlich hier (Boost/asio ist header-only; `shell32 ws2_32 mswsock` fehlen in der Gruppe
  und muessten evtl. ergaenzt werden: sie reisen nur ueber CMake-Usage-Requirements).
- `X11` entfaellt unter Windows (leeres Stand-in in CMake).
- Die Common-Objekte entstehen mit `-Werror`; Warnungen in runtime*.c unter MinGW (LLP64) fuehren
  zum Abbruch der Stufe 2 (gewollt, Fix-Runde).
- `deps.sh` braucht Netz (holt Header-Pakete); laeuft nur beim ersten Mal.

## Offen
- Abnahme `bash build-windows.sh [--no-gpu]` und mit GPU in CLANG64 offen.

## Review Opus
Nur Windows-Dateien, Linux unberührt. Offen für Welle B: Linkzeile der statischen bbgpu-Abhängigkeiten aus der CMake-Konfiguration übernehmen (geratene Liste ersetzen) bzw. eigenes Exe-Ziel in gpu/CMakeLists.txt, falls einfacher.
