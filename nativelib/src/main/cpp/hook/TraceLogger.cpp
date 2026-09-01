//
// Created by fang on 2024/12/26.
// TraceLogger - QBDI追踪日志记录工具实现
//

#include "TraceLogger.h"
#include "HookUtils.h"
#include "logger.h"
#include <fstream>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cerrno>
#include <dlfcn.h>
#include <sstream>
#include <sys/syscall.h>
#include <sys/types.h>
#include <mutex>
#include "vm.h"

//目前仅有线程创建需要统计别的hook 暂时不需要有任何统计信息
// 全局统计数据
static ThreadStatistics g_thread_stats;
static std::map<LogType, int> g_log_counters;
// 多个 trace 线程会并发调用 getLogPath，采番/建目录需串行化
static std::mutex g_log_path_mutex;

// 统计报告配置
#define REPORT_CALL_INTERVAL 60     // 每60次调用生成报告
#define REPORT_TIME_INTERVAL 60     // 60秒无新线程创建时生成报告

// SO分类功能实现
SoType classifySoType(const std::string& so_path) {
    if (so_path.empty()) {
        return SoType::UNKNOWN_SO;
    }

    // 系统SO判断
    if (so_path.find("/system/") == 0 ||
        so_path.find("/apex/") == 0 ||
        so_path.find("/vendor/") == 0 ||
        so_path.find("/product/") == 0 ||
        so_path.find("/odm/") == 0) {
        return SoType::SYSTEM_SO;
    }

    // 应用SO判断
    char* app_name = getAppName();
    if (app_name && strlen(app_name) > 0) {
        std::string data_app_path = std::string("/data/app/") + app_name;
        std::string data_data_path = std::string("/data/data/") + app_name;

        if (so_path.find(data_app_path) == 0 ||
            so_path.find(data_data_path) == 0) {
            return SoType::APP_SO;
        }
    }

    // 检查是否在应用相关目录中
    if (so_path.find("/data/app/") == 0 ||
        so_path.find("/data/data/") == 0) {
        return SoType::APP_SO;
    }

    // 其他路径归类为自定义SO
    if (so_path.find("/") == 0) {  // 绝对路径
        return SoType::CUSTOM_SO;
    }

    return SoType::UNKNOWN_SO;
}

const char* getSoTypeName(SoType type) {
    switch (type) {
        case SoType::SYSTEM_SO:  return "系统SO";
        case SoType::APP_SO:     return "应用SO";
        case SoType::CUSTOM_SO:  return "自定义SO";
        case SoType::UNKNOWN_SO: return "未知SO";
        default:                 return "未知类型";
    }
}

std::string getSoPathFromAddress(void* address) {
    if (!address) {
        return "";
    }

    Dl_info info;
    if (dladdr(address, &info) && info.dli_fname) {
        return std::string(info.dli_fname);
    }

    return "";
}

// 获取日志类型名称和前缀
const char* getLogTypeName(LogType type) {
    switch (type) {
        case LogType::QBDI_TRACE:    return "指令trace";
        case LogType::THREAD_TRACE:  return "thread";
        case LogType::HOOK_EVENT:    return "hook";
        case LogType::GENERAL_EVENT: return "event";
        default:                     return "unknown";
    }
}

// 获取日志文件前缀
const char* getLogTypePrefix(LogType type) {
    switch (type) {
        case LogType::QBDI_TRACE:    return "qbdi_";
        case LogType::THREAD_TRACE:  return "thread_";
        case LogType::HOOK_EVENT:    return "hook_";
        case LogType::GENERAL_EVENT: return "event_";
        default:                     return "unknown_";
    }
}

bool getFormattedTimestamp(char* buffer, size_t buffer_size) {
    if (!buffer || buffer_size < 16) {
        return false;
    }

    time_t rawtime;
    struct tm * timeinfo;
    time(&rawtime);
    timeinfo = localtime(&rawtime);

    if (strftime(buffer, buffer_size, "%Y%m%d_%H%M%S", timeinfo) == 0) {
        return false;
    }

    return true;
}

bool ensureLogDirectory(const std::string& log_dir) {

    struct stat st = {0};
    if (stat(log_dir.c_str(), &st) == -1) {
        LOGI("Directory does not exist, creating: %s", log_dir.c_str());

        // 递归创建父目录
        std::string parent_dir = log_dir;
        if (parent_dir.back() == '/') {
            parent_dir.pop_back();
        }

        size_t pos = parent_dir.find_last_of('/');
        if (pos != std::string::npos && pos > 0) {
            std::string parent = parent_dir.substr(0, pos);
            if (!ensureLogDirectory(parent + "/")) {
                return false;
            }
        }

        if (mkdir(log_dir.c_str(), 0755) != 0) {
            LOGE("Failed to create log directory: %s, errno: %d", log_dir.c_str(), errno);
            return false;
        }
        LOGI("Directory created successfully: %s", log_dir.c_str());
    }
    return true;
}

std::string getLogPath(LogType type,void* address)
{
    std::lock_guard<std::mutex> lock(g_log_path_mutex);
    std::string private_path = getPrivatePath();

    // 创建trace_logs目录
    std::string trace_logs_dir = private_path + "trace_logs/";
    if (!ensureLogDirectory(trace_logs_dir)) {
        LOGE("Failed to ensure trace_logs directory: %s", trace_logs_dir.c_str());
        return "";
    }

    // 获取时间戳
    char timestamp[80];
    if (!getFormattedTimestamp(timestamp, sizeof(timestamp))) {
        LOGE("Failed to get timestamp");
        return "";
    }

    // 增加计数器
    g_log_counters[type]++;

    // 获取地址信息
    char addr_info[128];
    getAddressInfo(address, addr_info, sizeof(addr_info));

    // 生成文件名：前缀_时间戳_地址信息_计数器.txt，放在trace_logs目录下
    char filename[256];
    snprintf(filename, sizeof(filename), "%s%s_%s_%d.txt",
             getLogTypePrefix(type), timestamp, addr_info, g_log_counters[type]);

    std::string full_path = trace_logs_dir + filename;
    LOGI("log path: %s", full_path.c_str());
    return full_path;
}
