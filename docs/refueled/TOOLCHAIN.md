# ReFueled – Toolchain: Bestand und was fehlt

Stand 2026-10-12. Bestandsaufnahme nur lesend, nichts installiert. Paketnamen und Versionen
stammen von packages.msys2.org (Stand 2026-10-12).

## 1. Vorhanden

| Werkzeug | Version | Pfad / Bemerkung |
|---|---|---|
| git | 2.55.0.windows.3 | C:\Program Files\Git |
| CMake | 4.4.1 | C:\Program Files\CMake (dazu VS-Kopie 3.31.6) |
| Ninja | 1.12.1 | nur VS-Kopie, nicht im PATH |
| Python | 3.13.15 (+ uv 3.14.7) | `python3` ist nur der Store-Stub; `python` funktioniert |
| Visual Studio Build Tools 2022 | 17.14.37, MSVC 14.44, Windows SDK 10.0.26100 | **ohne** Clang-Komponente |
| Vulkan SDK | 1.3.296.0 (aktiv) und 1.4.350.0 | glslangValidator, glslc, spirv-opt, dxc vorhanden |
| WSL2 | Ubuntu 24.04, gcc 13.3, Python 3.12 | **ohne** cmake, pkg-config, Vulkan-Header, SDL3 |
| Hardware | Ryzen 9 9900X (24 Threads), 31 GB RAM, 1,3 TB frei; RTX 5080 (Vulkan 1.4) + AMD iGPU | `LongPathsEnabled = 0` |

## 2. Fehlt für Option A (empfohlen, ARCHITECTURE.md D1)

**MSYS2**: Installer `msys2-x86_64-20260927.exe`
https://github.com/msys2/msys2-installer/releases/download/2026-09-27/msys2-x86_64-20260927.exe
(Seite: https://www.msys2.org/). Zielordner ist deine Entscheidung (Standard `C:\msys64`).

Danach in der **CLANG64**-Shell (eine Zeile):

```
pacman -S --needed mingw-w64-clang-x86_64-toolchain mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja mingw-w64-clang-x86_64-pkgconf mingw-w64-clang-x86_64-python mingw-w64-clang-x86_64-vulkan-headers mingw-w64-clang-x86_64-vulkan-loader mingw-w64-clang-x86_64-glslang mingw-w64-clang-x86_64-spirv-headers mingw-w64-clang-x86_64-spirv-tools mingw-w64-clang-x86_64-sdl3 mingw-w64-clang-x86_64-fmt mingw-w64-clang-x86_64-boost mingw-w64-clang-x86_64-robin-map mingw-w64-clang-x86_64-vulkan-memory-allocator mingw-w64-clang-x86_64-xxhash mingw-w64-clang-x86_64-zydis mingw-w64-clang-x86_64-ffmpeg mingw-w64-clang-x86_64-lldb git
```

| Paket | Version (2026-10-12) | Zweck |
|---|---|---|
| toolchain (clang, lld, libc++, winpthreads, …) | LLVM 23.1.3 | Compiler, Linker, C++-Bibliothek, pthreads |
| cmake / ninja / pkgconf | 4.4.4 / 1.13.2 / 3.0.7 | Build wie build.sh |
| python | 3.14.8 | Skripte (prepare, link, patches) in der MSYS2-Bash |
| vulkan-headers / vulkan-loader | 1.4.363 | Vulkan |
| glslang / spirv-headers / spirv-tools | 16.6.0 / 1.4.363 | Host-Shader, sirit |
| sdl3 | 3.4.18 | Fenster, Audio, Controller |
| fmt / boost / robin-map / vulkan-memory-allocator / xxhash / zydis / ffmpeg | 12.2.0 / 1.92.0 / 1.4.1 / 3.4.0 / 0.8.4 / 4.1.1 / 9.0.2 | GPU-Bibliothek (gpu/CMakeLists.txt) |
| lldb (oder gdb 18.1) | 23.1.3 | Debugger |

**Nicht in MSYS2**, kommen aus den Quellen über `tools/deps.sh` (wie heute auf Linux):
magic_enum (deps.sh holt v0.9.7), xbyak (deps.sh holt v7.30), **miniz** (neu, über ein eigenes
`tools/deps-windows.sh`, damit `deps.sh` für Linux unverändert bleibt; Paket F5).
Achtung: Die MSYS2-Versionen liegen über denen, mit denen bbport auf Linux baut (nixpkgs), z. B.
fmt 12 und boost 1.92. Brechen sie den GPU-Build, legt F5 Versionen fest (FetchContent).

## 3. Fehlt für die Linux-Regression in WSL (D15)

Vorschlag: **Nix single-user** in der vorhandenen Ubuntu-WSL. `build.sh` startet dann selbst
`nix-shell shell.nix`, das entspricht genau der Upstream-Umgebung.
Anleitung: https://nixos.org/manual/nix/stable/installation/single-user, Befehl laut
https://nixos.org/download/: `curl --proto '=https' --tlsv1.2 -L https://nixos.org/nix/install | sh -s -- --no-daemon`
(fragt per sudo nach `/nix`). Der erste `nix-shell`-Lauf lädt einige GB in `/nix` in der WSL,
nicht auf C:.
Alternative ohne Nix: `apt install cmake ninja-build pkg-config libvulkan-dev glslang-tools libboost-dev libfmt-dev libxxhash-dev libzydis-dev libavformat-dev libswscale-dev libswresample-dev libx11-dev …`.
SDL3 gibt es für Ubuntu 24.04 nicht im Archiv (erst ab 26.04), also aus den Quellen bauen.

Hinweis Pfadregel: Die WSL-Pakete landen im WSL-Dateisystem (ext4-Image unter
`%LOCALAPPDATA%\Packages\…`), also außerhalb von `C:\nitro-ps4-*`. Das ist bei jeder WSL-Nutzung so.

## 4. Nur für Option B (falls doch gewünscht)

- LLVM 23.1.3 für Windows: `LLVM-23.1.3-win64.msi`, https://github.com/llvm/llvm-project/releases/latest
  (oder in den VS Build Tools die Komponente "C++ Clang Compiler for Windows" nachinstallieren)
- SDL3 3.4.18: `SDL3-devel-3.4.18-VC.zip`, https://github.com/libsdl-org/SDL/releases/tag/release-3.4.18
- vcpkg: https://github.com/microsoft/vcpkg (fmt, boost, robin-map, xxhash, zydis, ffmpeg, miniz, magic-enum, vulkan-memory-allocator)

## 5. Empfohlene Systemeinstellung (optional)

- `LongPathsEnabled = 1` (Registry, Admin), damit tiefe Build-Pfade (`out/gpu/CMakeFiles/...`)
  nicht an 260 Zeichen scheitern. Ohne die Einstellung halten wir die Pfade kurz.
