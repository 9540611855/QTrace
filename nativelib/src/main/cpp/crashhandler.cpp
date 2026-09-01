//
// QTrace crash handler
//
#include "crashhandler.h"
#include "logger.h"

#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cinttypes>
#include <dlfcn.h>

// 记录当前线程最后一次进入 trace 的原始指令位置（PREINST 中更新）。
// 崩溃时 PC 往往落在 QBDI 的 JIT 缓冲里，没有意义，用这个才能定位到目标 SO 偏移。
// 热路径：每条指令都会更新，故只存指针+偏移，不做拷贝。module_name 指向
// vm::moduleName 内部缓冲，在整个 trace 期间保持有效。
static thread_local size_t       t_last_offset = 0;
static thread_local const char*  t_last_module = nullptr;
static thread_local bool         t_in_trace = false;

void crash_set_last_insn(size_t module_offset, const char* module_name)
{
    t_last_offset = module_offset;
    t_last_module = module_name;
    t_in_trace = true;
}

void crash_clear_trace_flag()
{
    t_in_trace = false;
}

static struct sigaction g_old_actions[32];
static const int g_fatal_signals[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGTRAP };

static const char* signalName(int sig)
{
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGTRAP: return "SIGTRAP";
        default:      return "UNKNOWN";
    }
}

static void crashSignalHandler(int sig, siginfo_t* info, void* ucontext)
{
    pid_t tid = gettid();

    // 再入/并发保护：全局 CAS 锁。
    // - handler 内部若二次崩溃（例如刷日志时再次踩到损坏的堆），第二次进入直接
    //   恢复默认处理并重新抛出，避免死循环；
    // - 多线程同时崩溃时，只有第一个线程完整打印+刷盘，其余线程直接重抛。
    //   这是为避免在崩溃上下文里加锁死锁而做的取舍：后到线程会丢失其“未刷盘的
    //   最后一小段”buf，但已写入的 .part 分片仍在磁盘上，不影响主体排查。
    static volatile sig_atomic_t in_handler = 0;
    if (__sync_val_compare_and_swap(&in_handler, 0, 1) != 0) {
        if (sig >= 0 && sig < 32) {
            sigaction(sig, &g_old_actions[sig], nullptr);
        }
        raise(sig);
        return;
    }

    LOGE("========== QTrace CRASH ==========");
    LOGE("signal=%d (%s) code=%d fault_addr=%p tid=%d pid=%d",
         sig, signalName(sig), info ? info->si_code : 0,
         info ? info->si_addr : nullptr, (int)tid, (int)getpid());

    // 当前线程最后 trace 到的原始指令位置（比 raw PC 更有用）
    if (t_in_trace) {
        LOGE("last traced insn: %s + 0x%zx  (thread was inside QBDI trace)",
             t_last_module ? t_last_module : "?", t_last_offset);
    } else {
        LOGE("thread was NOT inside QBDI trace when it crashed");
    }

    // 打印通用寄存器（arm64）
    auto* uc = (ucontext_t*)ucontext;
    if (uc) {
        auto& mc = uc->uc_mcontext;
        for (int i = 0; i < 30; i += 3) {
            LOGE("  x%-2d=0x%016" PRIx64 "  x%-2d=0x%016" PRIx64 "  x%-2d=0x%016" PRIx64,
                 i,   (uint64_t)mc.regs[i],
                 i+1, (uint64_t)mc.regs[i+1],
                 i+2, (uint64_t)mc.regs[i+2]);
        }
        LOGE("  x30(lr)=0x%016" PRIx64 "  sp=0x%016" PRIx64 "  pc=0x%016" PRIx64,
             (uint64_t)mc.regs[30], (uint64_t)mc.sp, (uint64_t)mc.pc);

        // 解析 pc/lr 落在哪个模块，方便判断是 QBDI JIT 还是真实代码
        Dl_info dli;
        if (dladdr((void*)mc.pc, &dli) && dli.dli_fname) {
            LOGE("  pc in module: %s (base=%p)", dli.dli_fname, dli.dli_fbase);
        }
        if (dladdr((void*)mc.regs[30], &dli) && dli.dli_fname) {
            LOGE("  lr in module: %s (base=%p)", dli.dli_fname, dli.dli_fbase);
        }
    }

    // 把 trace 缓冲刷到磁盘，尽量保留崩溃前的指令流。
    // 使用异步信号安全版本，避免在崩溃上下文中触碰 malloc/ofstream。
    if (_logger != nullptr) {
        LOGE("flushing trace log before crash: %s", _logger->logfile.c_str());
        writelog_signal_safe();
    }

    LOGE("========== END CRASH ==========");

    // 恢复默认处理并重新触发，保留系统 tombstone / 让上层 handler 继续
    if (sig >= 0 && sig < 32) {
        sigaction(sig, &g_old_actions[sig], nullptr);
    }
    raise(sig);
}

void crash_setup_thread_altstack()
{
    // 每线程一份备用栈；已设置过就跳过
    static thread_local bool done = false;
    static thread_local char alt_stack[SIGSTKSZ > 0 ? SIGSTKSZ : 16384];
    if (done) return;
    stack_t oldss;
    if (sigaltstack(nullptr, &oldss) == 0 && !(oldss.ss_flags & SS_DISABLE)) {
        // 该线程已有备用栈（app 自身/其 crash sdk 设置的）：不覆盖，
        // 我们的 handler 也在其上运行（SIGSTKSZ 起步，足够本 handler 使用）
        done = true;
        return;
    }
    stack_t ss;
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof(alt_stack);
    ss.ss_flags = 0;
    if (sigaltstack(&ss, nullptr) != 0) {
        LOGE("crash_setup_thread_altstack: sigaltstack failed (tid=%d)", (int)gettid());
    }
    done = true;
}

void installCrashHandler()
{
    // 主线程也设置备用信号栈：栈溢出/栈损坏下 handler 仍能运行。
    crash_setup_thread_altstack();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crashSignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    for (int sig : g_fatal_signals) {
        if (sigaction(sig, &sa, &g_old_actions[sig]) != 0) {
            LOGE("installCrashHandler: failed to hook signal %d", sig);
        }
    }
    LOGE("QTrace crash handler installed");
}
