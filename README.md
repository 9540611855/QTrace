中文 | [English](README_en.md)

# QTrace
基于qbdi的安卓arm64真机trace工具,使用android studio构建

# Features
* arm64真机指令trace，内存读写监控
* 自定义函数监控
* 自定义jni函数监控
* 自定义libc函数监控
* 多线程trace：被trace函数内部创建的子线程也会被完整trace

# 多线程 Trace 设计

被 trace 的函数如果通过 `pthread_create` 开启子线程，之前这些子线程会在 QBDI 之外原生执行、无法被 trace，且共享的全局状态在并发下会损坏导致进程异常退出。现在通过以下机制让主线程和它开启的所有子线程都被完整 trace：

1. **子线程入口重定向 (`libctrace.cpp` / `vm.cpp`)**
   `pthread_create` 由 libc trace 回调在 `br/blr` 的 PREINST 拦截。回调把子线程的 `start_routine`(x2) 与 `arg`(x3) 暂存到堆上的 `thread_trace_ctx`，并把 x2 改写为 `qtrace_thread_trampoline`、x3 改写为该 ctx。QBDI 原生执行 pthread_create 时便会用新的入口创建子线程。
   `qtrace_thread_trampoline` 在子线程自身的栈上新建一个独立的 QBDI VM（复用 `vm::init` 的计装范围与回调），还原真正的 `start_routine`/`arg` 后通过 `qvm.call` 执行，从而完整 trace 子线程。
   **范围过滤**：只有当 `start_routine` 落在目标 SO 的 `[start,end)` 区间内（即“由我们 trace 的代码开启的线程”）才重定向；范围外的线程（libc/libart 内部线程等）QBDI 无法计装，直接放行，避免无谓的 VM 开销与不稳定。

2. **Trace 状态线程本地化 (`logger.cpp` / `vm.cpp`)**
   `_logger`、`preRegOutput`、`lastAddr` 均改为 `thread_local`，每个 trace 线程持有独立的 sds 日志缓冲，避免并发写入导致堆破坏。日志文件名追加 `_tid<线程tid>` 后缀，每个线程写各自的文件。

3. **初始化/采番串行化 (`vm.cpp` / `hook/TraceLogger.cpp`)**
   `vm::init`（会读取 `/proc/self/maps` 等进程级状态）与 `getLogPath` 的计数器/建目录分别用 `std::mutex` 加锁，保证多线程同时进入 trace 时安全。

# 统一日志输出 (Merged Log)

一次 trace（含所有子线程）最终归整到**同一个文件** `*_merged.txt`，方便 trace 工具查看和大模型分析。为了在单文件内不乱序，采用“分片 + 成块合并”的方式（`logger.cpp`）：

* 每个线程 trace 期间写入自己的临时分片文件 `*_merged.txt.tid<tid>.part`，无需加锁、互不干扰；
* 线程 trace 结束时，在全局 `std::mutex` 保护下把整个分片作为**一个连续块**追加进合并文件，因此每个线程的指令流是完整、连续的，不会与其他线程交错；
* 每个块带有块头/块尾：

  ```
  ==== QTrace THREAD seq=<开始序号> tid=<线程tid> creator_tid=<父线程tid> routine=0x<入口偏移> ====
  ... 该线程完整的指令 trace ...
  ==== QTrace THREAD END seq=<开始序号> tid=<线程tid> ====
  ```

  其中 `seq` 是线程进入 trace 的全局递增序号（注意：块在文件中按各线程 trace **结束**的顺序合并，而非开始顺序，需按开始顺序阅读时用 `seq` 排序即可），`creator_tid` 记录是哪个线程开启了它（主线程为 0），据此可还原线程父子关系；
* 合并完成后临时分片被删除。若在合并前崩溃，分片文件仍保留，数据不丢失（见下）。

# 崩溃排查 (Crash Handler)

`crashhandler.cpp` 在注入时安装 SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE/SIGTRAP 的信号处理器，崩溃时在 logcat（tag `QTrace`）输出：

* 信号类型、`si_code`、fault 地址、崩溃线程 tid/pid
* **当前线程最后 trace 到的原始指令偏移**（`模块名 + 0x偏移`）——由于崩溃时 PC 通常落在 QBDI 的 JIT 缓冲区里没有意义，这个偏移才是定位崩溃点的关键
* arm64 全部通用寄存器 + sp/pc/lr，并用 `dladdr` 解析 pc/lr 落在哪个模块
* 崩溃前自动把当前线程的 trace 缓冲刷写到该线程的分片文件（`.part`），尽量保留最后的指令流（使用异步信号安全的 `writelog_signal_safe`，仅用 `open/write/close` 原始 syscall，不触碰 malloc/ofstream，避免堆损坏时二次崩溃）。由于崩溃时未做合并，分片文件会保留下来，可直接查看对应 `tid` 的 `.part` 定位问题

健壮性处理：
* 每个 trace 线程通过 `sigaltstack` 设置备用信号栈，栈溢出/栈损坏时 handler 仍能运行
* handler 内置再入保护，二次崩溃时直接恢复默认处理并重新抛出，避免死循环，同时保留系统 tombstone

# 配置 (/data/local/tmp/qtrace.config)

trace 目标与参数通过配置文件指定，换目标不再需要改源码重编。文件不存在时使用内置默认值（`native_main.cpp` 中的 `cfg_*`）：

```
# 目标SO名（SO 需 push 到 /data/local/tmp/ 下）
so=libtiny.so
# 目标函数在 SO 内的偏移，十六进制，可带 0x
func=157084
# hook proxy 模式：arg4/arg5/sig3/dfp/s1（对应不同函数签名/过滤逻辑）
mode=s1
# 参数过滤值（arg4/arg5 模式比较 x2；0=不过滤 trace 每次调用；十进制或 0x 十六进制）
filter=0
# 每线程日志缓冲刷盘阈值（十六进制字节；默认 1000000 即 16MB）
bufsize=1000000
# 同时 trace 的线程数上限（含主线程），0=不限；超限的子线程原生执行不重定向
max_threads=0
```

日志说明：合并文件中每行前缀为 `[00:00:00 tid]`（unidbg 格式的线程号槽位填入本线程 tid），配合块头 `==== QTrace THREAD ... ====`，用 `grep "\[00:00:00 12345\]"` 即可抽出单个线程的完整指令流。

目标 SO 未加载时（如 Zygisk 注入场景）会同时 hook `dlopen` 与 `android_dlopen_ext` 等待其出现：Java 层 `System.load` 走 `android_dlopen_ext`，native 直调 `dlopen` 走 `dlopen`，二者在 bionic 中互不经过对方，只挂一个会漏掉另一条加载路径。

# Usage
0.将nativelib\src\main\cpp\qbdi-arm64\lib 目录下的libQBDI.zip解压出libQBDI.a，置于nativelib\src\main\cpp\qbdi-arm64\lib目录下。或去qbdi官方 https://github.com/QBDI/QBDI/releases/ 下载最新的libQBDI.a，注意选择andorid aarch64架构的,置于nativelib\src\main\cpp\qbdi-arm64\lib目录下

1.将trace的目标so push到/data/local/tmp目录下

2.root 环境下执行 setenforce 0

3.（可选）创建 /data/local/tmp/qtrace.config 指定目标 SO/函数偏移/模式等，见上文"配置"一节；不放配置文件则使用 native_main.cpp 中的内置默认值

4.在qbdihook.cpp中添加自定义hook，在libctrace.cpp中添加需要trace 的libc函数，在jnitrace.cpp中添加需要trace的jni函数

5.Build-Generate Apks,将自动生成libnativelib.so ,将其 push 到 /data/local/tmp目录下

6.使用第三方工具（如 frida）将 libnativelib.so 注入目标进程（dlopen 即自动生效）