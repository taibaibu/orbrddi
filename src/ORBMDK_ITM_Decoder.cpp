/**
 * @file ORBMDK_ITM_Decoder.cpp
 * @brief ITM (Instrumentation Trace Macrocell) 解码器实现
 *
 * 支持：
 * - SW 包 (Software Stimulus)
 * - HW 包 (Hardware Trace, 包含 PC Sample)
 * - TS 包 (Timestamp)
 * - 同步检测
 */

#include "pch.h"
#include "ORBMDK_ITM_Decoder.h"

#include <cstring>

// ============================================================================
// ITM 协议常量 (参考 ARM ITM 规范)
// ============================================================================

// 同步序列: 5 个 0x00 后跟 1 个 0x80
// (ARMv7-M ARM D4.2.1 / orbuculum itmDecoder.c: SYNCMASK=0xFFFFFFFFFFFF,
//  SYNCPATTERN=0x000000000080)
//
// 注意: 0x70 是 **溢出包**(Overflow) 而不是同步包，旧实现误把它当作同步，
// 会导致同步判定错误并丢失真实的溢出事件。
#define ITM_SYNC_MASK     0x0000FFFFFFFFFFFFULL
#define ITM_SYNC_PATTERN  0x0000000000000080ULL

// ITM 包头类型 (bit[1:0])
#define ITM_HEADER_SW     0x01  // 0b01 = Software packet
#define ITM_HEADER_HW     0x02  // 0b10 = Hardware packet
#define ITM_HEADER_TS     0x03  // 0b11 = Timestamp

// HW 包端口号 (源地址)
// DWT 使用端口 2 发送 PC Sample
#define ITM_HW_PORT_PC_SAMPLE    2
#define ITM_HW_PORT_DWT_EVENT    3

// 溢出包
#define ITM_OVERFLOW_PACKET  0x70

// 源包最大负载字节数 (orbuculum: MAX_PACKET = 5)
#define ITM_MAX_PAYLOAD   5

// ============================================================================
// 内部函数
// ============================================================================

/**
 * @brief 从字节流中提取可变长度数据
 */
static uint32_t _extractValue(const uint8_t* data, int len)
{
    uint32_t val = 0;
    for (int i = 0; i < len; i++) {
        val |= ((uint32_t)data[i]) << (i * 8);
    }
    return val;
}

// ============================================================================
// API 实现
// ============================================================================

struct ORBMDK_ITM_Decoder* ORBMDK_ITM_Create(void)
{
    struct ORBMDK_ITM_Decoder* dec = new struct ORBMDK_ITM_Decoder;
    if (dec) {
        memset(dec, 0, sizeof(*dec));
        dec->state = ITM_STATE_UNSYNCED;
        dec->synced = false;
    }
    return dec;
}

void ORBMDK_ITM_Destroy(struct ORBMDK_ITM_Decoder* dec)
{
    if (dec) {
        delete dec;
    }
}

void ORBMDK_ITM_Init(struct ORBMDK_ITM_Decoder* dec)
{
    if (!dec) return;
    memset(&dec->stats, 0, sizeof(dec->stats));
    dec->state = ITM_STATE_UNSYNCED;
    dec->synced = false;
    dec->targetCount = 0;
    dec->len = 0;
    dec->lastType = ITM_PT_NONE;
    dec->packetType = ITM_PT_NONE;
    dec->srcAddr = 0;
    dec->lastSrcAddr = 0;
    dec->lastPC = 0;
    dec->lastSleeping = false;
    dec->lastByte = 0;
    dec->contextIDlen = 0;
    dec->syncStat = 0;
}

void ORBMDK_ITM_ForceSync(struct ORBMDK_ITM_Decoder* dec, bool isSynced)
{
    if (!dec) return;
    if (isSynced) {
        dec->state = ITM_STATE_IDLE;
        dec->synced = true;
        dec->len = 0;
        dec->syncStat = 0;
        dec->stats.syncCount++;
    } else {
        dec->state = ITM_STATE_UNSYNCED;
        dec->synced = false;
        dec->len = 0;
        dec->syncStat = 0;
        dec->stats.lostSyncCount++;
    }
}

bool ORBMDK_ITM_IsSynced(struct ORBMDK_ITM_Decoder* dec)
{
    return dec && dec->synced;
}

struct ORBMDK_ITM_Stats* ORBMDK_ITM_GetStats(struct ORBMDK_ITM_Decoder* dec)
{
    return dec ? &dec->stats : nullptr;
}

/**
 * @brief 获取 PC Sample 值 (从 HW 包数据中提取)
 * @return PC 值，如果不可用返回 false
 */
bool ORBMDK_ITM_GetPCSample(struct ORBMDK_ITM_Decoder* dec, uint32_t* pc, bool* sleeping)
{
    if (!dec || !pc || !sleeping) return false;
    
    // 检查是否是 HW 包且源地址为 PC Sample 端口
    if (dec->lastType == ITM_PT_HW && dec->lastSrcAddr == ITM_HW_PORT_PC_SAMPLE) {
        *pc = dec->lastPC;
        *sleeping = dec->lastSleeping;
        return true;
    }
    return false;
}

/**
 * @brief 泵入一个字节，返回事件
 *
 * 状态机与 orbuculum itmDecoder.c 对齐：
 *   - 同步: 连续 5 个 0x00 + 0x80 (48 位移位寄存器)
 *   - 源包 (SW/HW): 包头 bit[1:0] 编码负载长度 (1/2/4 字节)
 *   - 溢出包 0x70 / 时间戳 / 全局时间戳 / 扩展 / 新同步 / 保留包
 */
enum ORBMDK_ITM_PumpEvent ORBMDK_ITM_Pump(struct ORBMDK_ITM_Decoder* dec, uint8_t c)
{
    if (!dec) return ITM_EV_ERROR;

    enum ORBMDK_ITM_State newState = dec->state;
    enum ORBMDK_ITM_PumpEvent retVal = ITM_EV_NONE;

    // ---- 同步检测: 5 个 0x00 后跟 0x80 ----
    dec->syncStat = (dec->syncStat << 8) | c;
    if ((dec->syncStat & ITM_SYNC_MASK) == ITM_SYNC_PATTERN) {
        dec->stats.syncCount++;
        dec->synced = true;
        dec->len = 0;
        dec->lastByte = c;
        dec->state = ITM_STATE_IDLE;
        return ITM_EV_SYNCED;
    }

    switch (dec->state) {
    case ITM_STATE_UNSYNCED:
        // 未同步时丢弃所有数据
        retVal = ITM_EV_UNSYNCED;
        break;

    case ITM_STATE_IDLE:
        // 新包开始: 清空负载缓冲
        // (修复: 旧实现未重置 len，若上一包未及时取走就会与后续包叠加)
        memset(dec->data, 0, sizeof(dec->data));

        if (c == 0x00) {
            // 同步/填充字节
            break;
        }

        // ---- 源包 (SW/HW): bit[1:0] != 0 ----
        if (c & 0x03) {
            dec->targetCount = (c & 0x03);
            if (dec->targetCount == 3) {
                dec->targetCount = 4;  // 0b11 表示 4 字节
            }
            dec->len = 0;
            dec->srcAddr = (uint8_t)((c >> 3) & 0x1F);

            if (!(c & 0x04)) {
                // 软件激励包 (SW)
                dec->stats.SWPkt++;
                dec->packetType = ITM_PT_SW;
                newState = ITM_STATE_SW;
            } else {
                // 硬件包 (HW)
                // 注意: DWT PC Sample 的负载长度同样由包头 bit[1:0] 决定
                // (1 字节=睡眠指示 0x00, 4 字节=32 位 PC 值)，不能固定为 4 字节。
                dec->stats.HWPkt++;
                dec->packetType = ITM_PT_HW;
                newState = ITM_STATE_HW;
            }
            break;
        }

        // ---- 溢出包 (0x70) ----
        if (c == ITM_OVERFLOW_PACKET) {
            dec->stats.overflow++;
            retVal = ITM_EV_OVERFLOW;
            break;
        }

        // ---- 时间戳包 (bit[3:0] == 0) ----
        if ((c & 0x0F) == 0) {
            dec->len = 1;
            dec->data[0] = c;
            dec->stats.TSPkt++;
            dec->packetType = ITM_PT_TS;
            if (c & 0x80) {
                newState = ITM_STATE_TS;  // 格式 1: 后续还有字节
            } else {
                retVal = ITM_EV_PACKET_RXED;  // 格式 2: 单字节
            }
            break;
        }

        // ---- 全局时间戳包 (0b1001_0100 / 0b1011_0100) ----
        if ((c & 0xDF) == 0x94) {
            dec->packetType = ITM_PT_TS;
            newState = (c & 0x20) ? ITM_STATE_GTS2 : ITM_STATE_GTS1;
            break;
        }

        // ---- 新同步包 (0x08) ----
        if (c == 0x08) {
            dec->len = 0;
            dec->targetCount = ITM_MAX_PAYLOAD + dec->contextIDlen;
            dec->packetType = ITM_PT_NISYNC;
            newState = ITM_STATE_NISYNC;
            break;
        }

        // ---- 扩展包 (bit[3] == 1) ----
        if (c & 0x08) {
            dec->len = 1;
            dec->data[0] = c;
            dec->stats.XTNPkt++;
            dec->packetType = ITM_PT_XTN;
            if (!(c & 0x84)) {
                // 激励端口页寄存器设置，不是数据包
                dec->len = 0;
                dec->packetType = ITM_PT_NONE;
            } else {
                newState = ITM_STATE_XTN;
            }
            break;
        }

        // ---- 保留包 ----
        dec->len = 1;
        dec->data[0] = c;
        dec->stats.ReservedPkt++;
        dec->packetType = ITM_PT_RSRVD;
        if (!(c & 0x80)) {
            retVal = ITM_EV_PACKET_RXED;
        } else {
            newState = ITM_STATE_RSVD;
        }
        break;

    case ITM_STATE_GTS1:
    case ITM_STATE_GTS2:
        // 等待 continuation bit 变为 0
        if ((c & 0x80) == 0) {
            newState = ITM_STATE_IDLE;
        }
        break;

    case ITM_STATE_SW:
    case ITM_STATE_HW:
    case ITM_STATE_RECV:
        if (dec->len < (uint8_t)sizeof(dec->data)) {
            dec->data[dec->len++] = c;
        }
        if (dec->len >= dec->targetCount) {
            newState = ITM_STATE_IDLE;
            retVal = ITM_EV_PACKET_RXED;
        }
        break;

    case ITM_STATE_TS:
    case ITM_STATE_XTN:
    case ITM_STATE_RSVD:
        if (dec->len < (uint8_t)sizeof(dec->data)) {
            dec->data[dec->len++] = c;
        }
        if (!(c & 0x80) || dec->len >= ITM_MAX_PAYLOAD) {
            newState = ITM_STATE_IDLE;
            retVal = ITM_EV_PACKET_RXED;
        }
        break;

    case ITM_STATE_NISYNC:
        if (dec->len < (uint8_t)sizeof(dec->data)) {
            dec->data[dec->len++] = c;
        }
        if (dec->len > dec->targetCount) {
            newState = ITM_STATE_IDLE;
            retVal = ITM_EV_PACKET_RXED;
        }
        break;
    }

    dec->lastByte = c;
    dec->state = newState;
    return retVal;
}

/**
 * @brief 获取已解码的包
 */
bool ORBMDK_ITM_GetPacket(struct ORBMDK_ITM_Decoder* dec, struct ORBMDK_ITM_Packet* pkt)
{
    if (!dec || !pkt) return false;

    if (dec->len > 0) {
        pkt->type = dec->packetType;
        pkt->srcAddr = dec->srcAddr;
        pkt->len = dec->len;
        memcpy(pkt->d, dec->data, dec->len);
        
        // 特殊处理 PC Sample
        if (pkt->type == ITM_PT_HW && pkt->srcAddr == ITM_HW_PORT_PC_SAMPLE) {
            // 第一个字节判断：0x00 = 睡眠状态，1-4 = PC 值字节数
            if (pkt->len == 1 && pkt->d[0] == 0x00) {
                // 睡眠状态
                dec->lastSleeping = true;
                dec->lastPC = 0;
            } else {
                // PC 采样值 (小端序，1-4 字节)
                dec->lastSleeping = false;
                dec->lastPC = _extractValue(pkt->d, pkt->len);
            }
        }
        
        // 保存最后处理的包类型
        dec->lastType = pkt->type;
        dec->lastSrcAddr = pkt->srcAddr;
        
        // 清空缓冲区
        dec->len = 0;
        dec->packetType = ITM_PT_NONE;
        
        return true;
    }

    return false;
}
