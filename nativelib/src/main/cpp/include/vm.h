//
// Created by fang on 23-12-19.
//

#ifndef QBDIRECORDER_VM_H
#define QBDIRECORDER_VM_H
#include "QBDI.h"
#include "QBDI/State.h"
#include "logger.h"
#include <sstream>
#include <string>
#include "shadowhook.h"

typedef void (*TraceCallBack)(QBDI::VM *vm, QBDI::GPRState *gprState);
typedef bool (*TraceFilter)(size_t regs[]);

struct TraceFunc {
    TraceCallBack callback;
};

// 子线程 trace 上下文：pthread_create 被改写后，start_routine 与其参数
// 暂存于此，由 qtrace_thread_trampoline 在子线程中还原并进入 QBDI 执行。
struct thread_trace_ctx {
    size_t start_routine;
    size_t arg;
    int    creator_tid;   // 开启该子线程的线程 tid，用于日志中还原线程父子关系
};

// 子线程入口 trampoline（改写后的 pthread_create start_routine 指向它）
extern "C" void* qtrace_thread_trampoline(void* raw);

class vm {
public:
    QBDI::VM init(size_t start,size_t end);
    size_t base;
    std::string moduleName; // SO模块名，用于trace输出
private:

};
typedef void (*orig_func_t)(void*);

struct g_trace_data{
    size_t base;
    size_t start;
    size_t end;
    size_t target;
    void* hooktask;
    orig_func_t orig_addr;
};

extern g_trace_data* _g_trace_data;
extern int bufsize ;
extern bool debugInsn;
// 目标函数参数过滤值（当前用于 arg4/arg5 模式，比较 x2）。0 = 不过滤，trace 每次调用。
extern size_t trace_filter;
void setBufferSize(int);
void enableDebugInsn(bool);
// 同时被 trace 的线程数上限（含主线程），0 = 不限。超限时新子线程不重定向、原生执行。
void setMaxTraceThreads(int);
// pthread_create 重定向前调用：当前被 trace 线程数是否还在预算内
bool qtrace_thread_budget_ok();

// 主入口：保存寄存器参数并启动 QBDI trace（在 hook proxy 中调用）
size_t trace(size_t regs[31], size_t* stack_params = nullptr, int stack_param_count = 0);

void sync_regs(size_t* regs,size_t pc,QBDI::GPRState* qbdi_state);

typedef size_t (*func_arg_8)(size_t x0,size_t x1,size_t x2,size_t x3,size_t x4,size_t x5,size_t x6,size_t x7);
extern func_arg_8 ori_arg8;
size_t hook_and_trace_arg8(size_t x0,size_t x1,size_t x2,size_t x3,size_t x4,size_t x5,size_t x6,size_t x7);

typedef size_t (*func_arg_4)(size_t x0,size_t x1,size_t x2,size_t x3);
extern func_arg_4 ori_arg4;
size_t hook_and_trace_arg4(size_t x0,size_t x1,size_t x2,size_t x3);

typedef size_t (*func_arg_5)(size_t x0,size_t x1,size_t x2,size_t x3,size_t x4);
extern func_arg_5 ori_arg5;
size_t hook_and_trace_arg5(size_t x0,size_t x1,size_t x2,size_t x3,size_t x4);

// sig3 Mode2 专用: commandIndex=0x28b2 且 params[6]=true 时触发trace
size_t hook_and_trace_sig3_mode2(size_t x0,size_t x1,size_t x2,size_t x3);

// DFP (设备指纹加密): commandIndex=10400 时触发trace
size_t hook_and_trace_dfp(size_t x0, size_t x1, size_t x2, size_t x3);

// sign_request (sub_157084): 8 register args (x0-x7) + 4 stack args
// x0=ctx, x1=method, x2=method_len, x3=path, x4=path_len,
// x5=query, x6=query_len, x7=deviceId,
// sp0=bodyHash_ptr, sp1=bodyHash_len, sp2=mua_ptr, sp3=mua_len
typedef size_t (*func_arg_s1)(size_t, size_t, size_t, size_t,
                               size_t, size_t, size_t, size_t,
                               size_t, size_t, size_t, size_t);
extern func_arg_s1 ori_arg_s1;
size_t hook_and_trace_s1(size_t x0, size_t x1, size_t x2, size_t x3,
                          size_t x4, size_t x5, size_t x6, size_t x7,
                          size_t sp0, size_t sp1, size_t sp2, size_t sp3);

#endif //QBDIRECORDER_VM_H
