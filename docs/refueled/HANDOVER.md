# ReFueled – Übergabe (Stand 2026-10-14 abends)

Für eine neue Sitzung, die ohne die bisherige Unterhaltung weitermacht. Zuerst `C:\ReFueled\CLAUDE.md`
(nicht versioniert) lesen, dann diese Datei, dann [AGENT-REGELN.md](AGENT-REGELN.md),
[ARCHITECTURE.md](ARCHITECTURE.md) und [TASKS.md](TASKS.md).

## 1. Ort und Stand

| Was | Wo / Wert |
|---|---|
| Repo | `C:\ReFueled\repo`, Branch `refueled/windows` |
| Remote `refueled` | öffentliches GitHub-Repo ReFueled, `main` = `refueled/windows`; nur nach jeder Welle pushen, nie force |
| Remote `origin` | SacredFire upstream; meldet seit 2026-10-14 „Repository not found“, unverändert gelassen (Frage 5) |
| Worktrees | `C:\ReFueled\wt\<ID>`; offen ist nur `wt\T1` (Frage 2) |
| shadPS4-Referenz | `C:\ReFueled\ref\shadPS4` (main, 0ae50a4b) + Worktree `C:\ReFueled\ref\shadPS4-b4e7ae7`; nur lesen |
| Notizen außerhalb des Repos | `C:\ReFueled\notes` (Bestandsaufnahme SacredFire, `scratch-2026-10\` = Skripte und Rohdaten der Pakete bis 14.10.) |
| Scratch | `C:\ReFueled\tmp\<ID>\`, auch TMP/TEMP gestarteter Programme |
| Spielordner | `C:\CUSA13795`, nur lesen (`BB_GAME_DIR`); `game_check.py`: **1.00, Update 1.21 fehlt** |
| Toolchain | MSYS2 CLANG64 unter `C:\msys64` (siehe [TOOLCHAIN.md](TOOLCHAIN.md)); kein WSL, keine Nix-Umgebung, keine VM |

Letzte Commits vor dieser Übergabe (alle auf `main` gepusht; den Übergabe-Commit zeigt `git log`):

| Commit | Inhalt |
|---|---|
| `1959949` | CI1 (Linux-Testlink), Spitze der einmalig umgeschriebenen Historie |
| `2569307` | PUB2: publish_check prüft auch Autor-/Committer-Namen ([log/PUB2.md](log/PUB2.md)) |
| `77f8092` | GS1: `scePthreadAttrGetstack` ([log/GS1.md](log/GS1.md)) |
| `b1d0f76` | Umzug nach `C:\ReFueled`: Pfade in Regeln, Plänen, Werkzeugen |

Historie: Am 2026-10-14 wurde die eigene Historie zweimal umgeschrieben (Namensentfernung). Alle
Commit-Hashes aus Notizen vor 2026-10-14 abends sind veraltet. Die Hashes ab `47535e2` (Merge des
GitHub-Commits) haben sich beim zweiten Durchgang geändert; kein Dokument verweist auf die alten.

## 2. Prüfstand im neuen Ort

- `bash build-windows.sh --test` aus leerem `out/` (Abhängigkeiten neu geholt): Exe gelinkt,
  Plattformtests **11/11 grün**, build.sh-Satz **5/6**: `test_pad.exe` wird nach dem Linken von McAfee
  in Quarantäne genommen (zweimal bestätigt, Quarantäne-Einträge mit derselben Uhrzeit), der Test selbst
  lief im alten Ort grün. Frage 1.
- Einmalig beobachtet: `test_posix_subset` rename EACCES (frisch geschriebene Datei kurz gesperrt, vermutlich
  ebenfalls der Virenscanner); viermal einzeln und im nächsten Gesamtlauf grün.
- `git fsck` sauber, publish_check über die gesamte Historie sauber (170 Commits, Spielordner abgeglichen).
- `git status`: nur `gpu/third_party/fsr-vulkan` als geändert. Das ist der Patch aus `gpu/patches/fsr-vulkan`,
  den build-windows.sh/build.sh in den Submodul-Arbeitsbaum anwenden (so gewollt).
- Linux-CI (`linux-build`) ist rot seit dem ersten Lauf, unabhängig von GS1: nach dem Pad-Test bricht der
  Runtime-Test ab, weil die GPU-Lib `~/.local/share/shadPS4` anlegen will und `~/.local/share` auf dem Runner
  fehlt (`std::filesystem::create_directory`). `tests` ist grün. Siehe nächste Schritte.

## 3. Meilensteine

- **M1** blocked, bis 1.21 im Spielordner liegt. 1.00-Rauchtest ([log/M1-RAUCHTEST-100.md](log/M1-RAUCHTEST-100.md)):
  Importe 448 runtime / 275 native / 290 unaufgelöst, Image gemappt, Spielcode lief bis GPU-Submit 18,
  Stopp an `scePthreadAttrGetstack` (libSceNpToolkit2 1.00). **GS1 ist umgesetzt, der Rauchtest damit noch
  nicht wiederholt.**
- **M2–M4** blocked (hängen an M1).
- M3-Strang: M3S1 und M3S2 done (Testbild über sceVideoOut/Present-Thread, 60 Hz, ohne Validierungsfehler).
- Metrik `linux_hits.py`: 27 (Baseline 335, Rest gewollt, siehe [METRICS.md](METRICS.md)).

## 4. Offene Pakete (aus TASKS.md)

| ID | Stand |
|---|---|
| F5 | review: CMake-Zweig fertig, Abnahme cmake in CLANG64 offen |
| F7 | review: Windows-Lauf in CLANG64 offen |
| R2 | in_progress: PfStat/d_type in runtime_file.c, Wände in runtime_file.c/savedata.c |
| R3 | in_progress: Aufrufer in K3/V2 |
| R5 | in_progress: Kompilat der übrigen Dateien unter CLANG64 |
| T2 | in_progress: run-windows.sh mit CLANG64 |
| M3S | in_progress (M3S1, M3S2 done) |
| RZ | in_progress: RZ3–RZ5 offen (Red-Zone-Schutz, `BB_REDZONE`, fertig spätestens vor M3) |
| F8 | deferred-ci |
| M1–M4, D1, P2, P3 | blocked |

Mehrere „in_progress“-Zeilen stammen aus der Zeit vor dem ersten CLANG64-Build; seit Welle B baut alles
unter CLANG64. Erste Aufgabe der neuen Sitzung: diese Zeilen gegen den Code prüfen und auf done setzen
oder den echten Rest benennen.

## 5. Nächste Schritte (Vorschlag, Reihenfolge = kritischer Pfad zuerst)

1. **Benutzerordner der GPU-Lib umleiten** (Opus): Unter Windows schrieb ein Testlauf nach
   `%APPDATA%\shadPS4`, unter Linux will der Runtime-Test nach `~/.local/share/shadPS4`. Ziel: Tests und
   Läufe legen den Ordner unter `out/` bzw. `C:\ReFueled\tmp\<ID>` an (Umgebungsvariable oder portabler
   `user`-Ordner), Linux-CI wird damit grün. Mit Pflichtzeile „Verhaltensabweichung Linux“.
2. **1.00-Rauchtest wiederholen** (Opus) mit GS1, alle Ausgaben unter `C:\ReFueled\tmp\M1R2`, nächsten
   Stopp dokumentieren. Kein Debuggen über M1 hinaus, solange 1.21 fehlt, außer die Projektleitung gibt es frei.
3. **TASKS.md bereinigen** (Abschnitt 4), danach freie Pakete parallel an Sonnet (F5/F7-Abnahme, R2-Rest,
   RZ3–RZ5), kein Leerlauf.
4. **Sobald 1.21 da ist:** `game_check.py`, dann M1 echt, danach direkt weiter M2 (erster GPU-Submit) und
   erstes vom Spiel gerendertes Bild.

## 6. Offene Fragen an die Projektleitung

1. **McAfee** entfernt `test_pad.exe` im neuen Ort. Ausnahme für `C:\ReFueled` (mindestens `repo\out` und
   `wt\*\out`) einrichten oder die Datei freigeben? Bis dahin ist der Testsatz nicht vollständig grün, und
   die alten Orte werden nicht gelöscht.
2. **Worktree `wt\T1`**: 6 nicht übernommene Dateien (älterer Stand von T1; T1 selbst ist über b9e2d2d done).
   Löschen?
3. **`%APPDATA%\shadPS4`**: angelegt durch Testläufe der GPU-Lib (Cache bereits gelöscht, Ordnergerüst und
   Notice.txt liegen noch dort). Löschen?
4. **Dump 1.21**: weiterhin fehlend; M1 hängt daran.
5. **Remote `origin`** (SacredFire upstream) ist nicht mehr erreichbar. Behalten oder entfernen?
6. **Alter GitHub-Commit**: Nach dem force push ist der alte Commit aus keiner Ref mehr erreichbar. GitHub kann
   ihn über die direkte Commit-Adresse noch eine Zeit lang ausliefern; endgültig entfernt ihn nur eine Anfrage
   beim GitHub-Support (oder Löschen und Neuanlegen des Repos).
7. **GS1** ist eine Verhaltensabweichung unter Linux im Sinne der Regel: Ein Aufruf, der vorher zum Stopp
   führte, liefert jetzt Werte (gleich auf beiden Plattformen). Entscheidung Opus: beibehalten, eingetragen als
   D21 Nr. 11 und im Kopf von linux-build.yml. Einverstanden?
