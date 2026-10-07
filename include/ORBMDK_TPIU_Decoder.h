/**
 * @file ORBMDK_TPIU_Decoder.h
 * @brief TPIU (Trace Port Interface Unit) 解码器
 *
 * 独立实现的 TPIU 协议解码器，兼容 ARM TPIU 规范
 */

#pragma once

#include <cstdint>
#include <cstdbool>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// TPIU 协议定义
// ============================================================================

// TPIU 包格式
// 0x00000000 = 空闲/同步
// 0x0000007X = 异步时钟同步 (X=byte数-1)
// 0xXXXXXXXX = 数据

// TPIU 帧头标识
#define TPIU_FRAME_SYNC    0x0000007F  // 7 字节同步
#define TPIU_FRAME_MIN     0x00000001  // 最小帧

// TPIU 泵事件
enum ORBMDK_TPIU_PumpEvent {
    TPIU_EV_NONE = 0,
    TPIU_EV_UNSYNCED,
    TPIU_EV_SYNCED,
    TPIU_EV_NEWSYNC,
    TPIU_EV_RXING,
    TPIU_EV_RXEDPACKET,
    TPIU_EV_ERROR
};

// TPIU 解码器统计
struct ORBMDK_TPIU_Stats {
    uint32_t lostSync;
    uint32_t syncCount;
    uint32_t packets;
    uint32_t bytes;
    uint32_t overflow;
    uint32_t error;
};

// TPIU 解码器状态机
enum ORBMDK_TPIU_State {
    TPIU_STATE_UNSYNCED,
    TPIU_STATE_SYNCING,
    TPIU_STATE_FRAME,
    TPIU_STATE_PAYLOAD
};

// TPIU 包
struct ORBMDK_TPIU_Packet {
    uint8_t len;
    uint8_t data[256];
};

// TPIU 解码器
struct ORBMDK_TPIU_Decoder {
    enum ORBMDK_TPIU_State state;
    uint8_t frameLen;
    uint8_t payloadLen;
    uint8_t payload[256];
    uint8_t syncCount;
    struct ORBMDK_TPIU_Stats stats;
    bool synced;
    bool packetReady;   // 已组装好一个完整包，等待 ORBMDK_TPIU_GetPacket 取走
};

// ============================================================================
// TPIU 解码器 API
// ============================================================================

struct ORBMDK_TPIU_Decoder* ORBMDK_TPIU_Create(void);
void ORBMDK_TPIU_Destroy(struct ORBMDK_TPIU_Decoder* dec);
void ORBMDK_TPIU_Init(struct ORBMDK_TPIU_Decoder* dec);
void ORBMDK_TPIU_ForceSync(struct ORBMDK_TPIU_Decoder* dec);
bool ORBMDK_TPIU_IsSynced(struct ORBMDK_TPIU_Decoder* dec);
struct ORBMDK_TPIU_Stats* ORBMDK_TPIU_GetStats(struct ORBMDK_TPIU_Decoder* dec);

// 泵入一个字节，返回事件
enum ORBMDK_TPIU_PumpEvent ORBMDK_TPIU_Pump(struct ORBMDK_TPIU_Decoder* dec, uint8_t c);

// 获取已解码的包
bool ORBMDK_TPIU_GetPacket(struct ORBMDK_TPIU_Decoder* dec, struct ORBMDK_TPIU_Packet* pkt);

#ifdef __cplusplus
}
#endif
