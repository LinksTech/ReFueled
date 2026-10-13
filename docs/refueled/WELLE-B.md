# ReFueled – Welle B: Start, sobald MSYS2 CLANG64 installiert ist

Stand 2026-10-13. Auslöser: `C:\msys64\clang64\bin\clang.exe`, `ninja.exe` und `sdl3.pc` existieren
(Hintergrund-Wächter). Ablauf ohne Rückfrage (Planänderung 2026-10-13, Punkt 3). Vorgabe Projektleitung
2026-10-13: Compile- und Linkfehler des ersten CLANG64-Builds **nach Paket/Datei-Besitzer gruppieren
und parallel an Sonnet verteilen, nicht seriell**; kritischer Pfad zuerst; nach dem ersten
vollständigen Build kurzer Bericht mit Anzahl Fehler je Paket vorher/nachher.

## 1. Toolchain-Check (Opus, wenige Minuten)
In der CLANG64-Shell (`C:\msys64\usr\bin\bash.exe -lc` mit `MSYSTEM=CLANG64`, TMP/TEMP auf
`C:\CTR Reload\tmp\refueled\B`): `clang --version`, `pkg-config --modversion vulkan sdl3`,
`cmake --version`, `ninja --version`, `llvm-windres --version`, `python3 --version`. Fehlendes
Paket → der Projektleitung melden, Rest läuft weiter.

## 2. Zähl-Build "vorher" (Opus, ein Lauf)
`bash build-windows.sh --keep-going --test` übersetzt **jede** Einheit auch nach Fehlern
(`-ferror-limit=0`, GPU-Bibliothek mit `ninja -k 0`), legt je Fehlschlag ein Log in
`out/win/census/` ab und gruppiert alles mit `docs/refueled/tools/census_group.py` nach
`docs/refueled/tools/owners.txt` in `out/win/census/summary.md` / `summary.tsv`. Der Link läuft erst,
wenn alle Objekte bauen. Der Stand dieses Laufs ist die Spalte "vorher" im Bericht
(`docs/refueled/log/WELLE-B.md`). Ein Fehler in einem gemeinsamen Header zählt beim Besitzer des
Headers, nicht bei der Einheit, die ihn einbindet.

## 3. Verteilung (parallel, Priorität kritischer Pfad)
Je Besitzer-Gruppe mit Fehlern ein Sonnet-Agent, alle gleichzeitig gestartet, jeder in seinem
Worktree `C:\nitro-ps4-wt\<ID>-B` auf Branch `wt/<ID>-B` und nur mit seinen Dateien laut
owners.txt. Er bekommt seinen Abschnitt aus summary.md, repariert, übersetzt seine Einheiten mit
`build-windows.sh --keep-going` neu, bis seine Gruppe 0 Fehler hat, und liefert Diff + Log mit
alt↔neu-Tabelle für jede Linux-wirksame Zeile. Startreihenfolge innerhalb derselben Nachricht:

| Prio | Gruppen | Warum |
|---|---|---|
| 1 | IF-Opus, K3-Opus, IF7-Opus (**Opus selbst**, parallel zu den Agenten) | gemeinsame Header blockieren alle; Loader |
| 1 | F5-toolchain/F5 (CMake-Konfiguration), G2a, G2b, G3a, G3b, G3c, V1–V3 | GPU-Bibliothek, ohne die es keine sacredfire.exe und kein M1 gibt |
| 1 | F3b (Link sacredfire.exe, vulkan_smoke), F6 (Gap-Schicht) | Link und POSIX-Lücken aller Runtime-Dateien |
| 2 | K2, K5, R1, R2c, R4, R5 | Runtime-Objekte der Exe |
| 3 | K1, K4, K6, K7, LH, R2, R3, F2, F7 (Plattformtests) | Abnahme der Plattformschicht |
| 4 | T1 | bestehende Tests |

Regeln: Abbruchkriterium D1 gilt je GPU-Teilpaket (zwei Fix-Runden; danach nur die GPU-Lib auf
clang-cl). Ein Fix, der einen Header oder eine Datei außerhalb der eigenen Gruppe braucht, geht als
Meldung an Opus, nicht als Änderung. Gruppen mit sehr wenigen Fehlern (≤ 5) und gleichem Thema
dürfen in einem Agenten zusammengefasst werden; Gruppen > 150 Fehler werden nach Unterordnern
geteilt. Fertige Agenten nehmen sofort die nächste offene Gruppe (fortsetzen statt neu starten).
Opus mergt in Prioritätsreihenfolge, prüft jede Linux-wirksame Zeile und lässt danach
`build-windows.sh --keep-going --test` erneut laufen; neue Fehler gehen wieder nach Besitzer raus.
Änderungen in `gpu/shadps4` minimal, jede Zeile `// refueled:`.

## 4. Erster vollständiger Build
Erreicht, wenn `build-windows.sh --test` ohne `--keep-going` grün ist und `out/win/sacredfire.exe`
gelinkt ist. Dann Bericht an die Projektleitung (kurz): Tabelle "Paket | Fehler vorher | Fehler nachher |
ausgeführt von", Anzahl Fix-Runden, Abweichungen von D1, Testergebnis. Danach
`sacredfire.exe --vulkan-only` (Vulkan-Rauchtest, Abbruchkriterium D1 "Absturz beim ersten
Vulkan-Init").

## 5. Danach
- T1: bestehende `tests/test_*.c` unter Windows (test_pad: setenv/mkstemp/TMP-Pfade).
- M1 (sobald Dump 1.21 und `game_check.py` sauber): `run-windows.sh` bis `--list-imports`;
  Belege: "Mapped … bytes" mit Image bei 0x800000000, Import-Zusammenfassung gleich der
  Linux-Referenz (Referenzzahlen aus einem früheren Linux-Lauf bzw. CI).

## Anhang: GPU-Teilpakete (audit/GPU-MINGW.md §5)

| ID | Ordner (besessen) | Erwartung | Umfang |
|---|---|---|---|
| G2a | `gpu/shadps4/common/**` | path_util (shlobj-Guard), ntapi.h-Typkollisionen | S |
| G2b | `gpu/shadps4/shader_recompiler/**` | kaum Plattformcode; Risiko fmt 12 / boost 1.92 / libc++ | S–M |
| G3a | `gpu/shadps4/core/**`, `video_core/**` ohne renderer_vulkan und ohne V-Dateien | wenig | S |
| G3b | `gpu/shadps4/video_core/renderer_vulkan/**` (ohne V2/V3-Dateien) | viel Code, wenig Plattform | M |
| G3c | `gpu/shim/**` (ohne V3-Datei) + `fault_manager.cpp` | Shim-C++ unter MinGW | S–M |
