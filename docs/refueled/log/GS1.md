# GS1 – scePthreadAttrGetstack (2026-10-14)

Anlass: Der 1.00-Rauchtest (log/M1-RAUCHTEST-100.md) stoppte am ersten nicht unterstützten Import
`-quPa4SEJUw#libkernel` = `scePthreadAttrGetstack`, Aufrufer libSceNpToolkit2 (1.00). Ausgeführt und
geprüft von Opus.

- `src/runtime_thread.c`: `ThreadAttr` bekommt `stack_addr`/`stack_bytes` (der Stack, auf dem ein Thread
  wirklich läuft). `create()` trägt die Low-Map des Gast-Threads ein (Stack + 256 KiB Rand, wie gemappt),
  `runtime_thread_set_stack()` den Hauptthread-Stack aus probe.c, Host-Threads ohne eigenen Eintrag
  bekommen in `scePthreadAttrGet` auf sich selbst `pf_thread_get_stack()`.
- `scePthreadAttrGetstack(attr, &addr, &size)`: Attribute aus `scePthreadAttrGet` liefern den echten
  Stack, frische Attribute NULL und die eingestellte Größe (wie shadPS4 `posix_pthread_attr_getstack`).
  Fehler: ungültiges Attribut oder NULL-Zeiger = 0x80020016.
- `scePthreadAttrGetstacksize` und alle übrigen Attribut-Funktionen sind unverändert (die Größe bleibt die
  angeforderte).
- Test: `tests/test_runtime.c` (libc_support): Stack des Hauptthreads enthält eine lokale Variable,
  NULL-Zeiger = EINVAL, frisches Attribut = (NULL, 1 MiB). build-windows.sh --test: 11/11 + 6/6 grün.

| alt | neu | Linux-Gleichheit |
|---|---|---|
| Import `-quPa4SEJUw#libkernel` unaufgelöst (Stopp beim Aufruf) | Runtime-Funktion | neuer Import, auf beiden Plattformen gleich |
| `ThreadAttr` ohne Stackadresse | zwei Felder mehr, nur von Getstack gelesen | bestehende Pfade unverändert |

Verhaltensabweichung Linux: keine in bestehenden Abläufen; zusätzlich beantwortet die Runtime jetzt einen
Import, der vorher zum Stopp geführt hätte (gilt für Linux und Windows gleich).
