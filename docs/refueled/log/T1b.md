# T1b – test_pad / test_eventflag unter Windows

Verhaltensabweichung Linux: keine (nur `#ifdef _WIN32`-Zweige hinzugefuegt; Linux-Pfad `/tmp/<name>XXXXXX` per mkstemp bleibt identisch)

## Aenderungen (nur tests/)

| alt (Datei:Zeile, Code) | neu | Linux-Gleichheit |
|---|---|---|
| test_pad.c:23 `char path[]="/tmp/bbport-pad-test-XXXXXX";` | `char path[512]; test_tmp_path(path,sizeof path,"bbport-pad-test-");` | Nicht-Windows: Helfer macht `snprintf("/tmp/%sXXXXXX")` = gleicher String, danach gleiches mkstemp |
| test_pad.c:29 `/tmp/bbport-pad-config-XXXXXX` | analog mit Helfer | wie oben |
| test_eventflag.c:28 `/tmp/sacredfire-eventflag-XXXXXX` | analog mit Helfer | wie oben |
| test_eventflag.c:26 `runtime_start(1);` | davor unter `_WIN32`: `assert(pf_init()==0); assert(runtime_memory_init_layout()==0);` | nur Windows; Linux unveraendert |

Windows: Helfer nimmt TMP, sonst TEMP, sonst "."; Muster `<dir>\<name>XXXXXX`. mkstemp, setenv (Gap-Schicht), usleep, unlink sind unter MinGW/CLANG64 vorhanden; keine weiteren Aenderungen noetig.

## Ergebnis
- test_pad: PASS ("PASS: pad ABI, debug camera chord, ... stick calibration")
- test_eventflag: PASS ("PASS: event flag contracts")
- Keine Runtime-Fehler gefunden.

Hinweis Build: bei Aufruf von build-windows.sh mit Pipe auf stderr scheitern alle Tests an `/dev/stderr: No such file` (Zeile 258/test_log); Ausgabe daher in Datei umgeleitet (`> log 2>&1`). Kein Skriptfehler im Test, ggf. fuer Opus: Skript nutzt /dev/stderr.

## Review Opus
Trenner im Temp-Pfad korrigiert: "\%" ist keine gueltige C-Escape-Sequenz (wurde zu "%", Pfad ohne Trenner); jetzt "/".
