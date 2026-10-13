# B-SHIM (Teilpakete G3c / S3)

Verhaltensabweichung Linux: keine (wait_trace: gleiche Feldgroesse 10 und gleiche Puffergroesse 11, nur als benannte Konstante statt std::size; Overlay: der geaenderte Text steht im `#ifdef _WIN32`-Zweig, der Linux-Zweig ist textgleich)

## alt <-> neu

| alt (Datei:Zeile, Code) | neu (Datei:Zeile, Code) | Linux-Gleichheit |
|---|---|---|
| gpu/shim/bbport_wait_trace.h:20 `void* frames[10];` | bbport_wait_trace.h:20-22 `static constexpr int MaxFrames = 10; void* frames[MaxFrames];` | Feldgroesse unveraendert 10; statische Konstante ohne Laufzeitwirkung |
| gpu/shim/bbport_wait_trace.cpp:79 `void* all[std::size(frames) + 1];` | bbport_wait_trace.cpp:79 `void* all[MaxFrames + 1];` | 10 + 1 = 11 Elemente wie vorher; Ursache: clang wertet `std::size(frames)` (Member im Konstruktor) als nicht konstant aus (VLA-Typ, "variably modified type"), gcc akzeptierte es |
| gpu/shim/bbport_overlay.cpp:25 `.section .rdata,"dr"` ... :32 `.previous` (nur `#ifdef _WIN32`) | `.pushsection .rdata,"dr"` ... `.popsection` | nur Windows-Zweig; Linux-Zweig (Zeilen 34-45) unveraendert |

S3-Ursache: der LLVM-COFF-Assembler kennt `.previous` nicht ("unknown directive"); `.pushsection/.popsection` sind generisch und stellen die vorherige Sektion wieder her.

## Fehlerzahl (Shim-Einheiten)

| Stand | Fehler in gpu/shim/** | Einheiten gescheitert |
|---|---|---|
| vorher (Zaehl-Build) | 3 eigene (wait_trace 1, overlay 1, window 1) + bbgpu.cpp (fremd) | 4 |
| nachher | 0 eigene; offen: window.cpp strcasestr (B-GAP), bbgpu.cpp (texture_cache.h/atomic_ref, fremd) | 2 |

- window.cpp: mit `-Dstrcasestr=strstr` nur im Testlauf (nicht im Code) kompiliert fehlerfrei (rc=0). Sonst keine weiteren Fehler.
- bbgpu.cpp: bestaetigt, kein eigener Fehler; scheitert an libc++ `__atomic/atomic_ref.h:152` ("cannot initialize 'unsigned int *' with 'const unsigned int *'", ausgeloest durch texture_cache.h).
- Alle uebrigen Einheiten unter gpu/shim/** (inkl. common/assert.cpp) bauen. Gesamt-GPU-Bibliothek: 78 -> 76 Fehler (-2 eigene; der Rest in fremden Paketen).

## Befehle

```
MSYSTEM=CLANG64 C:\msys64\usr\bin\bash.exe -lc "cd /c/nitro-ps4-wt/B-SHIM && TMP/TEMP=C:\CTR Reload\tmp\refueled\B-SHIM bash build-windows.sh --keep-going --skip-libpf --skip-common --skip-atrac9 --skip-resource --skip-link"
ninja -C out/win/gpu -k 0   (Log out/win/census/gpu-build2.log)
window.cpp-Test: ninja -t commands <obj> mit -Dstrcasestr=strstr, Ausgabe nach out/win/gpu (danach geloescht)
```
Hinweis: build-windows.sh setzt gpu/third_party/fsr-vulkan zurueck und patcht neu (git zeigt dort `m`); nicht von mir geaendert.
