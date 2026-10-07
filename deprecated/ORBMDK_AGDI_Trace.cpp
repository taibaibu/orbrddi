/**
 * @file ORBMDK_AGDI_Trace.cpp
 * @brief AGDI Trace 接口实现
 *
 * 提供 ETM/ITM/SWO trace 功能的 AGDI 接口
 */

#include "pch.h"
#include "ORBMDK.h"
#include "ORBMDK_RDDI.h"
#include "ORBMDK_AGDI.h"
#include "ORBMDK_AGDI_Trace.h"
#include "ORBMDK_Trace.h"
#include "ORBMDK_USB_Bulk.h"

// ============================================================================
// 内部常量
// ============================================================================

// CoreSight 调试组件基地址
#define CS_BASE               0xE0000000

// ITM (Instrumentation Trace Macrocell)
#define ITM_BASE              (CS_BASE + 0x0000)
#define ITM_LOCKACCESS        (ITM_BASE + 0x0DF0)
#define ITM_TER               (ITM_BASE + 0x0E00)   // Trace Enable Register
#define ITM_TPR               (ITM_BASE + 0x0E40)   // Trace Privilege Register
#define ITM_TCR               (ITM_BASE + 0x0E80)   // Trace Control Register
#define ITM_BASE_ADDR         (ITM_BASE + 0x0F00)   // ITM Stimulus Base

// DWT (Data Watchpoint and Trace)
#define DWT_BASE              (CS_BASE + 0x1000)
#define DWT_CTRL              (DWT_BASE + 0x0000)   // Control Register
#define DWT_CYCCNT            (DWT_BASE + 0x0004)   // Cycle Counter
#define DWT_TFCR              (DWT_BASE + 0x0008)   // Trace FIFO Control Register
#define DWT_PCSR              (DWT_BASE + 0x0010)   // PC Sample Register
#define DWT_EXCCNT            (DWT_BASE + 0x0014)   // Exception Count
#define DWT_CNTCTRL           (DWT_BASE + 0x0020)   // Count Control
#define DWT_CYCMATCH          (DWT_BASE + 0x0030)   // Cycle Match
#define DWT_LPCNT             (DWT_BASE + 0x0064)   // Loop Counter

// TPIU (Trace Port Interface Unit)
#define TPIU_BASE             (CS_BASE + 0x4000)
#define TPIU_SSPSR            (TPIU_BASE + 0x0000)   // Supported Parallel Port Sizes
#define TPIU_CSPSR            (TPIU_BASE + 0x0004)   // Current Parallel Port Size
#define TPIU_SPPR             (TPIU_BASE + 0x0000)   // Selected Pin Protocol (偏移0x000,复用)
#define TPIU_FFCR             (TPIU_BASE + 0x0304)   // Formatter/Flush Control
#define TPIU_PSCR             (TPIU_BASE + 0x0004)   // Async Clock Prescaler (偏移0x004,复用)

// ETM (Embedded Trace Macrocell) - 可能在不同的基地址
#define ETM_BASE              (CS_BASE + 0x3000)
#define ETM_CR                (ETM_BASE + 0x0000)   // Control Register
#define ETM_TRIGGER           (ETM_BASE + 0x000C)   // Trigger Event Register
#define ETM_TEEVR             (ETM_BASE + 0x0020)   // Trace Enable Event
#define ETM_TECR1             (ETM_BASE + 0x0024)   // Trace Enable Control 1
#define ETM_FFRR             (ETM_BASE + 0x0028)   // FIFO Flush Request

// SWO 寄存器 (在 TPIU 内)
#define SWO_SPPR              TPIU_SPPR
#define SWO_TXCTL             (CS_BASE + 0x5000)   // SWO Transmit Control
#define SWO_TX2               (CS_BASE + 0x5004)   // SWO Transmit Data 2

// ============================================================================
// 前向声明 (内部函数)
// ============================================================================
static int _SWO_Config(enum ORBMDK_SWO_Mode mode, uint32_t baudRate);
static int _SWO_Start(void);
static int _SWO_Stop(void);
static int _ETM_Config(bool enabled);
static int _ETM_Start(void);
static int _ETM_Stop(void);
static int _ITM_Config(uint32_t portMask);
static int _ITM_Start(void);
static int _ITM_Stop(void);
static int _ITM_Write(uint8_t port, const uint8_t* data, size_t len);
static int _TPIU_Config(uint32_t traceClock, enum ORBMDK_Trace_Width traceWidth);
static int _TPIU_Start(void);
static int _TPIU_Stop(void);

// SWO 波特率分频表 (基于 1MHz 时钟)
static const struct {
    uint32_t baud;
    uint8_t prescaler;
} SWO_BAUD_TABLE[] = {
    { 9600,   104 },
    { 19200,  52  },
    { 57600,  17  },
    { 115200, 8   },
    { 230400, 4   },
    { 460800, 2   },
    { 921600, 1   },
    { 0, 0 }
};

// ============================================================================
// 内部状态
// ============================================================================

static bool g_initialized = false;
static int g_dapIndex = 0;

static struct {
    bool swoEnabled;
    bool etmEnabled;
    bool itmEnabled;
    bool tpiuEnabled;
    enum ORBMDK_SWO_Mode swoMode;
    uint32_t swoBaudRate;
    uint32_t itmPortMask;
    uint32_t traceClock;
    enum ORBMDK_Trace_Width traceWidth;
} g_traceConfig = {};

static struct {
    uint32_t swoBytesReceived;
    uint32_t itmPacketsReceived;
    uint32_t etmPacketsReceived;
    uint32_t overflowCount;
    uint32_t errorCount;
} g_traceStats = {};

static ORBMDK_AGDI_Trace_Data_CB g_dataCallback = nullptr;
static void* g_userData = nullptr;

// Trace 解码器
static void* g_swoDecoder = nullptr;
static void* g_itmDecoder = nullptr;
static void* g_etmDecoder = nullptr;
static void* g_tpiuDecoder = nullptr;

// ETM 解码器支持
#include "ORBMDK_ETM_Decoder.h"
static struct ORBMDK_ETM_Decoder* g_etm = nullptr;

// ETM 分支回调
static void _etmBranchCallback(const struct ORBMDK_ETM_Branch* branch, void* userData)
{
    (void)userData;
    if (branch && g_dataCallback) {
        g_traceStats.etmPacketsReceived++;
        // 发送分支事件
        uint8_t data[8];
        data[0] = (uint8_t)(branch->toAddr & 0xFF);
        data[1] = (uint8_t)((branch->toAddr >> 8) & 0xFF);
        data[2] = (uint8_t)((branch->toAddr >> 16) & 0xFF);
        data[3] = (uint8_t)((branch->toAddr >> 24) & 0xFF);
        g_dataCallback(AGDI_TRACE_ETM_START, data, 4, branch->timestamp, g_userData);
    }
}

// ETM 同步回调
static void _etmSyncCallback(uint32_t addr, void* userData)
{
    (void)userData;
    (void)addr;
    g_traceStats.etmPacketsReceived++;
}

// ETM 异常回调
static void _etmExceptionCallback(uint8_t type, uint16_t exceptionNumber, void* userData)
{
    (void)userData;
    (void)type;
    (void)exceptionNumber;
    g_traceStats.errorCount++;
}

// ============================================================================
// 内部函数
// ============================================================================

/**
 * @brief 查找 SWO 波特率分频值
 */
static uint8_t _findSWOPrescaler(uint32_t baudRate)
{
    for (int i = 0; SWO_BAUD_TABLE[i].baud != 0; i++) {
        if (SWO_BAUD_TABLE[i].baud == baudRate) {
            return SWO_BAUD_TABLE[i].prescaler;
        }
    }
    return 1;  // 默认 1MHz / 1 = 1MHz (最高)
}

/**
 * @brief 写入 ARM CoreSight 寄存器 (通过 AP)
 */
static int _writeRegister(uint32_t addr, uint32_t value)
{
    int regId = (int)((addr & 0x0F) << 24) | DAP_REG_AP_DRW;
    return DAP_WriteReg(nullptr, g_dapIndex, regId, (int)value);
}

/**
 * @brief 读取 ARM CoreSight 寄存器 (通过 AP)
 */
static int _readRegister(uint32_t addr, uint32_t* value)
{
    int regId = (int)((addr & 0x0F) << 24) | DAP_REG_AP_DRW;
    int val;
    int ret = DAP_ReadReg(nullptr, g_dapIndex, regId, &val);
    if (value) *value = (uint32_t)val;
    return ret;
}

/**
 * @brief 禁用跟踪单元
 */
static void _disableTraceUnit(void)
{
    // 禁用 ITM
    _writeRegister(ITM_TCR, 0);
    _writeRegister(ITM_TER, 0);
    
    // 禁用 DWT/SWO
    _writeRegister(DWT_CTRL, 0);
    _writeRegister(DWT_TFCR, 0);
}

/**
 * @brief SWO 数据回调
 */
static void _swoCallback(uint8_t byte, void* userData)
{
    (void)userData;
    g_traceStats.swoBytesReceived++;
    
    if (g_dataCallback) {
        g_dataCallback(AGDI_TRACE_SWO_START, &byte, 1, 0, g_userData);
    }
}

/**
 * @brief ITM 数据回调
 */
static void _itmCallback(uint8_t port, const uint8_t* data, size_t len, void* userData)
{
    (void)userData;
    g_traceStats.itmPacketsReceived++;
    
    if (g_dataCallback) {
        g_dataCallback(AGDI_TRACE_ITM_START, data, len, 0, g_userData);
    }
}

// ============================================================================
// API 实现
// ============================================================================

int _Init(int dapIndex)
{
    if (g_initialized) {
        return 0;
    }

    g_dapIndex = dapIndex;
    
    // 初始化解码器
    g_swoDecoder = ORBMDK_Trace_Create(TRACE_PROT_SWO_UART);
    if (g_swoDecoder) {
        ORBMDK_Trace_SetSWOCallback(g_swoDecoder, _swoCallback, nullptr);
    }
    
    g_itmDecoder = ORBMDK_Trace_Create(TRACE_PROT_ITM);
    if (g_itmDecoder) {
        ORBMDK_Trace_SetITMCallback(g_itmDecoder, _itmCallback, nullptr);
    }
    
    g_etmDecoder = ORBMDK_Trace_Create(TRACE_PROT_ETM35);
    g_tpiuDecoder = ORBMDK_Trace_Create(TRACE_PROT_TPIU);

    // 初始化 ETM 解码器
    g_etm = ORBMDK_ETM_Create(ETM_PV_ETM35);
    if (g_etm) {
        ORBMDK_ETM_Init(g_etm);
        ORBMDK_ETM_SetBranchCallback(g_etm, _etmBranchCallback, nullptr);
        ORBMDK_ETM_SetSyncCallback(g_etm, _etmSyncCallback, nullptr);
        ORBMDK_ETM_SetExceptionCallback(g_etm, _etmExceptionCallback, nullptr);
    }

    // 重置配置
    memset(&g_traceConfig, 0, sizeof(g_traceConfig));
    memset(&g_traceStats, 0, sizeof(g_traceStats));

    g_initialized = true;
    return 0;
}

ORBMDK_API void _Shutdown(void)
{
    if (!g_initialized) {
        return;
    }

    // 停止所有跟踪
    _SWO_Stop();
    _ETM_Stop();
    _ITM_Stop();
    _TPIU_Stop();

    // 禁用跟踪单元
    _disableTraceUnit();

    // 销毁解码器
    if (g_swoDecoder) {
        ORBMDK_Trace_Destroy(g_swoDecoder);
        g_swoDecoder = nullptr;
    }
    if (g_itmDecoder) {
        ORBMDK_Trace_Destroy(g_itmDecoder);
        g_itmDecoder = nullptr;
    }
    if (g_etmDecoder) {
        ORBMDK_Trace_Destroy(g_etmDecoder);
        g_etmDecoder = nullptr;
    }
    if (g_tpiuDecoder) {
        ORBMDK_Trace_Destroy(g_tpiuDecoder);
        g_tpiuDecoder = nullptr;
    }

    // 销毁 ETM 解码器
    if (g_etm) {
        ORBMDK_ETM_Destroy(g_etm);
        g_etm = nullptr;
    }

    g_initialized = false;
}

static int _SWO_Config(enum ORBMDK_SWO_Mode mode, uint32_t baudRate)
{
    if (!g_initialized) {
        return -1;
    }

    g_traceConfig.swoMode = mode;
    g_traceConfig.swoBaudRate = baudRate;

    if (mode == SWO_MODE_DISABLED) {
        g_traceConfig.swoEnabled = false;
        return 0;
    }

    // 配置 SWO 硬件
    uint8_t prescaler = _findSWOPrescaler(baudRate);
    
    // 配置 TPIU 为 SWO 模式
    uint32_t reg = (mode == SWO_MODE_UART) ? 1 : 2;  // 1=UART, 2=Manchester
    _writeRegister(TPIU_SPPR, reg);

    // 配置异步时钟预分频
    _writeRegister(TPIU_PSCR, prescaler - 1);

    // 配置 DWT 用于 SWO 输出
    _writeRegister(DWT_CTRL, 0x40010000);  // 启用 DWT
    _writeRegister(DWT_TFCR, 0x00000001);  // 启用 SWO 输出

    return 0;
}

static int _SWO_Start(void)
{
    if (!g_initialized) {
        return -1;
    }

    g_traceConfig.swoEnabled = true;
    
    // 启用 DWT 跟踪
    uint32_t ctrl;
    _readRegister(DWT_CTRL, &ctrl);
    ctrl |= 0x40010000;  // 启用
    _writeRegister(DWT_CTRL, ctrl);
    _writeRegister(DWT_TFCR, 0x00000001);  // 启用 SWO

    return 0;
}

static int _SWO_Stop(void)
{
    if (!g_initialized) {
        return -1;
    }

    g_traceConfig.swoEnabled = false;
    
    // 禁用 SWO 输出
    _writeRegister(DWT_TFCR, 0);
    
    uint32_t ctrl;
    _readRegister(DWT_CTRL, &ctrl);
    ctrl &= ~0x40010000;
    _writeRegister(DWT_CTRL, ctrl);

    return 0;
}

static int _ETM_Config(bool enabled)
{
    if (!g_initialized) {
        return -1;
    }

    g_traceConfig.etmEnabled = enabled;

    if (enabled) {
        // 配置 ETM
        _writeRegister(ETM_CR, 0x00010001);   // 启用 ETM
        _writeRegister(ETM_FFRR, 0x00000001); // 请求 FIFO 刷新
    }

    return 0;
}

static int _ETM_Start(void)
{
    if (!g_initialized) {
        return -1;
    }

    g_traceConfig.etmEnabled = true;
    
    // 启用 ETM
    uint32_t cr;
    _readRegister(ETM_CR, &cr);
    cr |= 0x00000001;
    _writeRegister(ETM_CR, cr);

    return 0;
}

static int _ETM_Stop(void)
{
    if (!g_initialized) {
        return -1;
    }

    g_traceConfig.etmEnabled = false;
    
    // 禁用 ETM
    uint32_t cr;
    _readRegister(ETM_CR, &cr);
    cr &= ~0x00000001;
    _writeRegister(ETM_CR, cr);

    return 0;
}

static int _ITM_Config(uint32_t portMask)
{
    if (!g_initialized) {
        return -1;
    }

    g_traceConfig.itmPortMask = portMask;

    // 解锁 ITM
    _writeRegister(ITM_LOCKACCESS, 0xC5ACCE55);
    
    // 配置 ITM
    _writeRegister(ITM_TER, portMask);        // 启用端口
    _writeRegister(ITM_TPR, 0x0000000F);      // 允许所有优先级
    _writeRegister(ITM_TCR, 0x0001000D);      // 启用 ITM, TSCEn=1, SWOEN=1

    return 0;
}

static int _ITM_Start(void)
{
    if (!g_initialized) {
        return -1;
    }

    g_traceConfig.itmEnabled = true;
    
    // 解锁并启用 ITM
    _writeRegister(ITM_LOCKACCESS, 0xC5ACCE55);
    uint32_t tcr;
    _readRegister(ITM_TCR, &tcr);
    tcr |= 0x0001000D;
    _writeRegister(ITM_TCR, tcr);

    return 0;
}

static int _ITM_Stop(void)
{
    if (!g_initialized) {
        return -1;
    }

    g_traceConfig.itmEnabled = false;
    
    // 禁用 ITM
    _writeRegister(ITM_LOCKACCESS, 0xC5ACCE55);
    uint32_t tcr;
    _readRegister(ITM_TCR, &tcr);
    tcr &= ~0x0001000D;
    _writeRegister(ITM_TCR, tcr);

    return 0;
}

static int _ITM_Write(uint8_t port, const uint8_t* data, size_t len)
{
    if (!g_initialized || port > 31) {
        return -1;
    }

    // 检查 ITM 端口是否启用
    if (!(g_traceConfig.itmPortMask & (1 << port))) {
        return -1;
    }

    // 通过 USB 发送 ITM 数据
    uint8_t cmd[64];
    cmd[0] = ID_DAP_VENDOR_START;  // V2 Vendor 命令
    
    // ITM 激励包格式
    size_t offset = 1;
    for (size_t i = 0; i < len && offset < 60; i++) {
        cmd[offset++] = (uint8_t)(port | 0x80);  // SW 写入
        cmd[offset++] = data[i];
    }

    return ORBMDK_USB_Bulk_Write(cmd, offset, 100) >= 0 ? 0 : -1;
}

static int _TPIU_Config(uint32_t traceClock, enum ORBMDK_Trace_Width traceWidth)
{
    if (!g_initialized) {
        return -1;
    }

    g_traceConfig.traceClock = traceClock;
    g_traceConfig.traceWidth = traceWidth;

    // 配置 TPIU
    uint32_t portSize = 1 << (traceWidth - 1);  // 1bit=1, 2bit=2, 4bit=4
    _writeRegister(TPIU_CSPSR, portSize);
    _writeRegister(TPIU_FFCR, 0x00000100);      // 禁用格式化器
    _writeRegister(TPIU_PSCR, traceClock / 1000000 - 1);  // 异步时钟分频

    return 0;
}

static int _TPIU_Start(void)
{
    if (!g_initialized) {
        return -1;
    }

    g_traceConfig.tpiuEnabled = true;
    return 0;
}

static int _TPIU_Stop(void)
{
    if (!g_initialized) {
        return -1;
    }

    g_traceConfig.tpiuEnabled = false;
    return 0;
}

int _AGDI_Trace_GetStatus(struct ORBMDK_AGDI_Trace_Status* status)
{
    if (!g_initialized || !status) {
        return -1;
    }

    status->swoEnabled = g_traceConfig.swoEnabled;
    status->etmEnabled = g_traceConfig.etmEnabled;
    status->itmEnabled = g_traceConfig.itmEnabled;
    status->tpiuEnabled = g_traceConfig.tpiuEnabled;
    status->swoBytesReceived = g_traceStats.swoBytesReceived;
    status->itmPacketsReceived = g_traceStats.itmPacketsReceived;
    status->etmPacketsReceived = g_traceStats.etmPacketsReceived;
    status->overflowCount = g_traceStats.overflowCount;
    status->errorCount = g_traceStats.errorCount;

    return 0;
}

void _AGDI_Trace_SetCallback(ORBMDK_AGDI_Trace_Data_CB callback, void* userData)
{
    g_dataCallback = callback;
    g_userData = userData;
}

int _AGDI_Trace_Read(uint8_t* buffer, size_t maxLen, int timeoutMs)
{
    if (!g_initialized || !buffer) {
        return -1;
    }

    size_t respLen = maxLen;
    uint8_t cmd[4] = { ID_DAP_VENDOR_START, 0x01, 0x00, 0x00 };  // V2 Read Trace

    int ret = ORBMDK_USB_Bulk_DAPCommand(cmd, sizeof(cmd), buffer, &respLen, timeoutMs);
    
    if (ret == 0 && respLen > 0) {
        // 解码数据
        if (g_etm) {
            ORBMDK_ETM_Pump(g_etm, buffer, respLen);
        }
        if (g_itmDecoder) {
            ORBMDK_Trace_Pump(g_itmDecoder, buffer, respLen);
        }
        if (g_swoDecoder) {
            ORBMDK_Trace_Pump(g_swoDecoder, buffer, respLen);
        }
    }
    
    return ret == 0 ? (int)respLen : -1;
}

int _AGDI_Trace_ReadNB(uint8_t* buffer, size_t maxLen)
{
    return _AGDI_Trace_Read(buffer, maxLen, 0);
}

int _AGDI_Trace_Flush(void)
{
    if (!g_initialized) {
        return -1;
    }

    // 刷新 ETM FIFO
    _writeRegister(ETM_FFRR, 0x00000001);
    
    // 等待完成
    uint32_t val;
    for (int i = 0; i < 100; i++) {
        _readRegister(ETM_FFRR, &val);
        if ((val & 0x00000001) == 0) {
            break;
        }
    }

    return 0;
}

int _AGDI_Trace_Reset(void)
{
    if (!g_initialized) {
        return -1;
    }

    // 停止所有跟踪
    _SWO_Stop();
    _ETM_Stop();
    _ITM_Stop();
    _TPIU_Stop();

    // 禁用所有跟踪单元
    _disableTraceUnit();

    // 重置统计
    memset(&g_traceStats, 0, sizeof(g_traceStats));

    return 0;
}

// ============================================================================
// 导出函数
// ============================================================================
extern "C" {

ORBMDK_API int AGDI_Trace_Init(int dapIndex) { return _Init(dapIndex); }
ORBMDK_API void AGDI_Trace_Shutdown(void) { _Shutdown(); }
ORBMDK_API int AGDI_Trace_SWO_Config(enum ORBMDK_SWO_Mode mode, uint32_t baudRate) { return _SWO_Config(mode, baudRate); }
ORBMDK_API int AGDI_Trace_SWO_Start(void) { return _SWO_Start(); }
ORBMDK_API int AGDI_Trace_SWO_Stop(void) { return _SWO_Stop(); }
ORBMDK_API int AGDI_Trace_ETM_Config(bool enabled) { return _ETM_Config(enabled); }
ORBMDK_API int AGDI_Trace_ETM_Start(void) { return _ETM_Start(); }
ORBMDK_API int AGDI_Trace_ETM_Stop(void) { return _ETM_Stop(); }
ORBMDK_API int AGDI_Trace_ITM_Config(uint32_t portMask) { return _ITM_Config(portMask); }
ORBMDK_API int AGDI_Trace_ITM_Start(void) { return _ITM_Start(); }
ORBMDK_API int AGDI_Trace_ITM_Stop(void) { return _ITM_Stop(); }
ORBMDK_API int AGDI_Trace_ITM_Write(uint8_t port, const uint8_t* data, size_t len) { return _ITM_Write(port, data, len); }
ORBMDK_API int AGDI_Trace_TPIU_Config(uint32_t traceClock, enum ORBMDK_Trace_Width traceWidth) { return _TPIU_Config(traceClock, traceWidth); }
ORBMDK_API int AGDI_Trace_TPIU_Start(void) { return _TPIU_Start(); }
ORBMDK_API int AGDI_Trace_TPIU_Stop(void) { return _TPIU_Stop(); }
ORBMDK_API int AGDI_Trace_GetStatus(struct ORBMDK_AGDI_Trace_Status* status) { return _AGDI_Trace_GetStatus(status); }
ORBMDK_API void AGDI_Trace_SetCallback(ORBMDK_AGDI_Trace_Data_CB callback, void* userData) { _AGDI_Trace_SetCallback(callback, userData); }
ORBMDK_API int AGDI_Trace_Read(uint8_t* buffer, size_t maxLen, int timeoutMs) { return _AGDI_Trace_Read(buffer, maxLen, timeoutMs); }
ORBMDK_API int AGDI_Trace_ReadNB(uint8_t* buffer, size_t maxLen) { return _AGDI_Trace_ReadNB(buffer, maxLen); }
ORBMDK_API int AGDI_Trace_Flush(void) { return _AGDI_Trace_Flush(); }
ORBMDK_API int AGDI_Trace_Reset(void) { return _AGDI_Trace_Reset(); }

} // extern "C"
