/**
 * @file ORBMDK_OFLOW.h
 * @brief OFLOW (Orbuculum Flow) 协议解码器
 *
 * OFLOW 是基于 COBS 的帧协议，用于传输 trace 数据流
 * 支持带时间戳的数据包封装
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "ORBMDK_COBS.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// OFLOW 常量
// ============================================================================
#define OFLOW_MAX_PACKET_LEN      (COBS_MAX_PACKET_LEN - 2)
#define OFLOW_MAX_ENC_PACKET_LEN  COBS_MAX_ENC_PACKET_LEN
#define OFLOW_EOP_LEN             COBS_EOP_LEN
#define OFLOW_TS_RESOLUTION       1000000000L     // 纳秒分辨率

// ============================================================================
// OFLOW 包标签类型
// ============================================================================
enum ORBMDK_OFLOW_Tag {
    OFLOW_TAG_RESERVED       = 0x00,
    OFLOW_TAG_DATA           = 0x01,    // 普通数据
    OFLOW_TAG_TIMESTAMP      = 0x02,    // 时间戳数据
    OFLOW_TAG_RESYNC         = 0x03,    // 重新同步
    OFLOW_TAG_FLOW_ON        = 0x04,    // 流开启
    OFLOW_TAG_FLOW_OFF       = 0x05,    // 流关闭
    OFLOW_TAG_HEARTBEAT      = 0x06,    // 心跳
    OFLOW_TAG_DEBUG          = 0x07,    // 调试信息
    OFLOW_TAG_ITM             = 0x08,    // ITM 数据
    OFLOW_TAG_ETM             = 0x09,    // ETM 数据
    OFLOW_TAG_TPIU            = 0x0A,    // TPIU 数据
    OFLOW_TAG_HETM            = 0x0B,    // HTM 数据
    OFLOW_TAG_8B_DATA        = 0x10,    // 8 字节数据
    OFLOW_TAG_8B_TIMESTAMP   = 0x11,    // 8 字节时间戳
    OFLOW_TAG_8B_ITM         = 0x12,    // 8 字节 ITM
    OFLOW_TAG_8B_ETM         = 0x13,    // 8 字节 ETM
};

// ============================================================================
// OFLOW 帧结构
// ============================================================================
struct ORBMDK_OFLOW_Frame {
    unsigned int len;               // 帧长度
    uint8_t tag;                    // 包类型标签
    uint8_t checksum;               // 校验和
    bool good;                      // 校验是否通过
    uint64_t timestamp;             // 时间戳 (纳秒)
    uint8_t* data;                  // 数据指针
};

// ============================================================================
// OFLOW 解码器上下文
// ============================================================================
struct ORBMDK_OFLOW_Context {
    bool selfAllocated;             // 是否自己分配内存
    struct ORBMDK_COBS_Context cobs;    // COBS 解码器
    struct ORBMDK_OFLOW_Frame frame;   // 当前帧
    uint64_t errorCount;             // 错误计数
    uint64_t packetCount;            // 包计数

    // 用户回调
    void (*packetCallback)(struct ORBMDK_OFLOW_Frame* frame, void* param);
    void* userParam;
};

// ============================================================================
// OFLOW API 函数
// ============================================================================

/**
 * @brief 初始化 OFLOW 解码器
 * @param ctx OFLOW 上下文 (可为 NULL，会自动分配)
 * @return 初始化后的上下文
 */
struct ORBMDK_OFLOW_Context* ORBMDK_OFLOW_Init(struct ORBMDK_OFLOW_Context* ctx);

/**
 * @brief 销毁 OFLOW 解码器
 * @param ctx OFLOW 上下文
 */
void ORBMDK_OFLOW_Delete(struct ORBMDK_OFLOW_Context* ctx);

/**
 * @brief 推送数据到 OFLOW 解码器
 * @param ctx OFLOW 上下文
 * @param data 输入数据
 * @param len 数据长度
 */
void ORBMDK_OFLOW_Pump(struct ORBMDK_OFLOW_Context* ctx, const uint8_t* data, size_t len);

/**
 * @brief 设置包接收回调
 * @param ctx OFLOW 上下文
 * @param callback 回调函数
 * @param param 用户参数
 */
void ORBMDK_OFLOW_SetCallback(struct ORBMDK_OFLOW_Context* ctx,
                              void (*callback)(struct ORBMDK_OFLOW_Frame* frame, void* param),
                              void* param);

/**
 * @brief 获取 OFLOW 错误计数
 * @param ctx OFLOW 上下文
 * @return 错误计数
 */
static inline uint64_t ORBMDK_OFLOW_GetErrors(struct ORBMDK_OFLOW_Context* ctx) {
    return ctx ? ctx->errorCount : (uint64_t)(-1);
}

/**
 * @brief 获取 COBS 错误计数
 * @param ctx OFLOW 上下文
 * @return COBS 错误计数
 */
static inline uint64_t ORBMDK_OFLOW_GetCOBSErrors(struct ORBMDK_OFLOW_Context* ctx) {
    return ctx ? ORBMDK_COBS_GetErrors(&ctx->cobs) : (uint64_t)(-1);
}

/**
 * @brief 获取时间戳分辨率
 * @param ctx OFLOW 上下文
 * @return 分辨率 (纳秒)
 */
static inline uint64_t ORBMDK_OFLOW_GetResolution(struct ORBMDK_OFLOW_Context* ctx) {
    (void)ctx;
    return OFLOW_TS_RESOLUTION;
}

/**
 * @brief 获取包计数
 * @param ctx OFLOW 上下文
 * @return 包计数
 */
static inline uint64_t ORBMDK_OFLOW_GetPacketCount(struct ORBMDK_OFLOW_Context* ctx) {
    return ctx ? ctx->packetCount : 0;
}

/**
 * @brief 重置 OFLOW 解码器
 * @param ctx OFLOW 上下文
 */
void ORBMDK_OFLOW_Reset(struct ORBMDK_OFLOW_Context* ctx);

#ifdef __cplusplus
}
#endif
