/**
 * @file ORBMDK_COBS.h
 * @brief COBS (Consistent Overhead Byte Stuffing) 编解码器
 *
 * COBS 用于帧同步和透明字节填充，是 OFLOW 协议的基础
 * 参考: RFC 3309
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// COBS 常量
// ============================================================================
#define COBS_MAX_PACKET_LEN         4096
#define COBS_SYNC_CHAR              0x00
#define COBS_OVERALL_MAX_PACKET_LEN (COBS_MAX_PACKET_LEN + 10)
#define COBS_MAX_ENC_PACKET_LEN     (COBS_OVERALL_MAX_PACKET_LEN + COBS_OVERALL_MAX_PACKET_LEN / 254)
#define COBS_EOP_LEN                1

// ============================================================================
// COBS 状态
// ============================================================================
enum ORBMDK_COBS_State {
    COBS_STATE_IDLE,
    COBS_STATE_RXING,
    COBS_STATE_DRAINING
};

// ============================================================================
// COBS 帧结构
// ============================================================================
struct ORBMDK_COBS_Frame {
    unsigned int len;                              // 解码后数据长度
    uint8_t data[COBS_MAX_ENC_PACKET_LEN];        // 编码后的数据缓冲区
};

// ============================================================================
// COBS 解码器上下文
// ============================================================================
struct ORBMDK_COBS_Context {
    struct ORBMDK_COBS_Frame frame;                // 已解码完成的帧
    enum ORBMDK_COBS_State state;                  // 解码状态机状态
    int intervalCount;                             // 当前块剩余字节数
    bool maxCount;                                 // 当前块是否由 0xFF 起始
    int error;                                     // 错误计数
    struct ORBMDK_COBS_Frame partialFrame;         // 正在收集的部分帧
    bool selfAllocated;                            // 是否自己分配内存
    bool frameReady;                               // 已解码出一帧，等待交付回调
};

// ============================================================================
// COBS 包回调
// ============================================================================
typedef void (*ORBMDK_COBS_Packet_CB)(struct ORBMDK_COBS_Frame* frame, void* param);

// ============================================================================
// COBS API 函数
// ============================================================================

/**
 * @brief 初始化 COBS 解码器
 * @param ctx COBS 上下文 (可为 NULL，会自动分配)
 * @return 初始化后的上下文
 */
struct ORBMDK_COBS_Context* ORBMDK_COBS_Init(struct ORBMDK_COBS_Context* ctx);

/**
 * @brief 销毁 COBS 解码器
 * @param ctx COBS 上下文
 */
void ORBMDK_COBS_Delete(struct ORBMDK_COBS_Context* ctx);

/**
 * @brief 推送数据到 COBS 解码器
 * @param ctx COBS 上下文
 * @param data 输入数据
 * @param len 数据长度
 * @param callback 包接收回调
 * @param param 用户参数
 */
void ORBMDK_COBS_Pump(struct ORBMDK_COBS_Context* ctx, const uint8_t* data, size_t len,
                       ORBMDK_COBS_Packet_CB callback, void* param);

/**
 * @brief 简单解码 (不处理帧边界)
 * @param input 编码输入
 * @param len 输入长度
 * @param output 输出帧
 * @return 成功返回 true
 */
bool ORBMDK_COBS_SimpleDecode(const uint8_t* input, size_t len, struct ORBMDK_COBS_Frame* output);

/**
 * @brief 获取帧边界
 * @param input 编码数据
 * @param len 数据长度
 * @return 帧结束位置 (含), -1 表示无完整帧
 */
const uint8_t* ORBMDK_COBS_GetFrameExtent(const uint8_t* input, size_t len);

/**
 * @brief 检查是否为帧结束
 * @param input 编码数据
 * @return 是否为帧结束
 */
bool ORBMDK_COBS_IsEOFrame(const uint8_t* input);

/**
 * @brief 编码数据
 * @param input 输入数据
 * @param len 输入长度
 * @param output 编码输出帧
 */
void ORBMDK_COBS_Encode(const uint8_t* input, size_t len, struct ORBMDK_COBS_Frame* output);

/**
 * @brief 获取错误计数
 * @param ctx COBS 上下文
 * @return 错误计数
 */
static inline int ORBMDK_COBS_GetErrors(struct ORBMDK_COBS_Context* ctx) {
    return ctx ? ctx->error : 0;
}

#ifdef __cplusplus
}
#endif
