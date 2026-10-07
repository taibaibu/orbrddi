# ORBMDK 兼容性分析与实现状态

> 分析日期：2026-09-29
> 实机联调：2026-09-30（ORBTrace 调试器 + STM32F1 目标）
>
> 本文档只保留**当前仍然有效**的结论。已过时的历史条目（导出符号数量快照、"已实现/缺失函数"逐条清单、
> 编译阻塞排查过程、与 RDDI 参考项目的逐项代码对比、各轮修改文件清单等）已删除或合并，避免与源码现状矛盾。

---

## 一、当前状态总览

| 项 | 状态 |
|----|------|
| 编译 | ✅ `build.ps1` 全量编译通过，**无 error、无 warning** |
| 运行库 | ✅ **静态 CRT（`/MT`）**，产物不依赖 `MSVCP140`/`VCRUNTIME140`/UCRT（原因见第十二节） |
| 输出 | `bin/ORBMDK_RDDI.dll` |
| 导出符号 | `ORBMDK_RDDI.h` 中带 `RDDI_FUNC` 的导出函数共 **78 个** |
| 实机验证 | ✅ `ORBMDK_RDDI_FullTest.exe` **40/40 全部通过** |
| 目标识别 | ✅ DP IDCODE `0x2BA01477`、AP IDR `0x24770011`（STM32F1） |
| JTAG 通路 | ✅ **已打通**（§18.9 第九步）：`Port=JTAG` 建链成功；扫链 IDCODE `0x4BA00477`、DP CTRL/STAT `0xF0000000`、AP IDR `0x24770011`，与 OpenOCD 交叉一致；SWD 无回归 |
| Keil 联调 | ✅ 官方 AGDI v1.33.24 下对话框正确显示 `IDCODE 0x2BA01477` / `ARM CoreSight SW-DP`（见 4.7） |
| Flash 下载 | ✅ 烧录与调试均验证通过（擦除失败根因见 4.8） |
| 烧录速率 | 🔧 块传输已接入且**能力探测通过**（日志 `Block transfer probe OK`，§13.7 更正）；`test/ORBMDK_BlockTransferTest.exe` 的越界/提速验证**待目标在线时补跑**（§9.9 的验证要求） |
| 传输模式 | ✅ V1(HID) / V2(Bulk) 两条在 Keil 适配器列表里都可选可切换（§14），**V2 已实机可用**：完整调试 + 下载（§17.3 / §17.5 B 臂） |
| V2 通道真因 | ✅ 句柄泄漏导致 WinUSB 接口被**永久**独占（§17.1）+ 出包长度必须用设备自报的 508、**绝不发整包**（§17.2） |
| 传输层选择 | ✅ 只认对话框里选中的那条接口；**已删除全部隐藏开关**（`ORBMDK_TRANSPORT` / `ORBMDK_FWVER` / `ORBMDK_BULK_PAD` / `ORBMDK_SERIAL`），选中通道打不开就如实报错（§17.4） |
| 固件版本门 | ✅ AGDI 用 `"%lu.%lu.%lu"` 解析 `Identify(idNo=4)` 并 `cmp eax,2`，主版本 ≥ 2 切另一套驱动逻辑（§17.6 反汇编）；本层钉 `1.0.0`（§17.5 单变量 A/B） |
| 日志 | ✅ 单一实现 `src/ORBMDK_Log.cpp`（§8.5）：多 Sink（文件 / stdout / DebugView / 宿主回调）、`%TEMP%\ORBMDK_LOG_LEVEL` 热更新、线程安全；`RDDI_SetLogCallback` 已接通 Keil 日志窗口 |

**导出符号分类（78 个）**

| 分类 | 数量 |
|------|------|
| RDDI 核心（Open/Close/GetLastError） | 3 |
| DAP 基础 + 寄存器 | 20 |
| CMSIS_DAP_* | 30 |
| PC_* | 6 |
| StreamingTrace_* | 13 |
| RDDI_SetLogCallback | 1 |

> `ORBMDK_USB_Bulk.cpp` 中的 `ORBMDK_USB_Bulk_*` / `CMSIS_DAP_V2_*` / `ORBMDK_Bulk_*` 使用 `ORBMDK_INTERNAL`（构建 DLL 时为空宏），**不导出**，属内部接口。

---

## 二、AGDI 层调用的 RDDI 函数清单

### 2.1 加载机制

**AGDI 层 = Keil 的 "CMSIS-DAP Debugger" = elaphureLinkAGDI**，三者是同一个东西。
它通过**函数指针表**间接调用 RDDI，而非静态链接：

- 指针声明：`DbgCM/rddi_dll.hpp`；定义：`DbgCM/rddi_dll.cpp`
- 加载：`LoadRddiDllFunction()`（`DbgCM/DbgCM.cpp:128`），由 `CDbgCMApp::InitInstance()` 调用
- 所有调用点均带 `rddi::` 限定（已全量校验，无未限定调用）

```cpp
auto handle = LoadLibrary("elaphureRddi.dll");   // elaphureLink 定制版按此名加载
rddi::rddi_Open  = (decltype(rddi::rddi_Open))GetProcAddress(handle, "RDDI_Open");
rddi::rddi_Close = (decltype(rddi::rddi_Close))GetProcAddress(handle, "RDDI_Close");
```

> ⚠️ **DLL 命名约束**：AGDI 层按**固定文件名**加载 RDDI 层 DLL。
> Keil 原版 "CMSIS-DAP Debugger" 加载 **`CMSIS_DAP.dll`**（elaphureLink 定制版为 `elaphureRddi.dll`）。
> ORBMDK 的构建产物为 `ORBMDK_RDDI.dll`，需重命名/复制为**实际生效的那个名字**并放到 Keil 的 `ARM\BIN\` 下。
>
> ⚠️ **位数约束**：Keil µVision 是 32 位进程，RDDI 层 DLL **必须是 x86**（`build.ps1` 已默认 x86）。
> 用 x64 编译的 DLL 无法被加载，Keil 会报成"DLL missing"。

### 2.2 实际被调用的函数（15 个 / 56 处）

**RDDI 核心（2）**

| 函数 | 调用点 | 所在 AGDI 函数 |
|------|--------|----------------|
| `RDDI_Open` | `DbgCM.cpp:176` | `RddiOpenInstance()` |
| `RDDI_Close` | `DbgCM.cpp:191` | `RddiCloseInstance()` |

**DAP 寄存器访问（5 个 / 36 处）—— 调用主力**

| 函数 | 处数 | 调用点（SWD / JTAG） |
|------|------|----------------------|
| `DAP_RegAccessBlock` | **20** | `SWD_ReadData`/`SWD_WriteData`、`SWD_GetARMRegs`、`SWD_SetARMRegs`、`SWD_SysCallExec`、`SWD_SysCallRes`（`SWD.cpp:260/289/1861/1898/1996/2036/2087/2130/2177/2197`）<br>`JTAG.cpp:270/299/1890/1927/2025/2065/2116/2159/2205/2225`（同构） |
| `DAP_WriteReg` | 6 | `SWD_WriteDP`(368)、`SWD_WriteAP`(457)、`SWD_DAPAbortVal`(2351)<br>`JTAG_WriteDP`(378)、`JTAG_WriteAP`(467)、`JTAG_DAPAbortVal`(2385) |
| `DAP_ReadReg` | 4 | `SWD_ReadDP`(321)、`SWD_ReadAP`(410)<br>`JTAG_ReadDP`(331)、`JTAG_ReadAP`(420) |
| `DAP_RegWriteRepeat` | 4 | `SWD_WriteBlock`(1247)、`SWD_VerifyBlock`(1357)<br>`JTAG_WriteBlock`(1281)、`JTAG_VerifyBlock`(1389) |
| `DAP_RegReadRepeat` | 2 | `SWD_ReadBlock`(1118)、`JTAG_ReadBlock`(1152) |

**CMSIS-DAP 接口（8 个 / 18 处）**

| 函数 | 处数 | 调用点 | 所在 AGDI 函数 |
|------|------|--------|----------------|
| `CMSIS_DAP_Identify` | 6 | `PDSCDebug.cpp:2778,2789`<br>`SetupDbg.cpp:154,186,196,200` | `PDSCDebug_InitDebugger`<br>配置对话框（idNo=2 产品名 / 3 序列号 / 4 固件版本） |
| `CMSIS_DAP_Detect` | 2 | `PDSCDebug.cpp:2769`、`SetupDbg.cpp:152` | 枚举接口数 |
| `CMSIS_DAP_ConfigureInterface` | 2 | `PDSCDebug.cpp:2803`、`SetupDbg.cpp:225` | 传入 `Master=Y;Port=SW;SWJ=Y;Clock=...` |
| `CMSIS_DAP_DetectNumberOfDAPs` | 2 | `SWD.cpp:160`、`JTAG.cpp:3102` | `SWD_ReadID`、`JTAG_DetectDevices` |
| `CMSIS_DAP_DetectDAPIDList` | 2 | `SWD.cpp:164`、`JTAG.cpp:3110` | 同上 |
| `CMSIS_DAP_Commands` | 2 | `AGDI.CPP:2375`、`PDSCDebug.cpp:4026` | `ResetTarget`、`PDSCDebug_ResetHardware`（**仅发 `ID_DAP_ResetTarget`**） |
| `CMSIS_DAP_SWJ_Sequence` | 1 | `SWD.cpp:3302` | `SWD_SWJ_Sequence` |
| `CMSIS_DAP_SWJ_Pins` | 1 | `AGDI.CPP:17610` | `DapAcc_Pins` |

### 2.3 加载了但从未调用（2 个）

`DbgCM.cpp:149-150` 加载，全工程无调用点：`CMSIS_DAP_ConfigureDAP`、`CMSIS_DAP_Capabilities`。

### 2.4 声明但未加载、未调用

`rddi_dll.cpp` 中以下指针为空声明，既不在加载列表也无调用：`DAP_GetNumberOfDAPs`、`DAP_GetDAPIDList`。

以下在 `rddi_dll.cpp` 中被**注释掉**（声明保留、定义与加载均无）：
`RDDI_GetLastError`、`RDDI_SetLogCallback`、`DAP_GetInterfaceVersion`、`DAP_Configure`、`DAP_Connect`、
`DAP_Disconnect`、`DAP_GetSupportedOptimisationLevel`、`DAP_RegReadBlock`、`DAP_RegWriteBlock`、
`DAP_RegReadWaitForValue`、`DAP_Target`、`DAP_DefineSequence`、`DAP_RunSequence`、`CMSIS_DAP_GetGUID`。

### 2.5 结论

AGDI 实际只依赖 **15 个** RDDI 函数。ORBMDK 其余导出（`StreamingTrace_*` 13 个、`CMSIS_DAP_SWO_*` 4 个、
`CMSIS_DAP_JTAG_*` 4 个、`CMSIS_DAP_PC_*` 6 个、`CMSIS_DAP_Atomic_*` 2 个、`CMSIS_DAP_SWD_*`、
`DAP_HostStatus`、`DAP_SetCommTimeout` 等）**AGDI 层完全不涉及**，只服务于其他宿主（Trace 工具），
不影响 Keil 调试链路。

两个关键契合点（ORBMDK 现有实现恰好覆盖）：

- `CMSIS_DAP_Commands` 只被用于 `ID_DAP_ResetTarget`，而 ORBMDK 的实现也只处理这一个命令，其余返回 `RDDI_BADARG` —— 实际不会触发。
- `CMSIS_DAP_Identify` 的 `idNo` 取值 2/3/4，与 ORBMDK 的 产品名/序列号/固件版本 映射一致。

---

## 三、协议实现要点（固化约束）

### 3.1 DAP_Transfer 响应字节序（关键）

```
< 0x05 | Transfer Count | Transfer Response | [TD_TimeStamp] | [Transfer Data]
```

- `Transfer Count`：实际执行的传输次数（1..255），**在 Transfer Response 之前**。
- `Transfer Response`：Bit[2:0] = ACK；Bit[3] = Protocol Error；Bit[4] = Value Mismatch。
- `Transfer Data`：仅读操作返回，从响应第 4 字节开始。

> ⚠️ 曾把「计数」与「状态」顺序颠倒，导致把 `Transfer Count=0` 误读为状态「成功」，再把数据区残留字节
> 读成无意义值（实机表现为 `IDCODE = 0x00030003`）。`DAP_TransferBlock` 同理，状态在 `resp[4]`。

### 3.2 DAP_Transfer 请求字节序

```
> 0x05 | DAP Index | Transfer Count | Transfer Request | [Transfer Data]
```

- `Transfer Request` 位定义：Bit0=APnDP, Bit1=RnW, Bit2=A2, Bit3=A3, Bit4=Value Match, Bit5=Match Mask, Bit7=TD_TimeStamp。
- 现有 `GetRegOffset` + `| 0x02`（读）/ `offset`（写）的编码与该定义一致。

### 3.3 HID 报告ID 处理

CMSIS-DAP V1 走 USB HID，IN/OUT 报告首字节为**报告ID（0x00）**：

- OUT：`reportOut[0] = 0x00`（报告ID），命令从 `reportOut[1]` 开始，总长 65 字节（1+64）。
- IN：`ReadFile` 返回的数据首字节为报告ID，响应数据从 `resp[1]` 开始。

**全层统一约定**（所有 `DAP_*` / `CMSIS_DAP_*` 解析必须遵守）：

```
resp[0] = 报告ID（通常 0x00）
resp[1] = 命令ID
resp[2..] = 响应负载
```

> 实测 orbtrace 固件 V1 路径即为 64 字节 IN 报告且含报告ID前缀，无需引入报告描述符动态解析。
> 历史上多处解析漏掉该偏移（读到命令ID），已全部修正，见第四节。

### 3.4 JTAG-to-SWD 切换序列（必须）

标准 CMSIS-DAP 规范要求 `DAP_Connect(port=SWD)` 由固件内部执行 SWD 连接序列。但 orbtrace 固件
`dbgIF.v` 的 `CMD_SET_SWD` 内部切换序列 line reset 仅 **50 个周期**（恰好卡在 ADIv5 规范"至少 50"的临界值），
不足以让 STM32F1 目标可靠进入 SWD 模式。

**主机侧必须在 `DAP_Connect` 之后补发一次标准切换序列**（与 OpenOCD 的 `cmsis_dap_swd_switch_seq` 一致）：

```
≥50 位 1  →  16-bit 0xE79E (LSB first)  →  ≥50 位 1  →  idle
对应实现：DAP_SWJ_Sequence(56,{0xFF×7}) → (16,{0x9E,0xE7}) → (56,{0xFF×7}) → (8,{0x00})
```

> ⚠️ 顺序不可颠倒：手动序列必须放在 Connect **之后**，放在之前会被固件内部切换覆盖。
> `CMSIS_DAP_Connect` 与 `CMSIS_DAP_ResetDAP` 均需执行该序列。

### 3.5 ACK 编码映射

orbtrace 固件 `RESP_Transfer_Process` 的响应字节为 `Cat(dbgif.ack, dbgif.perr, C(0,4))`，
`dbgif.ack` 直接透传 SWD 的 3 位 ACK，与内部 `DAP_RES_*` 的映射关系：

| 标准 ACK | 含义 | 内部 `DAP_RES_*` |
|----------|------|------------------|
| 1 | OK | `DAP_RES_OK`(0) |
| 2 | WAIT | `DAP_RES_WAIT`(1) |
| 4 | FAULT | `DAP_RES_FAULT`(2) |
| 7 | NO_ACK（SWDIO 未被驱动） | `DAP_RES_NO_ACK`(3) |

> `0x07` 是合法的"目标无响应"值，不是解析错位。

### 3.6 日志策略

| 项 | 约定 |
|----|------|
| 实现位置 | **`src/ORBMDK_Log.cpp`（唯一实现）+ `include/ORBMDK_Log.h`（宏）**。2026-09-30 之前是 `ORBMDK_RDDI.cpp` 的 `ORBMDK_Log` 与 `ORBMDK_HID.cpp` 的 `HID_Log` 两套实现（见 §8.5） |
| 默认级别 | **ERROR** |
| 阈值适用范围 | **控制台 / DebugView 与文件日志共用同一阈值**。文件日志曾写死 `level >= INFO` 而绕过阈值，已修正（见 4.10 #21） |
| 过滤时机 | 在构造日志字符串**之前**判断级别，被过滤时零格式化开销 |
| 行格式 | `[ORBMDK][HH:MM:SS.mmm][级别][模块][PID:TID] 消息` |
| 时间 / 线程维度 | 必带本地时间戳 + 进程 ID + 线程 ID。日志跨进程追加、AGDI 多线程调用，缺这两项无法界定"哪次运行、哪个线程" |
| 并发 | 文件写入用 `std::mutex` 串行化；文件句柄按行开关（UV4 会独占文件，外部诊断需要能读） |
| 缓冲区 | 1024 字节；超长时末尾写 `...` 标记，**不允许静默截断** |
| 可恢复故障 | `FAULT` / `WAIT` / `NO_ACK` 等 AGDI 会自行恢复的瞬时状态用 **WARN**，ERROR 只留给真正异常（见 4.10 #25） |
| 临时恢复 | **文件 `%TEMP%\ORBMDK_LOG_LEVEL`**（内容 `0=DEBUG 1=INFO 2=WARN 3=ERROR`）：µVision **运行中**改动最多 1 秒生效，删掉文件即恢复默认，**不必开命令行、不必重启 IDE**（见 11.1.3）。环境变量 `ORBMDK_LOG_LEVEL` 仍支持（文件名优先），但只在进程启动时读一次。日志路径用 `ORBMDK_LOG_FILE` |
| 禁止项 | 逐寄存器 / 逐次传输的 INFO 日志、响应十六进制转储 |

---

## 四、已修复问题记录

### 4.1 传输层

| # | 位置 | 问题 | 修复 |
|---|------|------|------|
| 1 | `include/ORBMDK.h` | HID 与 Bulk 各自定义 ORBTrace PID（`0x3456` / `0x6A02`），会匹配到不同设备 | 新增统一常量 `ORBMDK_ORBTRACE_VID = 0x1209` / `ORBMDK_ORBTRACE_PID = 0x3443`（依据 orbtrace 固件 `cmsis_test.py`、orbuculum `orbtraceIf.c`），两层共用 |
| 2 | `ORBMDK_HID.cpp` | `ORBMDK_HID_DAPCommand` 注释与实现矛盾（注释称"响应不含报告ID"） | 注释改为明确的响应约定说明 |
| 3 | `ORBMDK_HID.cpp` | `StreamingTrace_GetStatus` 按 `resp[1]/resp[2]/resp[3]` 解析，漏掉报告ID偏移 | 改为 `resp[2]`(Status) / `resp[3..4]`(Count)，要求 `respLen >= 5` |
| 4 | `ORBMDK_HID.cpp` | `DAP_Transfer` 每次调用输出 20 字节十六进制转储；`ORBMDK_HID_DAPCommand` 每次输出 IN 报告头 | 全部移除 |

### 4.2 RDDI 接口层（响应偏移类）

| # | 位置 | 问题 | 修复 |
|---|------|------|------|
| 5 | `CMSIS_DAP_SWO_Status` | 判断 `resp[0] == 1` 并读 `resp[1]`，`resp[0]` 实为报告ID(0x00) → 恒不成立 | 校验 `resp[1] == ID_DAP_SWO_STATUS`，状态取 `resp[2]` |
| 6 | `CMSIS_DAP_SWD_Sequence` | 判断 `resp[0] == 1` → 恒失败 | 校验 `resp[1] == ID_DAP_SWD_SEQUENCE`，状态取 `resp[2]` |
| 7 | `CMSIS_DAP_Atomic_Control` | 把报告ID当作响应值 | 改取第一个响应负载字节 `resp[2]` |
| 8 | `DAP_ReadReg` | AP 分支残留 `printf` 调试输出，且未检查 SELECT 写状态 | 改为 `LOG_DEBUG`，SELECT 写失败返回 `RDDI_DAP_ERROR` |
| 9 | `DAP_RegReadBlock` / `DAP_RegWriteBlock` | 逐寄存器 INFO 日志（块传输可达上万条） | 移除；失败时仅输出一条 ERROR |

### 4.3 解码器层

| # | 文件 | 问题 | 修复 |
|---|------|------|------|
| 10 | `ORBMDK_ITM_Decoder.*` | 同步字节误用 `0x70`（实为溢出包）；SW/HW 长度取自 `(c>>5)&3` 而非包头 bit[1:0]；HW PC Sample 固定预取 4 字节；进入新包不重置 `len` | 按 orbuculum `itmDecoder.c` 重写状态机：48 位移位寄存器同步（5×`0x00`+`0x80`）、源包长度由 bit[1:0] 决定（`0b11`→4 字节）、`0x70` 归为溢出、补齐 TS/GTS/XTN/NISYNC/RSVD 可变长包、IDLE 态清零负载缓冲 |
| 11 | `ORBMDK_ETM_Decoder.*` | `_decodeETMv35` 用 `len < 3` 判定，而 `_decodeETMv35ISYNC` 消费 4 字节 | 新增 `addrBytes`（默认 4），统一按 `need = 1 + addrBytes` 判定并返回缺失字节数 |
| 12 | `ORBMDK_TPIU_Decoder.*` | `frameLen` 判定与 `GetPacket` 条件互相矛盾；`PAYLOAD` 中遇到帧起始会静默丢弃上一包 | 重写状态机，新增显式 `packetReady` 标志；`GetPacket` 只检查该标志；未取走的包计入 `stats.overflow` |
| 13 | `ORBMDK_COBS.cpp` | `partialFrame` 存的是**编码后原始字节**而非解码结果；`DRAINING` 后未清空 `partialFrame` | 按 orbuculum `cobs.c` 实现真正的 COBS 解码（剥离块长度、还原被替换的 `0x00`）；新增 `frameReady` 标志，错误帧只排水不交付；每帧交付后复位状态 |

### 4.4 SWD 初始化根因（NO_ACK）

**现象**：`CMSIS_DAP_Connect` 返回 `Connected Interface: 1 (SWD)`（连接命令本身成功），但随后
`DAP_ReadReg(DP_IDCODE)` 返回 `Transfer Response = 0x07`（ACK=7=NO_ACK），IDCODE 读不出有效值。

**排查**：请求格式、request 字节、响应解析、报告ID 偏移**均无问题** —— 响应字节 `0x07` 是合法的
"目标无响应"（见 3.5）。`bin/fulltest_out*.txt` 中所有请求返回完全相同的 64 字节且 `Transfer Count = 0`，
也与"目标未响应"一致。

**根因**：主机侧缺少 Connect 之后的手动 JTAG-to-SWD 切换序列（见 3.4）。此前误删该序列，导致目标
未切换到 SWD → 所有 `DAP_Transfer` 均 `NO_ACK`。

**验证结果**：

| 项 | 修复前 | 修复后 |
|----|--------|--------|
| DP IDCODE | `0x00000000` / 垃圾值 | **`0x2BA01477`** ✅ |
| AP IDR | `0x00000000` | **`0x24770011`** ✅ |
| Test 5 寄存器读写 | 全部 NO_ACK | 全部 ACK=OK ✅ |
| 测试通过数 | 29/40 | **40/40**（ALL TESTS PASSED） ✅ |

### 4.5 日志收敛

| 项 | 处理 |
|----|------|
| 默认阈值 | `ORBMDK_RDDI.cpp` 由 `LOG_LEVEL_INFO` 提升为 **ERROR**；`ORBMDK_HID.cpp` 原**无级别判断**，新增阈值并默认 ERROR |
| 降级 | `RDDI_Open` / `DAP_Connect` / `CMSIS_DAP_Connect` / `CMSIS_DAP_ResetDAP` 等例行流程日志 `LOG_INFO` → `LOG_DEBUG` |
| 移除 | 逐寄存器日志、响应十六进制转储、`"HID WriteFile succeeded"`、残留调试行（`"DAP_WriteReg: success"`、`"DAP_RegWriteBlock: IsAP=..."`） |
| 可逆 | 环境变量 `ORBMDK_LOG_LEVEL`（详见 3.6） |

### 4.6 DAP 枚举与接口契约（实机 "No Debug Unit Found" 根因）

**现象**：x86 构建后 Keil 能加载 DLL，但启动调试时报 **"No Debug Unit Found"**（AGDI 的 `EU02`）。

**根因**：ARM `rddi_dap.h` 规定的调用顺序为

```
RDDI_Open → DAP_Configure → DAP_GetNumberOfDAPs → DAP_GetDAPIDList → DAP_Connect → ...
```

即 `DAP_GetNumberOfDAPs` / `DAP_GetDAPIDList` 会在**连接目标之前**被调用，且规范要求这两个函数
`does not communicate with the target`。而 ORBMDK 曾实现为返回 `ctx->dapIdList.size()`，
该列表只有 `CMSIS_DAP_DetectNumberOfDAPs`（需连接目标）才会填充 → Keil 拿到 0 个 DAP → `EU02`。

| # | 位置 | 问题 | 修复 |
|---|------|------|------|
| 14 | `DAP_GetNumberOfDAPs` | 返回 `dapIdList.size()`，连接目标前恒为 0（注释还写着"不再兜底为 1"，正好打掉了兜底） | 改为返回 `kSingleDapCount = 1`（ORBTrace 单 DAP，不通信目标） |
| 15 | `DAP_GetDAPIDList` | 列表为空时什么都不写 | 返回 `[kSingleDapId = 0]`；DAP ID 是**索引**（会被当作 CMSIS-DAP 的 DAP Index 字节），**不是 IDCODE** |
| 16 | `CMSIS_DAP_GetNumberOfDevices` | 同样返回 `dapIdList.size()` → 恒为 0 | 改为返回 `kSingleDapCount` |
| 17 | `CMSIS_DAP_DetectDAPIDList` | 目标未探测到时一个字节都不写，调用方（`SWD_ReadID`）读到未初始化值 | 列表为空时写入 `0` |
| 18 | 全部带标量输出参数的接口 | 句柄无效时**提前 return，不写输出参数** → 调用方读到初始值 0 或未初始化栈内存 | 统一改为"先写输出参数（0/空串），再判错返回" |
| 19 | `RDDI_Open` | `RDDIContext::productName / serialNumber / firmwareVersion` **从未赋值** → `CMSIS_DAP_Identify` 只能回退硬编码串、`GetDeviceIDList` 返回空串、`GetGUID` 得到 `"ORBTrace-"` | 在 `RDDI_Open` 中调用 `ORBMDK_HID_GetDeviceInfo()` 把 HID 层已取到的产品名/序列号/固件版本填入上下文 |
| 20 | `CMSIS_DAP_GetGUID` | 签名与 ARM 标准不符：标准为 `(handle, ifNo, str, len)`，ORBMDK 少了 `ifNo` → x86 `__cdecl` 下参数整体错位，`guid` 实收 `ifNo`(0/1)，`snprintf` 会向非法地址写入 | 补上 `ifNo`，与 `rddi_dap_cmsis.h` 对齐 |

**为什么"输出参数必须写入"是硬要求**：调用方只看输出值、不看返回值 ——

```c
rddi::CMSIS_DAP_Detect(rddi::k_rddi_handle, &numOfIFs);
if (numOfIFs == 0) {
    return EU02;              // "No Debug Unit Found"
}
```

`numOfIFs` 是调用方的局部变量（初值 0）。若 `CMSIS_DAP_Detect` 因句柄无效提前返回且不写它，
真实错误码（`RDDI_INVHANDLE`）被吞掉，对外表现为"没找到调试单元"。
`CMSIS_DAP_Identify` 更危险：不写 `str` 时 AGDI 会对**未初始化栈内存**做 `strcmp`。

**验证**：修复后 `ORBMDK_RDDI_FullTest.exe` 仍 **40/40 全部通过**，且 x86 构建无 error / 无 warning。

### 4.7 Keil 对话框显示 "IDCODE 0x00000000 / Unknown device" 根因

**现象**：官方 AGDI（`D:\Keil_v5\ARM\BIN\CMSIS_AGDI.dll` v1.33.24；旧环境为 `D:\MDK5\ARM\BIN\`）+ ORBMDK 组合下，
µVision 的 "CMSIS-DAP Cortex-M Target Driver Setup" 对话框里 SW Device 列表恒显示
`IDCODE 0x00000000` / `Device Name = Unknown device`，其余调试链路正常。

**定位**（反汇编官方 `CMSIS_AGDI.dll` 与官方 `CMSIS_DAP.dll.bak`）：

AGDI 用 `GetProcAddress` 把 55 个 RDDI 导出存进全局指针槽（`0x10362F08`–`0x10362FCC`），
**全程不做 NULL 检查**，取不到就是空指针直接调用。其中两组是**同名槽回退**：

| 首选名 | 回退名 | 槽 |
|--------|--------|-----|
| `CMSIS_DAP_GetDeviceIDList` | `CMSIS_DAP_DetectDAPIDList` | `0x10362F90` |
| `CMSIS_DAP_DetectNumberOfDevices` | `CMSIS_DAP_DetectNumberOfDAPs` | `0x10362F8C` |

因为 ORBMDK 两个首选名都导出，**官方 AGDI 只会调用 `GetDeviceIDList` / `DetectNumberOfDevices`**；
`DetectDAPIDList` / `DetectNumberOfDAPs` 在 Keil 下根本不会被调用（日志里那几条来自测试宿主）。

AGDI 的三处 `GetDeviceIDList` 调用点（`0x1002CC27` / `0x1002CCAF` / `0x100334CA`）
**全部传 `sizeOfArray = 0x100`（字节数）**，并把数组内容直接当作设备 ID 填入
`JTAG_devs.ic[i].id`。官方实现语义（`RVA 0x14FD0`）：

```asm
0x10014FEF  mov edx, 0x1005ab2c        ; 内部设备 ID 表
0x10014FF4  shr esi, 2                 ; arg3 是字节数，/4 得元素个数
0x10015004  mov edi, dword ptr [edx+eax]
0x10015007  mov dword ptr [eax], edi   ; 拷贝的是 **IDCODE**，不是索引
```

**根因**：ORBMDK 的 `CMSIS_DAP_GetDeviceIDList` 写的是 DAP 索引：

```cpp
idArray[i] = kSingleDapId + i;   // 恒为 0
```

→ AGDI 拿到 IDCODE = 0 → 名称查表落空 → `Unknown device`。

**修复**：把 IDCODE 探测逻辑从 `CMSIS_DAP_DetectNumberOfDAPs` 抽成公共函数
`DetectTargetDapIdList()`；`GetDeviceIDList` / `DetectDAPIDList` 在 `dapIdList` 为空时
主动探测，并写回**真实 IDCODE**。

**验证**：µVision 对话框显示 `IDCODE 0x2BA01477` / `ARM CoreSight SW-DP`；
日志 `CMSIS_DAP_GetDeviceIDList: maxEntries=64, returned=1, id[0]=0x2BA01477`。

**顺带确认（均无需修改）**：

| 项 | 结论 |
|----|------|
| `CMSIS_DAP_Disconnect` | 官方导出、ORBMDK 缺失，但 AGDI 内 `call/jmp` 引用数为 **0**，从不调用 |
| `CMSIS_DAP_ConfigureDebugger` | AGDI 按 **2 参**调用（`cfg = NULL`），返回值必须为 0；ORBMDK 已符合 |
| `CMSIS_DAP_JTAG_GetIDCODEs` | AGDI 按 **4 参**调用且第 3 参传 `NULL`；`DAP_JTAG_IDCODE` 有 `if (idcodes && ...)` 保护，安全 |
| `CMSIS_DAP_Connect` 第 2 参 | AGDI 传 `RDDI_DAP_CONN_DETAILS*`；ORBMDK 不写它（写反而可能越界），保持原样 |

### 4.8 Flash 下载 "Erase Failed! / RDDI-DAP Error" 根因（等待值匹配的掩码）

**现象**：编译通过，`Load` 阶段报

```
Erase Failed!
RDDI-DAP Error
Error: Flash Download failed  -  "Cortex-M4"
```

**定位**：日志

```
[ORBMDK][ERROR] [RDDI] DAP_RegAccessBlock[2]: no match after 100 retries (got 0x00030003)
```

AGDI 的等待匹配调用（`SWD.cpp:1841-1891`，读 DHCSR 等 `S_HALT` 置位）：

```cpp
regID[0]=DAP_REG_MATCH_RETRY; regData[0]=100;
regID[1]=DAP_REG_MATCH_MASK;  regData[1]=0x00010000;
...
regID[i+1] = DAP_REG_AP_0x0 | DAP_REG_RnW | DAP_REG_WaitForValue;
regData[i+1] = 0x00010000;   // 只给期望值
```

**但第二个 `DAP_RegAccessBlock` 调用的数组是从 index 0 重建的，不含 MATCH_MASK / MATCH_RETRY。**

**根因**：ORBMDK 的 `matchMask` 默认值是 `~0`：

```cpp
int matchMask = ~0;   // 默认全掩码
```

掩码缺省时退化成**全等比较**：`0x00030003 == 0x00010000` 永不成立
（而 `0x00030003 & 0x00010000 == 0x00010000`，按位匹配本应立即成立）→ 100 次后
返回 `RDDI_DAP_NO_MATCH` → 擦除失败。

**修复**：新增 `matchMaskSet` 标志；掩码缺省时按 `expected` 中为 1 的位匹配
（"等这些位置起来"），而不是全等；失败日志补充 期望值 / 掩码 / 实测值。

```cpp
const int effectiveMask = matchMaskSet ? matchMask : expected;
```

**验证**：待 Keil 重跑 Flash Download 确认。

### 4.9 官方 `rddi_dap.h` 寄存器编号（权威对照）

来源 `D:\WorkSpeace\RDDI\rddi_dap.h:128-144`，ORBMDK 的 `ORBMDK_RDDI.h` 已一致：

```c
#define DAP_REG_DP_0x0  0   ...  DAP_REG_DP_0xC  3
#define DAP_REG_AP_0x0  4   ...  DAP_REG_AP_0xC  7
#define DAP_REG_DP_ABORT      8
#define DAP_REG_JTAG_IDCODE   9
#define DAP_REG_DAP_IDR      10
#define DAP_REG_MATCH_MASK   16
#define DAP_REG_MATCH_RETRY  17
#define DAP_REG_RnW          0x10000
#define DAP_REG_WaitForValue 0x20000
```

即 `GetRegId()` 取低 16 位、`bit16 = RnW`、`bit17 = WaitForValue` 的解码方式正确。

### 4.10 日志改进（联调通过后的一轮收敛）

**背景**：烧录与调试功能验证通过后，对一次完整调试的日志（231 行）做审查，
发现噪声占比过高（224/231 为例行 INFO），且缺少排障必需的时间/线程维度。

| # | 位置 | 问题 | 修复 |
|---|------|------|------|
| 21 | `ORBMDK_Log` | **文件日志写死 `level >= LOG_LEVEL_INFO`，完全绕过 `ORBMDK_LogLevel()`** —— 默认阈值明明是 ERROR，例行 INFO 仍全部落盘。一次调试 231 行里 224 行是噪声（`RDDI_Open` / `CMSIS_DAP_Detect` / `ConfigureInterface` 各约 50 次），且每次写入都 `fopen`+`fputs`+`fclose` | 文件与调试器输出**共用同一阈值** `level >= ORBMDK_LogLevel()`；排障时 `ORBMDK_LOG_LEVEL=0` 即可让文件也收到 DEBUG |
| 22 | `ORBMDK_Log` | 日志无时间戳 / PID / TID。日志跨进程追加（每次 Keil 会话一个进程），实测 58 次 `RDDI_Open` 返回的 handle **全是 1**，极易被误判成"句柄泄漏" | 格式改为 `[ORBMDK][HH:MM:SS.mmm][级别][模块][PID:TID] 消息` |
| 23 | `ORBMDK_LogWriteFile` | 多线程（AGDI 对话框线程 + 调试线程）并发 `fputs` 会让行与行交错 | 加 `std::mutex` 串行化；句柄仍按行开关（UV4 会独占文件，外部诊断需能读） |
| 24 | `ORBMDK_Log` | 512 字节缓冲区**静默截断**，`ConfigureInterface` 的 cfg 串已接近上限 | 缓冲区扩到 1024，超长时末尾写 `...` 标记 |
| 25 | `DAP_ReadReg` / `DAP_WriteReg` | `FAULT` / `WAIT` / `NO_ACK` 记为 **ERROR**。这三者是 AGDI `SWD_CheckStatus` 会**自行恢复**的瞬时状态（识别 `RDDI_DAP_DP_STICKY_ERR` / `RDDI_DAP_OPERATION_TIMEOUT` 后清 sticky 位或写 ABORT 重试） | 降为 **WARN**，避免正常调试期间持续刷 ERROR 掩盖真问题 |
| 26 | `DAP_WriteReg` | FAULT 日志不含写入的 `value` | 补 `value=0x%08X`（写失败排查的关键字段） |
| 27 | `RDDI_Close` | **完全无日志**（实测 0 条）→ 无法确认 AGDI 是否回收实例、context 是否累积 | 补 `LOG_DEBUG`（含 handle 与剩余 context 数）；无效句柄补 `LOG_WARN` |
| 28 | `RDDI_GetLastError` | 完全无日志 | 补 `LOG_DEBUG`（错误码 + 描述）；无 context 时 `LOG_WARN` |
| 29 | `RDDI_Open` | 自动连接失败只报 `"HID auto-connect failed"`，无系统错误码 | 补 `GetLastError()`，便于区分"设备未插 / 被独占 / 驱动问题" |

**新日志格式**：

```
[ORBMDK][13:48:18.217][ERROR][RDDI][23140:23144] DAP_WriteReg: FAULT, regId=0x00000004, value=0x00000000 -> RDDI_DAP_DP_STICKY_ERR
```

**验证**：构建无 error / 无 warning；产物内确认含新格式串
`[ORBMDK][%02u:%02u:%02u.%03u][%s][%s][%lu:%lu]`。
默认（ERROR）阈值下，一次正常调试的日志文件应只剩 0～数行 WARN/ERROR。

**✅ 2026-09-30 已处理**（§8 统一方案落地，记录见 §8.5）：模块名统一
（宏移到头文件，各 .cpp 只定义 `ORBMDK_LOG_MODULE`）、HID 层 `HID_Log` 与 RDDI 层
合并为同一实现、`g_logEnabled` 死变量删除、`RDDI_SetLogCallback` 通道接通
（CallbackSink + 级别映射）、HID/Bulk 两处"命令级直写文件"并回统一入口
（并补上它们原本没有的时间戳）。

> 唯一遗留小项：**会话分隔标记**（跨会话追加的日志里加一条醒目分隔行）—— 见 §18.3。

### 4.11 设备拔出时显示伪 IDCODE `0x4A57533B`（输出参数未写入）

**现象**：拔掉调试器后打开 Debug Settings，SW Device 列表显示
`IDCODE 0x4A57533B / Device Name = Unknown device`，而不是 0 或空。

**定位**：`0x4A57533B` 按小端拆开是 `3B 53 57 4A` = ASCII **`;SWJ`** —— 正是
`CMSIS_DAP_ConfigureInterface` 收到的 cfg 串 `Master=Y;Port=SW;SWJ=Y;Clock=...`
里的一截。说明 AGDI 读到的是**上一轮调用残留在它自己栈上的内容**。

AGDI 把 `CMSIS_DAP_GetDeviceIDList` 的输出数组直接当 IDCODE 使用，而该数组是
AGDI 的**栈局部变量**（`[ebp-0x10c]`）。设备拔出时 `dapIdList` 为空，而 §4.7 引入的实现是：

```cpp
int n = static_cast<int>(ctx->dapIdList.size());
for (int i = 0; i < n; i++) {
    idArray[i] = static_cast<int>(ctx->dapIdList[i]);   // n == 0 时一次都不执行
}
```

一个字节都不写 → AGDI 读到栈残留。

**根因**：违反 §4.6 #17/#18 已确立的硬约束 —— "带标量输出参数的接口必须先写入
（0 / 空串），再判错返回；调用方只看输出值、不看返回值"。

> 旧版本恰好因为 `n = ctx->initialized ? kSingleDapCount : 0` 恒为 1、**总会写一个 `0` 进去**，
> 把这个坑盖住了；§4.7 改成按真实 `dapIdList` 计数后暴露。

**修复**：`n == 0` 时显式写入 `idArray[0] = 0` 并记日志，与
`CMSIS_DAP_DetectDAPIDList` 的兜底保持一致。

**验证**：设备拔出时对话框显示 `IDCODE 0x00000000 / Unknown device`。

**教训**：凡是"输出数组/输出标量"，都要先无条件写初值，再考虑数量是否为 0 ——
数量为 0 与"不写"是两件事。

### 4.12 设备未连接时 SW Device 列表应为空（计数的语义）

**现象**：§4.11 修复后，拔掉调试器时列表不再显示伪 IDCODE，但变成显示一行
`IDCODE 0x00000000 / Device Name = Unknown device`。**正确表现是列表为空。**

**定位**：官方 AGDI 把 `CMSIS_DAP_DetectNumberOfDevices` 的返回值直接当作
`JTAG_devs.cnt`，决定列表显示几行：

```asm
0x1002CC6D  call eax                    ; CMSIS_DAP_DetectNumberOfDevices(handle, &count)
0x1002CC9C  cmp  dword ptr [edi], 0x40  ; count > 64 → 报错
```

| 返回值 | 对话框表现 |
|--------|-----------|
| 1 | 显示一行 `IDCODE 0x00000000 / Unknown device` |
| 0 | 列表为空 |

**根因**：ORBMDK 把该函数实现成"适配器是否已打开"：

```cpp
if (ctx->initialized) {          // RDDI_Open 之后恒为 true
    *count = kSingleDapCount;    // → 恒为 1
}
```

而 ARM 的语义是"**目标上**扫描到的 DAP 数量"—— 与 §4.7 的
`CMSIS_DAP_GetDeviceIDList` 同源，官方实现取的是同一张内部设备表
（计数 `[0x1005ab24]`、数组 `[0x1005ab2c]`）。

**修复**：改为返回 `ctx->dapIdList.size()`（列表为空时先触发一次探测），
未连接时自然为 0。

**验证**：拔掉设备后 SW Device 列表为空；接上目标后显示一行
`IDCODE 0x2BA01477 / ARM CoreSight SW-DP`。

> 注：`CMSIS_DAP_GetNumberOfDevices`（RDDI 层另一个计数接口，AGDI 仅在 JTAG 分支
> 调用，见 `0x1002C9BC`）仍返回 `kSingleDapCount`，本次未改动 —— 它是 §4.6 #16
> 为修复 "No Debug Unit Found" 而定，且不在 SWD 路径上。**JTAG 已于 §18.9 第九步打通**，
> 此项在 JTAG 路径上是否仍适用待复核（当前 SWD 路径不受影响）。

> 另注：设备拔出后适配器下拉框仍显示 `CMSIS-DAP v1` / 序列号，属 AGDI 缓存的
> `MonConf` 与 HID 层连接状态未失效，**不影响** 上述列表行为。如需一并处理，
> 需让 `ORBMDK_HID_IsConnected()` 具备拔出探活能力。

### 4.13 列表行数的真正来源 + 输出参数全量复核（§4.12 的修正）

> §4.12 的改动**不是根因**（改动本身语义正确，保留）。本节记录真正的行数来源。

**现象**：§4.12 部署后（已用 SHA256 确认生效），拔掉设备时列表仍显示一行
`IDCODE 0x00000000 / Unknown device`；日志出现
`RDDI_Open: HID auto-connect failed, GetLastError()=0`。

**定位**：反汇编 AGDI 的设备探测例程 `0x1002c890`（其出参即列表行数）：

```asm
0x1002C918  push esi                        ; esi = 调用方传入的 &count
0x1002C919  push dword ptr [0x102fa57c]     ; handle
0x1002C91F  call dword ptr [0x10362F94]     ; CMSIS_DAP_JTAG_GetIDCODEs(handle, &count, NULL, 0)
```

即 **AGDI 把 `CMSIS_DAP_JTAG_GetIDCODEs` 的出参当作"设备数量"**，
而不是 `CMSIS_DAP_DetectNumberOfDevices` 的返回值 —— 后者只在该例程的
"函数缺失回退分支"（`0x1002cc62`）才被调用，而 ORBMDK 四个函数齐全，走不到那里。

**根因**：`CMSIS_DAP_JTAG_GetIDCODEs` 只在成功时写 `*count`：

```cpp
int status = ORBMDK::DAP_JTAG_IDCODE(&idcodeCount, idcodes);
if (status == 0) {
    *count = idcodeCount;      // ← 设备未插 → USB 失败 → 一个字节都不写
}
```

AGDI 于是读到它自己栈上的残留值（>0）→ 认为有设备 → 显示一行。

**修复**（3 处，同一类问题）：

| # | 位置 | 修复 |
|---|------|------|
| 30 | `CMSIS_DAP_JTAG_GetIDCODEs` | 入口先 `*count = 0`，再判错返回 |
| 31 | `CMSIS_DAP_JTAG_GetIRLengths` | 同上（原实现句柄无效时提前返回，不写 `*count`） |
| 32 | `RDDI_Open` | 自动连接失败时写 `*pHandle = 0`（本层句柄从 1 起，0 恒为无效）。否则 AGDI 会保留上次成功打开时的旧句柄，继续对已失效的连接调用各接口 |

**验证**：拔掉设备后列表为空；接上目标显示一行 `IDCODE 0x2BA01477 / ARM CoreSight SW-DP`。

**教训（升级版）**：§4.6 #18 曾宣称"全部带标量输出参数的接口已统一为先写后判"，
但 §4.11、§4.13 又各查出一批漏网。这类问题**无法靠抽样确认**，需对着
`ORBMDK_RDDI.h` 的 78 个导出逐个核对"是否存在未写输出参数就返回的路径"。

**建议**：单独做一轮全量复核 —— 可写静态检查脚本，对每个含指针出参的导出，
确认其每条 `return` 路径之前都已写过该出参。

### 4.14 目标不在时列表仍有一行（扫描返回值语义 + 另一个计数函数）

> 承接 §4.13。§4.13 把 `*count = 0` 写对了，但**函数返回值**仍不对，问题依旧。

**场景**：调试器（ORBTrace）连接正常，**目标板未连接 / 未上电**。

**定位**：反汇编 AGDI 设备扫描例程 `0x1002C890` 的后续分支：

```asm
0x1002C91F  call dword ptr [0x10362F94]   ; CMSIS_DAP_JTAG_GetIDCODEs(handle, &count, NULL, 0)
0x1002C933  test eax, eax
0x1002C935  jne  0x1002C9D8               ; ← 返回非 0
0x1002C9D8  mov  eax, 0x2028              ; → 整个扫描以 0x2028 失败退出
```

**根因（两层）**：

1. `CMSIS_DAP_JTAG_GetIDCODEs` 扫不到 IDCODE 时返回 `RDDI_DAP_ERROR`。AGDI 把它当
   **硬失败** → 不再更新 `JTAG_devs` → 对话框继续显示上一次的值（即工程里保存的
   `-D00(00000000) -N00("Unknown device")`）→ **表现为"目标不在却仍有一行"**。
2. 另一条分支（`0x1002C9BA`）用的是 `CMSIS_DAP_GetNumberOfDevices`，ORBMDK 仍固定返回
   `kSingleDapCount = 1`。

**修复**：

| # | 位置 | 修复 |
|---|------|------|
| 33 | `CMSIS_DAP_JTAG_GetIDCODEs` | **"没扫到 IDCODE"不是错误** —— 返回 `RDDI_SUCCESS` + `count = 0`，AGDI 才会把设备数清零 |
| 34 | `CMSIS_DAP_GetNumberOfDevices` | 返回真实设备数（列表为空时先 `DetectTargetDapIdList`），与 `DetectNumberOfDevices` 统一语义 |

> #34 与 §4.6 #16 的关系：当年改为固定 1，是因为 `dapIdList` 从未被填充、此处恒返回 0，
> 导致 "No Debug Unit Found"。现在空列表会主动探测，故可安全返回真实数量。

**关键认知**：AGDI 在 **SWD 模式**下同样会调用名字带 `JTAG_` 的导出 —— 它们是
**协议无关**的"扫描 DAP / IDCODE"探测器（`0x1002C890` 不区分协议）。
排查时不要因为名字里有 `JTAG` 就跳过。

**教训**：**"没找到"与"出错"是两件事**。AGDI 对返回值容忍度极低（非 0 即中止当前步骤、
且不更新状态），凡是"空结果"都应返回成功 + 计数 0。

**验证**：目标不在时列表为空；接上目标显示一行 `IDCODE 0x2BA01477 / ARM CoreSight SW-DP`。

---

## 五、仍存在的简化实现

> 以下均**不影响 Keil 调试链路**（AGDI 实际只调用第二节列出的 15 个函数）。

### 5.1 RDDI / HID 层 stub

| 函数 | 现状 |
|------|------|
| `DAP_DefineSequence` / `DAP_RunSequence` | 忽略全部参数，返回成功 |
| `StreamingTrace_SubmitEventBuffer` | 忽略 buffer 返回成功 |
| `CMSIS_DAP_Commands` | 仅处理 `ID_DAP_RESET_TARGET`，其余返回 `RDDI_BADARG` |
| `CMSIS_DAP_ConfigureDAP` | 仅识别 `SWJSwitch=` 前缀，无实质动作 |
| `CMSIS_DAP_JTAG_GetIRLengths` | 硬编码 `*count=0` |
| `CMSIS_DAP_Atomic_Result` | 硬编码 `*result=0` |
| `CMSIS_DAP_GetGUID` | 序列号拼串，非真 GUID |
| `DAP_Target`（`ORBMDK_HID.cpp`） | Halt/Resume/Step 仅置 0，**仅 Reset 有真实调用** |

### 5.2 Trace 适配层 stub（`ORBMDK_Trace.cpp`）

| 函数 | 现状 |
|------|------|
| `ORBMDK_Trace_GetCPUState` | 返回 static 空结构 |
| `ORBMDK_Trace_SetAltAddrEncode` | 空函数 |
| `ORBMDK_Trace_SWO_SetBaud` | 空函数 |

### 5.3 解码器实现深度

| 解码器 | 实际状态 |
|--------|----------|
| ITM | ✅ 状态机完整（同步 / SW / HW / TS / GTS / XTN / NISYNC / RSVD） |
| TPIU | ✅ 状态机自洽（显式 `packetReady`），包边界可靠 |
| ETM v3.5 | 处理 A-SYNC/ISYNC/CYCCNT/CONTEXTID/分支；ISYNC 地址宽度可配置（`addrBytes`，默认 4） |
| ETM v4 | 短/长地址 + 原子 + 异常少量；`FuncReturn` / `ExceptionReturn` / `Prefix` 分支仍为 `return 0` 空操作 |

### 5.4 未处理项

1. **ULINK+ 专有接口**：`ULINKPLUS_*` 属 Keil 独有，ORBMDK/ORBTrace 无需支持。
2. **ETM v4 深度**：`FuncReturn` / `ExceptionReturn` / `Prefix` 需结合具体 ETM 配置与实机 trace 数据完善。
3. ~~**V2 Bulk 路径实机验证**：当前实机联调走 V1 HID（ORBTrace 仅支持 V1），Bulk 路径尚未实机验证。~~
   → ✅ **2026-09-30 已完成**：V2 实机跑通完整调试与下载（§17.3 命令行、§17.5 B 臂 Keil）。
   注：原文"ORBTrace 仅支持 V1"也不准确 —— orbtrace 的 MI_05 就是 CMSIS-DAP v2
   （EP 0x03/0x85，设备自报包长 508）。

---

## 六、关键文件索引

| 文件 | 作用 |
|------|------|
| `bin/ORBMDK_RDDI.dll` | 构建产物（**x86**；部署时重命名为 AGDI 层实际加载的名字：Keil "CMSIS-DAP Debugger" 为 `CMSIS_DAP.dll`） |
| `include/ORBMDK_RDDI.h` | RDDI 接口声明（78 个导出） |
| `include/ORBMDK_HID.h` | HID 层声明 |
| `include/ORBMDK.h` | 版本信息、ORBTrace VID/PID 统一常量 |
| `src/ORBMDK_RDDI.cpp` | RDDI 实现（核心） |
| `src/ORBMDK_HID.cpp` | HID 通信层 + DAP 命令封装 |
| `src/ORBMDK_USB_Bulk.cpp` | USB Bulk V2 传输层 |
| `src/ORBMDK_ITM_Decoder.cpp` | ITM 解码器 |
| `src/ORBMDK_ETM_Decoder.cpp` | ETM 解码器 |
| `src/ORBMDK_TPIU_Decoder.cpp` | TPIU 解码器 |
| `src/ORBMDK_COBS.cpp` | COBS 解码 |
| `build.ps1` | 编译脚本（自动探测 VS2017+/MSVC/SDK，固定 **x86**，Keil 为 32 位进程，必须 x86） |
| `deploy.ps1` | 部署脚本（复制 `bin\ORBMDK_RDDI.dll` 覆盖 Keil 的 `CMSIS_DAP.dll`，自动定位 `ARM\BIN` 并备份原件） |
| `test/build_test.ps1` | 测试构建脚本（同为 x86） |
| `test/ORBMDK_RDDI_FullTest.cpp` | 实机功能测试（40 项） |
| `tools/pe_re.py` | 逆向分析工具（exports / imports / names / dis / xref / slots 六个子命令），用法与结论见 §十 |

---

## 七、硬编码路径与文件筛查

> 筛查日期：2026-09-30。范围为 `ORBMDK/` 全目录（`src/`、`include/`、`test/`、`deprecated/`、`build.ps1`）。
> 按「可移植性风险」分级，🔴 必改、🟠 建议改、🟡 可接受（设计内 fallback）。

### 7.1 ✅ 已修复：测试程序写死本机绝对路径

| 文件 | 原硬编码内容 | 现状 |
|------|--------------|------|
| `test/ORBMDK_RDDI_Test.cpp` | `"c:\\Users\\234896\\Desktop\\orbmdk\\ORBMDK\\bin\\ORBMDK_RDDI.dll"` | ✅ |
| `test/ORBMDK_RDDI_FullTest.cpp` | 同上 | ✅ |
| `test/orbprobe.cpp` / `orbprobe2.cpp` / `orbprobe3.cpp` | 同上 | ✅ |
| `test/ORBMDK_BlockTransferTest.cpp` | 同上 | ✅ |

- 均用于 `LoadLibraryA(dllPath)`，曾是测试唯一的外部依赖路径；换机器 / 换目录后必然 `LoadLibraryA` 失败。
- **修复方式**：默认改为**裸文件名** `"ORBMDK_RDDI.dll"` —— `build_test.ps1` 把 exe 与 DLL 都输出到 `bin\`，
  而 `LoadLibraryA` 的搜索顺序**首先就是"应用程序加载的目录"（exe 同目录）**，因此无需拼绝对路径。
  另支持 `argv[1]` 显式覆盖（`ORBMDK_BlockTransferTest` 的 `argv[1]` 是 RAM 地址，故不占用该参数）。

### 7.2 ✅ 已修复：构建脚本写死工具链位置

| 文件 | 原硬编码内容 | 现状 |
|------|--------------|------|
| `build.ps1` | `$MSVCDir = "D:\Program Files (x86)\Microsoft Visual Studio\2017\Community"`、`$VCTools = "...\MSVC\14.16.27023"`、`$WindowsSDKInclude/Lib = "D:\Windows Kits\10\...10.0.17763.0"` | ✅ |
| `test/build_test.ps1` | 同上 | ✅ |

- **修复方式**（两个脚本同构，按本项目既有的"脚本各自独立"风格维护）：
  - VS 安装路径：`vswhere.exe -latest -requires ...VC.Tools.x86.x64` → 回退到常见安装目录候选（VS 2022 / 2019 / 2017，`D:` 与 `C:`）；
  - VC 工具集：`VC\Tools\MSVC` 下**版本号最大者**（不写死 `14.16.27023`）；
  - Windows SDK：注册表 `HKLM\...\Windows Kits\Installed Roots` 的 `KitsRoot10` → 回退 `D:\Windows Kits\10` / `C:\Program Files (x86)\Windows Kits\10`；
    版本取"同时具备 `Include\<ver>` 与 `Lib\<ver>\ucrt\x86`"的**最高版本**（不写死 `10.0.17763.0`）。
- 位数约束不变：始终用 `bin\Hostx86\x86`，产物为 **x86**（Keil µVision 是 32 位进程）。
- 当前环境实测：VS2022 Community（`VC\Tools\MSVC\14.44.35207`）+ SDK `10.0.26100.0`，全量编译无 error / 无 warning。

### 7.3 🟡 低：符号解析的候选搜索路径（设计内 fallback）

| 文件:行 | 硬编码内容 | 说明 |
|---------|-----------|------|
| `src/ORBMDK_Symbols.cpp:927-933` | Keil `TOOLS.INI`：`C:\Program Files\Keil_v5\`、`C:\Program Files (x86)\Keil_v5\`、**`D:\Keil_v5\`**、`C:\Keil_v5\`、`D:\MDK5\` | `ORBMDK_FindObjdumpPath` 第 4 步候选 |
| `src/ORBMDK_Symbols.cpp:973-979` | GNU Arm Toolchain 7 条候选（含 `12.3.rel1` 等版本号） | 第 5 步候选 |
| `include/ORBMDK_Symbols.h:243` | 注释中的 `C:\Keil_v5\ARM\ARMCC\Bin` | 仅文档说明 |

- 该函数已按「环境变量 → Keil TOOLS.INI → 常见安装目录 → `PATH`」顺序探测，前序步骤（`OBJDUMP` / `ARM_TOOLCHAIN_PATH` / `ARMGCC_DIR`）和 `PATH` 兜底可覆盖多数场景，故归为可接受。
- 已补充 `D:\Keil_v5\TOOLS.INI`（当前环境的实际位置）与 `C:\Keil_v5\TOOLS.INI`。
- `D:\MDK5\TOOLS.INI` 与带具体版本号的 GNU 候选路径仍较脆，可继续精简为「系统盘默认项 + 环境变量」。

### 7.4 ✅ 无需处理

| 位置 | 说明 |
|------|------|
| `src/ORBMDK_Coverage.cpp`（`fopen(outputPath/gcdaPath)`） | 路径由调用方参数传入，非硬编码 |
| `src/ORBMDK_USB_Bulk.cpp` / `ORBMDK_HID.cpp`（`CreateFile(DevicePath)`） | 来自设备枚举结果，非硬编码 |
| `include/*.h`、`src/pch.h` 的 `"../include/xxx.h"` | 相对包含路径，正常 |
| `deprecated/` | 未发现硬编码路径 |
| `bin/*.txt`、`obj/*.obj`、`*.exe/.dll/.lib/.exp` | 构建产物 / 测试输出，忽略 |

### 7.5 结论

- ✅ **必改 1 类（已改）**：测试程序的 DLL 绝对路径（7.1，5 个文件），改为裸文件名 + `argv[1]` 覆盖。
- ✅ **建议改 1 类（已改）**：构建脚本的工具链路径（7.2，2 个脚本），改为 vswhere + 注册表自动探测。
- 🟡 **可选优化（部分已改）**：符号解析候选路径（7.3），已补 `D:\Keil_v5\`。
- 残留硬编码均在"候选路径 fallback"语义内，不影响构建与实机调试链路。

### 7.6 环境变更记录（2026-09-30）

| 项 | 旧值 | 新值 |
|----|------|------|
| Keil MDK | `D:\MDK5\ARM\BIN` | **`D:\Keil_v5\ARM\BIN`** |
| Visual Studio | `D:\Program Files (x86)\Microsoft Visual Studio\2017\Community`（MSVC 14.16.27023） | **`D:\Program Files\Microsoft Visual Studio\2022\Community`（MSVC 14.44.35207）** |
| Windows SDK | `D:\Windows Kits\10\...\10.0.17763.0` | **`D:\Windows Kits\10\...\10.0.26100.0`** |

- 受影响的文件：`build.ps1`、`test/build_test.ps1`、`deploy.ps1`、`test/*.cpp`（DLL 路径）、`src/ORBMDK_Symbols.cpp`。
- `deploy.ps1` 的目标路径已改为 `D:\Keil_v5\ARM\BIN\CMSIS_DAP.dll`，并支持 `-KeilArmBin` 显式指定与多候选自动探测。

---

## 八、日志模块现状分析与统一方案

> 分析日期：2026-09-30。范围：`ORBMDK/` 全目录（`src/`、`include/`、`deprecated/`）。
> **✅ 已落地（2026-09-30）**：§8.1~§8.4 是设计时的现状与方案（保留作历史依据），
> 落地结果、差异与验证见 §8.5。

### 8.1 现状分布图

| 模块文件 | 日志实现 | 状态 |
|----------|----------|------|
| `src/ORBMDK_RDDI.cpp` | 自建 `ORBMDK_Log()` + `LOG_DEBUG/INFO/WARN/ERROR` 宏 | 完整（含文件落盘、级别过滤） |
| `src/ORBMDK_HID.cpp` | 自建 `HID_Log()` + `LOG_HID_DEBUG/INFO/WARN/ERROR` 宏 | 独立重复实现（无文件落盘） |
| `src/ORBMDK_Trace.cpp` | 无 | 静默失败 |
| `src/ORBMDK_USB_Bulk.cpp` | 无 | 静默失败 |
| `src/ORBMDK_ETM_Decoder.cpp` / `ORBMDK_ITM_Decoder.cpp` / `ORBMDK_TPIU_Decoder.cpp` / `ORBMDK_COBS.cpp` / `ORBMDK_OFLOW.cpp` | 无 | 静默失败 |
| `src/ORBMDK_Symbols.cpp` | 仅 `snprintf` 拼命令 / 路径，非日志 | 静默失败 |
| `src/ORBMDK_Coverage.cpp` | `fprintf` 仅用于**报告文件输出**（KCacheGrind / JSON） | 非日志，勿混入 |
| `src/ORBMDK_DLL.cpp` | 无（`DllMain` 也不打日志） | 静默 |
| `deprecated/` | 无 | 已废弃 |
| `include/*.h` | 无任何日志声明 | 无公共入口 |

即：**全项目仅 2 个模块有日志，且各自独立实现；其余 7+ 个模块完全无日志。**

### 8.2 核心问题

**（1）两套并行实现，逻辑完全重复**

`ORBMDK_Log` 与 `HID_Log` 各自做一遍：环境变量解析、级别过滤、512 字节缓冲区、`printf` + `OutputDebugStringA`。

```25:142:src/ORBMDK_RDDI.cpp
// 日志模块 - 使用 OutputDebugString + DebugView
enum LogLevel { LOG_LEVEL_DEBUG = 0, LOG_LEVEL_INFO = 1, LOG_LEVEL_WARN = 2, LOG_LEVEL_ERROR = 3 };
static void ORBMDK_Log(LogLevel level, const char* module, const char* fmt, ...) { ... }
```

```18:66:src/ORBMDK_HID.cpp
// Local Logging
static int HID_LogLevel(void) { ... }
static void HID_Log(int level, const char* module, const char* fmt, ...) { ... }
```

**（2）宏定义在 `.cpp` 内，天然无法跨模块复用**

`ORBMDK_RDDI.cpp` 里定义了 `LOG_HID_*` / `LOG_BULK_*` / `LOG_TRACE_*` 并注释「可在其他模块使用」，但它们位于 `.cpp` 而非头文件，**任何其他编译单元都看不到**，实际是死代码：

```133:141:src/ORBMDK_RDDI.cpp
// 模块级日志宏 (可在其他模块使用)
#define LOG_HID_DEBUG(fmt, ...)   ORBMDK_Log(LOG_LEVEL_DEBUG, "HID", fmt, ##__VA_ARGS__)
...
#define LOG_TRACE_INFO(fmt, ...)  ORBMDK_Log(LOG_LEVEL_INFO,  "TRACE", fmt, ##__VA_ARGS__)
```

同时 `ORBMDK_HID.cpp` 又独立定义了一套同名 `LOG_HID_*`，两套语义 / 级别集合不一致（RDDI 版缺 `WARN`）。这是「分散」的根因。

**（3）输出格式与能力不一致**

- RDDI：`[ORBMDK][LEVEL] [MODULE] msg`，支持 `ORBMDK_LOG_FILE` 落盘、支持 `WARN`。
- HID：`[ORBMDK][MODULE] msg`（无级别字段），**不支持文件落盘**、支持 `WARN`。

**（4）RDDI 官方日志通道未接通**

`RDDI_SetLogCallback` 把回调存进 `ctx` 后**从未被调用**，Keil 的日志窗口收不到任何消息：

```2206:2219:src/ORBMDK_RDDI.cpp
RDDI_FUNC void RDDI_SetLogCallback(const RDDIHandle handle, RDDILogCallback pfn,
                                   void *context, int maxLogLevel)
{
    ctx->logCallback         = pfn;
    ctx->logCallbackContext  = context;
    ctx->logCallbackMaxLevel = maxLogLevel;
}
```

而 `RDDI_LOGLEVEL_*` 是 0–5 六级，内部是 0–3 四级，两套级别无映射：

```109:119:include/ORBMDK_RDDI.h
#define RDDI_LOGLEVEL_FATAL   0
...
#define RDDI_LOGLEVEL_TRACE   5
typedef void (*RDDILogCallback)(void *context, const char *const msg, const int logLevel);
```

**（5）线程安全与资源管理**

- 日志函数无锁，而 HID 的 USB I/O 有 `g_hidMutex`；多线程下 `printf` / `FILE*` 写入会交错甚至数据竞争。
- `ORBMDK_LogFile()` 的 `FILE*` 只在进程退出时由 CRT 回收，从不 `fclose`；且每行 `fflush`。
- 无时间戳、无线程 ID，排障时无法定位时序。

**（6）其他遗留**

- `g_logEnabled` 恒为 `true`，从未被赋值，属死变量。
- 默认阈值 `ERROR` 在两个文件里各写一遍，易漂移。
- 被过滤时 `ORBMDK_Log` 仍会先算 `toConsole`，逻辑重复。

### 8.3 统一方案设计

目标：**单一实现、头文件暴露、多 Sink 可插拔、编译期 / 运行期双层过滤、线程安全、保持默认 ERROR 静默。**

**（1）新增文件**

- `include/ORBMDK_Log.h` — 级别枚举、API、宏
- `src/ORBMDK_Log.cpp` — 唯一实现

> 不放进 `pch.h` 的依赖链，仅在各 `.cpp` 显式 include，避免宏污染。

**（2）统一级别与模块**

```cpp
// ORBMDK_Log.h
enum ORBMDK_LogLevel {
    ORBMDK_LOG_DEBUG = 0, ORBMDK_LOG_INFO, ORBMDK_LOG_WARN, ORBMDK_LOG_ERROR, ORBMDK_LOG_NONE
};
```

模块用**字符串常量**标识（`"RDDI"` / `"HID"` / `"BULK"` / `"TRACE"` / `"ETM"` / `"ITM"` / `"SYM"` / `"DLL"`），彻底消除「每个模块自定义一组宏」的做法。

**（3）核心 API（单入口 + 薄宏）**

```cpp
void ORBMDK_LogWrite(int level, const char* module, const char* fmt, ...);
void ORBMDK_LogSetLevel(int level);                       // 运行期改阈值
void ORBMDK_LogSetFile(const char* path);                 // 可选文件 sink
void ORBMDK_LogSetCallback(ORBMDK_LogSinkFn, void* ctx);  // 供 RDDI 桥接

// 唯一一组宏（全局可用）
#define ORBMDK_LOG_DEBUG(mod, ...) ORBMDK_LogWrite(ORBMDK_LOG_DEBUG, mod, __VA_ARGS__)
#define ORBMDK_LOG_INFO(mod, ...)  ORBMDK_LogWrite(ORBMDK_LOG_INFO,  mod, __VA_ARGS__)
#define ORBMDK_LOG_WARN(mod, ...)  ORBMDK_LogWrite(ORBMDK_LOG_WARN,  mod, __VA_ARGS__)
#define ORBMDK_LOG_ERROR(mod, ...) ORBMDK_LogWrite(ORBMDK_LOG_ERROR, mod, __VA_ARGS__)
```

为减少重复传 `mod`，再提供**模块内**便捷宏（放各 `.cpp` 顶部）：

```cpp
#define ORBMDK_LOG_MODULE "HID"
#define LOG_DEBUG(...) ORBMDK_LOG_DEBUG(ORBMDK_LOG_MODULE, __VA_ARGS__)
```

**（4）编译期 + 运行期双层过滤（保留零开销特性）**

```cpp
#ifndef ORBMDK_LOG_MIN_LEVEL
#define ORBMDK_LOG_MIN_LEVEL ORBMDK_LOG_DEBUG   // Release 可定义成 ERROR
#endif
#define ORBMDK_LOG_INFO(mod, ...) \
  do { if (ORBMDK_LOG_INFO >= ORBMDK_LOG_MIN_LEVEL) \
         ORBMDK_LogWrite(ORBMDK_LOG_INFO, mod, __VA_ARGS__); } while (0)
```

运行期阈值仍在 `ORBMDK_LogWrite` 内、**格式化之前**判断（沿用现有优良设计，见 3.6）。

**（5）多 Sink 架构（一次格式化广播）**

```
ORBMDK_LogWrite(level, module, fmt, ...)
        │  级别过滤（运行期）→ 不通过则直接 return
        ├─ 统一格式化： [时间] [级别] [模块] [TID] 消息
        └─ 分发到已启用的 Sink：
             ├─ ConsoleSink   → printf               （默认开）
             ├─ DebuggerSink  → OutputDebugStringA   （默认开，DebugView）
             ├─ FileSink      → ORBMDK_LOG_FILE      （可选，统一 fflush 策略）
             └─ CallbackSink  → RDDI_SetLogCallback 桥接
```

- 环境变量解析**只做一次**（`std::once_flag` / 函数内 static），`ORBMDK_LOG_LEVEL`、`ORBMDK_LOG_FILE` 语义对**所有模块一致**，HID 也获得文件落盘能力。
- 新增时间戳与线程 ID，便于 µVision 内取证。

**（6）接通 RDDI 官方日志通道**

在 `RDDI_SetLogCallback`（`ORBMDK_RDDI.cpp`）中把回调注册为 `CallbackSink`，并做级别映射：

| 内部级别 | RDDI 级别 |
|----------|-----------|
| DEBUG | `RDDI_LOGLEVEL_DEBUG`(4) |
| INFO | `RDDI_LOGLEVEL_INFO`(3) |
| WARN | `RDDI_LOGLEVEL_WARNING`(2) |
| ERROR | `RDDI_LOGLEVEL_ERROR`(1) |

回调需注意 ARM 的形参顺序 `(context, msg, logLevel)`（`ORBMDK_RDDI.h` 已注释提醒）。

**（7）线程安全与资源**

- 内部用 `std::mutex`（或 `SRWLOCK`）保护格式化缓冲与所有 Sink 写入，与 `g_hidMutex` 互不干扰。
- `FileSink` 用 RAII 封装 `FILE*`，在 `DllMain(DLL_PROCESS_DETACH)` 统一关闭，避免依赖 CRT 回收。
- 缓冲改用 `std::vector<char>` 动态扩容，或保留 512B 但显式截断标记，替代当前静默截断。

**（8）迁移路径（Strangler 模式，可分阶段落地）**

1. **新增** `ORBMDK_Log.h/.cpp`，不改任何调用点。
2. 把 `ORBMDK_RDDI.cpp` 的 `ORBMDK_Log` 与 `ORBMDK_HID.cpp` 的 `HID_Log` **替换为转发到统一实现**（保留原宏名做兼容 alias，如 `#define LOG_HID_ERROR(...) ORBMDK_LOG_ERROR("HID", __VA_ARGS__)`），行为零变化。
3. 逐个给静默模块（Trace / Bulk / Decoder / Symbols / DLL）加日志，统一走 `ORBMDK_LOG_*`。
4. 删除 `ORBMDK_RDDI.cpp` 中的死宏（`LOG_HID_*` / `LOG_BULK_*` / `LOG_TRACE_*`）与 `g_logEnabled`，并把 3.6 与 4.5 的日志策略表同步改为指向统一模块。

**（9）需同步更新的文档**

- 本文 3.6「日志策略」（当前写死 `ORBMDK_RDDI.cpp / ORBMDK_HID.cpp` 两处）
- 本文 4.5「日志收敛」
- `README.md` 的「日志策略」表

### 8.4 预期收益

| 维度 | 现状 | 统一后 |
|------|------|--------|
| 实现份数 | 2 套 + 死宏 | 1 套 |
| 跨模块可用 | 否（宏在 .cpp） | 是（头文件宏） |
| 文件落盘 | 仅 RDDI | 全模块一致 |
| Keil 日志窗口 | 未接通 | 经 CallbackSink 接通 |
| 线程安全 | 无锁 | 加锁 |
| 静默模块 | 7+ | 可统一接入 |
| 排障信息 | 无时间 / TID | 含时间戳 + TID |

### 8.5 落地结果（2026-09-30）

**新增**：`include/ORBMDK_Log.h`（级别 / API / 宏）、`src/ORBMDK_Log.cpp`（唯一实现，
已加入 `build.ps1` 源列表）。

**删除**（§8.2 的六类问题逐条闭环）：

| §8.2 问题 | 处理 |
|-----------|------|
| 两套并行实现 | 删除 `ORBMDK_RDDI.cpp` 的 `ORBMDK_Log` / `ORBMDK_LogLevel` / `ORBMDK_LogWriteFile`，以及 `ORBMDK_HID.cpp` 的 `HID_Log` / `HID_LogLevel` |
| 宏写在 .cpp 里（死代码） | 宏移入头文件；RDDI 里那组"模块级宏"（`LOG_HID_*` / `LOG_BULK_*` / `LOG_TRACE_*`）按 §8.3(8) 第 4 步删除 |
| 格式 / 能力不一致 | 统一为 `[ORBMDK][时间][级别][模块][pid:tid] 消息`；HID 现在也落盘、也支持 `%TEMP%\ORBMDK_LOG_LEVEL` 热更新 |
| `RDDI_SetLogCallback` 未接通 | 接为 CallbackSink（`HostLogBridge`）：内部级别 → RDDI 级别映射 + `maxLogLevel` 过滤 |
| 无锁 / 无时间戳 | 单 `std::mutex` 保护格式化与文件写入；宿主回调在**锁外**调用（回调里再打日志不会自锁） |
| `g_logEnabled` 死变量 | 删除 |
| 两处"命令级直写文件"（`HidTrace` / `BulkTrace`）绕过级别、绕过 `ORBMDK_LOG_FILE`、无时间戳 | 并入 `ORBMDK_LogTrace()`：仍**不受级别控制**、仍只落盘，但走同一路径与格式（**顺带补上时间戳 / 进程线程号**） |

**与设计的两处取舍**：

1. 级别枚举与便捷宏**同名**（`ORBMDK_LOG_INFO` 既是枚举又是函数式宏）—— 函数式宏只在
   后面紧跟 `(` 时展开，所以 `ORBMDK_LOG_INFO,` 这种裸用仍取枚举值，二者可共存
   （实现里就是这么用的：`_format(..., ORBMDK_LOG_INFO, ...)`）。
2. 日志文件仍是**写后即关**、不用常驻 `FILE*`：宿主 µVision 会独占该文件，
   外部诊断工具必须能随时读 —— 这比省几次 `fopen` 重要。

**验证**：

- 构建：无 error、无 warning；
- V2 通路（`bin\orbprobe3.exe`）：功能全通过；日志统一格式；`[BULK]` 命令级日志
  **首次带上时间戳**；
- HID 通路（`bin\v1probe.exe`，`ifNo=1`）：`transport = CMSIS-DAP v1 (HID)`、
  `idcode=0x2BA01477`、`[HID] DAPCommand` 行同样带时间戳。

---

## 九、烧录速率优化分析（待实施）

> 分析日期：2026-09-30。烧录与调试功能已验证通过，本节只做瓶颈定位与方案设计，**尚未落地代码**。

### 9.1 结论

**瓶颈在 USB 往返次数，不在 SWD 时钟。**

日志中 `Clock=1000000`（1 MHz，理论上限约 125 KB/s），而实测烧录吞吐只有 **≈ 0.5–1 KB/s**
—— **USB 开销占了 99%**。因此"把 1 MHz 提到 10 MHz"在当前实现下几乎没有收益，
必须先解决往返次数问题。

### 9.2 根因：块传输能力已实现，但完全闲置

`DAP_RegWriteRepeat` / `DAP_RegReadRepeat` 是 AGDI 的
`SWD_WriteBlock` / `SWD_VerifyBlock` / `SWD_ReadBlock` 所走路径（**烧录主力**，
见 §2.2），但被实现成**逐字循环**：

```812:836:src/ORBMDK_RDDI.cpp
RDDI_FUNC int DAP_RegWriteRepeat(const RDDIHandle handle, const int dapId, const int numRepeats,
                                 const int regId, const int *dataArray)
{
    ...
    for (int i = 0; i < numRepeats; i++) {
        uint32_t data = static_cast<uint32_t>(dataArray[i]);
        int status = ORBMDK::DAP_Transfer(dapId, request, &data);   // ← 每个字一次 USB 往返
    }
```

而 `DAP_TransferBlock()`（`ID_DAP_TRANSFER_BLOCK`）**已实现、已声明，但全项目零调用点**
（grep 确认只有定义 `src/ORBMDK_HID.cpp:607` 与声明 `include/ORBMDK_HID.h:33`）。

### 9.3 量化对比

包长常量（`src/ORBMDK_HID.cpp:90-91`）：`HID_MAX_PACKET_SIZE = 65`（64 数据 + 1 报告 ID）。

| 方式 | 每次 USB 往返携带 | 32 KB 固件所需往返 |
|------|------------------|-------------------|
| **现状**：逐字 `DAP_Transfer` | 1 字（4 字节） | **8192 次** |
| 改用 `DAP_TransferBlock` | `(64-5)/4` = **14 字** | 585 次 |
| 若走 V2 Bulk（512 字节/包） | ≈ **127 字** | 65 次 |

HID 中断端点 Full Speed 轮询间隔 1 ms，且每次 `ORBMDK_HID_DAPCommand` 是
OUT + IN 各一轮 → 保守按 **1–2 ms / 往返** 估算：

| 方案 | 估算吞吐 | 32 KB 固件耗时 |
|------|----------|----------------|
| 现状（逐字） | ≈ 0.5–1 KB/s | **30–60 s** |
| 块传输（14 字/次） | ≈ 7–14 KB/s | **3–5 s** |
| Bulk（127 字/次） | 数十 ~ 上百 KB/s | < 1 s |

### 9.4 优化清单

| 优先级 | 项 | 位置 | 说明 |
|--------|-----|------|------|
| **P0** | `DAP_RegWriteRepeat` / `DAP_RegReadRepeat` 改用 `DAP_TransferBlock`，按 14 字分块 | `src/ORBMDK_RDDI.cpp:812` / `:838` | **收益 ≈ 14×，改动最小**。AGDI 侧 TAR 已按递增 CSW 设好，写 DRW 语义与块传输一致，可安全合并 |
| **P1** | 复用 OVERLAPPED 事件对象 | `src/ORBMDK_HID.cpp:364` | 每次调用都 `CreateEvent` / `CloseHandle`，高频路径上是实打实的开销 |
| **P2** | `DAP_TransferBlock` 的 `std::vector<uint8_t> cmd` 改栈数组 | `src/ORBMDK_HID.cpp:610` | 避免每次调用堆分配 |
| **P3** | 尝试启用 V2 Bulk 通道 | `src/ORBMDK_USB_Bulk.cpp` | 收益最大（≈ 9×），但取决于 orbtrace 固件是否支持；§5.4 记为"尚未实机验证" |
| **P4** | 提高 Max Clock | Keil 对话框 | **P0 之后才有意义**；1 MHz → 10 MHz |
| **P5** | µVision 侧设置 | Keil 对话框 | 擦除方式改 "Erase Sectors"（全片擦除慢很多）；确认 "Verify Code Download" 不勾 |

### 9.5 ⚠️ P0 必须同时修的隐患：HID 缓冲区长度校验

`ORBMDK_HID_DAPCommand` 的长度校验与缓冲区大小**不一致**：

```356:360:src/ORBMDK_HID.cpp
    if (cmdLen == 0 || cmdLen > DAP_BUFFER_SIZE - 1) return -2;   // 上限 511

    uint8_t reportOut[HID_MAX_PACKET_SIZE] = {};                  // 实际只有 65 字节
    reportOut[0] = CMD_REPORT_ID;
    memcpy(&reportOut[1], cmd, cmdLen);                           // ← cmdLen 65..511 时栈溢出
```

当前无人触发（`DAP_Transfer` 最多 8 字节、`DAP_TransferBlock` 未被调用），
但**一旦把块传输接上，只要 `count > 14` 就会踩中**。

因此 P0 必须配套两处修改：

1. 把校验改为 `cmdLen > HID_MAX_PACKET_SIZE - 1`（即 64）；
2. `DAP_RegWriteRepeat` / `DAP_RegReadRepeat` 内部按 **14 字/次**分块循环调用 `DAP_TransferBlock`。

### 9.6 建议实施顺序

**先做 P0 + §9.5 的配套修正**（一个函数 + 一处校验），预期烧录耗时从 ~30–60 s 降到 ~3–5 s，
风险低、可回滚；实测确认后再评估是否推进 P3（Bulk）。

### 9.7 不在本次范围

- `DAP_RegAccessBlock` 仍是逐条 `DAP_Transfer`：其语义包含虚拟寄存器（`MATCH_MASK`/`MATCH_RETRY`）
  与 wait-for-value 读（见 §4.8），**不能简单合并**；且它不是烧录主力路径，收益有限。
- 擦除耗时由目标侧 flash 算法执行时间决定，与 USB 无关。

### 9.8 实施记录（2026-09-30）

**已实施 P0 + §9.5**：

| 项 | 位置 | 改动 |
|----|------|------|
| **P0** | `DAP_RegWriteRepeat` / `DAP_RegReadRepeat` | 改为按 `kMaxBlockWords = 14` 分块调用 `DAP_TransferBlock`；固件不支持时首次失败即**永久回退**到逐字 `DAP_Transfer`（`RDDIContext::blockTransferSupported`），功能不受影响 |
| **§9.5** | `ORBMDK_HID_DAPCommand` | 长度校验上限由 `DAP_BUFFER_SIZE - 1`(511) 改为 `HID_MAX_PACKET_SIZE - 1`(64)，消除 `memcpy(&reportOut[1], cmd, cmdLen)` 的栈溢出隐患 |
| 防御 | `DAP_RegReadRepeat` | 每块读之前先清零目标区间，避免块传输只回部分数据时残留旧值 |

**未实施（评估后认为收益可忽略）**：

- P1 复用 OVERLAPPED 事件、P2 去掉 `std::vector`：单次 `CreateEvent` / `CloseHandle` /
  堆分配是 **µs** 量级，而一次 HID 往返是 **ms** 量级 —— 相差三个数量级，
  不值得为此引入复杂度。

**待实测**：烧录耗时对比（预期 30–60 s → 3–5 s）。

> ✅ **2026-09-30 实测完成**（`bin\ORBMDK_BlockTransferTest.exe 0x20000000`）：

```
cases passed 12, failed 0            <- 1/13/14/15/28/64 字边界 + 两侧各 8 字哨兵
serial total 301779.1 us
block  total  38372.7 us
speedup       7.86x                  <- 单例 n=64：serial 147.5 ms -> block 16.3 ms (≈9x)
```

- **哨兵零损坏** → §9.9 的越界写（Keil 崩溃根因 #35）确认已修好，块传输可以放心启用；
- 加速比低于理论 14×：V2(Bulk) 下单次往返开销本来就小，固定开销占比上升
  （HID 通路会更接近 14×）；
- 走查过程中顺带修好了这个"验证程序"本身（它此前根本跑不通，四步都是缺的）：
  1. 只调低层 `DAP_Connect` —— 那是个**纯桩**（只填 implementor 字符串，不做 SWJ 切换）
     → 补上 `CMSIS_DAP_Connect`（Keil 实际走的入口，本层负责 SWJ/SWD 收尾，§4.4）；
  2. 顺序按 ARM `rddi_dap.h`：`DAP_Connect` → `CMSIS_DAP_Connect`
     （反过来会把刚建好的 SWD 链路弄断，随后第一条 AP 访问即 NO_ACK）；
  3. 缺 **DP/AP 上电**（CTRLSTAT = 0x50000000，等 READOK + CDBGPWRUPACK）→ AP 访问必 FAULT；
  4. 目标**运行时** 0x20000000 是其程序在用的 RAM（读回 `0x0100xxxx` 运行期数据）→
     加"先停核"再测，否则 12 个用例全部数据不符（属测试环境问题，不是块传输问题）。

- 若日志出现 `block transfer failed ... falling back to single transfers`，
  说明固件不支持 `ID_DAP_TRANSFER_BLOCK`，速度不会改善，需转向 P3（Bulk）。
- 若提速符合预期，再评估 P3（Bulk，约再 9×）与 P4（提高 Max Clock）。

### 9.9 ⚠️ 实机崩溃与回滚（同日）

**现象**：块传输版本部署后 Keil 崩溃。Windows 应用程序日志：

```
错误应用程序名称: UV4.exe，版本: 5.43.1.0
错误模块名称: unknown      异常代码: 0xc0000005      错误偏移量: 0x00000000
```

`0xc0000005` + 错误模块 `unknown` + 偏移 `0` = **调用空指针**的典型特征。

**定位**：`DAP_TransferBlock`（`src/ORBMDK_HID.cpp`）**此前从未被调用过**，
其缺陷一直没暴露。查得两处硬伤：

| # | 问题 | 说明 |
|---|------|------|
| 35 | **越界写调用方缓冲区** | `memcpy(readData, &resp[5], (respLen - 5) / 4 * 4)` —— 按**响应长度**拷贝，而 HID 每次返回整个报告（64/65 字节），`(respLen-5)/4` 可能比请求的 `count` **多 1 个字** → 越界 4 字节，踩坏 AGDI 的数组 → 之后跳空指针 |
| 36 | 缺少 Transfer Count 校验 | 固件若未实现 `ID_DAP_TRANSFER_BLOCK`，可能返回其它命令的响应；长度够、ACK 也可能凑巧为 OK → 被误判成功，把垃圾数据交给调用方 |

**处置**：

1. **修复 #35 / #36**（按请求字数截断拷贝 + 校验 `Transfer Count`）；
2. **块传输改为「默认开启 + 能力自动探测 + 不兼容自动回退」**（不再依赖环境变量）：

   ```cpp
   // 每个上下文首次用到块传输时探测一次（载荷 = 读 DP IDCODE，只读无副作用）
   static bool ProbeBlockTransfer(int dapId);
   static void EnsureBlockTransferProbed(RDDIContext* ctx, int dapId);
   ```

   | 情况 | 行为 |
   |------|------|
   | 固件支持 `ID_DAP_TRANSFER_BLOCK` | 走块传输，14 字/往返（约 14×） |
   | 固件**不支持**（如 orbtrace） | 探测失败 → **永久回退逐字 `DAP_Transfer`**，功能不变 |
   | 传输中途块传输意外失败 | 立即回退（双保险） |
   | 排障需强制关闭 | `set ORBMDK_BLOCK_TRANSFER=0`（跳过探测，直接逐字） |

   > ⚠️ **本节结论已被 §13.7 推翻**：orbtrace **确实支持** `ID_DAP_TRANSFER_BLOCK`
> （实测 `resp = [06 01 00 01]`）。当时之所以判定"不支持"，是因为探测代码放在
> `DAP_RegWriteRepeat` 里，而那个入口在收到非法 regID 时会提前 return ——
> **探测根本没跑过**，所谓"不支持"只是陈旧产物。现探测已移到连上目标时执行。
>
> 下面这段保留作历史记录：

**实测（当时的错误结论）：orbtrace 固件未实现 `ID_DAP_TRANSFER_BLOCK`**，故在该固件上自动回退、
   行为与优化前一致；换用实现了该命令的调试器时**无需改代码即可提速**。

**保留的有效修复**：§9.5 的 HID 长度校验（纯加固、无副作用）、`DAP_TransferBlock`
的两处修复（函数正确性）。

**重新启用前的验证要求**（避免再次把 Keil 拖崩）—— ✅ **2026-09-30 已满足**，
结果与过程见 §9.8（12/12 通过、哨兵零损坏、加速比 7.86×）。验证程序：

```bat
:: 目标板接好、已上电后直接跑（程序内部自己切换两种模式并对比耗时）
bin\ORBMDK_BlockTransferTest.exe 0x20000000
```

`test/ORBMDK_BlockTransferTest.cpp`（构建：`test\build_test.ps1 -Source ORBMDK_BlockTransferTest.cpp`）
直接调用 `DAP_RegWriteRepeat` / `DAP_RegReadRepeat` —— 即 Keil 烧录实际走的两个接口，
不经过 AGDI。检查项：

| 检查项 | 说明 |
|--------|------|
| 功能 | 写入的数据必须原样读回 |
| **越界** | 数据数组前后各 8 个哨兵字，被踩坏立即报错 —— **直接针对本次崩溃的根因** |
| 边界 | 字数 1 / 13 / 14 / 15 / 28 / 64，覆盖 `kMaxBlockWords = 14` 的分块边界 |
| 速率 | 同一进程内先后跑逐字与块传输（靠 `FreeLibrary` / `LoadLibrary` 重载 DLL，使 `BlockTransferEnabled()` 的静态缓存重新初始化），直接给出加速比 |

**判定标准**：

| 结果 | 结论 |
|------|------|
| 两种模式全部 PASS，加速比 > 10× | 固件支持块传输，Keil 中会自动启用 |
| 有 FAIL 或哨兵被踩坏 | 仍有越界，**不能启用** |
| 加速比 ≈ 1 | 固件未实现 `ID_DAP_TRANSFER_BLOCK`（已自动回退）。（**此判定当时是错的**，见 §13.7：orbtrace 支持块传输） |

> 注：该文件刻意写成**纯 ASCII**。原先含中文时 MSVC 报 C4819（当前代码页无法表示的字符），
> 并连带产生一条假的 C4474 printf 告警。

**教训**：**"写好但从未被调用"的代码等于未测试代码。** `DAP_TransferBlock`
在仓库里存在已久却零调用点，因此其越界拷贝一直未被发现 —— 把这类函数接入
新路径前，应先给它补最小验证。

---

## 十、逆向分析工具链与已得结论（缓存）

> 目的：把 §4.7 / §4.8 / §4.11 / §4.12 / §4.13 期间用到的工具、方法、结论固化下来，
> 后续排查 AGDI ↔ RDDI 接口问题时可**直接复现**，不必重新推导。

### 10.1 工具与依赖

| 项 | 说明 |
|----|------|
| 脚本 | `tools/pe_re.py`（本仓库，5 个子命令合一，无第三方 PE 库依赖） |
| 依赖 | Python 3.x + `capstone`（`python -m pip install capstone`）。实测 Python 3.14 + capstone 5.0.6 |
| 目标 | 32 位 PE。`CMSIS_AGDI.dll` / `CMSIS_DAP.dll` 的 imagebase 均为 `0x10000000` |

> ⚠️ 早期版本散落在 `%TEMP%`，会随清理丢失；现已合并进仓库。

### 10.2 命令速查

```bash
PY="python tools/pe_re.py"
AGDI="D:\Keil_v5\ARM\BIN\CMSIS_AGDI.dll"     # 官方 AGDI 层
RDDI="D:\Keil_v5\ARM\BIN\CMSIS_DAP.dll"      # 当前生效的 RDDI 层
OFF="D:\Keil_v5\ARM\BIN\CMSIS_DAP.dll.bak"   # 官方 RDDI 层（对照基准）

$PY exports "$AGDI"                    # 导出表（名称/序号/RVA/VA）
$PY imports "$AGDI"                    # 导入的 DLL 名
$PY names   "$AGDI" "^(CMSIS_DAP|DAP|RDDI)_"   # 扫描符号名（ASCII + UTF-16LE）
$PY dis     "$AGDI" 0x10022A60 0xD0            # 按 VA 反汇编（自动注释字符串立即数）
$PY dis     "$OFF"  CMSIS_DAP_GetDeviceIDList 400   # 按导出名反汇编
$PY xref    "$AGDI" CMSIS_DAP_Disconnect       # 字符串 + 代码引用点
$PY slots   "$AGDI" 0x1002C000 0x1002CA00      # AGDI 函数指针槽映射 + 调用点统计
```

### 10.3 方法论要点

**（1）AGDI 通过 `GetProcAddress` 动态解析 RDDI 导出，不是静态导入**

`$PY imports CMSIS_AGDI.dll` 只列出 `KERNEL32/USER32/...` 等系统 DLL，**没有 `CMSIS_DAP.dll`**
—— 说明是运行时按名字解析。因此"AGDI 需要哪些 RDDI 函数"要靠 `names` 扫字符串，
"哪些会被真正调用"要靠 `slots` 统计调用点。

**（2）槽位识别：不能看相邻的 `push`**

解析循环的指令模式是：

```asm
push <函数名字符串VA>
push dword ptr [hModule]
mov  dword ptr [slot], eax     ; ← 保存的是「上一次」GetProcAddress 的结果
call esi                       ; GetProcAddress(当前名字)
```

即 `slot` 与 `name` **错开一位**。`tools/pe_re.py slots` 的做法是：`call` 时记下当前名字，
遇到下一条 `mov [slot],eax` 才落表。早期脚本按相邻 `push` 关联，结果整体错位一格。

**（3）无 NULL 检查 → 缺导出即空指针调用**

`slots` 输出里的 `call/jmp` 计数即该槽被间接调用的次数。**AGDI 全程不做 NULL 检查**，
所以官方 DLL 有、ORBMDK 没有的导出，一旦走到就是崩溃（见 §4.7 的 `CMSIS_DAP_Disconnect` 分析）。

**（4）同名槽回退**

AGDI 对两组函数做了"首选名取不到才用回退名"的处理，**共用同一个槽**：

```asm
cmp  dword ptr [0x10362F90], 0   ; GetDeviceIDList
jne  skip
push "CMSIS_DAP_DetectDAPIDList" ; 回退名，覆盖同一个槽
```

推论：**ORBMDK 两个首选名都导出 → 回退名在 Keil 下永远不会被调用**
（日志里出现的 `DetectDAPIDList` 来自测试宿主，不是 Keil）。

**（5）x86 32 位 `__cdecl` 参数个数与含义**

`push` 个数 = 参数个数（从右往左）；`add esp, N` 的 `N` 可交叉验证。例如
`call [0x10362F74]` + `add esp, 0xc` → 3 个参数 → `ConfigureInterface(handle, ifNo, cfg)`。
指针参数与整数参数的区分，靠调用点前后对 `[ebp+N]` 的用法（是否解引用、是否做 `shr`）。

**（6）`shr reg, 2` 是"字节数 / 4 = 元素个数"的信号**

官方 `CMSIS_DAP_GetDeviceIDList` 与 `DAP_GetDAPIDList` 都用它把字节长度换算成元素个数 ——
这是判断"某参数是字节数还是元素个数"最可靠的线索（§4.7 的 ABI 结论即由此得出）。

### 10.4 已得结论 A：AGDI 的 RDDI 函数指针槽（`CMSIS_AGDI.dll` v1.33.24）

解析位置：`0x1002C200`–`0x1002C660`。复现：`$PY slots "$AGDI" 0x1002C000 0x1002CA00`。

| 槽 | 函数名 | 槽 | 函数名 |
|----|--------|----|--------|
| `0x10362F08` | `RDDI_Open` | `0x10362F78` | `CMSIS_DAP_Connect` |
| `0x10362F0C` | `RDDI_Close` | `0x10362F7C` | `CMSIS_DAP_Disconnect` |
| `0x10362F10` | `RDDI_GetLastError` | `0x10362F80` | `CMSIS_DAP_ConfigureDAP` |
| `0x10362F14` | `RDDI_SetLogCallback` | `0x10362F84` | `CMSIS_DAP_ResetDAP` |
| `0x10362F18` | `DAP_GetInterfaceVersion` | `0x10362F88` | `CMSIS_DAP_GetNumberOfDevices` |
| `0x10362F1C` | `DAP_Configure` | `0x10362F8C` | `CMSIS_DAP_DetectNumberOfDevices` ← 回退 `...DetectNumberOfDAPs` |
| `0x10362F20` | `DAP_Connect` | `0x10362F90` | `CMSIS_DAP_GetDeviceIDList` ← 回退 `...DetectDAPIDList` |
| `0x10362F24` | `DAP_Disconnect` | `0x10362F94` | `CMSIS_DAP_JTAG_GetIDCODEs` |
| `0x10362F28` | `DAP_GetSupportedOptimisationLevel` | `0x10362F98` | `CMSIS_DAP_JTAG_GetIRLengths` |
| `0x10362F2C` | `DAP_GetNumberOfDAPs` | `0x10362F9C` | `CMSIS_DAP_ConfigureDebugger` |
| `0x10362F30` | `DAP_GetDAPIDList` | `0x10362FA0` | `CMSIS_DAP_Delay` |
| `0x10362F34` | `DAP_ReadReg` | `0x10362FA4` | `CMSIS_DAP_SWJ_Pins` |
| `0x10362F38` | `DAP_WriteReg` | `0x10362FA8` | `CMSIS_DAP_SWJ_Clock` |
| `0x10362F3C` | `DAP_RegAccessBlock` | `0x10362FAC` | `CMSIS_DAP_SWJ_Sequence` |
| `0x10362F40` | `DAP_RegWriteBlock` | `0x10362FB0` | `CMSIS_DAP_JTAG_Sequence` |
| `0x10362F44` | `DAP_RegReadBlock` | `0x10362FB4` | `CMSIS_DAP_Atomic_Control` |
| `0x10362F48` | `DAP_RegWriteRepeat` | `0x10362FB8` | `CMSIS_DAP_Atomic_Result` |
| `0x10362F4C` | `DAP_RegReadRepeat` | `0x10362FBC` | `CMSIS_DAP_SWO_Baudrate` |
| `0x10362F50` | `DAP_RegReadWaitForValue` | `0x10362FC0` | `CMSIS_DAP_SWO_Control` |
| `0x10362F54` | `DAP_Target` | `0x10362FC4` | `CMSIS_DAP_SWO_Status` |
| `0x10362F58` | `DAP_DefineSequence` | `0x102FA574` | `CMSIS_DAP_SWO_Data`（**非连续，独立全局**） |
| `0x10362F5C` | `DAP_RunSequence` | `0x10362FCC` | `CMSIS_DAP_Commands` |
| `0x10362F60` | `DAP_SetCommTimeout` | | |
| `0x10362F64` | `CMSIS_DAP_Detect` | | |
| `0x10362F68` | `CMSIS_DAP_GetGUID` | | |
| `0x10362F6C` | `CMSIS_DAP_Identify` | | |
| `0x10362F70` | `CMSIS_DAP_Capabilities` | | |
| `0x10362F74` | `CMSIS_DAP_ConfigureInterface` | | |

**调用点统计（`call/jmp` 次数 = 会被真正间接调用的次数）**

| 函数 | 调用点 |
|------|--------|
| `DAP_Target` | 13（最多） |
| `DAP_RegAccessBlock` | 10 |
| `DAP_RegWriteRepeat` | 7 |
| `DAP_RegReadRepeat` | 6 |
| `CMSIS_DAP_GetDeviceIDList` / `DetectDAPIDList` | 3：`0x1002CC27` / `0x1002CCAF` / `0x100334CA`（**均传 `sizeOfArray = 0x100` 字节**） |
| `DAP_WriteReg` | 3 |
| `CMSIS_DAP_Identify` | 3 |
| `DAP_ReadReg` | 2 |
| `CMSIS_DAP_JTAG_GetIDCODEs` | 1：`0x1002C921`（`(handle, &count, NULL, 0)`，**4 参**） |
| `CMSIS_DAP_JTAG_GetIRLengths` | 2：`0x1002C978` / `0x100334FD`（**4 参**） |
| `CMSIS_DAP_ConfigureDebugger` | 2：`0x1002CC4B` / `0x10033556`（**2 参，cfg = NULL**） |
| `CMSIS_DAP_ConfigureInterface` | 1：`0x10022AAA` |
| `DAP_Configure` | 1：`0x10022AC9` |
| `DAP_Connect` | 1：`0x10022B08` |
| `DAP_Disconnect` | 1：`0x10022D34` |
| **`CMSIS_DAP_Disconnect`** | **0 —— 从不调用**（故其缺失无害） |
| `CMSIS_DAP_DetectNumberOfDevices` / `DetectNumberOfDAPs` | 0（经 `mov eax,[slot]` + `call eax` 间接调用，静态扫描不到 `call [slot]`） |
| `CMSIS_DAP_Connect` | 0（同上，AGDI 优先用 `CMSIS_DAP_Connect` 槽 `0x10362F78`） |

> `CMSIS_DAP_WriteABORT` 出现在字符串表中，但不在本扫描区间的落表结果里，需扩大范围复核。

### 10.5 已得结论 B：AGDI 关键调用序列

**（1）初始化序列（`0x10022A60`–`0x10022B30`）**

```
RDDI_Open                                  → handle
CMSIS_DAP_Detect(&n)                       → n 必须 > 0
CMSIS_DAP_Identify(...)                    → 选接口
CMSIS_DAP_ConfigureInterface(h, ifNo, cfg) → 返回值非 0 ⇒ 立即以 0x2028 中止整个初始化
DAP_Configure(h, NULL)                     → 非 0 ⇒ 0x2028
CMSIS_DAP_Connect(h, connDetails)          → 0x2001 ⇒ …；0x1001 ⇒ 0x202A
（`CMSIS_DAP_Connect` 槽为 NULL 时退回 `DAP_Connect`）
```

其中 `cfg` 由基础串拼接而成，实测为：

```
Master=Y;Port=SW;SWJ=Y;Clock=1000000;Trace=Off;TraceBaudrate=0;TraceTransport=None;
```

**（2）设备探测例程（`0x1002C890`）—— 决定对话框列表行数**

```asm
0x1002C918  push esi                        ; esi = 调用方传入的 &count
0x1002C919  push dword ptr [0x102fa57c]     ; handle
0x1002C91F  call dword ptr [0x10362F94]     ; CMSIS_DAP_JTAG_GetIDCODEs(handle, &count, NULL, 0)
```

**`*count` 即"设备数量"**，直接决定 SW Device 列表行数（0 → 空列表）。
前置还会调用 `CMSIS_DAP_ResetDAP`。⚠️ 这是 §4.13 的关键结论。

**（3）设备枚举例程（`0x1002CBF0`）**

```asm
GetDeviceIDList(handle, arr, 0x100)         ; 非 0 ⇒ 0x2028
if (global == 0) ConfigureDebugger(handle, NULL)   ; 0x2000 ⇒ 0x2060；0x100D ⇒ 0x202B
DetectNumberOfDevices(handle, &cnt)         ; 同上映射；cnt > 64 ⇒ 0x2028
GetDeviceIDList(handle, arr, 0x100)         ; 非 0 ⇒ 0x2028
```

**（4）四个 NULL 检查（`0x1002CB66`–`0x1002CB94`）**

`CMSIS_DAP_ConfigureDAP` / `CMSIS_DAP_ResetDAP` / `CMSIS_DAP_ConfigureDebugger` /
`CMSIS_DAP_GetNumberOfDevices` —— 任一为 NULL 则跳到"函数缺失回退分支"（`0x1002CC62`），
该分支才会调用 `DetectNumberOfDevices`。**ORBMDK 四个都导出，故走不到那里**
（这正是 §4.12 判断失误的原因）。

### 10.6 已得结论 C：官方 RDDI 层 ABI 要点（对照 `CMSIS_DAP.dll.bak`）

| 函数 | 官方语义 | 备注 |
|------|----------|------|
| `CMSIS_DAP_GetDeviceIDList(h, int *idArray, size_t sizeBytes)` | 从内部设备表 `[0x1005ab2c]`（计数 `[0x1005ab24]`）逐个拷贝 4 字节**设备 ID**；`sizeBytes` 是**字节数**（实现里 `shr esi,2`） | 写入的是 **IDCODE，不是索引**（§4.7） |
| `CMSIS_DAP_GetNumberOfDevices` | 写内部计数 | |
| `CMSIS_DAP_ConfigureDebugger(h, const char *cfg)` | **2 参**，校验句柄后返回状态（0 = 成功） | 与 ORBMDK 一致 |
| `CMSIS_DAP_Disconnect(h)` | 存在但 AGDI 从不调用 | |
| `CMSIS_DAP_GetInterfaceVersion(h, int *version)` | 2 参，写 `0x00020000` | |

**ORBMDK 相对官方多出的导出**（5 个，Keil 用不到）：
`CMSIS_DAP_DetectDAPIDList`、`CMSIS_DAP_DetectNumberOfDAPs`、`CMSIS_DAP_JTAG_Configure`、
`CMSIS_DAP_ResetTarget`、`CMSIS_DAP_SWD_Configure`。

**官方有、ORBMDK 没有**（10 个）：`CMSIS_DAP_Disconnect` + 9 个 `ULINKPLUS_*`（ULINKplus 专用）。

### 10.7 已得结论 D：错误码

| 码 | 含义 |
|----|------|
| `0x2000` | `RDDI_DAP_ERROR` |
| `0x100D` | 特定错误（AGDI 单独分支处理，映射为 `0x202B`） |
| `0x2028` | AGDI 内部错误（`ConfigureInterface` / `DAP_Configure` / `GetDeviceIDList` / `DetectNumberOfDevices` 失败，或设备数 > 64） |
| `0x202B` | AGDI 内部错误（子调用返回 `0x100D`） |
| `0x2060` | AGDI 内部错误（子调用返回 `0x2000`） |
| `0x0D` | 官方 `GetDeviceIDList` 在 `idArray == NULL` 时返回（即 `RDDI_BADARG`） |

### 10.8 复现清单（从头重跑一遍）

1. `$PY exports "$AGDI"` → 确认版本（本档基于 **v1.33.24.0**，imagebase `0x10000000`）
2. `$PY slots "$AGDI" 0x1002C000 0x1002CA00` → 得到 §10.4 的槽位表与调用点
3. `$PY xref "$AGDI" <函数名>` → 定位该名字的解析点与引用点
4. `$PY dis "$AGDI" <VA> <len>` → 反汇编目标调用点，读出参数个数与含义
5. `$PY dis "$OFF" <导出名> <len>` → 与官方实现对照，确认 ABI 与输出参数语义
6. 必要时用 `ORBMDK_LOG_LEVEL`（`0=DEBUG 1=INFO 2=WARN 3=ERROR`）打开日志，
   把 DLL 侧实际收到的参数与反汇编结论交叉验证

> **通用教训**：AGDI 只认输出参数、不认返回值（§4.6 #18）。
> 每次改动涉及"写回指针参数"的函数时，都要回到 §10.4 的调用点表确认
> **该出参被 AGDI 拿去做什么用**，再决定失败路径要不要写初值。

---

## 十一、CMSIS-DAP V2 (USB Bulk) 接入与状态 LED

> 日期：2026-09-30。对应两个问题："DLL 无法识别到 V2"、"`DAP_HostStatus` 指示的 LED 不亮"。

### 11.1 V2 Bulk：实现了，但从未被调用

**现象**：orbtrace 固件支持 CMSIS-DAP V2（USB Bulk），但本层始终走 V1（HID）。

**根因**：`ORBMDK_USB_Bulk_Init()`（`src/ORBMDK_USB_Bulk.cpp:396`）内部已经实现了
完整的"**优先 V2 Bulk、失败回退 V1 HID**"逻辑，但**全项目没有任何调用点** ——
因此 `ORBMDK_USB_Bulk_GetMode()` 恒返回 `USB_BULK_NOT_INITED`，所有路径
（含 `ORBMDK_HID_DAPCommand` 里的 PC 采样分支）都退到 HID。

**修复**：

| # | 位置 | 改动 |
|---|------|------|
| 37 | `RDDI_Open` | 首次打开时调用 `ORBMDK_USB_Bulk_Init(0, 0, nullptr)`（内部优先 Bulk、回退 HID），并把实际传输模式记入日志 |
| 38 | `ORBMDK_HID_DAPCommand` | 增加 V2 分发：处于 Bulk 模式时走 `ORBMDK_USB_Bulk_DAPCommand`，并把响应**规整为 V1 布局**（前置报告ID占位字节），使上层所有解析逻辑零改动 |

**⚠️ 关键约束：不要把这个调用放进 `ORBMDK_HID_OpenDevice`。**
它会持有 `g_hidMutex`，而回退路径又会调用 `ORBMDK_HID_OpenDevice`，
`std::mutex` 非递归 → **死锁**。放在 `RDDI_Open` 才安全。

**V1 / V2 报文差异**（规整的动机）：

| | 命令 | 响应 |
|---|------|------|
| V1 HID | `[报告ID][命令ID][负载]` | `[报告ID][命令ID][负载]` |
| V2 Bulk | `[命令ID][负载]` | `[命令ID][负载]` |

本层所有解析都假定 `resp[0]=报告ID, resp[1]=命令ID`（见 §3.3），
因此 V2 分支只需前置一个占位字节，两层即可共用同一套解析代码。

### 11.1.1 V2 仍打不开：`_findAndOpenDevice` 的三个缺陷

接上 §11.1 后实测仍只有 `CMSIS-DAP v1`，进一步定位到 `_findAndOpenDevice`
（`src/ORBMDK_USB_Bulk.cpp`）的三个缺陷 —— 任一个都足以导致 V2 打不开：

| # | 问题 | 说明 |
|---|------|------|
| 39 | **用 `WinUsb_GetDescriptor` 取设备描述符** | 复合设备的**接口**上请求"设备描述符"通常直接失败 → 代码在 `continue` 处跳过 → **永远打不开**。改为**从设备路径串解析 VID/PID**（`\\?\usb#vid_1209&pid_3443&mi_01#...`） |
| 40 | **未校验接口类型** | 同一 VID/PID 下复合设备暴露多个接口：HID 接口(class `0x03`) 在前、V2 Bulk 接口(class `0xFF` vendor-specific) 在后。不校验会**先匹配到 HID 接口并 break** —— 而它没有 Bulk 端点。改为要求 `bInterfaceClass == 0xFF` |
| 41 | **未校验端点** | HID 接口无 Bulk 端点，`bulkInPipe/bulkOutPipe` 保持 0，后续 `WinUsb_WritePipe(..., 0, ...)` 必然失败。改为**必须同时找到 Bulk IN 与 Bulk OUT**才接受 |

同时补了 `BulkTrace()` 诊断日志（该文件此前**完全没有日志**，见 §8.1）：
枚举到的每个接口、被拒绝的原因、最终选中的端点都会写入
`%TEMP%\ORBMDK_RDDI.log`，前缀 `[ORBMDK][INFO][BULK]`。

### 11.1.2 对话框仍显示 "CMSIS-DAP v1"：产品名的来源

**字节搜索结论**：`"CMSIS-DAP v"` 这个字面量在 **AGDI 与官方 RDDI DLL 里都不存在**
（`tools/pe_re.py` + 原始字节搜索均确认）。所以对话框里的
`CMSIS-DAP v1 / v2` **不是拼出来的，而是来自 USB 接口的字符串描述符**。

而 ORBMDK 的产品名来自 `HidD_GetProductString`（HID 接口）→ 永远是 `CMSIS-DAP v1`。

| # | 位置 | 改动 |
|---|------|------|
| 42 | `ORBMDK_USB_Bulk.cpp` | 打开 V2 接口后，用 `ifaceDesc.iInterface` 读该接口自己的字符串描述符（`WinUsb_GetDescriptor(USB_STRING_DESCRIPTOR_TYPE, ...)`），经 `ORBMDK_USB_Bulk_GetProductName()` 暴露 |
| 43 | `RDDI_Open` | 处于 Bulk 模式时，用 V2 接口的产品名覆盖 `ctx->productName` |

**关键**：Keil 对话框的适配器名字就是 `CMSIS_DAP_Identify(idNo=2)` 的返回值（§2.2）。
只改传输层不改产品名，对话框会一直显示 v1。

**待实测**：

1. `%TEMP%\ORBMDK_RDDI.log` 中搜 `[BULK]` 与 `transport =` —— 应看到
   `opened V2 iface N: bulkIn=0x.. bulkOut=0x..` 与 `interface string N = 'CMSIS-DAP v2'`，
   以及 `RDDI_Open: transport = CMSIS-DAP v2 (USB Bulk)`；
2. Keil 对话框应出现 `CMSIS-DAP v2`；
3. 烧录耗时应显著改善（V2 单包 512 字节、无 1 ms 轮询间隔）。

> 若日志里出现 `skip iface ...: class=0x03` 之后没有命中项，说明 WinUSB 未绑定到
> Bulk 接口（需要用 Zadig/Keil 自带驱动安装），此时会安全回退 V1。

### 11.1.3 日志开关：改为文件驱动（免命令行 / 免重启）

**问题**：原开关只有环境变量 `ORBMDK_LOG_LEVEL`，而 µVision 从 IDE 启动、不继承
任意 shell 的环境，于是"先在命令行 `set` 再启动 µVision"成了必需步骤 —— 实测太麻烦。

**改法**：`ORBMDK_LogLevel()` 改为**先读文件、再读环境变量**，并支持热更新。

| 项 | 说明 |
|----|------|
| 文件 | `%TEMP%\ORBMDK_LOG_LEVEL`，纯文本，内容只写一个数字（`0=DEBUG 1=INFO 2=WARN 3=ERROR`） |
| 生效 | **µVision 运行中**改动最多 **1 秒**生效（内部 1 秒节流的 `GetTickCount64` 缓存，避免每条日志都做 IO） |
| 恢复 | **删掉文件即回到默认 ERROR**，同样不用重启 |
| 优先级 | 文件 > 环境变量（后者只在进程启动时读一次，保留兼容） |
| 例外 | `ORBMDK_USB_Bulk.cpp` 的 `[BULK]` 埋点**直写文件、不受级别控制** —— 传输层排障永远有据可查 |

> 判定 DLL 是否为最新构建：日志里若**完全没有** `Init: entered` 行，说明 Keil 加载的
> 仍是旧 DLL。µVision 会把 DLL 常驻内存，**必须结束整个 `UV4` 进程**再重开，
> 仅关闭调试会话无效（实测踩过：DLL 部署于 14:57，而 `UV4` 启动于 14:53，
> 之后所有测试都跑在旧代码上，日志时间戳停在 14:11）。

### 11.1.4 **V2 找不到的真正根因：用错了设备接口 GUID**

前几轮一直卡在"枚举里根本没有 V2 接口"。加上逐节点埋点后，日志给出：

```
candidate[1] '\\?\usb#vid_1209&pid_3443#29b4e080e2289e06#{a5dcbf10-...}'
  reject: WinUsb_Initialize failed, err=8
enumeration done: 1 candidate(s) for vid_1209&pid_3443, opened=0
```

注意路径里**没有 `&mi_XX`** —— 这是复合设备的**父节点**，不是接口节点。

**设备树实测**（orbtrace 是一个复合设备，`USB\VID_1209&PID_3443`）：

| 接口 | FriendlyName | Class |
|------|--------------|-------|
| `MI_00` | USB 串行设备 (COM34) | Ports |
| `MI_02` | Trace | USBDevice |
| `MI_03` | Control Proxy | USBDevice |
| `MI_04` | HID 供应商定义设备 | **HIDClass** ← V1 |
| **`MI_05`** | **CMSIS-DAP v2** | **USBDevice** ← V2 |
| `MI_06` | Target power | USBDevice |
| `MI_07` | Version: v1.4.3-0-g2413e51 | USBDevice |

**根因**：V2 接口（`MI_05`）注册的设备接口 GUID **不是**
`GUID_DEVINTERFACE_USB_DEVICE`（`{A5DCBF10-...}`），而是 CMSIS-DAP v2 规范规定的
WinUSB 接口 GUID：

```
{CDB3B5AD-293B-4663-AA36-1AAE46463776}
```

**验证方式**：在官方 `CMSIS_DAP.dll` 的 `.rdata` 里按字节搜索该 GUID —— 命中于
偏移 **`0x41DE5`**。即官方正是用它枚举 V2 接口的。

**修复**（`src/ORBMDK_USB_Bulk.cpp`）：

| # | 改动 |
|---|------|
| 44 | 新增 `CMSIS_DAP_V2_GUID_DEVINTERFACE` 常量；枚举函数参数化为 `_findAndOpenDeviceInGuid(guid, ...)` |
| 45 | 新增 `_findAndOpenDevice()` 包装：**先**用 CMSIS-DAP v2 专用 GUID 枚举，**再**用普通 USB 设备 GUID 兜底 |

**顺带解释了名字问题**：`CMSIS_DAP_Identify(idNo=2)` 的产品名在 V1 下取自 HID 接口
（`MI_04` → "CMSIS-DAP v1"）。现在能打开 `MI_05`，即可读它自己的接口字符串
（"CMSIS-DAP v2"，见 §11.1.2）。

**教训**：WinUSB 设备（WCID）的接口 GUID 由设备自己的 MS OS 描述符决定，
**不能假设是 `GUID_DEVINTERFACE_USB_DEVICE`**。定位这类"明明在设备管理器里、
代码却枚举不到"的问题，最快的办法是到官方实现里搜 GUID 字节。

#### 11.1.4 补充：真正卡住的最后一步（已修正上文结论）

上面把"专用 GUID 枚举到 0 个"归因于 GUID 选择，**这个结论是错的**。

用独立测试程序（`%TEMP%\guidtest.cpp`，32/64 位都试过）直接验证，结果完全正常：

```
WinUSB   ({dee824ef}) total=5   → 含 ...mi_05...   ✓
CMSISv2  ({cdb3b5ad}) total=1   → 正是 ...mi_05...  ✓
直接 CreateFile + WinUsb_Initialize MI_05 → OK ✓（两个 GUID 各试一次都成功）
```

**真因**：把 `SetupDiGetClassDevs` 的 GUID 参数化之后，**漏改了配套的枚举调用**：

```cpp
// 获取集合时已改成 guid ✓
deviceInfoSet = SetupDiGetClassDevs(guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

// 但循环里仍硬编码 —— 两者必须一致，否则枚举返回 0 且不报任何错
for (DWORD index = 0; SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL,
            &USB_GUID_DEVINTERFACE, index, &deviceInterfaceData); index++) {
```

| 轮次 | 传入 GUID | 枚举用的 GUID | 候选数 |
|------|-----------|---------------|--------|
| WinUSB | `{dee824ef}` | `{a5dcbf10}` ✗ | 0 |
| CMSIS-DAPv2 | `{cdb3b5ad}` | `{a5dcbf10}` ✗ | 0 |
| USBDevice | `{a5dcbf10}` | `{a5dcbf10}` ✓ | 1 |

第三轮"碰巧一致"才拿到候选，正是这个巧合把排查带偏了两轮。

**修复后实测成功**：

```
candidate[1] '...mi_05...#{dee824ef-...}'
interface string 7 = 'CMSIS-DAP v2'
opened V2 iface 5: bulkIn=0x85 bulkOut=0x03
Init: V2 Bulk mode OK (product='CMSIS-DAP v2')
RDDI_Open: transport = CMSIS-DAP v2 (USB Bulk)     ← 成功
```

#### 11.1.5 残留问题：同一会话内反复 Open/Close 会退化到 V1

> ✅ **2026-09-30 已彻底解决 —— 真因不是"句柄释放延迟"，见 §17.1。**
> 本层 `_closeWinUSB()` 没有取消挂起的 overlapped IRP 就 `WinUsb_Free` +
> `CloseHandle`，内核一直持有 file object 引用 → 该 WinUSB 接口被**永久**独占，
> 此后任何进程 `CreateFile` 都是 `err=5`。下面的 #47 重试只是缓解，
> 真正修的是**关闭路径**（`CancelIoEx` → `WinUsb_AbortPipe/ResetPipe` → 才释放）。

实测 7 次 `RDDI_Open` 中只有第 1 次走 V2，其余 6 次全变成 V1。日志给出的直接原因：

```
candidate[1] '...mi_05...#{dee824ef-...}'
  reject: CreateFile failed, err=5      ← ERROR_ACCESS_DENIED
```

AGDI 在一次调试会话里会反复调用 `rddi_Open`（实测 7 次），每次都会先
`Shutdown` 再重开；而 **WinUSB 设备是独占的**，上一个句柄尚未完全释放时第二次
`CreateFile` 就返回 `ACCESS_DENIED`，于是整场会话退化成 V1 HID。

| # | 位置 | 改动 |
|---|------|------|
| 46 | `ORBMDK_USB_Bulk_Init` | 已是有效 V2 连接时**直接复用**，不再拆开重开（幂等） |
| 47 | `_findAndOpenDeviceInGuid` | `CreateFile` 遇 `ERROR_ACCESS_DENIED` 时**有限重试**（5 × 100 ms），应对句柄释放延迟 |

**通用教训（比这次的 bug 更值钱）**：这次绕了四轮，前两轮都在"猜"，真正的转折点是
**写了个独立测试程序**——它把"GUID 不对 / 驱动不对 / 我的调用序列不对"三种可能
一次性排除到只剩一种。凡是"代码里的调用在 A 环境正常、在 B 环境异常"的问题，
先做**最小可复现的独立程序**，比在被测进程里反复重启试探快一个数量级。

> ⚠️ V2 只改变**传输层**，不改变命令集。orbtrace 依然不支持
> ~~`ID_DAP_TRANSFER_BLOCK`（§9.9），块传输仍会自动回退~~（**该结论有误，见 §13.7**：
> orbtrace 支持块传输）；V2 的提速来自
> 包长与延迟，而非块传输。

### 11.2 状态 LED：`DAP_HostStatus` 无人调用

**现象**：烧录、调试时 orbtrace 的 Connect / Running LED 均不亮。

**根因**：`DAP_HostStatus` 两层都已实现（`ORBMDK_HID.cpp:968` 与
`ORBMDK_RDDI.cpp` 的导出），但**官方 AGDI 从不调用它** ——
其符号表里没有 `DAP_HostStatus`，反汇编（§10.4）也找不到调用点。
因此必须由本层自己驱动。

**修复**：新增 `SetHostLed()` / `TrackApTarWrite()` / `TrackApDataWrite()`，
由本层根据自身操作推断状态：

| LED | 触发 |
|-----|------|
| **Connect** | 连接目标成功（`DetectTargetDapIdList` 中 `DAP_ConnectTarget` 成功后）点亮；`RDDI_Close` 熄灭 |
| **Running** | 监视对 **DHCSR(`0xE000EDF0`)** 的写入：`C_HALT`(bit1) 置位 → 目标停机 → 熄灭；清零 → 目标运行 → 点亮 |

**AP 写地址的跟踪**：AGDI 通过 `SWD_WriteData`（TAR + DRW）写 DHCSR，
因此在 `DAP_WriteReg` 与 `DAP_RegAccessBlock` 里跟踪：

- 写 AP **TAR**（寄存器号 5）→ 记住 `ctx->lastApTar`
- 写 AP **DRW**（寄存器号 7）且 `lastApTar == 0xE000EDF0`
  → 按 `C_HALT` 更新 Running LED

**只在状态变化时发包**（`ctx->hostConnectLed` / `hostRunningLed` 做缓存），
避免每个字一次 USB 往返。

**待实测**：接上目标后 Connect LED 亮；烧录 / 运行时 Running LED 亮，停机时灭。

---

## 十二、VS2022 工具链升级导致的 Keil 崩溃（0xc0000005 in MSVCP140.dll）

> 日期：2026-09-30。环境由 VS2017 换到 VS2022 后，Keil µVision 一启动调试就崩溃。

### 12.1 现象

Windows 应用程序日志：

```
Faulting application name: UV4.exe, version: 5.43.1.0
Faulting module name: MSVCP140.dll, version: 14.29.30157.0
Exception code: 0xc0000005
Faulting module path: D:\Keil_v5\ARM\ARMCLANG\bin\MSVCP140.dll
```

`%TEMP%\ORBMDK_RDDI.log` **完全没有生成** —— 说明还没走到 `RDDI_Open` 就已经崩了。

### 12.2 根因：新工具集 + 旧运行库（MSVC 运行时只向前兼容）

| 运行库 | 版本 | 位置 |
|--------|------|------|
| Keil 自带（UV4 实际加载的那个） | **14.29**.30157.0 | `D:\Keil_v5\ARM\ARMCLANG\bin\` |
| 系统 SysWOW64 | 14.50.35719.0 | `C:\Windows\SysWOW64\` |
| 本次编译所用工具集 | **14.44**.35207 | VS2022 |

MSVC 运行时**只保证"向前兼容"**：旧工具集编译的程序可以跑在新版运行时上；
反向（新工具集编译、却跑在旧运行时上）**不受支持**。

- 旧环境 VS2017(14.16) 编译：需求被 14.29 满足 → 正常；
- 新环境 VS2022(14.44) 编译：UV4 进程里加载到的是 Keil 目录下的旧 14.29 → **不受支持组合** → 崩在 `MSVCP140.dll`。

`dumpbin /dependents` 确认旧产物的 CRT 依赖：

```
MSVCP140.dll      <- _Mtx_lock / _Mtx_unlock           (std::mutex)
                     _Query_perf_counter / _Query_perf_frequency  (std::chrono)
                     ?_Xlength_error / ?_Xout_of_range / ?_Xinvalid_argument / ?_Throw_Cpp_error
VCRUNTIME140.dll  <- __CxxFrameHandler3 / _CxxThrowException ...
api-ms-win-crt-*  <- UCRT
```

> 注意：这 8 个 `MSVCP140` 导入符号在旧版 14.29 的**导出表里全都存在** —— 所以不是"缺符号"，
> 而是**新 STL 头文件 + 旧运行库实现**的 ABI 不匹配，属未定义行为。

### 12.3 修复：改为静态链接 CRT（`/MT`）

`build.ps1` / `test/build_test.ps1` 的编译开关由 `/MD` 改为 **`/MT`**。

| 方案 | 评价 |
|------|------|
| **`/MT`（采用）** | 产物不再依赖 `MSVCP140` / `VCRUNTIME140` / UCRT，宿主进程里是哪个版本都无所谓 |
| 替换 Keil 的 `ARMCLANG\bin\MSVCP140.dll` | 要改 Keil 安装目录，可能影响 ARMCLANG 自身工具，风险外溢 |
| 让 UV4 改用系统 `SysWOW64` 的 14.50 | 依赖 Keil 的 DLL 搜索路径行为，不可控 |

`/MT` 对本项目是安全的：RDDI 是**纯 C ABI**（导出全为 `RDDI_FUNC` 函数，所有缓冲区由调用方提供，
不跨模块传 STL 对象或堆指针），不存在"两个 CRT 堆互相 free"的问题。

**验证**（改后）：

```
D:\Keil_v5\ARM\BIN\CMSIS_DAP.dll   323584 字节   machine (x86)
Dependents: KERNEL32 / SETUPAPI / WINUSB / HID / SHLWAPI      <- 已无任何 CRT DLL
```

> ⚠️ **不要改回 `/MD`**。`build.ps1` 中该处已就地写明原因。

### 12.4 附带修复：PowerShell 脚本的编码与行尾

本轮还发现并修掉一个**构建脚本自身**的坑（与崩溃无关，但会让构建失败）：

`build.ps1` / `deploy.ps1` / `test\build_test.ps1` 原本是**无 BOM 的 UTF-8 + LF 换行**。
Windows PowerShell 5.1 对无 BOM 脚本按 **ANSI(936/GBK)** 解码，于是：

```
…是安全的。<LF>
              "。" = E3 80 82，末尾字节 0x82 与后面的 0x0A 被 GBK 当成一个双字节字 → <LF> 被吃掉
```

→ 换行消失 → 下一行 `$CompilerFlags = "/c /nologo /MT …"` 被并入注释，
`/MT` 与 `/utf-8` **全部失效**，编译报 C4819 + C1004（"发现意外的文件尾"）。

| 试验 | 结果 |
|------|------|
| LF + 无 BOM | 赋值被吞 ❌ |
| CRLF + 无 BOM | 正常 ✅ |
| LF + UTF-8 BOM | 正常 ✅ |

是否被吞取决于**该行字节能凑成几对 GBK 双字节**（奇偶性），所以同一个文件里有的行正常、有的行被吞，
且**不报任何语法错误**。旧脚本之所以一直没暴露，是运气。

**处理**：三个 `.ps1` 统一为 **CRLF + UTF-8 BOM**。

> 教训：脚本里写非 ASCII 注释时，必须让 PowerShell 明确知道编码（BOM / CRLF，最好两者都要），
> 否则注释里的多字节字符可能"吃掉"换行，把下一行代码变成注释的一部分 —— 现象会诡异到像是编译器坏了。

---

## 十三、V2 (USB Bulk) 通道打通 + 传输模式可选

> 日期：2026-09-30。承接 §11（V2 能打开但用不了）。用户诉求：**V1 与 V2 两种模式都要能选用**，
> 且 bulk 出问题时要能回退。

### 13.1 现象

Keil 对话框能看到 `CMSIS-DAP v2`，但一选它就退化成 V1。日志（`%TEMP%\ORBMDK_RDDI.log`）：

```
Init: V2 Bulk mode OK (product='CMSIS-DAP v2')
[RDDI] CMSIS_DAP_Connect: DAP_ConnectTarget failed, mode=-1     ← V2 下命令全失败
Init: entered ...  reject: CreateFile failed, err=5             ← 之后连 V2 都打不开了
Init: V2 Bulk unavailable -> fall back to V1 HID
```

### 13.2 根因（三个独立缺陷，缺一不可）

**（1）分发契约不一致 —— V2 命令"成功"被当成"失败"**

```cpp
// src/ORBMDK_USB_Bulk.cpp：成功时返回**字节数**
return (int)copy;

// src/ORBMDK_HID.cpp：契约是 0 = 成功
const int r = ORBMDK_USB_Bulk_DAPCommand(...);
if (r != 0) { LOG_HID_ERROR("V2 Bulk DAPCommand failed: result=%d", r); return r; }
```

于是**每一条成功的 V2 命令都被判失败** → `DAP_ReadReg` / `DAP_ConnectTarget` 全挂 →
AGDI 立刻退回 V1。修正：成功统一返回 `0`，字节数由 `*respLen` 回传。

**（2）出包长度用了 512，而端点只有 64**

> ⚠️ 本小节的机理描述（"MI_05 是 Full Speed、描述符把 `wMaxPacketSize` 报成 0"）
> 已被 §17.2 的实测取代：该端点实际是 **High Speed、描述符报 512**，而设备用
> `DAP_Info(0xFF)` 自报 **508**。方向一致（不能用描述符整包），但数值与原因以
> §17.2 为准；`ORBMDK_BULK_PAD` 这个手动开关也已随之删除。

原实现把命令补齐到 512 字节（DAP_PACKET_SIZE）再发。orbtrace 的 MI_05 是
**Full Speed** bulk 端点，真实包长 64（其描述符把 `wMaxPacketSize` 报成 `0`，
本身就是不规范）。实测标定：

| `ORBMDK_BULK_PAD` | 结果 |
|---|---|
| `0`（精确长度） | 命令失步，读超时 |
| **`64`（端点包长）** | **读写即时成功** ✅ |
| `512` | 第一条能读到数据，第二条起 `bulkWrite` 直接 TIMEOUT（设备收不完、主机积压） |

修正：默认取端点 `wMaxPacketSize`（报 0 时按 FS bulk 的 64），保留
`ORBMDK_BULK_PAD` 供现场标定。

**（3）超时后不释放设备 → WinUSB 接口被永久占用**

```cpp
CancelIo(g_winusb.deviceHandle);   // 异步，返回时 IRP 往往还挂在驱动里
CloseHandle(overlapped.hEvent);    // 直接关事件对象
return -2;                         // …随后 _closeWinUSB() 里 WinUsb_Free + CloseHandle
```

有未完成 IRP 时内核不放 file object → 设备接口不释放 → 之后**所有** `CreateFile`
都是 `ERROR_ACCESS_DENIED(5)`，且是永久的（表现为"V2 整场会话再也打不开"）。
另外 `ORBMDK_USB_Bulk_Shutdown()` 里 `if (!g_ctx.initialized) return;` 会漏关
`g_winusb` 中仍然有效的句柄。

修正：
- 新增 `_cancelOverlapped()`：`CancelIoEx` 之后**带超时地等 IRP 真正完成**再释放；
- `ORBMDK_USB_Bulk_Shutdown()` 不再看 `g_ctx`，一律按 `g_winusb` 实际状态关闭。

**（4）附带：设备 FIFO 里的历史响应**

设备的响应是**流式**的，超时/中断后没取走的响应会留在设备 FIFO 里，
下一次打开时第一条命令就会读到**上一次的响应**（实测：发 `DAP_Info` 收到 `05 01 77 14`）。
修正：打开后 `AbortPipe` + `ResetPipe` + 把 IN FIFO 读空（`_flushAndDrainPipes()`），
并在命令层校验"响应首字节 == 命令号"，不符则冲刷后重试一次；
`0xFF` 单独识别为"设备不支持该命令"（DAPLink 约定），不做无意义的重试。

### 13.3 传输模式可选（本轮的显式需求）

新增开关，与日志级别同风格（**文件优先、环境变量次之**，免重新编译）：

| 开关 | 取值 |
|------|------|
| `%TEMP%\ORBMDK_TRANSPORT`（优先，运行期可改） | `auto` / `bulk`(=v2) / `hid`(=v1) |
| 环境变量 `ORBMDK_TRANSPORT` | 同上 |

| 取值 | 行为 |
|------|------|
| `auto`（缺省） | 优先 V2；不可用才回退 V1。**已回退后不再每次重试**（避免反复触发 CreateFile 失败路径） |
| `bulk` | 只用 V2；不可用**直接失败**，不做静默降级（便于定位） |
| `hid` | 只用 V1 |

切换到与当前不同的模式时，`RDDI_Open` 会重新初始化传输层（这是 AGDI 唯一能触发切换的入口）。

**V2 下的产品名/序列号**：V2 时没有打开 HID 接口，`HidD_GetProductString` /
`HidD_GetSerialNumberString` 都不可用。产品名改从 **Bulk 接口自己的字符串描述符**读
（§11.1.2 已有），序列号新增从设备的 `iSerialNumber` 字符串描述符读
（`ORBMDK_USB_Bulk_GetSerialNumber`），否则 `CMSIS_DAP_Identify(idNo=3)` 只能返回 `"Unknown"`。

### 13.4 实测（orbtrace v1.4.3 + STM32F1）

三种模式下的 DP IDCODE 读取完全一致：

| `ORBMDK_TRANSPORT` | `Identify(idNo=2)` | `DAP_ReadReg(DP_IDCODE)` |
|---|---|---|
| `auto` | `CMSIS-DAP v2` | `0x2BA01477` ✅ |
| `bulk` | `CMSIS-DAP v2` | `0x2BA01477` ✅ |
| `hid` | `CMSIS-DAP v1` | `0x2BA01477` ✅ |

V2 的报文实测（日志节选）：

```
opened V2 iface 5: bulkIn=0x85(0) bulkOut=0x03(0)
bulkWrite OUT ep=0x03 len=64 -> 64 bytes, 0 ms
bulkRead  IN ep=0x85 maxLen=64 -> 7 bytes, 0 ms
DAPCommand: cmd=0x05 outLen=64 -> resp 7 bytes [05 01 01 77]   ← ACK=OK + IDCODE
```

> ⚠️ 注意 V2 在这里**并不比 V1 快**：端点就是 64 字节，与 HID 单包相同，且 orbtrace
> 不支持 `ID_DAP_TRANSFER_BLOCK`（§9.9）。V2 的收益要等换用实现了块传输的调试器。

### 13.5 顺带修复：测试程序的陈旧原型（会直接崩）

`test/ORBMDK_RDDI_FullTest.cpp` 有多处仍按**旧 API**声明函数指针，实参错位后
直接往垃圾地址写入 → `0xc0000005`（cdb 定位：`CMSIS_DAP_SWO_Status+0x28`、
`DAP_RegWriteBlock+0x5e`）。V1 / V2 下都崩，与本次传输层改动无关。

| 函数 | 测试里的旧原型 | 真实签名 | 后果 |
|------|----------------|----------|------|
| `CMSIS_DAP_GetDeviceIDList` | `(h, int *count, char *buf, int len)` | `(h, int *idArray, size_t bytes)` | `sizeOfArray` 收到指针值（天文数字）→ 向 `&count` 后狂写 → 踩坏栈 |
| `CMSIS_DAP_SWO_Status` | `(h, uint8_t *status)` | `(h, int *count, int *status)` | 第 3 参取寄存器垃圾当指针写入 → AV |
| `CMSIS_DAP_SWO_Data` | `(h, int*, uint8_t*)` | `(h, int*, void*, int*)` | 同上 |
| `CMSIS_DAP_GetInterfaceVersion` | `(h, char*, int)` | `(h, int *version)` | 版本号是 int，不是字符串 |
| `DAP_RegReadBlock` / `DAP_RegWriteBlock` | 按"同一寄存器重复 N 次"调用 | `(h, DAP_ID, numRegs, const int *regIDArray, dataArr)` | `dataArray` 收到字面量 `4` → AV |
| `DAP_Target` | `(h, int, int*)` | `(h, const char*, char*, int)` | 未被调用，仅预防 |

已按真实签名修正（崩溃消失）。**仍待同步**：该文件里的 `DAP_REG_*` 常量仍是
"字节偏移"风格（例如 `DAP_REG_AP_DRW` 传成 `0x0001000C`），与
`rddi_dap.h` 的**编号**语义（DP 0–3 / AP 4–7）不符，导致 Test 9/10 报
`RDDI_DAP_BAD_REGISTER_ID`。因此该测试目前 `PASSED 22 / FAILED 20`，
**不再是 40/40**；剩余失败分两类：
1. 测试自身的常量/预期陈旧（上面这条）；
2. 目标侧状态：`DP CTRL/STAT = 0x00000000 [NOT powered]` —— 目标被复位后调试电源未上，
   AP 访问必然 FAULT。三种传输模式表现**完全一致**，可排除传输层。

> 复现崩溃定位的方法：`cdb` + `link /MAP`（见 §10）：
> `cdb -g -G -c "g; .ecxr; kb; q" bin\ORBMDK_RDDI_FullTest.exe`

### 13.6 ⭐ 最后一环：DLL 卸载时泄漏 WinUSB 句柄（"V2 只有第一次能用"的真因）

§13.2(3) 修完后，Keil 里**仍然是**第一次 V2、之后全 V1。日志给出两个铁证：

```
19:25:31.484  RDDI_Close: handle=1 closed
19:25:33.934  RDDI_Open called
19:25:33.935  RDDI_Open: opening transport (current=0, ...)   ← ★ 全局状态归零
              Init: entered …  reject: CreateFile failed, err=5 ← ★ MI_05 仍被占用
```

推论链：

1. `RDDI_Close` **不碰** `g_ctx`，但重载后 `GetMode()` 却是 `NOT_INITED` →
   说明那是一个**全新的 DLL 实例**：**AGDI 在两次 `rddi_Open` 之间卸载并重新加载了本 DLL**
   （§2.1 的 `LoadLibrary` 在 `InitInstance()` 里，窗口/会话切换时会走一次卸载-重载）。
2. **WinUSB 句柄属于进程，不随 DLL 卸载而关闭**。`DllMain(DLL_PROCESS_DETACH)` 原来只调了
   `ORBMDK_HID_Shutdown()`，**从没关 V2 的句柄** →
   `g_winusb.winusbHandle` / `deviceHandle` 永久泄漏在 `UV4.exe` 里 →
   新实例的 `CreateFile(MI_05)` 永远 `ERROR_ACCESS_DENIED(5)` → 只能退回 V1。

**修复**（`src/ORBMDK_DLL.cpp`）：`DLL_PROCESS_DETACH` 里补上 `ORBMDK_USB_Bulk_Shutdown()`。

**本地复现 / 验证方法**（不需要 Keil，`ORBMDK_BlockTransferTest.exe` 内部就会
`FreeLibrary` + `LoadLibrary` 重载 DLL，正好覆盖这个场景）：

```
修复前：Open#1 → V2 OK
        Open#2（重载后）→ reject: CreateFile failed, err=5 → 退回 V1   ✗
修复后：Open#1 → V2 OK
        Shutdown: mode=2, initialized=1, winusbHandle=1, deviceHandle=1
        Open#2（重载后）→ opened V2 iface 5 → V2 OK                      ✓
        Open#2 → V2 OK
```

> **通用教训**：插件型 DLL 里任何**进程级资源**（文件句柄、USB 句柄、内核对象）都必须在
> `DllMain(DLL_PROCESS_DETACH)` 里释放。宿主可以随时卸载再重载你，
> 而操作系统**不会**代你回收这些句柄 —— 泄漏后表现为"第一次正常、之后永远失败"，
> 且现象会被误判成"设备被独占"。
>
> 另一个排查要点：日志里出现 `GetMode()`/`current=0` 这类**全局状态归零**的迹象时，
> 优先怀疑"模块被卸载重载"，而不是逻辑写错。

### 13.7 更正：orbtrace **支持** `ID_DAP_TRANSFER_BLOCK`；端点包长取自描述符

> 日期：2026-09-30。本节**推翻 §9.9 的结论**。

**（1）§9.9 的"orbtrace 未实现 ID_DAP_TRANSFER_BLOCK"是错的**

实测（V2 通道，BulkTrace 原始字节日志）：

```
DAPCommand: cmd=0x06 outLen=64 -> resp 8 bytes [06 01 00 01]
Block transfer probe OK (ID_DAP_TRANSFER_BLOCK supported, idcode=0x2BA01477)
```

`06`=命令回显、`01 00`=Transfer Count(1)、`01`=ACK OK，后 4 字节就是 IDCODE —— 完全正常。

**为什么会得到错误结论**：探测写在 `DAP_RegWriteRepeat` 的"首次调用"里，
而那个入口前面有 `GetRegOffset(regId) < 0 → return`。测试/调用方只要传了非法 regID，
函数就直接返回，**探测从未执行**，"不支持"只是某次失败留下的陈旧产物。

修正：把探测移到 `DetectTargetDapIdList()` —— **只有这里能确定目标已连接**，
每次连上目标都探测一次并留证（`EnsureBlockTransferProbed`）。

**（2）端点包长必须从配置描述符解析，而且类型不能截断**

`WinUsb_QueryPipe` / 配置描述符给出的 `wMaxPacketSize` 是 **USHORT**。原代码用
`UCHAR` 存放/转换 —— orbtrace 声明的是 **512**，`512 & 0xFF == 0`，
于是包长"变成 0"，后面所有基于它的判断全被带偏。

实测描述符（新增的逐条 dump）：

```
desc iface 5 alt=0 class=0xFF eps=2  <- target
desc   ep 0x03 attr=0x02 type=Bulk wMaxPacketSize=512
desc   ep 0x85 attr=0x02 type=Bulk wMaxPacketSize=512
```

**（3）但"描述符值"不等于"能用的值" —— 需要自标定**

| 出包长度 | 结果 |
|----------|------|
| 512（描述符值，端口 `DEVICE_SPEED=3` High Speed） | 写入成功，设备**一律不响应**（读全超时） |
| **64** | **一问一答正常** ✅ |

orbtrace 固件只读固定 64 字节命令缓冲，跟描述符声明不一致 —— 这类差异没有
单一规则能覆盖（有的固件反而是"必须按端点包长发"）。

于是新增 **开机自标定** `_calibrateOutPacket()`：打开设备后用一次只读探测
（`DAP_Info` vendor 项）实测能通的长度，结果记在 `g_winusb.alignedOutPkt`。

- **先试 64**，再试描述符值。顺序不能反：先发一个过大的包会把设备的输入流带偏
  （它按 64 收下后再去凑剩余字节），**连后面的 64 也一起弄坏**（实测两次都无响应）。
- 每次换包重试之间 `_flushAndDrainPipes()` 清一次端点。
- 特殊设备仍可用 `ORBMDK_BULK_PAD=<n>` 直接指定（跳过标定）。

> ⚠️ 本节策略已被 §17.2 取代：现在是 ① 用 64 短包问 `DAP_Info(0xFF)` 拿**设备自报包长**；
> ② 用 `DAP_Info(0xF0)`（期望 `[00 01 xx]`）**验证**；③ 不合法才换候选（含 64）。
> **描述符值当整包永不使用** —— 实测按 512 发时"第一条能答、之后永久无响应"。
> `ORBMDK_BULK_PAD` 开关已删除（§17.4：不留隐藏开关）。

标定后的日志：

```
calibrate: outPacket=64 OK (descriptor=512, speed=3)
```

---

## 十四、V1 / V2 双接口可显示、可切换（AGDI 侧语义 + 实现）

> 日期：2026-09-30。需求："Keil 里 V1、V2 都要显示出来，并且可以切换"。

### 14.1 AGDI 到底怎么处理接口列表（反汇编 `CMSIS_AGDI.dll`）

对话框的枚举循环在 **`0x1002203C`**（`tools/pe_re.py dis` 可复现）：

```asm
0x10022051  lea  ecx, [ebp-0x21C]        ; &numOfIFs
0x10022057  push ecx
0x10022058  push eax                    ; handle
0x10022059  call [0x10362F64]           ; CMSIS_DAP_Detect(h, &numOfIFs)

0x1002206A  xor  esi, esi               ; ifNo = 0
0x1002206C  cmp  [ebp-0x21C], esi
0x10022072  jle  0x100222C7             ; 没有接口 -> 空列表
0x10022084  cmp  esi, 0x10              ; 最多枚举 16 个
0x10022087  jge  0x100222C7
loop:
  push 0x104  / push buf / push 2 / push esi / push handle
  call [0x10362F6C]                     ; CMSIS_DAP_Identify(h, ifNo, idNo=2, buf, 0x104) 产品名
  mov  [eax + 0x102F9204], esi          ; ★ 把 ifNo 存进列表项
  push 3 ... call [0x10362F6C]          ; idNo=3 -> 序列号
  ...(拷贝名字/序列号进 entry[0x148*n + ...])
```

用户选中某项后，初始化序列（`0x10022AA8`）把**该项的 ifNo** 原样传回来：

```asm
0x10022A9A  push cfg
0x10022AA1  push [edi + 0x102F9204]     ; ★ 选中项的 ifNo
0x10022AA7  push handle
0x10022AA8  call [0x10362F74]           ; CMSIS_DAP_ConfigureInterface(h, ifNo, cfg)
0x10022AAE  ...
0x10022ABF  push 0 / push handle / call [0x10362F1C]   ; DAP_Configure(h, NULL)
0x10022AF5  push [edi+0x102F9204] / push handle / call CMSIS_DAP_Connect
```

**结论**：AGDI 本来就是**多接口**的 ——
`Detect` 报几项就列几项、逐项用 `ifNo` 取名字、选中项的 `ifNo` 再传回 `ConfigureInterface`。
本层只要如实暴露两个接口即可，无需任何 AGDI 侧配合。

### 14.2 实现

| 接口序号 | 传输 | `CMSIS_DAP_Identify(idNo=2)` 返回 |
|----------|------|-----------------------------------|
| `ifNo=0` | CMSIS-DAP v2 (USB Bulk) | `CMSIS-DAP v2`（Bulk 接口字符串描述符） |
| `ifNo=1` | CMSIS-DAP v1 (HID) | `CMSIS-DAP v1` |

| 函数 | 改动 |
|------|------|
| `CMSIS_DAP_Detect` | 由 `1` 改为 `kTransportInterfaceCount = 2`（**绝不返回 0**，否则 AGDI 直接 EU02） |
| `CMSIS_DAP_Identify` | `idNo=2` 按 `ifNo` 返回不同名字（`ORBMDK_USB_Bulk_GetInterfaceName`）；序列号两个接口相同（同一物理设备） |
| `CMSIS_DAP_ConfigureInterface` | 入口按 `ifNo` 调 `ORBMDK_USB_Bulk_SelectInterface(ifNo)`，**现场切换传输层**（必须发生在 `DAP_Configure`/`Connect` 之前） |

`SelectInterface` 把选择写进 `g_forcedPref`，其优先级**高于** `%TEMP%\ORBMDK_TRANSPORT`
与环境变量 —— 用户在对话框里的显式选择应当压过配置文件。

### 14.3 实测

```
CMSIS_DAP_Detect -> numOfIFs=2
  Identify(ifNo=0, idNo=2) -> 'CMSIS-DAP v2'
  Identify(ifNo=1, idNo=2) -> 'CMSIS-DAP v1'
ConfigureInterface(ifNo=1) -> SelectInterface: ifNo=1 -> hid  (Init: switching transport)
                             DAP_ReadReg(DPIDR) -> 0x2BA01477   ✓
ConfigureInterface(ifNo=0) -> SelectInterface: ifNo=0 -> bulk (Init: switching transport)
                             DAP_ReadReg(DPIDR) -> 0x2BA01477   ✓
```

即 Keil 的适配器列表现在会出现 **两条**：`CMSIS-DAP v2` / `CMSIS-DAP v1`，
选中哪条就走哪条，且选择会被 UV4 保存在工程里、跨会话生效。

---

## 十五、⭐ 选中 `CMSIS-DAP v1` 就崩溃的根因：AGDI 把"接口索引"当指针传

> 日期：2026-09-30。§14 做完双接口后，Keil 一选 `CMSIS-DAP v1` 就当场消失。

### 15.1 现象

`%TEMP%\ORBMDK_RDDI.log` **恰好停在 `CMSIS_DAP_Connect` 里**：

```
19:44:54.000 CMSIS_DAP_Connect: configured SWD
19:44:54.000 CMSIS_DAP_Connect: configured transfer (wait=100, match=10)
             <- 日志到此为止，"connection complete" 再也没有出现
```

日志是**每行 fopen/fclose** 写的（必然落盘），所以最后一行就是真实的最后一步。

而 Windows 事件日志里**没有 UV4.exe 的 Application Error 记录** —— 主机把异常吞掉后直接结束了进程。

### 15.2 定位

`CMSIS_DAP_Connect` 中该语句之后只剩三条，唯一可能出错的是写"出参"：

```cpp
if (connectedInterface) {
    *connectedInterface = mode;      // ← 写 AGDI 传进来的指针
}
ctx->isConnected = true;
LOG_DEBUG("... connection complete ...");
```

**本地最小复现**（`test/orbprobe.cpp`：把第 2 个实参照 AGDI 的行为传成 `(int*)ifNo`）：

| 传入的实参 | 结果 |
|------------|------|
| `&iface`（合法指针) | 正常，`connection complete` 打印 ✅ |
| `(int *)1`（= AGDI 在 ifNo=1 时的行为） | `exit=0xC0000005`，日志尾巴与 Keil **逐字一致** ❌ |

反汇编印证（`0x10022AA1` 与 `0x10022AF5` 两处 push **同一个地址**）：

```asm
; ConfigureInterface(handle, ifNo, cfg)
0x10022AA1  push dword ptr [edi + 0x102F9204]   ; arg2 = 选中项索引
; CMSIS_DAP_Connect(handle, connectedInterface)
0x10022AF5  push dword ptr [edi + 0x102F9204]   ; arg2 = 同一个值！
```

**即 AGDI 把"选中项索引"当成了 `int *connectedInterface` 传进来。**

### 15.3 为什么几十年都没暴露

| 选中项索引 | 实参 | `if (connectedInterface)` | 结果 |
|-----------|------|---------------------------|------|
| **0**（此前列表里只有一项，恒为 0） | `NULL` | 判空跳过，**从不写入** | 一直正常 |
| **1**（§14 暴露两个接口后才可能出现） | `(int*)1` | 成立 → 向地址 `1` 写入 | `0xc0000005` |

也就是说：这个坑一直存在，只是**以前只有一条可选适配器，索引恒为 0**，恰好绕过了写入。

### 15.4 修复

写入前先校验指针是否可写（这是唯一一处宿主传入且需要写入的指针）：

```cpp
if (IsWritablePointer(connectedInterface)) {
    *connectedInterface = mode;      // 1=SWD, 2=JTAG
} else if (connectedInterface) {
    LOG_WARN("CMSIS_DAP_Connect: out-pointer %p is not writable "
             "(AGDI passes the selected interface index here) -> write skipped",
             (void *)connectedInterface);
}
```

`IsWritablePointer()` 用 `VirtualQuery` 检查 `MEM_COMMIT` + 可写保护属性 + 区域足够放下一个 `int`。

**验证**（同一份回归用例，修复前必崩）：

```
exit=0
  CMSIS_DAP_Connect(handle, (int*)1) [mimics AGDI] -> 0  (survived the bogus pointer)
  DAP_ReadReg(DPIDR) -> 0, 0x2BA01477
日志: WARN  out-pointer 00000001 is not writable ... -> write skipped
      DEBUG CMSIS_DAP_Connect: connection complete, interface=SWD
```

> **教训**：宿主传进来的"出参指针"不能无条件信任 —— 尤其是那种
> "索引 0 = NULL 所以一直没出事"的值。一旦语义扩展（这里从 1 个接口变成 2 个），
> 边界值立刻变成非法指针。凡是跨模块写入**外部**提供的内存，先校验再写。
> 定位手法仍然是"最小独立复现"：`test/orbprobe.cpp` 一个 `(int*)1` 就把线上现象
> 逐字复刻到了本地。

---

## 十六、双接口（V1/V2 分开显示）下 `RDDI-DAP Error` 的补齐

> 日期：2026-09-30。现象：µVision 的 "CMSIS-DAP Cortex-M Target Driver Setup" 里，
> 适配器下拉框有两条接口（`CMSIS-DAP v2` / `CMSIS-DAP v1`），但 SW Device 列表只有
> `SWDIO / RDDI-DAP Error`，ID CODE / Device Name 为空；Firmware Version 显示 `2.1.0`。
> 需求：**两个接口必须继续分开**，不得合并回单接口。

### 16.1 定位链（工作区内可复现的证据）

AGDI 有两条与"设备数量"有关的判断，触发条件都不在我们手里：

| 判断 | 触发 | 证据 |
|------|------|------|
| "多设备"（一个接口 = 一台调试器） | `CMSIS_DAP_Detect` 返回 > 1 | §14.1（反汇编 `0x1002203C`） |
| "多 DAP"（RDDI v2 能力） | `CMSIS_DAP_Identify(idNo=4)` 取主版本号 ≥ 2 | `test/orbprobe.cpp:97-99` |

而"多 DAP"分支里有一句裸名 `LoadLibraryA("CMSIS_DAP.dll")`，失败即 `0x2029`，
µVision 把它显示成 **`RDDI-DAP Error`**（`test/orbprobe.cpp:41-59`）。

版本串的来源是本轮最关键的一处不一致：

```223:223:src/ORBMDK_HID.cpp
    strcpy_s(g_firmwareVersion, "1.0.0");
```

只有 HID 通道才给 `ctx->firmwareVersion` 赋值，且是主版本 1；而 auto 现在默认优先 V2
（§13.3），V2 下这段被跳过 → 字段为空 → `CMSIS_DAP_Identify(idNo=4)` 落到回退分支
现问设备，把**设备自己的** DAP_Info 固件串 `2.1.0` 透给 AGDI（主版本 2）。
这解释了"**之前 V1 一直是好的**"：V1 路径恰好把版本钉在 `1.0.0`，从来不会进那条分支。

叠加 `CMSIS_DAP_Detect` 恒为 2（§14），AGDI 眼里"两台设备 + 驱动自报支持多 DAP"，
但本层的设备表/DAP 表始终只有 1 项 —— 三条口径互不自洽。

### 16.2 本轮修复（4 项）

| # | 位置 | 问题 | 修复 |
|---|------|------|------|
| 48 | `DAP_GetDAPIDList` | `sizeOfArray < sizeof(int) → RDDI_BADARG`：官方按**字节数**解释（§10.3(6) 的 `shr esi,2`），但 AGDI 在"多 DAP"分支里传的是**元素个数 1** → 直接返回 `RDDI_BADARG`，而且**静默**（不发任何 USB 命令、不写输出参数）。现场表现就是"`CMSIS_DAP_Connect` 成功之后立刻 Disconnect/Close，对话框显示 RDDI-DAP Error" | 两种解释都接受：`≥ sizeof(int)` 按字节、`1..sizeof(int)-1` 按元素个数；两者都不越界。内容仍是 DAP **索引** `0`（会被当作 CMSIS-DAP 的 DAP Index 字节使用，不能写 IDCODE）。日志补 `sizeOfArray → maxEntries` |
| 49 | `CMSIS_DAP_JTAG_GetIDCODEs` | 只认固件的 `ID_DAP_JTAG_IDCODE` 命令；**SWD** 目标上该命令必然失败 → 即使 IDCODE 读得出来，这里也给不出任何 IDCODE。单设备路径下 AGDI 只取 `*count`（还能容忍 0），"多 DAP"分支要的是 idcodes **列表** → 空 = 失败 | 改为优先用与 `GetDeviceIDList` / `DetectDAPIDList` / `DetectNumberOfDAPs` **同一张表**（`ctx->dapIdList`，即协议无关的 DAP 扫描）填充 `idcodes` + `*count`；表为空时才回退 JTAG 命令。保持"扫不到 = 成功 + count 0"（§4.14）。`idcodes == NULL` 时只写 `*count` |
| 50 | `RDDI_Open`（V2 分支） | V2 下 `firmwareVersion` 为空 → `Identify(idNo=4)` 每次现问设备，两次调用可能给出不同的串（AGDI 拿它做能力判定） | V2 打开后补一次 `DAP_GetInfo(DAP_INFO_FIRMWARE)` 并**缓存进 ctx**，此后 `Identify` 只回缓存值 |
| 51 | `ORBMDK_USB_Bulk.cpp` | `_findAndOpenDevice` 里 `(void)serial;`：序列号筛选**从未实现**。同机插多台时只能取枚举到的第一台，插拔顺序一变就换机，界面也看不出来 | 新增 `%TEMP%\ORBMDK_SERIAL` / 环境变量 `ORBMDK_SERIAL`（与传输开关同风格，文件优先）；`_findAndOpenDeviceInGuid` 打开候选后读 iSerialNumber 描述符比对（设备路径里是位置型实例 ID，取不到序列号）；`RDDI_Open` 把该值传下去，V2/HID 两条路径共用。⚠️ **2026-09-30 该开关已移除**（§17.4 的原则：不留任何隐藏开关）；AGDI 的 `pDetails` 恒为 NULL，本层按枚举顺序取第一台 |

### 16.3 明确不做（本轮结论）

- **不合并双接口**：`CMSIS_DAP_Detect` 保持 2，`Identify` 继续按 `ifNo` 分名，
  `ConfigureInterface(ifNo)` 继续负责切传输层（§14 的契约不变）。
- **不改版本串的取值** —— ⚠️ **本条已被 §17.5 的对照实验推翻**。当时的推断是
  "官方 RDDI 层同样把设备固件串透给 AGDI，所以只能逐项对齐那条分支里的接口"。
  实验证明：那条分支在本环境下**根本走不到**那些接口（AGDI 连设备枚举都不做，
  Connect 之后立即 Disconnect/Close；而 RDDI 层看到的 DPIDR/Connect 全是成功的）。
  现已改为由本层自持版本串 `kDriverFirmwareVersion = "1.0.0"`，理由与数据见 §17.5。

### 16.4 待实测

> ✅ 三项均已于 2026-09-30 闭环（见 §17）：① 两条接口 + SW Device 正常（§17.5 B 臂）；
> ② 日志核对完成；③ 序列号开关已删除（§17.4）。

1. ✅ 结束整个 `UV4.exe` → `.\deploy.ps1` → 重开 Keil → Debug Settings；
   SW Device 显示 `IDCODE 0x2BA01477 / ARM CoreSight SW-DP`，下拉框两条（§17.5 B 臂）。
2. ✅ `%TEMP%\ORBMDK_RDDI.log` 核对完成：`V2 Bulk mode OK`、`outPacket=508 verified`、
   `GetDeviceIDList: id[0]=0x2BA01477`、`ConfigureDebugger` 全在。
3. ~~多调试器：`echo 29B4E080E2289E06 > %TEMP%\ORBMDK_SERIAL` 指定具体设备~~
   —— 该开关已于 2026-09-30 移除（见 §17.4）。AGDI 的 `pDetails` 恒为 NULL，
   本层按枚举顺序取第一台（**多台同时插入时的选择问题仍未解决**，见 §18.1）。

---

## 十七、⭐ V2 打不开的两条真正根因（2026-09-30 定位并修复）

> 现场：`SelectInterface: ifNo=0 -> bulk` 之后总是
> `reject: CreateFile failed, err=5`，整场会话退化成 V1；
> 一旦 V2 真打开，又是 `write 成功 / 读永远超时`。
> 两条根因都不在 Keil、不在设备，而在本层。

### 17.1 根因一：WinUSB 句柄泄漏 → v2 接口被**永久**独占

| 探测（同一设备、同一驱动） | 结果 | 说明 |
|---------------------------|------|------|
| `mi_02/03/06/07` 的 WINUSB 接口，**非提权**进程 CreateFile | 全部成功 | 排除 ACL / 驱动 / 设备问题 |
| `mi_05`（CMSIS-DAP v2 的 Bulk 接口） | **err=5（拒绝访问）** | WinUSB 独占 → 有人仍持有句柄 |
| 全机进程列表 | 只有 `UV4.exe` | 持有者只能是 UV4 进程里的本层 DLL |

机理：WinUSB 是独占设备。关闭时若还有挂起的 overlapped IRP 没被取消并收尾，
内核会一直持有 file object 引用 → 接口不释放 → **此后任何进程**（含 Keil 自己）
CreateFile 都是 err=5，且不会自己恢复（必须结束持有它的进程）。
这解释了"**V2 明明以前能用，后来整场会话再也打不开**"。

修复（`ORBMDK_USB_Bulk.cpp`）：
- `_closeWinUSB()`：`CancelIoEx(句柄, NULL)` → `WinUsb_AbortPipe/ResetPipe(IN/OUT)`
  （同步等 IRP 真正结束）→ **才** `WinUsb_Free` + `CloseHandle`。
- `_cancelOverlapped()`：`CancelIoEx` 不回来就用 `WinUsb_AbortPipe` 强制收尾；
  仍不回来则**故意弃置该 OVERLAPPED**（绝不能释放驱动仍持有的指针）。

### 17.2 根因二：出包长度取了端点 wMaxPacketSize → 固件认为"传输未结束"

日志关键事实：`bulkWrite OUT ep=0x03 len=512 -> 512 bytes, 0 ms`，然后**永远读不到响应**。
用 `test/v2padprobe.cpp`（直连 WinUSB，**每个候选都重开设备**，避免残留字节造成假阳性）实测：

| OUT 包长 | 结果 |
|----------|------|
| 2（按命令精确长度） | 无响应 |
| **64** | 一问一答正常（仅"打开后第一条"会被吞，重发即可） |
| **508** | **完全正常，第一条也答** ✓ |
| 512（= 描述符 `wMaxPacketSize`） | **只有第一条能答，之后永久死** ✗ |

机理：USB 上**长度恰等于 wMaxPacketSize 的包不是短包**，TinyUSB 一类固件会认为
这次传输还没结束、继续等下一包 → 命令永远不被派发。设备自己用
`DAP_Info(0xFF)` 报了 **508 = 512 − 4**，正是让我们避开这个陷阱。

修复（`ORBMDK_USB_Bulk.cpp`）：
- `_calibrateOutPacket()` 重写：① 用 64 的短包做引导，问设备 `DAP_Info(0xFF)`
  拿**自报包长**；② 再用可校验的只读命令 `DAP_Info(0xF0)`（期望 `[00 01 xx]`）
  **验证**该长度；③ 不合法就换候选（含 64），候选之间 `_flushAndDrainPipes()`；
  ④ `_bulkPacketSize()` 无标定结果时用**描述符值减 1**，绝不发整包。

### 17.3 验证（命令行完成，不依赖 Keil）

`test/orbprobe3.exe`（强制 `ConfigureInterface(ifNo=0)` → 只许 V2）：

```
transport = CMSIS-DAP v2 (USB Bulk)   idcode=0x2BA01477
[1] ROM table base = 0xE00FF003      [2] AP CSW R/W = 0x23000052
[3] SCB->CPUID = 0x410FC241          [4] DHCSR -> S_HALT set
[5] PC 读出（S_REGRDY=1）            [6] RAM 0x20000000 写读 0xA5A55A5A [OK]
[7] resume -> running                （全程 ep=0x03/0x85，len=508）
```

### 17.4 传输选择：去掉一切"开关"

- 删除 `%TEMP%\ORBMDK_TRANSPORT` 文件与环境变量；`_transportPreference()` 只认
  AGDI 在 `CMSIS_DAP_ConfigureInterface(ifNo)` 给出的选择；未选定期间为 AUTO
  （V2 优先、不可用才 V1）。
- 选中的通道打不开时**如实失败**（`CMSIS_DAP_ConfigureInterface` 返回 `RDDI_FAILED`），
  **不再静默降级**到另一条 —— 静默降级正是把 17.1 的 err=5 掩盖了那么久的元凶。

### 17.5 结论：`Identify(idNo=4)` 的版本串**就是** AGDI"多 DAP 分支"的开关

2026-09-30 做了单变量对照实验：同一台设备、同一份 DLL、同一台 Keil，
**唯一变化**是上报的固件版本串（用临时诊断入口切换，实验后已删除该入口）。

| 臂 | 上报串 | 传输 | 结果 | AGDI 实际调用序列（日志） |
|----|--------|------|------|--------------------------|
| A | `2.1.0`（设备自报，主版本 2） | V2 | **失败**（RDDI-DAP Error） | Open → Detect(2) → Identify → ConfigureInterface → Connect（DPIDR 读到了 0x2BA01477）→ 再读 3 条寄存器 → **Disconnect/Close**。**没有** `GetNumberOfDevices` / `GetDeviceIDList` / `ConfigureDebugger` |
| A' | `2.1.0` | HID（另一条接口） | **失败**，同上 | 同上 |
| B | `1.0.0`（主版本 1） | **V2** | **成功**，完整调试下载 | Open → Detect → Identify → ConfigureInterface → Connect → `DetectTargetDapIdList`(0x2BA01477) → `Block transfer probe OK` → `GetNumberOfDevices: count=1` → `GetDeviceIDList: id[0]=0x2BA01477` → `ConfigureDebugger` → …整场调试… |

要点：
1. A 臂里 **RDDI 层完全正常**（V2 通、IDCODE 正确、Connect 成功），错只出在 AGDI
   那条分支上 —— 它连"设备列表枚举"都不做，连上就放弃。
2. 与传输层无关：A 在 V2 与 HID 上都失败，B 在 V2 上成功 → 唯一变量就是这个版本串。
3. 因此**保留钉值**：`kDriverFirmwareVersion = "1.0.0"`，设备自报串只写日志
   （`RDDI_Open: device DAP_Info firmware = '...' (log only, not exposed)`）。
   本节结论**取代 §16.3 中"不改版本串的取值"那一条**（当时的推断被本次实验推翻：
   那条分支在本环境下靠"逐项对齐接口"过不去，AGDI 根本不会走到那些接口）。
4. 临时诊断入口 `ORBMDK_FWVER` 已按约定删除，正式代码里不留开关。

> 附注：若将来希望对话框里显示设备真实版本，可考虑"只把主版本归一化为 1"
> （把 `2.1.0` 上报成 `1.1.0`），语义上仍能过闸；当前实现选择最保守的固定串。

### 17.6 AGDI 侧"版本号判断"的反汇编证据（CMSIS_AGDI.dll v1.33.24.0）

问："是不是 AGDI 层对版本号有判断？" —— **有，而且是显式的**。用新增的
`tools/agdi_ver.py`（按"适配器条目字段基址"的 disp32 字节模式在整个 `.text` 里搜，
再找指向目标地址的 `call rel32`）定位到下面这条链。

**适配器条目布局**（步长 `0x148`，数组基址 `0x102F9044` 附近）：

| 条目偏移 | 绝对地址 | 内容 | 写入者 |
|----------|----------|------|--------|
| `+0x07C` | `0x102F90C0` | 名字（0x104 字节） | `Identify(ifNo, 2)` @`0x100220AA` |
| `+0x17D` | `0x102F91C1` | 序列号（0x20 字节） | `Identify(ifNo, 3)` @`0x1002210A` |
| **`+0x19E`** | **`0x102F91E2`** | **固件版本（0x20 字节）** | **`Identify(ifNo, 4)` @`0x100225D9`** |
| `+0x1C0` | `0x102F9204` | ifNo（用户选的那条） | `Detect` 循环 @`0x100220CD` |

**取主版本号的函数 `0x10021FB0`**（返回 `%lu` 第一个值）：

```asm
0x10021FB0  push ebp / mov ebp,esp / sub esp,0xC
0x10021FB6  cmp  dword ptr [0x10362714], 0     ; 适配器条目数
0x10021FBD  mov  dword ptr [ebp-4], 0          ; major = 0
0x10021FE0  imul eax, dword ptr [0x102f90b8], 0x148
0x10021FEA  push 0x101EB37C                    ; ★ 格式串
0x10021FEF  add  eax, 0x102F91E2               ; 条目的"固件版本"字段
0x10021FF4  push eax
0x10021FF5  call 0x100237A0                    ; sscanf(buf,"%lu.%lu.%lu",&major,&minor,&patch)
0x10021FFF  jle  0x10022008                    ; 解析失败 → 返回 0
0x10022001  mov  eax, dword ptr [ebp-4]        ; ★ 返回 major
```
其中 `0x101EB37C` 处的字符串实测为 **`"%lu.%lu.%lu"`**（紧邻的 `0x101EB384` 是 `"%lu"`）。

**调用点一 `0x10022870`**（会话/DLL 加载路径）—— 判定之后就**换掉整张函数指针表**：

```asm
0x10022858  call ecx                            ; CMSIS_DAP_Capabilities(handle, ifNo, caps@0x102F8B90)
0x10022870  call 0x10021FB0                     ; major
0x10022875  cmp  eax, 2
0x10022878  jb   0x10022897                     ; major < 2 → 原样（单 DAP 路径）
0x1002287A  test byte ptr [0x102F8B90], 0x40    ; caps 位
0x10022890  mov  byte ptr [0x10304B79], al      ; ★ 置"新模式"标志
0x1002289C  mov  dword ptr [0x102375F4], 0x1003D030   ; ★ 换函数指针表 1
0x100228A6  mov  dword ptr [0x10237994], 0x1003D0F0   ; ★ 换函数指针表 2
```

**调用点二 `0x1003CB92`** —— 同一个判定，同样在 `major >= 2` 时改装另一张表：

```asm
0x1003CB92  call 0x10021FB0
0x1003CB97  cmp  eax, 2
0x1003CB9A  jb   0x1003CEF1
0x1003CBA0  cmp  dword ptr [0x10363010], 1
0x1003CBA7  je   0x1003CEF1
0x1003CBB2  mov  dword ptr [0x10363020], 0x10363120   ; ★ 又一张表
```

**结论**：AGDI 把 `Identify(idNo=4)` 的返回值当版本号 `sscanf`，用 **`cmp eax,2`** 做门槛；
主版本 ≥ 2 时切到它自己的另一套驱动逻辑（换函数指针表 + 置标志位）。本环境下那套
逻辑走不通（A 臂：跳过全部设备枚举、`Connect` 后立刻 Close），所以这一栏必须钉在
`1.0.0`。这也正是 §17.5"只改这一个字符串、行为就翻转"的代码级解释。

---

## 十八、剩余工作清单（2026-09-30 复核）

> 从全文"待实测 / 待定 / 尚未落地"里逐条筛出，按"是否影响当前 Keil 调试链路"排序。
> ✅ 已完成 · 🔴 待做（有价值）· 🟡 可选 · ⛔ 不需要

### 18.1 🔴 多台调试器同时插入时**无法指定用哪一台**

**现状**：`CMSIS_DAP_Detect` 固定返回 2（= 两种传输），`Identify` 只报名字/序列号，
`ConfigureInterface(ifNo)` 仅用于切传输层。AGDI 的 `rddi_Open(pDetails)` 恒为 NULL
（§2.1 实测），**它没有任何"用哪台设备"的信息可传**。两台 orbtrace 同时插入时本层
只能取枚举顺序第一台 —— 界面上两条目完全相同，用户无法选择（原 `ORBMDK_SERIAL`
开关已按"不留隐藏开关"原则删除，§17.4）。

**可行方案**（改动集中在 `CMSIS_DAP_Detect/Identify/ConfigureInterface` + 设备枚举）：

1. 枚举所有同型号调试器（各带序列号）；
2. `CMSIS_DAP_Detect` 返回 `N × 2`：每台设备两条（`CMSIS-DAP v2 (SN)` /
   `CMSIS-DAP v1 (SN)`）；`Identify(idNo=2/3)` 按 `ifNo` 给出带序列号的名字；
3. `ConfigureInterface(ifNo)` 反解出"第几台设备 + 哪种传输"，切到那台并记住；
4. 已连接同一台时复用，避免反复 Open/Close。

风险点：AGDI 遍历适配器条目的上限（`0x10022084 cmp esi,0x10` → 16 项），
以及**条目顺序必须稳定**（否则工程里保存的选择会串位）。

### 18.2 ✅ 块传输提速验证 —— 2026-09-30 完成

`bin\ORBMDK_BlockTransferTest.exe 0x20000000`：**12/12 通过、哨兵零损坏、加速比 7.86×**
（§9.8 有完整数据）。过程中修好了验证程序本身缺的四步（低层 `DAP_Connect` 是纯桩、
调用顺序、DP/AP 上电、目标运行时要先停核）。

> 遗留观察（非阻塞）：测试里的 `DHCSR` 回读为 `0x00000000`（正常应有 bit0/1 置位），
> 说明那次"停核"很可能没真正生效 —— 这次数据能对上是目标已处于停机态。
> 下次跑之前建议先确认目标处于 halt 状态（或把停核步骤改成经 `CMSIS_DAP_Target`）。

### 18.3 ✅ 日志模块统一（§8）—— 2026-09-30 已完成

单实现 `src/ORBMDK_Log.cpp` + 头文件宏；两套旧实现、四处"直写文件"全部并入；
`RDDI_SetLogCallback` 通道接通；HID/BULK 命令级日志补上时间戳。落地记录见 **§8.5**。

仍待办的小项：**会话分隔标记** —— ✅ 同日一并完成：`RDDI_Open` 入口打一条
INFO 级分隔行（`================ 会话 #N 开始 (pid=...) ================`），
便于在"跨进程/跨会话追加"的日志里快速定位（§4.10 的遗留项）。

### 18.4 🟡 状态 LED 实机确认（§11.2）

`SetHostLed` / `TrackApTarWrite` 已实现但**未在实机核对**：接上目标后 Connect LED 常亮，
烧录/运行时 Running LED 亮、停机灭。

### 18.5 🟡 简化实现（§5.1~§5.3，均不影响 Keil 链路）

`DAP_DefineSequence` / `DAP_RunSequence`、`CMSIS_DAP_Commands`（除 `RESET_TARGET`）、
`CMSIS_DAP_JTAG_GetIRLengths`、`CMSIS_DAP_Atomic_Result`、`CMSIS_DAP_GetGUID`（拼串非真
GUID）、`DAP_Target` 的 Halt/Resume/Step、Trace 适配层 3 个空函数、ETM v4 的
`FuncReturn`/`ExceptionReturn`/`Prefix`。只有真要跑 trace / sequencer 时才需要补。

### 18.6 🟡 `Firmware Version` 一栏的显示（§17.5 附注）

现固定 `1.0.0`（设备真实值 `2.1.0` 会触发 AGDI 的另一套逻辑）。若想让界面贴近真实值，
可改成"只把主版本归一化为 1"（`2.1.0` → `1.1.0`）；纯显示偏好，改完需一次 A/B 复验。

### 18.7 ⛔ 不需要做

- ULINK+ 专有接口（`ULINKPLUS_*`）：Keil 独有，ORBTrace 用不到。
- §7.3 符号搜索路径：属设计内 fallback，保持现状。

### 18.9 ✅⭐ JTAG 通路（2026-09-30 定位并打通 —— 结论见**第九步**）

**现象**（起因）：Debug 设置里把 Port 由 SW 改成 **JTAG** 后，Keil 报
`Cannot enter Debug Mode`；当时设备对 `cmd=0x02`(Connect) 回的是 **`01` = SWD 模式**
（因为本层过去恒发 `port=1`）。

#### 第一步（已完成）：宿主侧实现

| # | 位置 | 改动 |
|---|------|------|
| 1 | `DAP_ConnectTargetPort(port)` | 新增：`Connect(0/1/2)` 由参数决定（原来恒 `port=1`） |
| 2 | `CMSIS_DAP_Connect` | 按 `ctx->isSWD` 分岔：SWD 走原序列；JTAG 走新的 `JtagInitSequence` |
| 3 | `JtagInitSequence`（新） | `Connect(2)` → **SWD->JTAG 切换序列**（线复位 64×1 + 16 位 `0xE73C`）→ 重选 `Connect(2)` → `JTAG_Configure(1, ir=4)` → TAP 复位（尽力）→ `JTAG_IDCODE` 扫链 → 读 DP IDCODE |
| 4 | `SwdLinkRecover` | JTAG 模式下**不再**走 SWD 恢复序列（那会把链路打回 SWD），改为重做 JTAG 建链 |
| 5 | `CMSIS_DAP_JTAG_Configure` / `GetIRLengths` | 记录 IR 长度/器件数并回给宿主；SWD 模式下报 **count=0** |
| 6 | `INFO_CAPS_JTAG` | **暂不加**——能力位只宣称已验证能力；JTAG 已于第九步实测打通，是否对外宣称待评估（Keil 的 Port 下拉框不受该位限制，照样能选） |

#### 第二步：直连 WinUSB 探测（`test/jtagrawprobe.cpp`，绕开本层）

> 工具名对照见 README"测试工具清单（2026-09-30 整理）"：`jtagprobe2` → `jtagrawprobe`，
> `orbprobe*` → `swdprobe`，`v1probe` → `hidprobe`，`v2padprobe` → `v2rawprobe`。

| 探测 | 固件回应 | 结论 |
|------|---------|------|
| `Info(0xF0)` | `00 01 03` | caps = SWD+JTAG（固件自报支持 JTAG）|
| `Connect(0)` / `Connect(1)` | `02 01` | SWD ✓ |
| **`Connect(2)`** | **`02 02`** | **真的进入 JTAG 模式** ✓（并非强行回 SWD） |
| `JTAG_Configure(1, ir=4)` | `15 00` | 接受 ✓ |
| `JTAG_Configure(ir=5)` / `(count=2, ir=4)` | `15 FF` | 固件只支持**单 TAP / IR=4**（正合 Cortex-M JTAG-DP）|
| **`JTAG_IDCODE`** | `16 00 00 00 00 00` | **count = 0：链上无器件** ✗ |
| `JTAG_Sequence`（穷举 info=0x00~0x03、32 位、并**多段读**） | 只回 `14 00`，**从不回 TDO 数据** | 固件不提供通用位流（见第三步）|
| 补发 SWD->JTAG 切换序列后再扫（两种先后顺序） | 仍 count = 0 | 排除"SWJ-DP 停在 SWD 模式" |

#### 第三步：gateware 源码核实（`orbtrace-1.4.3/verilog/`）

- `jtagIF.v` **是真实实现**：`JTAG_CMD_IR`（写 IR）、`JTAG_CMD_TFR`（35 位 ADIv5 DR 传输）、
  `JTAG_CMD_READID`（读 IDCODE）、`JTAG_CMD_RESET`（TAP 复位）；且 `assign jtag_tdo = tdo_swo;`
  → **TDO 确实被采样** ✓。
- 但它**只实现这四件事**，没有通用 TMS/TDI/TDO 位流 → `DAP_JTAG_Sequence` 只 ACK、不回数据，
  属**设计如此**（不是故障）。因此链检测只能依赖 `DAP_JTAG_IDCODE`。
- `dbgIF.v` 侧命令：`CMD_SET_JTAG_CFG=6`、`CMD_JTAG_GET_ID=13`、`CMD_JTAG_RESET=14`、`CMD_JTAG_REG=15`；
  `assign pinsout = {nReset, nTRST=1, TDO, TDI, SWDIO, SWCLK}` → **TCK/TMS 与 SWCLK/SWDIO 是同一对**，
  JTAG 比 SWD 多需要 **TDI/TDO 两根**。
- 推论：**扫链为空 = TDO 上没有任何器件驱动** → 属物理层，软件侧已无可再试。

#### 第四步：结论与排查方向（硬件侧）

1. **TDI/TDO 接线**（最可能）：SWD 只需 SWCLK/SWDIO，JTAG 还要 TDI 与 TDO。用 4 针 SWD 线、
   或目标板只引出 SWD 接口时，JTAG 必然扫不到（本机现象与之完全一致）。
2. **目标 JTAG 是否使能**：JTAG 引脚被复用成 GPIO，或选项字节配成 "JTAG-DP Disabled"
   （部分 STM32 支持该配置）→ 接线正确也扫不到。
3. **目标状态**：处于复位/掉电时 TDO 无驱动。

#### 第五步：引脚级判定（把"软件侧已穷尽"钉死）

把 `SWJ_Pins`（操作码 **0x10**，注意不是标准里的 0x07——本方言 `0x07=TRANSFER_ABORT`，
用错会得到"恒 `FF`"的假读数）用起来之后：

| 检查 | 结果 | 含义 |
|------|------|------|
| 驱动 nRESET=0 后读回 | `0x6F`（bit7=0）| **引脚回读可信**（拉低即跟随） |
| 驱动 TMS/TDI=1、再=0 | 读回跟随 | **引脚驱动可信** |
| **TDO（bit3）** | **恒为 1**（翻转 TMS/TDI/TCK 均不变）| 目标侧**没有任何器件驱动 TDO** |
| **位拷贝扫 DR**（手工走 TAP：复位→RTI→Select-DR→Capture-DR→Shift-DR→移 32 位）| `0xFFFFFFFF` | **完全绕开 gateware 的 JTAG 引擎**，结论一致 |
| 拉低 nRESET（复位态）后再扫 | 仍 0 | **connect-under-reset 已排除**（复位线有效）|
| SWJ 时钟 100k/500k/1M/4M/10M 逐档 | 全部 count=0 | **不是时钟过快**（orbtrace 官方 JTAG 上限 10-12Mbps）|

两套互不相关的读链实现（gateware 的 `JTAG_CMD_READID` 与宿主手工位拷贝）都读不到器件；
而 **SWD 完全正常**（DPIDR `0x2BA01477`）→ 目标已上电、DP 活着。orbtrace 官方发布说明
亦确认 JTAG 是可用特性（v1.0.0 起）。据此：

> **结论：TDO 上没有信号 = 物理链路问题**，宿主侧变量已全部排除
> （模式切换 / IR 配置 / TAP 复位 / SWD→JTAG 切换 / 复位态 / 时钟 / info 编码 /
> 多段响应 / 独立位拷贝）。

**待用户侧验证（按性价比排序）**：

1. **逐根查线（含"有没有把 TDI/TDO 接反/交叉"）**：原理图正确 ≠ 线接对。JTAG 的 TDI/TDO
   是**直连**（不是 UART 那种交叉）。若交叉：探针 TDO 输入悬空 → 恒读 1，与本机现象完全吻合。
2. **换一块已知 JTAG 可用的目标板**（老 F1/F4 开发板，JTAG 引脚未被复用）→ 二分定位
   "目标板"还是"探针/线缆"。
3. **第三方宿主交叉验证**：用 OpenOCD(+orbtrace) 走 JTAG 试同一目标 —— 若 OpenOCD 也失败，
   即可断定与本层实现无关。

#### 第六步（2026-09-30，用户定位）：**目标 SWJ-DP 的 JTAG→SWD 是单向的**

用户实测确认：**目标一旦执行过一次 JTAG→SWD（即任何 SWD 连接），就再也回不到 JTAG**，
只能给目标**断电重上电**。补发 ADIv5 规定的 SWD→JTAG 序列（线复位 + `0xE73C`）无效 ——
该步骤已做进 `JtagInitSequence`，实测救不回来。

这一条一次性解释了此前所有"TDO 恒 1 / 扫链为空"的观察：**是我们自己先发了 SWD**
（探测程序第 [1] 步的 `Connect(0)`、以及 Keil Debug 设置对话框的设备扫描，发的都是 SWD），
把目标锁进 SWD；此后的 JTAG 尝试自然全为空。

**据此修正两条旧结论**：

1. 第五步"拉低 nRESET 后再扫仍为 0 → connect-under-reset 已排除"**不成立**：当时目标已锁在
   SWD，"复位也没用"只说明**当次实验无效**，需要**重新验证**。若 nRESET 能复位 SWJ-DP 的
   模式选择，驱动就能在 JTAG 建链失败时自动恢复（值得做，也值得写进 §18.9 的处置）。
2. 第五步据此推出的"接线/TDO 无驱动"**同样待重验**：目标被锁时 TDO 本来就无响应。

**正确姿势（本目标走 JTAG 的唯一路径）**：**给目标断电 → 上电 → 第一件事就用 JTAG 连接**
（`Port=JTAG`），中途**绝不能**发生任何 SWD 连接。驱动侧已保证这一点：三处
`DAP_ConnectTarget()`（SWD）全部在 `if (ctx->isSWD)` 分支内，选 JTAG 时本层不碰 SWD。
探测工具对应 `test/jtagrawprobe.cpp --no-swd`（JTAG-only：跳过全部 `Connect(0)`/`Connect(1)`）。

#### 第八步（2026-09-30）：帧格式根因**已定位并实测读通** ⭐

用 **OpenOCD 交叉验证**（`OpenOCD-20260302-0.12.0\test.bat` = `transport select jtag` +
`stm32f4x.cfg`）**一次成功**：

```
Info : CMSIS-DAP: Interface Initialised (JTAG)
Info : cmsis-dap JTAG TLR_RESET
Info : JTAG tap: stm32f4x.cpu tap/device found: 0x4ba00477   (IR=4)
Info : JTAG tap: stm32f4x.bs  tap/device found: 0x06413041   (IR=5)
Info : Cortex-M4 r0p1 processor detected / Examination succeed
```

⇒ **探针/线缆/TDO/目标 JTAG 全部正常**；第六步里"TDO 无驱动""connect-under-reset 无效"
**全部作废**；同时确认链上是 **两个 TAP**（cpu IR=4 + bs IR=5）。

**真正的根因**（在 orbtrace 自带固件源码里找到权威定义）：
`orbtrace-1.4.3/daplink/cmsis-dap/DAP.c` + `DAP.h`：

```c
// 请求：[0x14][count][info][data...]   响应：[0x14][status][TDO...]
sequence_count = *request++;            // 第 1 字节 = 段数（★ 在前！）
sequence_info  = *request++;            // 第 2 字节 = info（★ 在后！）
count = sequence_info & 0x3F;           // 低 6 位 = TCK 位数（0 视为 64）
count = (count + 7) / 8;                // 数据字节数，LSB first
if (sequence_info & 0x80) response += count;   // bit7 = TDO 捕获
// DAP.h: JTAG_SEQUENCE_TCK=0x3F  JTAG_SEQUENCE_TMS=0x40  JTAG_SEQUENCE_TDO=0x80
```

且 `JTAG_Sequence()`（`JTAG_DP.c`）里 **TMS 是整段恒定值** → 状态迁移必须一段一段发
（与 OpenOCD 日志里的 "1 bits, tms HIGH" 完全吻合）。

**修前 vs 修后**（`test/jtagrawprobe.cpp --no-swd --path`）：

| | 帧 | 结果 |
|---|---|---|
| 修前 | `[0x14][info][count]`，info 用 bit0/bit1 | `14 00` —— 无 TDO ✗ |
| **修后** | `[0x14][count=1][info]`，info = 位数｜TMS?0x40｜TDO?0x80 | `14 00 77 04 A0 4B 41 30 41 06` → **TDO=0x4BA00477 + 0x06413041** ✓ |

**待办 → ✅ 2026-09-30 已完成**：本层（`ORBMDK_HID.cpp` 的 `DAP_JTAG_Sequence`）已按上表
改成**正确帧**；由于固件的 `DAP_Transfer`（0x05）在 JTAG 模式下**不响应**（实测超时），JTAG 的
DP/AP 访问由本层用 `JTAG_Sequence` 自己实现（IR=0xA/0xB + 35 位 DR + 3 位 ACK 解析）。详见 **第九步**。

**旧处置 → 已作废**：`Port=JTAG` 时"把 SWJ 时钟限到 4 MHz、失败即打 ERROR"的降级处置已被
第九步的完整实现取代。**现状：SWD / JTAG 两条通路均可正常建链、读寄存器。**

#### 第九步（2026-09-30）：JTAG 全链路打通 ⭐

第八步把**扫链**（IDCODE 路径）改对后，DP/AP 访问仍失败（`TDO` 恒 0、`ACK=0`）。继续排查，
**两项根因均在宿主侧**，已一并修复：

**① IR / DR 移位多推一位**（`JtagSetIr` / `JtagDrScan`）

原实现先移 `total` 拍（TMS 恒 0）再补两位退出（`{2,1}`）——等于**多送一拍**，使 IR 发生旋转、
落入非法指令，DP 退化为 **BYPASS** → `TDO` / `ACK` 恒 0。按 IEEE 1149.1 改为：

- 前 `total-1` 位为一段（TMS=0）；**最后 1 位与 Exit1 同拍发出（TMS=1）**；再 Update + RTI。
- `JtagDrScan` 同样修正最后一拍，并把**分段捕获到的 TDO 按位拼回**连续位流后再解析
  （`DAP_JTAG_Sequence` 的每段响应按**字节**对齐，不能当作连续位流直接用）。

**② 建链判据错用 DPIDR 的值**（`JtagInitSequence`）

原以"DPACC 读 DPIDR(0x0) 的值非 0"判建链成功。实测本目标 STM32 的 ARM JTAG-DP 对
**DPACC 地址 0x0 的读恒返回 0**（同一条 DPACC 通道读 CTRL/STAT(0x4)=`0xF0000000`、经 APACC
读 AP IDR(0xFC)=`0x24770011` 均**正确**）。OpenOCD 的 `dap_dp_init()` **同样从不读 DPIDR**——
它只 poll CTRL/STAT 等 PWRUPACK，再读 AP IDR 识别 MEM-AP。据此改为：

> **以 DPIDR 读的 ACK 状态为通路判据，DP 身份优先取扫链 IDCODE**（`ids[dpIndex]`），
> DPIDR 仅在其可读时覆盖。

**验证**（`bin\jtagprobe.exe`，JTAG 模式）：

| 项 | 结果 |
|----|------|
| `CMSIS_DAP_Connect` | **0（mode=2, JTAG）** ✓ |
| 扫链 / DP 身份 | IDCODE `0x4BA00477`（TAP1 `0x06413041`）✓ |
| DP CTRL/STAT (0x4) | `0xF0000000`（powered up）✓ |
| AP CSW (0x0) 读写 | 写 `0x23000052` 回读一致 ✓ |
| **AP IDR (0xFC)** | **`0x24770011`** ✓（需先写 `DP SELECT=0x000000F0`：`APSEL` 在高 8 位、`APBANKSEL` 在 bits[7:4]）|

**SWD 无回归**（`bin\swdprobe.exe`）：`DPIDR=0x2BA01477`、AP CSW `0x23000052`、
`SCB->CPUID=0x410FC241` 全部正常 —— 这也**反证**"JTAG 下 DPIDR 读 0"是该 **JTAG-DP 的器件行为**
（同一 DLL 走 SWD 时读得完全正确），并非本层缺陷。

**与 OpenOCD 交叉一致**：IDCODE `0x4BA00477`、AP IDR `0x24770011` **完全相同**。

**对前文结论的修正**：第六步"SWJ-DP 只能 JTAG→SWD、必须给目标**断电**才能回到 JTAG"与本次实测
**不符** —— 本次 JTAG 成功前同一上电周期内发生过 SWD，且 `JtagInitSequence` 里的
**SWD→JTAG 切换序列**（线复位 64×1 + 16 位 `0xE73C`）实测**有效**，无需断电。结合第八步可知：
早前"扫链为空 / TDO 恒 1"的真因是**帧格式错 + 上面①的移位错位**，而非 SWJ-DP 锁死。

**改动文件**：`src/ORBMDK_RDDI.cpp`（`JtagSetIr` / `JtagDrScan` / `JtagInitSequence`）；
`test/jtagprobe.cpp`（新增"写 `DP SELECT` 后读 AP IDR"的端到端校验）。

### 18.8 ✅ 2026-09-30 已闭环

V2 真因与修复（§17.1 / §17.2）、四个隐藏开关删除（§17.4）、版本门单变量 A/B +
反汇编证据（§17.5 / §17.6）、§5.4 的"V2 实机验证"、§11.1.5 的"退化到 V1"、
§16.4 的三项待实测、**§18.9 的 JTAG 通路打通（第九步）**。



