/**
 * @file ORBMDK_OFLOW.cpp
 * @brief OFLOW 协议解码实现
 */

#include "pch.h"
#include "ORBMDK_OFLOW.h"
#include "ORBMDK_Trace.h"

// ============================================================================
// 内部常量
// ============================================================================

// OFLOW 帧头结构:
// [0x7E] [Length (2B)] [Tag (1B)] [Timestamp? (0 or 8B)] [Checksum (1B)] [Data...] [0x7E]

#define OFLOW_START_MARKER     0x7E
#define OFLOW_MIN_HEADER_SIZE  4   // Length(2) + Tag(1) + Checksum(1)
#define OFLOW_MAX_TIMESTAMP    8

// ============================================================================
// 内部函数
// ============================================================================

/**
 * @brief 计算校验和
 */
static uint8_t _oflowChecksum(const uint8_t* data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

/**
 * @brief 处理接收到的 COBS 帧
 */
static void _oflowFrameReceived(struct ORBMDK_COBS_Frame* frame, void* param)
{
    struct ORBMDK_OFLOW_Context* ctx = (struct ORBMDK_OFLOW_Context*)param;
    if (!ctx || !frame || frame->len < OFLOW_MIN_HEADER_SIZE) {
        if (ctx) ctx->errorCount++;
        return;
    }

    uint8_t* data = frame->data;
    size_t len = frame->len;

    // 检查起始标记
    if (data[0] != OFLOW_START_MARKER) {
        ctx->errorCount++;
        return;
    }

    // 解析帧头
    size_t idx = 1;
    uint16_t payloadLen = (uint16_t)(data[idx] | (data[idx + 1] << 8));
    idx += 2;

    if (idx + 2 > len) {
        ctx->errorCount++;
        return;
    }

    uint8_t tag = data[idx++];
    uint8_t checksum = data[idx + payloadLen - 1];

    // 解析时间戳
    uint64_t timestamp = 0;
    size_t tsBytes = 0;
    
    if (tag & 0x80) {
        // 带时间戳
        tsBytes = (tag >> 4) & 0x07;
        if (tsBytes > OFLOW_MAX_TIMESTAMP || idx + tsBytes > len) {
            ctx->errorCount++;
            return;
        }

        for (size_t t = 0; t < tsBytes; t++) {
            timestamp |= ((uint64_t)data[idx + t]) << (t * 8);
        }
        idx += tsBytes;
    }

    // 提取数据并验证校验和
    size_t dataLen = payloadLen - 1 - tsBytes - 1;  // Tag + Timestamp + Checksum
    uint8_t calcSum = _oflowChecksum(&data[idx], dataLen);

    if (calcSum != checksum) {
        ctx->errorCount++;
        return;
    }

    // 填充帧结构
    ctx->frame.tag = tag & 0x0F;
    ctx->frame.timestamp = timestamp;
    ctx->frame.len = static_cast<uint32_t>(dataLen);
    ctx->frame.good = true;
    ctx->frame.data = &data[idx];
    ctx->frame.checksum = checksum;

    ctx->packetCount++;

    // 调用用户回调
    if (ctx->packetCallback) {
        ctx->packetCallback(&ctx->frame, ctx->userParam);
    }
}

// ============================================================================
// API 实现
// ============================================================================

struct ORBMDK_OFLOW_Context* ORBMDK_OFLOW_Init(struct ORBMDK_OFLOW_Context* ctx)
{
    if (!ctx) {
        ctx = (struct ORBMDK_OFLOW_Context*)malloc(sizeof(struct ORBMDK_OFLOW_Context));
        if (!ctx) return nullptr;
        ctx->selfAllocated = true;
    } else {
        ctx->selfAllocated = false;
    }

    memset(ctx, 0, sizeof(*ctx));
    ORBMDK_COBS_Init(&ctx->cobs);
    return ctx;
}

void ORBMDK_OFLOW_Delete(struct ORBMDK_OFLOW_Context* ctx)
{
    if (!ctx) return;

    ORBMDK_COBS_Delete(&ctx->cobs);

    if (ctx->selfAllocated) {
        free(ctx);
    }
}

void ORBMDK_OFLOW_Pump(struct ORBMDK_OFLOW_Context* ctx, const uint8_t* data, size_t len)
{
    if (!ctx || !data) return;

    // 通过 COBS 解码器处理
    ORBMDK_COBS_Pump(&ctx->cobs, data, len, _oflowFrameReceived, ctx);
}

void ORBMDK_OFLOW_SetCallback(struct ORBMDK_OFLOW_Context* ctx,
                              void (*callback)(struct ORBMDK_OFLOW_Frame* frame, void* param),
                              void* param)
{
    if (!ctx) return;
    ctx->packetCallback = callback;
    ctx->userParam = param;
}

void ORBMDK_OFLOW_Reset(struct ORBMDK_OFLOW_Context* ctx)
{
    if (!ctx) return;

    memset(&ctx->frame, 0, sizeof(ctx->frame));
    ctx->errorCount = 0;
    ctx->packetCount = 0;
    
    // 重置 COBS
    ORBMDK_COBS_Init(&ctx->cobs);
}
