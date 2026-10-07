/**
 * @file ORBMDK_Trace.cpp
 * @brief Trace 解码器适配层实现
 *
 * 使用独立实现的 ITM/TPIU/ETM 解码器
 */

#include "pch.h"
#include "ORBMDK.h"
#include "ORBMDK_Trace.h"
#include "ORBMDK_ITM_Decoder.h"
#include "ORBMDK_TPIU_Decoder.h"
#include "ORBMDK_ETM_Decoder.h"

// ============================================================================
// 内部上下文
// ============================================================================

struct ORBMDK_Trace_Context {
    enum ORBMDK_Trace_Protocol protocol;
    bool initialized;

    // 回调函数
    ORBMDK_Trace_ITM_CB itmCallback;
    ORBMDK_Trace_SWO_CB swoCallback;
    ORBMDK_Trace_TPIU_CB tpiuCallback;
    ORBMDK_Trace_Branch_CB branchCallback;
    void* userData;

    // 内部解码器
    union {
        struct ORBMDK_ITM_Decoder* itm;
        struct ORBMDK_TPIU_Decoder* tpiu;
        struct ORBMDK_ETM_Decoder* etm;
    } decoder;

    // 统计
    struct ORBMDK_Trace_Stats stats;

    // 临时包缓冲
    uint8_t srcAddr;
    uint8_t packetData[64];
};

// ============================================================================
// 回调转发
// ============================================================================

static void _itmBranchCallback(const struct ORBMDK_ETM_Branch* branch, void* param)
{
    (void)branch;
    (void)param;
    // ITM 不产生分支
}

// ============================================================================
// API 实现
// ============================================================================

ORBMDK_Trace_Handle ORBMDK_Trace_Create(enum ORBMDK_Trace_Protocol protocol)
{
    struct ORBMDK_Trace_Context* ctx = new struct ORBMDK_Trace_Context;
    if (!ctx) return nullptr;

    memset(ctx, 0, sizeof(*ctx));
    ctx->protocol = protocol;
    ctx->initialized = false;

    switch (protocol) {
    case TRACE_PROT_ITM:
        ctx->decoder.itm = ORBMDK_ITM_Create();
        if (ctx->decoder.itm) {
            ORBMDK_ITM_Init(ctx->decoder.itm);
            ctx->initialized = true;
        }
        break;

    case TRACE_PROT_TPIU:
        ctx->decoder.tpiu = ORBMDK_TPIU_Create();
        if (ctx->decoder.tpiu) {
            ORBMDK_TPIU_Init(ctx->decoder.tpiu);
            ctx->initialized = true;
        }
        break;

    case TRACE_PROT_SWO_UART:
    case TRACE_PROT_SWO_MANCHESTER:
        ctx->initialized = true;
        break;

    case TRACE_PROT_ETM35:
        ctx->decoder.etm = ORBMDK_ETM_Create(ETM_PV_ETM35);
        if (ctx->decoder.etm) {
            ORBMDK_ETM_Init(ctx->decoder.etm);
            ORBMDK_ETM_SetBranchCallback(ctx->decoder.etm, nullptr, nullptr);
            ctx->initialized = true;
        }
        break;

    case TRACE_PROT_ETM4:
        ctx->decoder.etm = ORBMDK_ETM_Create(ETM_PV_ETM4);
        if (ctx->decoder.etm) {
            ORBMDK_ETM_Init(ctx->decoder.etm);
            ORBMDK_ETM_SetBranchCallback(ctx->decoder.etm, nullptr, nullptr);
            ctx->initialized = true;
        }
        break;

    case TRACE_PROT_MTB:
        ctx->initialized = true;
        break;

    default:
        break;
    }

    if (!ctx->initialized) {
        delete ctx;
        return nullptr;
    }

    return (ORBMDK_Trace_Handle)ctx;
}

void ORBMDK_Trace_Destroy(ORBMDK_Trace_Handle handle)
{
    if (!handle) return;

    struct ORBMDK_Trace_Context* ctx = (struct ORBMDK_Trace_Context*)handle;

    if (ctx->decoder.itm) {
        ORBMDK_ITM_Destroy(ctx->decoder.itm);
        ctx->decoder.itm = nullptr;
    }
    if (ctx->decoder.tpiu) {
        ORBMDK_TPIU_Destroy(ctx->decoder.tpiu);
        ctx->decoder.tpiu = nullptr;
    }
    if (ctx->decoder.etm) {
        ORBMDK_ETM_Destroy(ctx->decoder.etm);
        ctx->decoder.etm = nullptr;
    }

    delete ctx;
}

void ORBMDK_Trace_Init(ORBMDK_Trace_Handle handle, enum ORBMDK_Trace_Protocol protocol)
{
    if (!handle) return;

    struct ORBMDK_Trace_Context* ctx = (struct ORBMDK_Trace_Context*)handle;
    
    // 销毁旧的解码器
    if (ctx->decoder.itm) ORBMDK_ITM_Destroy(ctx->decoder.itm);
    if (ctx->decoder.tpiu) ORBMDK_TPIU_Destroy(ctx->decoder.tpiu);
    if (ctx->decoder.etm) ORBMDK_ETM_Destroy(ctx->decoder.etm);
    
    // 重新创建
    ctx->protocol = protocol;
    ctx->initialized = false;

    switch (protocol) {
    case TRACE_PROT_ITM:
        ctx->decoder.itm = ORBMDK_ITM_Create();
        if (ctx->decoder.itm) {
            ORBMDK_ITM_Init(ctx->decoder.itm);
            ctx->initialized = true;
        }
        break;

    case TRACE_PROT_TPIU:
        ctx->decoder.tpiu = ORBMDK_TPIU_Create();
        if (ctx->decoder.tpiu) {
            ORBMDK_TPIU_Init(ctx->decoder.tpiu);
            ctx->initialized = true;
        }
        break;

    case TRACE_PROT_ETM35:
        ctx->decoder.etm = ORBMDK_ETM_Create(ETM_PV_ETM35);
        if (ctx->decoder.etm) {
            ORBMDK_ETM_Init(ctx->decoder.etm);
            ctx->initialized = true;
        }
        break;

    case TRACE_PROT_ETM4:
        ctx->decoder.etm = ORBMDK_ETM_Create(ETM_PV_ETM4);
        if (ctx->decoder.etm) {
            ORBMDK_ETM_Init(ctx->decoder.etm);
            ctx->initialized = true;
        }
        break;

    default:
        ctx->initialized = true;
        break;
    }
}

int ORBMDK_Trace_Pump(ORBMDK_Trace_Handle handle, const uint8_t* data, size_t len)
{
    if (!handle || !data || len == 0) return TRACE_EV_ERROR;

    struct ORBMDK_Trace_Context* ctx = (struct ORBMDK_Trace_Context*)handle;

    switch (ctx->protocol) {
    case TRACE_PROT_ITM:
        if (ctx->decoder.itm) {
            for (size_t i = 0; i < len; i++) {
                enum ORBMDK_ITM_PumpEvent ret = ORBMDK_ITM_Pump(ctx->decoder.itm, data[i]);
                
                if (ret == ITM_EV_PACKET_RXED) {
                    struct ORBMDK_ITM_Packet pkt;
                    if (ORBMDK_ITM_GetPacket(ctx->decoder.itm, &pkt)) {
                        if (ctx->itmCallback && pkt.len > 0) {
                            ctx->itmCallback(pkt.srcAddr, pkt.d, pkt.len, ctx->userData);
                        }
                    }
                } else if (ret == ITM_EV_SYNCED) {
                    return TRACE_EV_SYNCED;
                } else if (ret == ITM_EV_UNSYNCED) {
                    return TRACE_EV_UNSYNCED;
                }
            }
        }
        break;

    case TRACE_PROT_TPIU:
        if (ctx->decoder.tpiu) {
            for (size_t i = 0; i < len; i++) {
                enum ORBMDK_TPIU_PumpEvent ret = ORBMDK_TPIU_Pump(ctx->decoder.tpiu, data[i]);
                
                if (ret == TPIU_EV_RXEDPACKET) {
                    struct ORBMDK_TPIU_Packet pkt;
                    if (ORBMDK_TPIU_GetPacket(ctx->decoder.tpiu, &pkt) && ctx->tpiuCallback) {
                        ctx->tpiuCallback(pkt.data, pkt.len, ctx->userData);
                    }
                } else if (ret == TPIU_EV_SYNCED) {
                    return TRACE_EV_SYNCED;
                }
            }
        }
        break;

    case TRACE_PROT_SWO_UART:
    case TRACE_PROT_SWO_MANCHESTER:
        for (size_t i = 0; i < len; i++) {
            if (ctx->swoCallback) {
                ctx->swoCallback(data[i], ctx->userData);
            }
        }
        return TRACE_EV_NONE;

    case TRACE_PROT_ETM35:
    case TRACE_PROT_ETM4:
        if (ctx->decoder.etm) {
            ORBMDK_ETM_Pump(ctx->decoder.etm, data, len);
        }
        break;

    default:
        break;
    }

    return TRACE_EV_NONE;
}

struct ORBMDK_Trace_Stats* ORBMDK_Trace_GetStats(ORBMDK_Trace_Handle handle)
{
    if (!handle) return nullptr;

    struct ORBMDK_Trace_Context* ctx = (struct ORBMDK_Trace_Context*)handle;
    memset(&ctx->stats, 0, sizeof(ctx->stats));

    if (ctx->decoder.itm) {
        struct ORBMDK_ITM_Stats* s = ORBMDK_ITM_GetStats(ctx->decoder.itm);
        if (s) {
            ctx->stats.lostSyncCount = s->lostSyncCount;
            ctx->stats.syncCount = s->syncCount;
            ctx->stats.overflow = s->overflow;
            ctx->stats.error = s->ReservedPkt + s->ErrorPkt;
        }
    } else if (ctx->decoder.tpiu) {
        struct ORBMDK_TPIU_Stats* s = ORBMDK_TPIU_GetStats(ctx->decoder.tpiu);
        if (s) {
            ctx->stats.lostSyncCount = s->lostSync;
            ctx->stats.syncCount = s->syncCount;
        }
    } else if (ctx->decoder.etm) {
        struct ORBMDK_ETM_Stats* s = ORBMDK_ETM_GetStats(ctx->decoder.etm);
        if (s) {
            ctx->stats.lostSyncCount = s->lostSyncCount;
            ctx->stats.syncCount = s->syncCount;
        }
    }

    return &ctx->stats;
}

bool ORBMDK_Trace_IsSynced(ORBMDK_Trace_Handle handle)
{
    if (!handle) return false;

    struct ORBMDK_Trace_Context* ctx = (struct ORBMDK_Trace_Context*)handle;

    if (ctx->decoder.itm) {
        return ORBMDK_ITM_IsSynced(ctx->decoder.itm);
    } else if (ctx->decoder.tpiu) {
        return ORBMDK_TPIU_IsSynced(ctx->decoder.tpiu);
    } else if (ctx->decoder.etm) {
        return ORBMDK_ETM_IsSynced(ctx->decoder.etm);
    }

    return false;
}

void ORBMDK_Trace_ForceSync(ORBMDK_Trace_Handle handle, bool isSynced)
{
    if (!handle) return;

    struct ORBMDK_Trace_Context* ctx = (struct ORBMDK_Trace_Context*)handle;

    if (ctx->decoder.itm) {
        ORBMDK_ITM_ForceSync(ctx->decoder.itm, isSynced);
    } else if (ctx->decoder.tpiu) {
        ORBMDK_TPIU_ForceSync(ctx->decoder.tpiu);
    } else if (ctx->decoder.etm) {
        ORBMDK_ETM_ForceSync(ctx->decoder.etm, isSynced);
    }
}

struct ORBMDK_Trace_CPUState* ORBMDK_Trace_GetCPUState(ORBMDK_Trace_Handle handle)
{
    (void)handle;
    static struct ORBMDK_Trace_CPUState state = {};
    return &state;
}

// ============================================================================
// ITM 特定 API
// ============================================================================

void ORBMDK_Trace_SetITMCallback(ORBMDK_Trace_Handle handle, ORBMDK_Trace_ITM_CB callback, void* userData)
{
    if (!handle) return;

    struct ORBMDK_Trace_Context* ctx = (struct ORBMDK_Trace_Context*)handle;
    ctx->itmCallback = callback;
    ctx->userData = userData;
}

size_t ORBMDK_Trace_ITM_GetPacket(ORBMDK_Trace_Handle handle, uint8_t* srcAddr, uint8_t* data, size_t maxLen)
{
    if (!handle || !data || maxLen == 0) return 0;

    struct ORBMDK_Trace_Context* ctx = (struct ORBMDK_Trace_Context*)handle;

    if (!ctx->decoder.itm) return 0;

    struct ORBMDK_ITM_Packet pkt;
    if (ORBMDK_ITM_GetPacket(ctx->decoder.itm, &pkt)) {
        if (srcAddr) *srcAddr = pkt.srcAddr;
        size_t copyLen = (pkt.len < maxLen) ? pkt.len : maxLen;
        memcpy(data, pkt.d, copyLen);
        return copyLen;
    }

    return 0;
}

// ============================================================================
// SWO 特定 API
// ============================================================================

void ORBMDK_Trace_SetSWOCallback(ORBMDK_Trace_Handle handle, ORBMDK_Trace_SWO_CB callback, void* userData)
{
    if (!handle) return;

    struct ORBMDK_Trace_Context* ctx = (struct ORBMDK_Trace_Context*)handle;
    ctx->swoCallback = callback;
    ctx->userData = userData;
}

// ============================================================================
// TPIU 特定 API
// ============================================================================

void ORBMDK_Trace_SetTPIUCallback(ORBMDK_Trace_Handle handle, ORBMDK_Trace_TPIU_CB callback, void* userData)
{
    if (!handle) return;

    struct ORBMDK_Trace_Context* ctx = (struct ORBMDK_Trace_Context*)handle;
    ctx->tpiuCallback = callback;
    ctx->userData = userData;
}

size_t ORBMDK_Trace_TPIU_GetPacket(ORBMDK_Trace_Handle handle, uint8_t* data, size_t maxLen)
{
    if (!handle || !data || maxLen == 0) return 0;

    struct ORBMDK_Trace_Context* ctx = (struct ORBMDK_Trace_Context*)handle;

    if (!ctx->decoder.tpiu) return 0;

    struct ORBMDK_TPIU_Packet pkt;
    if (ORBMDK_TPIU_GetPacket(ctx->decoder.tpiu, &pkt)) {
        size_t copyLen = (pkt.len < maxLen) ? pkt.len : maxLen;
        memcpy(data, pkt.data, copyLen);
        return copyLen;
    }

    return 0;
}

// ============================================================================
// ETM 特定 API
// ============================================================================

void ORBMDK_Trace_SetBranchCallback(ORBMDK_Trace_Handle handle, ORBMDK_Trace_Branch_CB callback, void* userData)
{
    if (!handle) return;

    struct ORBMDK_Trace_Context* ctx = (struct ORBMDK_Trace_Context*)handle;
    ctx->branchCallback = callback;
    ctx->userData = userData;

    if (ctx->decoder.etm && callback) {
        ORBMDK_ETM_SetBranchCallback(ctx->decoder.etm, 
            [](const struct ORBMDK_ETM_Branch* branch, void* userData) {
                struct ORBMDK_Trace_Context* ctx = (struct ORBMDK_Trace_Context*)userData;
                if (ctx->branchCallback) {
                    ctx->branchCallback(branch->fromAddr, branch->toAddr, nullptr, ctx->userData);
                }
            }, ctx);
    }
}

void ORBMDK_Trace_SetAltAddrEncode(ORBMDK_Trace_Handle handle, bool usingAlt)
{
    (void)handle;
    (void)usingAlt;
}

void ORBMDK_Trace_SWO_SetBaud(ORBMDK_Trace_Handle handle, uint32_t clock, uint32_t baud)
{
    (void)handle;
    (void)clock;
    (void)baud;
}
