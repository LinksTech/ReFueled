# B-F5: Drittanbieter-Code (fsr-vulkan, sirit) unter clang/libc++

Verhaltensabweichung Linux: keine (alles hinter `if(WIN32)`; keine bestehende Zeile geändert, nur Einfügung nach Zeile 81)

## Änderung

Datei: `gpu/CMakeLists.txt` (nur WIN32). `tools/deps-windows.sh` unverändert.
Im Build-Baum wird `refueled_thirdparty_compat.h` erzeugt (`file(WRITE)`) und per `-include` (nur C++) auf alle
kompilierten Ziele der Verzeichnisse `third_party/sirit/src` und `third_party/fsr-vulkan` gelegt. Inhalt:
`<cmath> <cstdarg> <cstddef> <cstdlib> <cwchar> <mutex> <new>`, `<wchar.h>` (deklariert MinGWs `swprintf_s`, es
braucht keinen eigenen Ersatz) und `#define _countof(a) (sizeof(a) / sizeof((a)[0]))`.
Unter `gpu/third_party/**` und `gpu/shadps4/**` nichts geändert.

| alt (Datei:Zeile, Code) | neu (Datei:Zeile, Code) | Linux-Gleichheit |
|---|---|---|
| `gpu/CMakeLists.txt:81` `add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)` (unverändert) | Zeilen 82-111 neuer `if(WIN32) ... endif()`-Block direkt danach | Block wird unter Linux nicht ausgeführt; alle übrigen Zeilen textgleich (git diff: nur Einfügungen) |

## Fehler der Gruppe (fsr-vulkan + sirit)

Vorher 56 Fehler gesamt / 24 Einheiten in der Gruppe (Zähl-Build laut Auftrag); nach Fix: 0 Fehler, Gruppe `F5` in
summary.md leer. Gebaut: libsirit.a, libffx_vulkan_portable.a, _fsr3_host_1_1_4.a, _fsr3_vk_backend_1_1_4.a,
_fsr4_v07_vulkan.a, _fsr4_v07_assets.a. Gesamt im GPU-Build: 22 Fehler, alle in fremden Gruppen.

## Befehl

`MSYSTEM=CLANG64 TMP=TEMP=C:\CTR Reload\tmp\refueled\B-F5 C:\msys64\usr\bin\bash.exe -lc "cd /c/nitro-ps4-wt/B-F5 && bash build-windows.sh --keep-going --skip-libpf --skip-common --skip-atrac9 --skip-resource --skip-link"`

## Nicht meine Fehler (offen)

F5-toolchain 16 (libc++ `atomic_ref<const unsigned>` in texture_cache.h:152), G3c 2 (vk_rasterizer.cpp:915/916
`_Find_first/_Find_next`), V2 3 (u. a. vk_rasterizer.cpp:752, shim/window.cpp:38 `strcasestr`,
shim/bbport_wait_trace.cpp:80 `size`), 1x `<inline asm>:8 unknown directive`.
