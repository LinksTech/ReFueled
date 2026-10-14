# ReFueled – Hypothesen-Protokoll für Abstürze ab M2 (verbindlich, Stand 2026-10-13)

Vorgabe Projektleitung 2026-10-13: Bei einem Absturz bis zu **3 Hypothesen parallel** in getrennten
Worktrees prüfen, nicht nacheinander.

## 1. Erste Minute (Opus)
1. Lauf mit `BB_CRASH_DETAIL=1` (unter Windows Standard in run-windows.sh, Paket DBG) und, wenn der
   Absturz in oder nach einem Import liegt, `BB_TRACE_IMPORTS=<Teilname>`. Log nach
   `C:\ReFueled\tmp\M2\<Lauf-ID>\` (keine Spieldaten, nur der Loader-Bericht).
2. **Pflicht-A/B Red Zone (D17):** derselbe Lauf mit `BB_REDZONE=0` und `=1`, bevor andere
   Fehlersuche beginnt. Ändert sich das Bild, ist RZ Hypothese 1.
3. Einordnen nach dem Crash-Detail-Bericht: Gast-RIP + Image-Offset, Fault-Adresse und Zugriffsart,
   Thread-Name, letzte Imports, Gast-Stack mit image+0x…-Werten.

## 2. Hypothesen bilden und verteilen
- Höchstens 3 Hypothesen, jede **eine Zeile**: Ursache, erwarteter Beleg, entscheidender Versuch.
  Typische Klassen (Reihenfolge = Erfahrungswert):
  1. TLS/GS (fehlende Instruktionsform, audit/TLS-SCAN.md), 2. ABI (Host-Funktion ohne
  `sysv_abi`, xmm6–15/rdi/rsi), 3. Adresse ≥ 1 TiB im Gast (Low-Heap-Lücke, LOWHEAP.md),
  4. Speicher/Platzhalter (Split, Schutz, transient fault, K1), 5. Stack (TEB-Grenzen, `__chkstk`,
  Guard-Seite, D7), 6. POSIX-Gap (Fehlercode/Pfad, F6), 7. Red Zone (D17).
- Je Hypothese ein Worktree `C:\ReFueled\wt\M2-<Lauf-ID>-H<n>` auf Branch
  `refueled/m2/<Lauf-ID>-h<n>` vom aktuellen `refueled/windows`; je ein Sonnet-Agent, **alle in
  derselben Nachricht gestartet**. Auftrag je Agent: die Hypothese, genau ein Versuch (Instrumentierung
  oder Minimaländerung), Abbruch nach höchstens 2 Läufen, Ergebnis "bestätigt / widerlegt /
  unklar" mit Logbeleg. Keine Fixes außerhalb der eigenen Hypothese, kein Commit (Opus).
- Läufe mit dem Spiel nur nach `game_check.py` sauber (Dump 1.21); BB_GAME_DIR nur lesen.
- Testumfang nach CLAUDE.md: jeder Lauf beantwortet genau eine offene Frage; mehr als 10 Läufe
  je Runde werden vorher begründet.

## 3. Auswertung (Opus)
- Bestätigte Hypothese: Fix als eigenes Paket (Besitzer laut owners.txt), Pflichtfeld
  "Verhaltensabweichung Linux", Merge, dann ein Kontrolllauf.
- Alle widerlegt: nächste 3 Hypothesen aus den Belegen der ersten Runde; nach zwei Runden ohne
  Ergebnis Bericht an die Projektleitung mit allen Belegen.
- Jede Runde hier unten eintragen: Lauf-ID, Symptom (1 Zeile), H1–H3, Ergebnis, Fix-Commit.

## 4. Protokoll

| Lauf-ID | Symptom | H1 | H2 | H3 | Ergebnis | Fix |
|---|---|---|---|---|---|---|
| – | – | – | – | – | – | – |
