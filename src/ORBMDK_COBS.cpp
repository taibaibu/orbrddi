/**
 * @file ORBMDK_COBS.cpp
 * @brief COBS 编解码实现
 */

#include "pch.h"
#include "ORBMDK_COBS.h"

// ============================================================================
// 内部常量
// ============================================================================
static const uint8_t COBS_EOP_VALUE = COBS_SYNC_CHAR;

// ============================================================================
// 内部函数
// ============================================================================

/**
 * @brief 处理单个字节 (真正的 COBS 解码)
 *
 * 与 orbuculum cobs.c / COBSPump 对齐：
 *   - IDLE: 遇到非 0 字节即作为第一个"块长度"开始新帧
 *   - RXING: 每收到一个字节 intervalCount--；归零时该字节是"下一块长度"，
 *            并隐含插入一个 0x00 数据字节 (0xFF 块除外)；若归零处正好是 0x00
 *            则整帧结束
 *
 * 修复: 旧实现把**编码后的原始字节**直接当作解码结果存入 partialFrame，
 *       既没有剥离块长度字节，也没有还原被 COBS 替换掉的 0x00。
 */
static void _cobsProcessByte(struct ORBMDK_COBS_Context* ctx, uint8_t byte)
{
    switch (ctx->state) {
    case COBS_STATE_IDLE:
        if (byte != COBS_SYNC_CHAR) {
            // 帧的第一个块: 该字节是块长度 (1..255)
            ctx->partialFrame.len = 0;
            ctx->intervalCount = byte;
            ctx->maxCount = (byte == 0xFF);
            ctx->state = COBS_STATE_RXING;
        }
        // 0x00 为帧分隔/空闲，继续等待
        break;

    case COBS_STATE_RXING:
        if (ctx->intervalCount > 0) {
            ctx->intervalCount--;
        }

        if (ctx->intervalCount == 0) {
            if (byte == COBS_SYNC_CHAR) {
                // 块结束处恰好是帧结束符 -> 整帧完成
                ctx->frame = ctx->partialFrame;
                ctx->frameReady = true;
                ctx->state = COBS_STATE_DRAINING;
            } else {
                // 块结束 -> 还原一个 0x00 数据字节 (0xFF 块除外)
                if (!ctx->maxCount) {
                    if (ctx->partialFrame.len < COBS_MAX_PACKET_LEN) {
                        ctx->partialFrame.data[ctx->partialFrame.len++] = COBS_SYNC_CHAR;
                    }
                }
                // 该字节是下一块的长度
                ctx->intervalCount = byte;
                ctx->maxCount = (byte == 0xFF);
            }
        } else {
            // 帧溢出或块内出现非法 0x00 -> 丢弃该帧，等待下一个同步字符
            if (ctx->partialFrame.len >= COBS_MAX_PACKET_LEN || byte == COBS_SYNC_CHAR) {
                ctx->error++;
                ctx->partialFrame.len = 0;
                ctx->state = COBS_STATE_DRAINING;
            } else {
                ctx->partialFrame.data[ctx->partialFrame.len++] = byte;
            }
        }
        break;

    case COBS_STATE_DRAINING:
        // 错误后等待 Pump 重新同步
        break;
    }
}

// ============================================================================
// API 实现
// ============================================================================

struct ORBMDK_COBS_Context* ORBMDK_COBS_Init(struct ORBMDK_COBS_Context* ctx)
{
    if (!ctx) {
        ctx = (struct ORBMDK_COBS_Context*)malloc(sizeof(struct ORBMDK_COBS_Context));
        if (!ctx) return nullptr;
        ctx->selfAllocated = true;
    } else {
        ctx->selfAllocated = false;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->state = COBS_STATE_IDLE;
    return ctx;
}

void ORBMDK_COBS_Delete(struct ORBMDK_COBS_Context* ctx)
{
    if (!ctx) return;
    
    if (ctx->selfAllocated) {
        free(ctx);
    }
}

void ORBMDK_COBS_Pump(struct ORBMDK_COBS_Context* ctx, const uint8_t* data, size_t len,
                       ORBMDK_COBS_Packet_CB callback, void* param)
{
    if (!ctx || !data) return;

    for (size_t i = 0; i < len; i++) {
        // 错误后处于 DRAINING: 等待下一个同步字符重新对齐帧边界
        if (ctx->state == COBS_STATE_DRAINING && !ctx->frameReady) {
            if (data[i] == COBS_SYNC_CHAR) {
                ctx->state = COBS_STATE_IDLE;
            }
            continue;
        }

        _cobsProcessByte(ctx, data[i]);

        if (ctx->frameReady) {
            // 一帧解码完成: 交付回调，随后复位以便接收下一帧
            // (修复: 旧实现 DRAINING 后未清空 partialFrame.len，导致帧间数据残留)
            ctx->frameReady = false;
            if (callback) {
                callback(&ctx->frame, param);
            }
            ctx->partialFrame.len = 0;
            ctx->intervalCount = 0;
            ctx->maxCount = false;
            ctx->state = COBS_STATE_IDLE;
        }
    }
}

bool ORBMDK_COBS_SimpleDecode(const uint8_t* input, size_t len, struct ORBMDK_COBS_Frame* output)
{
    if (!input || !output) return false;

    output->len = 0;
    size_t srcIdx = 0;
    bool firstBlock = true;

    while (srcIdx < len) {
        uint8_t blockLen = input[srcIdx++];
        
        if (blockLen == 0) {
            // 遇到 0 表示帧结束
            break;
        }

        if (firstBlock && blockLen == 0xFF) {
            // 第一个块为 0xFF 表示后面跟着 0x00
            output->data[output->len++] = 0;
            firstBlock = false;
            continue;
        }

        // 块长度减 1 表示后面 0x00 的位置
        size_t copyLen = blockLen - 1;
        
        if (output->len + copyLen > COBS_MAX_PACKET_LEN) {
            return false;
        }

        if (srcIdx + copyLen > len) {
            return false;
        }

        memcpy(&output->data[output->len], &input[srcIdx], copyLen);
        output->len += static_cast<uint32_t>(copyLen);
        srcIdx += copyLen;

        // 检查并复制 0x00
        if (input[srcIdx] != 0) {
            return false;
        }
        srcIdx++;

        firstBlock = false;
    }

    return true;
}

const uint8_t* ORBMDK_COBS_GetFrameExtent(const uint8_t* input, size_t len)
{
    if (!input || len == 0) return nullptr;

    size_t idx = 0;
    while (idx < len) {
        uint8_t code = input[idx];
        
        if (code == 0) {
            // 找到帧结束
            return input + idx;
        }

        // 计算块大小
        size_t blockSize = code == 0xFF ? 1 : code;
        
        if (blockSize == 0 || idx + blockSize > len) {
            return nullptr;  // 无效数据
        }

        idx += blockSize;
        
        // 跳过 0x00 分隔符
        if (idx < len && input[idx] == 0) {
            idx++;
        }
    }

    return nullptr;  // 没有完整帧
}

bool ORBMDK_COBS_IsEOFrame(const uint8_t* input)
{
    return input != nullptr && *input == 0;
}

void ORBMDK_COBS_Encode(const uint8_t* input, size_t len, struct ORBMDK_COBS_Frame* output)
{
    if (!input || !output) return;

    output->len = 0;
    size_t srcIdx = 0;

    while (srcIdx < len) {
        // 查找下一个 0x00
        size_t nonZeroLen = 0;
        while (srcIdx + nonZeroLen < len && input[srcIdx + nonZeroLen] != 0) {
            nonZeroLen++;
            if (nonZeroLen >= 254) break;
        }

        if (nonZeroLen > 0) {
            // 写入块长度 (不包括 0x00)
            output->data[output->len++] = (uint8_t)(nonZeroLen + 1);
            
            // 复制数据
            memcpy(&output->data[output->len], &input[srcIdx], nonZeroLen);
            output->len += static_cast<uint32_t>(nonZeroLen);
            srcIdx += nonZeroLen;

            // 写入 0x00 占位符
            if (output->len < COBS_MAX_ENC_PACKET_LEN) {
                output->data[output->len++] = 0;
            }
        }

        // 处理 0x00
        if (srcIdx < len && input[srcIdx] == 0) {
            output->data[output->len++] = 1;  // 块长度 = 1 表示只有分隔符
            output->data[output->len++] = 0;
            srcIdx++;
        }
    }

    // 添加帧结束标记
    if (output->len < COBS_MAX_ENC_PACKET_LEN) {
        output->data[output->len++] = 0;
    }
}
