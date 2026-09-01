//
// Created by fang on 23-12-19.
//

#ifndef QBDIRECORDER_LOGGER_H
#define QBDIRECORDER_LOGGER_H
#include <android/log.h>
#include "sstream"
#include "fstream"
#include "sds.h"
using namespace std;
// 日志
#define LOG_TAG "QTrace"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

struct logger{
    sds buf;
    std::string logfile;      // 本线程的临时分片文件（.part），trace 期间独占写入
    int64_t lastwrite;
    int64_t totallen;
    int     fd;               // 分片文件的持久 fd，避免每次刷盘 open/close
    int     seq;              // 本线程 trace 的开始序号（全局递增），用于最终排序
    int     tid;              // 本线程 tid
    int     creator_tid;      // 开启本线程的父线程 tid（主线程为 0）
    size_t  routine_offset;   // 本线程入口函数在目标 SO 中的偏移
};

extern thread_local logger *_logger;

// 设置最终归整所有线程 trace 的合并文件路径（首个进入 trace 的线程确定一次）。
void setMergedLogPath(size_t function_address);
// 将本线程的临时分片文件作为一个连续块追加进合并文件（加锁串行化，保证不乱序），
// 随后删除临时分片。在本线程 trace 结束时调用。
void mergeThreadLog();

void initLogger(size_t function_address, int creator_tid, size_t routine_offset);
void deleteLogger();
void writelog();
// 异步信号安全的日志刷写：仅用 open/write/close 原始 syscall，不触碰 malloc/ofstream。
// 供 crash handler 在崩溃时调用，尽量保留崩溃前的指令流。
void writelog_signal_safe();
void appendlog(const char* str);
// 带长度追加：调用方已知长度时省去一次 strlen（热路径用）
void appendlog_n(const char* str, size_t len);
void appendlogendl();
void appendformat(const char* format,...);

#endif //QBDIRECORDER_LOGGER_H
