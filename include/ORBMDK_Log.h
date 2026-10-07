/**
 * @file ORBMDK_Log.h
 * @brief 统一日志接口（COMPAT_ANALYSIS §8.3 落地）
 *
 * 设计要点（详见 COMPAT_ANALYSIS.md §8.3）：
 *   - **单一实现**：RDDI / HID / BULK / ... 全部调用 ORBMDK_LogWrite，不再各写一套；
 *   - **头文件暴露**：宏在这里，跨编译单元可用（旧实现把宏写在 .cpp 里，属死代码）；
 *   - **多 Sink**：日志文件 / 宿主回调（RDDI_SetLogCallback 经此接通，Keil 的
 *     日志窗口能收到消息）。**不再**往 stdout / OutputDebugStringA 送 —— 诊断日志
 *     是高频的，"把文本送出去"本身就能吃掉可观的时间（见 ORBMDK_LogWrite 注释）；
 *   - **日志文件句柄常驻、共享打开**（_fsopen + _SH_DENYNO）：外部工具在宿主运行
 *     期间照样能读，但不再逐行 open/close（那才是开销大头）；
 *   - **单层过滤、格式化之前判断**：被过滤时不构造字符串（零开销）；
 *   - **线程安全**：内部单锁；宿主回调在**锁外**调用，避免回调再打日志时自锁。
 *
 * 排障开关（对所有模块一致，免重启、免命令行）：
 *   %TEMP%\ORBMDK_LOG_LEVEL   0=DEBUG 1=INFO 2=TESTSPEED 3=VERBOSE 4=REV1
 *                             5=REV2 6=REV3 7=WARN 8=ERROR（最多 1 秒生效）
 *   环境变量 ORBMDK_LOG_LEVEL （进程启动读一次；上面的文件优先）
 *   环境变量 ORBMDK_LOG_FILE  （覆盖日志文件路径，默认 %TEMP%\ORBMDK_RDDI.log）
 *   删除文件即恢复默认（ERROR）。
 */
#ifndef ORBMDK_LOG_H
#define ORBMDK_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/** 日志级别：数值越大越严重。ORBMDK_LOG_NONE 只用于 SetLevel（全部关闭）。 */
enum ORBMDK_LogLevel {
    ORBMDK_LOG_DEBUG = 0,
    ORBMDK_LOG_INFO  = 1,
    ORBMDK_LOG_TESTSPEED  = 2,
    ORBMDK_LOG_VERBOSE  = 3,
    ORBMDK_LOG_REV1  = 4,
    ORBMDK_LOG_REV2  = 5,
    ORBMDK_LOG_REV3  = 6,
    ORBMDK_LOG_WARN  = 7,
    ORBMDK_LOG_ERROR = 8,
    ORBMDK_LOG_NONE  = 9,
};

/** 宿主日志回调：level 为上面的内部级别（RDDI 级别的映射由桥接层负责）。 */
typedef void (*ORBMDK_LogSinkFn)(void* context, const char* msg, int level);

/**
 * @brief **无锁闸门**：该级别的日志当前是否会输出（`level >= 当前阈值`）。
 *
 * 给 ORBMDK_LOG_* 宏用：阈值不满足时**连调用都不发生**（参数不求值、不取锁、
 * 不建 va_list）。只读一个 `std::atomic<int>` 快照，热路径上就是一条普通 load；
 * 快照由 `_resolveLevel()` 在解析处刷新（见 ORBMDK_Log.cpp），最多延迟 1 秒。
 *
 * ⚠ 这是**优化用的近似判断**，不是语义判断：真正的过滤仍在 ORBMDK_LogWrite 里
 * 再做一次（闸门放行 ≠ 一定输出）。反过来说，闸门说"不输出"时确实不会输出。
 */
int ORBMDK_LogIsEnabled(int level);

/** 唯一实现：按运行期阈值过滤后写到所有已启用 Sink（线程安全）。 */
void ORBMDK_LogWrite(int level, const char* module, const char* fmt, ...);

/**
 * @brief **只落日志文件**：不送 stdout / DebugView / 宿主回调。
 *
 * 给"高频性能诊断类"日志用（TESTSPEED 的速率/结算行就是这类）。它每秒可能
 * 落几十条，若每条都经 printf + OutputDebugString + 宿主 UI 回调输出，光是
 * "把文本送出去"就能吃掉可观的时间，反过来污染被测量的对象。
 */
void ORBMDK_LogWriteFileOnly(int level, const char* module, const char* fmt, ...);

/**
 * @brief 传输层"命令级"日志：按 INFO 级参与级别过滤，只落日志文件
 *
 * 单独一个入口是为了**只落文件**（不送 stdout/DebugView/宿主回调，避免噪声）；
 * 级别阈值同样生效：阈值 > INFO（即默认 ERROR / WARN）时整条命令日志被丢弃。
 * 需要看 V1/V2 每条 DAP 命令往返时，把 `%TEMP%\ORBMDK_LOG_LEVEL` 设为
 * 1(INFO) 或 0(DEBUG) 即可（见文件头）。
 */
void ORBMDK_LogTrace(const char* module, const char* fmt, ...);

void ORBMDK_LogSetLevel(int level);              /**< 强制阈值（之后不再读文件/环境） */
int  ORBMDK_LogGetLevel(void);                   /**< 当前生效阈值 */
void ORBMDK_LogSetFile(const char* path);        /**< 空 = 恢复默认路径 */
void ORBMDK_LogSetCallback(ORBMDK_LogSinkFn fn, void* context);
void ORBMDK_LogShutdown(void);                   /**< DllMain(DLL_PROCESS_DETACH) 调用 */

/* ---------------------------------------------------------------------------
 * 速率计量（TESTSPEED 级，重建说明见 COMPAT_ANALYSIS.md §9.4）
 *
 * 阈值 == TESTSPEED(2) 及以下时计量开启；否则所有入口**第一行就返回**，
 * 调用方既不取时钟也不记账（零开销）。落行时机 / 口径见 §9.4 与 README
 * 「烧录速率统计」。
 * --------------------------------------------------------------------------- */

/** 计量是否开启（无锁：读级别快照，阈值 ≤ TESTSPEED 时为真）。 */
int ORBMDK_LogMeterEnabled(void);

/** µs 级单调时钟（`QueryPerformanceCounter`）；只在计量开启时调用。 */
unsigned long long ORBMDK_LogMeterNowUs(void);

/**
 * @brief 一笔 DAP 往返**开始**（紧挨着 `ORBMDK_LogMeterNowUs()` 之前调用）。
 *
 * 墙钟从这一笔起算（此前从"记账时刻"起算会导致 `dap > wall`）；
 * 同时 `meter usb` 的采样只认 `TripBegin..Account` 之间，跨过不受计量的
 * DAP 命令（它们同样走 `_bulkWrite/_bulkRead`）不会被误记。
 */
void ORBMDK_LogMeterTripBegin(void);

/** 传输层 / 速度 / 出包字节 / 每次往返字数 / 块传输是否生效；任一变化重打 `meter config`。 */
void ORBMDK_LogMeterSetTransport(const char* transport, int linkMbps,
                                 int cmdPktBytes, int wordsPerTrip,
                                 int blockTransferOn);

/**
 * @brief 一笔成功的 DAP 往返记账。
 * @param isWrite    1=写（WRITE speed 行），0=读（READ speed 行）
 * @param bytes      本次往返搬运的字节数
 * @param trips      USB 往返次数（块传输 = 1；逐字回退 = chunk）
 * @param dapUs      该笔的 DAP 段耗时（µs）；**失败的那次不要调用**
 */
void ORBMDK_LogMeterAccount(int isWrite, unsigned bytes, unsigned trips,
                            unsigned long long dapUs);

/** 传输层 OUT(dir=0) / IN(dir=1) 的单次上报耗时；只统计 TripBegin..Account 之间。 */
void ORBMDK_LogMeterUsb(int dir, unsigned long long us);

#ifdef __cplusplus
}

namespace orbmdk {

/**
 * @brief `meter usb` 计时的 RAII 帮手：构造取时、析构记账。
 *
 * 计量关闭时构造/析构都是空操作（构造里那次 ORBMDK_LogMeterEnabled() 就是
 * 一次 atomic load）。适用"作用域内正好一次 OUT / IN"的场合；像
 * `_hidDapCommand` 那样一次作用域里要分开计 OUT 与 IN 的，仍用手工取时。
 */
class MeterUsbTimer {
public:
    explicit MeterUsbTimer(int dir) noexcept
        : dir_(dir), running_(false), t0_(0) {
        if (ORBMDK_LogMeterEnabled()) {
            running_ = true;
            t0_ = ORBMDK_LogMeterNowUs();
        }
    }
    ~MeterUsbTimer() {
        if (running_) {
            ORBMDK_LogMeterUsb(dir_, ORBMDK_LogMeterNowUs() - t0_);
        }
    }
    MeterUsbTimer(const MeterUsbTimer&) = delete;
    MeterUsbTimer& operator=(const MeterUsbTimer&) = delete;

private:
    int                dir_;
    bool               running_;
    unsigned long long t0_;
};

} // namespace orbmdk
#endif

/* ---------------------------------------------------------------------------
 * 模块内便捷宏
 *
 * 用法：各 .cpp 顶部（在 #include "ORBMDK_Log.h" 之后）定义
 *     #define ORBMDK_LOG_MODULE "HID"
 * 之后直接 ORBMDK_LOG_INFO("...") 即可。
 * 宏体里的 ORBMDK_LOG_MODULE 是**展开时**替换的，所以定义顺序不敏感。
 *
 * 每个宏先过 `ORBMDK_LogIsEnabled()` 闸门（无锁 atomic 读）：阈值不满足时
 * **连函数调用都不发生** —— 参数不求值、不取锁、不建 va_list。这是热路径
 * （寄存器读写、每次 USB 往返都可能打日志）上的关键优化。
 * --------------------------------------------------------------------------- */
#define ORBMDK_LOG_DEBUG(...)     do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_DEBUG))     ORBMDK_LogWrite(ORBMDK_LOG_DEBUG,     ORBMDK_LOG_MODULE, __VA_ARGS__); } while (0)
#define ORBMDK_LOG_INFO(...)      do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_INFO))      ORBMDK_LogWrite(ORBMDK_LOG_INFO,      ORBMDK_LOG_MODULE, __VA_ARGS__); } while (0)
#define ORBMDK_LOG_TESTSPEED(...) do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_TESTSPEED)) ORBMDK_LogWriteFileOnly(ORBMDK_LOG_TESTSPEED, ORBMDK_LOG_MODULE, __VA_ARGS__); } while (0)
#define ORBMDK_LOG_VERBOSE(...)   do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_VERBOSE))   ORBMDK_LogWrite(ORBMDK_LOG_VERBOSE,   ORBMDK_LOG_MODULE, __VA_ARGS__); } while (0)
#define ORBMDK_LOG_REV1(...)      do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_REV1))      ORBMDK_LogWrite(ORBMDK_LOG_REV1,      ORBMDK_LOG_MODULE, __VA_ARGS__); } while (0)
#define ORBMDK_LOG_REV2(...)      do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_REV2))      ORBMDK_LogWrite(ORBMDK_LOG_REV2,      ORBMDK_LOG_MODULE, __VA_ARGS__); } while (0)
#define ORBMDK_LOG_REV3(...)      do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_REV3))      ORBMDK_LogWrite(ORBMDK_LOG_REV3,      ORBMDK_LOG_MODULE, __VA_ARGS__); } while (0)
#define ORBMDK_LOG_WARN(...)      do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_WARN))      ORBMDK_LogWrite(ORBMDK_LOG_WARN,      ORBMDK_LOG_MODULE, __VA_ARGS__); } while (0)
#define ORBMDK_LOG_ERROR(...)     do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_ERROR))     ORBMDK_LogWrite(ORBMDK_LOG_ERROR,     ORBMDK_LOG_MODULE, __VA_ARGS__); } while (0)
/* 传输/命令级日志：级别显示为 INFO，只落文件（不送宿主窗口），同样过闸门 */
#define ORBMDK_LOG_TRACE(...)     do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_INFO))      ORBMDK_LogTrace(ORBMDK_LOG_MODULE, __VA_ARGS__); } while (0)

/* 跨模块打日志时显式给出模块名 */
#define ORBMDK_LOG_AT_DEBUG(mod, ...)     do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_DEBUG))     ORBMDK_LogWrite(ORBMDK_LOG_DEBUG,     mod, __VA_ARGS__); } while (0)
#define ORBMDK_LOG_AT_INFO(mod, ...)      do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_INFO))      ORBMDK_LogWrite(ORBMDK_LOG_INFO,      mod, __VA_ARGS__); } while (0)
#define ORBMDK_LOG_AT_TESTSPEED(mod, ...) do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_TESTSPEED)) ORBMDK_LogWriteFileOnly(ORBMDK_LOG_TESTSPEED, mod, __VA_ARGS__); } while (0)
#define ORBMDK_LOG_AT_VERBOSE(mod, ...)   do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_VERBOSE))   ORBMDK_LogWrite(ORBMDK_LOG_VERBOSE,   mod, __VA_ARGS__); } while (0)
#define ORBMDK_LOG_AT_REV1(mod, ...)      do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_REV1))      ORBMDK_LogWrite(ORBMDK_LOG_REV1,      mod, __VA_ARGS__); } while (0)
#define ORBMDK_LOG_AT_REV2(mod, ...)      do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_REV2))      ORBMDK_LogWrite(ORBMDK_LOG_REV2,      mod, __VA_ARGS__); } while (0)
#define ORBMDK_LOG_AT_REV3(mod, ...)      do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_REV3))      ORBMDK_LogWrite(ORBMDK_LOG_REV3,      mod, __VA_ARGS__); } while (0)
#define ORBMDK_LOG_AT_WARN(mod, ...)      do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_WARN))      ORBMDK_LogWrite(ORBMDK_LOG_WARN,      mod, __VA_ARGS__); } while (0)
#define ORBMDK_LOG_AT_ERROR(mod, ...)     do { if (ORBMDK_LogIsEnabled(ORBMDK_LOG_ERROR))     ORBMDK_LogWrite(ORBMDK_LOG_ERROR,     mod, __VA_ARGS__); } while (0)

#endif /* ORBMDK_LOG_H */
