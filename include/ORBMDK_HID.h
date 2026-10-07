/**
 * @file ORBMDK_HID.h
 * @brief USB HID 层内部声明
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace ORBMDK {

// USB HID Functions
int ORBMDK_HID_Init(void);
void ORBMDK_HID_Shutdown(void);
int ORBMDK_HID_OpenDevice(const char* serial);
void ORBMDK_HID_CloseDevice(void);
int ORBMDK_HID_IsConnected(void);
int ORBMDK_HID_GetDeviceInfo(char* product, size_t productLen, char* serial, size_t serialLen, char* version, size_t versionLen);
int ORBMDK_HID_DAPCommand(const uint8_t* cmd, size_t cmdLen, uint8_t* resp, size_t* respLen, int timeoutMs);

// ---------------------------------------------------------------------------
// DAP 命令通道熔断（实现与完整说明见 src/ORBMDK_HID.cpp；固件侧缺陷见 bug.md B1）
//
// 缓解的是一个**本层修不了**的固件缺陷：orbtrace 的 DAP 命令通道一次握手失败后
// 内部 busy 永不清零，此后所有命令都不再被消费，而 USB 层照常工作 —— 现场表现
// 为"每条命令都等满 1s 超时"，几十条就把整场会话拖死，且日志里看不出根因。
//
// 本层能做的只有刹车：连续超时到阈值即熔断，后续命令**立即失败**并打一条默认
// 可见的 ERROR，提示给探针重新上电（唯一有效的恢复手段）。
// ---------------------------------------------------------------------------
bool ORBMDK_DapChannelUsable(void);            // false = 已熔断，调用方应立即失败
void ORBMDK_DapNoteResult(int rc);             // 上报一次命令结果（只在统一分发点调用）
int  ORBMDK_DapFastFailCode(void);             // 熔断期间返回的码（与真超时一致）
void ORBMDK_DapChannelReset(const char* why);  // 主动解除熔断（设备重新打开时）

// DAP Command Helpers
int DAP_GetInfo(uint8_t infoId, char* buffer, size_t bufferLen);
int DAP_ConnectTarget(void);
// 指定端口连接：0=默认/自动，1=SWD，2=JTAG（COMPAT_ANALYSIS §18.9）
int DAP_ConnectTargetPort(int port);
int DAP_DisconnectTarget(void);
int DAP_ConfigureTransfer(uint8_t idleCycles, uint16_t waitRetry, uint16_t matchRetry);
int DAP_SetSWJClock(uint32_t clock);
int DAP_ConfigureSWD(uint8_t config);

// Core DAP Operations
int DAP_Transfer(int dapId, uint8_t request, uint32_t* data);
int DAP_TransferBlock(int dapId, uint16_t count, uint8_t request, const uint32_t* writeData, uint32_t* readData);
int DAP_TransferAbort(int dapId);
int DAP_WriteAbort(int dapId, uint32_t abort);
int DAP_SWJ_Sequence(uint8_t count, const uint8_t* data);
int DAP_SWJ_Pins(uint8_t pinSelect, uint8_t pinOut, int* pinIn, int wait);
int DAP_ResetTarget(void);

// JTAG Operations
int DAP_JTAG_Configure(const uint8_t* irLengths, uint8_t devCount);

// JTAG 序列段：一次 USB 往返可带多段，段间 TMS 连续（TAP 状态机不停顿），
// 这样"复位 TAP → 扫链"这类多步时序才是一次原子操作。
//
// 线上格式（CMSIS-DAP ID_DAP_JTAG_SEQUENCE = 0x14）：
//     请求 [0x14][段数][info0][TDI0…][info1][TDI1…]…
//     info = 本段位数(1..63；0 表示 64) | bit6 = 本段 TMS 电平 | bit7 = 捕获 TDO
//     响应 [0x14][status][各捕获段的 TDO 依次拼接]
//
// 尺寸约定：bits 是**本段**位数（1..64）；tdi 指向的缓冲按 (bits+7)/8 字节读取，
// LSB first（第 0 位 = 第一个时钟沿送出的位）。
struct JtagSeg {
    uint8_t bits;        // 1..64（线上以 0 编码 64）
    uint8_t tms;         // 本段恒定 TMS 电平（0/1）
    uint8_t capture;     // 1 = 捕获本段 TDO
    const uint8_t* tdi;  // TDI 数据；NULL = 全 0（补 BYPASS 位时用）
};

// 返回 0 成功；tdoOut 收到捕获数据（tdoLen 可为 NULL）；<0 失败。
int DAP_JTAG_Sequence(const JtagSeg* segs, int segCount,
                      uint8_t* tdoOut, size_t tdoCap, size_t* tdoLen);

int DAP_JTAG_IDCODE(int* idcodeCount, uint32_t* idcodes);

// SWO Trace Operations
int DAP_SWO_Transport(uint8_t transport);
int DAP_SWO_Mode(uint8_t mode);
int DAP_SWO_Baudrate(uint32_t baudrate);
int DAP_SWO_Control(uint8_t control, uint8_t* status);
int DAP_SWO_Data(uint8_t* data, size_t* dataLen, uint8_t* status, uint16_t* traceCount);

// Command Queue Operations
int DAP_QueueCommands(const uint8_t* commands, size_t cmdLen, uint8_t* responses, size_t* respLen);

// DAP Advanced Operations
int DAP_Delay(uint16_t delay_us);
int DAP_SWD_Sequence(uint8_t count, const uint8_t* data);
int DAP_HostStatus(uint8_t hostStatus, uint8_t state);
int DAP_GetSupportedOptimisationLevel(void);
int DAP_SetCommTimeout(int timeoutMs);
int DAP_TargetOp(int operation, int* result);

// Streaming Trace Operations
int StreamingTrace_Init(void);
void StreamingTrace_Shutdown(void);
int StreamingTrace_Start(uint8_t mode);
int StreamingTrace_Stop(void);
int StreamingTrace_GetData(uint8_t* buffer, size_t* size, uint32_t timeoutMs);
// 阻塞式消费环形缓冲：>0 取到字节数 / 0 超时 / -1 未运行。AGDI 的 WaitForEvent 依赖它。
int StreamingTrace_Read(uint8_t* buffer, size_t* size, uint32_t timeoutMs);
int StreamingTrace_Flush(void);
void StreamingTrace_SetBaudrate(uint32_t baudrate);
// SWO 端口：1 = UART/NRZ（默认），2 = Manchester。
// 来源是 AGDI 配置串里的 Trace=，由 RDDI 层在 ConfigureDebugger 里解析后传入；
// 必须在 StreamingTrace_Start 之前调用 —— Start 用它决定 DAP_SWO_Mode 的参数，
// StreamingTrace_GetSinkInfo 也用它决定回报给 AGDI 的 sink 类型。
void StreamingTrace_SetMode(uint8_t swoPort);
int StreamingTrace_GetStatus(uint8_t* status, uint16_t* traceCount);
int StreamingTrace_GetSinkInfo(int index, char* name, int nameLen, char* type, int typeLen);

// DAP Response codes (与 CMSIS-DAP 协议一致: 0=OK, 1=WAIT, 2=FAULT)
enum {
    DAP_RES_OK              = 0,    // Transfer OK
    DAP_RES_WAIT            = 1,    // Transfer WAIT
    DAP_RES_FAULT           = 2,    // Transfer FAULT
    DAP_RES_NO_ACK          = 3,    // No ACK from target
    DAP_RES_VALUE_MISMATCH  = 16,   // Value mismatch
    DAP_RES_ERROR           = 0xFF  // General error
};

} // namespace ORBMDK
