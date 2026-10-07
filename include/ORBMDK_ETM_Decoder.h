/**
 * @file ORBMDK_ETM_Decoder.h
 * @brief ETM (Embedded Trace Macrocell) 解码器
 *
 * 独立实现的 ETM v3.5/v4 协议解码器，兼容 ARM ETM 规范
 */

#pragma once

#include <cstdint>
#include <cstdbool>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// ETM 协议定义
// ============================================================================

// ETM 协议版本
enum ORBMDK_ETM_Version {
    ETM_PV_NONE = 0,
    ETM_PV_ETM35 = 3,   // ETM v3.5
    ETM_PV_ETM4 = 4,    // ETM v4
    ETM_PV_ETM4P1 = 5,  // ETM v4.1+
    ETM_PV_ETM4P2 = 6   // ETM v4.2+
};

// ETM 状态
enum ORBMDK_ETM_State {
    ETM_STATE_UNSYNCED,
    ETM_STATE_IDLE,
    ETM_STATE_COLLECTING
};

// ETM 分支类型
enum ORBMDK_ETM_BranchType {
    ETM_BRANCH_NORMAL,      // 正常分支
    ETM_BRANCH_INDIRECT,    // 间接分支
    ETM_BRANCH_EXCEPTION,   // 异常入口
    ETM_BRANCH_RETURN       // 函数返回
};

// ETM 分支信息
struct ORBMDK_ETM_Branch {
    uint32_t fromAddr;       // 源地址
    uint32_t toAddr;         // 目标地址
    enum ORBMDK_ETM_BranchType type;
    uint64_t timestamp;
    bool isConditional;
    bool isTaken;
};

// ETM CPU 状态
struct ORBMDK_ETM_CPUState {
    uint32_t currentAddr;
    uint32_t prevAddr;
    uint64_t cycleCount;
    uint8_t contextID;
    bool exceptionPending;
};

// ETM 解码器统计
struct ORBMDK_ETM_Stats {
    uint32_t lostSyncCount;
    uint32_t syncCount;
    uint32_t branchCount;
    uint32_t exceptionCount;
    uint32_t contextCount;
    uint32_t instructionCount;
    uint32_t atomCount;
    uint32_t overflow;
    uint32_t error;
};

// ETM 解码器
struct ORBMDK_ETM_Decoder {
    enum ORBMDK_ETM_Version version;
    enum ORBMDK_ETM_State state;

    // 地址信息
    uint32_t currentAddr;
    uint32_t prevAddr;
    uint32_t nextAddr;
    uint8_t addrInc;
    uint8_t addrBytes;        // ISYNC/地址编码的地址字节数 (默认 4 = 32-bit)

    // 上下文
    uint32_t contextID;
    uint8_t contextIDlen;

    // 周期计数
    uint64_t cycleCount;
    uint64_t cycleDelta;

    // 状态
    bool exceptionPending;
    bool inDelaySlot;
    bool isThumb;
    bool usingAltAddrEncode;  // 备选地址编码

    // A-SYNC 检测
    uint8_t asyncCount;

    // 统计
    struct ORBMDK_ETM_Stats stats;

    // 回调函数
    void (*branchCallback)(const struct ORBMDK_ETM_Branch* branch, void* userData);
    void (*syncCallback)(uint32_t addr, void* userData);
    void (*exceptionCallback)(uint8_t type, uint16_t number, void* userData);
    void* userData;
};

// ============================================================================
// ETM 解码器 API
// ============================================================================

struct ORBMDK_ETM_Decoder* ORBMDK_ETM_Create(enum ORBMDK_ETM_Version version);
void ORBMDK_ETM_Destroy(struct ORBMDK_ETM_Decoder* dec);
void ORBMDK_ETM_Init(struct ORBMDK_ETM_Decoder* dec);

// 回调设置
void ORBMDK_ETM_SetBranchCallback(struct ORBMDK_ETM_Decoder* dec, 
    void (*callback)(const struct ORBMDK_ETM_Branch* branch, void* userData), 
    void* userData);
void ORBMDK_ETM_SetSyncCallback(struct ORBMDK_ETM_Decoder* dec,
    void (*callback)(uint32_t addr, void* userData),
    void* userData);
void ORBMDK_ETM_SetExceptionCallback(struct ORBMDK_ETM_Decoder* dec,
    void (*callback)(uint8_t type, uint16_t number, void* userData),
    void* userData);

// 状态查询
void ORBMDK_ETM_ForceSync(struct ORBMDK_ETM_Decoder* dec, bool synced);
bool ORBMDK_ETM_IsSynced(struct ORBMDK_ETM_Decoder* dec);
uint32_t ORBMDK_ETM_GetCurrentAddress(struct ORBMDK_ETM_Decoder* dec);
uint64_t ORBMDK_ETM_GetCycleCount(struct ORBMDK_ETM_Decoder* dec);
struct ORBMDK_ETM_Stats* ORBMDK_ETM_GetStats(struct ORBMDK_ETM_Decoder* dec);

// 数据处理
// 返回: 0=成功, -1=错误, 正值=需要更多字节
int ORBMDK_ETM_Pump(struct ORBMDK_ETM_Decoder* dec, const uint8_t* data, size_t len);

// 地址编码控制
void ORBMDK_ETM_SetAltAddrEncode(struct ORBMDK_ETM_Decoder* dec, bool altEncode);
bool ORBMDK_ETM_GetAltAddrEncode(struct ORBMDK_ETM_Decoder* dec);

#ifdef __cplusplus
}
#endif
