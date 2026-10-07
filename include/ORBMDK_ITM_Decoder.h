/**
 * @file ORBMDK_ITM_Decoder.h
 * @brief ITM (Instrumentation Trace Macrocell) 解码器
 *
 * 独立实现的 ITM 协议解码器，兼容 ARM ITM 规范
 */

#pragma once

#include <cstdint>
#include <cstdbool>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// ITM 协议定义
// ============================================================================

// ITM 包类型
enum ORBMDK_ITM_PacketType {
    ITM_PT_NONE = 0,
    ITM_PT_TS,        // 时间戳包
    ITM_PT_SW,        // 软件生成包
    ITM_PT_HW,        // 硬件生成包
    ITM_PT_XTN,       // 延期同步包
    ITM_PT_RSRVD,     // 保留
    ITM_PT_NISYNC     // 新同步包
};

// ITM 泵事件
enum ORBMDK_ITM_PumpEvent {
    ITM_EV_NONE = 0,
    ITM_EV_PACKET_RXED,
    ITM_EV_UNSYNCED,
    ITM_EV_SYNCED,
    ITM_EV_OVERFLOW,
    ITM_EV_ERROR
};

// ITM 包
struct ORBMDK_ITM_Packet {
    enum ORBMDK_ITM_PacketType type;
    uint8_t srcAddr;
    uint8_t len;
    uint8_t d[12];  // 最大 12 字节
};

// ITM 解码器统计
struct ORBMDK_ITM_Stats {
    uint32_t lostSyncCount;
    uint32_t syncCount;
    uint32_t overflow;
    uint32_t SWPkt;
    uint32_t TSPkt;
    uint32_t HWPkt;
    uint32_t XTNPkt;
    uint32_t ReservedPkt;
    uint32_t ErrorPkt;
};

// ITM 解码器状态机
enum ORBMDK_ITM_State {
    ITM_STATE_UNSYNCED,
    ITM_STATE_IDLE,
    ITM_STATE_RECV,    // 正在接收数据 (保留，等价于 SW/HW)
    ITM_STATE_SW,      // 正在接收 SW(激励) 包数据
    ITM_STATE_HW,      // 正在接收 HW(DWT) 包数据
    ITM_STATE_TS,      // 时间戳 (可变长)
    ITM_STATE_XTN,     // 扩展包 (可变长)
    ITM_STATE_RSVD,    // 保留包 (可变长)
    ITM_STATE_NISYNC,  // 新同步包
    ITM_STATE_GTS1,    // 全局时间戳 1
    ITM_STATE_GTS2     // 全局时间戳 2
};

// ITM 解码器
struct ORBMDK_ITM_Decoder {
    enum ORBMDK_ITM_State state;
    uint8_t contextIDlen;
    int targetCount;
    uint8_t srcAddr;
    uint8_t len;
    uint8_t data[12];
    struct ORBMDK_ITM_Stats stats;
    bool synced;

    // 内部状态
    uint64_t syncStat;                 // 同步移位寄存器 (检测 5x0x00 + 0x80)
    uint8_t lastByte;                  // 上一个字节
    enum ORBMDK_ITM_PacketType packetType;  // 当前包类型
    enum ORBMDK_ITM_PacketType lastType;    // 上一个包类型
    uint8_t lastSrcAddr;               // 上一个源地址
    uint32_t lastPC;                   // 上一个 PC 采样值
    bool lastSleeping;                 // 上一个是否为睡眠状态
};

// ============================================================================
// ITM 解码器 API
// ============================================================================

struct ORBMDK_ITM_Decoder* ORBMDK_ITM_Create(void);
void ORBMDK_ITM_Destroy(struct ORBMDK_ITM_Decoder* dec);
void ORBMDK_ITM_Init(struct ORBMDK_ITM_Decoder* dec);
void ORBMDK_ITM_ForceSync(struct ORBMDK_ITM_Decoder* dec, bool isSynced);
bool ORBMDK_ITM_IsSynced(struct ORBMDK_ITM_Decoder* dec);
struct ORBMDK_ITM_Stats* ORBMDK_ITM_GetStats(struct ORBMDK_ITM_Decoder* dec);

// 泵入一个字节，返回事件
enum ORBMDK_ITM_PumpEvent ORBMDK_ITM_Pump(struct ORBMDK_ITM_Decoder* dec, uint8_t c);

// 获取已解码的包
bool ORBMDK_ITM_GetPacket(struct ORBMDK_ITM_Decoder* dec, struct ORBMDK_ITM_Packet* pkt);

// 获取 PC Sample 值 (从 HW 包中提取)
bool ORBMDK_ITM_GetPCSample(struct ORBMDK_ITM_Decoder* dec, uint32_t* pc, bool* sleeping);

#ifdef __cplusplus
}
#endif
