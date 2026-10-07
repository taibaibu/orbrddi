/**
 * @file ORBMDK_Trace.h
 * @brief Trace 解码器适配层
 *
 * 集成 orbuculum trace 解码库
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// 协议类型
// ============================================================================
enum ORBMDK_Trace_Protocol {
    TRACE_PROT_NONE       = 0,
    TRACE_PROT_SWO_UART   = 1,
    TRACE_PROT_SWO_MANCHESTER = 2,
    TRACE_PROT_ITM        = 3,
    TRACE_PROT_ETM35      = 4,
    TRACE_PROT_ETM4       = 5,
    TRACE_PROT_TPIU       = 6,
    TRACE_PROT_MTB        = 7,
};

// ============================================================================
// Trace 事件
// ============================================================================
enum ORBMDK_Trace_Event {
    TRACE_EV_NONE,
    TRACE_EV_UNSYNCED,
    TRACE_EV_SYNCED,
    TRACE_EV_ERROR,
    TRACE_EV_BRANCH,         // 分支事件
    TRACE_EV_EXCEPTION,       // 异常事件
    TRACE_EV_TIMESTAMP,      // 时间戳
    TRACE_EV_CONTEXTID,      // 上下文 ID
};

// ============================================================================
// Trace 变化类型
// ============================================================================
enum ORBMDK_Trace_Change {
    EV_CH_EX_ENTRY,
    EV_CH_EX_EXIT,
    EV_CH_ADDRESS,
    EV_CH_CYCLECOUNT,
    EV_CH_CONTEXTID,
    EV_CH_TRIGGER,
    EV_CH_VMID,
    EV_CH_TSTAMP,
    EV_CH_OVERFLOW,
    EV_CH_DATASYNC,
    EV_CH_SECURE,
    EV_CH_ALTISA,
    EV_CH_HYP,
    EV_CH_JAZELLE,
    EV_CH_THUMB,
};

// ============================================================================
// CPU 状态
// ============================================================================
struct ORBMDK_Trace_CPUState {
    uint32_t changeRecord;               // 变化记录

    // 地址信息
    uint32_t addr;                       // 当前地址
    uint32_t nextAddr;                   // 下一地址 (MTB 模式)
    uint8_t addrMode;                    // 0=Thumb, 1=ARM, 2=Jazzelle

    // 上下文
    uint32_t contextID;                  // 上下文 ID
    uint8_t vmid;                        // 虚拟机 ID
    uint8_t exceptionLevel;              // 异常级别
    bool amSecure;                       // 安全模式
    bool am64bit;                        // 64 位模式

    // 性能计数
    uint64_t cycleCount;                 // 周期计数
    uint64_t instCount;                  // 指令计数
    uint64_t ts;                         // 时间戳

    // 异常信息
    uint16_t exception;                  // 异常类型
    uint16_t resume;                     // 恢复代码
    bool jazelle;                        // Jazzelle 模式
    bool thumb;                          // Thumb 模式

    // 原子执行信息
    uint8_t eatoms;                     // 执行的原子数
    uint8_t natoms;                     // 非执行的原子数
    uint8_t numInstructions;             // 指令数

    // 状态
    bool serious;                        // 严重错误标志
    uint64_t overflows;                  // 溢出计数
};

// ============================================================================
// Trace 解码器统计
// ============================================================================
struct ORBMDK_Trace_Stats {
    uint32_t lostSyncCount;              // 丢失同步次数
    uint32_t syncCount;                  // 同步次数
    uint32_t packets;                    // 包计数
    uint32_t overflow;                   // 溢出计数
    uint32_t error;                      // 错误计数
};

// ============================================================================
// 解码器句柄
// ============================================================================
typedef void* ORBMDK_Trace_Handle;

// ============================================================================
// 回调函数类型
// ============================================================================

// ITM 数据回调 (源地址, 数据, 长度, 用户数据)
typedef void (*ORBMDK_Trace_ITM_CB)(uint8_t srcAddr, const uint8_t* data, size_t len, void* userData);

// SWO 数据回调 (数据字节, 用户数据)
typedef void (*ORBMDK_Trace_SWO_CB)(uint8_t byte, void* userData);

// 分支事件回调 (从地址, 到地址, CPU状态, 用户数据)
typedef void (*ORBMDK_Trace_Branch_CB)(uint32_t fromAddr, uint32_t toAddr, struct ORBMDK_Trace_CPUState* state, void* userData);

// TPIU 包回调 (数据, 长度, 用户数据)
typedef void (*ORBMDK_Trace_TPIU_CB)(const uint8_t* data, size_t len, void* userData);

// ============================================================================
// 解码器 API
// ============================================================================

/**
 * @brief 创建 Trace 解码器
 * @param protocol 协议类型
 * @return 解码器句柄，失败返回 NULL
 */
ORBMDK_Trace_Handle ORBMDK_Trace_Create(enum ORBMDK_Trace_Protocol protocol);

/**
 * @brief 销毁 Trace 解码器
 * @param handle 解码器句柄
 */
void ORBMDK_Trace_Destroy(ORBMDK_Trace_Handle handle);

/**
 * @brief 初始化解码器
 * @param handle 解码器句柄
 * @param protocol 协议类型
 */
void ORBMDK_Trace_Init(ORBMDK_Trace_Handle handle, enum ORBMDK_Trace_Protocol protocol);

/**
 * @brief 馈送数据到解码器
 * @param handle 解码器句柄
 * @param data 数据
 * @param len 数据长度
 * @return 事件类型
 */
int ORBMDK_Trace_Pump(ORBMDK_Trace_Handle handle, const uint8_t* data, size_t len);

/**
 * @brief 获取解码器统计
 * @param handle 解码器句柄
 * @return 统计信息
 */
struct ORBMDK_Trace_Stats* ORBMDK_Trace_GetStats(ORBMDK_Trace_Handle handle);

/**
 * @brief 检查是否同步
 * @param handle 解码器句柄
 * @return 是否同步
 */
bool ORBMDK_Trace_IsSynced(ORBMDK_Trace_Handle handle);

/**
 * @brief 强制同步状态
 * @param handle 解码器句柄
 * @param isSynced 是否同步
 */
void ORBMDK_Trace_ForceSync(ORBMDK_Trace_Handle handle, bool isSynced);

/**
 * @brief 获取 CPU 状态
 * @param handle 解码器句柄
 * @return CPU 状态
 */
struct ORBMDK_Trace_CPUState* ORBMDK_Trace_GetCPUState(ORBMDK_Trace_Handle handle);

// ============================================================================
// ITM 特定 API
// ============================================================================

/**
 * @brief 设置 ITM 回调
 * @param handle 解码器句柄
 * @param callback 回调函数
 * @param userData 用户数据
 */
void ORBMDK_Trace_SetITMCallback(ORBMDK_Trace_Handle handle, ORBMDK_Trace_ITM_CB callback, void* userData);

/**
 * @brief 获取 ITM 包
 * @param handle 解码器句柄
 * @param srcAddr 输出: 源地址
 * @param data 输出: 数据缓冲区
 * @param maxLen 最大数据长度
 * @return 实际数据长度
 */
size_t ORBMDK_Trace_ITM_GetPacket(ORBMDK_Trace_Handle handle, uint8_t* srcAddr, uint8_t* data, size_t maxLen);

// ============================================================================
// SWO 特定 API
// ============================================================================

/**
 * @brief 设置 SWO 回调
 * @param handle 解码器句柄
 * @param callback 回调函数
 * @param userData 用户数据
 */
void ORBMDK_Trace_SetSWOCallback(ORBMDK_Trace_Handle handle, ORBMDK_Trace_SWO_CB callback, void* userData);

// ============================================================================
// TPIU 特定 API
// ============================================================================

/**
 * @brief 设置 TPIU 回调
 * @param handle 解码器句柄
 * @param callback 回调函数
 * @param userData 用户数据
 */
void ORBMDK_Trace_SetTPIUCallback(ORBMDK_Trace_Handle handle, ORBMDK_Trace_TPIU_CB callback, void* userData);

/**
 * @brief 获取 TPIU 包
 * @param handle 解码器句柄
 * @param data 输出: 数据缓冲区
 * @param maxLen 最大数据长度
 * @return 实际数据长度
 */
size_t ORBMDK_Trace_TPIU_GetPacket(ORBMDK_Trace_Handle handle, uint8_t* data, size_t maxLen);

// ============================================================================
// ETM 特定 API
// ============================================================================

/**
 * @brief 设置分支回调
 * @param handle 解码器句柄
 * @param callback 回调函数
 * @param userData 用户数据
 */
void ORBMDK_Trace_SetBranchCallback(ORBMDK_Trace_Handle handle, ORBMDK_Trace_Branch_CB callback, void* userData);

/**
 * @brief 设置是否使用备用地址编码
 * @param handle 解码器句柄
 * @param usingAlt 是否使用备用编码
 */
void ORBMDK_Trace_SetAltAddrEncode(ORBMDK_Trace_Handle handle, bool usingAlt);

// ============================================================================
// SWO 波特率配置
// ============================================================================

/**
 * @brief 记录 SWO 时钟 / 波特率
 * @param handle 解码器句柄
 * @param clock SWO 时钟频率（Hz）
 * @param baud 目标波特率
 *
 * 说明：本层是纯解码层，输入数据已是探针送来的字节流，波特率在探针侧生效，
 * 解码本身不需要它。因此这里只负责把 clock / baud 以及两者能整除时的分频比
 * 记进句柄（供日志与排查"乱码"时核对），并**不**向探针下发任何东西。
 * 真正的下发行程是：AGDI 的 TraceBaudrate= → CMSIS_DAP_ConfigureDebugger →
 * StreamingTrace_SetBaudrate / DAP_SWO_Baudrate（见 ORBMDK_RDDI.cpp / ORBMDK_HID.cpp）。
 */
void ORBMDK_Trace_SWO_SetBaud(ORBMDK_Trace_Handle handle, uint32_t clock, uint32_t baud);

#ifdef __cplusplus
}
#endif
