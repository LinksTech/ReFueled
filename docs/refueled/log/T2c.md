# Paket-Log T2c (run-windows.sh, Sonnet)

Verhaltensabweichung Linux: gen_import_names.py liest/schreibt UTF-8 unabhängig von der Locale (D21 Nr. 10); run-windows.sh nur Windows

Status: **ungetestet** bis MSYS2 CLANG64, ein Spielordner 1.21 und `out/win/sacredfire.exe` (F3b) vorhanden sind.
Nur `bash -n` und ein Trockenlauf unter Git-Bash (MINGW64) wurden gemacht.

## Was gemacht
Neu: `run-windows.sh` (Repo-Wurzel). `run.sh` unverändert. Kein Linux-Code berührt, daher entfällt die Gleichheitstabelle.

Schritte wie `run.sh`, gleiche Reihenfolge: Umgebung prüfen, `game_check.py`, `prepare.py`, `link_libc.py`,
`link_modules.py` (Module `libc.prx libSceFios2.prx libSceNpToolkit2.prx`), `gen_import_names.py`, `content_profile.py`,
`patches.py`, Schalter, Exe starten (`--app0`, `--user`, `--timeout`, `"$@"` wie run.sh).

### Pflichten aus dem Auftrag
| Pflicht | Umsetzung |
|---|---|
| `MSYSTEM=CLANG64` | sonst Meldung und Exit 1; nur mit `BB_DRY_RUN=1` eine Warnung (damit der Trockenlauf unter Git-Bash geht) |
| Python | `PYTHON` oder erstes ausführbares von `$MSYSTEM_PREFIX/bin/python3`, `…/python`, `/clang64/bin/python3`, `/clang64/bin/python`, `/usr/bin/python3`, dann `command -v python3` nur wenn der Pfad nicht `*WindowsApps*` enthält (Store-Platzhalter); Prüfung `sys.version_info >= (3,10)`; `PYTHONUTF8=1`, `PYTHONDONTWRITEBYTECODE=1` |
| `BB_GAME_DIR` Pflicht | ohne Wert Abbruch, kein Standardordner |
| Pfadnormalisierung | alle Pfade (`BB_GAME_DIR`, `BB_DATA_DIR`, `BB_USER_DIR`, `BB_CONFIG`) kommen als `C:\…` oder `/c/…` an, werden mit `cygpath -u` + `realpath -m` zu absoluten MSYS-Pfaden. Dafür: Bash-Tests (`-f`, `-d`), `mkdir`, `rm`, Vergleiche mit `realpath` brauchen eine einheitliche POSIX-Form. |
| Übergabe an Python und Exe | `cygpath -w` (Backslash-Form). Begründung: `MSYS2_ARG_CONV_EXCL='*'` schaltet die automatische Umwandlung aus (sie ist bei `--opt=/pfad` und Umgebungsvariablen unvorhersehbar); darum übergibt das Skript jeden Pfad selbst im Windows-Format, das `python.exe` und `sacredfire.exe` unverändert lesen. Leerzeichen (`C:\CTR Reload`) sind überall in Anführungszeichen bzw. als Arrayelemente. |
| Spielordner nur lesen | das Skript schreibt nur nach `out/`, `out/win/tmp`, `user/`, `logs/`; Mods und der Link-Baum über den Dump entfallen |
| Löschschutz (Befund 1) | eigene Funktionen `may_delete_overlay` (nur `out/mod-game-*` direkt unter `out/`) und `may_delete_cache` (nur `<user>/cache/CUSA13795`), beide mit `realpath`-Vergleich und `refuses_game` (Ziel ist, enthält oder liegt im Spielordner → Ablehnung). Einziger Löschaufruf im Skript: `guarded_rm`. Selbsttest `BB_GUARD_SELFTEST=1`. |
| Mods | `BB_MODS_DIR`, `BB_MODS_CONFIG` oder `BB_MODS_ENABLED=1` gesetzt → Meldung und Exit 1; `scripts/mods.py` wird nicht gestartet. Vorhandene `mods/` bzw. `<Spiel>-mods` → nur ein Hinweis. |
| `bbport.ini` CRLF (Befund 7 des Audits) | `tr -d '\r' < "$BB_CONFIG" \| grep -qx 'patch_60fps=1'` |
| TMP/TEMP | `out/win/tmp` (als Windows-Pfad exportiert) |
| Logs | wie run.sh (`BB_SAVE_LOG=1`), die Schleife schneidet zusätzlich `\r` ab; `game_check`-Ergebnis und `VERSION` laufen durch `tr -d '\r'` |
| `BB_DRY_RUN=1` | alle zustandsändernden Befehle laufen über `run()`: nur ausgeben. Auch `mkdir`, `rm`, Python-Aufrufe, `build-windows.sh`, der Exe-Start. Das `game_check`-Ergebnis ist im Trockenlauf `ctr`. |
| Game-Check-Abbruch | `profile=$(… game_check.py … \| tr -d '\r') \|\| exit 2`: bei `missing_update` usw. steht die Meldung von `game_check` (stderr, nicht gefangen) im Terminal, Exit 2 wie run.sh |

### Was von run.sh entfällt (je Punkt)
| run.sh | Entscheidung |
|---|---|
| 4-12 `--software`, Lavapipe, `VK_DRIVER_FILES`, `VK_LOADER_LAYERS_DISABLE` | entfällt (Linux-Vulkan-Treiberwahl); `--software` als erstes Argument wird mit Hinweis ignoriert |
| 22-24 `nix-shell` | entfällt |
| 40-47 Python-Suche, `/nix/store` | ersetzt (siehe oben) |
| 57-65 `mods.py`, Overlay, `trap … rmtree` | entfällt (Mods nicht unterstützt) |
| 108-118 `BB_AS_0_3`, `BB_PC_MODEL` | entfällt; `BB_GUEST_IN_PLACE=0` fest (dma-buf nur Linux/AMD, D12) |
| 119-139 `bb-gpu-capabilities`, `RADV_DEBUG=nohiz`, `BB_AUTO_NOHIZ` | entfällt (Mesa/RADV) |
| 140-146 `__GL_SHADER_DISK_CACHE*`, `MESA_SHADER_CACHE_MAX_SIZE` | entfällt (Mesa/Linux-NVIDIA) |
| 147-161 MangoHud, `pgrep` | entfällt |
| 162-166 `BB_UFFD` | `BB_UFFD=0` fest (D13) |
| 195-204 Hintergrundstart mit `kill -TERM` und Overlay-Aufräumen | entfällt, weil es kein Overlay gibt; die Exe wird mit `exec` gestartet |
| gamescope, X11/Wayland, `ulimit`, `taskset` | kommen in run.sh nicht vor; nichts zu entfernen |

## Prüfungen und Ergebnis
| Prüfung | Ergebnis |
|---|---|
| `bash -n run-windows.sh` | grün |
| Zeilenenden der Datei | LF (kein `\r`) |
| Trockenlauf A: `BB_DRY_RUN=1 BB_GAME_DIR='C:\CTR Reload\tmp\refueled\T2c\game'`, Argument `--foo` | alle Schritte in Reihenfolge ausgegeben; Spiel-, `out`-, `user`-Pfade als Windows-Pfade (`C:\CTR Reload\…`, `C:\nitro-ps4-wt\T2c\out`); `--foo` am Ende der Exe-Zeile; Exit 0; nichts angelegt (`out/` existiert danach nicht) |
| Trockenlauf B: Selbsttest mit `/c/CTR Reload/…/game` | 11 von 11 `ok`: erlaubt `out/mod-game-abc123` und `user/cache/CUSA13795`; abgelehnt Spielordner, `out`, `out/other`, `out/mod-game-abc123/sub`, `<Spiel>/mod-game-abc123`, `out/../mod-game-abc123`, `user/cache`, `<Spiel>/cache/CUSA13795` |
| C: `BB_MODS_DIR=x` | Meldung, Exit 1 |
| D: ohne `BB_GAME_DIR` | Meldung, Exit 1 |
| E: `BB_CLEAR_SHADER_CACHE=1` mit `BB_USER_DIR` = Spielordner | `refusing to delete '…/game/cache/CUSA13795'`, nichts gelöscht |
| F: `BB_CLEAR_SHADER_CACHE=1` mit eigenem `BB_USER_DIR` | `rm -rf -- …/u/cache/CUSA13795` nur ausgegeben |

Der Testordner `C:\CTR Reload\tmp\refueled\T2c\game` ist leer und blieb leer. `C:\CUSA13795` wurde nicht berührt.

Die Backslashes erscheinen im Trockenlauf verdoppelt (`C:\\CTR\ Reload`), weil `printf %q` sie für die Shell quotet; der echte Wert hat einfache Backslashes.

## Ungebaut und ungetestet bis CLANG64
- Echte Läufe von `cygpath`, `python.exe`, `sacredfire.exe`, `build-windows.sh` (ruft F3b; Exe-Pfad `out/win/sacredfire.exe`, Version `out/win/VERSION` sind Annahmen aus dem Auftrag).
- `BB_SAVE_LOG`-Umleitung (Prozesssubstitution) unter CLANG64.
- Verhalten von `MSYS2_ARG_CONV_EXCL='*'` zusammen mit `bash build-windows.sh`: das Skript erbt die Variable.

## Offen / Fragen an Opus
1. `run.sh` löscht nach wie vor `user/cache/CUSA13795` ohne Prüfung; hier läuft das über den Schutz. Gewollt auch für `BB_USER_DIR` außerhalb des Repos?
2. Soll `run-windows.sh` `build-windows.sh` bei jedem Start aufrufen (wie `run.sh` `build.sh`)? Mit `BB_SKIP_BUILD=1` abschaltbar.
3. `pf_restart_through_launcher` (Neustart) ruft unter Linux `bash run.sh`; unter Windows müsste es `run-windows.sh` mit MSYS2-Bash starten (Befund 25).
4. `tools/gen_import_names.py` schreibt noch mit CRLF/Systemkodierung (Befund 21, nicht in diesem Paket).

## Review Opus
Löschschutz (guarded_rm, 11/11 Selbsttest) und Pfadnormalisierung geprüft. Ergänzt: BB_RESTART_COMMAND (Neustart über run-windows.sh), tools/gen_import_names.py mit encoding/newline (Befund 21, Linux bytegleich).

## Prüfung Opus 2026-10-13 (Pflichtfeld)
Feld bei der einmaligen Durchsicht aller Logs gesetzt und gegen die alt↔neu-Tabelle geprüft.
