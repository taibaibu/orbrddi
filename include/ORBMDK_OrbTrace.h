/**
 * @file ORBMDK_OrbTrace.h
 * @brief orbtrace 调试器的**配置**接口（orbuculum `orbtraceIf.c` 的 WinUSB 移植）。
 *
 * 本层（RDDI）统一管理 USB：这里负责打开 orbtrace 复合设备上除 CMSIS-DAP 之外的
 * 三个 vendor 接口，并把 orbuculum 的全部配置动作搬过来：
 *
 *   接口（`bInterfaceClass = 0xFF`，按 `bInterfaceSubClass` 区分）：
 *     0x50 'P'  Power    - 目标供电（电压 / 使能）
 *     0x54 'T'  Trace    - trace 采集（宽度 / SWO / 波特率），一个 bulk IN 端点
 *     0x56 'V'  Version  - 探针版本串（iInterface 字符串，去 "Version: " 前缀）
 *
 *   Trace 接口的 `bInterfaceProtocol`：0x00 未知 / 0x01 TPIU raw / 0x10 OFLOW v1.0。
 *
 *   vendor 控制请求（`bmRequestType = 0x41`，`wIndex = (indexUpperHalf<<8)|interface`）：
 *     Trace:  `RQ_SET_TWIDTH(1)` wValue = 宽度 1/2/3，或 SWO 模式
 *                                `(isMANCH ? 0x10 : 0x12) | (useTPIU ? 0x01 : 0)`
 *             `RQ_SET_SPEED(2)`  wValue = 0，数据 = 4 字节小端波特率
 *     Power:  `RQ_SET_ENABLE(1)`  wValue = 0/1，wIndex 高字节 = 通道
 *             `RQ_SET_VOLTAGE(2)` wValue = 毫伏，wIndex 高字节 = 通道
 *
 * 与 orbuculum 的差异：本层用 **WinUSB**（不是 libusb），因此没有 claim/release
 * 接口的概念（`WinUsb_Initialize` 即独占）；端点按 `WinUsb_QueryPipe` 取。
 *
 * ⚠️ 未实机验证（本机无 orbtrace + 目标板）。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// orbtrace 供电通道（与 orbuculum 的 enum Channel 对齐）
typedef enum {
    ORBMDK_ORBTRACE_CH_VTREF = 0,
    ORBMDK_ORBTRACE_CH_VTPWR = 1,
    ORBMDK_ORBTRACE_CH_MAX   = 2,
    ORBMDK_ORBTRACE_CH_NONE  = 3,
    ORBMDK_ORBTRACE_CH_ALL   = 0xFF,
} ORBMDK_OrbTrace_Channel;

// trace 接口协议（bInterfaceProtocol）
enum {
    ORBMDK_ORBTRACE_PROT_UNKNOWN   = 0x00,
    ORBMDK_ORBTRACE_PROT_TPIU      = 0x01,
    ORBMDK_ORBTRACE_PROT_OFLOWV1_0 = 0x10,
};

// 设备/接口信息
typedef struct {
    int      present;        // 1 = 找到 orbtrace 设备
    int      opened;         // 1 = 接口已打开
    uint16_t vid;
    uint16_t pid;
    int      traceIf;        // trace 接口号（-1 = 无）
    int      powerIf;        // power 接口号（-1 = 无）
    int      versionIf;      // version 接口号（-1 = 无）
    int      traceProtocol;  // 见 ORBMDK_ORBTRACE_PROT_*
    int      supportsOFLOW;  // 1 = trace 接口 protocol == 0x10
    char     product[128];
    char     serial[64];
    char     version[64];
} ORBMDK_OrbTrace_Info;

// ---------------------------------------------------------------------------
// 设备发现 / 打开
// ---------------------------------------------------------------------------

// 扫描（并缓存）orbtrace 设备与其三个接口。幂等；返回 1 = 找到。
int  ORBMDK_OrbTrace_Detect(void);

// 取缓存的信息（先自动 Detect）。返回 0 成功。
int  ORBMDK_OrbTrace_GetInfo(ORBMDK_OrbTrace_Info* info);

// 打开 trace / power 接口（Detect 之后调用）。返回 0 成功。
int  ORBMDK_OrbTrace_Open(void);
void ORBMDK_OrbTrace_Close(void);
int  ORBMDK_OrbTrace_IsOpen(void);

// ---------------------------------------------------------------------------
// 配置（对应 orbuculum OrbtraceIfSet*）
// ---------------------------------------------------------------------------

// 并口 trace 宽度：1 / 2 / 3（= 1/2/4-bit）；传 4 视作 3（与 orbuculum 一致）。
int ORBMDK_OrbTrace_SetTraceWidth(int width);

// SWO 模式：isManchester 选编码，useTPIU 选是否经 TPIU。
int ORBMDK_OrbTrace_SetTraceSWO(int isManchester, int useTPIU);

// SWO 波特率（Hz）。
int ORBMDK_OrbTrace_SetSWOBaudrate(uint32_t baud);

// 目标供电：channel 见 ORBMDK_OrbTrace_Channel；millivolts 需在
// 900..5010 mV（orbuculum 的 MIN/MAX + 容差）内，否则拒绝。
int ORBMDK_OrbTrace_SetVoltage(int channel, int millivolts);

// 目标供电使能：on = 0/1；channel 可为 CH_ALL。
int ORBMDK_OrbTrace_SetVoltageEnable(int channel, int on);

// ---------------------------------------------------------------------------
// trace 数据（bulk IN，**raw 字节**，不做 OFLOW/COBS 解帧）
// ---------------------------------------------------------------------------

// 读一批 raw trace 字节。返回读到的字节数（>=0），<0 失败；0 = 超时无数据。
int ORBMDK_OrbTrace_ReadTrace(uint8_t* buf, size_t len, int timeoutMs);

// 环溢出丢掉的字节数（累计；Open 时清零）。返回 0 成功。
//
// 读取线程按 32 × 64 KB 常驻传输往环里灌，消费者跟不上时环会满 —— 满了就丢，
// 丢一个字节记一次。**这是"静默丢数据"的唯一量化出口**：没有它，上层只能看到
// "字节数比预期少"，分不出是探针没发还是本层丢了。
int ORBMDK_OrbTrace_GetDropped(unsigned long long* dropped);

#ifdef __cplusplus
}
#endif
