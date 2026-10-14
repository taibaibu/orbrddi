<h1 style="color:red">🔴🔴🔴 最高警告（MAX） 🔴🔴🔴</h1>

<h1 style="color:red">任何偏离 <code>&lt;ORBAGDI 源码树&gt;\AGDI\DbgCM</code> 实现方法的实现皆为风险。</h1>

<h1 style="color:red">PDSC 是 KEIL 核心流程，严禁硬编码，软开关；功能应做就做，行到行兼容。</h1>

---
# ORBMDK_RDDI.dll 使用手册

> **本手册对应当前 0.6.0 的导出接口**（2026-10-13 复核）。**API 使用请以本文为准**。

本文给出 `ORBMDK_RDDI.dll` **全部 RDDI 导出函数**的调用用例。

- 导出方式：每个函数都带 `RDDI_FUNC`（`extern "C" __declspec(dllexport)`），共 **91 个**，纯 C ABI。
- 调用方式：宿主（Keil 的 AGDI 层、或你自己的程序）用 `LoadLibrary` + `GetProcAddress` 逐个取指针调用。
- 缓冲区、字符串一律由调用方提供，本 DLL 不跨模块传堆指针（可安全配 `/MT` 静态 CRT）。

> 与 Keil 实际调试链路相关的约束（DAP 枚举契约、输出参数必须写、HID 响应偏移、
> JTAG-to-SWD / SWD-to-JTAG 切换序列、日志策略等）见 [README.md](README.md) 与
> [COMPAT_ANALYSIS.md](COMPAT_ANALYSIS.md)。本文只讲“怎么调”。
>
> **协议支持**：**SWD 与 JTAG 均已实机验证**（JTAG 建链 / 扫链 / 寄存器访问见 [§7](#7-jtag-接口)）。
> 走哪条由 `CMSIS_DAP_ConfigureInterface` 的配置串 `Port=SW|JTAG` 决定。
>
> **2026-10-08 变更（影响调用方）**：
> - 本层已**移除全部 trace 解码器**（ITM / ETM / TPIU / Trace / OFLOW / COBS / Coverage /
>   Symbols）—— 本层只做传输，解码归 AGDI 层。`CMSIS_DAP_PC_*` 导出**保留**，但**不再在本地
>   解码出采样值**（见 [§8](#8-pc-采样接口)）。
> - **新增** orbtrace 配置与 raw trace 流导出（`OrbTrace_*` / `TraceStream_*`，共 12 个，见
>   [§10](#10-orbtrace-配置与-raw-trace-流)）。导出面 **79 → 91**
>   （2026-10-13 补 `TraceStream_DroppedBytes`，原 90）。

---

## 目录

1. [加载与函数指针](#1-加载与函数指针)
2. [典型调试会话（最小可用流程）](#2-典型调试会话最小可用流程)
3. [RDDI 核心](#3-rddi-核心)
4. [DAP 基础与寄存器](#4-dap-基础与寄存器)
5. [CMSIS-DAP 接口](#5-cmsis-dap-接口)
6. [SWO 接口](#6-swo-接口)
7. [JTAG 接口](#7-jtag-接口)
8. [PC 采样接口](#8-pc-采样接口)
9. [StreamingTrace 接口](#9-streamingtrace-接口)
10. [orbtrace 配置与 raw trace 流](#10-orbtrace-配置与-raw-trace-流)
11. [日志回调](#11-日志回调)
12. [错误码速查](#12-错误码速查)
13. [注意事项（易踩的坑）](#13-注意事项易踩的坑)

---

## 1. 加载与函数指针

### 1.1 头文件中的常量与类型

```c
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "../include/ORBMDK_RDDI.h"   // 提供 RDDIHandle / 错误码 / DAP_REG_* / RDDI_DAP_CONN_DETAILS

// VID/PID（定义于 include/ORBMDK.h）
//   ORBMDK_ORBTRACE_VID = 0x1209
//   ORBMDK_ORBTRACE_PID = 0x3443

// 版本（定义于 include/ORBMDK.h）
//   ORBMDK_VERSION_STRING = "0.6.0"
```

`RDDIHandle` 是 `int` 类型，**从 1 开始编号，0 恒为无效句柄**。

### 1.2 通用加载骨架

```c
typedef int  (*PFN_RDDI_Open)(RDDIHandle*, const void*);
typedef int  (*PFN_RDDI_Close)(RDDIHandle);
typedef int  (*PFN_RDDI_GetLastError)(int*, char*, size_t);

HMODULE h = LoadLibraryA("ORBMDK_RDDI.dll");
if (!h) { printf("load failed: %lu\n", GetLastError()); return 1; }

PFN_RDDI_Open  pRDDI_Open  = (PFN_RDDI_Open) GetProcAddress(h, "RDDI_Open");
PFN_RDDI_Close pRDDI_Close = (PFN_RDDI_Close)GetProcAddress(h, "RDDI_Close");
/* ...其余同名取指针... */

RDDIHandle handle = 0;
int rc = pRDDI_Open(&handle, NULL);   // 成功时 handle >= 1
```

### 1.3 一次性取全部函数指针（推荐）

```c
#define LOAD(name)  pfn_##name = (void*)GetProcAddress(h, #name)

static HMODULE h;
static PFN_RDDI_Open          pfn_RDDI_Open;
static PFN_RDDI_Close         pfn_RDDI_Close;
static PFN_RDDI_GetLastError  pfn_RDDI_GetLastError;
/* ... 在函数内继续声明需要的槽位 ... */

static int load_all(void)
{
    h = LoadLibraryA("ORBMDK_RDDI.dll");
    if (!h) return -1;
    LOAD(RDDI_Open);
    LOAD(RDDI_Close);
    LOAD(RDDI_GetLastError);
    /* ... 这里把用到的函数都 LOAD 一遍 ... */
    return 0;
}
```

> 每个导出名都可用 `GetProcAddress` 直接取到；空指针表示该 DLL 里没有这个导出。

---

## 2. 典型调试会话（最小可用流程）

这是与 Keil AGDI 内部一致的调用顺序，也是自测程序 `test/ORBMDK_RDDI_FullTest.cpp` 的骨架：

```
RDDI_Open
  → CMSIS_DAP_Detect                （探测接口数，恒 >= 1）
  → CMSIS_DAP_Identify              （取适配器显示名/序列号）
  → CMSIS_DAP_ConfigureInterface    （选 V1(HID)/V2(Bulk) 锁定传输层；配置串 Port=SW|JTAG 定协议）
  → DAP_Configure                   （DAP 配置）
  → DAP_GetNumberOfDAPs             （恒 1，必须在 Connect 之前）
  → DAP_GetDAPIDList                （返回 [0]）
  → CMSIS_DAP_Connect               （SWD：切换序列+DPIDR；JTAG：扫链，见 §7）
  → CMSIS_DAP_SWJ_Clock             （设时钟）
  → DAP_ReadReg / DAP_WriteReg ...  （寄存器/内存访问）
  → DAP_Disconnect → RDDI_Close
```

完整示例：

```c
RDDIHandle hd = 0;
if (pfn_RDDI_Open(&hd, NULL) != RDDI_SUCCESS) { /* 设备未插/打不开 */ }

int noOfIFs = 0;
pfn_CMSIS_DAP_Detect(hd, &noOfIFs);           // 0=V2 Bulk, 1=V1 HID

char name[64] = {0};
pfn_CMSIS_DAP_Identify(hd, 0, 2, name, sizeof(name));   // idNo=2 产品名

pfn_CMSIS_DAP_ConfigureInterface(hd, 0, "Port=SW");     // ifNo=0 选 V2(Bulk)；Port=SW|JTAG 定协议

pfn_DAP_Configure(hd, "SWD");

int nDaps = 0;
pfn_DAP_GetNumberOfDAPs(hd, &nDaps);                    // -> 1
int ids[8] = {0};
pfn_DAP_GetDAPIDList(hd, ids, sizeof(ids));             // ids[0] = 0（DAP 索引）

int iface = 0;
pfn_CMSIS_DAP_Connect(hd, &iface);                      // 1=SWD, 2=JTAG
pfn_CMSIS_DAP_SWJ_Clock(hd, 1000000);

int idcode = 0;
pfn_DAP_ReadReg(hd, 0, DAP_REG_DP_IDCODE, &idcode);     // 例如 0x2BA01477

pfn_DAP_Disconnect(hd);
pfn_RDDI_Close(hd);
FreeLibrary(h);
```

---

## 3. RDDI 核心

### 3.1 `RDDI_Open`

```c
int RDDI_Open(RDDIHandle *pHandle, const void *pDetails);
```

打开设备并分配会话句柄。`pDetails` 传 `NULL` 即可（AGDI 实测传 `NULL`）。
- V2(Bulk) 优先，不可用回退 V1(HID)。
- 失败时 `*pHandle = 0`。

```c
RDDIHandle hd = 0;
int rc = pfn_RDDI_Open(&hd, NULL);
if (rc != RDDI_SUCCESS || hd == 0) { /* 打开失败 */ }
```

### 3.2 `RDDI_Close`

```c
int RDDI_Close(RDDIHandle handle);
```

释放会话上下文并熄灭状态 LED。句柄编号不复用（只增不减）。

```c
pfn_RDDI_Close(hd);
hd = 0;
```

### 3.3 `RDDI_GetLastError`

```c
int RDDI_GetLastError(int *pError, char *pDetails, size_t detailsLen);
```

取最近一个上下文的错误码与描述文本（**无 handle 参数**）。

```c
int err = 0;
char detail[256] = {0};
if (pfn_RDDI_GetLastError(&err, detail, sizeof(detail)) == RDDI_SUCCESS) {
    printf("last error = %d (%s)\n", err, detail);
}
```

---

## 4. DAP 基础与寄存器

寄存器 ID 用头文件里的 `DAP_REG_*` 宏 —— 编号严格对齐 ARM `rddi_dap.h`
（低 16 位 = 寄存器号，`bit16` = `DAP_REG_RnW`，`bit17` = `DAP_REG_WaitForValue`）：

| 宏 | 值 | 含义 |
|----|----|------|
| `DAP_REG_DP_IDCODE` / `DAP_REG_DP_0x0` | 0 | DP IDCODE（读）/ ABORT（写） |
| `DAP_REG_DP_CTRL_STAT` / `DAP_REG_DP_0x4` | 1 | DP CTRL/STAT |
| `DAP_REG_DP_SELECT` / `DAP_REG_DP_0x8` | 2 | DP SELECT（AP bank 选择） |
| `DAP_REG_DP_RDBUFF` / `DAP_REG_DP_0xC` | 3 | DP RDBUFF |
| `DAP_REG_AP_CSW` / `DAP_REG_AP_0x0` | 4 | AP CSW |
| `DAP_REG_AP_TAR` / `DAP_REG_AP_0x4` | 5 | AP TAR |
| `DAP_REG_AP_BASE` / `DAP_REG_AP_0x8` | 6 | AP BASE（`APBANK=0xF` 时为 ROM 表基址） |
| `DAP_REG_AP_DRW` / `DAP_REG_AP_0xC` | 7 | AP DRW；`APBANK=0xF` 时为 AP IDR（`DAP_REG_AP_IDR` 同值） |
| `DAP_REG_DP_ABORT` | 8 | DP ABORT（写） |
| `DAP_REG_JTAG_IDCODE` | 9 | JTAG IDCODE（读 DP 0x00） |
| `DAP_REG_DAP_IDR` | 10 | DAP IDR（读 DP 0x00） |
| `DAP_REG_MATCH_MASK` / `DAP_REG_MATCH_RETRY` | 16 / 17 | 虚拟寄存器（不产生总线访问） |

> ⚠️ 方向由**调用的函数**决定：`DAP_ReadReg` 即读、`DAP_WriteReg` 即写，**不要**再
> `| DAP_REG_RnW`（该标志在只读 / 只写函数里被忽略）。`DAP_REG_RnW` / `DAP_REG_WaitForValue`
> 只用于 `DAP_RegAccessBlock`（见 §4.8）。AP bank 由调用方写 `DP SELECT` 决定，本层不改写它。

### 4.1 `DAP_GetInterfaceVersion`

```c
int DAP_GetInterfaceVersion(RDDIHandle handle, int *version);  // -> 1 (RDDI v1)
```

```c
int ver = 0;
pfn_DAP_GetInterfaceVersion(hd, &ver);   // ver == 1
```

### 4.2 `DAP_Configure`

```c
int DAP_Configure(RDDIHandle handle, const char *configFileName);
```

CMSIS-DAP 下 `configFileName` 通常传 `NULL` 或 `"SWD"`。

```c
pfn_DAP_Configure(hd, "SWD");
```

### 4.3 `DAP_Connect`

```c
int DAP_Connect(RDDIHandle handle, RDDI_DAP_CONN_DETAILS *pConnDetails);
```

填充实现者信息（`implementorName` / `connectionDescription`，各 160 字节）。`pConnDetails` 可传 `NULL`。

```c
RDDI_DAP_CONN_DETAILS det;
if (pfn_DAP_Connect(hd, &det) == RDDI_SUCCESS) {
    printf("%s / %s\n", det.implementorName, det.connectionDescription);
}
```

### 4.4 `DAP_GetNumberOfDAPs`

```c
int DAP_GetNumberOfDAPs(RDDIHandle handle, int *noOfDAPs);
```

在 `DAP_Connect` **之前**调用，且**不与目标通信**。ORBTrace 单 DAP，恒返回 `1`。

```c
int n = 0;
pfn_DAP_GetNumberOfDAPs(hd, &n);   // n == 1
```

### 4.5 `DAP_GetDAPIDList`

```c
int DAP_GetDAPIDList(RDDIHandle handle, int *DAP_ID_Array, size_t sizeOfArray);
```

返回 DAP **索引**（不是 IDCODE），`DAP_ID_Array[0] = 0`。
`sizeOfArray` 按字节数或元素个数解释都能接受。

```c
int ids[8] = {0};
pfn_DAP_GetDAPIDList(hd, ids, sizeof(ids));   // ids[0] == 0
```

### 4.6 `DAP_Disconnect`

```c
int DAP_Disconnect(RDDIHandle handle);
```

释放目标连接（幂等）。

```c
pfn_DAP_Disconnect(hd);
```

### 4.7 `DAP_ReadReg` / `DAP_WriteReg`

```c
int DAP_ReadReg (RDDIHandle handle, int DAP_ID, int regID, int *value);
int DAP_WriteReg(RDDIHandle handle, int DAP_ID, int regID, int value);
```

`DAP_ID` 传 DAP 索引（单 DAP 用 `0`）。AP bank 由调用方通过写 `DP_SELECT` 管理。

```c
int idcode = 0, ctrl = 0;
pfn_DAP_ReadReg(hd, 0, DAP_REG_DP_IDCODE, &idcode);       // e.g. 0x2BA01477
pfn_DAP_ReadReg(hd, 0, DAP_REG_DP_CTRL_STAT, &ctrl);

// 使能调试电源（CDBGPWRUPREQ|CSYSPWRUPREQ）
pfn_DAP_WriteReg(hd, 0, DAP_REG_DP_CTRL_STAT, 0x50000000);
pfn_DAP_ReadReg (hd, 0, DAP_REG_DP_RDBUFF, &ctrl);         // 读回清 sticky

// 选 AP0 bank0，读 CSW
pfn_DAP_WriteReg(hd, 0, DAP_REG_DP_SELECT, 0x00000000);
pfn_DAP_ReadReg (hd, 0, DAP_REG_AP_CSW, &ctrl);            // e.g. 0x23000052

// AP IDR 在 AP 地址 0xFC：先把 SELECT 的 APBANKSEL 置 0xF，再读 AP reg3（regID 7）
pfn_DAP_WriteReg(hd, 0, DAP_REG_DP_SELECT, 0x000000F0);
pfn_DAP_ReadReg (hd, 0, DAP_REG_AP_IDR, &ctrl);            // e.g. 0x24770011
pfn_DAP_WriteReg(hd, 0, DAP_REG_DP_SELECT, 0x00000000);    // 复原 bank0
```

> 读回 `Transfer Response` 为 FAULT/WAIT/NO_ACK 时，函数返回
> `RDDI_DAP_DP_STICKY_ERR`(0x100F) / `RDDI_DAP_OPERATION_TIMEOUT`(0x100E)，
> AGDI 会自行清 sticky / 写 ABORT 重试。

### 4.8 `DAP_RegAccessBlock`

```c
int DAP_RegAccessBlock(RDDIHandle handle, int DAP_ID, int numRegs,
                       const int *regIDArray, int *dataArray);
```

一次传**多个不同寄存器号**，元素 `i` 读写 `regIDArray[i]`。
虚拟寄存器 `id==16`(MATCH_MASK) / `id==17`(MATCH_RETRY) 只更新本地设置、不产生总线访问。

```c
int regs[3] = { DAP_REG_DP_CTRL_STAT, DAP_REG_AP_CSW, DAP_REG_AP_DRW };
int data[3] = { 0x50000000, 0x23000052, 0 };
pfn_DAP_RegAccessBlock(hd, 0, 3, regs, data);   // data[] 回填读取值（对 RnW 位为读的项）
```

### 4.9 `DAP_RegReadBlock` / `DAP_RegWriteBlock`

```c
int DAP_RegReadBlock (RDDIHandle handle, int DAP_ID, int numRegs,
                      const int *regIDArray, int *dataArray);
int DAP_RegWriteBlock(RDDIHandle handle, int DAP_ID, int numRegs,
                      const int *regIDArray, const int *dataArray);
```

与 `DAP_RegAccessBlock` 同构：一次多个**独立 regID**。

```c
int ids [4] = { DAP_REG_AP_DRW, DAP_REG_AP_DRW, DAP_REG_AP_DRW, DAP_REG_AP_DRW };
int wdat[4] = { 0x12345678, 0x9ABCDEF0, 0x11111111, 0x22222222 };
int rdat[4] = { 0 };

pfn_DAP_WriteReg(hd, 0, DAP_REG_AP_CSW, 0x23000052);   // 32-bit word + auto-inc
pfn_DAP_WriteReg(hd, 0, DAP_REG_AP_TAR, 0x20000000);   // STM32F1 SRAM 起点
pfn_DAP_RegWriteBlock(hd, 0, 4, ids, wdat);

pfn_DAP_WriteReg(hd, 0, DAP_REG_AP_TAR, 0x20000000);   // TAR 已自增，复位
pfn_DAP_RegReadBlock (hd, 0, 4, ids, rdat);
```

### 4.10 `DAP_RegWriteRepeat` / `DAP_RegReadRepeat`

```c
int DAP_RegWriteRepeat(RDDIHandle handle, int DAP_ID, int numRepeats,
                       int regID, const int *dataArray);
int DAP_RegReadRepeat (RDDIHandle handle, int DAP_ID, int numRepeats,
                       int regID, int *dataArray);
```

**同一个 regID 重复 N 次**（区别于上面的“多个不同 regID”）。

```c
int wdat[4] = { 0xAAAAAAAA, 0x55555555, 0xAAAAAAAA, 0x55555555 };
int rdat[4] = { 0 };

pfn_DAP_WriteReg(hd, 0, DAP_REG_AP_TAR, 0x20000000);
pfn_DAP_RegWriteRepeat(hd, 0, 4, DAP_REG_AP_DRW, wdat);

pfn_DAP_WriteReg(hd, 0, DAP_REG_AP_TAR, 0x20000000);
pfn_DAP_RegReadRepeat (hd, 0, 4, DAP_REG_AP_DRW, rdat);
```

### 4.11 `DAP_RegReadWaitForValue`

```c
int DAP_RegReadWaitForValue(RDDIHandle handle, int DAP_ID, int numRepeats,
                            int regID, const int *mask, const int *requiredValue);
```

轮询读取直到 `(value & *mask) == *requiredValue`，最多 `numRepeats` 次（<=0 视为 1 次）。成功返回 `RDDI_SUCCESS`，超时返回 `RDDI_DAP_OPERATION_TIMEOUT` 一类错误。

```c
int mask = 0x00030000, want = 0x00030000;   // 等 DHCSR 的 S_HALT/HALTED 位
pfn_DAP_RegReadWaitForValue(hd, 0, 100, DAP_REG_AP_DRW, &mask, &want);
```

### 4.12 `DAP_DefineSequence` / `DAP_RunSequence`

```c
int DAP_DefineSequence(RDDIHandle handle, int seqID, void *seqDef);
int DAP_RunSequence  (RDDIHandle handle, int seqID, void *seqInData, void *seqOutData);
```

当前为占位实现，返回 `RDDI_DAP_LEVEL1_NOT_IMPL` (0x1006)。可安全调用探测。

```c
int rc = pfn_DAP_DefineSequence(hd, 1, NULL);   // rc == RDDI_DAP_LEVEL1_NOT_IMPL
```

### 4.13 `DAP_HostStatus`

```c
int DAP_HostStatus(RDDIHandle handle, int hostStatus, int state);
```

`hostStatus`：`0`=halted，`1`=running；`state`：`0`=off，`1`=on。用于驱动状态 LED。

```c
pfn_DAP_HostStatus(hd, 1, 1);   // running 指示灯亮
pfn_DAP_HostStatus(hd, 1, 0);   // 灭
```

### 4.14 `DAP_GetSupportedOptimisationLevel`

```c
int DAP_GetSupportedOptimisationLevel(RDDIHandle handle, int *level);
```

```c
int lvl = 0;
pfn_DAP_GetSupportedOptimisationLevel(hd, &lvl);
```

### 4.15 `DAP_SetCommTimeout`

```c
int DAP_SetCommTimeout(RDDIHandle handle, int timeoutMs);
```

```c
pfn_DAP_SetCommTimeout(hd, 5000);   // 5 秒
```

### 4.16 `DAP_GetSupportedHostStatusIDs`

```c
int DAP_GetSupportedHostStatusIDs(RDDIHandle handle, int *count, int *statusIDs);
```

返回支持的主机状态类型，当前为 `{0x00, 0x01}`，`*count = 2`。

```c
int cnt = 0, ids[8] = {0};
pfn_DAP_GetSupportedHostStatusIDs(hd, &cnt, ids);   // cnt == 2
```

### 4.17 `DAP_Target`

```c
int DAP_Target(RDDIHandle handle, const char *request_str, char *resp_str, int resp_len);
```

文本命令协议，多条以 `;` 分隔：
- `"signal_avail"` → `"sys_reset;sys_power"`
- `"<signal>.on"` / `.off` / `.read` → `"<signal>.<state>"`，状态取 `on/off/unknown/err`

```c
char resp[64] = {0};
pfn_DAP_Target(hd, "sys_reset.on", resp, sizeof(resp));   // 拉低 nRESET

pfn_DAP_Target(hd, "signal_avail", resp, sizeof(resp));
printf("signals: %s\n", resp);                            // sys_reset;sys_power
```

---

## 5. CMSIS-DAP 接口

### 5.1 `CMSIS_DAP_Detect`

```c
int CMSIS_DAP_Detect(RDDIHandle handle, int *noOfIFs);
```

返回可用接口数（**恒 >= 1**）：`0` = CMSIS-DAP v2 (Bulk)，`1` = CMSIS-DAP v1 (HID)。

```c
int n = 0;
pfn_CMSIS_DAP_Detect(hd, &n);   // >= 1
```

### 5.2 `CMSIS_DAP_Identify`

```c
int CMSIS_DAP_Identify(RDDIHandle handle, int ifNo, int idNo, char *str, int len);
```

`idNo`：`1`=Vendor，`2`=Product（对话框显示名），`3`=Serial，`4`=Firmware Version。

```c
char vendor[64] = {0}, product[64] = {0}, serial[64] = {0}, fw[64] = {0};
pfn_CMSIS_DAP_Identify(hd, 0, 1, vendor,  sizeof(vendor));
pfn_CMSIS_DAP_Identify(hd, 0, 2, product, sizeof(product));   // "CMSIS-DAP v2"
pfn_CMSIS_DAP_Identify(hd, 0, 3, serial,  sizeof(serial));
pfn_CMSIS_DAP_Identify(hd, 0, 4, fw,      sizeof(fw));
```

### 5.3 `CMSIS_DAP_ConfigureInterface`

```c
int CMSIS_DAP_ConfigureInterface(RDDIHandle handle, int ifNo, char *str);
```

**做两件事**：

1. **锁定传输层**：`ifNo=0` 用 V2(Bulk)，`ifNo=1` 用 V1(HID)。选中的通道打不开时**直接返回错误**，绝不静默切换。
2. **解析配置串** `str`（`键=值;` 分隔）：
   - `Port=SW` → SWD；**其它值（如 `Port=JTAG`）→ JTAG** —— 决定后续 `CMSIS_DAP_Connect` 走哪条建链路径。
   - `Clock=<Hz>` → 记录调试时钟（只接受合理范围）。

```c
if (pfn_CMSIS_DAP_ConfigureInterface(hd, 0, "Port=SW") != RDDI_SUCCESS) {
    /* 选中的接口不可用，按错误处理 */
}
/* 改走 JTAG： */
pfn_CMSIS_DAP_ConfigureInterface(hd, 0, "Port=JTAG");
```

### 5.4 `CMSIS_DAP_DetectNumberOfDAPs` / `CMSIS_DAP_DetectDAPIDList`

```c
int CMSIS_DAP_DetectNumberOfDAPs(RDDIHandle handle, int *noOfDAPs);
int CMSIS_DAP_DetectDAPIDList  (RDDIHandle handle, int *DAP_ID_Array, size_t sizeOfArray);
```

这两个是 **Keil 扩展**，返回的是**目标 IDCODE**（与 `DAP_GetNumberOfDAPs`/`DAP_GetDAPIDList` 的“索引”语义不同）。
`CMSIS_DAP_DetectDAPIDList` 的 `sizeOfArray` 是**元素个数**。

```c
int n = 0, idcodes[8] = {0};
pfn_CMSIS_DAP_DetectNumberOfDAPs(hd, &n);
pfn_CMSIS_DAP_DetectDAPIDList(hd, idcodes, 8);
printf("IDCODE: 0x%08X\n", idcodes[0]);
```

### 5.5 `CMSIS_DAP_Commands`

```c
int CMSIS_DAP_Commands(RDDIHandle handle, int num,
                       unsigned char **request, int *req_len,
                       unsigned char **response, int *resp_len);
```

当前仅支持 `num==1 && req_len==1 && resp_len==1` 且命令为 `ID_DAP_RESET_TARGET`(0x0A)。

```c
unsigned char cmdBuf[1] = { 0x0A };         // ID_DAP_RESET_TARGET
unsigned char respBuf[1] = { 0 };
unsigned char *req[1] = { cmdBuf };
unsigned char *rsp[1] = { respBuf };
int reqLen = 1, respLen = 1;
pfn_CMSIS_DAP_Commands(hd, 1, req, &reqLen, rsp, &respLen);
```

### 5.6 `CMSIS_DAP_ConfigureDAP`

```c
int CMSIS_DAP_ConfigureDAP(RDDIHandle handle, const char *str);
```

解析配置串（如 `"SWJSwitch=0xE79E"`）；`NULL` 直接成功返回。

```c
pfn_CMSIS_DAP_ConfigureDAP(hd, "SWJSwitch=0xE79E");
```

### 5.7 `CMSIS_DAP_Capabilities`

```c
int CMSIS_DAP_Capabilities(RDDIHandle handle, int ifNo, int *cap_info);
```

位定义见头文件 `INFO_CAPS_*`（`SWD`/`JTAG`/`SWO_UART`/`ATOMIC_CMDS` …）。

当前返回 `INFO_CAPS_SWD | INFO_CAPS_ATOMIC_CMDS` —— **`INFO_CAPS_JTAG` 位有意不置**（只宣称
已验证能力；JTAG 通路本身可用，见 [§7](#7-jtag-接口)。Keil 的 Port 下拉框并不受该位限制）。

```c
int caps = 0;
pfn_CMSIS_DAP_Capabilities(hd, 0, &caps);
printf("SWD=%d JTAG=%d ATOMIC=%d\n",
       !!(caps & INFO_CAPS_SWD), !!(caps & INFO_CAPS_JTAG), !!(caps & INFO_CAPS_ATOMIC_CMDS));
```

### 5.8 `CMSIS_DAP_SWJ_Sequence`

```c
int CMSIS_DAP_SWJ_Sequence(RDDIHandle handle, int num, unsigned char *request);
```

`num` 是**位数**（不是字节数）。标准 JTAG-to-SWD 切换序列：

```c
unsigned char lineReset[7] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };  // 56 位 1
unsigned char jtagToSwd[2] = { 0x9E, 0xE7 };                          // 0xE79E, LSB first
unsigned char idle[1]      = { 0x00 };

pfn_CMSIS_DAP_SWJ_Sequence(hd, 56, lineReset);
pfn_CMSIS_DAP_SWJ_Sequence(hd, 16, jtagToSwd);
pfn_CMSIS_DAP_SWJ_Sequence(hd, 56, lineReset);
pfn_CMSIS_DAP_SWJ_Sequence(hd, 8,  idle);
```

### 5.9 `CMSIS_DAP_SWJ_Pins`

```c
int CMSIS_DAP_SWJ_Pins(RDDIHandle handle, unsigned char pinselect,
                       unsigned char pinout, int *res, int wait);
```

`pinselect`/`pinout` 位定义见 `DAP_PIN_*`（`nRESET` = bit7，`1`=释放，`0`=拉低）。`wait` 单位 us。

```c
int pins = 0;
pfn_CMSIS_DAP_SWJ_Pins(hd, DAP_PIN_nRESET, 0x00, &pins, 0);   // 拉低复位
pfn_CMSIS_DAP_SWJ_Pins(hd, DAP_PIN_nRESET, 0x80, &pins, 0);   // 释放
```

### 5.10 `CMSIS_DAP_SWJ_Clock`

```c
int CMSIS_DAP_SWJ_Clock(RDDIHandle handle, unsigned int clock);   // Hz
```

```c
pfn_CMSIS_DAP_SWJ_Clock(hd, 1000000);   // 1 MHz
```

### 5.11 `CMSIS_DAP_WriteABORT`

```c
int CMSIS_DAP_WriteABORT(RDDIHandle handle, int dap_id, unsigned int abort);
```

写 DP ABORT 寄存器（清 sticky 错误位等），位定义见 `ABORT_*`。

```c
pfn_CMSIS_DAP_WriteABORT(hd, 0, ABORT_STKERRCLR | ABORT_WDERRCLR);   // 0x06
```

### 5.12 `CMSIS_DAP_Delay`

```c
int CMSIS_DAP_Delay(RDDIHandle handle, int delay_us);
```

```c
pfn_CMSIS_DAP_Delay(hd, 1000);   // 1 ms
```

### 5.13 `CMSIS_DAP_ResetTarget`

```c
int CMSIS_DAP_ResetTarget(RDDIHandle handle);
```

```c
pfn_CMSIS_DAP_ResetTarget(hd);
pfn_CMSIS_DAP_Delay(hd, 10000);   // 等目标复位稳定
```

### 5.14 `CMSIS_DAP_SWD_Configure`

```c
int CMSIS_DAP_SWD_Configure(RDDIHandle handle, uint8_t cfg);   // 0 = 默认
```

```c
pfn_CMSIS_DAP_SWD_Configure(hd, 0);
```

### 5.15 `CMSIS_DAP_SWD_Sequence`

```c
int CMSIS_DAP_SWD_Sequence(RDDIHandle handle, int num, unsigned char *request);
```

`num` 为位数（0..255），命令 `0x1D`。

```c
unsigned char seq[2] = { 0x00, 0x00 };
pfn_CMSIS_DAP_SWD_Sequence(hd, 16, seq);
```

### 5.16 `CMSIS_DAP_GetGUID`

```c
int CMSIS_DAP_GetGUID(RDDIHandle handle, int ifNo, char *guid, int len);
```

CMSIS-DAP 无真实 GUID，本实现返回 `"ORBTrace-<序列号>"`。

```c
char guid[64] = {0};
pfn_CMSIS_DAP_GetGUID(hd, 0, guid, sizeof(guid));
```

### 5.17 `CMSIS_DAP_GetInterfaceVersion`

```c
int CMSIS_DAP_GetInterfaceVersion(RDDIHandle handle, int *version);
```

版本号是 **int**（`bits[31:24]`=major，`[23:16]`=minor，`[15:0]`=build），不是字符串。当前 `0x00020000`。

```c
int v = 0;
pfn_CMSIS_DAP_GetInterfaceVersion(hd, &v);
printf("%d.%d.%d\n", (v >> 24) & 0xFF, (v >> 16) & 0xFF, v & 0xFFFF);
```

### 5.18 `CMSIS_DAP_GetNumberOfDevices` / `CMSIS_DAP_DetectNumberOfDevices`

```c
int CMSIS_DAP_GetNumberOfDevices     (RDDIHandle handle, int *count);
int CMSIS_DAP_DetectNumberOfDevices  (RDDIHandle handle, int *count);
```

返回**目标上**扫描到的设备数（`dapIdList` 为空时会主动探测）。目标不在时返回 `0`。

```c
int count = 0;
pfn_CMSIS_DAP_GetNumberOfDevices(hd, &count);
```

### 5.19 `CMSIS_DAP_GetDeviceIDList`

```c
int CMSIS_DAP_GetDeviceIDList(RDDIHandle handle, int *idArray, size_t sizeOfArray);
```

`sizeOfArray` 是**字节数**，写入的是 **IDCODE**。

```c
int idcodes[8] = {0};
pfn_CMSIS_DAP_GetDeviceIDList(hd, idcodes, sizeof(idcodes));
```

### 5.20 `CMSIS_DAP_Connect`

```c
int CMSIS_DAP_Connect(RDDIHandle handle, int *connectedInterface);
```

连接目标；**按 `ConfigureInterface` 里 `Port=` 的选择分岔**：

- **SWD**：`DAP_Connect(port=SWD)` → 补发 JTAG-to-SWD 切换序列 → **读 DPIDR**（建链收尾）→ 写 ABORT 清 sticky。
- **JTAG**：`DAP_Connect(port=JTAG)` → SWD-to-JTAG 切换序列 → 扫链得 IDCODE/IR 长度 → 读 DP IDCODE（见 §7）。

**仅当 `connectedInterface` 指向可写内存时才写**（AGDI 会传选中项索引），输出 `1`=SWD / `2`=JTAG。

```c
int iface = 0;
pfn_CMSIS_DAP_Connect(hd, &iface);
printf("connected: %s\n", iface == 1 ? "SWD" : "JTAG");
```

### 5.21 `CMSIS_DAP_Disconnect` / `CMSIS_DAP_ResetDAP`

```c
int CMSIS_DAP_Disconnect(RDDIHandle handle);   // 幂等断开 + 熄灭 LED
int CMSIS_DAP_ResetDAP  (RDDIHandle handle);   // 重连 + 切换序列 + 复位目标
```

```c
pfn_CMSIS_DAP_ResetDAP(hd);
pfn_CMSIS_DAP_Disconnect(hd);
```

### 5.22 `CMSIS_DAP_ConfigureDebugger`

```c
int CMSIS_DAP_ConfigureDebugger(RDDIHandle handle, const char *config);
```

```c
pfn_CMSIS_DAP_ConfigureDebugger(hd, "config-string");
```

### 5.23 `CMSIS_DAP_Atomic_Control` / `CMSIS_DAP_Atomic_Result`

```c
int CMSIS_DAP_Atomic_Control(RDDIHandle handle, int reserved);                       // 2 参
int CMSIS_DAP_Atomic_Result (RDDIHandle handle, int a2,int a3,int a4,int a5,int a6);  // 6 参
```

当前未实现，返回 `RDDI_DAP_LEVEL1_NOT_IMPL`。参数个数须与官方一致，否则会因参数错位崩溃。

```c
int rc = pfn_CMSIS_DAP_Atomic_Control(hd, 0);   // rc == RDDI_DAP_LEVEL1_NOT_IMPL
```

---

## 6. SWO 接口

### 6.1 `CMSIS_DAP_SWO_Control`

```c
int CMSIS_DAP_SWO_Control(RDDIHandle handle, int control);   // 0=Stop, 1=Start
```

```c
int baud = 115200;
pfn_CMSIS_DAP_SWO_Baudrate(hd, &baud);
pfn_CMSIS_DAP_SWO_Control (hd, 1);   // 启动
```

### 6.2 `CMSIS_DAP_SWO_Baudrate`

```c
int CMSIS_DAP_SWO_Baudrate(RDDIHandle handle, int *baudrate);
```

> ⚠️ 第二参数是 **int 指针**，不是值。AGDI 把候选波特率放在栈上、传地址进来
> （µVision 的 SWO 时钟自动探测循环：时钟频率除以候选分频后逐个试），
> 详见 Todo.md §18.14。按值声明会让 DLL 解引用一个非法地址 → `0xc0000005`。

```c
int baud = 921600;
pfn_CMSIS_DAP_SWO_Baudrate(hd, &baud);
```

### 6.3 `CMSIS_DAP_SWO_Status`

```c
int CMSIS_DAP_SWO_Status(RDDIHandle handle, int *count, int *status);
```

`count` = 待读字节数；`status`：bit0 采集活动，bit6 流错误，bit7 缓冲溢出。

```c
int count = 0, status = 0;
pfn_CMSIS_DAP_SWO_Status(hd, &count, &status);
printf("swo count=%d status=0x%02X\n", count, status);
```

### 6.4 `CMSIS_DAP_SWO_Data`

```c
int CMSIS_DAP_SWO_Data(RDDIHandle handle, int *num_written, void *buffer, int *status);
```

`*num_written` 兼作 in/out：入参为 `buffer` 容量，出参为实际字节数。

```c
uint8_t buf[1024];
int n = sizeof(buf), st = 0;
if (pfn_CMSIS_DAP_SWO_Data(hd, &n, buf, &st) == RDDI_SUCCESS && n > 0) {
    fwrite(buf, 1, n, stdout);   // n 已被改写为实际长度
}
```

---

## 7. JTAG 接口

> JTAG 通路已实机打通（建链 / 扫链 / DP·AP 访问）。使用时先让 `CMSIS_DAP_ConfigureInterface`
> 收到 `Port=JTAG`，再调用 `CMSIS_DAP_Connect`（§5.20）。ADIv5 的语义（选 IR、组 35 位 DR、
> posted read）**全部由本层生成**，固件只负责把 `CMSIS_DAP_JTAG_Sequence` 的位流打到线上。

### 7.1 `CMSIS_DAP_JTAG_Configure`

```c
int CMSIS_DAP_JTAG_Configure(RDDIHandle handle, int count, uint8_t *ir_len);
```

把**链上各器件的 IR 长度**告诉本层：`ir_len[0]` = **离 TDO 最近**的器件，`count` 为器件数（1..16）。
本层据此记录链布局（也用于 `GetIRLengths` 回报）。建链时 `CMSIS_DAP_Connect` 会自动扫链得到同一份
布局，因此通常**不必手动调用**。

```c
uint8_t irLen[2] = { 4, 5 };      // 例：JTAG-DP(IR=4) + 边界扫描(IR=5)
pfn_CMSIS_DAP_JTAG_Configure(hd, 2, irLen);
```

### 7.2 `CMSIS_DAP_JTAG_Sequence`

```c
int CMSIS_DAP_JTAG_Sequence(RDDIHandle handle, int num, uint8_t *info,
                            uint8_t *tdi, uint8_t *tdo, uint8_t mask);
```

**一次下发多段位流**，逐段语义：

- `num` = **段数**（1..64）。
- `info[i]` = 该段位数（低 6 位，`1..63`；**`0` 表示 64 位**）
  `| 0x40`（该段 **TMS 恒为高**，否则恒为低）`| 0x80`（捕获该段 TDO）。
- `tdi` = 各段 TDI **依次拼接**，每段占 `(bits+7)/8` 字节、**LSB first**（不需 TDI 时传 `NULL`）。
- `tdo` = 各**捕获段**的 TDO 依次拼接（同样按字节对齐）。
- `mask` 当前忽略（传 `0`）。

> ⚠️ **每段 TMS 恒定**，所以状态迁移必须**一段一段**发（`RTI→Select-DR→Capture-DR→Shift-DR`
> 各一段），不能指望一段内翻转 TMS。

```c
// 例：Shift-DR 移 32 位；最后 1 位同拍抬 TMS 进入 Exit1-DR
uint8_t info[2] = { 31, 1 | 0x40 };              // 前 31 位 TMS=0；最后 1 位 TMS=1
uint8_t tdi [8] = { 0x78, 0x56, 0x34, 0x12, 0 }; // 32 位数据（LSB first）
uint8_t tdo [8] = { 0 };
pfn_CMSIS_DAP_JTAG_Sequence(hd, 2, info, tdi, tdo, 0);
```

### 7.3 `CMSIS_DAP_JTAG_GetIDCODEs`

```c
int CMSIS_DAP_JTAG_GetIDCODEs(RDDIHandle handle, int *count, uint32_t *idcodes);
```

返回扫描到的 **IDCODE 列表**与数量。实现**优先用与 `GetDeviceIDList` / `DetectDAPIDList`
同一张目标表**（协议无关），表为空时才回退固件的 JTAG 扫描命令。

> 这是 `CMSIS_DAP_*` 里**名字带 `JTAG_` 但协议无关**的典型：**SWD 会话同样会调用它**。
> **“扫不到”不是错误** —— 返回 `RDDI_SUCCESS` + `*count = 0`（否则宿主不会清空列表、
> 会残留上一台的显示）。`idcodes` 可为 `NULL`（只取数量）。官方按 4 参调用
> （多一个 `sizeOfArray`），本实现忽略它。

```c
int cnt = 0; uint32_t codes[8] = {0};
pfn_CMSIS_DAP_JTAG_GetIDCODEs(hd, &cnt, codes);
if (cnt > 0) printf("IDCODE[0] = 0x%08X\n", codes[0]);  // 例 0x4BA00477
```

### 7.4 `CMSIS_DAP_JTAG_GetIRLengths`

```c
int CMSIS_DAP_JTAG_GetIRLengths(RDDIHandle handle, int *count, uint8_t *lengths);
```

返回链上**每个器件的 IR 位数**（`lengths[0]` = 离 TDO 最近者）与器件数。
**SWD 模式一律报 `count = 0`**（避免宿主误以为链上有 JTAG 器件）。

```c
int cnt = 0; uint8_t lens[8] = {0};
pfn_CMSIS_DAP_JTAG_GetIRLengths(hd, &cnt, lens);   // JTAG: 例 cnt=2, lens={4,5}
```

---

## 8. PC 采样接口

### 8.1 `CMSIS_DAP_PC_Capture`

```c
int CMSIS_DAP_PC_Capture(RDDIHandle handle, uint8_t control);
```

`control` bit0：`1` 启动采样，`0` 停止。

> ⚠️ **2026-10-08 起本层不再本地解码**：原实现用内置 ITM 解码器从 trace 里抽 DWT PC Sample，
> 该解码链已随解码器一起移除。`CMSIS_DAP_PC_*` 导出**保留**（ABI 兼容：名字 / 序号 / 签名不变），
> 但 `PC_GetData` / `PC_GetValues` **不再返回采样值**（恒 0 条）。
> 需要 PC 采样请在 **AGDI 层**做（ORBAGDI 已在解码 DWT PC Sample 并喂给 `AG_HistFunc`）。

```c
pfn_CMSIS_DAP_PC_Capture(hd, 0x01);   // 启动
/* ... 运行一段时间 ... */
pfn_CMSIS_DAP_PC_Capture(hd, 0x00);   // 停止
```

### 8.2 `CMSIS_DAP_PC_GetNumberOfChannels`

```c
int CMSIS_DAP_PC_GetNumberOfChannels(RDDIHandle handle, int *count);
```

```c
int ch = 0;
pfn_CMSIS_DAP_PC_GetNumberOfChannels(hd, &ch);
```

### 8.3 `CMSIS_DAP_PC_GetChannelInfos`

```c
int CMSIS_DAP_PC_GetChannelInfos(RDDIHandle handle, int *count, void *infos);
```

`infos` 传 `NULL` 时只回填通道数；否则按 `*count` 个填充 `{type, capacity, width}`（每项 12 字节）。

```c
struct { uint32_t type, capacity, width; } infos[8];
int ch = 8;
pfn_CMSIS_DAP_PC_GetChannelInfos(hd, &ch, infos);
```

### 8.4 `CMSIS_DAP_PC_GetCommonFrequency`

```c
int CMSIS_DAP_PC_GetCommonFrequency(RDDIHandle handle, uint32_t *frequency);
```

```c
uint32_t freq = 0;
pfn_CMSIS_DAP_PC_GetCommonFrequency(hd, &freq);   // 默认 10 MHz
```

### 8.5 `CMSIS_DAP_PC_GetData` / `CMSIS_DAP_PC_GetValues`

```c
int CMSIS_DAP_PC_GetData  (RDDIHandle handle, int *count, uint8_t *data);    // *count 是字节数
int CMSIS_DAP_PC_GetValues(RDDIHandle handle, int *count, uint32_t *values); // *count 是元素个数
```

读取已缓存的 PC 采样值（读后从缓存移除）。

```c
uint32_t vals[256];
int n = 256;
if (pfn_CMSIS_DAP_PC_GetValues(hd, &n, vals) == RDDI_SUCCESS) {
    for (int i = 0; i < n; i++) printf("PC[%d] = 0x%08X\n", i, vals[i]);
}

uint8_t raw[1024];
int m = sizeof(raw);                 // GetData 用字节数
pfn_CMSIS_DAP_PC_GetData(hd, &m, raw);
```

---

## 9. StreamingTrace 接口

`Connect` 的 `mode`：`0`=none，`1`=SWO，`2`=ETM。

> ⚠️ **本层没有 ETM 通路**：`StreamingTrace_*` 只服务 **SWO 流式传输**（sink 名
> `cmsis_dap_swo_trace`）；`mode=2` 不构成 ETM 数据来源（见 [COMPAT_ANALYSIS.md §17.9](COMPAT_ANALYSIS.md)）。
> orbtrace 的 trace 端点走 [§10](#10-orbtrace-配置与-raw-trace-流) 的 `TraceStream_*`。

### 9.1 `StreamingTrace_Attach` / `StreamingTrace_Detach`

```c
int StreamingTrace_Attach(RDDIHandle handle, const char *sinkName);
int StreamingTrace_Detach(RDDIHandle handle);
```

```c
pfn_StreamingTrace_Attach(hd, "swo");   // 初始化 trace 子系统
/* ... */
pfn_StreamingTrace_Detach(hd);
```

### 9.2 `StreamingTrace_Connect` / `StreamingTrace_Disconnect`

```c
int StreamingTrace_Connect(RDDIHandle handle, const char *sinkName, int mode);
int StreamingTrace_Disconnect(RDDIHandle handle);
```

```c
pfn_StreamingTrace_Connect(hd, "swo", 1);   // mode=1 SWO
pfn_StreamingTrace_Disconnect(hd);
```

### 9.3 `StreamingTrace_Start` / `StreamingTrace_Stop` / `StreamingTrace_Flush`

```c
int StreamingTrace_Start(RDDIHandle handle);   // 需先 Connect
int StreamingTrace_Stop (RDDIHandle handle);
int StreamingTrace_Flush(RDDIHandle handle);   // 清空内部缓冲
```

```c
pfn_StreamingTrace_Attach(hd, "swo");
pfn_StreamingTrace_Connect(hd, "swo", 1);
pfn_StreamingTrace_Start(hd);
/* ... 采集 ... */
pfn_StreamingTrace_Stop(hd);
pfn_StreamingTrace_Flush(hd);
pfn_StreamingTrace_Disconnect(hd);
pfn_StreamingTrace_Detach(hd);
```

### 9.4 `StreamingTrace_WaitForEvent`

```c
int StreamingTrace_WaitForEvent(RDDIHandle handle, int timeoutMs, int *eventType);
```

轮询等待，超时返回 `RDDI_SUCCESS` 且 `*eventType = 0`；有数据时 `*eventType = 1`。

```c
int ev = 0;
pfn_StreamingTrace_WaitForEvent(hd, 1000, &ev);
if (ev == 1) { /* 有 trace 数据可读 */ }
```

### 9.5 `StreamingTrace_SubmitEventBuffer`

```c
int StreamingTrace_SubmitEventBuffer(RDDIHandle handle, uint8_t *buffer, int bufferSize);
```

```c
uint8_t tb[4096] = {0};
pfn_StreamingTrace_SubmitEventBuffer(hd, tb, sizeof(tb));
```

### 9.6 `StreamingTrace_GetSinkCount`

```c
int StreamingTrace_GetSinkCount(RDDIHandle handle, int *count);
```

```c
int sinks = 0;
pfn_StreamingTrace_GetSinkCount(hd, &sinks);   // SWO 流式 sink（本层无 ETM sink）
```

### 9.7 `StreamingTrace_GetSinkDetails`

```c
int StreamingTrace_GetSinkDetails(RDDIHandle handle, int reserved1, int reserved2);
```

Keil 扩展，3 参且不写调用方缓冲区；当前返回 `RDDI_DAP_LEVEL1_NOT_IMPL`。

```c
int rc = pfn_StreamingTrace_GetSinkDetails(hd, 0, 0);
```

### 9.8 `StreamingTrace_GetConfigItem` / `StreamingTrace_SetConfigItem`

```c
int StreamingTrace_GetConfigItem(RDDIHandle handle, int item, int *value);
int StreamingTrace_SetConfigItem(RDDIHandle handle, int item, int value);
```

`item`：`0`=波特率，`1`=缓冲大小，`2`=模式。

```c
int baud = 0;
pfn_StreamingTrace_GetConfigItem(hd, 0, &baud);        // 默认 115200
pfn_StreamingTrace_SetConfigItem(hd, 0, 921600);       // 改波特率
pfn_StreamingTrace_SetConfigItem(hd, 2, 1);            // mode = SWO
```

---

## 10. orbtrace 配置与 raw trace 流

> **2026-10-08 新增，2026-10-13 补齐**（共 12 个导出，设计与依据见 [COMPAT_ANALYSIS.md §二十三](COMPAT_ANALYSIS.md)）。
>
> 本层**统一管理 USB**：orbtrace 复合设备上除 CMSIS-DAP 之外的三个 vendor 接口由本层打开与配置
> （`bInterfaceClass = 0xFF`，按 `bInterfaceSubClass` 区分：`0x50` Power / `0x54` Trace /
> `0x56` Version）。AGDI 只从这里取 **raw trace 字节流**，自行解码。
>
> ⚠️ **未实机验证**。非 orbtrace 设备上 `OrbTrace_Available` 返回 `*available = 0`，
> 其余接口返回错误码（不崩溃，也不影响 DAP 通路）。

### 10.1 设备探测与信息

```c
int OrbTrace_Available(RDDIHandle handle, int *available);        // *available: 1/0
int OrbTrace_GetInfo  (RDDIHandle handle, void *info, int size);  // size = sizeof(ORBMDK_OrbTrace_Info)
```

```c
#include "../include/ORBMDK_OrbTrace.h"

int avail = 0;
pfn_OrbTrace_Available(hd, &avail);
if (avail) {
    ORBMDK_OrbTrace_Info info;
    pfn_OrbTrace_GetInfo(hd, &info, sizeof(info));
    printf("orbtrace %04X:%04X '%s' sn='%s' ver='%s' traceIf=%d OFLOW=%d\n",
           info.vid, info.pid, info.product, info.serial, info.version,
           info.traceIf, info.supportsOFLOW);
}
```

`ORBMDK_OrbTrace_Info` 字段：

| 字段 | 说明 |
|------|------|
| `present` / `opened` | 1 = 找到 / 已打开 |
| `vid` / `pid` | 设备身份（`1209:3443` 或 `1209:3442`） |
| `traceIf` / `powerIf` / `versionIf` | 三个 vendor 接口号（`-1` = 无） |
| `traceProtocol` | `0x00` 未知 / `0x01` TPIU raw / `0x10` OFLOW v1.0 |
| `supportsOFLOW` | 1 = trace 接口 protocol == `0x10` |
| `product` / `serial` / `version` | 产品名 / 序列号 / 探针版本串（已去 `"Version: "` 前缀） |

### 10.2 配置 trace 源（对应 orbuculum `OrbtraceIfSet*`）

```c
int OrbTrace_SetTraceWidth   (RDDIHandle handle, int width);          // 1/2/3 (= 1/2/4-bit)；4 视作 3
int OrbTrace_SetTraceSWO     (RDDIHandle handle, int isManchester, int useTPIU);
int OrbTrace_SetSWOBaudrate  (RDDIHandle handle, int baud);           // Hz
int OrbTrace_SetVoltage      (RDDIHandle handle, int channel, int millivolts);
int OrbTrace_SetVoltageEnable(RDDIHandle handle, int channel, int on);
```

供电通道（`include/ORBMDK_OrbTrace.h`）：`ORBMDK_ORBTRACE_CH_VTREF`(0) / `CH_VTPWR`(1) /
`CH_ALL`(0xFF)。`SetVoltage` 的毫伏须在 **900..5010** 内，否则返回 `RDDI_DAP_ERROR`。

```c
pfn_OrbTrace_SetTraceWidth(hd, 4);                              // 4-bit 并口
pfn_OrbTrace_SetTraceSWO(hd, 0, 1);                             // UART 编码，经 TPIU
pfn_OrbTrace_SetSWOBaudrate(hd, 2000000);
pfn_OrbTrace_SetVoltage(hd, ORBMDK_ORBTRACE_CH_VTPWR, 3300);
pfn_OrbTrace_SetVoltageEnable(hd, ORBMDK_ORBTRACE_CH_ALL, 1);
```

### 10.3 打开 / 读取 raw trace 流

```c
int TraceStream_Open  (RDDIHandle handle, const char *config);   // config 可为 NULL
int TraceStream_Close (RDDIHandle handle);
int TraceStream_Read  (RDDIHandle handle, int *numBytes, void *buffer, int timeoutMs);
int TraceStream_Status(RDDIHandle handle, int *count, int *status);
int TraceStream_DroppedBytes(RDDIHandle handle, unsigned long long *dropped);  // 环溢出累计丢帧字节数
```

`config` 选项串（分号分隔，**可选**）：`"width=4;swo=uart|manchester;tpui=0|1;baud=<Hz>"`。

`TraceStream_Read`：`*numBytes` 进 = 缓冲容量、出 = 实际字节数；`timeoutMs`：`0` = 非阻塞、
`<0` = 阻塞到有数据。返回 `RDDI_SUCCESS`(0) = 有数据、**`0x204`** = 本轮无数据
（与 `StreamingTrace_WaitForEvent` 同口径）、其他 = 错误。

```c
pfn_TraceStream_Open(hd, "width=4;swo=uart;baud=2000000");

uint8_t buf[8192];
for (int i = 0; i < 100; ++i) {
    int n = (int)sizeof(buf);
    const int rc = pfn_TraceStream_Read(hd, &n, buf, 100);
    if (rc == RDDI_SUCCESS && n > 0) {
        /* buf[0..n) 是 raw trace 字节（ITM/ETM 语义由 AGDI 层解释） */
    } else if (rc == 0x204) {
        /* 本轮无数据 */
    } else {
        break;   // 出错（设备拔出 / 端点未打开）
    }
}

pfn_TraceStream_Close(hd);
```

`TraceStream_Status` 的 `*status` 位标志：`bit0` 已打开 / `bit1` trace 接口支持 OFLOW v1.0 /
`bit2` 有 power 接口；`*count` = 已打开(0/1)。

`TraceStream_DroppedBytes`：返回**自 `TraceStream_Open` 起**环形缓冲写满（消费者跟不上生产者）
而累计丢弃的字节数（`Open` 时清零）。`TraceStream_Read` 只能说明"读到了多少"；
当读到的字节少于探针实际发出的量时，靠这个计数区分"探针没发"与"环满丢了"。

> **注意**：`TraceStream_Read` 目前交的是 **raw USB 字节**（OFLOW / COBS 帧**未拆**）。
> 需要纯 payload 时按 [COMPAT_ANALYSIS.md §23.2](COMPAT_ANALYSIS.md) 自行解帧
> （COBS 去分隔 + `tag(1) | payload | sum(1)` 校验）。

---

## 11. 日志回调

### 11.1 `RDDI_SetLogCallback`

```c
void RDDI_SetLogCallback(RDDIHandle handle, RDDILogCallback pfn, void *context, int maxLogLevel);
```

回调签名（注意顺序是 `context, msg, logLevel`）：

```c
typedef void (*RDDILogCallback)(void *context, const char *const msg, const int logLevel);
```

日志级别：`RDDI_LOGLEVEL_FATAL`=0 … `RDDI_LOGLEVEL_TRACE`=5。

```c
static void on_log(void *ctx, const char *msg, int level)
{
    const char *lv[] = { "FATAL","ERROR","WARN","INFO","DEBUG","TRACE" };
    printf("[%s] %s\n", (level >= 0 && level <= 5) ? lv[level] : "?", msg);
}

/* 注册（maxLogLevel 调小可减少转发量） */
pfn_RDDI_SetLogCallback(hd, on_log, NULL, RDDI_LOGLEVEL_INFO);

/* 取消：传 NULL */
pfn_RDDI_SetLogCallback(hd, NULL, NULL, 0);
```

> 回调在**锁外**调用，可以在回调里安全地调用其他函数。
> 运行期还可用 `%TEMP%\ORBMDK_LOG_LEVEL` 文件调整阈值（0=DEBUG…3=ERROR），无需重启进程。

---

## 12. 错误码速查

通用错误码（`RDDI_SUCCESS` = 0 表示成功）：

| 宏 | 值 | 说明 |
|----|-----|------|
| `RDDI_SUCCESS` | 0x0000 | 成功 |
| `RDDI_BADARG` | 0x0001 | 参数非法 |
| `RDDI_INVHANDLE` | 0x0002 | 句柄无效 |
| `RDDI_FAILED` | 0x0003 | 一般失败 |
| `RDDI_BUFFER_OVERFLOW` | 0x0005 | 缓冲区溢出 |

DAP 错误码（0x1000 段，AGDI 按数值识别并做恢复）：

| 宏 | 值 | 说明 |
|----|-----|------|
| `RDDI_DAP_OPERATION_TIMEOUT` | 0x100E | 超时（AGDI 会写 ABORT 重试） |
| `RDDI_DAP_DP_STICKY_ERR` | 0x100F | DP 粘滞错误（AGDI 会清 sticky 重试） |
| `RDDI_DAP_LEVEL1_NOT_IMPL` | 0x1006 | 该功能未实现 |
| `RDDI_DAP_BAD_REGISTER_ID` | 0x1011 | 寄存器 ID 不支持 |

Keil 扩展错误码（0x2000 段）：`RDDI_DAP_ERROR`(0x2000)、`RDDI_DAP_ERROR_MEMORY`(0x2005) 等。

---

## 13. 注意事项（易踩的坑）

1. **输出参数总是先写**：所有带标量出参的接口，即使句柄无效也会先把出参写成 `0`/空串再返回错误。
   调用方应**先看输出值、再看返回值**（AGDI 就是只看输出值）。

2. **`sizeOfArray` 的单位不统一**：
   - `DAP_GetDAPIDList` / `CMSIS_DAP_GetDeviceIDList`：**字节数**（也兼容元素个数）。
   - `CMSIS_DAP_DetectDAPIDList`：**元素个数**。
   - `CMSIS_DAP_PC_GetData` 的 `*count`：字节数；`CMSIS_DAP_PC_GetValues` 的 `*count`：元素个数。

3. **`CMSIS_DAP_SWJ_Sequence` 的 `num` 是位数**，不是字节数；不要用 `sizeof()`。

4. **Block 与 Repeat 语义不同**：
   - `DAP_Reg*Block`：数组里是**多个不同 regID**。
   - `DAP_Reg*Repeat`：**同一 regID 重复 N 次**。

5. **`CMSIS_DAP_Connect` 的第 2 参不一定是有效指针**：AGDI 会把它当作“选中项索引”传入，
   DLL 内部已做可写性校验；自己调用时应传真实 `int*`。

6. **未实现的接口返回 `RDDI_DAP_LEVEL1_NOT_IMPL`**（0x1006），不会崩溃，可用于探测：
   `DAP_DefineSequence`、`DAP_RunSequence`、`StreamingTrace_GetSinkDetails`、
   `CMSIS_DAP_Atomic_Control`、`CMSIS_DAP_Atomic_Result`。

7. **接口选择必须显式**：`CMSIS_DAP_ConfigureInterface(ifNo)` 一旦选定，通道打不开会**报错**，
   不会静默回退；因此调用前建议先 `CMSIS_DAP_Detect` 确认接口数。

8. **32 位 DLL**：Keil µVision 是 32 位进程，必须用 x86 构建的 `ORBMDK_RDDI.dll`；
   覆盖 DLL 前需结束整个 `UV4.exe`（µVision 会把 DLL 常驻内存）。

9. **JTAG 与 SWD 是两条独立建链路径**（由 `CMSIS_DAP_ConfigureInterface` 的 `Port=` 决定）：
   - `Port=SW`：`Connect` 后补 **JTAG-to-SWD** 切换序列（`0xE79E`）再读 DPIDR；
   - 其它（如 `Port=JTAG`）：`Connect` 走 **SWD-to-JTAG** 切换序列（`0xE73C`）+ 扫链 + 读 DP IDCODE。

   同一会话内请固定一条。JTAG 下 `CMSIS_DAP_Connect` 建链时会**临时把 SWJ 时钟限到 4 MHz**
   （JTAG 裕量）。若 JTAG 扫链为空，先查 **TDI/TDO 接线**与目标 JTAG 是否使能，再考虑目标是否
   刚被 SWD 连接过（部分 SWJ-DP 需目标重新上电才回到 JTAG）。日常调试优先选 **SWD**。
