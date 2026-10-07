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
#include <climits>
#include <atomic>
#include <mutex>
#include <string>
#include <share.h>   // _fsopen / _SH_DENYNO（共享模式常驻句柄）

namespace {

/* 顺序必须与 ORBMDK_LogLevel 一一对应（按 level 直接索引） */
const char* const kLevelName[]       = { "DEBUG", "INFO", "TESTSPEED", "VERBOSE",
                                         "REV1", "REV2", "REV3", "WARN", "ERROR", "NONE" };
const char* const kEnvLevel          = "ORBMDK_LOG_LEVEL";
const char* const kEnvFile           = "ORBMDK_LOG_FILE";
const char* const kLevelFileName     = "ORBMDK_LOG_LEVEL";
const char* const kDefaultFileName   = "ORBMDK_RDDI.log";

std::mutex  g_lock;                       // 保护级别/路径/格式化/文件写入
int         g_level        = ORBMDK_LOG_ERROR;
bool        g_levelForced  = false;       // SetLevel 之后不再读文件/环境
ULONGLONG   g_levelStamp   = 0;

/*
 * 无锁级别快照（给 ORBMDK_LogIsEnabled / ORBMDK_LogMeterEnabled 用）：
 * 由 _publishLevel() 在每次 _resolveLevel() 之后刷新。INT_MAX = "尚未解析"，
 * 这时闸门不知道真实阈值，要抢一次锁做首次解析（只发生一次）。
 * 之所以要这个快照：热路径上的日志宏若先取锁再判断，锁竞争本身就是开销，
 * 而且被过滤的日志还会白白做参数求值。
 */
std::atomic<int>  g_levelFast{ INT_MAX };
std::atomic<bool> g_levelResolved{ false };
std::string g_filePath;
bool        g_filePathResolved = false;
FILE*       g_file         = nullptr;  // 常驻句柄：开一次、追加多行（不再逐行开/关）
std::string g_fileOpenFor;             // g_file 当前对应的路径（路径变了要重开）
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
 * 解析阈值（**调用方必须持有 g_lock**）：%TEMP%\ORBMDK_LOG_LEVEL（热更新）→
 * 环境变量 → 默认 ERROR。每条日志都查文件太浪费，1 秒最多查一次，因此改动最多
 * 延迟 1 秒生效。
 */
int _resolveLevelLocked()
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

/** 刷新无锁快照（阈值/是否已解析）。只在持有 g_lock 时调用。 */
void _publishLevel(int level)
{
    g_levelFast.store(level, std::memory_order_relaxed);
    g_levelResolved.store(true, std::memory_order_relaxed);
}

/** 解析阈值并刷新快照（调用方必须持有 g_lock）。 */
int _resolveLevel()
{
    const int level = _resolveLevelLocked();
    _publishLevel(level);
    return level;
}

/**
 * 首次解析：闸门发现快照还是 INT_MAX（从未解析过）时调用。
 * 抢不到锁就直接返回 —— 另一个线程正在解析，下一次闸门再读快照即可。
 */
void _ensureLevelResolved()
{
    std::lock_guard<std::mutex> guard(g_lock);
    _resolveLevel();
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
 * 追加一行到**常驻句柄**。
 *
 * 现在改成**共享模式常驻句柄**（_fsopen + _SH_DENYNO）：外部工具在宿主运行
 * 期间照样能打开读取，而进程内只开一次。每行仍 fflush，保证外部读到的是
 * 最新内容、异常退出也不丢已经写进去的内容。
 */
void _closeFile()
{
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    g_fileOpenFor.clear();
}

void _appendLine(const char* line)
{
    const std::string& path = _filePath();
    if (path.empty()) {
        return;
    }

    if (!g_file || g_fileOpenFor != path) {
        _closeFile();
        g_file = _fsopen(path.c_str(), "a", _SH_DENYNO);
        if (!g_file) {
            return;
        }
        g_fileOpenFor = path;
    }

    fputs(line, g_file);
    fputc('\n', g_file);
    fflush(g_file);      // 立即可见：外部工具随读随到
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

int ORBMDK_LogIsEnabled(int level)
{
    if (level < ORBMDK_LOG_DEBUG || level > ORBMDK_LOG_ERROR) {
        return 0;
    }
    if (g_levelFast.load(std::memory_order_relaxed) <= level) {
        return 1;
    }
    if (!g_levelResolved.load(std::memory_order_relaxed)) {
        _ensureLevelResolved();            // 只发生一次；此后纯 atomic load
    }
    return g_levelFast.load(std::memory_order_relaxed) <= level ? 1 : 0;
}

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

    // 只送宿主回调，且在**锁外**调用（回调很可能再调回本模块，锁内会自锁）。
    //
    // 这里不再 printf / OutputDebugStringA：诊断开启时日志是高频的，每行都往
    // 控制台和 DebugView 送一遍，"把文本送出去"本身就能吃掉可观的时间（UV4 里
    // 还会连带触发宿主 UI 刷新）。要看文本就读日志文件 / 用 DebugView 挂别的进程。
    if (sink) {
        sink(sinkContext, line.c_str(), level);
    }
}

/**
 * 只落日志文件：不送 stdout / DebugView / 宿主回调（见头文件说明）。
 */
void ORBMDK_LogWriteFileOnly(int level, const char* module, const char* fmt, ...)
{
    if (level < ORBMDK_LOG_DEBUG || level > ORBMDK_LOG_ERROR) {
        return;
    }

    std::lock_guard<std::mutex> guard(g_lock);

    if (level < _resolveLevel()) {
        return;                            // ★ 过滤在格式化之前，零开销
    }

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    _format(buf, sizeof(buf), level, module, fmt, args);
    va_end(args);

    _appendLine(buf);
}

void ORBMDK_LogTrace(const char* module, const char* fmt, ...)
{
    std::lock_guard<std::mutex> guard(g_lock);

    // 命令级日志：**同样受级别阈值约束** —— 低于阈值直接丢弃（格式化之前返回，零开销）。
    // 级别显示为 INFO，与历史格式（[ORBMDK][INFO][BULK] ...）兼容，因此只有阈值设为
    // INFO/DEBUG 时才输出；默认 ERROR 下完全不落盘（此前无条件落盘是日志膨胀的主因）。
    if (ORBMDK_LOG_INFO < _resolveLevel()) {
        return;
    }

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
    _publishLevel(level);                  // 立即对闸门生效（不再等 1 秒节流）
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
    _closeFile();                          // 路径变了：下次写入按新路径重开
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

    // 常驻句柄必须在这里关闭：宿主会随时卸载本 DLL，句柄要跟着走。
    _closeFile();
}

/* ===========================================================================
 * 速率计量（TESTSPEED）—— 口径与重建背景见 COMPAT_ANALYSIS.md §9.4
 *
 * 目前服务"烧录/校验"这条数据通道（DAP_RegWriteRepeat / DAP_RegReadRepeat）。
 * 落四类行：meter config / WRITE|READ speed / meter segment / meter usb。
 * 阈值 > TESTSPEED 时所有入口第一行返回（零开销）。
 * =========================================================================== */

namespace {

const char* const kMeterModule = "RDDI";

/* 窗口落行条件：累计 ≥1024 字（4096 B）或 ≥200 ms（README「烧录速率统计」） */
const unsigned           kMeterFlushWords = 1024u;
const unsigned           kMeterFlushBytes = kMeterFlushWords * 4u;
const unsigned long long kMeterFlushUs    = 200000ull;
/* 两次记账空闲超过此值 = 上一段连续传输结束（在下一笔到来时补打段总结） */
const unsigned long long kMeterIdleUs     = 500000ull;

/** µs 级单调时钟。QPC 分辨率 ~100 ns，远好于原 GetTickCount64 的 1 ms。 */
unsigned long long _qpcUs()
{
    static const unsigned long long freq = []() -> unsigned long long {
        LARGE_INTEGER f;
        if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) {
            return 1ull;
        }
        return (unsigned long long)f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (unsigned long long)c.QuadPart * 1000000ull / freq;
}

/** 速率：**1 kB = 1024 B**（与 §9.4 的历史数据口径一致，改了不可比）。 */
double _kbps(unsigned bytes, unsigned long long us)
{
    if (us == 0) {
        return 0.0;
    }
    return (double)bytes * 1000000.0 / (1024.0 * (double)us);
}

const char* _speedName(int mbps)
{
    switch (mbps) {
    case 480: return "High";
    case 12:  return "Full";
    case 2:   return "Low";
    default:  return "?";
    }
}

struct MeterState {
    /* 配置（变化时重打 meter config） */
    std::string transport;
    int         linkMbps      = 0;
    int         cmdPktBytes   = 0;
    int         wordsPerTrip  = 0;
    int         blockTransfer = -1;      // -1 = 尚未落定，保证首次必打

    /* 当前速度窗口 */
    bool               windowActive = false;
    unsigned long long windowStartUs = 0;   // 窗口首笔 TripBegin
    unsigned long long windowLastUs  = 0;   // 窗口末笔记账时刻
    unsigned long long windowDapUs   = 0;
    unsigned           windowWriteBytes = 0;
    unsigned           windowReadBytes  = 0;
    unsigned           windowTrips      = 0;

    /* 累计（跨段，从进程启动起从不清零） */
    unsigned long long totalWriteDapUs = 0;
    unsigned long long totalReadDapUs  = 0;
    unsigned           totalWriteBytes = 0;
    unsigned           totalReadBytes  = 0;
    unsigned           totalTrips      = 0;

    /* 当前段 */
    bool               segActive  = false;
    unsigned long long segStartUs = 0;
    unsigned long long segEndUs   = 0;
    unsigned long long segDapUs   = 0;
    unsigned           segWriteBytes = 0;
    unsigned           segReadBytes  = 0;
    unsigned           segTrips      = 0;

    /* meter usb：只统计 TripBegin..Account 之间（见 §9.4 首轮缺陷） */
    bool               tripOpen = false;
    unsigned long long lastActivityUs = 0;
    unsigned           usbOutCmds = 0;
    unsigned           usbInCmds  = 0;
    unsigned long long usbOutUs   = 0;
    unsigned long long usbInUs    = 0;
};

MeterState g_meter;

/** 加统一前缀落一行（调用方必须持有 g_lock；TESTSPEED 级只落文件）。 */
void _meterEmitLocked(const char* body)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1024];
    snprintf(line, sizeof(line),
             "[ORBMDK][%02u:%02u:%02u.%03u][%s][%s][%lu:%lu] %s",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             kLevelName[ORBMDK_LOG_TESTSPEED], kMeterModule,
             (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetCurrentThreadId(), body);
    _appendLine(line);
}

void _meterResetWindowLocked()
{
    g_meter.windowActive     = false;
    g_meter.windowStartUs    = 0;
    g_meter.windowLastUs     = 0;
    g_meter.windowDapUs      = 0;
    g_meter.windowWriteBytes = 0;
    g_meter.windowReadBytes  = 0;
    g_meter.windowTrips      = 0;
}

void _meterResetSegmentLocked()
{
    g_meter.segActive     = false;
    g_meter.segStartUs    = 0;
    g_meter.segEndUs      = 0;
    g_meter.segDapUs      = 0;
    g_meter.segWriteBytes = 0;
    g_meter.segReadBytes  = 0;
    g_meter.segTrips      = 0;
    g_meter.usbOutCmds    = 0;
    g_meter.usbInCmds     = 0;
    g_meter.usbOutUs      = 0;
    g_meter.usbInUs       = 0;
    g_meter.tripOpen      = false;
    g_meter.lastActivityUs = 0;
    _meterResetWindowLocked();
}

/** 落 `WRITE|READ speed` 行并按需重置窗口（调用方必须持有 g_lock）。 */
void _meterFlushWindowLocked()
{
    if (g_meter.windowWriteBytes == 0 && g_meter.windowReadBytes == 0) {
        _meterResetWindowLocked();
        return;
    }

    const bool     isWrite = g_meter.windowWriteBytes >= g_meter.windowReadBytes;
    const unsigned bytes   = isWrite ? g_meter.windowWriteBytes : g_meter.windowReadBytes;

    const unsigned long long wallUs =
        (g_meter.windowLastUs > g_meter.windowStartUs)
            ? (g_meter.windowLastUs - g_meter.windowStartUs) : 0;

    const unsigned long long totalDapUs = isWrite ? g_meter.totalWriteDapUs : g_meter.totalReadDapUs;
    const unsigned           totalBytes = isWrite ? g_meter.totalWriteBytes : g_meter.totalReadBytes;

    const double usPerTrip = (g_meter.windowTrips > 0)
        ? (double)g_meter.windowDapUs / (double)g_meter.windowTrips : 0.0;
    const double dapPct = (wallUs > 0)
        ? (100.0 * (double)g_meter.windowDapUs / (double)wallUs) : 0.0;

    char body[640];
    snprintf(body, sizeof(body),
             "%s speed: %u B in %.1f ms -> %.1f kB/s | %u round trips, %.1f us/trip "
             "| window wall %.1f ms, dap %.1f%% | total %u B in %.1f ms "
             "(avg %.1f kB/s, %u trips)",
             isWrite ? "WRITE" : "READ", bytes,
             (double)g_meter.windowDapUs / 1000.0, _kbps(bytes, g_meter.windowDapUs),
             g_meter.windowTrips, usPerTrip,
             (double)wallUs / 1000.0, dapPct,
             totalBytes, (double)totalDapUs / 1000.0,
             _kbps(totalBytes, totalDapUs), g_meter.totalTrips);
    _meterEmitLocked(body);

    _meterResetWindowLocked();
}

/** 补打 `meter segment` + `meter usb` 并清零段计数（调用方必须持有 g_lock）。 */
void _meterSettleSegmentLocked()
{
    _meterFlushWindowLocked();             // 段结束前先把窗口余量落掉，不丢数据

    const unsigned long long wallUs =
        (g_meter.segEndUs > g_meter.segStartUs) ? (g_meter.segEndUs - g_meter.segStartUs) : 0;
    const unsigned           cmds =
        (g_meter.usbInCmds > g_meter.usbOutCmds) ? g_meter.usbInCmds : g_meter.usbOutCmds;

    if (wallUs == 0 && g_meter.segTrips == 0 && cmds == 0) {
        _meterResetSegmentLocked();
        return;
    }

    const double wallMs = (double)wallUs / 1000.0;
    const double dapMs  = (double)g_meter.segDapUs / 1000.0;
    const double dapPct = (wallUs > 0) ? (100.0 * (double)g_meter.segDapUs / (double)wallUs) : 0.0;

    char body[512];
    snprintf(body, sizeof(body),
             "meter segment: wall %.1f ms | dap %.1f ms (%.1f%%) | other %.1f ms (%.1f%%) "
             "| write %u B (%.1f kB/s) + read %u B | %u round trips",
             wallMs, dapMs, dapPct,
             wallMs - dapMs, wallUs > 0 ? (100.0 - dapPct) : 0.0,
             g_meter.segWriteBytes, _kbps(g_meter.segWriteBytes, g_meter.segDapUs),
             g_meter.segReadBytes, g_meter.segTrips);
    _meterEmitLocked(body);

    if (cmds > 0) {
        const unsigned long long rtUs = g_meter.usbOutUs + g_meter.usbInUs;
        snprintf(body, sizeof(body),
                 "meter usb: %u cmds, out %.1f ms (%.1f us/cmd), in %.1f ms (%.1f us/cmd), "
                 "round trip %.1f us/cmd",
                 cmds,
                 (double)g_meter.usbOutUs / 1000.0, (double)g_meter.usbOutUs / (double)cmds,
                 (double)g_meter.usbInUs / 1000.0, (double)g_meter.usbInUs / (double)cmds,
                 (double)rtUs / (double)cmds);
        _meterEmitLocked(body);
    }

    _meterResetSegmentLocked();
}

/** 落 `meter config` 行（调用方必须持有 g_lock）。 */
void _meterEmitConfigLocked()
{
    /* 总线理论上限：mbps → MB/s → kB/s（1 kB = 1024 B） */
    const double ceilingKbps = (double)g_meter.linkMbps * 1000000.0 / 8.0 / 1024.0;

    char body[448];
    snprintf(body, sizeof(body),
             "meter config: transport=%s speed=%s(%dMbps) cmdPkt=%d B -> %d words/round trip, "
             "blockTransfer=%s | bus ceiling ~%.0f kB/s",
             g_meter.transport.c_str(), _speedName(g_meter.linkMbps), g_meter.linkMbps,
             g_meter.cmdPktBytes, g_meter.wordsPerTrip,
             g_meter.blockTransfer ? "on" : "off", ceilingKbps);
    _meterEmitLocked(body);
}

/**
 * 每次"有活动"（TripBegin / Account）时调用：空闲 >500 ms 则把上一段结算掉，
 * 否则刷新段活动的最后时刻（调用方必须持有 g_lock）。
 */
void _meterActivityLocked(unsigned long long now)
{
    if (g_meter.segActive && g_meter.lastActivityUs != 0 &&
        now > g_meter.lastActivityUs && (now - g_meter.lastActivityUs) > kMeterIdleUs) {
        _meterSettleSegmentLocked();
    }
    if (!g_meter.segActive) {
        g_meter.segActive  = true;
        g_meter.segStartUs = now;
    }
    g_meter.lastActivityUs = now;
    g_meter.segEndUs       = now;
}

} // namespace

int ORBMDK_LogMeterEnabled(void)
{
    if (g_levelFast.load(std::memory_order_relaxed) <= ORBMDK_LOG_TESTSPEED) {
        return 1;
    }
    if (!g_levelResolved.load(std::memory_order_relaxed)) {
        _ensureLevelResolved();            // 只发生一次；此后纯 atomic load
    }
    return g_levelFast.load(std::memory_order_relaxed) <= ORBMDK_LOG_TESTSPEED ? 1 : 0;
}

unsigned long long ORBMDK_LogMeterNowUs(void)
{
    return _qpcUs();
}

void ORBMDK_LogMeterTripBegin(void)
{
    if (!ORBMDK_LogMeterEnabled()) {
        return;
    }

    std::lock_guard<std::mutex> guard(g_lock);
    const unsigned long long now = _qpcUs();

    _meterActivityLocked(now);
    if (!g_meter.windowActive) {
        g_meter.windowActive  = true;
        g_meter.windowStartUs = now;       // 墙钟从窗口首笔起算（此前从记账时刻起算会 dap>wall）
    }
    g_meter.tripOpen = true;
}

void ORBMDK_LogMeterSetTransport(const char* transport, int linkMbps,
                                 int cmdPktBytes, int wordsPerTrip,
                                 int blockTransferOn)
{
    if (!ORBMDK_LogMeterEnabled()) {
        return;
    }

    std::lock_guard<std::mutex> guard(g_lock);

    const char* name = (transport && *transport) ? transport : "?";
    const int   bt   = blockTransferOn ? 1 : 0;

    if (g_meter.transport == name && g_meter.linkMbps == linkMbps &&
        g_meter.cmdPktBytes == cmdPktBytes && g_meter.wordsPerTrip == wordsPerTrip &&
        g_meter.blockTransfer == bt) {
        return;                            // 什么都没变，不重打
    }

    g_meter.transport     = name;
    g_meter.linkMbps      = linkMbps;
    g_meter.cmdPktBytes   = cmdPktBytes;
    g_meter.wordsPerTrip  = wordsPerTrip;
    g_meter.blockTransfer = bt;
    _meterEmitConfigLocked();
}

void ORBMDK_LogMeterAccount(int isWrite, unsigned bytes, unsigned trips,
                            unsigned long long dapUs)
{
    if (!ORBMDK_LogMeterEnabled()) {
        return;
    }

    std::lock_guard<std::mutex> guard(g_lock);
    const unsigned long long now = _qpcUs();

    _meterActivityLocked(now);
    if (!g_meter.windowActive) {
        g_meter.windowActive  = true;
        g_meter.windowStartUs = now;
    }
    g_meter.tripOpen = false;

    if (isWrite) {
        g_meter.windowWriteBytes += bytes;
        g_meter.segWriteBytes    += bytes;
        g_meter.totalWriteBytes  += bytes;
        g_meter.totalWriteDapUs  += dapUs;
    } else {
        g_meter.windowReadBytes  += bytes;
        g_meter.segReadBytes     += bytes;
        g_meter.totalReadBytes   += bytes;
        g_meter.totalReadDapUs   += dapUs;
    }
    g_meter.windowDapUs += dapUs;
    g_meter.segDapUs    += dapUs;
    g_meter.windowTrips += trips;
    g_meter.segTrips    += trips;
    g_meter.totalTrips  += trips;
    g_meter.windowLastUs = now;

    if (g_meter.windowWriteBytes + g_meter.windowReadBytes >= kMeterFlushBytes ||
        (now - g_meter.windowStartUs) >= kMeterFlushUs) {
        _meterFlushWindowLocked();
    }
}

void ORBMDK_LogMeterUsb(int dir, unsigned long long us)
{
    if (!ORBMDK_LogMeterEnabled()) {
        return;
    }

    std::lock_guard<std::mutex> guard(g_lock);

    /* 只认 TripBegin..Account 之间的往返：枚举/标定等**不受计量**的命令同样走
     * _bulkWrite/_bulkRead，若不设这道闸，段内 19 次 DAP 往返会被记成 7107 cmds。 */
    if (!g_meter.tripOpen) {
        return;
    }

    if (dir == 0) {                        // OUT：把命令包搬上线
        g_meter.usbOutCmds++;
        g_meter.usbOutUs += us;
    } else {                               // IN：等设备应答
        g_meter.usbInCmds++;
        g_meter.usbInUs += us;
    }
}
