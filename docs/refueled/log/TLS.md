# Log TLS: statische fs:/gs:-Analyse

Verhaltensabweichung Linux: keine (nur neues Werkzeug `tools/tls_scan.py` und Doku; kein bestehender Code geändert)

Geänderte Dateien: `tools/tls_scan.py` (neu), `docs/refueled/audit/TLS-SCAN.md` (neu), `docs/refueled/log/TLS.md` (neu).
Tabelle alt/neu: entfällt (keine Linux-wirksame Stelle geändert).

## Befehle

```
python -I tools/tls_scan.py --game C:\CUSA13795 --json "C:\CTR Reload\tmp\refueled\TLS\out.json"
```
Disassembler `C:\msys64\clang64\bin\llvm-objdump.exe` (per stdin, kein Spielbyte auf Platte). Laufzeit ca. 15 s.
Temp nur unter `C:\CTR Reload\tmp\refueled\TLS\` (out.json/out.txt enthalten nur Zählungen, Formen, Offsets).

## Ergebnis (APP_VER 01.00)

- eboot: 3748 fs-Zugriffe im Code, alle `mov rax, fs:[0]` (9 Byte), abgedeckt; entspricht der rohen Mustersuche aus link_libc.py (3748).
- libc.prx, libSceFios2.prx, libSceNpToolkit2.prx: 0 Segmentzugriffe im Code.
- 0 nicht abgedeckte, 0 originale GS-Zugriffe im Code; 32208 Scheintreffer ausschließlich im Datenbereich hinter dem letzten FDE-Bereich.
- Details und Methodik: `docs/refueled/audit/TLS-SCAN.md`. Mit 1.21 wiederholen.
- Nicht gebaut/getestet: nur Python, lief durch. Kein Commit.

## Review Opus
Keine Spielbytes in Skript, Bericht oder Log (Prüfung auf Byte-/Hexfolgen). Ergebnis plausibel: rohe Mustersuche = 3748 = Disassembler-Treffer im Code, also auch keine Treffer des 9-Byte-Musters im Datenbereich (die Rohsuchen von link_libc.py und pf_tls_patch_image laufen über das ganze X-Segment).
