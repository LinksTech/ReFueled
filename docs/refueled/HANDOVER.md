# ReFueled – Übergabe (Stand 2026-10-14 abends)

Für eine neue Sitzung, die ohne die bisherige Unterhaltung weitermacht. Zuerst `C:\ReFueled\CLAUDE.md`
(nicht versioniert) lesen, dann diese Datei, dann [AGENT-REGELN.md](AGENT-REGELN.md),
[ARCHITECTURE.md](ARCHITECTURE.md) und [TASKS.md](TASKS.md).

## 1. Ort und Stand

| Was | Wo / Wert |
|---|---|
| Repo | `C:\ReFueled\repo`, Branch `refueled/windows` |
| Remote `refueled` | öffentliches GitHub-Repo ReFueled, `main` = `refueled/windows`; nur nach jeder Welle pushen, nie force |
| Remote `sacredfire-gone` | früher `origin` = SacredFire upstream; Repo existiert nicht mehr (Abschnitt 7), Remote umbenannt statt gelöscht |
| Worktrees | `C:\ReFueled\wt\<ID>`; derzeit keiner (T1 gelöscht, siehe Abschnitt 7) |
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
  in Quarantäne genommen (Quarantäne-Einträge 21:22:03, 21:22:43 und, nach Einrichtung der Ausnahme für
  `repo\out` und `wt`, erneut 21:43:58), der Test selbst lief im alten Ort grün. Frage 1.
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

1. **McAfee** entfernt `test_pad.exe` im neuen Ort auch nach eingerichteter Ausnahme für `C:\ReFueled\repo\out`
   und `C:\ReFueled\wt` (21:43:58). Die Ausnahme greift für diesen Zugriff nicht (vermutlich eine andere
   McAfee-Komponente als der Echtzeit-Scan, oder die Ausnahme wird erst nach Neustart des Dienstes wirksam).
   Bis dahin ist der build.sh-Testsatz 5/6, und die alten Orte sind nicht gelöscht (Umzug Schritt 4/5 offen).
2. **Dump 1.21**: weiterhin fehlend; M1 hängt daran.
3. **Alter GitHub-Commit**: Nach dem force push ist der alte Commit aus keiner Ref mehr erreichbar. GitHub kann
   ihn über die direkte Commit-Adresse noch eine Zeit lang ausliefern; endgültig entfernt ihn nur eine Anfrage
   beim GitHub-Support (oder Löschen und Neuanlegen des Repos). Entscheidung der Projektleitung offen.

## 7. Entscheidungen der Projektleitung (2026-10-14 abends)

- **GS1** als D21 Nr. 11 abgenommen (fehlende Funktion ergänzt, kein Verhaltensbruch).
- **Worktree T1** gelöscht. Dabei fiel auf: Der Branch `refueled/pkg/T1` stand noch auf der Historie vor der
  ersten Namensbereinigung (nur lokal, nie gepusht) und hielt die alten Objekte am Leben. Branch gelöscht,
  danach reflog expire + gc; im neuen Repo gibt es nur noch `main` (SacredFire), `refueled/windows` und die
  Upstream-Tags. T1 selbst ist über b9e2d2d done.
- **`%APPDATA%\shadPS4`** bleibt (dort lag schon vorher etwas); entfernt werden nur Dateien, die nachweislich aus
  unseren Testläufen stammen.
- **Upstream SacredFire ist nicht mehr erreichbar** (geprüft 2026-10-14): `T3CHNOLOG1C/SacredFire` liefert 404,
  ohne Umleitung (GitHub leitet umbenannte oder verschobene Repos weiter, also gelöscht oder privat). Unter den
  61 öffentlichen Repos des Kontos ist kein Nachfolger; die GitHub-Suche nach dem Namen findet nur fremde
  Projekte auf SacredFire-Basis (u. a. ein weiterer Windows-Fork `GetLostIn/CTR-NitroFueled-PC`, ein UFC-3-Port),
  keinen umgezogenen Upstream. Folgen: Remote `origin` → `sacredfire-gone` umbenannt (nicht gelöscht), Vermerk
  im README (Credits unverändert). publish_check nimmt die Upstream-Historie jetzt über
  `refs/remotes/sacredfire-gone/main` und zusätzlich über den festen Commit `f1f7935` (letzter Upstream-Stand)
  von der Prüfung aus, damit Pushes nicht an der fremden Upstream-Historie hängen bleiben.
