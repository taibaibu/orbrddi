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
