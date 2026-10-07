/**
 * @file ORBMDK_Log.h
 * @brief 统一日志接口（COMPAT_ANALYSIS §8.3 落地）
 *
 * 设计要点（详见 COMPAT_ANALYSIS.md §8.3）：
 *   - **单一实现**：RDDI / HID / BULK / ... 全部调用 ORBMDK_LogWrite，不再各写一套；
 *   - **头文件暴露**：宏在这里，跨编译单元可用（旧实现把宏写在 .cpp 里，属死代码）；
 *   - **多 Sink**：日志文件 / stdout / OutputDebugStringA / 宿主回调
 *     （RDDI_SetLogCallback 经此接通，Keil 的日志窗口能收到消息）；
 *   - **单层过滤、格式化之前判断**：被过滤时不构造字符串（零开销）；
 *   - **线程安全**：内部单锁；宿主回调在**锁外**调用，避免回调再打日志时自锁。
 *
 * 排障开关（对所有模块一致，免重启、免命令行）：
 *   %TEMP%\ORBMDK_LOG_LEVEL   0=DEBUG 1=INFO 2=WARN 3=ERROR（最多 1 秒生效）
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
    ORBMDK_LOG_WARN  = 2,
    ORBMDK_LOG_ERROR = 3,
    ORBMDK_LOG_NONE  = 4,
};

/** 宿主日志回调：level 为上面的内部级别（RDDI 级别的映射由桥接层负责）。 */
typedef void (*ORBMDK_LogSinkFn)(void* context, const char* msg, int level);

/** 唯一实现：按运行期阈值过滤后写到所有已启用 Sink（线程安全）。 */
void ORBMDK_LogWrite(int level, const char* module, const char* fmt, ...);

/**
 * @brief 传输层"命令级"日志：**不受级别控制**，只落日志文件
 *
 * 为什么单独一个入口：V1/V2 的 DAP 命令往返原本完全不可见（默认阈值 ERROR），
 * 而"AGDI 连上目标后什么都不做就断开"这类问题必须靠命令级线索定位 ——
 * 传输层的排障线索不允许依赖日志级别。不送 stdout/DebugView/宿主回调，避免噪声。
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
#define ORBMDK_LOG_DEBUG(...) ORBMDK_LogWrite(ORBMDK_LOG_DEBUG, ORBMDK_LOG_MODULE, __VA_ARGS__)
#define ORBMDK_LOG_INFO(...)  ORBMDK_LogWrite(ORBMDK_LOG_INFO,  ORBMDK_LOG_MODULE, __VA_ARGS__)
#define ORBMDK_LOG_WARN(...)  ORBMDK_LogWrite(ORBMDK_LOG_WARN,  ORBMDK_LOG_MODULE, __VA_ARGS__)
#define ORBMDK_LOG_ERROR(...) ORBMDK_LogWrite(ORBMDK_LOG_ERROR, ORBMDK_LOG_MODULE, __VA_ARGS__)
#define ORBMDK_LOG_TRACE(...) ORBMDK_LogTrace(ORBMDK_LOG_MODULE, __VA_ARGS__)

/* 跨模块打日志时显式给出模块名 */
#define ORBMDK_LOG_AT_DEBUG(mod, ...) ORBMDK_LogWrite(ORBMDK_LOG_DEBUG, mod, __VA_ARGS__)
#define ORBMDK_LOG_AT_INFO(mod, ...)  ORBMDK_LogWrite(ORBMDK_LOG_INFO,  mod, __VA_ARGS__)
#define ORBMDK_LOG_AT_WARN(mod, ...)  ORBMDK_LogWrite(ORBMDK_LOG_WARN,  mod, __VA_ARGS__)
#define ORBMDK_LOG_AT_ERROR(mod, ...) ORBMDK_LogWrite(ORBMDK_LOG_ERROR, mod, __VA_ARGS__)

#endif /* ORBMDK_LOG_H */
