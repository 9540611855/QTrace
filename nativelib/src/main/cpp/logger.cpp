//
// Created by zgy on 2025/12/3.
//
#include "logger.h"
#include "TraceLogger.h"
#include "sds.h"
#include <unistd.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <atomic>
#include <string>

// 每个 trace 线程各自持有独立的 logger，避免多线程共享 sds 缓冲导致堆破坏
thread_local logger *_logger = nullptr;

// ---- 合并输出：所有线程（含子线程）的 trace 最终归整到同一个文件 ----
// 各线程 trace 期间写自己的临时分片文件（.part），结束时在锁保护下把整块
// 追加进合并文件，从而“单文件、按线程成块、不乱序”。
static std::mutex       g_merge_mutex;      // 保护对合并文件的追加
static std::string      g_merged_path;      // 合并文件路径（首个线程确定一次）
static std::once_flag   g_merged_once;      // 保证只初始化一次
static std::atomic<int> g_seq_counter{0};   // 线程 trace 开始序号（全局递增）

void setMergedLogPath(size_t function_address)
{
    std::call_once(g_merged_once, [function_address]() {
        // 复用既有的日志目录/命名规则生成一个基准路径，去掉扩展名后统一加后缀，
        // 使合并文件与分片文件同目录、易于关联。
        std::string base = getLogPath(LogType::QBDI_TRACE, (void*)function_address);
        if (base.empty()) {
            // 建目录/时间戳失败：大声报错。g_merged_path 保持为空，
            // mergeThreadLog 会因此保留各线程分片，日志不至于静默丢到坏路径。
            LOGE("setMergedLogPath: getLogPath failed! trace 日志目录不可用，将只保留 .part 分片");
            return;
        }
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) {
            g_merged_path = base.substr(0, dot) + "_merged" + base.substr(dot);
        } else {
            g_merged_path = base + "_merged";
        }
    });
}

void initLogger(size_t function_address, int creator_tid, size_t routine_offset)
{
    _logger = new logger();
    _logger->buf = sdsempty();
    pid_t tid = (pid_t)syscall(SYS_gettid);

    // 本线程的临时分片文件：合并文件路径 + .tidNNN.part
    std::string base = g_merged_path.empty()
                       ? getLogPath(LogType::QBDI_TRACE, (void*)function_address)
                       : g_merged_path;
    if (base.empty()) {
        LOGE("initLogger: getLogPath failed! 本线程日志将写入相对路径，刷盘大概率失败");
    }
    std::string path = base + ".tid" + std::to_string(tid) + ".part";

    _logger->logfile = path;
    _logger->lastwrite = 0;
    _logger->totallen = 0;
    // 持久 fd：整个 trace 期间只 open/close 一次，writelog 直接 write。
    // 每次 trace 会话新建分片（会话结束时 mergeThreadLog 会 unlink），TRUNC 合理。
    _logger->fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (_logger->fd < 0) {
        LOGE("initLogger: open part file fail: %s", path.c_str());
    }
    // 预留适量空间，避免 sds 反复扩容搬移（大缓冲留给长 trace 按需增长）
    _logger->buf = sdsMakeRoomFor(_logger->buf, 0x40000);
    _logger->seq = g_seq_counter.fetch_add(1);
    _logger->tid = (int)tid;
    _logger->creator_tid = creator_tid;
    _logger->routine_offset = routine_offset;
}

void deleteLogger()
{
    if(_logger != nullptr)
    {
        sdsfree(_logger->buf);
        if(_logger->fd >= 0)
        {
            close(_logger->fd);
            _logger->fd = -1;
        }
    }
    delete _logger;
    _logger = nullptr;
}

void appendlog(const char* str)
{
      if(_logger != nullptr)
      {
          _logger->buf = sdscat(_logger->buf, str);
      }
}

void appendlog_n(const char* str, size_t len)
{
    if(_logger != nullptr)
    {
        _logger->buf = sdscatlen(_logger->buf, str, len);
    }
}

void appendlogendl()
{
    appendlog("\n");
}

void appendformat(const char* format,...)
{
    va_list ap;
    va_start(ap, format);
    _logger->buf = sdscatvprintf(_logger->buf,format,ap);
    va_end(ap);
}

// 把 buf 全量写入 fd，处理短写与 EINTR
static bool write_all(int fd, const char* buf, size_t count)
{
    size_t written = 0;
    while (written < count) {
        ssize_t n = write(fd, buf + written, count - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOGE("writelog: write failed: %s", strerror(errno));
            return false;
        }
        written += (size_t)n;
    }
    return true;
}

void writelog()
{
    _logger->totallen = _logger->lastwrite + sdslen(_logger->buf);
    LOGE("write log:%lx,%lx,%s", _logger->lastwrite,_logger->totallen,_logger->logfile.c_str());
    if (_logger->fd >= 0) {
        write_all(_logger->fd, _logger->buf, sdslen(_logger->buf));
    } else {
        // fd 打开失败时的兜底：退回 ofstream 追加
        std::ofstream out(_logger->logfile.c_str(), std::ios::app);
        if (!out.is_open()) {
            LOGE("Failed to create trace log file: %s", _logger->logfile.c_str());
            return ;
        }
        out << _logger->buf;
        out.close();
    }
    _logger->lastwrite = _logger->totallen;
    sdsfree(_logger->buf);
    _logger->buf = sdsempty();
    LOGE("write log done!");
}

void writelog_signal_safe()
{
    // 崩溃上下文中调用：绝不使用 malloc/free/ofstream（堆可能已损坏）。
    // 只读取已有的 sds 缓冲并用原始 syscall 追加写入文件。
    if (_logger == nullptr || _logger->buf == nullptr) {
        return;
    }
    size_t len = sdslen(_logger->buf);
    if (len == 0) {
        return;
    }
    const char* path = _logger->logfile.c_str(); // 只读，不分配
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return;
    }
    const char* p = _logger->buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n <= 0) {
            break; // EINTR 以外的错误直接放弃，保证 handler 不卡住
        }
        p += n;
        remaining -= (size_t)n;
    }
    close(fd);
    // 不修改 buf/lastwrite：此处允许与已写入内容有少量重叠，
    // 崩溃排查场景下“宁可多写也不丢”。
}

void mergeThreadLog()
{
    if (_logger == nullptr) {
        return;
    }
    // 1. 先把本线程剩余的 buf 落到自己的分片文件
    if (_logger->buf != nullptr && sdslen(_logger->buf) > 0) {
        writelog();
    }

    // 合并路径未确定（正常流程不会发生：run_qbdi_trace 会先调 setMergedLogPath）。
    // 此时保留分片文件直接返回，避免把文件拷贝进它自己再删掉。
    if (g_merged_path.empty()) {
        LOGE("mergeThreadLog: merged path unset, keep part file %s",
             _logger->logfile.c_str());
        return;
    }

    // 2. 加锁，把整个分片作为一个连续块追加进合并文件（保证块内不被其他线程打断）
    std::lock_guard<std::mutex> lock(g_merge_mutex);

    std::string merged = g_merged_path;
    int out_fd = open(merged.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (out_fd < 0) {
        LOGE("mergeThreadLog: open merged file fail: %s", merged.c_str());
        return;
    }

    // 块头：标注线程序号、tid、父线程、入口偏移，方便工具/大模型还原线程逻辑
    char header[256];
    int hn = snprintf(header, sizeof(header),
        "\n==== QTrace THREAD seq=%d tid=%d creator_tid=%d routine=0x%zx ====\n",
        _logger->seq, _logger->tid, _logger->creator_tid, _logger->routine_offset);
    if (hn > 0) {
        (void)!write(out_fd, header, (size_t)hn);
    }

    // 拷贝分片内容
    int in_fd = open(_logger->logfile.c_str(), O_RDONLY);
    if (in_fd >= 0) {
        char iobuf[65536];
        ssize_t r;
        bool out_failed = false;
        while (!out_failed && (r = read(in_fd, iobuf, sizeof(iobuf))) > 0) {
            char* q = iobuf;
            ssize_t left = r;
            while (left > 0) {
                ssize_t w = write(out_fd, q, (size_t)left);
                if (w <= 0) {
                    // 合并文件写入失败（磁盘满等）：中止拷贝，保住未删除的分片文件
                    out_failed = true;
                    break;
                }
                q += w;
                left -= w;
            }
        }
        close(in_fd);
        if (out_failed) {
            LOGE("mergeThreadLog: write merged file failed, part file kept: %s",
                 _logger->logfile.c_str());
            close(out_fd);
            return;
        }
    } else {
        LOGE("mergeThreadLog: open part file fail: %s", _logger->logfile.c_str());
    }

    char footer[128];
    int fn = snprintf(footer, sizeof(footer),
        "==== QTrace THREAD END seq=%d tid=%d ====\n",
        _logger->seq, _logger->tid);
    if (fn > 0) {
        (void)!write(out_fd, footer, (size_t)fn);
    }

    close(out_fd);

    // 3. 删除临时分片
    unlink(_logger->logfile.c_str());
    LOGE("mergeThreadLog: merged tid=%d seq=%d into %s",
         _logger->tid, _logger->seq, merged.c_str());
}