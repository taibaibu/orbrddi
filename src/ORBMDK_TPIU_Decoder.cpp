/**
 * @file ORBMDK_TPIU_Decoder.cpp
 * @brief TPIU 解码器实现
 */

#include "pch.h"
#include "ORBMDK_TPIU_Decoder.h"

#include <cstring>

// ============================================================================
// TPIU 协议常量
// ============================================================================

// TPIU 同步模式检测
// 0x7X 表示 X+1 字节的同步序列后跟有效数据
#define TPIU_SYNC_MIN     0x70  // 最小同步字节
#define TPIU_SYNC_7F      0x7F  // 7 字节同步标记

// TPIU 帧格式
// 帧头: 0x0000007X (X = 数据长度, 0-7)
// 帧数据: 后续 X 字节
#define TPIU_MAX_PAYLOAD  255

// ============================================================================
// 内部函数
// ============================================================================

static bool _isSyncByte(uint8_t c)
{
    // 同步字节: 0x00 或 0xFF
    return c == 0x00 || c == 0xFF;
}

static bool _isFrameStart(uint8_t c)
{
    // 帧开始: 高 4 位为 0x7，低 4 位表示长度-1
    return (c & 0xF0) == 0x70;
}

// ============================================================================
// API 实现
// ============================================================================

struct ORBMDK_TPIU_Decoder* ORBMDK_TPIU_Create(void)
{
    struct ORBMDK_TPIU_Decoder* dec = new struct ORBMDK_TPIU_Decoder;
    if (dec) {
        memset(dec, 0, sizeof(*dec));
        dec->state = TPIU_STATE_UNSYNCED;
        dec->synced = false;
    }
    return dec;
}

void ORBMDK_TPIU_Destroy(struct ORBMDK_TPIU_Decoder* dec)
{
    if (dec) {
        delete dec;
    }
}

void ORBMDK_TPIU_Init(struct ORBMDK_TPIU_Decoder* dec)
{
    if (!dec) return;
    memset(&dec->stats, 0, sizeof(dec->stats));
    dec->state = TPIU_STATE_UNSYNCED;
    dec->synced = false;
    dec->payloadLen = 0;
    dec->syncCount = 0;
    dec->frameLen = 0;
    dec->packetReady = false;
}

void ORBMDK_TPIU_ForceSync(struct ORBMDK_TPIU_Decoder* dec)
{
    if (!dec) return;
    dec->state = TPIU_STATE_UNSYNCED;
    dec->synced = false;
    dec->packetReady = false;
    dec->payloadLen = 0;
    dec->syncCount = 0;
    dec->stats.lostSync++;
}

bool ORBMDK_TPIU_IsSynced(struct ORBMDK_TPIU_Decoder* dec)
{
    return dec && dec->synced;
}

struct ORBMDK_TPIU_Stats* ORBMDK_TPIU_GetStats(struct ORBMDK_TPIU_Decoder* dec)
{
    return dec ? &dec->stats : nullptr;
}

/**
 * @brief 泵入一个字节，返回事件
 *
 * 状态机（自洽版本，修复原实现 frameLen 判定与 GetPacket 条件互相矛盾的问题）：
 *   UNSYNCED --(>=4 个同步字节)--> SYNCING --(0x7X 帧起始)--> PAYLOAD
 *   PAYLOAD  --(收满 frameLen 字节)--> FRAME(包就绪) --(0x7X)--> PAYLOAD
 *
 * 包是否可取由显式的 packetReady 标志决定，ORBMDK_TPIU_GetPacket 只检查该标志，
 * 不再依赖 state 的隐式条件。
 */
enum ORBMDK_TPIU_PumpEvent ORBMDK_TPIU_Pump(struct ORBMDK_TPIU_Decoder* dec, uint8_t c)
{
    if (!dec) return TPIU_EV_ERROR;

    dec->stats.bytes++;

    switch (dec->state) {
    case TPIU_STATE_UNSYNCED:
        if (_isSyncByte(c)) {
            if (dec->syncCount < 0xFF) dec->syncCount++;
            if (dec->syncCount >= 4) {
                dec->state = TPIU_STATE_SYNCING;
                dec->synced = true;
                dec->stats.syncCount++;
                return TPIU_EV_SYNCED;
            }
        } else {
            dec->syncCount = 0;
        }
        return TPIU_EV_UNSYNCED;

    case TPIU_STATE_SYNCING:
        if (_isFrameStart(c)) {
            dec->frameLen = (uint8_t)((c & 0x07) + 1);
            dec->payloadLen = 0;
            dec->packetReady = false;
            dec->state = TPIU_STATE_PAYLOAD;
            return TPIU_EV_RXING;
        }
        if (_isSyncByte(c)) {
            if (dec->syncCount < 0xFF) dec->syncCount++;
            return TPIU_EV_NEWSYNC;
        }
        // 已同步但不是帧起始字节: 丢弃，等待下一帧
        return TPIU_EV_NONE;

    case TPIU_STATE_FRAME:
        // 上一帧已组装完成，等待 GetPacket 取走
        if (_isFrameStart(c)) {
            // 开始下一帧（若上一帧未被取走则视为丢弃）
            if (dec->packetReady) {
                dec->stats.overflow++;
            }
            dec->frameLen = (uint8_t)((c & 0x07) + 1);
            dec->payloadLen = 0;
            dec->packetReady = false;
            dec->state = TPIU_STATE_PAYLOAD;
            return TPIU_EV_RXING;
        }
        if (_isSyncByte(c)) {
            if (dec->syncCount < 0xFF) dec->syncCount++;
            return TPIU_EV_NEWSYNC;
        }
        return TPIU_EV_NONE;

    case TPIU_STATE_PAYLOAD:
        if (dec->payloadLen < (uint8_t)sizeof(dec->payload)) {
            dec->payload[dec->payloadLen++] = c;
        } else {
            dec->stats.overflow++;
        }

        // 检查是否收到完整帧
        if (dec->frameLen > 0 && dec->payloadLen >= dec->frameLen) {
            dec->stats.packets++;
            dec->packetReady = true;
            dec->state = TPIU_STATE_FRAME;
            return TPIU_EV_RXEDPACKET;
        }
        return TPIU_EV_RXING;
    }

    return TPIU_EV_ERROR;
}

bool ORBMDK_TPIU_GetPacket(struct ORBMDK_TPIU_Decoder* dec, struct ORBMDK_TPIU_Packet* pkt)
{
    if (!dec || !pkt) return false;

    // 仅当显式标记"包已就绪"时返回，且取出后清空标志与长度
    if (dec->packetReady && dec->payloadLen > 0) {
        pkt->len = dec->payloadLen;
        memcpy(pkt->data, dec->payload, dec->payloadLen);
        dec->payloadLen = 0;
        dec->packetReady = false;
        return true;
    }

    return false;
}
