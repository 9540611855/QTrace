//
// Created by fang on 23-12-19.
//
#include "fstream"
#include <iostream>
#include <iomanip>
#include <cassert>
#include <sstream>
#include <unordered_map>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <mutex>
#include <atomic>
#include <cstring>
#include <sstream>
#include <string>
#include <QBDI/Callback.h>
#include "vm.h"
#include "logger.h"
#include "qbdihook.h"
#include "jnitrace.h"
#include "libctrace.h"
#include "TraceLogger.h"
#include "TraceUtils.h"
#include "HookUtils.h"
#include "crashhandler.h"
#include "shadowhook.h"
using namespace std;
using namespace QBDI;

// defined in native_main.cpp
extern string libname;

// 寄存器名小写化：QBDI 返回静态串（"X0".."X30"、"SP"），写入调用方栈缓冲，不分配堆。
// 每条指令的每个操作数都会走到，是热路径。
static void regNameLower(const char* in, char* out, size_t outsz) {
    size_t i = 0;
    while (in[i] != '\0' && i + 1 < outsz) {
        char c = in[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        i++;
    }
    out[i] = '\0';
}

// pre-exec register values captured in PREINST, consumed in POSTINST
// 线程本地固定缓冲：多个 trace 线程同时执行时互不干扰，且避免每条指令堆分配
static thread_local char preRegBuf[1024];

g_trace_data* _g_trace_data = nullptr;
int bufsize = 0x1000000;
bool debugInsn = false;
size_t trace_filter = 0; // 0 = 不过滤（由 /data/local/tmp/qtrace.config 的 filter 配置）

// ---- trace 线程预算 ----
// 每个被 trace 的线程持有独立 QBDI VM（各自 JIT 代码缓存），线程池型目标会线性
// 消耗内存与翻译时间。超预算的 pthread_create 不再重定向，子线程原生执行。
static std::atomic<int> g_active_trace_threads{0};
static int g_max_trace_threads = 0; // 0 = 不限

void setMaxTraceThreads(int n)
{
    g_max_trace_threads = n;
}

bool qtrace_thread_budget_ok()
{
    if (g_max_trace_threads <= 0) {
        return true;
    }
    return g_active_trace_threads.load(std::memory_order_relaxed) < g_max_trace_threads;
}

func_arg_4 ori_arg4{};
func_arg_5 ori_arg5{};
func_arg_8 ori_arg8{};

void setBufferSize(int size)
{
    bufsize = size;
}

void enableDebugInsn(bool enable)
{
    debugInsn = enable;
}

void sync_regs(size_t* regs, size_t pc,QBDI::GPRState* qbdi_state)
{
    for(int i=0;i<31;i++)
    {
        QBDI_GPR_SET(qbdi_state,i,regs[i]);
    }
    qbdi_state->pc = pc;
}

static inline uintptr_t get_current_sp() {
    uintptr_t sp;
    __asm__ __volatile__("mov %0, sp" : "=r"(sp)); // 读取SP寄存器到sp变量
    return sp;
}

uintptr_t inline get_current_x30() {
    uintptr_t reg;
    __asm__ __volatile__("mov %0, x30" : "=r"(reg));
    return reg;
}

static void save_regs(size_t* regs)
{
    regs[0] = get_current_x0();
    regs[1] = get_current_x1();
    regs[2] = get_current_x2();
    regs[3] = get_current_x3();
    regs[4] = get_current_x4();
    regs[5] = get_current_x5();
    regs[6] = get_current_x6();
    regs[7] = get_current_x7();
    // x8-x17 是调用者保存的临时寄存器：执行到这里时代理函数体（如 LOGE）已经覆写
    // 过它们，捕获到的是无意义值；而这些寄存器在被调函数入口本就没有确定语义，
    // 置 0 更确定。x18-x30 是被调用者保存的，代理函数的序言保证了原值。
    for (int i = 8; i <= 17; i++) {
        regs[i] = 0;
    }
    regs[18] = get_current_x18();
    regs[19] = get_current_x19();
    regs[20] = get_current_x20();
    regs[21] = get_current_x21();
    regs[22] = get_current_x22();
    regs[23] = get_current_x23();
    regs[24] = get_current_x24();
    regs[25] = get_current_x25();
    regs[26] = get_current_x26();
    regs[27] = get_current_x27();
    regs[28] = get_current_x28();
    regs[29] = get_current_x29();
    regs[30] = get_current_x30();
}

// 核心：在当前线程上建立独立的 QBDI VM + logger 并执行 function_address。
// 被主 trace() 和子线程 trampoline 共用，二者各自持有 thread_local 的 logger。
static size_t run_qbdi_trace(size_t function_address, size_t regs[31],
                             size_t* stack_params, int stack_param_count,
                             int creator_tid)
{
    // 所有进入 trace 的线程（主线程/代理线程/trampoline 子线程）都设置备用信号栈，
    // 保证栈溢出/栈损坏时 crash handler 仍能运行。已设置过则内部跳过。
    crash_setup_thread_altstack();
    g_active_trace_threads.fetch_add(1, std::memory_order_relaxed);

    struct timeval start, end;
    gettimeofday(&start, nullptr);
    vm* vm_ = new vm();
    auto qvm = vm_->init(_g_trace_data->start,_g_trace_data->end);
    auto qbdi_state = qvm.getGPRState();
    // 确保合并文件路径已确定（首个进入的线程设置一次），再初始化本线程 logger
    setMergedLogPath(function_address);
    initLogger(function_address, creator_tid, function_address - _g_trace_data->base);
    vm_->base = _g_trace_data->base;
    vm_->moduleName = libname;
    sync_regs(regs,function_address,qbdi_state);
    //在当前栈上开辟栈，不使用allocateVirtualStack，防止libart中Thread crash
    size_t sp = get_current_sp();
    size_t stack_size = 0x10000;
    size_t new_sp = sp - stack_size;
    // 将栈参数写入QBDI虚拟栈顶（函数入口时 sp+0, sp+8, ...）
    if (stack_params && stack_param_count > 0) {
        for (int i = 0; i < stack_param_count; i++) {
            *(size_t*)(new_sp + i * 8) = stack_params[i];
        }
        LOGE("wrote %d stack params at sp=0x%lx", stack_param_count, new_sp);
    }
    QBDI_GPR_SET(qbdi_state, QBDI::REG_SP, new_sp);
    QBDI_GPR_SET(qbdi_state, QBDI::REG_BP, new_sp);
    QBDI::rword qbdi_retval = 0;
    LOGE("trace begin @%p tid=%d", (void*)function_address, (int)gettid());
    bool qbdi_success = qvm.call(&qbdi_retval, (uint64_t)function_address);
    LOGE("trace end @%p tid=%d", (void*)function_address, (int)gettid());
    if (qbdi_success) {
        LOGE("trace completed successfully %s",_logger->logfile.c_str());
    } else {
        LOGE("trace failed");
    }
    gettimeofday(&end, nullptr);
    long elapsed_sec;
    elapsed_sec = (end.tv_sec - start.tv_sec) +
                  (end.tv_usec - start.tv_usec) / 1000000;
    LOGI("trace time cost:%lds",elapsed_sec);
    // 把本线程的分片整块合并进统一文件（内部会先刷完剩余 buf），再释放 logger
    mergeThreadLog();
    deleteLogger();
    delete vm_;
    crash_clear_trace_flag();
    g_active_trace_threads.fetch_sub(1, std::memory_order_relaxed);
    return qbdi_retval;
}

// 子线程入口：由 pthread_create 被改写后的 start_routine 指向此处。
// 在子线程自身的栈上建立独立 QBDI VM，完整 trace 子线程的执行。
extern "C" void* qtrace_thread_trampoline(void* raw)
{
    thread_trace_ctx* ctx = (thread_trace_ctx*)raw;
    size_t routine = ctx->start_routine;
    size_t arg = ctx->arg;
    int creator_tid = ctx->creator_tid;
    delete ctx;

    // 备用信号栈在 run_qbdi_trace 入口统一设置（含本子线程）
    LOGE("child thread trampoline start: routine=%p tid=%d creator_tid=%d",
         (void*)routine, (int)gettid(), creator_tid);

    size_t regs[31] = {0};
    regs[0] = arg; // start_routine 的唯一参数走 x0
    size_t ret = run_qbdi_trace(routine, regs, nullptr, 0, creator_tid);
    return (void*)ret;
}

// ---- 目标函数 hook 的摘除/重挂 ----
// 多个线程可能同时命中被 hook 的目标函数（各自进入 proxy 各自调用 trace()），
// 直接并发调用 shadowhook_unhook/hook 会出现同一 stub 被双重 unhook、或重复挂上
// 两个 hook 导致后续调用被 trace 两次。互斥 + hooktask 判空保证任一时刻至多一个 hook。
static std::mutex g_hook_mutex;

static void disarm_target_hook()
{
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (_g_trace_data->hooktask != nullptr) {
        shadowhook_unhook(_g_trace_data->hooktask);
        _g_trace_data->hooktask = nullptr;
    }
}

static void rearm_target_hook(void* proxy, void** ori)
{
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (_g_trace_data->hooktask != nullptr) {
        return; // 已有其他线程重挂，无需重复
    }
    _g_trace_data->hooktask = shadowhook_hook_func_addr(
        (void*)(_g_trace_data->start + _g_trace_data->target), proxy, ori);
}

size_t trace(size_t regs[31], size_t* stack_params, int stack_param_count)
{
    size_t function_address = _g_trace_data->start + _g_trace_data->target;
    LOGE("start trace:%p",(void*)function_address);
    disarm_target_hook();
    // 主线程 trace，无父线程，creator_tid=0
    return run_qbdi_trace(function_address, regs, stack_params, stack_param_count, 0);
}

size_t hook_and_trace_arg4(size_t x0,size_t x1,size_t x2,size_t x3)
{
    //可配置过滤（qtrace.config 的 filter）：非 0 时仅 x2 等于该值才 trace，其余直接调用原函数
    if(trace_filter != 0 && x2 != trace_filter)
    {
        return ori_arg4(x0,x1,x2,x3);
    }
    size_t regs[31] = {0};
    save_regs(regs);
    regs[0] = x0;
    regs[1] = x1;
    regs[2] = x2;
    regs[3] = x3;
    size_t result = trace(regs);
    rearm_target_hook((void*)(hook_and_trace_arg4), (void**)&ori_arg4);
    return result;
}

size_t hook_and_trace_arg8(size_t x0,size_t x1,size_t x2,size_t x3,
                           size_t x4,size_t x5,size_t x6,size_t x7)
{
    //可配置过滤（qtrace.config 的 filter）：非 0 时仅 x0 等于该值才 trace，其余直接调用原函数
    if(trace_filter != 0 && x0 != trace_filter)
    {
        return ori_arg8(x0,x1,x2,x3,x4,x5,x6,x7);
    }
    size_t regs[31] = {0};
    save_regs(regs);
    regs[0] = x0;
    regs[1] = x1;
    regs[2] = x2;
    regs[3] = x3;
    regs[4] = x4;
    regs[5] = x5;
    regs[6] = x6;
    regs[7] = x7;
    size_t result = trace(regs);
    rearm_target_hook((void*)(hook_and_trace_arg8), (void**)&ori_arg8);
    return result;
}



size_t hook_and_trace_arg5(size_t x0,size_t x1,size_t x2,size_t x3,size_t x4)
{
    //可配置过滤（qtrace.config 的 filter）：非 0 时仅 x2 等于该值才 trace，其余直接调用原函数
    if(trace_filter != 0 && x2 != trace_filter)
    {
        return ori_arg5(x0,x1,x2,x3,x4);
    }
    size_t regs[31] = {0};
    save_regs(regs);
    regs[0] = x0;
    regs[1] = x1;
    regs[2] = x2;
    regs[3] = x3;
    regs[4] = x4;
    size_t result = trace(regs);
    rearm_target_hook((void*)(hook_and_trace_arg5), (void**)&ori_arg5);
    return result;
}

size_t hook_and_trace_sig3_mode2(size_t x0,size_t x1,size_t x2,size_t x3)
{
    // trace commandIndex == 10412 (0x28ac, init) 或 10418 (0x28b2, sig3 Mode2)
    if(x2 != 10412 && x2 != 10418)
    {
        return ori_arg4(x0,x1,x2,x3);
    }

    if(x2 == 10418)
    {
        // sig3: 需要 params[6]==true (Mode2) 才trace
        JNIEnv* env = (JNIEnv*)x0;
        jobjectArray params = (jobjectArray)x3;
        jint paramsLen = env->GetArrayLength(params);
        if(paramsLen < 7)
        {
            return ori_arg4(x0,x1,x2,x3);
        }

        jobject param6 = env->GetObjectArrayElement(params, 6);
        if(param6 == nullptr)
        {
            return ori_arg4(x0,x1,x2,x3);
        }

        jclass boolClass = env->FindClass("java/lang/Boolean");
        jmethodID boolValueMethod = (boolClass != nullptr)
                                    ? env->GetMethodID(boolClass, "booleanValue", "()Z")
                                    : nullptr;
        if (boolValueMethod == nullptr) {
            // 找不到 Boolean.booleanValue（异常环境/被 hook）：无法判定 Mode2，
            // 按“非 Mode2”放行原函数，避免对空指针调方法在 proxy 里直接崩
            LOGE("sig3 Mode2: Boolean/booleanValue unavailable, pass through");
            if (boolClass != nullptr) {
                env->DeleteLocalRef(boolClass);
            }
            env->DeleteLocalRef(param6);
            return ori_arg4(x0,x1,x2,x3);
        }
        jboolean isMode2 = env->CallBooleanMethod(param6, boolValueMethod);
        env->DeleteLocalRef(boolClass);
        env->DeleteLocalRef(param6);

        if(!isMode2)
        {
            return ori_arg4(x0,x1,x2,x3);
        }
        LOGE("sig3 Mode2 triggered! commandIndex=0x28b2, params[6]=true");
    }
    else
    {
        LOGE("sig3 init triggered! commandIndex=0x28ac (10412)");
    }

    size_t regs[31] = {0};
    save_regs(regs);
    regs[0] = x0;
    regs[1] = x1;
    regs[2] = x2;
    regs[3] = x3;
    size_t result = trace(regs);
    rearm_target_hook((void*)(hook_and_trace_sig3_mode2), (void**)&ori_arg4);
    return result;
}

size_t hook_and_trace_dfp(size_t x0, size_t x1, size_t x2, size_t x3)
{
    // sub_4f86bc is the dedicated DFP handler (dispatched by cmdId 0x28c8698)
    // No filter needed — this function only handles DFP profile encrypt
    LOGE("DFP triggered! x0=0x%lx x1=0x%lx x2=0x%lx x3=0x%lx", x0, x1, x2, x3);

    size_t regs[31] = {0};
    save_regs(regs);
    regs[0] = x0;
    regs[1] = x1;
    regs[2] = x2;
    regs[3] = x3;
    size_t result = trace(regs);
    rearm_target_hook((void*)(hook_and_trace_dfp), (void**)&ori_arg4);
    return result;
}

func_arg_s1 ori_arg_s1{};

size_t hook_and_trace_s1(size_t x0, size_t x1, size_t x2, size_t x3,
                          size_t x4, size_t x5, size_t x6, size_t x7,
                          size_t sp0, size_t sp1, size_t sp2, size_t sp3)
{
    // 只打印指针/长度，不解引用：本函数运行在 shadowhook 上下文（QBDI 之外），
    // 一旦某次调用的参数布局与预期不符，解引用会直接 SIGSEGV 且无法被 trace 防线拦截。
    // 字符串内容交给 trace 日志本身（里面有完整内存访问记录）。
    LOGE("sign_request captured: method=%p(len=%lu) path=%p(len=%lu) query=%p(len=%lu) "
         "deviceId=%p bodyHash=%p(len=%lu) mua=%p(len=%lu)",
         (void*)x1, (unsigned long)x2, (void*)x3, (unsigned long)x4,
         (void*)x5, (unsigned long)x6, (void*)x7,
         (void*)sp0, (unsigned long)sp1, (void*)sp2, (unsigned long)sp3);

    size_t regs[31] = {0};
    save_regs(regs);
    regs[0] = x0; regs[1] = x1; regs[2] = x2; regs[3] = x3;
    regs[4] = x4; regs[5] = x5; regs[6] = x6; regs[7] = x7;

    size_t stack_params[4] = {sp0, sp1, sp2, sp3};
    size_t result = trace(regs, stack_params, 4);

    rearm_target_hook((void*)(hook_and_trace_s1), (void**)&ori_arg_s1);
    return result;
}

bool checkAndCallHook(QBDI::VM *vm, QBDI::GPRState *gprState,size_t addr,size_t lastaddr)
{
    if(_g_hook_data == nullptr || _g_hook_data->hookMap.empty())
    {
        return false;
    }
    auto it = _g_hook_data->hookMap.find(addr);
    if(it != _g_hook_data->hookMap.end())
    {
        if(it->second->ignore != nullptr && it->second->ignorenum != 0)
        {
            for(int i=0;i<it->second->ignorenum;i++)
            {
                if(it->second->ignore[i] == lastaddr)
                {
                    return false;
                }
            }
        }
        it->second->callback(vm,gprState);
        return true;
    }
    return false;
}

bool checkLibcTrace_pre(QBDI::VM *vm, QBDI::GPRState *gprState,size_t target)
{
    if(_g_libc_trace == nullptr || _g_libc_trace->map.empty())
    {
        return false;
    }
    auto it = _g_libc_trace->map.find(target);
    if(it != _g_libc_trace->map.end())
    {
        it->second->callback(vm,gprState);
        return true;
    }
    return false;
}

bool checkJniCall_pre(QBDI::VM *vm, QBDI::GPRState *gprState,size_t target)
{
    if(pJFunc == nullptr)
    {
        LOGE("checkJniCall,pJFunc not init");
        return false;
    }
    if(_g_jni_trace == nullptr || _g_jni_trace->map.empty())
    {
        return false;
    }
    auto it = _g_jni_trace->map.find(target);
    if(it != _g_jni_trace->map.end())
    {
        it->second->callback(vm,gprState);
        return true;
    }
    return false;
}

static thread_local size_t lastAddr = 0;
// 指令执行前：仅做 hook 检查 + 捕获执行前寄存器值
QBDI::VMAction showPreInstruction(QBDI::VM *vm, QBDI::GPRState *gprState, QBDI::FPRState *fprState, void *data)
{
    auto thiz = (class vm *)data;
    const QBDI::InstAnalysis *instAnalysis = vm->getInstAnalysis(QBDI::ANALYSIS_INSTRUCTION | QBDI::ANALYSIS_DISASSEMBLY | QBDI::ANALYSIS_OPERANDS);

    if(instAnalysis == nullptr)
    {
        lastAddr = 0;
        return QBDI::VMAction::CONTINUE;
    }

    bool hasCheck = false;
    //执行前hook
    hasCheck = checkAndCallHook(vm,gprState,(instAnalysis->address-thiz->base),(lastAddr - thiz->base));

    if(!hasCheck)
    {
        //检查blr：用 isCall+mnemonic 精确判定，替代子串匹配（"blr" 含 "br"，子串法易误配）
        if(instAnalysis->isCall && instAnalysis->mnemonic && !strcmp(instAnalysis->mnemonic,"BLR"))
        {
            for (int i = 0; i < instAnalysis->numOperands; ++i)
            {
                auto op = instAnalysis->operands[i];
                if (op.regAccess == QBDI::REGISTER_READ || op.regAccess == REGISTER_READ_WRITE)
                {
                    if (op.regCtxIdx != -1 && op.type == OPERAND_GPR && op.regCtxIdx != 31)
                    {
                        uint64_t regValue = QBDI_GPR_GET(gprState, op.regCtxIdx);
                        //blr大多是jni函数表调用，但也可能是函数指针方式的libc调用
                        //（如通过函数指针调pthread_create），两种表都要查，否则该子线程不会被重定向trace
                        hasCheck = checkLibcTrace_pre(vm,gprState,regValue);
                        if(!hasCheck)
                        {
                            hasCheck = checkJniCall_pre(vm,gprState,regValue);
                        }
                        break;
                    }
                }
            }
        }
    }

    if(!hasCheck)
    {
        //检查br：用 isBranch+mnemonic 精确判定（见上 blr 处说明）
        if(instAnalysis->isBranch && instAnalysis->mnemonic && !strcmp(instAnalysis->mnemonic,"BR") && hasLibctrace())
        {
            for (int i = 0; i < instAnalysis->numOperands; ++i)
            {
                auto op = instAnalysis->operands[i];
                if (op.regAccess == QBDI::REGISTER_READ || op.regAccess == REGISTER_READ_WRITE)
                {
                    if (op.regCtxIdx != -1 && op.type == OPERAND_GPR && op.regCtxIdx != 31)
                    {
                        uint64_t regValue = QBDI_GPR_GET(gprState, op.regCtxIdx);
                        //br可能是libc调用，也可能是jni调用
                        if(!checkLibcTrace_pre(vm,gprState,regValue))
                        {
                            checkJniCall_pre(vm,gprState,regValue);
                        }
                        break;
                    }
                }
            }
        }
    }

    // 捕获执行前的寄存器值（READ 和 READ_WRITE 的操作数）。
    // 热路径：snprintf + 固定缓冲，不做任何堆分配。
    preRegBuf[0] = '\0';
    {
        size_t pos = 0;
        for (int i = 0; i < instAnalysis->numOperands; ++i)
        {
            auto op = instAnalysis->operands[i];
            if (op.regAccess == QBDI::REGISTER_READ || op.regAccess == REGISTER_READ_WRITE)
            {
                if (op.regCtxIdx != -1 && op.type == OPERAND_GPR)
                {
                    uint64_t regValue = QBDI_GPR_GET(gprState, op.regCtxIdx);
                    char lower[16];
                    regNameLower(op.regName, lower, sizeof(lower));
                    int n = snprintf(preRegBuf + pos, sizeof(preRegBuf) - pos,
                                     "%s=0x%lx ", lower, (unsigned long)regValue);
                    if (n < 0 || (size_t)n >= sizeof(preRegBuf) - pos) {
                        break; // 截断：操作数过多（理论不会发生），丢弃剩余
                    }
                    pos += (size_t)n;
                }
            }
        }
    }

    if(debugInsn && instAnalysis->disassembly)
    {
        LOGE("trace:0x%lx:%s [%s]",(instAnalysis->address-thiz->base), instAnalysis->disassembly, preRegBuf);
    }

    // lightweight null deref guard: only fires on memory instructions (has '[')
    if (instAnalysis->mayLoad || instAnalysis->mayStore) {
        const char* dis = instAnalysis->disassembly;
        if (dis) {
            const char* bracket = strchr(dis, '[');
            if (bracket) {
                bracket++;
                while (*bracket == ' ') bracket++;
                if (bracket[0] == 'x' && bracket[1] >= '0' && bracket[1] <= '9') {
                    int reg = bracket[1] - '0';
                    if (bracket[2] >= '0' && bracket[2] <= '9')
                        reg = reg * 10 + (bracket[2] - '0');
                    if (reg >= 0 && reg <= 28) {
                        uint64_t val = QBDI_GPR_GET(gprState, reg);
                        if (val < 0x1000) {
                            LOGE("!!! NULL DEREF at 0x%lx: %s x%d=0x%lx",
                                 instAnalysis->address - thiz->base, dis, reg, val);
                            writelog();
                            return QBDI::VMAction::STOP;
                        }
                    }
                }
            }
        }
    }

    lastAddr = instAnalysis->address;
    // 记录当前线程正在执行的原始指令偏移，供 crash handler 定位崩溃点
    crash_set_last_insn(instAnalysis->address - thiz->base, thiz->moduleName.c_str());
    return QBDI::VMAction::CONTINUE;
}

// 指令执行后：输出完整的 Unidbg 格式单行
QBDI::VMAction showPostInstruction(QBDI::VM *vm, QBDI::GPRState *gprState, QBDI::FPRState *fprState, void *data)
{
    auto thiz = (class vm *)data;

    // 若上一条指令刚重定向了一个 pthread_create，此刻其返回值已在 x0：
    // 创建失败则回收暂存的 ctx（成功时由子线程 trampoline 释放）。无待定时仅一次判空。
    libc_pthread_create_post(gprState);

    const QBDI::InstAnalysis *instAnalysis = vm->getInstAnalysis(QBDI::ANALYSIS_INSTRUCTION | QBDI::ANALYSIS_DISASSEMBLY | QBDI::ANALYSIS_OPERANDS);
    if(instAnalysis == nullptr)
    {
        appendlogendl();
        return QBDI::VMAction::CONTINUE;
    }

    size_t offset = instAnalysis->address - thiz->base;

    // 1. Unidbg 行前缀: [HH:MM:SS NNN][module 0xOFFSET] [00000000]
    // NNN 槽位填本线程 tid：合并文件里一行 grep 即可抽出单个线程的指令流
    int tidn = (_logger != nullptr) ? _logger->tid : 0;
    appendformat("[00:00:00 %03d][%s 0x%lx] [00000000] ", tidn,
                 thiz->moduleName.c_str(), offset);

    // 2. 地址 + 带引号的反汇编（tab 转空格、去前导空白；栈缓冲处理，不分配堆）
    const char* disasmRaw = instAnalysis->disassembly ? instAnalysis->disassembly : "???";
    char dis[192];
    {
        const char* p = disasmRaw;
        while (*p == ' ' || *p == '\t') p++;
        size_t i = 0;
        while (p[i] != '\0' && i + 1 < sizeof(dis)) {
            dis[i] = (p[i] == '\t') ? ' ' : p[i];
            i++;
        }
        dis[i] = '\0';
    }
    appendformat("0x%lx: \"%s\"", offset, dis);

    // 3. 内存访问注解（仅第一个）
    auto memAccesses = vm->getInstMemoryAccess();
    if (!memAccesses.empty())
    {
        const auto& acc = memAccesses[0];
        if (acc.type == MEMORY_READ)
        {
            appendformat(" ; mem[READ] abs=0x%lx", acc.accessAddress);
        }
        else
        {
            appendformat(" ; mem[WRITE] abs=0x%lx", acc.accessAddress);
        }
    }

    // 4. 执行前寄存器值（PREINST 中捕获）
    if (preRegBuf[0] != '\0')
    {
        appendlog(" ");
        appendlog(preRegBuf);
    }

    // 5. 执行后寄存器值（WRITE / READ_WRITE 操作数），同样走栈缓冲
    {
        char postBuf[1024];
        size_t pos = 0;
        postBuf[0] = '\0';
        for (int i = 0; i < instAnalysis->numOperands; ++i)
        {
            auto op = instAnalysis->operands[i];
            if (op.regAccess == QBDI::REGISTER_WRITE || op.regAccess == REGISTER_READ_WRITE)
            {
                if (op.regCtxIdx != -1 && op.type == OPERAND_GPR)
                {
                    uint64_t regValue = QBDI_GPR_GET(gprState, op.regCtxIdx);
                    char lower[16];
                    regNameLower(op.regName, lower, sizeof(lower));
                    int n = snprintf(postBuf + pos, sizeof(postBuf) - pos,
                                     "%s=0x%lx ", lower, (unsigned long)regValue);
                    if (n < 0 || (size_t)n >= sizeof(postBuf) - pos) {
                        break; // 截断
                    }
                    pos += (size_t)n;
                }
            }
        }
        if (postBuf[0] != '\0')
        {
            appendlog("=> ");
            appendlog(postBuf);
        }
    }

    appendlogendl();

    if(sdslen(_logger->buf) > bufsize)
    {
        writelog();
    }
    return QBDI::VMAction::CONTINUE;
}

QBDI::VMAction showSyscall(QBDI::VM *vm, QBDI::GPRState *gprState, QBDI::FPRState *fprState, void *data)
{
    const QBDI::InstAnalysis *instAnalysis = vm->getInstAnalysis(QBDI::ANALYSIS_INSTRUCTION);
    if (instAnalysis->mnemonic && strcasecmp(instAnalysis->mnemonic, "svc") == 0)
    {

    }
    return QBDI::VMAction::CONTINUE;
}

QBDI::VM vm::init(size_t start,size_t end)
{
    // QBDI 初始化会读取 /proc/self/maps 等进程级状态，多个子线程可能同时进入，
    // 加锁串行化，避免初始化期间的数据竞争。
    static std::mutex init_mutex;
    std::lock_guard<std::mutex> lock(init_mutex);

    uint32_t cid;
    QBDI::GPRState *state;
    QBDI::VM qvm{};
    state = qvm.getGPRState();
    loadMemoryRanges();
    assert(state != nullptr);
    qvm.recordMemoryAccess(QBDI::MEMORY_READ_WRITE);

    //指令前hook
    cid = qvm.addCodeCB(QBDI::PREINST, showPreInstruction, this);
    assert(cid != QBDI::INVALID_EVENTID);

    //指令后hook
    cid = qvm.addCodeCB(QBDI::POSTINST, showPostInstruction, this);
    assert(cid != QBDI::INVALID_EVENTID);

    //TODO:syscall trace
    //cid = qvm.addCodeCB(QBDI::PREINST, showSyscall, this);
    //assert(cid != QBDI::INVALID_EVENTID);

    bool ret = qvm.addInstrumentedModuleFromAddr(reinterpret_cast<QBDI::rword>(start));
    if(!ret)
    {
        LOGE("init vm fail");
        assert(ret == true);
    }
    LOGE("init vm success");
    return qvm;
}
