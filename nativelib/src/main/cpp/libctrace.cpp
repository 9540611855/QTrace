//
// Created by zgy on 2025/11/12.
//
#include "libctrace.h"
#include "dlfcn.h"
#include "logger.h"
#include "HookUtils.h"
#include <unistd.h>
LibcTraceMap* _g_libc_trace = nullptr;
static bool debug = false;

void enable_libc_trace_debug(bool enable)
{
    debug = enable;
}

void initLibcTrace()
{
    _g_libc_trace = new LibcTraceMap();
    _g_libc_trace->map.reserve(10);
}

bool hasLibctrace()
{
    if(_g_libc_trace == nullptr)
    {
        return false;
    }
    if(_g_libc_trace->map.empty())
    {
        return false;
    }
    return true;
}

void addLibctrace(void* handle,TraceCallBack callback,const char* funcname)
{
    size_t addr = (size_t)dlsym(handle,funcname);
    if(addr == 0)
    {
        LOGE("libc trace : %s find fail",funcname);
        return;
    }
    TraceFunc* func_trace = new TraceFunc();
    func_trace->callback = callback;
    if(_g_libc_trace == nullptr)
    {
        LOGE("libc trace : not init!");
        return;
    }
    auto it = _g_libc_trace->map.find(addr);
    if(it != _g_libc_trace->map.end())
    {
        LOGE("libc trace: %p has installed!",(void*)addr);
        return;
    }
    _g_libc_trace->map[addr] = func_trace;
    LOGE("libc trace: %s:%p install",funcname,(void*)addr);
}
void libc_memmove(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(_logger == nullptr)
    {
        LOGE("libc trace:logger not init!");
        return;
    }
    uint64_t x0 = QBDI_GPR_GET(gprState, 0);
    uint64_t x1 = QBDI_GPR_GET(gprState, 1);
    uint64_t x2 = QBDI_GPR_GET(gprState, 2);
    bool bIsString = false;
    if(x2 > 1)
    {
        int len = x2 <= 10 ? x2:10;
        bIsString = isString((char*)x1,len);
        if(bIsString)
        {
            LOGE("libc memmove: %s",(char*)x1);
        }
    }
    appendlogendl();
    appendformat("[log] libc memmove: dest 0x%lx,src 0x%lx,len 0x%lx",x0,x1,x2);
    if(bIsString)
    {
        appendformat(",%s;",(char*)x1);
    }
    else{
        char* hex = bytes_to_hex_string((char*)x1,x2);
        appendformat("\n%s;",hex);
        free(hex);
    }
    appendlogendl();
}

void libc_memset(QBDI::VM *vm, QBDI::GPRState *gprState) {
    if (_logger == nullptr)
    {
        LOGE("libc trace:logger not init!");
        return;
    }
    uint64_t x0 = QBDI_GPR_GET(gprState, 0);
    uint64_t x1 = QBDI_GPR_GET(gprState, 1);
    uint64_t x2 = QBDI_GPR_GET(gprState, 2);
    appendlogendl();
    appendformat("libc memset:addr:%p,val:%x,num:%x",x0,x1,x2);
    appendlogendl();
}

void libc_memcpy(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(_logger == nullptr)
    {
        LOGE("libc trace:logger not init!");
        return;
    }
    uint64_t x0 = QBDI_GPR_GET(gprState, 0);
    uint64_t x1 = QBDI_GPR_GET(gprState, 1);
    uint64_t x2 = QBDI_GPR_GET(gprState, 2);
    bool bIsString = false;
    if(x2 > 1)
    {
        int len = x2 <= 10 ? x2:10;
        bIsString = isString((char*)x1,len);
        if(bIsString)
        {
            LOGE("libc memcpy: %s",(char*)x1);
        }
    }
    appendlogendl();
    appendformat("[log] libc memcpy: dest 0x%lx,src 0x%lx,len 0x%lx",x0,x1,x2);
    if(bIsString)
    {
        appendformat(",%s;",(char*)x1);
    }
    else{
        char* hex = bytes_to_hex_string((char*)x1,x2);
        appendformat("\n%s;",hex);
        free(hex);
    }
    appendlogendl();
}

void libc_access(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(_logger == nullptr)
    {
        LOGE("libc trace:logger not init!");
        return;
    }
    uint64_t x0 = QBDI_GPR_GET(gprState, 0);
    if(debug)
    {
        LOGE("libc access:%s",(char*)x0);
    }
    appendlogendl();
    appendlog("[log] libc access:");
    appendlog((char*)x0);
    appendlogendl();
}

void libc_system_property_get(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(_logger == nullptr)
    {
        LOGE("qdbi hook:logger not init!");
        return;
    }
    uint64_t x0 = QBDI_GPR_GET(gprState, 0);
    if(debug)
    {
        LOGE("libc _system_property_get:%s",(char*)x0);
    }
    appendlogendl();
    appendlog("[log] libc _system_property_get:");
    appendlog((char*)x0);
    appendlogendl();
}

// 最近一次重定向 pthread_create 时暂存的 ctx（线程本地）。
// PREINST 中记录，紧随其后的 POSTINST（libc_pthread_create_post）消费：
// 创建失败时回收，成功时仅清标记（ctx 由子线程 trampoline 释放）。
static thread_local thread_trace_ctx* t_pending_pthread_ctx = nullptr;

void libc_pthread_create(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(_logger == nullptr)
    {
        LOGE("qdbi hook:logger not init!");
        return;
    }
    // pthread_create(pthread_t* thread, const attr*, void* (*start_routine)(void*), void* arg)
    // x2 = start_routine, x3 = arg
    uint64_t x2 = QBDI_GPR_GET(gprState, 2);
    uint64_t x3 = QBDI_GPR_GET(gprState, 3);

    if(debug)
    {
        LOGE("libc pthread_create:%lx",x2 - _g_trace_data->base);
    }
    appendlogendl();
    appendformat("[log] libc pthread_create:%lx",x2 - _g_trace_data->base);
    appendlogendl();

    // 只追踪 start_routine 落在目标 SO 内的子线程（即“由我们 trace 的代码开启的线程”）。
    // 范围外的线程（libc/libart 内部线程等）QBDI 无法计装，直接放行，避免无谓的 VM 开销。
    if (x2 < _g_trace_data->start || x2 >= _g_trace_data->end)
    {
        LOGE("pthread_create routine 0x%lx out of target SO [0x%lx,0x%lx), pass through",
             x2, _g_trace_data->start, _g_trace_data->end);
        return;
    }

    // 线程预算（qtrace.config 的 max_threads，0=不限）：每个被 trace 线程持有独立
    // QBDI VM，线程池型目标会线性吃内存；超预算的子线程不重定向，原生执行。
    if (!qtrace_thread_budget_ok())
    {
        LOGE("pthread_create routine 0x%lx: trace thread budget exhausted, pass through", x2);
        return;
    }

    // 关键：把子线程的入口改写为我们的 trampoline，让子线程在自己的
    // QBDI VM 中被完整 trace。原始 start_routine/arg 暂存在堆上的 ctx 里。
    // 该回调运行在 br/blr 的 PREINST，此时 QBDI 尚未原生执行 pthread_create，
    // 改写寄存器后 ExecBroker 会用新的入口创建子线程。
    thread_trace_ctx* ctx = new thread_trace_ctx();
    ctx->start_routine = x2;
    ctx->arg = x3;
    ctx->creator_tid = (int)gettid();
    QBDI_GPR_SET(gprState, 2, (QBDI::rword)qtrace_thread_trampoline);
    QBDI_GPR_SET(gprState, 3, (QBDI::rword)ctx);
    // 记录待定 ctx：本条指令执行完（pthread_create 返回）后由
    // libc_pthread_create_post 检查返回值，创建失败则在此回收，避免泄漏。
    // 成功时 ctx 由子线程 trampoline 内 delete，post 检查只清标记。
    t_pending_pthread_ctx = ctx;
    LOGE("pthread_create redirected: routine=0x%lx -> trampoline, ctx=%p",
         x2, (void*)ctx);
}

void libc_pthread_create_post(QBDI::GPRState* gprState)
{
    if (t_pending_pthread_ctx == nullptr) {
        return;
    }
    thread_trace_ctx* ctx = t_pending_pthread_ctx;
    t_pending_pthread_ctx = nullptr;
    // pthread_create 成功返回 0，此时 ctx 已/将由子线程 trampoline 释放；
    // 失败返回错误码，子线程不存在，ctx 无人释放，在这里回收。
    uint64_t ret = QBDI_GPR_GET(gprState, 0);
    if (ret != 0) {
        LOGE("pthread_create failed ret=%lu, reclaim ctx=%p",
             (unsigned long)ret, (void*)ctx);
        delete ctx;
    }
}

void libc_clock_gettime(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(debug)
    {
        LOGE("libc clock_gettime");
    }
    uint64_t x1 = QBDI_GPR_GET(gprState, 1);
    appendlogendl();
    appendformat("[log] libc clock_gettime:%p",x1);
    appendlogendl();
}

void libc_exit(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(debug)
    {
        LOGE("libc exit");
    }
    appendlogendl();
    appendformat("[log] libc exit");
    appendlogendl();
}

void libc_abort(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(debug)
    {
        LOGE("libc abort");
    }
    appendlogendl();
    appendformat("[log] libc abort");
    appendlogendl();
}

void libc_kill(QBDI::VM *vm, QBDI::GPRState *gprState)
{
}

void libc_strlen(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(_logger == nullptr)
    {
        LOGE("libc trace:logger not init!");
        return;
    }
    uint64_t x0 = QBDI_GPR_GET(gprState, 0);
    if(debug)
    {
        LOGE("libc strlen:%s",(char*)x0);
    }
    appendlogendl();
    appendformat("[log] libc strlen:%s",(char*)x0);
    appendlogendl();
}

void libc_execve(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(_logger == nullptr)
    {
        LOGE("libc trace:logger not init!");
        return;
    }
    uint64_t x0 = QBDI_GPR_GET(gprState, 0);
    uint64_t x1 = QBDI_GPR_GET(gprState, 1);
    if(debug)
    {
        LOGE("libc execv:%s",(char*)x0);
        while (true)
        {
            char* arg = *((char **)x1) ;
            if(arg == nullptr)
            {
                break;
            }
            LOGE("[log] libc execv argv:%s",arg);
            x1 += 8;
        }
    }
}

void libc_fstatat(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(_logger == nullptr)
    {
        LOGE("libc trace:logger not init!");
        return;
    }
    uint64_t x1 = QBDI_GPR_GET(gprState, 1);
    if(debug)
    {
        LOGE("libc fstat:%s",(char*)x1);
    }
    appendlogendl();
    appendlog("[log] libc fstat:");
    appendlog((char*)x1);
    appendlogendl();
}

void libc_stat(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(_logger == nullptr)
    {
        LOGE("libc trace:logger not init!");
        return;
    }
    uint64_t x0 = QBDI_GPR_GET(gprState, 0);
    if(debug)
    {
        LOGE("libc stat:%s",(char*)x0);
    }
    appendlogendl();
    appendlog("[log] libc stat:");
    appendlog((char*)x0);
    appendlogendl();
}

void libc_lstat(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(_logger == nullptr)
    {
        LOGE("libc trace:logger not init!");
        return;
    }
    uint64_t x0 = QBDI_GPR_GET(gprState, 0);
    if(debug)
    {
        LOGE("libc lstat:%s",(char*)x0);
    }
    appendlogendl();
    appendlog("[log] libc lstat:");
    appendlog((char*)x0);
    appendlogendl();
}

void libc_fopen(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(_logger == nullptr)
    {
        LOGE("libc trace:logger not init!");
        return;
    }
    uint64_t x0 = QBDI_GPR_GET(gprState, 0);
    if(debug)
    {
        LOGE("libc fopen:%s",(char*)x0);
    }
    appendlogendl();
    appendlog("[log] libc fopen:");
    appendlog((char*)x0);
    appendlogendl();
}