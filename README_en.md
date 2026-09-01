[中文版](README.md) | English

# QTrace
QBDI based Android arm64 trace tool,this is an android studio project.

# Features
* arm64 code Instrumentation,memory read/write monitor
* custome function monitor
* custome jni monitor
* custome libc monitor
* multi-thread trace: child threads spawned inside the traced function are also fully traced

# Multi-thread Trace Design

If the traced function spawns child threads via `pthread_create`, those threads used to run natively outside QBDI (untraced), and the shared global state was corrupted under concurrency, causing the process to abort. Now both the main thread and every child thread it spawns are fully traced via:

1. **Child-thread entry redirection (`libctrace.cpp` / `vm.cpp`)**
   `pthread_create` is intercepted by the libc-trace callback at the `br/blr` PREINST. The callback saves the child's `start_routine`(x2) and `arg`(x3) into a heap `thread_trace_ctx`, then rewrites x2 to `qtrace_thread_trampoline` and x3 to that ctx. When QBDI natively executes pthread_create, the child thread starts at the new entry.
   `qtrace_thread_trampoline` builds an independent QBDI VM on the child thread's own stack (reusing `vm::init`'s instrumented range and callbacks), restores the real `start_routine`/`arg` and runs it via `qvm.call`, fully tracing the child thread.
   **Range filter**: the redirect only happens when `start_routine` falls inside the target SO's `[start,end)` range (i.e. threads spawned by the code we trace); threads outside that range (libc/libart internal threads, etc.) cannot be instrumented by QBDI and are passed through, avoiding pointless VM overhead and instability.

2. **Thread-local trace state (`logger.cpp` / `vm.cpp`)**
   `_logger`, `preRegOutput` and `lastAddr` are now `thread_local`, so each trace thread owns an independent sds log buffer and concurrent writes no longer corrupt the heap. The log filename gets a `_tid<thread tid>` suffix so each thread writes its own file.

3. **Serialized init / counter (`vm.cpp` / `hook/TraceLogger.cpp`)**
   `vm::init` (which reads process-level state such as `/proc/self/maps`) and the counter/mkdir in `getLogPath` are each guarded by a `std::mutex`, making concurrent entry into trace safe.

# Merged Log Output

One trace run (including all child threads) is consolidated into a **single file** `*_merged.txt`, which is easier for trace tools to view and for LLMs to analyze. To keep it ordered within one file, a "shard + block merge" approach is used (`logger.cpp`):

* Each thread writes to its own temporary shard file `*_merged.txt.tid<tid>.part` during tracing — no lock, no interference;
* When a thread finishes tracing, its entire shard is appended to the merged file as **one contiguous block** under a global `std::mutex`, so each thread's instruction stream stays complete and uninterrupted, never interleaved with other threads;
* Each block carries a header/footer:

  ```
  ==== QTrace THREAD seq=<start order> tid=<thread tid> creator_tid=<parent tid> routine=0x<entry offset> ====
  ... the thread's full instruction trace ...
  ==== QTrace THREAD END seq=<start order> tid=<thread tid> ====
  ```

  `seq` is a globally increasing order in which threads entered trace (note: blocks are merged into the file in the order threads *finish*, not start; sort by `seq` to read in start order); `creator_tid` records which thread spawned it (0 for the main thread), letting you reconstruct the parent/child relationship;
* The temporary shard is deleted after merging. If a crash happens before merging, the shard file is preserved and no data is lost (see below).

# Crash Handler

`crashhandler.cpp` installs handlers for SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE/SIGTRAP at injection time. On a crash it logs to logcat (tag `QTrace`):

* signal type, `si_code`, fault address, crashing thread tid/pid
* **the last original-instruction offset this thread traced** (`module + 0xoffset`) — since the PC at crash time usually points into QBDI's JIT buffer and is meaningless, this offset is the key to locating the crash site
* all arm64 general registers plus sp/pc/lr, resolving which module pc/lr fall into via `dladdr`
* flushes the current thread's trace buffer to that thread's shard file (`.part`) before crashing, to preserve the last instruction stream (via the async-signal-safe `writelog_signal_safe`, using only raw `open/write/close` syscalls — no malloc/ofstream — so a corrupted heap won't cause a secondary crash). Since no merge happens on crash, the shard file is preserved — inspect the `.part` for the corresponding `tid` to locate the problem

Robustness:
* each trace thread sets up a `sigaltstack` alternate signal stack, so the handler still runs on stack overflow / corruption
* the handler has re-entry protection: on a secondary crash it restores the default action and re-raises, avoiding an infinite loop while preserving the system tombstone

# Configuration (/data/local/tmp/qtrace.config)

Trace targets and options are read from a config file, so switching targets no longer requires editing source code. Built-in defaults (`cfg_*` in `native_main.cpp`) apply when the file is missing:

```
# target SO name (push the SO to /data/local/tmp/)
so=libtiny.so
# target function offset inside the SO, hex, optional 0x prefix
func=157084
# hook proxy mode: arg4/arg5/sig3/dfp/s1 (different signatures/filters)
mode=s1
# argument filter (compares x2 in arg4/arg5 modes; 0 = no filter, decimal or 0x hex)
filter=0
# per-thread log buffer flush threshold in bytes (hex; default 1000000 = 16MB)
bufsize=1000000
# max concurrently traced threads including the main one, 0 = unlimited; excess children run natively
max_threads=0
```

Log lines are prefixed `[00:00:00 tid]` (the unidbg thread-number slot carries the thread tid), so `grep "\[00:00:00 12345\]"` extracts one thread's instruction stream from the merged file.

If the target SO is not loaded yet (e.g. Zygisk injection), both `dlopen` and `android_dlopen_ext` are hooked to wait for it: Java `System.load` goes through `android_dlopen_ext` while native code calls `dlopen` directly — the two entry points do not pass through each other in bionic.

# Usage

0.unzip libQBDI.zip under nativelib\src\main\cpp\qbdi-arm64\lib.Or download latest libQBDI.a from https://github.com/QBDI/QBDI/releases/ ,we use andorid-aarch64.then put libQBDI.a under nativelib\src\main\cpp\qbdi-arm64\lib

1.push your target so file to /data/local/tmp

2.under root ,run cmd: setenforce 0

3.(optional) create /data/local/tmp/qtrace.config to select target SO / function offset / mode — see the Configuration section above; without it the built-in defaults in native_main.cpp apply

4.add custome function monitor in qbdihook.cpp，add custome libc function monitor in libctrace.cpp，add custome jni function monitoe in jnitrace.cpp

5.run Build-Generate Apks,push the output lib file: libnativelib.so , to /data/local/tmp

6.inject libnativelib.so into the target process with a third-party injector (e.g. frida); a single dlopen activates it
