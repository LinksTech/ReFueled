# ReFueled – Regeln für Paket-Agenten (verbindlich, Stand 2026-10-13)

- Repo `C:\nitro-ps4-port` nur lesen. Arbeit nur im eigenen Worktree `C:\nitro-ps4-wt\<ID>`
  (Branch `refueled/pkg/<ID>`, von Opus angelegt). Nur die im Auftrag genannten Dateien ändern.
  Header `src/platform/pf_*.h`, `src/runtime.h`, `gpu/bbgpu.h` gehören Opus: Bedarf melden, nicht ändern.
- Nicht committen, kein `git config`, kein push, keine Remotes, nichts installieren, nichts
  herunterladen, keine Workflows/Multi-Agent-Werkzeuge.
- **Keine Personen namentlich nennen** (feste Regel 2026-10-14, öffentliches Repo): in keinem Dokument, Log,
  Kommentar, Commit oder Code. Auftraggeber = "Projektleitung" (Tabellen "PL", englisch "the project lead").
  Keine Benutzernamen, Rechnernamen, E-Mail-Adressen, keine Pfade `C:\Users\<name>` (stattdessen
  `%APPDATA%`, `C:\Users\…`). `publish_check.py` blockiert jeden Push mit solchen Angaben.
- **Kein WSL, kein Linux, keine VM** (Planänderung 2026-10-13). Linux-Code wird lokal weder gebaut
  noch getestet. Einzige Build-Umgebung: Windows mit MSYS2 CLANG64 (sobald installiert, Shell
  `C:\msys64\usr\bin\bash.exe -lc` mit `MSYSTEM=CLANG64`) und – nur für reine Win32-Dateien – MSVC
  über `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat`
  (mit `/link /HIGHENTROPYVA:NO /DYNAMICBASE:NO`).
- Spielordner `C:\CUSA13795`: nur lesen, nie verändern, nichts daraus kopieren, loggen oder committen.
  Nur benutzen, wenn der Auftrag es ausdrücklich sagt.
- Temporäre Dateien nur unter `C:\CTR Reload\tmp\refueled\<ID>\`; TMP/TEMP gestarteter Programme dorthin.
- Code und Kommentare englisch, Doku unter `docs/refueled/` deutsch. Neue Dateien: SPDX
  `GPL-2.0-or-later` + `refueled:`-Kommentar. Änderungen in `gpu/shadps4/**` minimal und jede Zeile
  mit `refueled:` markiert (D14); vom Upstream übernommene Teile mit Herkunft.
- **Linux-Verhalten bleibt gleich: nur verschieben/abstrahieren.** Da Linux lokal nicht gebaut wird,
  liefert jedes Paket im Log `docs/refueled/log/<ID>.md` eine Tabelle
  `| alt (Datei:Zeile, Code) | neu (Datei:Zeile, Code) | Linux-Gleichheit (Begründung: gleicher
  Systemaufruf/gleiche Flags/gleiche Reihenfolge/gleicher Meldungstext, über pf_*_linux.c:Zeile) |`
  für **jede** geänderte Linux-wirksame Stelle. Opus prüft sie beim Merge Zeile für Zeile.
- **Pflichtfeld im Paket-Log** (Regel Projektleitung 2026-10-13): eine eigene Zeile
  `Verhaltensabweichung Linux: keine` oder `Verhaltensabweichung Linux: <Beschreibung>`.
  Abweichung heißt: irgendein beobachtbarer Unterschied unter Linux gegenüber vorher, unter
  irgendeiner Bedingung (Gast-sichtbare Werte und Adressen, Fehlercodes, Dateien, Signale,
  Ausgaben/Logtexte, Zeitverhalten, auch nur in Fehlerpfaden oder seltenen Umgebungen). Reine
  Umformungen ohne beobachtbaren Unterschied (z. B. calloc statt malloc vor vollständiger
  Initialisierung) sind "keine", gern mit Begründung in Klammern. Im Zweifel beschreiben.
  Ein Eintrag ungleich "keine" ist eine Entscheidung für Opus, nie nur ein Log-Eintrag.
- Die Linux-Implementierungen stehen in `src/platform/pf_*_linux.c` (verschobener Code); vor dem
  Umstellen dort nachsehen, dass die pf-Funktion exakt den alten Aufruf macht. Weicht sie ab: nicht
  umstellen, sondern melden.
- Kompilierender Code ist Pflicht, sobald es geht. Solange CLANG64 fehlt: sorgfältig schreiben,
  Includes/Typen gegen die Header prüfen, im Log "ungebaut bis CLANG64" vermerken.
- Festhängen oder Architekturfrage: wörtlich melden und an Opus zurückgeben, nicht improvisieren.
- Schlussmeldung: geänderte Dateien, Prüfungen mit Ergebnis, Zahl der umgestellten Stellen,
  das Feld "Verhaltensabweichung Linux", offene Punkte, Schnittstellenbedarf.
