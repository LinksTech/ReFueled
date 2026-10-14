# MO – virtuelles Mod-Overlay ohne Dateisystem-Links

Ausgeführt von Sonnet, geprüft von Opus (Prüfung und Commit stehen aus).

Verhaltensabweichung Linux: keine

(Begründung: Ohne die Umgebungsvariable `BB_MOD_MANIFEST` bleibt `overlay_count` 0 und jede neue Stelle in
`runtime_file.c` kehrt sofort zurück, bevor sie etwas liest oder ändert. `run.sh` und der Link-Baum von
`mods.py` sind unverändert; `mods.py` ohne `--manifest` gibt dieselben Meldungen und dasselbe Ergebnis. Der
Backslash-Fall der `..`-Prüfung steht unter `#ifdef _WIN32`. Einzige Randbedingung: Wer unter Linux selbst
`BB_MOD_MANIFEST` setzt, schaltet das Overlay ein; run.sh setzt sie nie. Linux wurde nicht gebaut, Regel
2026-10-13.)

## Dateien

| Datei | Änderung |
|---|---|
| `scripts/mods.py` | Auswahl in `collect_replacements` gezogen (gemeinsam für Link- und Manifest-Modus); neu `manifest_entries`, `write_manifest`, Option `--manifest FILE` |
| `src/runtime_file.c` | Overlay-Index aus `BB_MOD_MANIFEST`; Auflösung in `translate`; Vereinigung in Verzeichnislisten; Overlay-only-Verzeichnisse in open/stat/access; `..`-Prüfung mit `\` unter `_WIN32` |
| `run-windows.sh` | Mods-Sperre entfernt; `mods.py --manifest` und `BB_MOD_MANIFEST`, nur wenn Mods aktiv sind |
| `tests/test_file_mods.c` | Overlay-Tests (synthetisch) |
| `tests/refueled/test_mods_manifest.py` | neu, Python-Unittest für `--manifest` |

Nicht angefasst: `run.sh`, `src/runtime.h`, `src/platform/pf_*.h`, TASKS.md, `docs/refueled/MODLOADER.md`.

## Entwurf

**Manifest v1** (UTF-8, LF): Zeile 1 `# refueled mod manifest 1`; je Datei `<gastpfad>\t<hostpfad>`;
Gastpfad relativ zu `/app0` (also mit `dvdroot_ps4/`), `/`-getrennt, in der Schreibweise des Link-Baums
(Namen, die das Spiel in anderer Groß-/Kleinschreibung hat, übernehmen dessen Schreibweise; Namen, die es dort
nicht gibt, die des ersten Mods, der sie anlegt). Hostpfad absolut (Windows `C:\...`). Weitere `#`-Zeilen
sind Kommentare, Leerzeilen werden übergangen, Zeilen sind sortiert (deterministisch). Tab oder Zeilenumbruch in einem Pfad:
`mods.py` bricht mit `Mods: Tab or line break in a path (not possible in a manifest)` ab.

**mods.py**: `collect_replacements(mods)` ist der bisherige Auswahlteil von `build_overlay` (Schleife über
Mods, `mod_files`, casefold-Besitzer, Meldungen `Mods: X: N files`, `Mods: rel: A -> B`), unverändert in
Text und Reihenfolge. `build_overlay` ruft sie auf und macht danach genau das Bisherige. `--manifest` ruft
`write_manifest` statt `build_overlay`: kein `--out`-Ordner, keine Links, nichts auf stdout, am Ende dieselbe
Zählmeldung `Mods: R game files replaced, A added` auf stderr. `manifest_entries` bildet die
Namensauflösung des Link-Baums (`child`/`expand`) auf einem virtuellen Baum nach, inklusive
Datei/Verzeichnis-Konflikt-Fehlern.

**Runtime** (`BB_MOD_MANIFEST` gesetzt und nicht leer; sonst alles wie vorher):
- Laden einmal in `runtime_file_configure` (jeder weitere Aufruf lädt neu, Test). Fehler (Datei fehlt, falsche
  Kopfzeile, fehlender Tab, `..`/`.`/leere Komponente, absoluter Gastpfad, doppelter Gastpfad, Pfad ist zugleich
  Datei und Verzeichnis): Meldung `Runtime: mod manifest <datei>, line N: ...` auf stderr und `exit(1)`
  (Muster wie `Out of memory listing directory`).
- `translate`: Gastpfad unter `/app0` oder `/hostapp` wird normalisiert (doppelte `/`, `.`-Komponenten
  entfernt) und exakt im sortierten Index gesucht (bsearch); Treffer liefert den Hostpfad der Mod-Datei, sonst
  läuft die Mount-Auflösung wie bisher. Damit sehen `open`, `stat`, `access` und alle Fremdnutzer von
  `runtime_file_translate` das Overlay.
- Schreiben: `game_path()` wird vor `translate` geprüft; Anlegen/Schreiben/Truncate/Löschen/Umbenennen unter
  `/app0`, `/hostapp` (mit oder ohne Overlay-Treffer) liefert damit unverändert `-EROFS`.
- Verzeichnislisten: `list_directory` des Spielordners (falls es ihn gibt) wird mit den Kindern des Overlays
  vereinigt (`overlay_merge_dir`); gleiche Namen einmal, Typ des Overlays; Verzeichnisse nur im Overlay
  erscheinen als Typ 4.
- Verzeichnis nur im Overlay: open/stat/access nach ENOENT auf dem Spielordner nehmen den App0-Wurzelordner
  des Hosts als Stellvertreter (Deskriptor und stat zeigen ein Verzeichnis), die Liste kommt nur aus dem Overlay.
- `..`: unter `_WIN32` gelten `/` und `\` als Trenner der Prüfung (`PATH_SEP`).

**run-windows.sh**: Bedingung wie run.sh (Mods an, `BB_MODS_ENABLED` ungleich 0, und Mods-Ordner `${BB_MODS_DIR:-$data/mods}`
oder `<Spiel>-mods` vorhanden). Dann `mods.py ... --manifest "<out>\mods.manifest"`, `export BB_MOD_MANIFEST`,
Meldung `Mods: virtual overlay, BB_MOD_MANIFEST=...`. Sonst `unset BB_MOD_MANIFEST`. Löschschutz unverändert.

## alt <-> neu (Linux-wirksame Stellen)

Linux-wirksam sind `src/runtime_file.c` und `scripts/mods.py` (run.sh benutzt es). `pf_*_linux.c` wird nicht
berührt; die neuen Stellen rufen keine pf-Funktion anders auf als bisher.

| alt (Datei:Zeile, Code) | neu (Datei:Zeile, Code) | Linux-Gleichheit (Begründung) |
|---|---|---|
| runtime_file.c:186 (alt) `static void free_listing(...) {...}` hinter `list_directory`-Kopf | runtime_file.c:99, derselbe Text, nur nach oben verschoben | reine Verschiebung (wird vom Overlay-Code davor gebraucht), kein Textunterschied |
| runtime_file.c:101 `runtime_file_configure`: `snprintf(user_root..)`, Mounts | :290-292 zusätzlich `snprintf(app0_host,..)` und `overlay_load(getenv("BB_MOD_MANIFEST"))` | Ohne Variable: `getenv` liefert NULL, `overlay_load` gibt nach dem Leeren der (leeren) Tabelle zurück; app0_host wird nur vom Overlay gelesen. Sonst kein Aufruf, keine Ausgabe |
| runtime_file.c:121-122 `if ((p==buffer \|\| p[-1]=='/') && (p[2]==0 \|\| p[2]=='/')) return EACCES;` | :318 gleiche Bedingung mit `PATH_SEP(c)`; unter Linux `#define PATH_SEP(c) ((c)=='/')` (:309) | Makro ist ausgeschrieben identisch; `\` wird nur unter `_WIN32` Trenner |
| runtime_file.c:123-132 `pthread_mutex_lock; Mount-Suche; result; unlock` | :319-336: gleiche Zeilen in `if (!overlaid) {...}`; davor `if (overlay_count) {...}` | `overlay_count==0` ⇒ `overlaid` bleibt 0, der Block läuft mit gleicher Reihenfolge, gleichem Lock, gleichen Ergebnissen; Trace-Ausgabe danach unverändert |
| runtime_file.c:324 `char *commit=NULL, *temp=NULL, current[1024];` | :527 zusätzlich `int standin=0;` | lokale Variable |
| runtime_file.c:333-337 `fix_case`-Zweig nach `open` | :540-543 danach `if (host<0 && errno==ENOENT && overlay_standin(...))` | `overlay_standin` gibt bei `overlay_count==0` sofort 0 zurück (keine Systemaufrufe, errno unberührt); `standin` bleibt 0 |
| runtime_file.c:347 `if (!pf_fs_fstat(host,&s) && S_ISDIR(s.mode)) dir=list_directory(path);` | :554-557 gleicher Aufruf in Block, `dir=standin ? NULL : list_directory(path); if (overlay_count) dir=overlay_merge_dir(..)` | `standin==0`, `overlay_count==0` ⇒ `dir=list_directory(path)` wie vorher |
| runtime_file.c:505 `int r=pf_fs_stat(path,&s);` + `fix_case`-Zweig | :716 zusätzlich `if (r==-ENOENT && overlay_standin(...)) r=pf_fs_stat(...)` | wie oben: ohne Overlay keine Wirkung; Reihenfolge der übrigen Zeilen gleich |
| runtime_file.c:580 `int r=access(path,mode&7) ? -errno : 0;` + `fix_case`-Zweig | :792 zusätzlich `if (r==-ENOENT && overlay_standin(...)) r=access(...)` | wie oben |
| mods.py:123-134 (alt) Schleife in `build_overlay` (`replacements`, `owners`, Meldungen) | mods.py:120-135 `collect_replacements(mods)`, :200 `replacements = collect_replacements(mods)` | gleicher Code, gleiche Reihenfolge der stderr-Meldungen, vor `if not replacements: return game`; `game.resolve(strict=True)` weiter zuerst |
| mods.py `main`: `print(build_overlay(args.game, args.out, layers))` | :253-256 `if args.manifest: write_manifest(...) else: print(build_overlay(...))` | ohne `--manifest` (run.sh) derselbe Aufruf |
| mods.py `parser` | :242 neue Option `--manifest` | ohne sie unverändert; Pflichtoptionen gleich |

## Tests (CLANG64, Windows)

Build: `bash build-windows.sh --no-gpu --test --skip-gpu` (Weg aus WS1.md: `out/win/gpu` aus dem Hauptrepo kopiert;
zusätzlich `third_party/LibAtrac9/` aus dem Hauptrepo kopiert, weil der Worktree-Submodulordner leer war; die
`.git`-Datei darin wurde wieder entfernt). Log: `C:\CTR Reload\tmp\refueled\MO\build.log`.

```
PASS test_posix_subset
Guest mod files: reads, stat, merged listing, readonly assets and writable saves, virtual overlay PASS
PASS test_file_mods
tests (build.sh set): 6 run, 3 failed
```
Die 3 Fehlschläge sind `test_runtime`, `test_sema`, `test_eventflag`: Linker-Fehler `undefined symbol: mz_uncompress`
(`gpu/shim/window.cpp` der kopierten GPU-Bibliothek aus dem Hauptrepo, miniz fehlt beim Linken). Sie
liegen am Aufbau der kopierten Bibliothek, nicht an dieser Änderung (kein Test-Code von ihnen wurde berührt);
sie sind bisher unabhängig davon nicht grün zu bekommen gewesen (vor meinem Overlay-Code
gleicher Fehler im ersten Lauf).

`test_file_mods` deckt ab: Datei ersetzt, neue Datei, neues Verzeichnis nur im Overlay (und darin Datei),
normalisierte Pfade (`//`, `./`), `/hostapp` und relative Pfade, `stat` und `access` aus dem Overlay, Vereinigung
in drei Verzeichnislisten ohne Duplikate (`.`/`..` des Windows-readdir werden ignoriert), Schreiben/Anlegen/Truncate/
Unlink/Rename/mkdir/rmdir auf Overlay-Treffer und Overlay-Verzeichnis gleich `-EROFS`, ohne `BB_MOD_MANIFEST`
(vorher und nachher) das alte Verhalten, `..` mit `/` und unter Windows mit `\`, Mount `/data` unberührt.

Python: `python.exe -m unittest discover -v -s tests/refueled -p test_mods_manifest.py` (Log
`pytest-manifest.log`): 10 Tests, OK, 1 übersprungen. Übersprungen ist der direkte Vergleich mit dem Link-Baum
(`test_same_selection_as_the_link_mode`): Symlinks brauchen unter Windows ein Recht (WinError 1314, so
scheitern auch 11 der 13 bestehenden Tests in `tests/test_mods.py` schon vorher). Ersatz: Link-Baum und
Manifest beginnen beide mit derselben Funktion `collect_replacements` (Code-Identität statt Vergleich der
Ergebnisse); der Test prüft zusätzlich, dass die Manifestpfade dieselbe casefold-Menge ergeben wie diese
Auswahl. Auf einem Rechner mit Symlinks vergleicht derselbe Test auch den tatsächlichen Link-Baum mit
Spielordner + Manifest.

run-windows.sh (BB_DRY_RUN=1, leerer `eboot.bin`, Dummy-Mod-Ordner unter tmp):
- mit Mod-Ordner: `+ ... scripts/mods.py ... --enabled 1 --manifest ...\out\mods.manifest` und
  `Mods: virtual overlay, BB_MOD_MANIFEST=...\out\mods.manifest`
- ohne Mod-Ordner und mit `BB_MODS_ENABLED=0`: weder `mods.py` noch `BB_MOD_MANIFEST` in der Ausgabe
- `BB_GUARD_SELFTEST=1`: alle Zeilen ok, Exit 0

## Offene Punkte

- Archiveinträge (Mods, die Dateien in `.dcx`/Archiven ersetzen) = später.
- Fehler beim Laden des Manifests sind nicht als Test abgedeckt (der Prozess endet mit `exit(1)`).
- Ein Spielpfad mit abweichender Groß-/Kleinschreibung findet Overlay-Dateien nicht (exakter Vergleich, wie
  verlangt); der bestehende `fix_case`-Weg gilt nur für den Spielordner.
- Nicht getestet: Start des Spiels (`run-windows.sh` ohne `BB_DRY_RUN`), Linux (nicht gebaut).
- Reload von `/app0`-Mounts durch SaveData-ähnliche Mounts wird nicht berücksichtigt; `app0_host` ist der
  Wert aus `runtime_file_configure`.
- Bei einem Overlay-Pfad wie `/app0/x/` mit Nicht-Verzeichnis-Treffer (Datei unter Dateinamen) gilt der Index
  vor dem Spielordner.
- Beim Testen wurden zwei Python-Läufe (`tests/test_mods.py`, erster Lauf von `test_mods_manifest.py`) mit dem
  Standard-Temp des Systems gestartet statt mit TMP/TEMP unter `C:\CTR Reload\tmp`; die Verzeichnisse wurden von
  `tempfile` selbst wieder gelöscht. Danach wurde TMP/TEMP korrekt gesetzt.

## Review Opus
runtime_file.c Zeile für Zeile: ohne `BB_MOD_MANIFEST` gleiche Auflösung, gleiche Lock-Reihenfolge, gleiche Meldungen,
errno-Weg unverändert (overlay_standin kehrt bei overlay_count 0 sofort zurück); `PATH_SEP` unter Linux nur `/`.
mods.py: Link-Modus unverändert (gleiche Reihenfolge Spielordner-Prüfung → Auswahl → Meldungen). Antworten: Bedingung in
run-windows.sh (Mod-Ordner oder `<Spiel>-mods` vorhanden) passt; Manifest nur mit Kopfzeile ist gültig. Die drei
Testfehler `mz_uncompress` kamen vom fehlenden `out/deps` im Worktree: mit kopiertem `out/deps` unter CLANG64
`bash build-windows.sh --no-gpu --test --skip-gpu`: 6 run, 0 failed, test_file_mods inkl. Overlay PASS (Opus-Lauf).
Ausgeführt von Sonnet, geprüft von Opus. Verhaltensabweichung Linux: keine (bestätigt).
