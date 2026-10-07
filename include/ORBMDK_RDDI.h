/**
 * @file ORBMDK_RDDI.h
 * @brief RDDI 函数声明
 *
 * 兼容 elaphureLinkAGDI 的 RDDI 接口
 */

#pragma once

#include "ORBMDK.h"
#include <stdint.h>
#include "ORBMDK_DAP.h"

// RDDI 导出宏 - 兼容 elaphureLink 的导出方式
#ifndef RDDI_FUNC
#ifdef ORBMDK_EXPORTS
#define RDDI_FUNC extern "C" __declspec(dllexport)
#else
#define RDDI_FUNC extern "C" __declspec(dllimport)
#endif
#endif

// RDDI Handle 类型 - 必须与标准 RDDI (rddi.h) 一致
// 标准定义: typedef int RDDIHandle; (4字节)
typedef int RDDIHandle;

// ---------------------------------------------------------------------------
// RDDI 核心错误码
//
// 取值必须与 ARM rddi.h 完全一致：**非负值**。
// 旧版本此处用 -1/-2/-3... 负值，与官方 CMSIS_DAP.dll 不符；Keil 只判断
// "0 == 成功 / 非 0 == 失败"，但会把返回码原样转交上层显示。
// ---------------------------------------------------------------------------
enum {
    RDDI_SUCCESS               = 0x0000,
    RDDI_BADARG                = 0x0001,
    RDDI_INVHANDLE             = 0x0002,
    RDDI_FAILED                = 0x0003,
    RDDI_TOOMANYCONNECTIONS    = 0x0004,
    RDDI_BUFFER_OVERFLOW       = 0x0005,
    RDDI_BROWSE_FAILED         = 0x000C,
    RDDI_INTERNAL_ERROR        = 0x000D,
    RDDI_PARSE_FAILED          = 0x000E,
    RDDI_OUTOFMEM              = 0x0043,
    RDDI_EXTERNAL_CMD_FAILED   = 0x006E,
    RDDI_LICENSE_FAILED        = 0x0070,
    RDDI_LICENSE_WARNING       = 0x8070,
    RDDI_MAX_ERR               = 0xFFFF,
};

// ---------------------------------------------------------------------------
// RDDI-DAP 接口错误码（ARM rddi_dap.h，0x1000 段）
//
// Keil 的 AGDI（RDDI_DAP_IF.cpp）按 **具体数值** 识别这两个码并做特殊处理：
//   RDDI_DAP_OPERATION_TIMEOUT (0x100E) -> 写 ABORT 后重试
//   RDDI_DAP_DP_STICKY_ERR     (0x100F) -> 清 sticky 错误位后重试
// 旧版本把它们定义成 0x2008/0x2009，AGDI 匹配不上，超时/粘滞错误会被当成
// 普通错误直接上报，表现为调试时 "Cannot access Memory"。
// ---------------------------------------------------------------------------
enum {
    RDDI_DAP_ERR_START                     = 0x1000,
    RDDI_DAP_MULTI_REG_CMD_ERR             = 0x1000,
    RDDI_DAP_DAPINUSE                      = 0x1001,
    RDDI_DAP_NOT_CONFIGURED                = 0x1002,
    RDDI_DAP_NOT_CONNECTED                 = 0x1003,
    RDDI_DAP_CONFIG_CONNECTED              = 0x1004,
    RDDI_DAP_CONNECT_INUSE                 = 0x1005,
    RDDI_DAP_LEVEL1_NOT_IMPL               = 0x1006,
    RDDI_DAP_LEVEL2_NOT_IMPL               = 0x1007,
    RDDI_DAP_CONFIG_FAILED                 = 0x1008,
    RDDI_DAP_CONFIG_FILE_NOT_FOUND         = 0x1009,
    RDDI_DAP_CONFIG_FILE_INVALID           = 0x100A,
    RDDI_DAP_CONNECT_FAILED                = 0x100B,
    RDDI_DAP_CONNECTION_LOST               = 0x100C,
    RDDI_DAP_OPERATION_FAILED              = 0x100D,
    RDDI_DAP_OPERATION_TIMEOUT             = 0x100E,
    RDDI_DAP_DP_STICKY_ERR                 = 0x100F,
    RDDI_DAP_NCONT_STALL                   = 0x1010,
    RDDI_DAP_BAD_REGISTER_ID               = 0x1011,
    RDDI_DAP_REGISTER_NOT_SUPPORTED        = 0x1012,
    RDDI_DAP_NO_MATCH                      = 0x1013,
    RDDI_DAP_MEMAP_UNSUPPORTED             = 0x1014,
    RDDI_TARGET_COMMAND_RESPONSE_TOO_SMALL = 0x1015,
    RDDI_TARGET_CMD_ERROR                  = 0x1016,
    RDDI_COMMUNICATION_ERROR               = 0x1017,
};

// ---------------------------------------------------------------------------
// Keil 扩展错误码（RDDI_DAP_IF.h，0x2000 段）
// AGDI 会把它们转换成 Keil 自己的错误提示。
// ---------------------------------------------------------------------------
enum {
    RDDI_DAP_ERROR             = 0x2000,
    RDDI_DAP_ERROR_NO_DLL      = 0x2001,
    RDDI_DAP_ERROR_INTERNAL    = 0x2002,
    RDDI_DAP_ERROR_POWER       = 0x2003,
    RDDI_DAP_ERROR_DEBUG       = 0x2004,
    RDDI_DAP_ERROR_MEMORY      = 0x2005,
    RDDI_DAP_ERROR_INUSE       = 0x2006,
    RDDI_DAP_ERROR_SWJ         = 0x2007,
};

// 兼容旧代码中使用的别名
enum {
    RDDI_TIMEOUT       = RDDI_DAP_OPERATION_TIMEOUT,  // 0x100E
    RDDI_BUFFER_ERROR  = RDDI_BUFFER_OVERFLOW,        // 0x0005
};

// RDDI 日志级别（ARM rddi.h）
#define RDDI_LOGLEVEL_FATAL   0
#define RDDI_LOGLEVEL_ERROR   1
#define RDDI_LOGLEVEL_WARNING 2
#define RDDI_LOGLEVEL_INFO    3
#define RDDI_LOGLEVEL_DEBUG   4
#define RDDI_LOGLEVEL_TRACE   5

// RDDI 日志回调（ARM rddi.h）
// 注意：ARM 的顺序是 (context, msg, logLevel)，与直觉相反，不可写错。
typedef void (*RDDILogCallback)(void *context, const char *const msg, const int logLevel);

// RDDI_DAP_CONN_DETAILS 结构体 - 与标准 rddi_dap.h 一致 (320字节)
typedef struct _RDDI_DAP_CONN_DETAILS {
    char implementorName[160];       // 实现者名称
    char connectionDescription[160]; // 连接描述
} RDDI_DAP_CONN_DETAILS;

enum {
    DAP_REG_RnW           = 0x00010000,
    DAP_REG_WaitForValue  = 0x00020000,
    DAP_REG_MatchRetry    = 0x00030000,
    DAP_REG_MatchMask     = 0x00040000,

    DAP_REG_DP_IDCODE     = 0x00000000,
    DAP_REG_DP_ABORT      = 0x00000000,
    DAP_REG_DP_CTRL_STAT  = 0x00000004,
    DAP_REG_DP_RESEND     = 0x00000008,
    DAP_REG_DP_SELECT     = 0x00000008,
    DAP_REG_DP_RDBUFF     = 0x0000000C,

    DAP_REG_AP_CSW        = 0x00000000 | DAP_REG_RnW,
    DAP_REG_AP_TAR        = 0x00000004 | DAP_REG_RnW,
    DAP_REG_AP_DRW        = 0x0000000C | DAP_REG_RnW,
    DAP_REG_AP_BASE       = 0x000000F8 | DAP_REG_RnW,
    DAP_REG_AP_IDR        = 0x000000FC | DAP_REG_RnW,

    DAP_REG_AP_CSW_WAIT   = DAP_REG_AP_CSW | DAP_REG_WaitForValue,
    DAP_REG_AP_TAR_WAIT  = DAP_REG_AP_TAR | DAP_REG_WaitForValue,
    DAP_REG_AP_DRW_WAIT  = DAP_REG_AP_DRW | DAP_REG_WaitForValue,
};

enum {
    INFO_CAPS_SWD                 = 0x01,   // BIT(0)
    INFO_CAPS_JTAG                = 0x02,   // BIT(1)
    INFO_CAPS_SWO_UART            = 0x04,   // BIT(2)
    INFO_CAPS_SWO_MANCHESTER      = 0x08,   // BIT(3)
    INFO_CAPS_ATOMIC_CMDS         = 0x10,   // BIT(4) - 原子命令
    INFO_CAPS_TEST_DOMAIN_TIMER   = 0x20,   // BIT(5)
    INFO_CAPS_SWO_STREAMING_TRACE = 0x40,   // BIT(6)
    INFO_CAPS_UART_PORT           = 0x80,   // BIT(7)
    INFO_CAPS_USB_COM_PORT        = 0x100,  // BIT(8)
};

// C 接口导出 - 兼容 elaphureLinkAGDI
#ifdef __cplusplus
extern "C" {
#endif

// RDDI Core - 与标准 rddi.h 一致
RDDI_FUNC int RDDI_Open(RDDIHandle *pHandle, const void *pDetails);
RDDI_FUNC int RDDI_Close(RDDIHandle handle);
// RDDI_GetLastError 标准签名: 无 handle 参数
RDDI_FUNC int RDDI_GetLastError(int *pError, char *pDetails, size_t detailsLen);

// DAP Functions - 与标准 rddi_dap.h 一致
RDDI_FUNC int DAP_GetInterfaceVersion(const RDDIHandle handle, int *version);
RDDI_FUNC int DAP_Configure(const RDDIHandle handle, const char *configFileName);
RDDI_FUNC int DAP_Connect(const RDDIHandle handle, RDDI_DAP_CONN_DETAILS *pConnDetails);
RDDI_FUNC int DAP_GetNumberOfDAPs(const RDDIHandle handle, int *noOfDAPs);  // 标准函数
RDDI_FUNC int DAP_GetDAPIDList(const RDDIHandle handle, int *DAP_ID_Array, size_t sizeOfArray);  // 标准函数
RDDI_FUNC int DAP_Disconnect(const RDDIHandle handle);
RDDI_FUNC int DAP_ReadReg(const RDDIHandle handle, const int DAP_ID, const int regID, int *value);
RDDI_FUNC int DAP_WriteReg(const RDDIHandle handle, const int DAP_ID, const int regID, const int value);
RDDI_FUNC int DAP_RegAccessBlock(const RDDIHandle handle, const int DAP_ID, const int numRegs,
                                 const int *regIDArray, int *dataArray);
RDDI_FUNC int DAP_RegWriteRepeat(const RDDIHandle handle, const int DAP_ID, const int numRepeats,
                                 const int regID, const int *dataArray);
RDDI_FUNC int DAP_RegReadRepeat(const RDDIHandle handle, const int DAP_ID, const int numRepeats,
                                const int regID, int *dataArray);

// DAP 高级序列函数 —— 签名严格对齐 ARM rddi_dap.h
RDDI_FUNC int DAP_RegReadBlock(const RDDIHandle handle, const int DAP_ID, const int numRegs,
                               const int *regIDArray, int *dataArray);
RDDI_FUNC int DAP_RegWriteBlock(const RDDIHandle handle, const int DAP_ID, const int numRegs,
                                const int *regIDArray, const int *dataArray);
RDDI_FUNC int DAP_RegReadWaitForValue(const RDDIHandle handle, const int DAP_ID, const int numRepeats,
                                      const int regID, const int *mask, const int *requiredValue);
RDDI_FUNC int DAP_DefineSequence(const RDDIHandle handle, const int seqID, void *seqDef);
RDDI_FUNC int DAP_RunSequence(const RDDIHandle handle, const int seqID, void *seqInData, void *seqOutData);
RDDI_FUNC int DAP_HostStatus(const RDDIHandle handle, int hostStatus, int state);
RDDI_FUNC int DAP_GetSupportedOptimisationLevel(const RDDIHandle handle, int *level);
RDDI_FUNC int DAP_SetCommTimeout(const RDDIHandle handle, int timeoutMs);
RDDI_FUNC int DAP_Target(const RDDIHandle handle, const char *request_str, char *resp_str, const int resp_len);

// CMSIS-DAP - 与标准 rddi_dap_cmsis.h 一致
RDDI_FUNC int CMSIS_DAP_Detect(const RDDIHandle handle, int *noOfIFs);
RDDI_FUNC int CMSIS_DAP_Identify(const RDDIHandle handle, int ifNo, int idNo, char *str, const int len);
RDDI_FUNC int CMSIS_DAP_ConfigureInterface(const RDDIHandle handle, int ifNo, char *str);
RDDI_FUNC int CMSIS_DAP_DetectNumberOfDAPs(const RDDIHandle handle, int *noOfDAPs);
RDDI_FUNC int CMSIS_DAP_DetectDAPIDList(const RDDIHandle handle, int *DAP_ID_Array, size_t sizeOfArray);
RDDI_FUNC int CMSIS_DAP_Commands(const RDDIHandle handle, int num, unsigned char **request, int *req_len, unsigned char **response, int *resp_len);
RDDI_FUNC int CMSIS_DAP_ConfigureDAP(const RDDIHandle handle, const char *str);  // 标准函数
RDDI_FUNC int CMSIS_DAP_Capabilities(const RDDIHandle handle, int ifNo, int *cap_info);  // 标准函数
RDDI_FUNC int CMSIS_DAP_SWJ_Sequence(const RDDIHandle handle, int num, unsigned char *request);
RDDI_FUNC int CMSIS_DAP_SWJ_Pins(const RDDIHandle handle, unsigned char pinselect, unsigned char pinout, int *res, int wait);

// 缺失的 CMSIS-DAP 函数
RDDI_FUNC int CMSIS_DAP_Delay(const RDDIHandle handle, int delay_us);
RDDI_FUNC int CMSIS_DAP_ResetTarget(const RDDIHandle handle);
RDDI_FUNC int CMSIS_DAP_SWJ_Clock(const RDDIHandle handle, unsigned int clock);
RDDI_FUNC int CMSIS_DAP_WriteABORT(const RDDIHandle handle, int dap_id, unsigned int abort);
RDDI_FUNC int CMSIS_DAP_SWD_Configure(const RDDIHandle handle, uint8_t cfg);
RDDI_FUNC int CMSIS_DAP_SWD_Sequence(const RDDIHandle handle, int num, unsigned char *request);
RDDI_FUNC int CMSIS_DAP_JTAG_Configure(const RDDIHandle handle, int count, uint8_t *ir_len);
RDDI_FUNC int CMSIS_DAP_JTAG_Sequence(const RDDIHandle handle, int num, uint8_t *info, uint8_t *tdi, uint8_t *tdo, uint8_t mask);
RDDI_FUNC int CMSIS_DAP_JTAG_GetIDCODEs(const RDDIHandle handle, int *count, uint32_t *idcodes);
RDDI_FUNC int CMSIS_DAP_JTAG_GetIRLengths(const RDDIHandle handle, int *count, uint8_t *lengths);
// SWO 接口 —— 签名严格对齐 ARM rddi_dap_swo.h
RDDI_FUNC int CMSIS_DAP_SWO_Control(const RDDIHandle handle, int control);
RDDI_FUNC int CMSIS_DAP_SWO_Status(const RDDIHandle handle, int *count, int *status);
RDDI_FUNC int CMSIS_DAP_SWO_Baudrate(const RDDIHandle handle, int baudrate);
RDDI_FUNC int CMSIS_DAP_SWO_Data(const RDDIHandle handle, int *num_written, void *buffer, int *status);
// 签名与 ARM rddi_dap_cmsis.h 对齐（含 ifNo 参数）
RDDI_FUNC int CMSIS_DAP_GetGUID(const RDDIHandle handle, int ifNo, char *guid, int len);
// Keil 扩展：版本号是 int（bits[31:24]=major,[23:16]=minor,[15:0]=build），非字符串
RDDI_FUNC int CMSIS_DAP_GetInterfaceVersion(const RDDIHandle handle, int *version);
RDDI_FUNC int CMSIS_DAP_GetNumberOfDevices(const RDDIHandle handle, int *count);
RDDI_FUNC int CMSIS_DAP_Connect(const RDDIHandle handle, int *connectedInterface);
RDDI_FUNC int CMSIS_DAP_ResetDAP(const RDDIHandle handle);
RDDI_FUNC int CMSIS_DAP_DetectNumberOfDevices(const RDDIHandle handle, int *count);
// Keil 扩展：与 DAP_GetDAPIDList 同构 —— idArray 是 int 数组，sizeOfArray 是字节数
RDDI_FUNC int CMSIS_DAP_GetDeviceIDList(const RDDIHandle handle, int *idArray, size_t sizeOfArray);
RDDI_FUNC int DAP_GetSupportedHostStatusIDs(const RDDIHandle handle, int *count, int *statusIDs);
RDDI_FUNC int CMSIS_DAP_ConfigureDebugger(const RDDIHandle handle, const char *config);
// Keil 扩展：参数个数以官方 CMSIS_DAP.dll 反汇编为准（2 参 / 6 参）
RDDI_FUNC int CMSIS_DAP_Atomic_Control(const RDDIHandle handle, const int reserved);
RDDI_FUNC int CMSIS_DAP_Atomic_Result(const RDDIHandle handle, const int a2, const int a3,
                                      const int a4, const int a5, const int a6);

// PC_* 跟踪捕获接口 (Program Counter Sampling)
RDDI_FUNC int CMSIS_DAP_PC_Capture(const RDDIHandle handle, uint8_t control);
RDDI_FUNC int CMSIS_DAP_PC_GetNumberOfChannels(const RDDIHandle handle, int *count);
RDDI_FUNC int CMSIS_DAP_PC_GetChannelInfos(const RDDIHandle handle, int *count, void *infos);
RDDI_FUNC int CMSIS_DAP_PC_GetCommonFrequency(const RDDIHandle handle, uint32_t *frequency);
RDDI_FUNC int CMSIS_DAP_PC_GetData(const RDDIHandle handle, int *count, uint8_t *data);
RDDI_FUNC int CMSIS_DAP_PC_GetValues(const RDDIHandle handle, int *count, uint32_t *values);

// StreamingTrace 函数
RDDI_FUNC int StreamingTrace_Attach(const RDDIHandle handle, const char *sinkName);
RDDI_FUNC int StreamingTrace_Detach(const RDDIHandle handle);
RDDI_FUNC int StreamingTrace_Connect(const RDDIHandle handle, const char *sinkName, int mode);
RDDI_FUNC int StreamingTrace_Disconnect(const RDDIHandle handle);
RDDI_FUNC int StreamingTrace_Start(const RDDIHandle handle);
RDDI_FUNC int StreamingTrace_Stop(const RDDIHandle handle);
RDDI_FUNC int StreamingTrace_Flush(const RDDIHandle handle);
RDDI_FUNC int StreamingTrace_WaitForEvent(const RDDIHandle handle, int timeoutMs, int *eventType);
RDDI_FUNC int StreamingTrace_SubmitEventBuffer(const RDDIHandle handle, uint8_t *buffer, int bufferSize);
RDDI_FUNC int StreamingTrace_GetSinkCount(const RDDIHandle handle, int *count);
// Keil 扩展：官方实现为 3 参且不写调用方缓冲区
RDDI_FUNC int StreamingTrace_GetSinkDetails(const RDDIHandle handle, const int reserved1,
                                            const int reserved2);
RDDI_FUNC int StreamingTrace_GetConfigItem(const RDDIHandle handle, int item, int *value);
RDDI_FUNC int StreamingTrace_SetConfigItem(const RDDIHandle handle, int item, int value);

// RDDI 日志回调 —— 签名严格对齐 ARM rddi.h（返回值 void，共 4 个参数）
RDDI_FUNC void RDDI_SetLogCallback(const RDDIHandle handle, RDDILogCallback pfn, void *context, int maxLogLevel);

// USB HID 函数在 ORBMDK_HID.h 中声明（namespace ORBMDK）

#ifdef __cplusplus
}
#endif
