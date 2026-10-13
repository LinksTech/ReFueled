# ReFueled – Audit T2a: Portabilität der Skripte unter Windows

Stand 2026-10-13, Worktree `C:\nitro-ps4-wt\T2` (Branch `refueled/pkg/T2`). Statischer Audit plus Ausführung der vorhandenen
Python-Unittests mit Windows-Python. Es wurde kein Code geändert, der Spieldump `C:\CUSA13795` wurde nicht gelesen und
kein Skript gegen ihn gestartet.

Geprüft: `scripts/prepare.py`, `link_libc.py`, `link_modules.py`, `patches.py`, `game_check.py`, `mods.py`,
`content_profile.py`, `tools/gen_import_names.py`, die Logik von `run.sh` (205 Zeilen), die Python-Tests unter `tests/`.
Nicht geprüft: `launcher/`, `packaging/`, die übrigen `tools/*.sh`.

Zielbild: `run-windows.sh` (MSYS2-Bash, MSYS2-Python) erzeugt aus demselben Spielordner dieselben Dateien in `out/` wie
`run.sh` unter Linux.

## 1. Methode

| Was | Wie |
|---|---|
| Statisch | alle Skripte vollständig gelesen; Suchmuster `open( write_text read_text encoding newline tempfile os.rename/replace/chmod/access/getuid/fork symlink subprocess sys.platform mmap ctypes as_posix resolve rglob iterdir casefold` |
| Unittests | `python -I -B -m unittest discover -s C:\nitro-ps4-wt\T2\tests -p test_<name>.py`, `TMP`/`TEMP` = `C:\CTR Reload\tmp\refueled\T2\tmp`, cwd unter `C:\CTR Reload\tmp\refueled\T2\` |
| Verhalten | vier kleine Versuche in Git-Bash (MSYS2-Laufzeit) mit Windows-Python (siehe 3.1, 3.4) |
| Python | verfügbar war nur `Python 3.13.15` (Windows, `cp1252`, UTF-8-Modus aus). Das MSYS2-Python 3.14 aus dem Auftrag ist hier **nicht installiert**; alle Aussagen zu Standardkodierung und Zeilenenden gelten auch für 3.14 (PEP 686 macht UTF-8 erst in 3.15 zum Standard). |

Ergebnis der Unittests unter Windows:

| Test | Ergebnis |
|---|---|
| `test_prepare`, `test_patches`, `test_game_check`, `test_link_libc`, `test_import_names`, `test_steam_shortcuts` | grün (6 + 7 + 8 + 7 + 2 + 4) |
| `test_content` | grün, 3 übersprungen (brauchen `out/test_content`-Binary) |
| `test_runtime`, `test_probe`, `test_sema` | alle übersprungen (Linux-Binaries) |
| `test_mods` | **10 Fehler + 1 Fehlschlag** von 13: 10-mal `OSError [WinError 1314] Dem Client fehlt ein erforderliches Recht` bei `symlink_to`, 1-mal `No eboot.bin in orig/CUSA13795 (set BB_GAME_DIR)` (Befund 12) |
| `test_packaged_vulkan` | 1 Fehler (`symlink`), 1 Fehlschlag (Linux-Paketierung, `libGLX_nvidia`); gehört zur Linux-Paketierung, nicht zu T2 |

Folgerung: Die **Kernskripte (`prepare`, `link_*`, `patches`, `game_check`, `content_profile`) laufen unter Windows
unverändert** und bestehen ihre Tests. Probleme liegen an den Rändern: Symlinks, Textmodus, Kodierung und
vor allem in `run.sh` (MSYS-Pfade).

## 2. Befunde

Priorität: **M1** = blockiert Meilenstein 1 (Windows-Lauf mit dem Spiel bis `--list-imports`), **V** = Vergleichbarkeit
(Ausgabedateien nicht byte-gleich zu Linux), **S** = später.

| # | Datei:Zeile | Problem | Fix-Vorschlag (Linux unverändert) | Prio |
|---|---|---|---|---|
| 1 | run.sh:62-64 (`[[ $game != "$(realpath "$original_game")" ]]` und `trap … shutil.rmtree "$mod_game"`) | **Gefahr für den Spielordner.** `mods.py` gibt den Spielordner als Windows-Pfad aus (`C:\nitro…`), `realpath` liefert den MSYS-Pfad (`/c/…`). Beide sind nie gleich, auch wenn keine Mods aktiv sind (`build_overlay` gibt dann `game` unverändert zurück, mods.py:135). run.sh hält den echten Spielordner dann für ein privates Overlay und löscht ihn beim Beenden per `rmtree`. **Im Versuch bestätigt** (Abschnitt 3.1): `py=[C:\nitro-ps4-wt\T2] realpath=[/c/nitro-ps4-wt/T2]`, Vergleich ungleich. | In `run-windows.sh` den Vergleich nicht über Textpfade führen. Zwei Schutzschichten: (a) `mod_game` nur setzen, wenn der Ordnername mit `mod-game-` beginnt **und** der Elternordner `$out` ist (`case ${game##*[/\\]} in mod-game-*)`); (b) der Cleanup-Aufruf wird ein kleines Python-Snippet, das vor `rmtree` dieselbe Bedingung prüft und sonst abbricht. Dieselbe Härtung (b) darf in `run.sh` übernommen werden (Linux-Verhalten gleich, nur sicherer). | **M1** |
| 2 | run.sh:40 (`PYTHON=$(command -v python3 \|\| true)`) | Unter Windows steht in PATH der **Store-Platzhalter** `C:\Users\…\WindowsApps\python3.exe` (`where python3` im Versuch). Er startet kein Python. In MSYS2 CLANG64 gibt es `python3` evtl. gar nicht oder als Wrapper. | `run-windows.sh` setzt `PYTHON` explizit (`/clang64/bin/python`, sonst `python`), prüft mit `"$PYTHON" -c 'import sys; assert sys.version_info >= (3,11)'` und meldet klar. | **M1** |
| 3 | run.sh:22-24 (`nix-shell`), :40-47 (`/nix/store`) | Linux-/Nix-Zweig; unter MSYS2 wirkungslos, aber `pkg-config --exists vulkan sdl3` kann dort zufällig gelingen und `nix-shell` fehlt dann → kein Fehler (`command -v nix-shell` falsch). | In `run-windows.sh` entfällt der Block (Abschnitt 5). | S |
| 4 | prepare.py:259, :271; link_libc.py:202; link_modules.py:246; content_profile.py:18; tools/gen_import_names.py:58 | `Path.write_text()` im Textmodus: **jedes `\n` wird unter Windows `\r\n`** (Versuch: `b'a\r\nb\r\n'`). Betrifft `profile.txt`, `analysis.json`, `libc-link.json`, `link.json`, `content-profile.json`, `import_names.txt`. Funktional unschädlich (Runtime liest `import_names.txt` mit `fopen("r")`, `sscanf` überspringt `\r`; `json.loads` akzeptiert CRLF), aber **die Ausgabedateien sind nicht byte-gleich zu Linux**; das ist das Abnahmeziel von T2. Nur `analysis.json` setzt `encoding='utf-8'`, die anderen nicht (Befund 5). | Überall `write_text(text, encoding='utf-8', newline='\n')` (`newline=` gibt es seit Python 3.10 bei `Path.write_text`; sonst `write_bytes(text.encode())`). Auf Linux identische Bytes. | V |
| 5 | mods.py:65; patches.py:34, :88; tools/gen_import_names.py:38, :54; (Tests: tests/test_link_libc.py:33) | `read_text()` ohne `encoding=`: unter Windows `cp1252`. Eine UTF-8-Datei `mods.json`/`patches.json`/`bbport.ini` mit Nicht-ASCII-Zeichen (Modnamen!) wird zu Mojibake, mit den Bytes 0x81/0x8D/0x8F/0x90/0x9D zu `UnicodeDecodeError`. Die Modnamen werden mit Ordnernamen verglichen (mods.py:68-76), ein Mojibake-Name findet den Ordner nicht. | `read_text(encoding='utf-8')` an den fünf Stellen. Zusätzlich in `run-windows.sh`: `export PYTHONUTF8=1` (wirkt auch auf `print()` und Argumente, siehe 6). | V (M1 nur bei Nicht-ASCII-Pfaden) |
| 6 | mods.py:185 (`print(build_overlay(…))`), game_check.py:104 (`print(p['name'])`) | Die Ergebnisse gehen über **stdout in Bash** (`game=$(…)`, `profile=$(…)`). Unter Windows schreibt `print` die Locale-Kodierung (`cp1252`) und `\r\n`. Im Versuch entfernte die MSYS-Bash das `\r` in `$(…)` (Länge 3 statt 4), die Bytes in der Pipe enthielten es aber (`ctr\r\n`). Ein Pfad mit Zeichen außerhalb `cp1252` (z. B. Benutzername) würde `UnicodeEncodeError` auslösen oder falsch ankommen. | `PYTHONUTF8=1` (siehe 5). Zusätzlich robust gegen `\r`: in `run-windows.sh` `$(… \| tr -d '\r')` oder `sys.stdout.reconfigure(newline='\n')` am Anfang von `main()` in beiden Skripten. | S |
| 7 | run.sh:96 (`grep -qx 'patch_60fps=1' "$BB_CONFIG"`) und gpu/shim/bbport_settings.cpp:304 (`fopen(…, "w")`) | Die Einstellungsdatei wird vom Spiel im **Textmodus** geschrieben (CRLF unter Windows; `_fmode` ändert ein explizites `"w"` nicht). `grep -x` vergleicht ganze Zeilen, `patch_60fps=1\r` passt nicht → der 480-Hz-Vblank für den 60-FPS-Patch wird still nicht gesetzt (Bildraten-Verlust, kein Absturz). `patches.py:read_settings` ist robust (`splitlines`, `strip`). Zusätzlich: `std::rename(tmp, Path())` (bbport_settings.cpp:307) scheitert unter UCRT, wenn das Ziel existiert (bekannte Lücke, F6-Gap-`rename`). | In `run-windows.sh`: `tr -d '\r' < "$BB_CONFIG" \| grep -qx 'patch_60fps=1'`. Besser an der Quelle: `"wb"` in bbport_settings.cpp (Paket S2/S3, nicht T2a). | S |
| 8 | mods.py:113, :141, :157 (`symlink_to`) | **Symlinks sind für diesen Benutzer nicht möglich** (`WinError 1314`, kein Entwicklermodus / kein `SeCreateSymbolicLinkPrivilege`; Beleg: 10 Fehler in `test_mods`). Der Overlay-Baum für Mods (`out/mod-game-XXXX`) entsteht nur, wenn Mods oder ein Ordner `<Spiel>-mods` vorhanden sind (sonst Rückgabe schon in mods.py:135); **ohne Mods ist M1 nicht betroffen**. | Siehe Abschnitt 4 (Link-Baum-Alternativen). Kurzfassung: Junctions für Ordner, Hartlinks für Dateien, Kopie als Rückfall. | M1 nur mit Mods, sonst S |
| 9 | mods.py:106-113, :150-157 (`expand`, `destination.is_symlink()`, `.resolve().is_relative_to(game)`) | Die Overlay-Logik **erkennt Links mit `is_symlink()`**. Junctions und Hartlinks sind unter Windows keine Symlinks (`Path.is_symlink()` ist für beide `False`); beim Ersatz greift dann der Zweig `elif destination.exists(): raise ValueError('File/directory conflict')`. Ein bloßer Austausch von `symlink_to` genügt also nicht. | Overlay in zwei Schritten bauen: erst die Menge der **zu materialisierenden Ordner** aus allen Modpfaden berechnen (alle Vorfahren der ersetzten Dateien), diese als echte Ordner anlegen, jeden anderen Eintrag verlinken, dann die Moddateien einsetzen. Kein späteres `expand`, kein `is_symlink`. Zähler `replaced`/`added` aus der Vorberechnung. Auf Linux dieselbe Logik mit Symlinks, Ergebnis gleich. | M1 nur mit Mods |
| 10 | mods.py:161 (`shutil.rmtree(overlay)`), run.sh:64 | `rmtree` über einen Baum aus Junctions: Python ≥ 3.12 löscht Junctions als Link, ohne hineinzugehen (`os.path.isjunction`); bei Hartlinks entfernt es nur den Namen. Das gilt es mit einem Test zu belegen (Atrappe, keine Spieldaten), bevor Junctions eingesetzt werden: ein `rmtree` durch eine Junction würde Dateien des Dumps löschen. | Unittest `test_mods` (Windows-Zweig): Overlay mit Junction + Hartlink bauen, `rmtree`, prüfen, dass die Quelldateien noch da sind. **Vor dem ersten Lauf mit echtem Dump.** | M1 mit Mods |
| 11 | prepare.py:255 (`(game/'dvdroot_ps4').rglob('*')`) | Zählt `resources`/`resource_bytes` für `analysis.json`. `rglob` folgt unter Python 3.13 keinen Symlinks auf Ordner (3.10-3.12: doch), Junctions dagegen schon. Dieselben Spieldaten können so je nach Linkart und Python-Version andere Zahlen in `analysis.json` ergeben. Für `boot*.bin` (binär, aus eboot und libc) ohne Belang. | Beim Vergleich Linux/Windows nur `boot.bin`, `boot-libc.bin`, `boot-linked.bin`, `eboot.elf`, `entry.bin`, `content.bin`, `patches.bin`, `import_names.txt` byte-genau vergleichen; `analysis.json` ohne die Felder `resources`/`resource_bytes`. Nach Wahl des Linkverfahrens die Zählung für beide Plattformen festlegen (z. B. `os.walk(followlinks=True)`). | V |
| 12 | tests/test_mods.py:155 (`subprocess.run(['bash', 'run.sh'], …)`), :141-144, :149-151 | (a) `bash` in PATH ist `C:\Windows\System32\bash.exe` (**WSL-Starter**, `where bash`): kein Umgebungs- und Pfad-Durchreichen, daher `No eboot.bin in orig/CUSA13795` (die Variable `BB_GAME_DIR` kam nicht an). (b) `PYTHON`/`probe` sind Skripte mit Shebang `#!{sys.executable}` und `chmod(0o755)`; Windows führt keine Shebangs aus. (c) der Test erwartet `rmtree` des Overlays; siehe Befund 1. | Test auf Windows mit MSYS2-`bash` (absoluter Pfad aus `shutil.which` gegen `MSYSTEM_PREFIX`/Umgebungsvariable `BB_TEST_BASH`) und `.cmd`-Wrappern, sonst `skipUnless(sys.platform != 'win32')`. Ein eigener Windows-Test für `run-windows.sh` folgt in T2b. | S |
| 13 | tests/test_mods.py:79, tests/test_packaged_vulkan.py:59 | `symlink_to` in Tests: Fehler statt Überspringen. | `@unittest.skipUnless(can_symlink(), …)` mit kleiner Hilfsfunktion in `tests/paths.py`; die Overlay-Tests laufen zusätzlich mit dem Windows-Linkverfahren. | S |
| 14 | mods.py:42-47 (`content_root`) | Eine Modordner-Wurzel zählt als "Archivordner um das Mod", wenn sie keine Dateien außer `.txt .md .jpg .png .ini` enthält. Unter Windows legt der Explorer `Thumbs.db`/`desktop.ini` an; `.db` stört die Erkennung, `desktop.ini` ist versteckt, aber nicht mit Punkt (`startswith('.')`). | `e.name.casefold() in ('thumbs.db', 'desktop.ini')` ignorieren. | S |
| 15 | mods.py:85-89 (`os.walk(followlinks=False)`, `is_symlink`) | Junctions in einem **entpackten Mod** sind für `os.walk` Ordner und werden begangen; `is_symlink` meldet sie nicht. Selten (Archive enthalten keine), kein Datenrisiko, da nur gelesen wird. | Optional `os.path.isjunction` (3.12+) mit abweisen. | S |
| 16 | mods.py:138, run.sh:58-59 | Pfadlänge: Overlay `out\mod-game-XXXXXXXX\dvdroot_ps4\…` plus Tiefe der Spielpfade. Ohne `LongPathsEnabled` (laut ARCHITECTURE R8 = 0) und ohne `longPathAware`-Manifest bei `python.exe`/`sacredfire.exe` scheitern Pfade über 260 Zeichen. Die längsten Pfade im Dump wurden **nicht** gemessen (Dump ist tabu). | Kurzer `--out` (z. B. `C:\nitro-ps4-port\out`), kürzerer Präfix `m-` statt `mod-game-` (nur Windows). PL kann die längste Zeichenzahl selbst messen (`Get-ChildItem -Recurse C:\CUSA13795 \| % { $_.FullName.Length } \| measure -Maximum`). | S |
| 17 | run.sh:30-37 (`BB_SAVE_LOG`, `exec > >(…)`) | Prozesssubstitution und `printf -v … '%(…)T'` laufen in MSYS2-Bash. Die Schleife `while read … printf` ist langsam und gibt `\r` aus `sacredfire.exe` (Textmodus-Ausgabe der C-Runtime: `\r\n`) 1:1 in die Logdatei weiter. | In `run-windows.sh` übernehmen und `tr -d '\r'` ergänzen oder `tee` verwenden (in MSYS2 vorhanden, anders als im AppImage). | S |
| 18 | run.sh:195-205 (`kill -TERM "$mod_pid"`, `wait`) | Der Hintergrundprozess ist ein natives Windows-Programm. MSYS-`kill -TERM` kann es nicht per Signal beenden, nur mit `kill -f` (TerminateProcess). Ctrl+C im MSYS-Terminal erreicht den Prozess über die Konsole, nicht über den Trap. | `run-windows.sh`: `kill -f` verwenden; Overlay-Cleanup bleibt im `EXIT`-Trap. | S |
| 19 | run.sh:205 (`exec "$probe"`) | `out/sacredfire` heißt unter Windows `out/win/sacredfire.exe` (build-windows.sh) bzw. `bin/sacredfire.exe`. | In `run-windows.sh` `probe=${BB_PROBE:-out/win/sacredfire.exe}` und `bash build-windows.sh` statt `bash build.sh`. | **M1** |
| 20 | run.sh (alle Zeilen mit `"$out/…"`, `"$game"`) und die Skripte | **MSYS-Pfadumwandlung.** Bash wandelt beim Start eines nativen Programms Argumente, die wie POSIX-Pfade aussehen (`/c/…`), in Windows-Pfade um; `--opt=/pfad` und Umgebungsvariablen wandelt sie teils um, teils nicht (Liste der Muster in der MSYS2-Doku). Die Skripte nutzen `argparse` mit `Path`, tolerieren beide Schreibweisen, solange die Umwandlung stattfindet. Relative Pfade (`out`, `orig/…`) sind unkritisch. Der einzige Ort mit Zeichenketten-Vergleich von Pfaden ist Befund 1. Im `sacredfire.exe` kommen die Pfade (`--app0 "$game"`) als Windows-Pfad an. Leerzeichen (`C:\CTR Reload`) sind überall in Anführungszeichen. | `run-windows.sh` wandelt `BB_GAME_DIR` einmal mit `cygpath -u` in einen POSIX-Pfad für Bash-Tests (`[[ -f … ]]`) und übergibt an Python/Exe den `cygpath -m`-Pfad (`C:/CUSA13795`, Schrägstriche, ohne Backslashes). `MSYS2_ARG_CONV_EXCL='*'` setzen, damit nichts still verändert wird, und Pfade selbst umwandeln. | **M1** |
| 21 | tools/gen_import_names.py:28-37 | Lädt `aerolib.inl` per `urllib` aus dem Netz, wenn `out/aerolib.inl` fehlt; offline fällt es auf leere Namen zurück (Meldung, kein Fehler). Unter MSYS2-Python braucht `https` das CA-Paket (`ca-certificates`). Die Ausgabe `import_names.txt` hängt davon ab, **ob die Datei geladen wurde** (benannte gegen `?`-Importe), also nicht byte-gleich zwischen Läufen mit und ohne Netz. | Für den Vergleich `out/aerolib.inl` von Linux nach Windows kopieren (`--aerolib`); Pfadregel: Download landet in `out/`, nicht in `%TEMP%`. | V |
| 22 | alle Skripte (`tempfile`) | Nur `mods.py:138` nutzt `tempfile.mkdtemp`, und mit `dir=out`: **liegt im Repo**, nicht in `%TEMP%`. Die Tests nutzen `TemporaryDirectory()` im Standardordner. Python schreibt außerdem `__pycache__` in `scripts/`. | Läufe und Tests mit `TMP`/`TEMP` unter `C:\CTR Reload\tmp\…` und `PYTHONDONTWRITEBYTECODE=1` (`-B`) starten. `run-windows.sh` setzt beides. | S |
| 23 | Repo-Wurzel (fehlt: `.gitattributes`); `git config core.autocrlf` = `false` im Worktree | Ohne `.gitattributes` hängt die Zeilenendung der Skripte von der Git-Konfiguration ab. Bei `core.autocrlf=true` (Windows-Standard bei Git for Windows) würden `run.sh`, `build*.sh` mit CRLF ausgecheckt und Bash bricht mit `$'\r': command not found` ab. Hier nicht der Fall (alle geprüften Dateien LF). | `.gitattributes`: `*.sh text eol=lf`, `*.py text eol=lf`, `*.S text eol=lf` (Opus, nicht T2a). | S (M1, falls Git der PL `autocrlf=true` setzt) |
| 24 | scripts/game_check.py (SUPPORTED nur 1.21) | Der Dump `C:\CUSA13795` ist Version 1.00. `game_check` meldet dafür `missing_update` und beendet `run.sh` mit Exit 2; `prepare.py` bricht mit demselben Text ab. Nicht in diesem Audit zu lösen, aber für M1 wichtig. | Für Windows-Läufe mit 1.00: `BB_SKIP_GAME_CHECK=1` (nur Importliste/`--list-imports`; die Ausgabe ist **nicht** mit der Linux-Referenz 1.21 vergleichbar). | M1 (Entscheidung Opus/PL) |
| 25 | restart: src/platform/pf_process_linux.c (`execlp bash run.sh`) | Der Neustart über den Launcher ruft `bash run.sh`. Unter Windows muss er `run-windows.sh` mit MSYS2-Bash aufrufen (nicht die WSL-`bash.exe`). | Paket R3/Opus: `pf_restart_through_launcher` unter Windows über `BB_RESTART_CMD` oder festen MSYS2-Pfad. | S |

Nicht gefunden (ausdrücklich geprüft): `os.fork`, `os.getuid`, `os.access(X_OK)`, `os.chmod`, `os.rename`/`os.replace` (die Skripte nutzen
keines davon), `signal`, `subprocess` (nur in Tests), `shell=True`, `mmap`, `ctypes`, `sys.platform`-Abfragen,
Endianness (alle `struct`-Formate mit `<`), Binärmodus (alle Binärdateien über `read_bytes`, `write_bytes`, `open('wb')`),
harte `/`-Annahmen (`Path`-Operator `/` mit `/`-Teilstrings funktioniert unter Windows; `as_posix()` wird nur für
den Schlüsselvergleich in mods.py:127 benutzt), `\\`-Literale, Laufwerksbuchstaben. Groß-/Kleinschreibung ist in `mods.py`
durchgängig über `casefold()` gelöst (Zeilen 22-23, 44, 47, 58, 95-96, 127), also unter Windows eher einfacher.

### Zahl der Befunde

| Priorität | Anzahl | Nummern |
|---|---|---|
| M1 (blockiert) | 5 | 1, 2, 19, 20, 24 |
| M1 nur mit Mods | 3 | 8, 9, 10 |
| V (Vergleichbarkeit) | 4 | 4, 5, 11, 21 |
| später (S) | 13 | 3, 6, 7, 12, 13, 14, 15, 16, 17, 18, 22, 23, 25 |
| gesamt | 25 | |

## 3. Belege aus den Versuchen

### 3.1 Pfadvergleich in run.sh (Befund 1)

Git-Bash (MSYS2-Laufzeit) mit Windows-Python 3.13:

```
py=[C:\nitro-ps4-wt\T2]   realpath=[/c/nitro-ps4-wt/T2]   -> MISMATCH
cygpath -u "$g"           -> /c/nitro-ps4-wt/T2
```

Der MSYS-Pfad wird beim Aufruf von Python in einen Windows-Pfad umgewandelt, `Path.resolve()` gibt Windows-Schreibweise
zurück, `realpath` bleibt POSIX. Der Vergleich in run.sh:62 ist daher immer "ungleich".

### 3.2 Zeilenenden (Befund 4)

```
Path('crlf.txt').write_text('a\nb\n') ; read_bytes() -> b'a\r\nb\r\n'
locale.getpreferredencoding(False) -> cp1252 ; sys.flags.utf8_mode -> 0
```

### 3.3 `$(…)` und `\r` (Befund 6)

`python -c "print('ctr')" | od -c` zeigt `c t r \r \n`; `x=$(python -c "print('ctr')")` hat in dieser Bash die Länge 3. Die MSYS-Bash
entfernte das `\r` also. Das ist Laufzeitverhalten der MSYS-Bash, nicht Python; ob es in der CLANG64-Bash gleich ist, wurde nicht
getestet (nicht installiert). Daher bleibt die Absicherung per `tr -d '\r'` oder `newline='\n'` ratsam.

### 3.4 `where`

`where bash` → `C:\Windows\System32\bash.exe` (WSL), `C:\Users\…\AppData\Local\Microsoft\WindowsApps\bash.exe`;
`where python3` → Store-Platzhalter. `python` → `Python313\python.exe`.

## 4. Link-Baum für Mods: Alternativen

Zur Klarstellung: `run.sh` baut **keinen** Link-Baum des Dumps. Der Ordner `orig/CUSA13795` ("read-only link tree of the merged CTR dump")
legt der Benutzer an; unter Windows wird `BB_GAME_DIR=C:\CUSA13795` direkt übergeben und **nur gelesen**. Einen Baum aus Links
baut nur `scripts/mods.py` (`build_overlay`) für Mods, und nur, wenn Mods vorhanden sind. Der Dump wird dabei nie verändert
(Links zeigen auf ihn, geschrieben wird nur in `out/mod-game-XXXX`).

| Verfahren | Funktioniert unter Windows ohne Admin? | Vorteile | Nachteile |
|---|---|---|---|
| **A. Symlinks** (heute) | Nein (`WinError 1314`; nur mit Entwicklermodus oder Adminrecht) | Linux-Code unverändert | Für PL nicht verfügbar. |
| **B. Junctions für Ordner + Hartlinks für Dateien** (NTFS, gleiche Partition) | Ja, keine Rechte nötig. Junction: `_winapi.CreateJunction` oder `cmd /c mklink /J`. Hartlink: `os.link` | Kein Kopieren, Overlay entsteht in Sekunden, `Path.exists()`/`iterdir()` funktionieren wie bei Symlinks, `rmtree` entfernt Hartlink-Namen und (ab Python 3.12) Junctions als Links. Dump und Repo liegen beide auf C:. | Nur gleiche Partition (Mods auf D: oder anderem Laufwerk: Hartlink scheitert → Rückfall Kopie). `is_symlink()` ist `False`: Overlay-Logik umbauen (Befund 9). Junction darf nicht durch `rmtree` betreten werden: Test nötig (Befund 10). Hartlink-Dateien teilen sich Rechte und Zeitstempel mit dem Original. Zeigt das Overlay auf Mod-Dateien, die der Benutzer später ändert, ändert sich der Overlay-Inhalt mit (bei Symlinks ebenso). |
| **C. Kopie** der ersetzten Dateien, Links für den Rest | Ja | Einfach, keine Sonderfälle | Mods mit hunderten MB werden bei jedem Start kopiert (Dauer und Plattenverbrauch in `out/`); der Rest braucht weiterhin B. |
| **D. Kein Baum: mehrere Wurzeln an die Runtime** (`--app0 <Spiel> --mod <Ordner>`; Auflösung `runtime_file.c: translate` zuerst in den Mods, dann im Spiel; `list_directory` vereint Einträge) | Ja | Keine Links, kein Cleanup, keine Pfadlängenvergrößerung, plattformgleich, kein Löschrisiko | Eingriff in `runtime_file.c` (R2, Dateipfade, Verzeichnislisting, Groß-/Kleinschreibung). Größter Aufwand, aber die sauberste Dauerlösung; `mods.py` wird dann nur zur Auswahl und Prüfung. |
| **E. Dump verschieben/kopieren** | – | – | **Nicht erlaubt** (Dump bleibt unverändert) und ein Vielfaches an Platz. |

Empfehlung: M1 ohne Mods (kein Baum nötig). Für Mods zuerst **B mit C als Rückfall** in `mods.py` (Hilfsfunktionen
`link_dir`, `link_file`, Linux bleibt `symlink_to`), später **D** prüfen. Hartlinks auf Dateien des Dumps verändern den Dump nicht
(neuer Name, gleicher Inhalt), erhöhen aber den Linkzähler der Dateien; das ist die einzige Spur am Dump. Wem das nicht
genügt, nimmt für Dumpdateien Junction auf Ordnerebene (liegt ganz außerhalb der geänderten Pfade) und Kopie für die
wenigen veränderten Ordner.

## 5. Vorschlag `run-windows.sh`

Aus `run.sh` übernommen (Zeilenbezug auf run.sh):

| run.sh | Übernahme in `run-windows.sh` |
|---|---|
| 2-3 `set -euo pipefail`, `cd` in das Skriptverzeichnis | ja, unverändert |
| 4-12 `--software`, Lavapipe | **entfällt** (Linux-Treiberwahl). Optional später `--software` mit Lavapipe-ICD aus MSYS2. |
| 13-19 `BB_PREBUILT`, `BB_DATA_DIR`, `out`, `BB_CONFIG` | ja; Standard `data=.`; zusätzlich `export TMP=… TEMP=…` und `PYTHONDONTWRITEBYTECODE=1`, `PYTHONUTF8=1` |
| 20-24 `nix-shell` | **entfällt** |
| 25-37 `BB_SAVE_LOG` | ja, mit `tr -d '\r'` (Befund 17) |
| 38-47 `PYTHON` suchen | ersetzt: fester Eintrag `/clang64/bin/python` mit Prüfung (Befund 2) |
| 48-52 `BB_GAME_DIR` | **ohne Standardwert** `orig/CUSA13795`; Pflicht, z. B. `BB_GAME_DIR=C:\CUSA13795` (nur gelesen); Umwandlung per `cygpath` (Befund 20). Fehlt `eboot.bin`: Abbruch wie bisher. |
| 53-56 `game_check.py`, `BB_GAME_PROFILE` | ja (bei 1.00-Dump Befund 24) |
| 57-65 `mods.py` und Cleanup | ja, mit gehärtetem Cleanup (Befund 1); Mods-Overlay erst nach Befund 8-10 aktivieren, bis dahin `BB_MODS_ENABLED=0` als Standard unter Windows, sofern kein Mod vorhanden ist |
| 66-72 `prepare`, `link_libc`, `link_modules`, `gen_import_names`, `content_profile` | ja, unverändert, `--out "$out"` |
| 73-79 `patches.py` | ja |
| 80-86 `BB_NEO` | ja |
| 87-94 `BB_CLEAR_SHADER_CACHE` | ja (`rm -rf` auf `user/cache/CUSA13795`) |
| 95-99 60-FPS-Vblank | ja, mit `tr -d '\r'` (Befund 7) |
| 100-118 `BB_PREUPLOAD`, `BB_PC_MODEL`, `BB_AS_0_3`, `BB_GUEST_IN_PLACE` | `BB_PREUPLOAD` ja; `BB_GUEST_IN_PLACE` **fest 0** (D12: dma-buf nicht unterstützt), `BB_PC_MODEL` ignorieren |
| 119-139 GPU-Erkennung, `RADV_DEBUG=nohiz` | **entfällt** (Mesa/RADV) |
| 140-146 `__GL_SHADER_DISK_CACHE*`, `MESA_SHADER_CACHE_MAX_SIZE` | **entfällt** (NVIDIA unter Windows nutzt andere Variablen; Treiber-Shadercaches sind laut Pfadregel akzeptiert) |
| 147-161 MangoHud, `pgrep`, `VK_LOADER_LAYERS_DISABLE` | **entfällt** |
| 162-166 `BB_UFFD` | **fest 0** (D13) |
| 167-172 `BB_COPY_GPU_BUFFERS`, `BB_GPU_WRITE_TWINS*`, `BB_VBLANK_HZ` nach `BB_FPS` | ja |
| 173-181 `build.sh`, `VERSION`, `probe` | `bash build-windows.sh`, `probe=${BB_PROBE:-out/win/sacredfire.exe}`; `BB_PREBUILT` → `bin/sacredfire.exe` |
| 182-190 `BB_GPU_USER_DIR`, `probe_args` | ja (`--app0 "$game_m"`, `--user "$user_m"` mit `cygpath -m`) |
| 191-205 Start mit/ohne Overlay | ja, `kill -f` (Befund 18) |

Umgebungsvariablen, die das Skript setzt: `TMP=TEMP=C:\CTR Reload\tmp\…` (nur für Tests und Entwickler; für den
Spielbetrieb ist `%TEMP%` unkritisch, die Pfadregel gilt für die Entwicklungsläufe), `PYTHONUTF8=1`, `PYTHONDONTWRITEBYTECODE=1`,
`MSYS2_ARG_CONV_EXCL='*'`, `BB_GAME_DIR` (Pflicht, **nur lesen**), `BB_GAME_PROFILE`, `BB_CONFIG`, `BB_NEO`, `BB_GUEST_IN_PLACE=0`,
`BB_UFFD=0`, `BB_PREUPLOAD`, `BB_COPY_GPU_BUFFERS`, `BB_GPU_WRITE_TWINS(_MAX)`, `BB_VBLANK_HZ`, `BB_GPU_USER_DIR`. Der
Spielordner wird weder verlinkt noch beschrieben noch ausgeführt; `out/` ist der einzige beschriebene Ordner neben `user/`.

Reihenfolge der Schritte (wie Linux): `game_check` → `mods` → `prepare` → `link_libc` → `link_modules` → `gen_import_names` →
`content_profile` → `patches` → Start.

## 6. Offene Fragen an Opus

1. Befund 1: Soll die Härtung des Cleanup (Präfix `mod-game-` + Elternordner `out`) auch in das gemeinsame `run.sh` (Linux-Verhalten gleich, nur
   sicherer), oder nur in `run-windows.sh`? Empfehlung: in beide.
2. Mods unter Windows: zuerst Variante B (Junction + Hartlink + Kopie) in `mods.py`, oder gleich D (mehrere Wurzeln in der
   Runtime)? B ist in `scripts/` abgeschlossen (T2), D braucht R2.
3. Soll T2 die Textdateien auf `newline='\n'` + `encoding='utf-8'` umstellen (Befunde 4, 5)? Die Änderung gehört in `scripts/*.py` und
   `tools/gen_import_names.py`, nicht in die Dateien von T2a; Linux-Ausgabe bleibt byte-gleich.
4. Windows-Referenz: Der vorhandene Dump ist 1.00. Für den Byte-Vergleich Linux/Windows der Dateien in `out/` braucht es denselben 1.21-Spielordner auf
   beiden Seiten. Gibt es einen (nur von der Projektleitung zu benutzenden) 1.21-Ordner für Windows? Ohne ihn ist die Vergleichsabnahme von T2 nur mit den
   synthetischen Testdaten der Python-Unittests möglich.
5. MSYS2-Python 3.14 war hier nicht verfügbar (nur Windows-Python 3.13). Die Befunde 4-6 hängen von `locale`/`newline` ab, nicht von der
   Minor-Version; ein Lauf der Unittests mit dem 3.14 aus CLANG64 steht noch aus.
6. `.gitattributes` (Befund 23) ist ein Repo-Wurzelfile: ergänzt Opus es?
7. Pfadlängen (Befund 16): Kann die Projektleitung die längste Pfadlänge im Dump selbst messen (Befehl in Befund 16)? Das hängt von `LongPathsEnabled` und dem Manifest ab.
8. `launcher/` und `packaging/` wurden nicht geprüft; ein eigenes Paket T2c?
