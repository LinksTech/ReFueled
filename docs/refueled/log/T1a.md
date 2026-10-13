# T1a – tests/test_runtime.c unter Windows

Verhaltensabweichung Linux: keine (die neuen Aufrufe pf_init() und runtime_memory_init_layout() sind unter Linux laut src/runtime.h bzw. pf_base.h nur der mallopt-Aufruf aus probe.c main, bzw. ein No-op; zusätzlich ist es nur ein Test, kein Laufzeitcode)

Stand 2026-10-13, Branch `refueled/pkg/T1a`, nicht committet. Eigene Dateien: `tests/test_runtime.c`, dieses Log.

## alt ↔ neu

| alt (Datei:Zeile, Code) | neu (Datei:Zeile, Code) | Linux-Gleichheit |
|---|---|---|
| tests/test_runtime.c:2 `#include "runtime.h"` | tests/test_runtime.c:2-3 zusätzlich `#include "platform/pf_base.h"` | nur Deklaration von pf_init |
| tests/test_runtime.c:329 `int main(...) {` direkt `runtime_start(0);` | tests/test_runtime.c:330-331 davor `assert(pf_init()==0); assert(runtime_memory_init_layout()==0);` | Linux: pf_init setzt nur mallopt (Host-Objekte unter 1 TiB), Layout No-op; nur im Test. Ungebaut unter Linux, nur gelesen |

Weitere Anpassungen für Windows waren nicht nötig. Kein Runtime-Fehler gefunden.

## Ausgabe (gekürzt)

```
Runtime: direct allocation offset=0x0, size=16384 bytes, type=0
Runtime: mapping direct memory 0x200000, size=32768 failed (error 0x80020016)
Runtime: mapped direct memory 0000001000000000, size=16384, prot=0x3
PASS: callback lifecycle, guard ABI, mutex errors, shared direct memory, memory primitives, resolver scope
PASS test_runtime
tests (build.sh set): 1 run, 0 failed
```

## Beobachtung zu build-windows.sh (nicht geändert)

build-windows.sh:256 (und :222, :292) verwendet `test_log=/dev/stderr`. Wird das Skript mit stderr
auf eine Pipe gestartet (z. B. `bash.exe -lc "..." 2>&1 | tail`), gibt es "/dev/stderr: No such
file or directory" und alle Plattformtests melden FAIL (Compile-Redirect scheitert). Mit Umleitung
in eine Datei (`> build.log 2>&1`) läuft alles. Das ist eine Umgebungsfrage des Aufrufers; Vorschlag:
statt `2> /dev/stderr` bei unset keep_going einfach keine Umleitung verwenden.
