# GPU-Bibliothek unter MinGW (CLANG64): Vorab-Analyse für G2/G3

Stand 2026-10-13, Paket F5 (Teil 2). Rein statische Analyse des Worktree-Stands `refueled/pkg/F5`
(= `refueled/windows` ab f1f7935), **ohne Compiler**: CLANG64 ist noch nicht installiert. Alle
Aussagen über Header und Compilerverhalten sind Vorhersagen und tragen den Vermerk "vermutlich"
oder "prüfen", wo sie nicht aus dem Quelltext folgen.

Methode: Skript `C:\ReFueled\notes\scratch-2026-10\F5\scan.py` (Grep nach POSIX-/Linux-Includes, Aufrufen,
Registernamen, `/proc`), danach von Hand durchgesehen. "Upstream" heißt: die Zeile steht textgleich
(nach `strip`) in `C:\ReFueled\ref\shadPS4-b4e7ae7\src\<Pfad>`; "bbport" heißt: nicht in der
Referenz (eigene Zeile oder ganz eigene Datei unter `gpu/shim`). Der Referenzbaum hat CRLF, der
Worktree LF; das Skript vergleicht gestrippte Zeilen. Zeilennummern beziehen sich auf den Worktree.
`getenv("BB_…")` (über 150 Stellen in bbport-Zeilen) ist portabel (UCRT) und steht nicht in den
Tabellen.

Pakete: V1 `page_manager.*`; V2 liverpool, vk_scheduler, vk_rasterizer, vk_pipeline_cache,
vk_draw_pipe.h, buffer_cache.cpp, videoout/driver.cpp; V3 `bbport_guest_memory.*`, `buffer.*`,
`vk_instance.*`; S1 `bbport_free_check.*`, `bbport_guest_hooks.*`; S2 clobber, write_log,
wait_trace, threads.h, gnm_hooks, bbgpu.cpp, signals.h; S3 window, sdl_window, overlay.
Dateien ohne Paket sind mit "(neu)" markiert und mit Zuordnungsvorschlag versehen.

## 1. Was bbgpu übersetzt (aus `gpu/CMakeLists.txt`)

| Gruppe | Ordner | .cpp-Dateien | ca. Zeilen | Bemerkung |
|---|---|---:|---:|---|
| G2 | `shadps4/common` (Liste `COMMON`, ohne `shim/common/logging/log.cpp`) | 12 (+1 `ntapi.cpp` nur Windows, F5) | 2 600 | decoder, error, io_file, native_clock, number_utils, path_util, rdtsc, spin_lock, recursive_lock, string_util, thread, signal_context |
| G2 | `shadps4/shader_recompiler` | 72 | 28 800 | root 1, backend/spirv 19, frontend 7, frontend/translate 9, ir 12, ir/passes 24 |
| G3 | `shadps4/video_core` (ohne `renderdoc.cpp`) | 49 | 30 800 | root 2 (`page_manager`, `cache_storage`), amdgpu 4, buffer_cache 3, renderer_vulkan 29 + fsr411 1 + host_passes 2 = 32, texture_cache 8 |
| G3 | `shadps4/core/libraries` | 11 | 7 800 | gnmdriver 1, videoout 2, kernel/equeue 1, avplayer 6, videodec/video_utils 1 |
| S* | `gpu/shim` (`shim/*.cpp`, `shim/*/*.cpp`) | 15 | – | bbgpu, bbport_{clobber,copy,free_check,gnm_hooks,guest_hooks,guest_memory,heap_sites,overlay,settings,wait_trace,write_log}, common/assert, common/logging/log, window |
| – | `third_party/imgui` (5 .cpp) | 5 | – | Submodul, im Worktree **leer** (siehe Abschnitt 4, Punkt 9) |
| – | `third_party/sirit`, `third_party/fsr-vulkan` | statische Bibliotheken | – | sirit im Worktree vorhanden (135 Dateien); fsr-vulkan Submodul **leer** |

Summe bbgpu: 164 eigene .cpp (+ ntapi.cpp unter Windows). Nicht übersetzt (vom Linux-Build ebenfalls
nicht): `renderdoc.cpp`, `key_manager.cpp`, `memory_patcher.cpp`, `aes.h`-Nutzer, `va_ctx.h`-Nutzer.
Include-Reihenfolge: `shim` steht vor `shadps4`, also ersetzen `shim/common/logging/log.h`,
`shim/common/debug.h`, `shim/core/*` die Upstream-Header gleichen Namens (spdlog, Tracy, Emulator-
Einstellungen entfallen damit auch unter Windows).

`shader_recompiler/**` (G2, 72 Dateien) hat **keinen einzigen** POSIX-Treffer. Dort sind nur
Compiler-/Bibliotheksunterschiede zu erwarten (Abschnitt 4).

## 2. Linux-/POSIX-Spezifisches (Fundstellen)

Legende Spalte "Upstream/bbport": **U-ok** = Upstream-Zeile mit vorhandenem `_WIN32`-/Nicht-Linux-
Gegenzweig, vermutlich ohne Arbeit; **U-tot** = Upstream, steht in einem Zweig, der unter Windows
nicht übersetzt wird; **B** = bbport-Zeile (Arbeit im genannten Paket); **B-neu** = Datei komplett
bbport. MinGW-Verfügbarkeit der Header (vermutlich): vorhanden `unistd.h`, `pthread.h`, `sched.h`
(winpthreads), `signal.h` (ohne `sigaction`, `sigjmp_buf`, `SIGUSR*`), `sys/types.h`, `sys/stat.h`,
`sys/param.h`, `dirent.h`, `fcntl.h`, `x86intrin.h`; **fehlen**: `sys/mman.h`, `sys/uio.h`,
`sys/resource.h`, `sys/syscall.h`, `sys/ioctl.h`, `linux/*`, `poll.h`, `ucontext.h`, `dlfcn.h`,
`execinfo.h`.

### 2.1 G2 `common/` (alles Upstream)

| Datei:Zeile | Befund | Upstream/bbport | Paket | Vorschlag |
|---|---|---|---|---|
| common/thread.cpp:18-31, 66, 133-147, 191-205, 256 | pthread/sched/`pthread_*_np` im Nicht-Windows-Zweig; Windows-Zweig (172-182, 250) mit `SetThreadDescription`/`GetThreadDescription` | U-ok (Windows-Gegenzweig 82-124, 172-182, 246-256) | G2 | keiner; `_WIN32_WINNT=0x0A00` (F5) liefert die Deklarationen. Zeilen 187-212 (`pthread_setname_np`) sind unter MinGW tot |
| common/signal_context.cpp:7-44 | `ucontext_t`, `REG_RIP`, `REG_ERR` im Nicht-Windows-Zweig; Windows-Zweig nutzt `EXCEPTION_POINTERS` | U-ok | G2 (Aufrufer: S1/V1) | **Entwurfsfrage, kein Compilerfehler:** der Windows-Zweig erwartet `EXCEPTION_POINTERS*`, der Loader liefert `PfContext` (IF-2). `GetRip`/`IsWriteError` in `page_manager.cpp` müssen auf die Pf-Variante umgestellt werden (V1/S1) |
| common/rdtsc.cpp:8-23 | `_WIN64`-Zweig mit `GetSystemTimePreciseAsFileTime`; `clock_gettime` (36) im Nicht-Windows-Zweig | U-ok | G2 | keiner |
| common/io_file.cpp:13-19, 187-193, 225-243, 262-276, 300, 322, 344 | `_WIN32`: `_wfsopen`, `_wfopen_s`, `_commit`, `_chsize_s`; `_WIN64`: `NtSetInformationFile`, `CreateFileMapping2`, `_get_osfhandle`; `unistd.h`/`unlink`/`fsync`/`ftruncate` nur im Nicht-Windows-Zweig | U-ok | G2 | `CreateFileMapping2` (Zeile 268) vermutlich in den mingw-w64-Headern vorhanden (Win10 RS4+); fehlt sie: `refueled:`-Stub, `GetFileMapping()` hat keinen Aufrufer außerhalb der Datei. `_FILE_OFFSET_BITS=64` gesetzt (F5) |
| common/path_util.cpp:18-24, 104-107, 162-166 | `<Shlobj.h>` nur innerhalb von `#ifndef MAX_PATH`; `SHGetFolderPath` | U-ok | G2 | **prüfen**: ist `MAX_PATH` durch einen früheren `<windows.h>`-Include schon definiert, fehlt `<shlobj.h>` und `SHGetFolderPath` ist undeklariert. Fix: `#ifdef _WIN32 #include <shlobj.h> #endif` außerhalb des Guards (`refueled:`). `shell32` ist verlinkt (F5) |
| common/path_util.cpp:13-14 | `<dlfcn.h>`, `<sys/param.h>` | U-tot (`__APPLE__`) | G2 | keiner |
| common/path_util.cpp:89 | `BB_GPU_USER_DIR` | B | G2 | portabel; Windows nutzt den Ordner aus `bbgpu_init` (`setenv` in bbgpu.cpp:267, S2) |
| common/adaptive_mutex.h:7-33 | `pthread_mutex_*`, `PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP` | U-ok (`#if __unix__`; winpthreads definiert das Makro nicht, Rückfall-Klasse greift) | G2 | keiner |
| common/spin_lock.cpp:6-28 | `_MSC_VER`-Zweig `<intrin.h>`; sonst `<xmmintrin.h>` | U-ok | G2 | keiner |
| common/bit_array.h:12, va_ctx.h:9 | `<immintrin.h>`/`<xmmintrin.h>` | U-ok (clang liefert sie) | G2 | keiner |
| common/error.cpp:6-46 | `FormatMessageA` im `_WIN32`-Zweig, `strerror_r` sonst | U-ok | G2 | keiner |
| common/string_util.cpp:11-44, string_util.h:21 | `UTF8ToUTF16W`, `UTF16ToUTF8` nur `_WIN32` | U-ok | G2 | keiner |
| common/ntapi.h:6-554, ntapi.cpp | Eigene Deklarationen von `_PEB`, `_TEB`, `_UNICODE_STRING`, `_CLIENT_ID`, `_IO_STATUS_BLOCK` … nach `<windows.h>` | U-ok | G2 | **Risiko Neudefinition** gegen mingw-w64-Typen (`winnt.h` deklariert `struct _TEB` vorwärts, das Ausfüllen ist erlaubt; `UNICODE_STRING` & Co. stehen in `winternl.h`, das `<windows.h>` nicht lädt). Fehlerfall: `refueled:`-`#include`-Reihenfolge oder `#ifndef`-Klammern. `ntapi.cpp` fehlt in der Linux-Quellenliste und wird unter WIN32 ergänzt (F5), sonst Linkfehler für `NtSetInformationFile` aus io_file.cpp:263 |

### 2.2 G3 `core/` und `video_core` ohne V-Dateien

| Datei:Zeile | Befund | Upstream/bbport | Paket | Vorschlag |
|---|---|---|---|---|
| core/libraries/kernel/time.h:7 | `<sys/types.h>` | U-ok (MinGW hat ihn) | G3 | keiner |
| core/libraries/videoout/driver.cpp:10,12,13 | `<sys/resource.h>`, `<dirent.h>`, `<unistd.h>` unbedingt | B | V2 | `sys/resource.h` und `/proc` hinter Pf-Helfer (V2) |
| core/libraries/videoout/driver.cpp:307-310, 332 | `opendir("/proc/self/fdinfo")`, `readdir`, `closedir` (fd-Zählung Diagnose) | B | V2 | unter `#ifdef __linux__`, Windows: Zähler 0 |
| core/libraries/videoout/driver.cpp:335-341 | `/proc/self/statm`, `sysconf(_SC_PAGESIZE)` | B | V2 | `pf_*`-RSS-Funktion oder `#ifdef __linux__` |
| core/libraries/videoout/driver.cpp:414, 428 | `getrusage(RUSAGE_SELF)`, `clock_gettime(static_cast<clockid_t>(clock))` mit CPU-Zeit-Clock-ID | B | V2 | Pf-Funktion CPU-Zeit (`GetProcessTimes`/`pthread_getcpuclockid`) |
| video_core/buffer_cache/buffer_cache.cpp:33 | `<pthread.h>` | B | V2 | unkritisch (winpthreads) |
| video_core/buffer_cache/buffer_cache.cpp:595 | `pthread_getname_np` | B | V2 | winpthreads hat sie in neueren Versionen (vermutlich, prüfen); sonst Pf-Helfer `pf_thread_name` |
| video_core/buffer_cache/region_manager.h:15-33 | `#ifdef __unix__` + `PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP` | U-ok (Rückfall-Klasse) | G3 | keiner |
| video_core/amdgpu/liverpool.cpp:6-9 | `<pthread.h>`, `<sys/resource.h>`, `<sys/uio.h>`, `<unistd.h>` unbedingt | B | V2 | `sys/resource.h`, `sys/uio.h` hinter Pf |
| video_core/amdgpu/liverpool.cpp:123 | `pthread_getcpuclockid` | B | V2 | Pf-CPU-Zeit |
| video_core/amdgpu/liverpool.cpp:127-128, liverpool.h:198, 346 | `#ifdef __linux__` + `gettid()` | U-ok | G3 | keiner |
| video_core/amdgpu/liverpool.cpp:1054, 2343, 2396 | `process_vm_readv(getpid(), …)` (sicheres Lesen von Gastspeicher) | B | V2 | `pf_safe_read` (IF-2 "sicheres Lesen", K1) |
| video_core/amdgpu/liverpool.cpp:1810, 2244 | `getrusage(RUSAGE_THREAD)`, `gettid()` | B | V2 | Pf-Helfer |
| video_core/renderer_vulkan/vk_breadcrumbs.cpp:11-12 | `<sys/stat.h>`, `<unistd.h>` (Datei bbport-neu, nicht in V2) | B-neu | **G3** (kein Paket) | unkritisch, beide Header vorhanden; Rest prüfen beim Übersetzen |
| video_core/renderer_vulkan/vk_platform.cpp:261-270 | `setenv("VK_DRIVER_FILES", …)` | U-tot (`__APPLE__`) | G3 | keiner |
| video_core/renderer_vulkan/vk_platform.cpp:7-8, 66-77, 174-176 | `_WIN64` → `VK_USE_PLATFORM_WIN32_KHR`, `vk::Win32SurfaceCreateInfoKHR` | U-ok | G3 (S3 liefert HWND) | `window_info.render_surface` muss unter Windows eine `HWND` sein (S3) |
| video_core/renderer_vulkan/vk_instance.cpp:706-712 | `#if _WIN64 … eQueryPerformanceCounter`, `#elif __linux__` | U-ok | G3 | keiner |
| video_core/renderer_vulkan/vk_rasterizer.cpp:4264, vk_rasterizer.h:259 | `#ifdef __linux__` | U-ok | G3 | keiner |
| video_core/renderer_vulkan/vk_draw_pipe.h:24 `x86intrin.h` | Include | B | V2 | keiner (clang) |
| video_core/renderer_vulkan/vk_bind_helper.h:12 | `x86intrin.h` | B-neu | G3 | keiner |
| video_core/renderer_vulkan/vk_temporal_upscaler.cpp, vk_camera_motion.cpp, vk_fsr4.cpp, vk_frame_capture.cpp, fsr411/fsr411.cpp | nur `getenv` (30+ Stellen), `std::fopen`/Dateipfade als `std::string` | B-neu | G3 | keiner; Pfade mit `\` vs `/` beim Test beachten |
| video_core/texture_cache/*, host_passes/*, vk_graphics_pipeline.cpp, vk_compute_pipeline.cpp | keine POSIX-Treffer außer `getenv` | – | G3 | keiner |
| video_core/cache_storage.cpp:104-193 | `miniz` mit `path.string().c_str()` | U-ok | G3 | `string()` ist unter Windows die ANSI-Codepage; für ASCII-Cachepfade unkritisch |
| core/libraries/avplayer/* | `ffmpeg` über pkg-config; sonst nur `open`-Funktionszeiger-Namen | U-ok | G3 | keiner (ffmpeg 9 aus MSYS2; API-Drift gegen nixpkgs-Stand prüfen) |

### 2.3 V1 `page_manager.cpp` (23 bbport-Zeilen, 18 Upstream-Zeilen)

| Datei:Zeile | Befund | Upstream/bbport | Paket | Vorschlag |
|---|---|---|---|---|
| page_manager.cpp:19-24 | `<sys/uio.h>`, `<dlfcn.h>`, `<ucontext.h>`, `<unistd.h>` unbedingt | B | V1 | Windows: nichts davon; `Pf*`-Header |
| page_manager.cpp:29-34 | `#ifndef _WIN64 mman.h / adaptive_mutex.h #else windows.h` | U-ok | V1 | keiner |
| page_manager.cpp:36-49 | `#ifdef __linux__`: `fcntl`, `linux/userfaultfd.h`, `poll.h`, `sys/ioctl.h`, `sys/syscall.h` | B | V1 | bleibt unter `__linux__` (D13) |
| page_manager.cpp:83-85 | `dladdr` (Fault-Stelle benennen) | B | V1 | `pf_symbolize` |
| page_manager.cpp:104-123, 299 | `ucontext_t.gregs[REG_RIP/RBP/RSP]`, `process_vm_readv` | B | V1 | `PfContext`-Zugriffe + `pf_safe_read` |
| page_manager.cpp:468-597 | `mprotect`-Merkliste, `syscall(SYS_userfaultfd)`, `ioctl(UFFDIO_*)` | B | V1 | in `__linux__`; Windows-Pfad über `pf_vm_protect` (D13) |
| page_manager.cpp:636-640 | `BB_GUEST_GPU_MEMORY`, `BB_UFFD` | B | V1 | `BB_UFFD` unter Windows ignorieren |

### 2.4 V2/V3 weitere vendored Dateien

| Datei:Zeile | Befund | Upstream/bbport | Paket | Vorschlag |
|---|---|---|---|---|
| vk_scheduler.cpp:12-14 | `<dlfcn.h>`, `<execinfo.h>`, `<unistd.h>` unbedingt | B | V2 | `pf_backtrace`/`pf_symbolize` |
| vk_scheduler.cpp:41 | `/proc/self/task/%u/comm` | B | V2 | `pf_thread_name` |
| vk_scheduler.cpp:60, 73, 529 | `gettid()` | B | V2 | `pf_thread_id()` |
| vk_scheduler.cpp:66 | `backtrace(frames, 24)` | B | V2 | `pf_backtrace` |
| vk_scheduler.cpp:301, 336 | `dladdr`; `(unsigned long)` für Zeigerdifferenz (LLP64: 32 Bit) | B | V2 | `pf_symbolize`; Cast auf `unsigned long long` |
| vk_scheduler.cpp:271, vk_scheduler.h:760 | `__builtin_return_address(0)` | B | V2 | keiner (clang) |
| vk_rasterizer.cpp:4, 1333 | `<dlfcn.h>`, `dladdr` | B | V2 | `pf_symbolize` |
| vk_rasterizer.cpp:1199, vk_pipeline_cache.cpp:1087 | `sigsetjmp(recover, 0)` + `runtime_fault_recover` | B | V2 | `pf_sigsetjmp` (K6-Asm; `sigjmp_buf` aus `bbport_toggles.h` mitumstellen) |
| vk_draw_pipe.h:201 | `gettid()` | B | V2 | `pf_thread_id()` |
| bbport_toggles.h:15 (neu) | `extern "C" __thread sigjmp_buf* runtime_fault_recover` | B-neu | **S2 (Vorschlag)** | `sigjmp_buf` ist unter MinGW undeklariert; `__thread` ist mit clang/COFF in Ordnung; Typ auf `PfJmpBuf*` umstellen |
| buffer_cache/buffer.* , vk_instance.* (dma-buf, `VK_EXT_external_memory_dma_buf`) | die Dateien enthalten keine POSIX-Aufrufe; die Linux-Spezifik steckt in `bbport_guest_memory.cpp` und im Vulkan-Extension-Namen | – | V3 | `external_memory_fd`-Strukturen unter Windows ausblenden (Extension nicht vorhanden); D12 |

### 2.5 gpu/shim (alle bbport)

| Datei:Zeile | Befund | Upstream/bbport | Paket | Vorschlag |
|---|---|---|---|---|
| bbport_free_check.cpp:15-22 | `pthread`, `sys/mman`, `sys/syscall`, `ucontext`, `sys/uio`, `unistd` | B-neu | S1 | nur unter `__linux__` oder Pf |
| bbport_free_check.cpp:275, 681, 693, 807 | `process_vm_readv` | B-neu | S1 | `pf_safe_read` |
| bbport_free_check.cpp:116, 684 | `gettid()`, `syscall(SYS_gettid)` | B-neu | S1 | `pf_thread_id` |
| bbport_free_check.cpp:287 | `pthread_setname_np` | B-neu | S1 | `pf_thread_name` |
| bbport_free_check.cpp:707, 742, 784, 814, 819, 1041, 1086 | `mprotect` (Seitenverfolgung Freigabeprüfung) | B-neu | S1 | `pf_vm_protect` |
| bbport_free_check.cpp:756-838, 799-800 | `sigaction`/`SIGTRAP`, `raise(SIGTRAP)`, `siginfo_t` (Einzelschritt mit Trap-Flag) | B-neu | S1 | `pf_fault_add_handler` + Einzelschritt (K7); größte Einzelposition |
| bbport_free_check.cpp:764-1061 (≈30 Stellen) | `ucontext_t.gregs[REG_*]` (RIP, RSP, RBP, RAX, RCX, RSI, RDI, R14, EFL) | B-neu | S1 | `PfContext` (IF-2) |
| bbport_guest_hooks.cpp:13-16, 68-179 | `sigaction(SIGTRAP)` auf int3-Haken im Gastcode, `gregs[REG_*]` (≈20), `process_vm_readv` (153), `mprotect` (160, 165), `sysconf` (157) | B-neu | S1 | wie free_check; **S1 ist die Hälfte aller shim-Funde** |
| bbport_clobber.cpp:11-14, 109, 138, 202-257 | `pthread_getname_np`, `gettid`, `std::aligned_alloc` (202; fehlt in libc++/UCRT), `mprotect` (207, 230, 233), `ucontext_t`/`REG_RIP` (246, 257) | B-neu | S2 | `pf_thread_name`, `pf_thread_id`, `_aligned_malloc`-Helfer, `pf_vm_protect`, `PfContext` |
| bbport_write_log.cpp:10-12, 60, 108-117 | `ucontext.h`, `gettid`, `gregs[REG_*]` (≈12) | B-neu | S2 | `pf_thread_id`, `PfContext` |
| bbport_wait_trace.cpp:13-15, 40, 73, 88 | `dlfcn`, `execinfo`, `dladdr`, `backtrace`, `pthread_getname_np` | B-neu | S2 | `pf_symbolize`, `pf_backtrace`, `pf_thread_name` |
| bbport_threads.h:9-30 | `sched_getaffinity`, `sched_setscheduler(SCHED_IDLE)`, `setpriority`, `gettid` | B-neu | S2 | Pf-Priorität/Affinität; unter Windows `SetThreadPriority`/`SetThreadAffinityMask` |
| bbport_gnm_hooks.cpp:13-14, 48, 58-60, 532, 603 | `sysconf(_SC_PAGESIZE)`, `mmap(MAP_FIXED_NOREPLACE)`, `mprotect(PROT_EXEC)` (Stub-Seite im Gastbereich) | B-neu | S2 | `pf_vm_*` + Seitengröße 4 KiB fest |
| bbgpu.cpp:7, 267 | `<sys/resource.h>`, `setenv` | B-neu | S2 | `_putenv_s` hinter Helfer; `sys/resource.h` entfällt |
| shim/common/assert.cpp:7, 15-18 | `<unistd.h>`, `pause()`; `extern "C" __attribute__((weak)) volatile int runtime_restarting` | B-neu | **S2 (Vorschlag)** | `Sleep(INFINITE)`/`std::this_thread::sleep_for`; schwache Referenz auf COFF mit lld vermutlich ok, sonst Pf-Funktion |
| core/signals.h:7 | `<signal.h>` | B-neu | S2 | vorhanden, unkritisch |
| bbport_guest_memory.cpp:9-10, 190-224 | `mmap`/`munmap(MAP_SHARED, fd)` auf dma-buf-fd | B-neu | V3 | `#ifdef __linux__`, Windows liefert "nicht unterstützt" (D12) |
| bbport_heap_sites.cpp:162-207 | `__builtin_frame_address`, Slot-Offsets im Gast-Libc; keine POSIX-Aufrufe | B-neu | **S2 (Vorschlag, Compile nur prüfen)** | vermutlich kompiliert ohne Änderung |
| bbport_overlay.cpp:23-33 | `asm(".section .rodata … .hidden … .incbin …")` | B-neu | S3 | siehe Abschnitt 3 (COFF) |
| bbport_sections.h:10, bbport_write_log.cpp:12 | `x86intrin.h` | B-neu | S2/S3 | keiner |
| window.cpp, sdl_window.h | SDL3 (`SDL_GetWindowProperties`, HWND), `WindowSystemType::Windows` ist bereits im Enum | B-neu | S3 | Fensterhandle über `SDL_PROP_WINDOW_WIN32_HWND_POINTER` |
| bbport_copy.cpp, bbport_settings.cpp, bbport_timeline.h, bbport_ce_stats.h, emulator_settings.h | nur `getenv`, `long`-Werte (`std::strtol`) | B-neu | S2 | keiner |

## 3. Upstream-Windows-Zweige und MSVC-Spezifika im vendored Code

| Datei:Zeile | Befund | Upstream/bbport | Paket | Vorschlag |
|---|---|---|---|---|
| common/assert.h:17-20, common/debug.h:6-12 | `_MSC_VER` → `__declspec(noinline)`, `__debugbreak`; sonst GCC-Attribute, `__builtin_trap` | U-ok | G2 | keiner (clang definiert `__GNUC__`, nicht `_MSC_VER`) |
| common/rdtsc.h:8-31 | `_MSC_VER` → `<intrin.h>`, `__rdtsc`; sonst `asm volatile("lfence; rdtsc; lfence")` | U-ok | G2 | keiner |
| common/uint128.h:8-14, 58-75 | `_MSC_VER`+`!__clang__` → `_umul128`; clang hat `__SIZEOF_INT128__` | U-ok | G2 | keiner |
| common/io_file.cpp:23-27 | `_MSC_VER` → `#define fseeko _fseeki64` | U-ok | G2 | unter MinGW nicht aktiv; `fseeko` kommt aus mingw-w64 (64 Bit nur mit `_FILE_OFFSET_BITS=64`, in F5 gesetzt) |
| common/spin_lock.cpp:6-26 | `_MSC_VER` und `__aarch64__ && _MSC_VER` | U-ok | G2 | keiner |
| common/aes.h:416-430 | `#pragma optimize("", off)` nur `clang && _WIN32 && !_WIN64` | U-ok, Datei wird von bbgpu nicht übersetzt | – | keiner |
| common/memory_patcher.h:10-12 | `__declspec(dllexport)` bzw. `visibility("default")` | U-ok, nicht übersetzt | – | keiner |
| common/logging/log.h:13-18, log.cpp:139-196 | spdlog `msvc_sink`/`wincolor_sink` | U-ok, **vom shim überdeckt** | – | keiner |
| common/thread.cpp:172-182 | `SetThreadDescription`/`GetThreadDescription` | U-ok, bbport-Kommentar "kept by the C runtime" | G2 | Deklaration erst ab `_WIN32_WINNT >= 0x0A00` (F5 gesetzt); wenn mingw-w64 sie nicht deklariert: Laufzeitbindung über `GetProcAddress("kernel32")` |
| common/thread.cpp:105-125 | `LARGE_INTEGER interval{.QuadPart = …}` (designated initializer auf Union) | U-ok | G2 | clang akzeptiert vermutlich; sonst `LARGE_INTEGER interval; interval.QuadPart = …` |
| video_core/renderdoc.cpp:12-60 | `LoadLibraryA`, `dlopen` | U-ok, **nicht übersetzt** | – | keiner |
| video_core/renderer_vulkan/vk_platform.cpp:5-14 | `_WIN64` → `VK_USE_PLATFORM_WIN32_KHR` nur in dieser Übersetzungseinheit | U-ok | G3 | **Hinweis:** Vulkan-Hpp-Dispatcher-Layout hängt von den `VK_USE_PLATFORM_*`-Makros ab; gleiche Konstruktion wie Linux (dort Wayland+Xlib) und wie Upstream. Stürzt `vk::Instance`-Erzeugung ab, zuerst global `VK_USE_PLATFORM_WIN32_KHR` setzen |
| shim/bbport_overlay.cpp:23-33 | `asm(".section .rodata\n .hidden bb_font_ttf …")` | B-neu | S3 | **bricht vermutlich unter COFF:** `.hidden` gibt es dort nicht, `.rodata` heißt `.rdata`. Vorschlag: `#ifdef _WIN32` mit `.section .rdata,"dr"` ohne `.hidden`, `.global bb_font_ttf` bleibt; Symbolnamen ohne Unterstrich (x86_64) |
| video_core/renderer_vulkan/vk_scheduler.cpp:271, vk_scheduler.h:760 | `__builtin_return_address(0)` | B | V2 | gleich unter clang |
| shim/bbport_toggles.h:15 | `__thread` (ELF-Stil) | B-neu | S2 | clang/COFF kennt `__thread` und implementiert es als `thread_local`; Vorsicht: **Linux** nutzt es aus der Exe, Windows-Exe und statisches bbgpu teilen dieselbe Definition (kein TLS-Modellproblem) |
| gpu/third_party/fsr-vulkan, sirit | `#pragma comment(lib…)`/SEH/`<intrin.h>` | – | – | nicht prüfbar, Submodul leer; fsr-vulkan hat in `CMakeLists.txt` einen `MSVC`-Zweig (`/W4`), `UNIX`-Zweig nur für `-lm` |

Treffer für `__try`, `#pragma comment`, `_aligned_malloc`, `<intrin.h>` ohne `_MSC_VER`-Guard: **keine**
im kompilierten Teil. `std::aligned_alloc` steht nur in `bbport_clobber.cpp:202` (S2).

## 4. Annahmen aus dem Linux-Build

| Datei:Zeile | Befund | Upstream/bbport | Paket | Vorschlag |
|---|---|---|---|---|
| gesamtes bbgpu | `long` ist unter LLP64 32 Bit. Treffer in kompilierten Dateien: `vk_scheduler.cpp:305,340`, `bbport_wait_trace.cpp:50,54` (Cast auf `unsigned long` für `%lx` von Zeigerdifferenzen), `bbport_guest_hooks.cpp:157` (`long page_size`), `emulator_settings.h:18-46`, `window.cpp:30` (`strtol`, harmlos). `va_ctx.h:96-100 vaArgLong` und `aes.h` werden nicht übersetzt | B / U | V2, S1, S2 | Casts auf `unsigned long long` / `%llx`; `va_ctx.h` bleibt außen vor (F10 betrifft den Gast-ABI, nicht diese Bibliothek) |
| `std::filesystem::path` | `value_type` ist `wchar_t`: `path.c_str()` ist `const wchar_t*`. Treffer: `io_file.cpp` (bereits `_wfopen`-Zweig), `cache_storage.cpp` (`string().c_str()`, ok), `vk_frame_capture.cpp:90` (`std::string path`, ok), `bbport_overlay.cpp`/`window.cpp` (char*-Pfade, ok) | U | G2/G3 | keiner; `ValidatePath` begrenzt auf `MAX_PATH` 260 (LongPaths=0) |
| `fmt` 12 (MSYS2) gegen nixpkgs-Stand | Upstream bündelt eine eigene Version; 15 `fmt::formatter`-Spezialisierungen, alle mit `const`-`format` (geprüft per Grep: `logging/formatter.h:40`, `ir/condition.h:50`); `fmt::join` in `vk_instance.cpp:812`, `vk_platform.cpp:299-300` mit `<fmt/ranges.h>` eingebunden; `fmt/std.h` (Pfade) in `shim/common/logging/log.h:14` | – | G2/G3 | vermutlich ohne Arbeit; erst bei Fehlern in `fmt` festlegen (FetchContent, TOOLCHAIN.md §2) |
| `boost` 1.92 | nur `boost::asio::steady_timer`/`io_context` (`equeue.cpp:27,142`), `boost::container` im Recompiler; `BOOST_ASIO_STANDALONE` gesetzt | U | G3 | `io_context` unter Windows braucht `ws2_32`, `mswsock` (F5 gesetzt); `_WIN32_WINNT` gesetzt (F5). Die Definition `boost::asio::io_context io_context` steht in `shim/bbgpu.cpp:350` (S2); `io_context.run()` läuft dort auf einem eigenen Thread (bbgpu.cpp:370) |
| libc++ statt libstdc++ | genutzt: `std::jthread`/`std::stop_token`/`std::stop_callback` (≈25 Dateien), `std::ranges::to` (3), `std::atomic_ref` (2), `std::binary_semaphore`, `std::shared_mutex`, `std::source_location`, `std::format` (4), `std::bit_cast` | U | G3 | libc++ 23 kennt alle; Auffälligkeiten erst beim Übersetzen. `<semaphore>` in `liverpool.h:15` |
| `ssize_t`, `off_t`, `pid_t`, `clockid_t`, `iovec`, `rusage` | `ssize_t`/`off_t` in MinGW vorhanden (`off_t` 32 Bit ohne `_FILE_OFFSET_BITS=64`, gesetzt); `iovec`, `rusage`, `clockid_t` (Linux-Bedeutung) nicht | B | V2/S* | mit den jeweiligen Paketen |
| `x86intrin.h`, `<immintrin.h>` | in clang vorhanden | U/B | – | keiner |
| `thread_local` | ELF `-ftls-model=initial-exec` entfällt (F5); COFF-TLS in einer statischen Bibliothek ist unkritisch | – | – | keiner |
| `-fno-omit-frame-pointer` | bleibt; Windows-Frame-Walking des Loaders hängt davon ab | – | – | keiner |
| PGO/LTO | `check_ipo_supported` mit clang+lld+`llvm-ar` vermutlich ok; `BB_PGO=generate` schreibt Profil mit LLVM-Laufzeit, nicht GCC `.gcda` (build.sh sucht `*.gcda`) | – | F5/F3 | LTO zunächst `-DBB_LTO=OFF` für den ersten Lauf |
| Vulkan-Auffindung | `find_package(Vulkan)` findet unter CLANG64 evtl. das Windows-SDK (`VULKAN_SDK=C:\VulkanSDK\1.3.296.0`, MSVC-`.lib`) statt MSYS2-Header | – | F5 | Umgebungsvariable `VULKAN_SDK` in der CLANG64-Shell leeren oder `-DVulkan_INCLUDE_DIR/-DVulkan_LIBRARY` setzen |
| Submodule | `third_party/imgui` und `third_party/fsr-vulkan` sind im Worktree **leer** (`git submodule update --init` braucht Netz; im Hauptrepo `C:\nitro-ps4-port` vorhanden: 282 bzw. 786 Dateien); `build.sh` wendet dort `gpu/patches/fsr-vulkan/*.patch` an | – | F5 (Abnahme) | vor `cmake -S gpu` aus dem Hauptrepo kopieren oder Submodule initialisieren, dann Patches anwenden (nicht F5) |

## 5. Empfehlung G2/G3

Reihenfolge nach Arbeit und Abhängigkeit:

1. **G2a `common/`** (13 Dateien, Umfang S). Zuerst, weil alles davon abhängt; Befund: nur
   `path_util.cpp` (Shlobj-Guard) und `ntapi.h` (Typkollisionen) können Fehler erzeugen.
2. **G2b `shader_recompiler/**`** (72 Dateien, Umfang S bis M). Keine POSIX-Treffer; nur
   Bibliotheksfragen (fmt, boost, libc++, sirit/magic_enum). Parallel zu G2a möglich, sobald
   `common` kompiliert (Header).
3. **G3a `core/libraries` + `video_core` Wurzel, `amdgpu` ohne liverpool, `texture_cache`** (ca. 25
   Dateien, S). Wenig Plattformcode.
4. **G3b `renderer_vulkan`** ohne V2/V3-Dateien (ca. 28 Dateien, M). Viel Code (20 800 Zeilen),
   aber wenig Plattform: `vk_platform.cpp` ist Upstream-Windows, `vk_breadcrumbs.cpp` nur
   `unistd`/`sys/stat`.
5. **G3c `buffer_cache` ohne `buffer.cpp`/`buffer_cache.cpp`** (nur `fault_manager.cpp`, S).

Danach V1 (`page_manager`, M), V2 (7 Dateien, M, Hauptlast: `process_vm_readv`, `gettid`,
`dladdr`, `sigsetjmp`, `/proc`), V3 (S–M), S1 (L: ≈85 Fundstellen `ucontext`/`sigaction`/`mprotect`,
Einzelschritt-Trap; bei Umfang eher L als M), S2 (M, ≈45 Stellen plus drei nicht zugeordnete
Dateien), S3 (S, aber `.incbin`-Assembler unter COFF und HWND).

Schätzung Fix-Umfang (Fundstellen im Code; Compilerfehler kommen on top):

| Teil | Dateien | Umfang | Grund |
|---|---:|:---:|---|
| G2a common | 13 | S | 2 vorhergesagte Stellen |
| G2b shader_recompiler | 72 | S–M | 0 POSIX-Stellen, Risiko nur Bibliotheken |
| G3a core/amdgpu/texture_cache | ca. 25 | S | 1 Stelle (vk_breadcrumbs) |
| G3b renderer_vulkan | ca. 28 | M | Umfang, kaum Plattformcode |
| G3c buffer_cache (fault_manager) | 1 | S | – |
| V1 page_manager | 1 | M | 23 B-Zeilen + Entwurf `PfContext` |
| V2 | 7 | M | ≈45 Stellen |
| V3 | 3 | S–M | dma-buf-Pfad komplett aus |
| S1 | 2 | L | ≈85 Stellen, Einzelschritt/Trap |
| S2 | 7 (+3) | M | ≈45 Stellen |
| S3 | 3 | S | incbin, HWND |

**Zählung der Vorab-Befunde** (Zeilen, grob; Skript zählt `getenv` nicht): Upstream-Zeilen mit
Gegenzweig (U-ok/U-tot) ca. 120, davon G2 ca. 77, V1 18; bbport-Zeilen ca. 210: V1 23, V2 39,
V3 17, S1 79, S2 47, S3 1, G3 (vk_breadcrumbs) 5. MSVC-only-Zweige, die unter MinGW brechen
könnten: 1 sicher (`bbport_overlay.cpp` Assembler), 3 vermutet (`ntapi.h`-Typen, `path_util.cpp`
Shlobj-Guard, `CreateFileMapping2`).
