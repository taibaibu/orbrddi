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
// DAP 命令超时（毫秒）—— ORBMDK_HID_DAPCommand / ORBMDK_USB_Bulk_DAPCommand
// 调用点的**唯一来源**，不要在调用点直写字面量（历史上散落 30+ 处，改一处漏一处）。
// ⚠ 熔断判据隐含依赖"普通命令 ~1 s 超时"（kDapTripAfter / kDapHalfOpenMs，见
//   ORBMDK_HID.cpp），调这三个值时要连带复核熔断参数。
// ---------------------------------------------------------------------------
#define ORBMDK_TIMEOUT_CMD_MS    1000  // 普通 DAP 命令
#define ORBMDK_TIMEOUT_RESET_MS  5000  // 目标复位、大块传输（DAP_ResetTarget / DAP_TransferBlock）
#define ORBMDK_TIMEOUT_SHORT_MS   100  // 短命令：TransferAbort、SWO 轮询、SWD/JTAG 序列、标定探测

// ---------------------------------------------------------------------------
// HID 命令返回值（V1 路径）
//
// 问题：写失败曾被一律并入超时码 -3，于是"报告长度不匹配被驱动当场拒收
// (err=87)"被熔断机制误判成"探针不再响应"，两条命令就误熔断。
// 熔断只看 WRITE_TIMEOUT / READ_TIMEOUT，立即失败不参与。
// ---------------------------------------------------------------------------
enum {
    HID_RC_OK             =  0,
    HID_RC_NOT_CONNECTED  = -1,   // 未打开设备 / 模式不符
    HID_RC_BAD_LENGTH     = -2,   // 命令长度超出报告负载（编程错误）
    HID_RC_WRITE_TIMEOUT  = -3,   // 写超时（参与熔断）
    HID_RC_READ_FAILED    = -4,   // 读立即失败
    HID_RC_READ_TIMEOUT   = -5,   // 读超时（参与熔断）
    HID_RC_WRITE_FAILED   = -6,   // 写立即失败（含报告长度不匹配 err=87）
    HID_RC_EMPTY_RESPONSE = -7,   // 读到 0 字节
    HID_RC_BAD_RESPONSE   = -8,   // 读到 3 个报告都不是本命令的响应（布局/配对异常）
    HID_RC_NOT_IMPLEMENTED = -9,  // 设备明确回 0xFF = "该命令未实现"（规范约定，见
                                  // ORBMDK_DAP.h 那条）。**协议层失败**：不是超时、
                                  // 不参与熔断、也不该被当成"响应错乱"去丢包重读
};

// 当前 HID 设备的单条命令最大负载字节数（来自 HidP_GetCaps；0 = 未打开）
size_t ORBMDK_HID_GetPayloadMax(void);

// ---------------------------------------------------------------------------
// 总线上是否存在 CMSIS-DAP v1 候选（1 = 存在 / 0 = 没探测到）
//
// 判据与枚举/打开同一套：报告可解析 + 报告负载 ≤ 64 +（首选表命中 或
// UsagePage=0xFF00）。**不含** DAP_Info 自检（那要 1 秒超时，不能放在 Identify 里）。
//
// 用途：CMSIS_DAP_Detect 恒报 2 个接口（0=v2 / 1=v1），不存在的 v1 也会出现在
// µVision 的适配器列表里。上层必须先问这一句，再决定 ifNo=1 怎么命名、要不要
// 用当前设备的身份（序列号/固件版本）去背书它 —— 否则那条幽灵 v1 看起来就是
// 一台真 DAP，而选中后必然在 ConfigureInterface 自检失败。
//
// 实现代价：一次全机 HID 扫描（静默 + 结果带 TTL 缓存；已打开 HID 时直接回 1）。
// ---------------------------------------------------------------------------
int ORBMDK_HID_HasV1Candidate(void);

// 当前 HID 设备的 VID/PID（0 = 未打开），用于如实上报设备身份
void ORBMDK_HID_GetIdentity(uint16_t* vid, uint16_t* pid);

// ---------------------------------------------------------------------------
// DAP 命令通道熔断（实现与完整说明见 src/ORBMDK_HID.cpp；根因是固件侧 busy 闸死）
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
// 问设备一条 DAP_Info，返回设备回报的 Info Length（0 = 没给信息；按规范"未识别的 ID"
// 也是这一档，本层不区分）。⚠ 只对**字符串类 ID** 有效：内部无条件按 C 串处理（拷 Len
// 字节 + 补 '\0'），数值类 ID（0xF0/0xF1/0xFB..0xFF）的二进制值会被 0x00 截断 ——
// 数值请走 DAP_GetCapabilities 或标定里的自解析路径。
int DAP_GetInfo(uint8_t infoId, char* buffer, size_t bufferLen);
// 探针自报能力位（DAP_Info 0xF0）。返回值 = 低字节 | 高字节<<8（CMSIS-DAP v2.1
// 的固件按 2 字节报，老固件 1 字节，两者都兼容）。
// 返回 0 = 成功；<0 = 这条命令没答上来（*caps = 0）。
// 用途：收口 AGDI 侧能力位（SWO/流式等）——**不要替固件宣称它没有的能力**。
int DAP_GetCapabilities(uint16_t* caps);
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
