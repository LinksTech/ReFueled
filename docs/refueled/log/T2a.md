# Paket-Log T2a (Portabilitätsaudit der Skripte, Sonnet)

## Was gemacht
- Gelesen: `scripts/prepare.py`, `link_libc.py`, `link_modules.py`, `patches.py`, `game_check.py`, `mods.py`, `content_profile.py`,
  `tools/gen_import_names.py`, `run.sh`, `build-windows.sh` (Kopf), Tests `tests/test_*.py` (Köpfe, Aufrufe von `subprocess`, `symlink`, `tempfile`).
- Bericht: `docs/refueled/audit/SCRIPTS-WINDOWS.md` (25 Befunde, Linkbaum-Alternativen, Vorschlag `run-windows.sh`, offene Fragen).
- Kein Code geändert, nichts committet. Spieldump `C:\CUSA13795` nicht gelesen, kein Skript gegen ihn gestartet.

## Testbefehle mit Ergebnis
Alle mit `TMP=TEMP=C:\CTR Reload\tmp\refueled\T2\tmp`, cwd `C:\CTR Reload\tmp\refueled\T2\`, Windows-Python 3.13.15:

```
python -I -B -m unittest discover -s C:\nitro-ps4-wt\T2\tests -p test_<name>.py
```

| Test | Ergebnis |
|---|---|
| prepare | 6 OK |
| patches | 7 OK |
| game_check | 8 OK |
| link_libc | 7 OK |
| import_names | 2 OK |
| content | 5 (3 übersprungen) OK |
| steam_shortcuts | 4 OK |
| runtime, probe, sema | nur übersprungen (Linux-Binaries) |
| mods | 13 Tests: 10 Fehler (`WinError 1314` bei `symlink_to`), 1 Fehlschlag (WSL-`bash.exe` bekommt `BB_GAME_DIR` nicht) |
| packaged_vulkan | 10 Tests: 1 Fehler (`symlink`), 1 Fehlschlag (Linux-Paketierung) |

Kleine Versuche (nur Textausgabe, keine Dateien im Repo): Pfadvergleich `Path.resolve()` gegen `realpath` in Git-Bash,
`write_text`-Bytes (CRLF), `print` und `$(…)`, `where bash`/`where python3`.

## Offen
- Siehe Abschnitt 6 im Bericht (u. a. MSYS2-Python 3.14 nicht installiert, 1.21-Spielordner für den Byte-Vergleich fehlt).
- Wichtigster Befund: run.sh:62-64 löscht unter Windows den Spielordner (Pfadvergleich `C:\…` gegen `/c/…`), bevor `run-windows.sh`
  existiert, ist das zu härten.
