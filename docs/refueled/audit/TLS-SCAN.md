# Audit TLS-SCAN: fs:/gs:-Segmentzugriffe im Spielcode

Stand 2026-10-13, Worktree `C:\nitro-ps4-wt\TLS` (Branch `refueled/pkg/TLS`). Werkzeug: `tools/tls_scan.py`.
Spielversion: **APP_VER 01.00** (aus `sce_sys/param.sfo`). **Mit 1.21 wiederholen** (`python tools/tls_scan.py --game <Pfad>`).

## Methode

- Eingaben: `eboot.bin` und die drei verlinkten Module `libc.prx`, `libSceFios2.prx`, `libSceNpToolkit2.prx`
  (`sce_module`). Parser aus `scripts/prepare.py` (`parse_self`); nur Programmheader vom Typ PT_LOAD mit X-Flag.
- Disassembler: `llvm-objdump` (MSYS2 CLANG64, LLVM 23), Intel-Syntax, **lineare Disassemblierung**. Die ausführbaren
  Segmente werden im Speicher zu einem Mini-ELF verpackt und per stdin-Pipe übergeben; es wird keine Spieldatei kopiert
  und keine Disassembly gespeichert. Ausgegeben werden nur normalisierte Instruktionsformen, Zählungen, Segmentindizes
  und Offsets. Displacements mit Betrag über 0xFFFF erscheinen nur als `large`.
- Jede Instruktion mit fs- oder gs-Überschreibung wird auf Mnemonic, Operandenarten/-größen, Segment, Displacement und
  Basis-/Indexbeteiligung normalisiert. "Abgedeckt" heißt: `mov rax, fs:[0]` mit genau 9 Byte (die Form, die
  `link_libc.py`/`link_modules.py` auf GS und `pf_tls_patch_image` auf `gs:[0x1480+8*slot]` umschreiben).
- **Code/Daten-Trennung.** Das X-Segment (eboot: 0x21fb5d0 Byte) enthält hinter dem Code noch Nur-Lese-Daten. Lineare
  Disassemblierung liest dort Text und Tabellen als Instruktionen und erzeugt massenhaft Scheintreffer (ASCII-Byte-Muster
  als Segmentpräfix). Deshalb wird jede Fundstelle gegen die Funktionsbereiche aus `PT_GNU_EH_FRAME` (FDE-Tabelle und
  `pc_range`) geprüft: "in FDE" = echter Code, "hinter dem letzten FDE-Bereich" = Daten. Lücken zwischen FDE-Bereichen
  (Code ohne FDE) hätten als eigene Klasse gezählt: es gibt keine (0 in allen vier Dateien).
- Plausibilisierung gegen `link_libc.py`: die rohe 9-Byte-Mustersuche (wie dort) ergibt im eboot 3748 Treffer, identisch
  mit den 3748 per Disassembler gefundenen, in FDE-Bereichen liegenden Treffern der Form `mov r64, fs:[0]` (Zielregister
  jeweils rax, Länge 9). Erwartete Ausgabe von `link_libc.py`: `fs->gs patched=3748`. Das Skript wurde dafür nicht
  gestartet. In den Modulen ist die Zahl 0 (auch `link_modules.py` würde 0 melden).

## Ergebnis (APP_VER 01.00)

| Form | Datei | Anzahl | abgedeckt | Bewertung |
|---|---|---|---|---|
| `mov r64(rax), m64 fs:[disp]`, disp 0, 9 Byte | eboot.bin, in FDE | 3748 | ja | wird von link_libc.py auf GS umgeschrieben, unter Windows von pf_tls_patch_image auf den TLS-Slot |
| beliebige fs:-Formen, in FDE | eboot.bin | 0 | n. a. | keine nicht abgedeckte Form im Code |
| beliebige gs:-Formen (Original-GS), in FDE | eboot.bin | 0 | n. a. | kein GS-Zugriff im Originalcode |
| fs:/gs:-Scheintreffer hinter dem letzten FDE-Bereich (0x1ada1bd): 10516 fs, 19933 gs, 560 verschiedene Formen | eboot.bin | 30449 | nein | Daten, kein Code (Text/Tabellen als Instruktionen gelesen); bedeutungslos |
| beliebige fs:/gs:-Formen, in FDE | libc.prx | 0 | n. a. | keine Segmentzugriffe im Code |
| Scheintreffer hinter letztem FDE-Bereich (0xcf12c): 338 fs, 324 gs, 63 Formen | libc.prx | 662 | nein | Daten |
| beliebige fs:/gs:-Formen, in FDE | libSceFios2.prx | 0 | n. a. | keine Segmentzugriffe im Code |
| Scheintreffer hinter letztem FDE-Bereich (0x490b3): 263 fs, 354 gs, 47 Formen | libSceFios2.prx | 617 | nein | Daten |
| beliebige fs:/gs:-Formen, in FDE | libSceNpToolkit2.prx | 0 | n. a. | keine Segmentzugriffe im Code |
| Scheintreffer hinter letztem FDE-Bereich (0x8e862): 149 fs, 331 gs, 42 Formen | libSceNpToolkit2.prx | 480 | nein | Daten |

FDE-Abdeckung: eboot 135468 FDE (27232317 Byte Funktionsbereich), libc 2816, libSceFios2 727, libSceNpToolkit2 2610.
Instruktionen linear gelesen: eboot 10495911, libc 338387, Fios2 108934, NpToolkit2 211229; `(bad)`: 0.

## Nicht abgedeckte Offsets im Code

Keine (0 Stellen). Die Liste der Scheintreffer in den Datenbereichen (zusammen 32208) wird bewusst nicht aufgeführt; sie
sind über `--json` abrufbar (Felder `vaddr`, `region`).

## Fazit

Im Code von eboot und den drei Modulen kommt fs: ausschließlich als 9-Byte-`mov rax, fs:[0]` vor (3748 Stellen, alle
abgedeckt); gs: und andere fs:-Formen treten im Code nicht auf. Die TLS-Umschreibung des Ports ist für Version 01.00
also vollständig, soweit die FDE-Tabellen den gesamten Code erfassen (Code ohne FDE gibt es nach der Lückenprüfung nicht).
Grenzen: lineare Disassemblierung kann in Code mit eingestreuten Daten desynchronisieren, und zur Laufzeit erzeugter
oder per Sprungtabelle erreichter Code ohne FDE wäre nicht sichtbar.

Mit 1.21 wiederholen: `python -I tools/tls_scan.py --game <Pfad zu 1.21>`.
