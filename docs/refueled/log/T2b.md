# Paket-Log T2b (Portabilitätsfixes in scripts/*.py und .gitattributes, Sonnet)

Verhaltensabweichung Linux: mods.py/patches.py lesen UTF-8 unabhängig von der Locale (D21 Nr. 10)

## Was gemacht
Alle Änderungen sind Kodierungs-/Zeilenende-Angaben; Logik und Linux-Ausgabe bleiben gleich.

| Datei:Zeile | Änderung | Warum unter Linux bytegleich |
|---|---|---|
| scripts/content_profile.py:18 | `write_text(…, encoding='utf-8', newline='\n')` | Linux schreibt ohnehin `\n` (keine Übersetzung); der Inhalt ist reines ASCII (`json.dumps` mit `ensure_ascii=True`), UTF-8 und ASCII sind identisch |
| scripts/link_libc.py:202 | wie oben (`libc-link.json`) | wie oben |
| scripts/link_modules.py:246 | wie oben (`link.json`) | wie oben |
| scripts/prepare.py:259 | wie oben (`profile.txt`) | wie oben |
| scripts/prepare.py:271 | `newline='\n'` ergänzt (`encoding='utf-8'` stand schon da; `analysis.json`) | wie oben; Nicht-ASCII (™) wurde schon als UTF-8 geschrieben |
| scripts/mods.py:65 | `read_text(encoding='utf-8')` (`mods.json`) | Lesen: auf Linux mit UTF-8-Locale identisch; bei `LANG=C` (ASCII) wäre ein Nicht-ASCII-Name vorher ein Fehler, jetzt richtig |
| scripts/patches.py:34 | `read_text(encoding='utf-8')` (`bbport.ini`) | wie mods.py |
| scripts/patches.py:88 | `read_text(encoding='utf-8')` (`patches.json`) | wie mods.py |
| `.gitattributes` (neu) | `*.sh *.py *.c *.h *.cpp *.S *.inc *.nix text eol=lf`; `*.ttf *.otf *.png *.jpg *.ico *.bin *.spv *.zip *.gz binary`; `/gpu/third_party/** /third_party/** /gpu/shadps4/** -text` | Linux checkt LF aus und hat LF im Index; es ändert sich kein Index-Inhalt und keine Arbeitsdatei |

Nicht umgesetzt (nicht in `scripts/*.py` oder nicht Teil der Aufgabe):
- Befund 21 / `tools/gen_import_names.py:38, 54, 58` (`read_text`/`write_text`): liegt in `tools/`, nicht in `scripts/`; gleicher Fix (`encoding='utf-8'`, `newline='\n'`) noch offen.
- Befund 11 (`prepare.py:255`, `rglob` und Junction/Symlink): ist eine Festlegung (Zählung der Ressourcen), kein Kodierungsfix. Offen.
- `bbport.ini` CRLF (`bbport_settings.cpp:304`), mods.py-Linklogik (Befunde 8-10), run.sh, build.sh: laut Auftrag nicht angefasst.

`.gitattributes`: Warum die Ausnahmen? `git ls-files --eol` zeigt 32 Dateien, die schon **mit CRLF im Index** liegen: `gpu/third_party/half/**` (Fremdbibliothek),
`gpu/shadps4/shader_recompiler/ir/position.h` (Upstream-Datei) und `docs/refueled/audit/GPU-MINGW.md` (Dokument aus einem früheren Paket, von `*.md` nicht erfasst und deshalb unverändert).
Ein `text eol=lf` auf diesen Dateien würde sie sofort als geändert anzeigen. Darum sind `gpu/third_party`, `third_party` und `gpu/shadps4` auf `-text` gesetzt (Bytes bleiben, wie sie sind).
`gpu/third_party/sirit/externals/SPIRV-Headers/.gitattributes` bleibt gültig (tieferes Verzeichnis gewinnt). Die drei Submodule (`gpu/third_party/fsr-vulkan`, `imgui`, `third_party/LibAtrac9`) sind nicht betroffen.
Binär im Repo ist nur `gpu/third_party/fonts/DejaVuSans.ttf` (schon `binary` gesetzt); die übrigen `binary`-Zeilen sind Vorsorge.

## Abnahme

### 1. Linux (WSL, Python 3.12.3), `python3 -m unittest discover -s tests -p 'test_*.py'` im Worktree
Vorher (unveränderter Stand ed08c6c): `Ran 87 tests … OK (skipped=28)`. Nachher: `Ran 87 tests … OK (skipped=28)`. Die 28 Übersprungenen brauchen Linux-Binaries
(`test_runtime`, `test_probe`, `test_sema`, Teile von `test_content`, `test_pad`…), vorher wie nachher gleich.

Byte-Vergleich der geänderten Schreibstellen unter Linux mit synthetischen Daten (`C:\CTR Reload\tmp\refueled\T2b\sites.py`; Stand ed08c6c aus `git archive` gegen neuen Stand):

```
libc-link.json        sha256 46670940fa3f394c  alt == neu (cmp bytegleich)
content-profile.json  sha256 5accded1138d2a52  alt == neu (cmp bytegleich)
content.bin           sha256 40ef219b7e3f14ce  (unverändert)
```

### 2. Windows (Python 3.13.15, `python -m unittest …`, cwd im Worktree, `TMP`/`TEMP` = `C:\CTR Reload\tmp\refueled\T2b\tmp`)
| Test | Vorher | Nachher |
|---|---|---|
| prepare | 6 OK | 6 OK |
| patches | 7 OK | 7 OK |
| game_check | 8 OK | 8 OK |
| link_libc | 7 OK | 7 OK |
| content | 5 OK (3 übersprungen) | 5 OK (3 übersprungen) |
| import_names | 2 OK | 2 OK |
| steam_shortcuts | 4 OK | 4 OK |
| mods | 10 Fehler + 1 Fehlschlag (Symlinks/WSL-bash, Befunde 8, 12) | gleich |

Zusatzlauf `sites.py` (Windows) zeigt die Wirkung auf die Zeilenenden:

| Datei | Stand ed08c6c (Windows) | neu (Windows) | Linux (alt und neu) |
|---|---|---|---|
| libc-link.json | CR=29 / LF=29, sha 1c302e50f415c5ab | **CR=0** / LF=29, sha 46670940fa3f394c | CR=0, sha 46670940fa3f394c |
| content-profile.json | CR=13 / LF=13, sha 0f5f07ebbb9009d4 | **CR=0** / LF=13, sha 5accded1138d2a52 | CR=0, sha 5accded1138d2a52 |

Windows neu und Linux liefern damit **dieselben Bytes** (per `cmp` bestätigt).

### 3. Git
`git diff --stat`:
```
 scripts/content_profile.py | 2 +-
 scripts/link_libc.py       | 2 +-
 scripts/link_modules.py    | 2 +-
 scripts/mods.py            | 2 +-
 scripts/patches.py         | 4 ++--
 scripts/prepare.py         | 4 ++--
 6 files changed, 8 insertions(+), 8 deletions(-)      (+ .gitattributes neu)
```
`git check-attr -a run.sh scripts/prepare.py src/runtime.c` : jeweils `text: set`, `eol: lf`.

`git ls-files --eol | awk '{print $1,$2}' | sort | uniq -c` (Index/Arbeitsbaum):
```
      3 i/ w/                 (Submodule)
      1 i/-text w/-text       (DejaVuSans.ttf)
     32 i/crlf w/crlf         (Fremd-/Upstream-Dateien und GPU-MINGW.md, siehe oben; unverändert)
     72 i/lf w/crlf           (SPIRV-Headers: Arbeitsbaum CRLF, Index LF; eigene .gitattributes dort)
    727 i/lf w/lf
      2 i/none w/none
```
**Im Index liegen 32 Dateien mit CRLF**, keine davon unter `src/`, `scripts/`, `tests/`, `tools/`, `*.sh`. Der Auftragssatz "nichts mit CRLF im Index" gilt für die Eigenteile, nicht für diese Altlasten.

## Offen
- `tools/gen_import_names.py` (Kodierung, CRLF) und `bbport_settings.cpp` ("w" statt "wb") brauchen jeweils eigene Anweisung.
- Entscheidung Befund 11 (Ressourcenzählung in `analysis.json`).
- `docs/refueled/audit/GPU-MINGW.md` hat CRLF im Index.

## Prüfung Opus 2026-10-13 (Pflichtfeld)
Feld bei der einmaligen Durchsicht aller Logs gesetzt und gegen die alt↔neu-Tabelle geprüft.
