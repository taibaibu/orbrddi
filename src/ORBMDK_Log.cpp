/**
 * @file ORBMDK_Log.cpp
 * @brief 统一日志实现（COMPAT_ANALYSIS §8.3 落地）
 *
 * 取代了原来的两套并行实现：
 *   - `ORBMDK_RDDI.cpp` 的 `ORBMDK_Log`（全功能、只有 RDDI 模块能用）
 *   - `ORBMDK_HID.cpp`  的 `HID_Log`  （简化版、不落盘）
 * 以及两处绕过级别、绕过 `ORBMDK_LOG_FILE` 的"命令级直写文件"
 * （`HidTrace` / `BulkTrace`，见 COMPAT_ANALYSIS §8.2）。
 *
 * 输出格式（所有模块统一）：
 *     [ORBMDK][HH:MM:SS.mmm][级别][模块][pid:tid] 消息
 */

#include "pch.h"
#include "ORBMDK_Log.h"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {

const char* const kLevelName[]       = { "DEBUG", "INFO", "WARN", "ERROR", "NONE" };
const char* const kEnvLevel          = "ORBMDK_LOG_LEVEL";
const char* const kEnvFile           = "ORBMDK_LOG_FILE";
const char* const kLevelFileName     = "ORBMDK_LOG_LEVEL";
const char* const kDefaultFileName   = "ORBMDK_RDDI.log";

std::mutex  g_lock;                       // 保护级别/路径/格式化/文件写入
int         g_level        = ORBMDK_LOG_ERROR;
bool        g_levelForced  = false;       // SetLevel 之后不再读文件/环境
ULONGLONG   g_levelStamp   = 0;
std::string g_filePath;
bool        g_filePathResolved = false;
ORBMDK_LogSinkFn g_sink    = nullptr;
void*       g_sinkContext  = nullptr;

const char* _levelName(int level)
{
    return (level >= 0 && level <= ORBMDK_LOG_NONE) ? kLevelName[level] : "?";
}

/** 取 %TEMP%\<name>；失败返回 false */
bool _tempFile(char* out, size_t cap, const char* name)
{
    char tmp[MAX_PATH] = {0};
    const DWORD n = GetTempPathA((DWORD)sizeof(tmp), tmp);
    if (n == 0 || n >= sizeof(tmp)) {
        return false;
    }
    const int w = snprintf(out, cap, "%s%s", tmp, name);
    return w > 0 && (size_t)w < cap;
}

/**
 * 解析阈值：%TEMP%\ORBMDK_LOG_LEVEL（热更新）→ 环境变量 → 默认 ERROR。
 * 每条日志都查文件太贵，1 秒最多查一次，因此改动最多延迟 1 秒生效。
 */
int _resolveLevel()
{
    if (g_levelForced) {
        return g_level;
    }

    const ULONGLONG now = GetTickCount64();
    if (now - g_levelStamp < 1000) {
        return g_level;
    }
    g_levelStamp = now;

    char path[MAX_PATH] = {0};
    if (_tempFile(path, sizeof(path), kLevelFileName)) {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "r") == 0) {
            int lvl = -1;
            if (fscanf_s(f, "%d", &lvl) != 1) {
                lvl = -1;
            }
            fclose(f);
            g_level = (lvl >= ORBMDK_LOG_DEBUG && lvl <= ORBMDK_LOG_ERROR) ? lvl
                                                                          : ORBMDK_LOG_ERROR;
            return g_level;
        }
    }

    const char* env = getenv(kEnvLevel);
    if (env && *env) {
        const int lvl = atoi(env);
        if (lvl >= ORBMDK_LOG_DEBUG && lvl <= ORBMDK_LOG_ERROR) {
            g_level = lvl;
            return g_level;
        }
    }

    g_level = ORBMDK_LOG_ERROR;            // 默认：仅 ERROR
    return g_level;
}

const std::string& _filePath()
{
    if (!g_filePathResolved) {
        g_filePathResolved = true;
        const char* env = getenv(kEnvFile);
        if (env && *env) {
            g_filePath = env;
        } else {
            char p[MAX_PATH] = {0};
            if (_tempFile(p, sizeof(p), kDefaultFileName)) {
                g_filePath = p;
            }
        }
    }
    return g_filePath;
}

/**
 * 追加一行并**立即关闭**：宿主 µVision 会独占日志文件，
 * 不关句柄的话外部诊断工具读不到（历史问题，别再改回长持有）。
 */
void _appendLine(const char* line)
{
    const std::string& path = _filePath();
    if (path.empty()) {
        return;
    }

    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "a") != 0 || !f) {
        return;
    }
    fputs(line, f);
    fputc('\n', f);
    fclose(f);
}

/** 统一格式化：[ORBMDK][时间][级别][模块][pid:tid] 消息（超长显式标记截断） */
void _format(char* buf, size_t cap, int level, const char* module,
             const char* fmt, va_list args)
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    const int off = snprintf(buf, cap, "[ORBMDK][%02u:%02u:%02u.%03u][%s][%s][%lu:%lu] ",
                             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                             _levelName(level), module ? module : "-",
                             (unsigned long)GetCurrentProcessId(),
                             (unsigned long)GetCurrentThreadId());
    if (off < 0) {
        buf[0] = '\0';
        return;
    }
    if ((size_t)off >= cap) {
        buf[cap - 1] = '\0';
        return;
    }
    if (!fmt) {
        return;
    }

    const int n = vsnprintf(buf + off, cap - off, fmt, args);
    if (n < 0) {
        buf[off] = '\0';
    } else if ((size_t)(off + n) >= cap - 1) {
        // 显式标记截断：静默截断会让日志"看起来完整"却少了后半段
        const size_t tail = cap - 5;
        if (tail > (size_t)off) {
            memcpy(buf + tail, "...\0", 4);
        }
    }
}

} // namespace

void ORBMDK_LogWrite(int level, const char* module, const char* fmt, ...)
{
    if (level < ORBMDK_LOG_DEBUG || level > ORBMDK_LOG_ERROR) {
        return;
    }

    std::string line;
    ORBMDK_LogSinkFn sink = nullptr;
    void* sinkContext = nullptr;

    {
        std::lock_guard<std::mutex> guard(g_lock);

        if (level < _resolveLevel()) {
            return;                        // ★ 过滤在格式化之前，零开销
        }

        char buf[1024];
        va_list args;
        va_start(args, fmt);
        _format(buf, sizeof(buf), level, module, fmt, args);
        va_end(args);

        line.assign(buf);
        _appendLine(line.c_str());

        sink = g_sink;
        sinkContext = g_sinkContext;
    }

    // 锁外输出：宿主的日志回调很可能再调回本模块，锁内调用会自锁。
    printf("%s\n", line.c_str());
    OutputDebugStringA(line.c_str());

    if (sink) {
        sink(sinkContext, line.c_str(), level);
    }
}

void ORBMDK_LogTrace(const char* module, const char* fmt, ...)
{
    std::lock_guard<std::mutex> guard(g_lock);

    // 命令级日志：不参与级别过滤（排障线索），只落文件。
    // 级别显示为 INFO，与历史格式（[ORBMDK][INFO][BULK] ...）兼容。
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    _format(buf, sizeof(buf), ORBMDK_LOG_INFO, module, fmt, args);
    va_end(args);

    _appendLine(buf);
}

void ORBMDK_LogSetLevel(int level)
{
    std::lock_guard<std::mutex> guard(g_lock);
    g_level = level;
    g_levelForced = true;
}

int ORBMDK_LogGetLevel(void)
{
    std::lock_guard<std::mutex> guard(g_lock);
    return _resolveLevel();
}

void ORBMDK_LogSetFile(const char* path)
{
    std::lock_guard<std::mutex> guard(g_lock);
    if (path && *path) {
        g_filePath = path;
        g_filePathResolved = true;
    } else {
        g_filePath.clear();
        g_filePathResolved = false;        // 空 = 下次重新解析（回到默认路径）
    }
}

void ORBMDK_LogSetCallback(ORBMDK_LogSinkFn fn, void* context)
{
    std::lock_guard<std::mutex> guard(g_lock);
    g_sink = fn;
    g_sinkContext = context;
}

void ORBMDK_LogShutdown(void)
{
    std::lock_guard<std::mutex> guard(g_lock);

    g_sink = nullptr;
    g_sinkContext = nullptr;

    // 文件是"写后即关"，没有需要 fclose 的常驻句柄；这里保留统一收尾入口，
    // 由 DllMain(DLL_PROCESS_DETACH) 调用（宿主可随时卸载本 DLL）。
}
