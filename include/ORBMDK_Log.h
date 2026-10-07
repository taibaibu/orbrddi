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

#ifdef __cplusplus
}
#endif

/* ---------------------------------------------------------------------------
 * 模块内便捷宏
 *
 * 用法：各 .cpp 顶部（在 #include "ORBMDK_Log.h" 之后）定义
 *     #define ORBMDK_LOG_MODULE "HID"
 * 之后直接 ORBMDK_LOG_INFO("...") 即可。
 * 宏体里的 ORBMDK_LOG_MODULE 是**展开时**替换的，所以定义顺序不敏感。
 * --------------------------------------------------------------------------- */
#define ORBMDK_LOG_DEBUG(...)     ORBMDK_LogWrite(ORBMDK_LOG_DEBUG,     ORBMDK_LOG_MODULE, __VA_ARGS__)
#define ORBMDK_LOG_INFO(...)      ORBMDK_LogWrite(ORBMDK_LOG_INFO,      ORBMDK_LOG_MODULE, __VA_ARGS__)
#define ORBMDK_LOG_TESTSPEED(...) ORBMDK_LogWriteFileOnly(ORBMDK_LOG_TESTSPEED, ORBMDK_LOG_MODULE, __VA_ARGS__)
#define ORBMDK_LOG_VERBOSE(...)   ORBMDK_LogWrite(ORBMDK_LOG_VERBOSE,   ORBMDK_LOG_MODULE, __VA_ARGS__)
#define ORBMDK_LOG_REV1(...)      ORBMDK_LogWrite(ORBMDK_LOG_REV1,      ORBMDK_LOG_MODULE, __VA_ARGS__)
#define ORBMDK_LOG_REV2(...)      ORBMDK_LogWrite(ORBMDK_LOG_REV2,      ORBMDK_LOG_MODULE, __VA_ARGS__)
#define ORBMDK_LOG_REV3(...)      ORBMDK_LogWrite(ORBMDK_LOG_REV3,      ORBMDK_LOG_MODULE, __VA_ARGS__)
#define ORBMDK_LOG_WARN(...)      ORBMDK_LogWrite(ORBMDK_LOG_WARN,      ORBMDK_LOG_MODULE, __VA_ARGS__)
#define ORBMDK_LOG_ERROR(...)     ORBMDK_LogWrite(ORBMDK_LOG_ERROR,     ORBMDK_LOG_MODULE, __VA_ARGS__)
#define ORBMDK_LOG_TRACE(...)     ORBMDK_LogTrace(ORBMDK_LOG_MODULE, __VA_ARGS__)

/* 跨模块打日志时显式给出模块名 */
#define ORBMDK_LOG_AT_DEBUG(mod, ...)     ORBMDK_LogWrite(ORBMDK_LOG_DEBUG,     mod, __VA_ARGS__)
#define ORBMDK_LOG_AT_INFO(mod, ...)      ORBMDK_LogWrite(ORBMDK_LOG_INFO,      mod, __VA_ARGS__)
#define ORBMDK_LOG_AT_TESTSPEED(mod, ...) ORBMDK_LogWriteFileOnly(ORBMDK_LOG_TESTSPEED, mod, __VA_ARGS__)
#define ORBMDK_LOG_AT_VERBOSE(mod, ...)   ORBMDK_LogWrite(ORBMDK_LOG_VERBOSE,   mod, __VA_ARGS__)
#define ORBMDK_LOG_AT_REV1(mod, ...)      ORBMDK_LogWrite(ORBMDK_LOG_REV1,      mod, __VA_ARGS__)
#define ORBMDK_LOG_AT_REV2(mod, ...)      ORBMDK_LogWrite(ORBMDK_LOG_REV2,      mod, __VA_ARGS__)
#define ORBMDK_LOG_AT_REV3(mod, ...)      ORBMDK_LogWrite(ORBMDK_LOG_REV3,      mod, __VA_ARGS__)
#define ORBMDK_LOG_AT_WARN(mod, ...)      ORBMDK_LogWrite(ORBMDK_LOG_WARN,      mod, __VA_ARGS__)
#define ORBMDK_LOG_AT_ERROR(mod, ...)     ORBMDK_LogWrite(ORBMDK_LOG_ERROR,     mod, __VA_ARGS__)

#endif /* ORBMDK_LOG_H */
