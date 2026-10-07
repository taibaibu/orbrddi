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
int DAP_JTAG_Configure(uint8_t irLength, uint8_t devCount);
int DAP_JTAG_Sequence(uint8_t sequenceInfo, uint8_t count, const uint8_t* tdiData, uint8_t* tdoData);
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
