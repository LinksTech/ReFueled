# Log RZ2 + RZ1-offline (Funktionsliste, Red-Zone-Zählung)

Ausgeführt von Sonnet, Opus prüft und committet. Worktree `C:\nitro-ps4-wt\RZ2`, Branch `refueled/pkg/RZ2`. Nichts committet.

## Gebaut (nur neue Dateien)
- `tools/redzone_funcs.py` (RZ2): liest PT_GNU_EH_FRAME / `.eh_frame_hdr` (Encodings absptr, uleb, udata2/4/8, sleb, sdata2/4/8; pcrel, datarel),
  schreibt `redzone.bin` im von Opus festgelegten Format (`RZFL`, v1, count, image_size, sortierte u32-Offsets). Optionen `--walk`
  (.eh_frame CIE/FDE durchlaufen, Anzahl gegen Tabelle), `--summary`, zusätzlich `--union` (siehe offene Punkte 1). Wiederverwendbar:
  `function_starts(path, walk=False, union=False) -> (starts, image, exec_ranges, info)`. Einlesen über `scripts/prepare.py:parse_self`/`span`.
  Image = PT_LOAD (1) und 0x61000010 (wie `game_check.image_sha256`), Basis = kleinste vaddr; ausführbar = PT_LOAD mit PF_X.
- `tools/redzone_census.py` (RZ1 offline): capstone, linear von Start bis nächster Start bzw. Ende des Exec-Bereichs; nur Zahlen.
  Red-Zone-Befehl = Speicheroperand Basis rsp, kein Index, kein Segment, disp < 0; `lea` und `nop` zählen nicht. Liveness ist RZ3, nicht implementiert.
- `tests/refueled/rz_synth.py` (Hilfsmodul: synthetisches ELF64), `test_redzone_funcs.py` (8 Tests), `test_redzone_census.py` (2 Tests).

## Tests
`python -m unittest discover -s tests/refueled -p "test_redzone*.py"` aus dem Worktree: `Ran 11 tests ... OK`
(11 statt 10: ein Test für doppelte Tabelleneinträge). Geprüft: datarel|sdata4, udata8-Tabelle mit udata4-eh_frame_ptr, pcrel|sdata4, Sortierung,
Verwerfen eines Starts im rw-Segment, Duplikate, Walk gegen Tabelle, Dateiformat byte-genau, u32-Überlauf, Fehler ohne PT_GNU_EH_FRAME,
Zählung exakt (Blattfunktion, sub rsp ohne Red Zone, 4-Byte-Befehl, indirekter Sprung, push davor, Dekodierfehler, lea/positives disp zählen nicht).
Hinweis: `python -m unittest` ohne `discover -s tests/refueled` findet sie nicht (kein `__init__.py`, wie bei den anderen Tests dort).

## Zählung, Spiel Version **1.00** (CUSA13795 eboot.bin und sce_module/libc.prx)
Zwei Läufe je Datei: "Tabelle" = nur Starts aus der eh_frame_hdr-Tabelle (Vorgabe), "Union" = Tabelle plus FDEs, die nur der .eh_frame-Durchlauf findet.

| Kennzahl | eboot Tabelle | eboot Union | libc Tabelle | libc Union |
|---|---|---|---|---|
| Funktionen | 135468 | 154157 | 2816 | 3071 |
| Funktionen mit Red-Zone-Zugriff | 158 | 177 | 28 | 30 |
| davon nur schreibend / nur lesend / beides | 1 / 64 / 93 | 1 / 64 / 112 | 0 / 0 / 28 | 0 / 2 / 28 |
| Befehle mit Red-Zone-Operand | 3100 | 3100 | 239 | 239 |
| davon schreibend / lesend / beides / ohne Zugriffsangabe | 701 / 2332 / 64 / 3 | gleich | 106 / 132 / 1 / 0 | gleich |
| davon kürzer als 5 Byte | 412 | 412 | 114 | 114 |
| Längenverteilung (Byte: Anzahl) | 4:412 5:1423 6:1157 7:67 8:41 | gleich | 4:114 5:100 6:21 7:4 | gleich |
| RZ-Funktionen mit indirektem Sprung | 13 | 23 | 1 | 3 |
| RZ-Funktionen ohne sub rsp/push vor erstem Zugriff | 43 | 45 | 20 | 20 |
| davon zusätzlich ohne `call` | 43 | 44 | 20 | 20 |
| Befehle mit disp < -128 (außerhalb Red Zone) | 62 | 62 | 4 | 4 |
| Dekodierfehler (Bytes übersprungen) | 487914 | 500943 | 11738 | 11906 |
| Funktionen mit Dekodierfehler | 47 | 13348 | 6 | 224 |
| Laufzeit | 293066 ms | 81181 ms | 2125 ms | 2025 ms |

(Tabelle-Lauf der eboot ist langsamer, weil die letzte "Funktion" 2,6 MB ohne FDE-Eintrag umfasst und viele Dekodierfehler enthält.)

Größtes negatives Displacement je RZ-Funktion (obere Eimergrenze: Funktionen), Union:
- eboot: -8:26, -16:12, -24:26, -32:13, -40:10, -48:9, -56:8, -64:6, -72:3, -80:2, -88:2, -96:3, -104:2, -112:1, -120:7, -128:33, <-128:14.
- libc: -8:9, -16:4, -24:5, -40:2, -56:2, -72:2, -128:4, <-128:2.
(Tabelle-Lauf eboot: gleiche Form, <-128:1, -128:33.)

Beispiel-Offsets (Image-Offsets, 1.00): eboot 0x18c790 0x1da7c0 0x1dcf00 0x1eece0 0x27b6d0; libc 0x7abf0 0x7b130 0xadd90 0xb68b0 0xb7e20.

Einordnung (Zahlen, keine Liveness): in 154157 Funktionen nur 177 (0,11 %) mit Red-Zone-Zugriff, 3100 Befehle, davon 412 kürzer als 5 Byte.
Die Zahl "disp < -128" und die hohen Dekodierfehlerzahlen deuten auf Daten/Sprungtabellen im Code bzw. auf falsch gelesene Bytes hin; die
Red-Zone-Zahlen sind deshalb eine Obergrenze mit Rauschen (lineares Dekodieren, kein Kontrollfluss).

## Abgleich mit pm4_scan
`tools/re/pm4_scan.py` meldet in der ersten Ausgabezeile `functions (eh_frame): N` = Zahl der Tabelleneinträge (kein Filter, keine Dedup). Lauf auf dem aus
eboot.bin entpackten ELF (temporär, gelöscht): **135468**, identisch mit unserer Tabellenzahl (135468, 0 verworfen, 0 Duplikate). Gleiche Zählweise.

## Laufzeit
Siehe Tabelle. Dekodierung des Python-capstone-Laufs: eboot Union 81 s, libc ~2 s. Für einen Einbau in den Loader ist das nur offline brauchbar.

## Verhaltensabweichung Linux: keine
(Nur neue Dateien: `tools/redzone_funcs.py`, `tools/redzone_census.py`, `tests/refueled/rz_synth.py`, `test_redzone_funcs.py`, `test_redzone_census.py`, dieses Log.
`git status` zeigt nur `??`-Einträge; prepare.py, link_modules.py, run*.sh, src/, gpu/, Header unangetastet.)

## Offene Punkte / Fragen an Opus
1. **Die eh_frame_hdr-Tabelle ist unvollständig.** `.eh_frame` enthält mehr FDEs als die Tabelle: eboot 154157 gegen 135468 (18689 mehr), libc 3071 gegen 2816 (255 mehr).
   Alle fehlenden liegen hinter dem letzten Tabellenstart (eboot ab 0x1ce477c, Tabelle endet bei 0x1ada03e). Folge: mit der reinen Tabelle (Vorgabe, shadPS4-Verfahren)
   hat der Tail-Code keine Funktionsgrenzen, und `pm4_scan` übersieht ihn ebenfalls. Ich habe `--union` ergänzt (nicht verlangt, Standard bleibt reine Tabelle);
   `redzone.bin` mit Union enthält die zusätzlichen Starts. Entscheidung Format/Standard: Opus.
2. Dekodierfehler sind zahlreich (Daten im Code?). Für RZ3 zu klären, ob Funktionsbereiche mit Sprungtabellen/Inline-Daten auszuschließen sind.
3. Image-Definition: PT_LOAD plus 0x61000010 (SCE_RELRO) wie `game_check.py`; die kleinste vaddr ist in beiden Dateien die des r-x-Segments (Offset 0), das Mitnehmen von 0x61000010 ändert nur image_size nicht, da es innerhalb der letzten PT_LOAD endet (nicht gesondert geprüft).
4. Nur Version 1.00 gemessen; 1.21 steht aus (Zahlen nicht übertragbar).
5. `tests/refueled` hat keine `__init__.py`; Aufruf nur mit `discover -s tests/refueled`.
6. Temp-Ordner `C:\CTR Reload\tmp\refueled\RZ2\` ist geleert (enthielt redzone-Ausgaben und das entpackte ELF).

## Review Opus
Neue Werkzeuge ohne Einbindung, nichts Bestehendes geändert (Linux: keine Abweichung bestätigt). Ausgabe nur Zahlen,
höchstens 5 Beispiel-Offsets, keine Spielbytes. Entscheidungen: (1) Standard für `redzone_funcs.py`/`redzone_census.py`
ist jetzt Tabelle + `.eh_frame`-Durchlauf (die Suchtabelle von CTR endet vor 18689 FDEs); `--table-only` für den
Abgleich mit pm4_scan. Die Bibliotheksfunktion `function_starts()` behält `union=False` als Parameter-Standard
(Tests). (2) Dekodierfehler/Inline-Daten: Klärung in RZ3 (Kontrollfluss-Dekodierung statt linear), die Zahlen
hier sind Obergrenzen. (3) 0x61000010 zählt zum Image wie in game_check.py. Mit 1.21 wiederholen.
Ausgeführt von Sonnet, geprüft von Opus.
