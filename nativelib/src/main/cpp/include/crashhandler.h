//
// QTrace crash handler: 捕获 SIGSEGV/SIGABRT 等致命信号，
// 输出寄存器、backtrace 以及当前线程最后 trace 到的原始指令偏移，
// 便于定位 trace 过程中的崩溃。
//
#ifndef QTRACE_CRASHHANDLER_H
#define QTRACE_CRASHHANDLER_H

#include <cstddef>

// 安装信号处理器（进程内只需调用一次）
void installCrashHandler();

// 为当前线程设置备用信号栈。每个 trace 子线程入口调用一次，
// 保证子线程栈溢出/损坏时 crash handler 仍能运行。
void crash_setup_thread_altstack();

// 由 trace 主循环在每条指令执行前更新，记录“当前线程正在执行的
// 原始指令模块偏移 + 模块名”，崩溃时打印出来。
void crash_set_last_insn(size_t module_offset, const char* module_name);

// trace 结束后清除本线程的“正在 trace”标记，避免崩溃时误报。
void crash_clear_trace_flag();

#endif //QTRACE_CRASHHANDLER_H
