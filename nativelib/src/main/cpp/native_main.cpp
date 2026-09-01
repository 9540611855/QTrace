#include <jni.h>
#include <string>
#include <iostream>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <dlfcn.h>
#include <fstream>
#include <unistd.h>
#include "vm.h"
#include "HookUtils.h"
#include "qbdihook.h"
#include "jnitrace.h"
#include "libctrace.h"
#include "shadowhook.h"
#include "crashhandler.h"
using namespace std;

static void addHooks()
{
    addHook(0x71DD54,hook_0x71DD54);
}

static void addLibctrace()
{
    void * handle = dlopen("libc.so",RTLD_LAZY);
    if(handle == nullptr)
    {
        LOGE("dlopen libc fail");
        return;
    }
    addLibctrace(handle,libc_access,"access");
    addLibctrace(handle,libc_system_property_get,"__system_property_get");
    addLibctrace(handle,libc_memcpy,"memcpy");
    addLibctrace(handle,libc_pthread_create,"pthread_create");
    addLibctrace(handle,libc_fopen,"fopen");
    addLibctrace(handle,libc_lstat,"lstat");
    addLibctrace(handle,libc_stat,"stat");
    addLibctrace(handle,libc_fstatat,"fstatat");
    addLibctrace(handle,libc_execve,"execve");
    addLibctrace(handle,libc_clock_gettime,"clock_gettime");
    addLibctrace(handle,libc_strlen,"strlen");
    addLibctrace(handle,libc_memmove,"memmove");
    addLibctrace(handle,libc_memset,"memset");
    addLibctrace(handle,libc_kill,"kill");
    addLibctrace(handle,libc_abort,"abort");
    addLibctrace(handle,libc_exit,"exit");
    dlclose(handle);
}

static void addJNItrace()
{
    addJNITrace((void*)pJFunc->NewStringUTF,"NewStringUTF",trace_NewStringUTF);
    addJNITrace((void*)pJFunc->GetStringUTFChars,"GetStringUTFChars",trace_GetStringUTFChars);
    addJNITrace((void*)pJFunc->NewString,"NewString",trace_NewString);
    addJNITrace((void*)pJFunc->FindClass,"FindClass",trace_FindClass);
    addJNITrace((void*)pJFunc->GetFieldID,"GetFieldID",trace_GetFieldID);
    addJNITrace((void*)pJFunc->GetMethodID,"GetMethodID",trace_GetMethodID);
    addJNITrace((void*)pJFunc->RegisterNatives,"RegisterNatives",trace_RegisterNatives);
    addJNITrace((void*)pJFunc->GetLongField,"GetLongField",trace_GetLongField);
    addJNITrace((void*)pJFunc->GetStaticMethodID,"GetStaticMethodID",trace_GetStaticMethodID);
    addJNITrace((void*)pJFunc->CallStaticObjectMethodV,"CallStaticObjectMethodV",trace_CallStaticObjectMethodV);
    addJNITrace((void*)pJFunc->GetStaticFieldID,"GetStaticFieldID",trace_GetStaticFieldID);
    addJNITrace((void*)pJFunc->GetIntField,"GetIntField",trace_GetIntField);
    addJNITrace((void*)pJFunc->GetByteArrayRegion,"GetByteArrayRegion",trace_GetByteArrayRegion);
    addJNITrace((void*)pJFunc->GetArrayLength,"GetArrayLength",trace_GetArrayLength);
    addJNITrace((void*)pJFunc->GetByteArrayElements,"GetByteArrayElements",trace_GetByteArrayElements);
}

/* trace的so的名称*/
string libname;

/* 将原始so放在手机的一个目录中，方便FQ读取,
 * FQ将通过这个so文p定位内存中目标so的位置'
 * 因为在maps中的so文件名不总是原始的so名，例如config.arm64_v8a.apk这种
 * */
const char *libdir = "/data/local/tmp/";
string libpath;
/*需要trace的函数地址*/
size_t trace_func = 0;

/* ===================== 外置配置 (/data/local/tmp/qtrace.config) =====================
 * 换目标/换参数不再需要改源码重编。文件不存在时使用下面的内置默认值。
 * 格式为 key=value，每行一条，# 开头为注释：
 *   so=libtiny.so            目标SO名（文件需放在 /data/local/tmp/ 下）
 *   func=157084              目标函数在SO内的偏移（十六进制，可带0x）
 *   mode=s1                  hook proxy 模式：arg4/arg5/arg8/sig3/dfp/s1
 *   filter=0                 参数过滤值（arg4/arg5模式比较x2；0=不过滤。十进制或0x十六进制）
 *   bufsize=1000000          每线程日志缓冲刷盘阈值（十六进制；默认16MB）
 *   max_threads=0            同时trace的线程数上限，0=不限
 * ================================================================================== */
#define QTRACE_CONFIG_PATH "/data/local/tmp/qtrace.config"

static char  cfg_so[256]  = "libtiny.so";
static size_t cfg_func    = 0x157084;
static char  cfg_mode[16] = "s1";
static size_t cfg_bufsize = 0x1000000; // 16MB：logger 是 thread_local 的，每线程一份，不宜过大
static int   cfg_max_threads = 0;      // 0 = 不限

static char* trimInPlace(char* s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\r' || s[len-1] == '\n')) {
        s[--len] = '\0';
    }
    return s;
}

static void loadConfig()
{
    FILE* fp = fopen(QTRACE_CONFIG_PATH, "r");
    if (fp == nullptr) {
        LOGE("config %s not found, use built-in defaults (so=%s func=0x%lx mode=%s)",
             QTRACE_CONFIG_PATH, cfg_so, cfg_func, cfg_mode);
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), fp) != nullptr) {
        char* hash = strchr(line, '#');
        if (hash != nullptr) *hash = '\0';
        char* eq = strchr(line, '=');
        if (eq == nullptr) continue;
        *eq = '\0';
        char* key = trimInPlace(line);
        char* val = trimInPlace(eq + 1);
        if (*key == '\0' || *val == '\0') continue;
        if (strcmp(key, "so") == 0) {
            strncpy(cfg_so, val, sizeof(cfg_so) - 1);
        } else if (strcmp(key, "func") == 0) {
            cfg_func = strtoul(val, nullptr, 16);
        } else if (strcmp(key, "mode") == 0) {
            strncpy(cfg_mode, val, sizeof(cfg_mode) - 1);
        } else if (strcmp(key, "filter") == 0) {
            trace_filter = strtoul(val, nullptr, 0);
        } else if (strcmp(key, "bufsize") == 0) {
            cfg_bufsize = strtoul(val, nullptr, 16);
        } else if (strcmp(key, "max_threads") == 0) {
            cfg_max_threads = (int)strtol(val, nullptr, 10);
        } else {
            LOGE("config: unknown key \"%s\" (ignored)", key);
        }
    }
    fclose(fp);
    LOGE("config loaded: so=%s func=0x%lx mode=%s filter=0x%lx bufsize=0x%lx max_threads=%d",
         cfg_so, cfg_func, cfg_mode, (unsigned long)trace_filter,
         (unsigned long)cfg_bufsize, cfg_max_threads);
}

void config()
{
    loadConfig();

    libname = cfg_so;

    string localdir(libdir);
    libpath = localdir + libname;
    trace_func = cfg_func;

    //trace buffer 累计多少字节往本地写一次。设置为0表示每trace一条指令就写入本地。
    //logger 为 thread_local，每个被 trace 线程各占一份缓冲，默认16MB/线程
    setBufferSize((int)cfg_bufsize);

    setMaxTraceThreads(cfg_max_threads);

    enableDebugInsn(false);

    //是否开启jni trace 日志，可在logcat中查看jni调用
    enable_jni_trace_debug(true);

    //是否开启libc trace 日志，可在logcat中查看libc调用
    enable_libc_trace_debug(true);

}

void init_shadowhook()
{
    shadowhook_init(SHADOWHOOK_MODE_UNIQUE, true);
    shadowhook_set_debuggable(true);
}

/* 跨线程共享的 trace 目标信息 */
static std::atomic<bool> trace_installed{false};
static char target_name[256] = {0};
static char target_path[512] = {0};
static size_t target_func = 0;

/* mode → hook proxy 映射表：不同目标的函数签名/过滤条件不同，
 * 由 qtrace.config 的 mode 选择，避免换目标改源码 */
struct HookModeEntry {
    const char* name;
    void* proxy;
    void** ori;
};
static const HookModeEntry g_hook_modes[] = {
    {"arg4", (void*)hook_and_trace_arg4,       (void**)&ori_arg4},
    {"arg5", (void*)hook_and_trace_arg5,       (void**)&ori_arg5},
    {"arg8", (void*)hook_and_trace_arg8,       (void**)&ori_arg8},
    {"sig3", (void*)hook_and_trace_sig3_mode2, (void**)&ori_arg4},
    {"dfp",  (void*)hook_and_trace_dfp,        (void**)&ori_arg4},
    {"s1",   (void*)hook_and_trace_s1,         (void**)&ori_arg_s1},
};

static void setup_trace()
{
    auto soinfo = getSoBaseAddress(target_path, target_name);
    if(soinfo.start != 0)
    {
        const HookModeEntry* mode = nullptr;
        for (const auto& m : g_hook_modes) {
            if (strcmp(m.name, cfg_mode) == 0) {
                mode = &m;
                break;
            }
        }
        if (mode == nullptr) {
            LOGE("setup_trace: unknown mode \"%s\", valid: arg4/arg5/arg8/sig3/dfp/s1 — trace NOT installed", cfg_mode);
            return;
        }
        _g_trace_data = new g_trace_data();
        _g_trace_data->base = soinfo.start;
        _g_trace_data->start = soinfo.start;
        _g_trace_data->end = soinfo.start + soinfo.size;
        _g_trace_data->target = target_func;
        _g_trace_data->hooktask = shadowhook_hook_func_addr((void*)(_g_trace_data->start + _g_trace_data->target),
                                                            mode->proxy,
                                                            mode->ori);
        LOGE("trace hook installed on %s+0x%lx (%s mode)", target_name, target_func, mode->name);
    }
    else
    {
        LOGE("fail to load %s", target_name);
    }
}

/* 目标 SO 加载完成后的初始化（原子防重：多个线程同时 dlopen 同一 SO 只安装一次） */
static void maybe_setup_trace(const char* filename)
{
    if (filename == nullptr || target_name[0] == '\0') {
        return;
    }
    if (strstr(filename, target_name) == nullptr) {
        return;
    }
    bool expected = false;
    if (!trace_installed.compare_exchange_strong(expected, true)) {
        return;
    }
    LOGE("detected %s loaded, setting up trace...", target_name);
    setup_trace();
}

/* hook dlopen 与 android_dlopen_ext 两个入口等待目标 SO 出现：
 * bionic 中二者互不经过对方（Java 层 System.load 走 android_dlopen_ext，
 * native 直调 dlopen 走 dlopen），只挂一个会漏掉另一条加载路径。
 * 命中后不卸载 hook，仅靠 trace_installed 原子短路——省去“在 proxy 里
 * unhook 自己”的边界情况，代价只是每次 dlopen 多一次 strstr。 */
typedef void* (*dlopen_ext_t)(const char*, int, const void*);
typedef void* (*dlopen_t)(const char*, int);
static dlopen_ext_t ori_dlopen_ext = nullptr;
static dlopen_t ori_dlopen = nullptr;
static void* dlopen_ext_hook_stub = nullptr;
static void* dlopen_hook_stub = nullptr;

static void* my_dlopen_ext(const char* filename, int flags, const void* extinfo)
{
    void* ret = ori_dlopen_ext(filename, flags, extinfo);
    maybe_setup_trace(filename);
    return ret;
}

static void* my_dlopen(const char* filename, int flags)
{
    void* ret = ori_dlopen(filename, flags);
    maybe_setup_trace(filename);
    return ret;
}

void trace()
{
    /*trace 配置*/
    config();

    /*JNI Trace*/
    initJni();
    //添加监控的jni函数
    addJNItrace();

    /*libc Trace*/
    initLibcTrace();
    //添加监控的libc函数
    addLibctrace();

    /*自定义hook监控*/
   //initHookData();
    //添加目标so的hook点位
    //addHooks();

    /* 拷贝到 static char[] 保证跨线程可见 */
    strncpy(target_name, libname.c_str(), sizeof(target_name) - 1);
    strncpy(target_path, libpath.c_str(), sizeof(target_path) - 1);
    target_func = trace_func;

    /* 先尝试直接查找 (Frida inject.js 方式下 SO 可能已加载) */
    auto soinfo = getSoBaseAddress(target_path, target_name);
    if(soinfo.start != 0)
    {
        setup_trace();
    }
    else
    {
        /* SO 还没加载 (Zygisk 注入), hook dlopen/android_dlopen_ext 等待它出现 */
        LOGE("%s not loaded yet, hooking dlopen & android_dlopen_ext to wait...", target_name);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        dlopen_ext_hook_stub = shadowhook_hook_sym_name(
            "libdl.so",
            "android_dlopen_ext",
            (void*)my_dlopen_ext,
            (void**)&ori_dlopen_ext);
        dlopen_hook_stub = shadowhook_hook_sym_name(
            "libdl.so",
            "dlopen",
            (void*)my_dlopen,
            (void**)&ori_dlopen);
        if (dlopen_ext_hook_stub == nullptr && dlopen_hook_stub == nullptr)
        {
            LOGE("failed to hook both dlopen & android_dlopen_ext, errno: %d", shadowhook_get_errno());
        }
    }
}

__unused __attribute__((constructor)) void init_main() {
    LOGE("Injected!");
    installCrashHandler();
    init_shadowhook();
    trace();
}