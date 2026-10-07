# ORBMDK 兼容性分析与实现状态

> 分析日期：2026-09-29
> 实机联调：2026-09-30（ORBTrace 调试器 + STM32F4 目标）
>
> 本文档只保留**当前可能有效**的结论。已过时或被后续实测推翻的内容（导出符号数量快照、"已实现/缺失函数"
> 逐条清单、编译阻塞排查过程、逐项代码对比、各轮修改文件清单、未落地的方案设计稿，以及已被更正的错误结论
> 与死路排查记录）均已删除或合并，避免与源码现状矛盾。

---

## 一、当前状态总览

| 项 | 状态 |
|----|------|
| 版本 | **`0.5.0`** —— 驱动自身发版号（`include/ORBMDK.h` 的 `ORBMDK_VERSION_STRING`）。⚠️ 与下表"固件版本门"里**上报给宿主的协议版本串**（设备 `DAP_Info(0x04)`，主版本归一化抬到 ≥ 2）不是一回事，**勿联动** |
| 编译 | ✅ `build.ps1` 全量编译通过，**无 error、无 warning** |
| 运行库 | ✅ **静态 CRT（`/MT`）**，产物不依赖 `MSVCP140`/`VCRUNTIME140`/UCRT（原因见第十二节） |
| 编译器依赖 | ⚠️ **与 MSVC / UCRT 强绑定**（Annex K 安全 CRT、`#pragma comment(lib)`）：`clang-cl` 理论可行，MinGW-w64 GCC 与 VS2013 及更早不支持 —— 静态审查见 **§19** |
| 输出 | `bin/ORBMDK_RDDI.dll` |
| 导出符号 | `ORBMDK_RDDI.h` 中带 `RDDI_FUNC` 的导出函数共 **78 个** |
| 实机验证 | ⚠️ `ORBMDK_RDDI_FullTest.exe` 原 **40/40 全部通过**；后因测试自身 `DAP_REG_*` 常量陈旧（§13.5）变为 `PASSED 22 / FAILED 20` → 修复项见 **`Todo.md` §18.11** |
| 目标识别 | ✅ DP IDCODE `0x2BA01477`、AP IDR `0x24770011`（STM32F1） |
| JTAG 通路 | ✅ **已打通**（§18.9 第九步）：`Port=JTAG` 建链成功；扫链 IDCODE `0x4BA00477`、DP CTRL/STAT `0xF0000000`、AP IDR `0x24770011`，与 OpenOCD 交叉一致；SWD 无回归。⚠️ **JTAG 侧不启用固件块传输**（实测会把探针挂死，§18.9 第十步）|
| Keil 联调 | ✅ 官方 AGDI v1.33.24 下对话框正确显示 `IDCODE 0x2BA01477` / `ARM CoreSight SW-DP`（见 4.7） |
| Flash 下载 | ✅ 烧录与调试均验证通过（擦除失败根因见 4.8） |
| 烧录速率 | ✅ 块传输已接入且**能力探测通过**（日志 `Block transfer probe OK`，§13.7 更正）；越界/正确性验证已通过（§9.2：12/12、哨兵零损坏）。适用范围 = **仅 SWD**（V1(HID) / V2(Bulk) 两条传输层都生效）；**JTAG 侧禁用** —— 实测会挂死探针（§18.9 第十步） |
| 能力探测深度 | ✅ 块传输探测已按**真实工作深度**深读（`BlockWordsLimit()`，orbtrace = 125 字）并逐字校验（**§20.1**）；多笔探测**只验读** —— 其"批量内写校验"实测会破坏本会话后续 AP 存储器访问，**已撤回**（**§20.2**） |
| 传输模式 | ✅ V1(HID) / V2(Bulk) 两条在 Keil 适配器列表里都可选可切换（§14），**V2 已实机可用**：完整调试 + 下载（§17.3 / §17.5 B 臂） |
| V2 通道真因 | ✅ 句柄泄漏导致 WinUSB 接口被**永久**独占（§17.1）+ 出包长度必须用设备自报的 508、**绝不发整包**（§17.2） |
| 传输层选择 | ✅ 只认对话框里选中的那条接口；**已删除全部隐藏开关**（`ORBMDK_TRANSPORT` / `ORBMDK_FWVER` / `ORBMDK_BULK_PAD` / `ORBMDK_SERIAL`），选中通道打不开就如实报错（§17.4） |
| 固件版本门 | ✅ AGDI 用 `"%lu.%lu.%lu"` 解析 `Identify(idNo=4)` 并 `cmp eax,2`，主版本 ≥ 2 **只多走两步**：SWO 传输置 Stream（还需 caps `0x40`）+ 注册 streaming sink —— **函数指针表是无条件安装的**（§17.6 反汇编，§17.7 末更正块）；本层 2026-10-01 起**放开到 ≥ 2**（早年"钉 `1.0.0`"见 §17.5 A/B；放开的实机回归判据见 `Todo.md.bak` §18.10-C 阶段 5） |
| 日志 | ✅ 单一实现 `src/ORBMDK_Log.cpp`（§8.1）：多 Sink（文件 / stdout / DebugView / 宿主回调）、`%TEMP%\ORBMDK_LOG_LEVEL` 热更新、线程安全；`RDDI_SetLogCallback` 已接通 Keil 日志窗口 |

### 文档引用约定（0.5.0 整理时补）

本文中出现的 **`Todo.md §18.x` 属历史编号**。`Todo.md` 于 2026-10-03 精简为"只列未完成项"，
原 §18 体系已整卷移入 **`Todo.md.bak`**（历史快照；**其中 `18.10-C` 等方案的结论已被后续实验推翻，勿直接采信**）。
当前活跃待办只落在 **`Todo.md`**，对应关系如下：

| 本文常引 | 现行落点 |
|---|---|
| §18.8（界面为何开不了 Trace） | `Todo.md` **§5 T1-b**（路线 (b) 已实测否决 → `bug.md` **B12**） |
| §18.10 / §18.10-B / §18.10-C（ETM、SWO 流式） | `Todo.md` **§5 T2**（ETM 建议结案）；流式方案 2026-10-01 **已放开**（版本门 ≥ 2 + caps `0x40`），Keil 侧实机回归（阶段 5）待做，细案在 `Todo.md.bak` |
| §18.5 / §18.11 / §18.12 / §18.13 / §18.14 | `Todo.md` **同号**（现行有效） |
| §18.1 / §18.2-遗留 | `Todo.md` **§3**（保留同号标签） |
| §18.2 / §18.6 / §18.9 / §18.15 | **已完成**，留档在 `Todo.md.bak` |
| §18.7a / §18.7b | 已决"**不做**"，留档在 `Todo.md.bak` |
| §4 阶段 N（性能） | 数据与结论已迁 `bug.md` **附录 B**（B-1=阶段1、B-2=阶段2、B-3=阶段4、B-4=阶段5、B-6=实测口径） |

> 实现细节与排查记录一律在 **`bug.md`**（B 系列）；本文只放**逆向结论与 ABI / 行为判定**。

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

| 项 | 探测方式 |
|----|----------|
| Visual Studio | `vswhere.exe` → 回退常见安装目录（VS 2022 / 2019 / 2017） |
| VC 工具集 | `VC\Tools\MSVC` 下版本号最大者 |
| Windows SDK | 注册表 `KitsRoot10` → 回退 `D:\Windows Kits\10` / `C:\Program Files (x86)\Windows Kits\10`，取同时具备 `Include\<ver>` 与 `Lib\<ver>\ucrt\x86` 的最高版本 |

> 当前环境实测：VS2022 Community（MSVC `14.44.35207`）+ Windows SDK `10.0.26100.0`。

### 编辑器 / IntelliSense

`.vscode/c_cpp_properties.json` 供 VS Code 的 C/C++ 扩展（cpptools）使用，参数与 `build.ps1` 一一对应：

| 配置项 | 值 | build.ps1 |
|--------|----|-----------|
| `compilerPath` | `…\VC\Tools\MSVC\14.44.35207\bin\Hostx86\x86\cl.exe` | `:92` 的 `Hostx86\x86`（32 位，Keil 是 32 位进程） |
| `includePath` | `src` / `include` / MSVC `include` / SDK `ucrt,shared,um,winrt` | `:146-151` 的 `/I` |
| `defines` | `_WINDOWS`、`_USRDLL`、`ORBMDK_EXPORTS`、`WIN32`、`_WINDLL`、`NOMINMAX`、`WIN32_LEAN_AND_MEAN`、`_CRT_SECURE_NO_WARNINGS` + `pch.h` 的 `COBJMACROS` / `INITGUID` 等 | `:143-145` + `src/pch.h` |
| `cppStandard` | `c++17` | `:142` 的 `/std:c++17` |
| `intelliSenseMode` | `windows-msvc-x86` | 由 `compilerPath` 推导 |

> ⚠️ **`compilerPath` 必须显式写**。省略时 cpptools 会去系统 `PATH` 自动探测一个编译器；若 `PATH` 上存在交叉工具链
> （如 MounRiver 的 `arm-none-eabi-gcc.exe`），会被它选中并把模式改成 **`windows-gcc-arm`**，于是 `"pch.h"`、
> `<Windows.h>` 全部解析失败（C/C++ 输出窗口会打印"IntelliSenseMode 已根据编译器参数和查询 compilerPath 从
> windows-msvc-x86 更改为 windows-gcc-arm"）。本项目是 Windows x86 DLL，与 ARM GCC 无关。
>
> VS 大版本升级或换盘符后需同步这一行 —— **只影响 IntelliSense**，`build.ps1` 始终自己探测工具链，互不依赖。

### ⚠️ 必须用 `/MT`（静态链接 CRT），不要改回 `/MD`

Keil 的 `ARM\ARMCLANG\bin\` 下自带一个**旧的** `MSVCP140.dll`（14.29），而 UV4 加载的是它。
MSVC 运行时**只保证向前兼容**：用 VS2022(14.4x) 编译的模块跑在 14.29 上是不受支持的组合，
会直接崩在 `MSVCP140.dll`（`0xc0000005`）。

`/MT` 让本 DLL 不再依赖 `MSVCP140` / `VCRUNTIME140` / UCRT：

```
Dependents: KERNEL32 / SETUPAPI / WINUSB / HID / SHLWAPI
```

RDDI 是纯 C ABI（缓冲区均由调用方提供，不跨模块传 STL/堆指针），静态 CRT 是安全的。
详细分析见 [COMPAT_ANALYSIS.md 第十二节](COMPAT_ANALYSIS.md)。


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

### 2.3 加载后调用点稀少（2 个）

`DbgCM.cpp:149-150` 加载。经 2026-10-01 全量 `scan` 复核，二者**都落在 AGDI 的指针槽里**，
只是触发条件苛刻：

- `CMSIS_DAP_Capabilities`（槽 `0x10362F70`，`stored @0x1002C443`）—— 静态 `call [slot]` 计数为 0，
  因为它走的是 `mov ecx,[slot]` + `call ecx` 的间接形式；**实际有 1 个调用点 `0x10022858`**
  （配置串构建函数：`Capabilities(handle, ifNo, &caps@0x102F8B90)`）。该 caps 是 trace 门控的
  输入（逐条门控 asm、"界面为什么开不了 trace"的结论见 **Todo.md §5 T1-b**；ETM 见 **Todo.md §5 T2**，
  SWO 流式细案见 **`Todo.md.bak` §18.10-B**）。
- `CMSIS_DAP_ConfigureDAP`（槽 `0x10362F80`，`stored @0x1002C48B`）—— 有 **1 个 `call [slot]` 调用点**
  （`refs=5`）。早期"全工程无调用点"的结论**有误，此处修正**。

### 2.4 加载了，但静态扫不到调用点

早期记为"空声明、未加载"的这两个函数，AGDI 的加载块**确实绑定了它们**：
`DAP_GetNumberOfDAPs`（槽 `0x10362F2C`，`stored @0x1002C304`）、
`DAP_GetDAPIDList`（槽 `0x10362F30`，`stored @0x1002C316`），二者 `refs=3`、`call/jmp=0`
（同样经 `mov reg,[slot]` + `call reg` 调用，静态扫 `call [slot]` 扫不到）。
所以准确说法是"**加载了、静态看不到调用点**"，而不是"未加载"。

以下在 `rddi_dll.cpp` 中被**注释掉**（声明保留、定义与加载均无）：
`RDDI_GetLastError`、`RDDI_SetLogCallback`、`DAP_GetInterfaceVersion`、`DAP_Configure`、`DAP_Connect`、
`DAP_Disconnect`、`DAP_GetSupportedOptimisationLevel`、`DAP_RegReadBlock`、`DAP_RegWriteBlock`、
`DAP_RegReadWaitForValue`、`DAP_Target`、`DAP_DefineSequence`、`DAP_RunSequence`、`CMSIS_DAP_GetGUID`。

### 2.5 结论（2026-10-01 修订）

AGDI 在**常规调试链路**里只依赖 **15 个** RDDI 函数。但"trace 相关导出 AGDI 层完全不涉及"
这个结论是**错的**，必须更正为：

- **AGDI 自己实现了 trace**：它在 `0x1003CC80`–`0x1003CEF1` 对 RDDI 模块句柄 `[0x102FA6C0]`
  做**第二轮 `GetProcAddress`**，把 `StreamingTrace_*` 13 个函数全部绑到槽
  `0x10362FDC`–`0x1036300C`；`CMSIS_DAP_SWO_*` 4 个绑在 `0x10362FBC`–`0x10362FC8`。
  两组都有真实 `call [slot]` 调用点（明细见 §10.4；门控结论见 **Todo.md §5 T1-b**）。
- 但这套 trace 通路**在运行时被门控关闭**（`Identify(idNo=4)` 主版本 + caps 位），当前环境不会触发。
  所以"不影响 Keil 调试链路"这句**在行为层面仍然成立**，只是原因不是"不涉及"，而是"没开闸"。

其余导出（`CMSIS_DAP_JTAG_*`、`CMSIS_DAP_PC_*`、`CMSIS_DAP_Atomic_*`、`CMSIS_DAP_SWD_*`、
`DAP_HostStatus` 等）仍属"AGDI 未加载或未调用"。

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
| 实现位置 | **`src/ORBMDK_Log.cpp`（唯一实现）+ `include/ORBMDK_Log.h`（宏）**。2026-09-30 之前是 `ORBMDK_RDDI.cpp` 的 `ORBMDK_Log` 与 `ORBMDK_HID.cpp` 的 `HID_Log` 两套实现（见 §8.1） |
| 默认级别 | **ERROR** |
| 阈值适用范围 | **控制台 / DebugView 与文件日志共用同一阈值**。文件日志曾写死 `level >= INFO` 而绕过阈值，已修正（见 4.10 #21） |
| 过滤时机 | 在构造日志字符串**之前**判断级别，被过滤时零格式化开销 |
| 行格式 | `[ORBMDK][HH:MM:SS.mmm][级别][模块][PID:TID] 消息` |
| 时间 / 线程维度 | 必带本地时间戳 + 进程 ID + 线程 ID。日志跨进程追加、AGDI 多线程调用，缺这两项无法界定"哪次运行、哪个线程" |
| 并发 | 文件写入用 `std::mutex` 串行化；文件句柄按行开关（UV4 会独占文件，外部诊断需要能读） |
| 缓冲区 | 1024 字节；超长时末尾写 `...` 标记，**不允许静默截断** |
| 可恢复故障 | `FAULT` / `WAIT` / `NO_ACK` 等 AGDI 会自行恢复的瞬时状态用 **WARN**，ERROR 只留给真正异常（见 4.10 #25） |
| 临时恢复 | **文件 `%TEMP%\ORBMDK_LOG_LEVEL`**（内容 `0=DEBUG 1=INFO 2=TESTSPEED 3=VERBOSE 4=REV1 5=REV2 6=REV3 7=WARN 8=ERROR`）：µVision **运行中**改动最多 1 秒生效，删掉文件即恢复默认，**不必开命令行、不必重启 IDE**（见 11.1.3）。环境变量 `ORBMDK_LOG_LEVEL` 仍支持（文件名优先），但只在进程启动时读一次。日志路径用 `ORBMDK_LOG_FILE` |
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

**验证**：Keil 重跑 Flash Download 已通过（§13.4 实测、§17.5 B 臂）—— ✅ 已关闭。

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

**✅ 2026-09-30 已处理**（§8 统一方案落地，记录见 §8.1）：模块名统一
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
> "此项在 JTAG 路径上是否仍适用"这一复核 → 已迁至 **`Todo.md` §18.12**。

> 另注：设备拔出后适配器下拉框仍显示 `CMSIS-DAP v1` / 序列号，属 AGDI 缓存的
> `MonConf` 与 HID 层连接状态未失效，**不影响** 上述列表行为。"若要一并处理" →
> 已迁至 **`Todo.md` §18.14**（需让 `ORBMDK_HID_IsConnected()` 具备拔出探活能力）。

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

### 4.15 Keil「HW RESET」报 `RDDI-DAP Error`（`DAP_Target` 的响应缓冲可以是 `NULL`）

**现象**：µVision 的 Reset 方式选 **HW RESET** 时立即弹 `RDDI-DAP Error`；
同一工程选 `SYSRESETREQ` / `VECTRESET`（软件复位）完全正常 —— 后两者走
`DAP_RegAccessBlock` 写 AIRCR，不经过本节这条路径。

**定位（AGDI 侧调用序列）**：硬件复位走 `DAP_Target`，函数 `0x10022EF0`
（`0x100230F0`、`0x100231xx` 为同型副本）：

| 地址 | 反汇编 | 含义 |
|------|--------|------|
| `0x10022F7D` | `push 0 / push 0 / push 0x101E8F24("sys_reset.on") / push handle / call [0x10362F54]` | `DAP_Target(h,"sys_reset.on",NULL,0)` |
| `0x10022F94` | `push 0x32` → `call [0x101B547C]`（`Sleep`） | 断言后保持 ≥50 ms |
| `0x10022FCF` | 同样四个参数，串换成 `0x101E8F14("sys_reset.off")` | 释放复位 |
| `0x10023162` / `0x10023222` | 与上面两条同型（`.on` / `.off`） | 另一份副本 |
| `0x10023041` / `0x10023056` | `call 0x10182AB0`（`strcat` 式辅助，`(dest,src)`） | 先把 `"sys_reset.off"`、`"sys_reset.on"` 拼进 `[ebp-0x24]` 再整串下发 |
| `0x10022F86` / `0x10022FD8`（各副本同） | `test eax,eax / jne … / mov esi,0x2028` | **返回值非 0 即取错误码 `0x2028`** |

> 槽位 `0x10362F54` 即 §10.4 表中的 `DAP_Target`（与其后的 `DAP_DefineSequence` 相邻）。

`0x2028` 的语义：错误串分派 `0x10021BC0` 先做 `code-0x2007`，再查**字节索引表**
`0x10021F28`（`[0x21] = 0x15`，实测落在文件偏移 `0x10021F49`），最后查跳转表
`0x10021DE4`（`[0x15] = 0x10021C7C`）——该桩为 `mov eax,0x101EA970; ret`，
而 `0x101EA970` 正是字符串 `"RDDI-DAP Error"`。**`0x2028` 就是用户看到的那句话**
（§4.8 那次擦除失败也是这条链）。

**根因**：AGDI 这些调用**只传命令串、不要响应**（`resp_str = NULL, resp_len = 0`），
仅以返回值判成败；而 ORBMDK 的 `DAP_Target` 把"响应缓冲为空"当成了参数错误：

```cpp
if (!request_str || !resp_str || resp_len <= 0) return RDDI_BADARG;   // ← 旧实现
```

**ABI 对照**（官方 `CMSIS_DAP.dll.bak` 的 `DAP_Target` @ `0x10019F00`）：先校验句柄，
再 `test eax,eax; je` 跳过写缓冲 —— **只有 `resp_len != 0` 时才写 `[resp] = 0`**；
未知信号（内部词表里是 `"unknown"`）与引脚驱动失败都返回 `0`。
"有缓冲才动缓冲"是官方契约，也是本层必须遵守的 ABI（§10.6）。

**修复（本层要求）**：`DAP_Target` 只把 `request_str == NULL` 视为 `RDDI_BADARG`；
`resp_str == NULL || resp_len <= 0` 解释为"不需要响应"，跳过长度校验与写回，
命令照常执行并返回 `RDDI_SUCCESS`（代码改动与自查记录见 `Todo.md`）。

**同路径的第二个坑（不报错也复位不了）**：`sys_reset` 靠 `DAP_SWJ_Pins` 驱动，
引脚位必须是 **bit7 = nRESET（`0x80`）**，bit5 是 nTRST。三处依据一致：CMSIS-DAP 规范 ·
`include/ORBMDK_DAP.h` 的 `DAP_PIN_nRESET = 1u<<7` · `test/jtagrawprobe.cpp` 对
ORBTrace `dbgIF.v CMD_PINS_WRITE` 的逆向记录。旧实现用的是 `0x20`。

**其它**：AGDI 从不调用 `signal_avail`（DLL 内无该串），故 `"sys_reset;sys_power"`
与官方 `"sys_reset;run_led;"` 的差异对 Keil 无影响。

**验证**：待实机 —— 关 µVision → `build.ps1 -DeployOnly` → Reset 方式选 HW RESET；同时看日志里的
`pins=0x..` 回读（若恒为 `0xFF`，说明固件并未真正驱动 nRESET）。

---

## 五、仍存在的简化实现

> 本节原为逐条 stub 现状（原 §5.1~§5.4）。**全量清单已迁至 `Todo.md` §18.5**
> （RDDI/HID stub 表、Trace 适配层 stub 表、解码器实现深度表、未处理项表四张表）。
>
> 一句话结论：以下均**不影响 Keil 调试链路**（AGDI 常规链路实际只调用第二节列出的
> 15 个函数）；三种"补齐后才会用到"的场景分别对应 —— trace / sequencer → `Todo.md`
> §18.5、ULINK+（不实现）→ §18.7a（已决"不做"，见 `Todo.md.bak`）、ETM v4 深度 → `Todo.md` §5 T2。

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
| `build.ps1` | 编译脚本（自动探测 VS2017+/MSVC/SDK，固定 **x86**，Keil 为 32 位进程，必须 x86）；`-Deploy` / `-DeployOnly` 兼做部署（复制 `bin\ORBMDK_RDDI.dll` 覆盖 Keil 的 `CMSIS_DAP.dll`，自动定位 `ARM\BIN` 并备份原件），原 `deploy.ps1` 已并入 |
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

硬编码候选清单（3 处）与处置结论**已迁至 `Todo.md.bak` §18.7b**（状态 `[-]`，保持现状）。
要点：`ORBMDK_FindObjdumpPath` 已按「环境变量 → Keil `TOOLS.INI` → 常见安装目录 → `PATH`」
顺序探测，前序步骤（`OBJDUMP` / `ARM_TOOLCHAIN_PATH` / `ARMGCC_DIR`）与 `PATH` 兜底可覆盖
多数场景；残留硬编码均在"候选路径 fallback"语义内，不影响构建与实机调试链路。

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
- 🟡 **可选优化（部分已改）**：符号解析候选路径（7.3），已补 `D:\Keil_v5\` —— 明细与处置（`[-]` 保持现状）见 **`Todo.md.bak` §18.7b**。
- 残留硬编码均在"候选路径 fallback"语义内，不影响构建与实机调试链路。

### 7.6 环境变更记录（2026-09-30）

| 项 | 旧值 | 新值 |
|----|------|------|
| Keil MDK | `D:\MDK5\ARM\BIN` | **`D:\Keil_v5\ARM\BIN`** |
| Visual Studio | `D:\Program Files (x86)\Microsoft Visual Studio\2017\Community`（MSVC 14.16.27023） | **`D:\Program Files\Microsoft Visual Studio\2022\Community`（MSVC 14.44.35207）** |
| Windows SDK | `D:\Windows Kits\10\...\10.0.17763.0` | **`D:\Windows Kits\10\...\10.0.26100.0`** |

- 受影响的文件：`build.ps1`、`test/build_test.ps1`、`deploy.ps1`、`test/*.cpp`（DLL 路径）、`src/ORBMDK_Symbols.cpp`。
- 部署脚本的目标路径已改为 `D:\Keil_v5\ARM\BIN\CMSIS_DAP.dll`，并支持 `-KeilArmBin` 显式指定与多候选自动探测（该脚本 2026-10-02 已并入 `build.ps1` 的 `-Deploy`）。

---

## 八、日志模块统一（2026-09-30 已落地）

> 唯一实现 `src/ORBMDK_Log.cpp` + `include/ORBMDK_Log.h`；对外契约见 §3.6。

### 8.1 落地记录（2026-09-30）

**新增**：`include/ORBMDK_Log.h`（级别 / API / 宏）、`src/ORBMDK_Log.cpp`（唯一实现，
已加入 `build.ps1` 源列表）。

**删除**（六类问题逐条闭环）：

| 原问题 | 处理 |
|--------|------|
| 两套并行实现 | 删除 `ORBMDK_RDDI.cpp` 的 `ORBMDK_Log` / `ORBMDK_LogLevel` / `ORBMDK_LogWriteFile`，以及 `ORBMDK_HID.cpp` 的 `HID_Log` / `HID_LogLevel` |
| 宏写在 .cpp 里（死代码） | 宏移入头文件；RDDI 里那组"模块级宏"（`LOG_HID_*` / `LOG_BULK_*` / `LOG_TRACE_*`）删除 |
| 格式 / 能力不一致 | 统一为 `[ORBMDK][时间][级别][模块][pid:tid] 消息`；HID 现在也落盘、也支持 `%TEMP%\ORBMDK_LOG_LEVEL` 热更新 |
| `RDDI_SetLogCallback` 未接通 | 接为 CallbackSink（`HostLogBridge`）：内部级别 → RDDI 级别映射 + `maxLogLevel` 过滤 |
| 无锁 / 无时间戳 | 单 `std::mutex` 保护格式化与文件写入；宿主回调在**锁外**调用（回调里再打日志不会自锁） |
| `g_logEnabled` 死变量 | 删除 |
| 两处"命令级直写文件"（`HidTrace` / `BulkTrace`）绕过级别、绕过 `ORBMDK_LOG_FILE`、无时间戳 | 并入 `ORBMDK_LogTrace()`：走同一路径与格式（**顺带补上时间戳 / 进程线程号**），且**按 INFO 级参与级别过滤**、只落盘（阈值 > INFO 时丢弃） |

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

## 九、烧录速率优化（2026-09-30 已落地）



### 9.1 结论

**瓶颈在 USB 往返次数，不在 SWD 时钟。**

日志中 `Clock=1000000`（1 MHz，理论上限约 125 KB/s），而实测烧录吞吐只有 **≈ 0.5–1 KB/s**
—— **USB 开销占了 99%**。因此"把 1 MHz 提到 10 MHz"在当前实现下几乎没有收益，
必须先解决往返次数问题。

### 9.2 实施记录（2026-09-30）

**已实施（P0 + HID 缓冲区加固）**：

| 项 | 位置 | 改动 |
|----|------|------|
| **P0** | `DAP_RegWriteRepeat` / `DAP_RegReadRepeat` | 改为按 `kMaxBlockWords = 14` 分块调用 `DAP_TransferBlock`；固件不支持时首次失败即**永久回退**到逐字 `DAP_Transfer`（`RDDIContext::blockTransferSupported`），功能不受影响 |
| 加固 | `ORBMDK_HID_DAPCommand` | 长度校验上限由 `DAP_BUFFER_SIZE - 1`(511) 改为 `HID_MAX_PACKET_SIZE - 1`(64)，消除 `memcpy(&reportOut[1], cmd, cmdLen)` 的栈溢出隐患 |
| 防御 | `DAP_RegReadRepeat` | 每块读之前先清零目标区间，避免块传输只回部分数据时残留旧值 |

**未实施（评估后认为收益可忽略）**：

- P1 复用 OVERLAPPED 事件、P2 去掉 `std::vector`：单次 `CreateEvent` / `CloseHandle` /
  堆分配是 **µs** 量级，而一次 HID 往返是 **ms** 量级 —— 相差三个数量级，
  不值得为此引入复杂度。

> ✅ **2026-09-30 实测完成**（`bin\ORBMDK_BlockTransferTest.exe 0x20000000`）：

```
cases passed 12, failed 0            <- 1/13/14/15/28/64 字边界 + 两侧各 8 字哨兵
serial total 301779.1 us
block  total  38372.7 us
speedup       7.86x                  <- 单例 n=64：serial 147.5 ms -> block 16.3 ms (≈9x)
```

- **哨兵零损坏** → §9.3 的越界写（Keil 崩溃根因 #35）确认已修好，块传输可以放心启用；
- 加速比低于理论 14×：V2(Bulk) 下单次往返开销本来就小，固定开销占比上升
  （HID 通路会更接近 14×）；
- 本节的加速比数据均为 **SWD** 下实测。**JTAG** 侧**不启用**块传输：2026-10-01 曾放开，
  实测该命令会让 orbtrace 固件挂死（须重新插拔 USB），当日即撤销 —— 见 **§18.9 第十步**；
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

### 9.3 首次接入时的越界崩溃（已修）

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

   > ⚠️ **本节结论已被 §13.7 推翻**：orbtrace **确实支持** `ID_DAP_TRANSFER_BLOCK`
> （实测 `resp = [06 01 00 01]`）。当时之所以判定"不支持"，是因为探测代码放在
> `DAP_RegWriteRepeat` 里，而那个入口在收到非法 regID 时会提前 return ——
> **探测根本没跑过**，所谓"不支持"只是陈旧产物。现探测已移到连上目标时执行。
>
> 下面这段保留作历史记录：

**实测（当时的错误结论）：orbtrace 固件未实现 `ID_DAP_TRANSFER_BLOCK`**，故在该固件上自动回退、
   行为与优化前一致；换用实现了该命令的调试器时**无需改代码即可提速**。

**保留的有效修复**：HID 长度校验（纯加固、无副作用）、`DAP_TransferBlock`
的两处修复（函数正确性）。

**重新启用前的验证要求**（避免再次把 Keil 拖崩）—— ✅ **2026-09-30 已满足**，
结果与过程见 §9.2（12/12 通过、哨兵零损坏、加速比 7.86×）。验证程序：

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
| 速率 | 打印每个用例的写/读耗时。**无逐字基线** —— 驱动不存在关闭块传输的开关，走哪条路完全由固件探测结果决定 |

**判定标准**：

| 结果 | 结论 |
|------|------|
| 全部 PASS | 固件支持块传输，Keil 中会自动启用 |
| 有 FAIL 或哨兵被踩坏 | 仍有越界，**不能启用** |
| 耗时明显偏慢 | 固件未实现 `ID_DAP_TRANSFER_BLOCK`（已自动回退）。（**此判定当时是错的**，见 §13.7：orbtrace 支持块传输） |

> 注：该文件刻意写成**纯 ASCII**。原先含中文时 MSVC 报 C4819（当前代码页无法表示的字符），
> 并连带产生一条假的 C4474 printf 告警。

**教训**：**"写好但从未被调用"的代码等于未测试代码。** `DAP_TransferBlock`
在仓库里存在已久却零调用点，因此其越界拷贝一直未被发现 —— 把这类函数接入
新路径前，应先给它补最小验证。

### 9.4 速率计量（TESTSPEED）重建 —— 2026-10-03

**背景**：`ORBMDK_LOG_TESTSPEED` / `LOG_AT_TESTSPEED` 宏一直存在，但**全工程零调用点**
（`meter config` / `meter segment` 等字样在 `src/` 搜不到）—— README「烧录速率统计」那节
记录的输出在代码里已经不存在，等于**没有基线**，任何"加速"都无法验收。

**本次只恢复计量，不做任何加速**：

| 位置 | 内容 |
|------|------|
| `src/ORBMDK_Log.cpp` | `MeterState` + `_meterFlushWindow` / `_meterSettleSegment` / `_meterEmit`；公开入口 `ORBMDK_LogMeterEnabled / NowUs / TripBegin / SetTransport / Account / Usb` |
| `include/ORBMDK_Log.h` | 计量 API + `orbmdk::MeterUsbTimer`（RAII，放在 `extern "C"` 之外） |
| `src/ORBMDK_RDDI.cpp` | `PublishMeterConfig()`（传输层 / 端口速度 / 出包字节 / 每次往返字数 / 块传输是否生效，变化时重打）+ `DAP_RegWriteRepeat` / `DAP_RegReadRepeat` 记账（**块 = 1 往返，逐字回退 = chunk 往返**） |
| `src/ORBMDK_HID.cpp` / `ORBMDK_USB_Bulk.cpp` | 每次 OUT / IN 上报耗时（`meter usb` 的 out/in 拆分） |

**口径**（与 §9.2 的实测数据保持一致，改了会让历史数据不可比）：

- kB/s 按 **1 kB = 1024 B**（`4096 B / 7.1 ms → 563.4 kB/s` 就是这么算的）；
- `us/trip` = 窗口 DAP 耗时 ÷ 窗口往返数；`window wall` = 窗口**首笔到末笔**的墙钟，
  它减去 DAP 段即 AGDI / 目标侧的时间（判断"瓶颈在不在这一层"的唯一依据）；
- 落行时机：窗口累计 ≥1024 B 或 ≥200 ms；空闲 >500 ms 视为上一段结束，
  在**下一笔传输到来时**补打 `meter segment` / `meter usb`（故日志末尾可能少最后一段）；
- **失败的那一次不记账**（只在成功路径上 Account）。

**实机首轮暴露并修掉的 4 个计量缺陷**（2026-10-03，HSLinkPro 实测）：

| 现象 | 根因 | 修法 |
|------|------|------|
| 段内 19 次 DAP 往返却报 **7107 cmds** | `meter usb` 只要"窗口 active"就采样，而窗口跨越多笔**不受计量**的 DAP 命令（它们同样走 `_bulkWrite/_bulkRead`） | 新增 `ORBMDK_LogMeterTripBegin()`；USB 采样只认 `TripBegin..Account` 之间 |
| `dap 1.8 ms (137.5%) \| other 0.0 ms (-37.5%)` | 墙钟从"记账时刻"起算，而 DAP 耗时是在记账**之前**测的 ⇒ DAP 段落在窗口之外 | 墙钟改从 `TripBegin` 起算，`dap ≤ wall` 恒成立 |
| `total` 越滚越大、此后再不打 `meter segment` | `_meterSettleSegment()` 在 `wallUs == 0` 时提前 `return`，**没清零段计数** | 抽出 `resetSegment()` 无条件清零；且不再用"上一段末笔"当新段起点 |
| `window wall 0.0 ms, dap 0.0%` | 窗口墙钟用 1 ms 分辨率的 `GetTickCount64` | 全部时间戳改用 µs 单调时钟（`ORBMDK_LogMeterNowUs`） |

**零开销保证**：阈值 > TESTSPEED 时 `ORBMDK_LogMeterEnabled()` 返回 0，调用方既不取时钟也不记账；
传输层的 `MeterUsbTimer` 在关闭时构造/析构都是空操作。

**待实机验收**（见 `Todo.md` §4 阶段 1）：设 `%TEMP%\ORBMDK_LOG_LEVEL = 2` 跑一次下载，
确认四类行出现，且 `us/trip` 与 `words/round trip` 自洽
（V1/HID 14 字 ≈95 µs/trip；V2/508 B 包 125 字 ≈789 µs/trip）。

**本轮明确排除**：JTAG 侧的 IR+DR 合并、响应缓冲放大等加速项 —— 另行再议。

### 9.5 多笔传输（D2，`DAP_Transfer` count>1）—— 2026-10-03

**目的**：`DAP_RegAccessBlock` 过去**逐寄存器一笔 USB 往返**。AGDI 的 `SWD_GetARMRegs` /
`SWD_SetARMRegs`（单步、运行、命中断点时的上下文保存/恢复）一次就是十几笔 ⇒ 十几次往返，
而每次往返固定 ≈150 µs（§9.4 实测）。改为把**连续的非等待**寄存器合进一条 `DAP_Transfer`。

| 位置 | 内容 |
|------|------|
| `src/ORBMDK_HID.cpp` | 新增 `DAP_TransferMulti()` / `DAP_TransferMultiMax()`：请求 `[0x05][dapId][count][req…]`；响应按**读笔顺序**取数；`done != count` 视为失败 |
| `src/ORBMDK_RDDI.cpp` | `DAP_RegAccessBlock` 攒批量 + `flushBatch()`；`kMaxMultiBatch = 100`，实际上限取 `min(它, DAP_TransferMultiMax())`（V1/HID 12 笔、V2/511 B 101 笔） |

**安全设计**（与块传输 §13.7 同一套："先探测、失败即回退"）：

- **只用于 SWD**：JTAG 下固件的 `DAP_Transfer(0x05)` 不响应（§18.9），那里 `DapTransferFor`
  走本层 JTAG 引擎，无法合并批量；
- **能力先探测**：首次真正合并批量前先发一个批量（"连读两次 DP IDCODE"），并与**单笔**结果交叉比对，
  三者一致才算通过（`ProbeTransferMulti`）。理由：这条命令在内存/寄存器热路径上，
  固件若把 `count` 理解错会**静默读错**，不能只信"规范要求支持"；
- **失败即永久回退**：探测失败或中途 `done != count` ⇒ 本会话 `transferMultiSupported=false`，
  之后全部走逐笔（与历史行为逐字一致）；
- **顺序语义不变**：`DAP_Transfer` 按序执行，同一批量里的 DP_SELECT 之类顺带写保序；
  `MATCH_MASK(16)` / `MATCH_RETRY(17)` 虚拟寄存器与 `WaitForValue` 项都会**先 flush 再单独处理**；
- **失败不使用部分结果**（与逐笔路径"在首个失败处返回"一致）。

**诊断**：TESTSPEED 级每次调用落一行
`RegAccessBlock: numRegs=N -> T round trip(s) (multi-transfer on)` —— 合并批量生效时 `T << N`。

**待实机验证**（见 `Todo.md` §4 阶段 5）：Keil 里单步/运行几次，确认 ① 出现 `TransferMulti probe OK`；
② 不出现 `多笔传输失败 … 回退逐笔`；③ 寄存器/内存窗口数值正确；④ `numRegs=16 -> 1 round trip(s)`。

### 9.6 D1：`WaitForValue` 的处理（一次失败的尝试 + 最终方案）—— 2026-10-03

**问题（D2 落地后实测暴露）**：D2 只把"连续的非等待寄存器"合并批量，而 AGDI 会在普通寄存器之间
**插入 `DAP_REG_WaitForValue` 读**。实测形态（HSLinkPro）：

| `numRegs → trips` | `[batch, single, wait]` | 说明 |
|---|---|---|
| `5 → 1` | `[1, 0, 0]` | 完美合并批量 |
| `6 → 5` | `[1, 2, 2]` | 2 个 wait 把 6 笔切成 3 段 |
| `60 → 41` | `[19, 2, 20]` | **20 个 wait 把 40 个普通项切成 19 段** |

即：瓶颈不是"轮询太多次"（这些 wait 第一次读就匹配上、各只花 1 往返），而是**它们把批量切碎了**。

#### 尝试：把 wait 也编进批量（❌ 实测失败，已撤销）

思路是让固件用 `MATCH_VALUE` 在内部等，并把匹配读与普通项放进同一条 `DAP_Transfer`。
固件语义确实支持其中大部分（`DAP/Source/DAP.c`）：

| 事实 | 出处 |
|------|------|
| 比较式 `(data & match_mask) != match_value` —— **匹配值不参与掩码**，主机需预先相与 | `:782` |
| `MATCH_MASK(0x20)` 是**写笔伪操作**（只设固件状态、不产生总线访问），且**持久有效** | `:864-867` |
| 失配 `→ MISMATCH; break;`：**中断后续笔**，失配项不计入 `response_count` | `:783-788, 890` |
| ★ **匹配读不回数据**：`MATCH_VALUE` 分支结束后直接出循环，而 "Store data" 只在**普通读**分支里 | `:832-836` vs `:755-788` |

**决定性的一条是第 4 条**：匹配读**不往响应里写数据**。实测吻合 —— 探测发
`[掩码伪写笔][匹配读 IDCODE]` 得到 `done=2` 且无失配位，但读回**全 0**
（`TransferMulti probe: 匹配值路径未按预期成功 (done=2, rd=0x00000000)`）。
⇒ 匹配读**只能用于"等待条件成立"**，不能用来取值，因此"把 wait 编进批量"在该固件上不成立。
撤销后探测自身失败还会把 `transferMultiSupported` 置 false，连带 D2 全部退回逐笔
（实测 `29 -> 29`、`5 -> 3`、`batch=0`）—— 这也是"探测失败要能干净回退"的一次验证。

#### 最终方案：**有界等待**（`processOneLegacy` 的等待值分支）

保持 wait 走单笔、不编进批量（顺序语义不变），但把"最坏 100 次往返"压成 3 次：

1. 先按老路读一次 —— 条件已满足时**就是 1 次往返**（与历史完全一致，热路径无变化）；
2. 未满足且 `ctx->waitMatchSupported` 时：发一条 `[掩码伪写笔]+[匹配读]`（**1 次往返**）让固件
   在其内部按 `match_retry` 等；成功后**再补一次普通读**（1 次往返）取真实值；
3. 若固件说成功、普通读却仍不匹配 ⇒ 该固件忽略 match 位 ⇒ 本会话把 `waitMatchSupported`
   置 false，回到纯主机轮询。目标重连 / 切 SWD-JTAG 模式时重新置 true。

**收益**：条件迟迟不满足时（擦除/写 flash 的等待、§4.8 那类场景）从"最多 100 次往返"变成
**最多 3 次**；条件已满足时与历史逐字相同。

**诊断**：TESTSPEED 级 `numRegs ≥ 4` 时落一行
`RegAccessBlock: numRegs=N -> T round trip(s) [batch=… single=… wait=…]`。
D2 生效的形态应回到 `5 -> 1`、`6 -> 3`、`60 -> 21` 量级（wait 各占 1 往返 + 普通项合并批量）。

---

## 十、逆向分析工具链与已得结论

> 目的：把 §4.7 / §4.8 / §4.11 / §4.12 / §4.13 期间用到的工具、方法、结论固化下来，
> 后续排查 AGDI ↔ RDDI 接口问题时可**直接复现**，不必重新推导。
> 另有 `tools/agdi_ver.py` 按"适配器条目字段基址"的 disp32 字节模式定位 AGDI 的版本判断链（§17.6）。

### 10.1 工具与依赖

| 项 | 说明 |
|----|------|
| 脚本 | `tools/pe_re.py`（本仓库，8 个子命令合一，无第三方 PE 库依赖；`scan` 用字节模式匹配，不依赖解码同步） |
| 依赖 | Python 3.x + `capstone`（`python -m pip install capstone`）。实测 Python 3.14 + capstone 5.0.6 |
| 目标 | 32 位 PE。`CMSIS_AGDI.dll` / `CMSIS_DAP.dll` 的 imagebase 均为 `0x10000000` |

> ⚠️ 早期版本散落在 `%TEMP%`，会随清理丢失；现已合并进仓库。

### 10.2 命令速查

```bash
PY="python tools/pe_re.py"
AGDI="D:\Keil_v5\ARM\BIN\CMSIS_AGDI.dll"     # 官方 AGDI 层（实测装在 D:\Keil_v5）
RDDI="D:\Keil_v5\ARM\BIN\CMSIS_DAP.dll"      # 当前生效的 RDDI 层
OFF="D:\Keil_v5\ARM\BIN\CMSIS_DAP.dll.bak"   # 官方 RDDI 层（对照基准）

$PY exports "$AGDI"                    # 导出表（名称/序号/RVA/VA）
$PY imports "$AGDI"                    # 导入的 DLL 名
$PY names   "$AGDI" "^(CMSIS_DAP|DAP|RDDI)_"   # 扫描符号名（ASCII + UTF-16LE）
$PY dis     "$AGDI" 0x10022A60 0xD0            # 按 VA 反汇编（自动注释字符串立即数）
$PY dis     "$OFF"  CMSIS_DAP_GetDeviceIDList 400   # 按导出名反汇编
$PY hex     "$AGDI" 0x101EB390 0x120           # 十六进制转储（配置串模板等）
$PY xref    "$AGDI" CMSIS_DAP_Disconnect       # 字符串 + 代码引用点
$PY refs    "$AGDI" 0x102F8B90                 # 谁引用了这个全局（默认只扫 .text）
$PY slots   "$AGDI" 0x1002C000 0x1002CA00 0x1003CC80 0x1003CF00   # 槽映射（可多段）
$PY scan    "$AGDI"                            # 通扫 .text，自动兜出所有 GetProcAddress 块
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

**还有一个变体**：编译器可能先把结果搬进中间寄存器再落表——

```asm
call esi                        ; GetProcAddress(名字)
cmp  dword ptr [0x10362FBC], 0  ; 中间夹 cmp/test 不打断 pending
mov  ecx, eax                   ; 先搬进中间寄存器
mov  dword ptr [slot], ecx      ; 再落表
```

只认 `mov [slot],eax` 会**漏掉整类写法**（`CMSIS_DAP_SWO_Data` 的槽就是这么被漏判成
`0x102FA574` 的）。现版本同时认 `mov [slot],eax` 与 `mov reg,eax` + `mov [slot],reg`。

**（3）`scan` 刻意不用线性反汇编**

`.text` 里嵌着跳转表（如 `0x10011ED8` 的 `DllUv3Cap` 分派表 —— 该函数按 `cmp eax,0x6d` +
字节表 `0x10011EF0` 分派，仅 `n=1/2/100/110` 四个真实分支且返回值全为常量，是 µVision 判定
"驱动类别"的来源，见 `Todo.md` §5 T2 证据 1）和对齐填充，线性反汇编一旦
在某处脱同步，**后面全部解错**（实测按线性扫描实现的 `scan` 直接返回空）。现版本改用字节模式：
`push imm32(str)`（`68 xx`）+ `call reg`（`FF D0..D7`）+ `mov [slot],<reg>`（`A3` 或
`89 05/0D/15/1D/25/35/3D`），按地址配对，完全绕开解码同步问题；因此能自动兜出分散在多处的
加载块（`scan` 找到 103 个槽，而手工给两段区间只找到 65 个）。

> 注意：`slots`/`refs` 打印的 `call/jmp @0x…` 是**操作数地址**，即 `call [slot]` 指令地址 `+2`。

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
| `0x10362F58` | `DAP_DefineSequence` | `0x10362FC8` | `CMSIS_DAP_SWO_Data` |
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

> `CMSIS_DAP_WriteABORT` 已在全量 `scan` 中找到：槽 `0x10362FA4`（`stored @0x1002C566`），
> `refs=4`、`call/jmp=1` —— 早期"不在扫描区间"的存疑项已关闭。

**第二轮 GetProcAddress：trace 专用加载块（`0x1003CD28`–`0x1003CDFB`）**

常规加载块之后，AGDI 还在 `0x1003CC80`–`0x1003CEF1` 对同一个模块句柄 `[0x102FA6C0]`
再跑一轮 `GetProcAddress`，专门绑定 trace 接口（`$PY scan "$AGDI"` 可自动兜出；
`$PY slots "$AGDI" 0x1003CC80 0x1003CF00` 可单看这一段）：

| 槽 | 函数名 | 落表点 | `call/jmp` |
|----|--------|--------|-----------|
| `0x10362FDC` | `StreamingTrace_Connect` | `0x1003CD28` | 0（经 `mov reg,[slot]`+`call reg`） |
| `0x10362FE0` | `StreamingTrace_Disconnect` | `0x1003CD3A` | 0（同上） |
| `0x10362FE4` | `StreamingTrace_GetSinkCount` | `0x1003CD4C` | 1：`0x1003CEB5` |
| `0x10362FE8` | `StreamingTrace_GetSinkDetails` | `0x1003CD5E` | 1：`0x1003CEDA` |
| `0x10362FEC` | `StreamingTrace_GetConfigItem` | `0x1003CD70` | 0 |
| `0x10362FF0` | `StreamingTrace_SetConfigItem` | `0x1003CD82` | 0 |
| `0x10362FF4` | `StreamingTrace_Attach` | `0x1003CD94` | 1：`0x1003D5B1` |
| `0x10362FF8` | `StreamingTrace_Detach` | `0x1003CDA6` | 2：`0x1003D01C` |
| `0x10362FFC` | `StreamingTrace_SubmitEventBuffer` | `0x1003CDB8` | 1：`0x1003CF95` |
| `0x10363000` | `StreamingTrace_WaitForEvent` | `0x1003CDCA` | 1：`0x1003D663` |
| `0x10363004` | `StreamingTrace_Start` | `0x1003CDDC` | 1：`0x1003D5C6` |
| `0x10363008` | `StreamingTrace_Flush` | `0x1003CDEE` | 1：`0x1003D002` |
| `0x1036300C` | `StreamingTrace_Stop` | `0x1003CDFB` | 1：`0x1003D00F` |

SWO 四槽的真实调用点：`Baudrate` @`0x1003D094`（共 2 处）、`Control` @`0x1003D27D`、
`Status` @`0x100344E4`、`Data` @`0x1003D35A`（**4 参**）。
门控条件、sink 名、配置串构建与"界面上为什么开不了 trace"见 **Todo.md §5 T1-b**；
ETM 指令跟踪为什么开不了（**UI 层不可选** —— µVision 按驱动类别限定；AGDI 协议层无并行 / ETB 编码；
另需目标硬件前提）见 **Todo.md §5 T2**（结论 5 为 2026-10-01 二次复核新增）。

> **sink 注册链路的逐参取证与记录布局**（逐调用点入参表、sink 描述记录 `0x20` 布局、
> 事件条目 16 字节布局、`WaitForEvent` token 协议）已迁至 **`Todo.md.bak` §18.10-C 执行记录**；
> 本节只保留 trace 函数指针槽与槽位落表点。参数语义结论见 `Todo.md.bak` §18.10-C 的 P4 表。

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

**（5）硬件复位序列（`0x10022EF0`，Reset 方式 = HW RESET）**

```
DAP_Target(h, "sys_reset.on",  NULL, 0)   → 返回值非 0 ⇒ 0x2028 → "RDDI-DAP Error"
Sleep(50)
DAP_Target(h, "sys_reset.off", NULL, 0)   → 同上
```

两处**都不带响应缓冲**（`NULL, 0`），所以本层绝不能把"无缓冲"当参数错误（§4.15）。
`sys_reset` 具体怎么驱动由本层决定；另有调用点 `0x100152xx` 传的是 `sys_reset.read`
+ `buf[0x20]`，它用 `strstr` 在响应里找 `"sys_reset.off"` —— 所以本层返回的响应
**不必**带官方的结尾 `;`。

### 10.6 已得结论 C：官方 RDDI 层 ABI 要点（对照 `CMSIS_DAP.dll.bak`）

| 函数 | 官方语义 | 备注 |
|------|----------|------|
| `CMSIS_DAP_GetDeviceIDList(h, int *idArray, size_t sizeBytes)` | 从内部设备表 `[0x1005ab2c]`（计数 `[0x1005ab24]`）逐个拷贝 4 字节**设备 ID**；`sizeBytes` 是**字节数**（实现里 `shr esi,2`） | 写入的是 **IDCODE，不是索引**（§4.7） |
| `CMSIS_DAP_GetNumberOfDevices` | 写内部计数 | |
| `CMSIS_DAP_ConfigureDebugger(h, const char *cfg)` | **2 参**，校验句柄后返回状态（0 = 成功） | 与 ORBMDK 一致 |
| `CMSIS_DAP_Disconnect(h)` | 存在但 AGDI 从不调用 | |
| `CMSIS_DAP_GetInterfaceVersion(h, int *version)` | 2 参，写 `0x00020000` | |
| `DAP_Target(h, const char *cmd, char *resp, int respLen)` | 命令串以 `;` 分隔、响应以 `;` 结尾（末位 `;` 换成 `\0`）；**`resp == NULL` / `respLen == 0` 合法**，只在 `respLen != 0` 时才写缓冲；未知信号与驱动失败仍返回 `0` | 硬件复位 `sys_reset.on/off` 就是这种"不要响应"的调用（**§4.15**） |

**ORBMDK 相对官方多出的导出**（5 个，Keil 用不到）：
`CMSIS_DAP_DetectDAPIDList`、`CMSIS_DAP_DetectNumberOfDAPs`、`CMSIS_DAP_JTAG_Configure`、
`CMSIS_DAP_ResetTarget`、`CMSIS_DAP_SWD_Configure`。

**官方有、ORBMDK 没有**（10 个）：`CMSIS_DAP_Disconnect` + 9 个 `ULINKPLUS_*`（ULINKplus 专用）。

### 10.7 已得结论 D：错误码

| 码 | 含义 |
|----|------|
| `0x2000` | `RDDI_DAP_ERROR` |
| `0x100D` | 特定错误（AGDI 单独分支处理，映射为 `0x202B`） |
| `0x2028` | AGDI 内部错误（`ConfigureInterface` / `DAP_Configure` / `GetDeviceIDList` / `DetectNumberOfDevices` 失败、设备数 > 64，**或 `DAP_Target("sys_reset.on/off")` 返回非 0**）；用户可见串 = `"RDDI-DAP Error"`（**§4.15**） |
| `0x202B` | AGDI 内部错误（子调用返回 `0x100D`） |
| `0x2060` | AGDI 内部错误（子调用返回 `0x2000`） |
| `0x0D` | 官方 `GetDeviceIDList` 在 `idArray == NULL` 时返回（即 `RDDI_BADARG`） |

> 用户可见串的取法（`0x10021BC0`）：`index = code - 0x2007` → 字节表 `0x10021F28[index]`
> → 跳转表 `0x10021DE4[+4*n]` → 各桩返回字符串常量。已逐个核对：
> `0x2028`（`index 0x21` → `0x15` → 桩 `0x10021C7C` → `0x101EA970`）= `"RDDI-DAP Error"`；
> 邻近的 `0x2027` = `"MTB Trace Error"`（`0x101EA960`）、
> `0x2029` = `"CMSIS_DAP.DLL missing"`（`0x101EA980`）。

### 10.8 复现清单（从头重跑一遍）

1. `$PY exports "$AGDI"` → 确认版本（本档基于 **v1.33.24.0**，imagebase `0x10000000`）
2. `$PY slots "$AGDI" 0x1002C000 0x1002CA00` → 得到 §10.4 的槽位表与调用点
3. `$PY xref "$AGDI" <函数名>` → 定位该名字的解析点与引用点
4. `$PY dis "$AGDI" <VA> <len>` → 反汇编目标调用点，读出参数个数与含义
5. `$PY dis "$OFF" <导出名> <len>` → 与官方实现对照，确认 ABI 与输出参数语义
6. 必要时用 `ORBMDK_LOG_LEVEL`（`0=DEBUG 1=INFO 2=TESTSPEED 3=VERBOSE 4=REV1 5=REV2 6=REV3 7=WARN 8=ERROR`）打开日志，
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

**验证 → ✅ 已关闭（2026-09-30）**：原 3 条"待实测"项全部达成 ——
V2 实机可用、对话框显示 `CMSIS-DAP v2`、烧录提速 7.86×（§13.3 / §13.4 / §9.2）。

> 排障提示（保留）：若日志里出现 `skip iface ...: class=0x03` 之后没有命中项，说明 WinUSB 未绑定到
> Bulk 接口（需要用 Zadig/Keil 自带驱动安装），此时会安全回退 V1。

### 11.1.3 日志开关：改为文件驱动（免命令行 / 免重启）

**问题**：原开关只有环境变量 `ORBMDK_LOG_LEVEL`，而 µVision 从 IDE 启动、不继承
任意 shell 的环境，于是"先在命令行 `set` 再启动 µVision"成了必需步骤 —— 实测太麻烦。

**改法**：`ORBMDK_LogLevel()` 改为**先读文件、再读环境变量**，并支持热更新。

| 项 | 说明 |
|----|------|
| 文件 | `%TEMP%\ORBMDK_LOG_LEVEL`，纯文本，内容只写一个数字（`0=DEBUG 1=INFO 2=TESTSPEED 3=VERBOSE 4=REV1 5=REV2 6=REV3 7=WARN 8=ERROR`） |
| 生效 | **µVision 运行中**改动最多 **1 秒**生效（内部 1 秒节流的 `GetTickCount64` 缓存，避免每条日志都做 IO） |
| 恢复 | **删掉文件即回到默认 ERROR**，同样不用重启 |
| 优先级 | 文件 > 环境变量（后者只在进程启动时读一次，保留兼容） |
| 命令级埋点 | `ORBMDK_USB_Bulk.cpp` / `ORBMDK_HID.cpp` 的 `[BULK]`/`[HID]` 命令级埋点经 `ORBMDK_LogTrace()` **只落盘**，并**按 INFO 级参与级别过滤**（阈值 > INFO 时不落盘，避免命令级日志无限膨胀） |

> 判定 DLL 是否为最新构建：日志里若**完全没有** `Init: entered` 行，说明 Keil 加载的
> 仍是旧 DLL。µVision 会把 DLL 常驻内存，**必须结束整个 `UV4` 进程**再重开，
> 仅关闭调试会话无效（实测踩过：DLL 部署于 14:57，而 `UV4` 启动于 14:53，
> 之后所有测试都跑在旧代码上，日志时间戳停在 14:11）。

### 11.2 状态 LED：`DAP_HostStatus` 无人调用

**现象**：烧录、调试时 orbtrace 的 Connect / Running LED 均不亮。

**根因**：`DAP_HostStatus` 两层都已实现（`ORBMDK_HID.cpp:968` 与
`ORBMDK_RDDI.cpp` 的导出），但**官方 AGDI 从不调用它** ——
其符号表里没有 `DAP_HostStatus`，反汇编（§10.1）也找不到调用点。
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

**✅ 实机确认（2026-10-01）**：接上目标后 Connect LED 亮；烧录 / 运行时 Running LED 亮，
停机时灭。本节遗留的"待实测"项已关闭（§18.4）。

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

> 该表只说明三种模式的**命令通路**等价；当时"V2 不比 V1 快、orbtrace 不支持块传输"的
> 结论已被 §13.7 / §17.2 更正（orbtrace 支持块传输；出包长度取 508 而非 `wMaxPacketSize`）。

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

已按真实签名修正（崩溃消失）。**剩余两类失败**（`DAP_REG_*` 常量陈旧 → 当前
`PASSED 22 / FAILED 20` 不再是 40/40；目标未上电致 AP FAULT）**已迁至 `Todo.md` §18.11。**

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

> 日期：2026-09-30。本节**推翻 §9.3 的结论**。

**（1）§9.3 的"orbtrace 未实现 ID_DAP_TRANSFER_BLOCK"是错的**

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
| `ifNo=1` | CMSIS-DAP v1 (HID) | `CMSIS-DAP v1`；总线上无 v1 候选时 `CMSIS-DAP v1 (未检测到)`（见 Todo 续 7） |

| 函数 | 改动 |
|------|------|
| `CMSIS_DAP_Detect` | 由 `1` 改为 `kTransportInterfaceCount = 2`（**绝不返回 0**，否则 AGDI 直接 EU02） |
| `CMSIS_DAP_Identify` | `idNo=2` 按 `ifNo` 返回不同名字（`ORBMDK_USB_Bulk_GetInterfaceName`）；序列号两个接口相同（同一物理设备），但 `ifNo=1` 且无 v1 候选时回 `Unknown`（不给幽灵 v1 背书，见 Todo 续 7） |
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
这解释了"**之前 V1 一直是好的**"：V1 路径恰好把版本钉在 `1.0.0`（**早年口径**），从来不会进那条分支。

叠加 `CMSIS_DAP_Detect` 恒为 2（§14），AGDI 眼里"两台设备 + 驱动自报支持多 DAP"，
但本层的设备表/DAP 表始终只有 1 项 —— 三条口径互不自洽。

### 16.2 本轮修复（4 项）

| # | 位置 | 问题 | 修复 |
|---|------|------|------|
| 48 | `DAP_GetDAPIDList` | `sizeOfArray < sizeof(int) → RDDI_BADARG`：官方按**字节数**解释（§10 方法论里的 `shr esi,2`），但 AGDI 在"多 DAP"分支里传的是**元素个数 1** → 直接返回 `RDDI_BADARG`，而且**静默**（不发任何 USB 命令、不写输出参数）。现场表现就是"`CMSIS_DAP_Connect` 成功之后立刻 Disconnect/Close，对话框显示 RDDI-DAP Error" | 两种解释都接受：`≥ sizeof(int)` 按字节、`1..sizeof(int)-1` 按元素个数；两者都不越界。内容仍是 DAP **索引** `0`（会被当作 CMSIS-DAP 的 DAP Index 字节使用，不能写 IDCODE）。日志补 `sizeOfArray → maxEntries` |
| 49 | `CMSIS_DAP_JTAG_GetIDCODEs` | 只认固件的 `ID_DAP_JTAG_IDCODE` 命令；**SWD** 目标上该命令必然失败 → 即使 IDCODE 读得出来，这里也给不出任何 IDCODE。单设备路径下 AGDI 只取 `*count`（还能容忍 0），"多 DAP"分支要的是 idcodes **列表** → 空 = 失败 | 改为优先用与 `GetDeviceIDList` / `DetectDAPIDList` / `DetectNumberOfDAPs` **同一张表**（`ctx->dapIdList`，即协议无关的 DAP 扫描）填充 `idcodes` + `*count`；表为空时才回退 JTAG 命令。保持"扫不到 = 成功 + count 0"（§4.14）。`idcodes == NULL` 时只写 `*count` |
| 50 | `RDDI_Open`（V2 分支） | V2 下 `firmwareVersion` 为空 → `Identify(idNo=4)` 每次现问设备，两次调用可能给出不同的串（AGDI 拿它做能力判定） | V2 打开后补一次 `DAP_GetInfo(DAP_INFO_FIRMWARE)` 并**缓存进 ctx**，此后 `Identify` 只回缓存值 |
| 51 | `ORBMDK_USB_Bulk.cpp` | `_findAndOpenDevice` 里 `(void)serial;`：序列号筛选**从未实现**。同机插多台时只能取枚举到的第一台，插拔顺序一变就换机，界面也看不出来 | 新增 `%TEMP%\ORBMDK_SERIAL` / 环境变量 `ORBMDK_SERIAL`（与传输开关同风格，文件优先）；`_findAndOpenDeviceInGuid` 打开候选后读 iSerialNumber 描述符比对（设备路径里是位置型实例 ID，取不到序列号）；`RDDI_Open` 把该值传下去，V2/HID 两条路径共用。⚠️ **2026-09-30 该开关已移除**（§17.4 的原则：不留任何隐藏开关）；AGDI 的 `pDetails` 恒为 NULL，本层按枚举顺序取第一台 |

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
3. 因此**在本环境下**上报串要落在 1.x 才走得通（这一半曾长期"钉住"）；设备自报串照问、照记日志。
   来源 = 设备 `DAP_Info(0x04)`。**不再读 USB 描述符的 bcdDevice** —— 那是 USB 栈 / 引导写的
   字段，与固件版本无关（已从三个源文件中删除）。
   ⚠️ **主版本的归一化方向已于 2026-10-01 反转**：旧实现把主版本**压到 1**（`2.1.0` → `1.1.0`，
   自报 1.x 原样）；自 `Todo.md.bak` §18.10-C 第三轮起改为**抬到 ≥ 2**（`1.x` → `2.x`，`2.x` 原样），
   以放行 AGDI 的 SWO 流式分支。问不到设备时仍兜底 `1.0.0`。放开的**回归判据与回退条件**见
   `Todo.md.bak` §18.10-C —— **阶段 5 实机回归尚未完成**（本表 A 臂即放开后必须整体回归的清单）。
   本节结论**推翻了早期"不改版本串的取值"那一条**（当时的推断被本次实验推翻：
   那条分支在本环境下靠"逐项对齐接口"过不去，AGDI 根本不会走到那些接口）。
4. 临时诊断入口 `ORBMDK_FWVER` 已按约定删除，正式代码里不留开关。

> 附注（**2026-10-01 后已过时，仅存史**）：这里设想的"只把主版本归一化为 1"曾是实现口径；
> 自 `Todo.md.bak` §18.10-C 第三轮（2026-10-01）起改为**主动放开到 ≥ 2**
> （**只为 SWO 流式传输，与 ETM 无关** —— 旧记述"ETM 的唯一前置"已作废），并配套补齐了
> `StreamingTrace_*` 流式语义。上表 A 臂的失败即放开后**必须整体回归**的清单
> —— 判据 / 回退见 `Todo.md.bak` §18.10-C，**阶段 5 实机回归尚待完成**。

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

**调用点一 `0x10022870`**（会话/DLL 加载路径）—— `cmp/jb` **只门控"caps → Read/Stream"**；
两张函数指针表在其后**无条件安装**（`jb` 落点 `0x10022897` 紧接 `0x1002289C`）：

```asm
0x10022858  call ecx                            ; CMSIS_DAP_Capabilities(handle, ifNo, caps@0x102F8B90)
0x10022870  call 0x10021FB0                     ; major
0x10022875  cmp  eax, 2
0x10022878  jb   0x10022897                     ; major < 2 → 原样（单 DAP 路径）
0x1002287A  test byte ptr [0x102F8B90], 0x40    ; caps 位
0x10022890  mov  byte ptr [0x10304B79], al      ; 仅 major≥2 且 caps.0x40 时到达 → 传输方式 = Stream
0x1002289C  mov  dword ptr [0x102375F4], 0x1003D030   ; ★ 安装函数指针表 1（无条件；jb 落点紧接此处）
0x100228A6  mov  dword ptr [0x10237994], 0x1003D0F0   ; ★ 安装函数指针表 2（无条件）
```

**调用点二 `0x1003CB92`** —— 同一个 `major ≥ 2` 门槛（此处还要求"sink 尚未注册"），
通过后填的是 **streaming sink 描述记录区**（8 条 × `0x20`），**不是函数指针表**：

```asm
0x1003CB92  call 0x10021FB0
0x1003CB97  cmp  eax, 2
0x1003CB9A  jb   0x1003CEF1
0x1003CBA0  cmp  dword ptr [0x10363010], 1
0x1003CBA7  je   0x1003CEF1
0x1003CBB2  mov  dword ptr [0x10363020], 0x10363120   ; ★ sink 描述记录区（8 条 × 0x20）
```

**结论**：AGDI 把 `Identify(idNo=4)` 的返回值当版本号 `sscanf`，用 **`cmp eax,2`** 做门槛；
主版本 ≥ 2 时**只多走两步** —— 把 SWO 传输方式置为 Stream（还需 caps `0x40`）并注册
streaming sink。**两张"函数指针表"（`0x1003D030`/`0x1003D0F0`）是无条件安装的，与本门槛
无关**（订正见 §17.7 末的更正块）。早年（§17.5 A 臂）这条路径在本环境下走不通（跳过全部设备枚举、
`Connect` 后立刻 Close），故一度把这一栏**钉在 `1.0.0`**；**2026-10-01 起改为放开到 ≥ 2**
（见 §17.5 要点 3 与 `Todo.md.bak` §18.10-C）。这也正是 §17.5"只改这一个字符串、行为就翻转"的代码级解释。

> **这个"钉子"已于 2026-10-01 解开（放开到 ≥ 2）**；完整方案 / 回退判据见 `Todo.md.bak` §18.10-C
> （原 §17「多 DAP 分支」专章已整体迁出至 §18.10-B）。

### 17.7 ⭐ 「多 DAP 分支」专章 —— 已整体迁出至 `Todo.md.bak` §18.10-B

> 本节（原含 17.7.1 触发条件 / 17.7.2 两个候选解释 H1·H2 / 17.7.3 已就绪接口账 /
> 17.7.4 P0~P3 探测立项 / 17.7.5 三条解法路线）**已全文并入 `Todo.md.bak` §18.10-B**，
> 此处只留索引。§17.5/§17.6 回答"**版本门是怎么被 AGDI 用起来的**"（早年据此钉 `1.0.0`，
> 2026-10-01 已改为放开到 ≥ 2）；"**放开 / 回退的条件与步骤**"见 `Todo.md.bak` §18.10-C。
> ETM **不**走 `StreamingTrace_*` 表（那套是 SWO 流式传输，见下方 ②）；两条
> `cmp eax,2 / jb`（`0x10022870`、`0x1003CB92`，`major` 由 `0x10021FB0` 对
> `Identify(idNo=4)` 串做 `sscanf("%lu.%lu.%lu")` 得来）**只管**"SWO 的 Read / Stream 选择"
> 与"streaming sink 注册"，与"能不能开 ETM"**无关**。决定 B 能否成立的仍是
> "`LoadLibraryA("CMSIS_DAP.dll")` 失败"（H1）是否属实，须先做 P1/P3 探测定论。
>
> ⚠️ **2026-10-01 更正（见 `Todo.md` §5 T2 结论 1~5）**：上面这句已被实测推翻 ——
> ① 表 1/表 2（`0x1003D030`/`0x1003D0F0`）**无条件安装**，`jb` 只跳过"caps→Read/Stream"判定；
> ② `StreamingTrace_*` 是 **SWO 流式传输**（sink 名 `cmsis_dap_swo_trace`），**不是 ETM 通路**；
> ③ 并行 / ETB 项在 Trace 页 **UI 层即不可选**（**非**"可见但选后被拒"）—— 可用项由 UV4 按
>    **调试器驱动类别**限定，类别取自 `DllUv3Cap(2)` 的**常量 `7`**（AGDI `0x10011D9B`），
>    **不经过 RDDI 层、不经过本层 caps**；AGDI 侧配置串构建亦只有
>    UART/Manchester × Read/Stream + Off 五种编码 → **Keil + CMSIS-DAP 内 ETM 不可达**；
> ④ `0x2029` 出自 RDDI 模块加载函数 `0x1002C1B0`（两臂共用），故 H1 基本可排除、优先按 H2 排查；
> ⑤ **`0x2024` 只在 SWO 两项与 caps 位 `0x04`/`0x08` 不匹配时出现**；改 caps 任何位都不能让
>    并行 / ETB 项变为可选（该判定发生在 UV4 侧且只用常量 `7`）—— 旧记述"选中并行/ETB 即 `0x2024`"作废。

### 17.8 ⭐ `DllUv3Cap`：µVision 的「驱动类别」判定入口（2026-10-01 新增）

问："Trace 页里哪些 Trace Port 项可用，是不是由本层上报的 caps 决定？" —— **不是**。证据如下。

- µVision 问驱动能力的**唯一入口**是 AGDI 导出 `DllUv3Cap`（`ord=13`，`0x10011D70`）。
  二进制实测：`cmp eax,0x6d` + 二级跳表（`0x10011ED8`）+ 字节表（`0x10011EF0`）分派 110 个
  `n`，**只有 4 个真实分支，返回值全是常量**：

| `n` | 分支 | 返回 |
|-----|------|------|
| `1` | `0x10011DB3` | `1`（状态缓存块） |
| **`2`** | **`0x10011D9B`** | **`7`**（`mov ecx,7`） |
| `100` | `0x10011DA8` | `1`（**不写**第二参数） |
| `110` | `0x10011E3A` | Flash Download 提示块 |
| 其余 | `0x10011ECF` | `0` |

- **该函数不读 caps、不读任何外部状态** → 能力位（能力上报）无法通过它影响 UI 可用项。
- `UV4.exe` 用 `DllUv3Cap(2)` 的返回值识别**调试器驱动类别**：调用点 `0x5F3E88`
  （`push 2` / `push 0`），6 路跳表 `0x5F3E99`（`jmp [eax*4+0x5F3F1C]`）逐一比对 magic
  `0x13927` / `0x1f73` / `0x2073` / `0x2173` / `0x1397b` / **`7`**；命中 `7`（AGDI /
  CMSIS-DAP 类）后 `0x5F3ED2` 调 `DllUv3Cap(100,&buf)`，并把其返回值原样返回。
- 由此：**AGDI / CMSIS-DAP 类驱动的「并行 Trace 端口」/「ETB」项在 UI 层即不可选**
  （详见 `Todo.md` §5 T2 结论 5）。本层哪怕改 `CMSIS_DAP_Capabilities` 的任何位，也
  **不可能**让这些项变为可选；caps 只影响 SWO 的 `Read`/`Stream`（`0x40`）与
  `UART`/`Manchester`（`0x04`/`0x08`）之选择。
- 复核命令：`tools/pe_re.py xref "D:\Keil_v5\UV4\UV4.exe" "DllUv3Cap"`（4 处 `GetProcAddress`）、
  `tools/pe_re.py dis "$AGDI" 0x10011D70 0x220`、`tools/pe_re.py dis "…\UV4.exe" 0x5F3E40 0x140`。
- 附注：`n=100` 分支**不写**第二参数，而 UV4 传入的是未初始化的栈缓冲（`[ebp-0x10524]`）；
  该路径 UV4 只取返回值、不读该缓冲（`0x5F3ED2`–`0x5F3EFE` 全程无读），故无副作用。
- ⚠️ **2026-10-03 精确化**：上面"类别取自 `DllUv3Cap(2)` 的常量 `7`"应读作 ——
  **UV4 记录的「驱动条目」所期待的自报值就是 `7`**。UV4 的**类别索引**来自它自己的驱动条目
  （`0x7189D0`，不读 magic），`DllUv3Cap(2)` 只做"条目类别 ↔ DLL 自报值"的**交叉校验**，
  **不带任何能力语义**；且 Trace 页（含 `Trace Port` 下拉）的**资源由 AGDI 提供**，UV4 里没有这些串。
  ⇒ 完整证据、三个边界与**三路线解法**见下节 **§17.9**。

### 17.9 ⭐ `DllUv3Cap` 的第二层真相：magic ≠ 类别；Trace 页的提供方是 AGDI（2026-10-03 新增）

**问**："既然 §17.8 说 Trace 页的可用项由 `DllUv3Cap(2)` 的常量 `7` 决定，那把它改成别的值、
或者 hook 掉，是不是就能放开「并行 Trace 端口」/「ETB」？" —— **不能**；§17.8 的表述也需按本节精确化。

#### (1) 硬证据 A：UV4 对 `DllUv3Cap(2)` 只有**一个**使用点，且它是"身份白名单"，不携带能力

`pe_re.py xref UV4.exe DllUv3Cap` 共 4 处字符串引用，其中 `0x5F3E73` 是唯一取 `n=2` 的地方
（另三处 `0x5264B2` / `0x526C17` / `0x52764A` / `0x5B6433` 走别路径）：

```asm
0x5F3E72  push 0xbd10c8            ; "DllUv3Cap"
0x5F3E77  push eax                 ; hMod
0x5F3E78  call [0xb29588]          ; GetProcAddress
0x5F3E84  push 0                   ; 参数 2（未初始化栈缓冲）
0x5F3E86  push 2                   ; n = 2
0x5F3E88  call esi                 ; ★ DllUv3Cap(2, 0)：全镜像唯一 n=2 调用点
0x5F3E8D  mov ecx, 0x2d46c40       ; UV4 全局配置对象
0x5F3E92  mov edx, eax             ; edx = AGDI 自报 magic
0x5F3E94  call 0x7189D0            ; eax = 「当前驱动类别索引」0..5（**该函数不读 edx**）
0x5F3E99  cmp eax, 5 / ja 0x5F3EFF ; 类别未知 → 放弃
0x5F3E9E  jmp [eax*4 + 0x5F3F1C]   ; 6 路跳表
```

跳表（`0x5F3F1C`，实测 6 项）与各分支白名单：

| 类别索引 | 分支 | 接受的自报 magic |
|---|---|---|
| 0 | `0x5F3EAD` | `0x1f73` / `0x2073` / `0x2173` |
| 1 | `0x5F3EC5` | `0x1397b` |
| 2 | `0x5F3EA5` | `0x13927` |
| 3 / 4 | `0x5F3ECD` | `7`（`cmp edx,7` / `jne 0x5F3EFF`） |
| 5 | `0x5F3ECC` | 跳表项落在指令中间 ⇒ 不可达/未定义 |

**所有命中分支都汇聚到 `0x5F3ED0 → 0x5F3ED2`**（再调 `DllUv3Cap(100,&buf)` 并原样返回）。
⇒ 这一段是"**UV4 记录的类别** ↔ **DLL 自报 magic**"的**交叉校验**（防换错 DLL），
返回什么 magic **不带能力语义**。

#### (2) 硬证据 B：「驱动类别索引」来自 UV4 自己的驱动条目，**不来自** `DllUv3Cap`

`0x7189D0`（全镜像 34 个调用点，通用）反汇编：

```asm
0x7189D0  mov eax,[ecx+0x9ab8]        ; 当前选中的「调试器驱动条目」
0x7189DA  mov eax,[eax+0x1e]          ; 其子记录
0x7189E1  movzx eax, word [eax+0x12]  ; ★ 类别编号（word）
0x7189E5  cmp eax,5 / ja → return 0x64 ; 未知类别
0x7189EA  movzx ecx, byte [eax+0x718a04] / jmp [ecx*4+0x718a00]  ; 二级映射
```

入参只有 `ecx`（`0x2d46c40`），**完全不读 edx（magic）**；另一处调用 `0x5F3D85` 拿它去
`[eax*4+0x2d4bb4c]` 取类别记录、再按 `word[+0x18]` 拼 `"%s%s\Debug\%s.dll"` / `Release`
—— 即**驱动 DLL 的目录/路径解析**。

⇒ 决定"这个驱动属于哪一类"的是 **UV4 的驱动条目数据**（内建表 / `TOOLS.INI` 侧）；
`DllUv3Cap(2)=7` 只证明"DLL 自称与条目类别配套"。

#### (3) 硬证据 C：Trace 页的**内容提供方是 AGDI**（UV4 里没有这些字符串）

AGDI `.rsrc` 里整套 Trace 页控件文本（宽串，实测区 `0x10366790`–`0x103669B4`）：

| 文本 | VA | 文本 | VA |
|---|---|---|---|
| `Trace Enable` | `0x10366790` | `Trace Port` | `0x103668C4` |
| `ETM Trace Enable` | `0x103667C0` | `SWO Clock Prescaler:` | `0x10366918` |
| `Trace Clock:` | `0x10366808` | `Autodetect` | `0x10366980` |
| `Use Core Clock` | `0x10366880` | `SWO Clock:` | `0x103669B4` |

同一批串在 **UV4.exe 中一处都没有**（`xref "Trace Port"` 未命中；UV4 的 `Parallel` 只在
"Parallel Build Configuration"，属编译页）⇒ µVision 的 Trace 页（含 `Trace Port` 下拉）
**由 AGDI 提供资源/页**，"哪些项可选"的最终决定权在 **AGDI + UV4 的类别限定**，两边都不过本层。

#### (4) 硬证据 D：本层（RDDI 层）**没有** ETM / 并行 Trace 接口

`include/ORBMDK_RDDI.h` 的 Trace 相关导出只有 `StreamingTrace_*`（8 个，SWO 流式，
sink 名 `cmsis_dap_swo_trace`）。**没有** ETM/ETB/并口采集 API
⇒ 即便 UI 放开，配置与数据也**没有既有通道**可走。

#### (5) 三个边界（定论，勿重复踩）

1. **只改 magic（含 hook `DllUv3Cap`）= 破坏交叉校验** ⇒ 驱动被 `0x5F3EFF` 拒；
   不是放开 UI 的抓手。
2. **只改 caps（本层 `CMSIS_DAP_Capabilities`）永远无效** —— 判定在 UV4/AGDI 内（§17.8）。
3. **放开 UI 而不建数据通路 = 看不到数据**（ETM 取数在 RDDI 层无接口，见 (4)）。

#### (6) 解决方法（三路线，按"代价 / 确定性"排序）

**路线 1（成本最低，先做）—— 进程内自改：本层 DLL 就是"注入器"**

- 前提事实：本层 `CMSIS_DAP.dll` 由 AGDI 在 **UV4 进程内** `LoadLibrary`（见 `src/ORBMDK_DLL.cpp`
  顶注"AGDI 在两次 rddi_Open 之间会卸载并重新加载本 DLL"）⇒ **无需外部注入器**：
  在本层 `DllMain`（ATTACH 后）用 `GetModuleHandle(NULL)` 拿基址，对 UV4 代码
  `VirtualProtect(PAGE_EXECUTE_READWRITE)` → 字节改写 / inline hook。
- 候选落点：

  | 目标 | 地址（本机 UV4） | 改法 | 效果 / 风险 |
  |---|---|---|---|
  | 类别查询（只此一处） | `0x5F3E94` 的 `call 0x7189D0` | 改跳到本层 stub，返回"支持 ETM 的类别"（0/1/2 之一） | 让 UV4 以为"这是 ULINKpro 类"；**风险**：同进程另 33 处 `0x7189D0` 仍返回原类别 ⇒ 不一致，需实测 |
  | magic 校验段 | `0x5F3EA5`–`0x5F3ED0` | 6 段 `cmp/jne` 全部 nop 成"直落 `0x5F3ED2`" | **只**解除身份校验，不放开 UI（单独做无意义，仅作配合） |
  | 驱动初始化 / 能力位 | `0x5F3F40`（读 `[esi+0x206]` 位）、`0x5F3F90` | 置 `0x206` 相应位、令 `[esi+0x21e]` 非空 | 这是 UV4 侧"是否存在 Trace 配置"的判定，需实测其与 UI 的耦合 |

- 验证：`pe_re.py dis "<UV4>" 0x5F3E80 0x120` 改前后比对 + 实机看 Trace 页。

**路线 2（路线 1 的决定性前提）—— 逆向 AGDI 的 Trace 页资源 ID 与回调**

- 从 `.rsrc` 中可见的控件 ID 入手（`0x44D`/`0x44E`/`0x451`/`0x456`/`0x457`/`0x458`/`0x459`/`0x578`/`0x579`，
  位于 `0x10366790` 一带的 DLGINIT 数据内），定位创建该页的 `CreateDialogParam` /
  属性页注册与 `WM_INITDIALOG` 中填充 `Trace Port` 下拉的代码。
- 复核：`pe_re.py hex "<AGDI>" 0x10366780 0x280`（控件 ID + 文本）。
  ⚠️ 资源经 `MAKEINTRESOURCE` 引用，**不会有绝对 VA 引用**（实测 `refs 0x103668C4` = 0 处），
  须按"立即数 `push imm32 == 控件 ID`"搜。

**路线 3（真正的工程量，仅确定要 ETM 时立项）—— 在本层新增 ETM / 并行 Trace 通路**

- 需在 RDDI 层实现 ETM 数据搬运（探针侧 TPIU/并口采集 + 缓冲 → AGDI 期望的 sink/回调），
  并把 AGDI Trace 页的配置请求映射过去。CMSIS-DAP 标准不覆盖此路径 ⇒
  必须先用**路线 2** 弄清 AGDI 期望什么接口/数据结构，否则无从对接。
- 不做路线 3 时，路线 1/2 的产出只是"**确认 UI 无法放开这一边界**"，不等于"ETM 能跑"。

#### (7) ⭐ 铁证 E：`Trace Port` 下拉（含 `Embedded Trace Buffer`）被 **AGDI 自己硬禁用**（2026-10-03 定论 / T0 关闭）

**T0 结论：UI 收紧发生在 AGDI，不在 UV4。** 依据（本机 `CMSIS_AGDI.dll` 实测）：

- 控件映射（`tools/_rsrc_scan.py` 解析 `DIALOG / DLGINIT 102`）：
  - `DIALOG 102` = µVision Trace 页（`style=0x80C800C0`，`cdit=93`）；控件 `0x456`（1110）类别
    序数 `133 = 0x85` ⇒ **COMBOBOX**，紧邻静态框 `Trace Port`（`0x103668C4`）⇒ **`0x456` = Trace Port 下拉**。
  - `DLGINIT 102`（`0x10367728`）给 `0x456` 填 **6 项**（`msg=0x403`）：
    `0`=Sync…1-bit / `1`=Sync…2-bit / `2`=Sync…4-bit / `3`=SWO-Manchester /
    `4`=SWO-UART/NRZ / **`5`=Embedded Trace Buffer**。⇒ **ETB 项静态就在表里**，不是"缺项"，
    而是"**整条下拉被灰**"。

- 代码侧（IAT 已核实：`0x101B56EC`=`GetDlgItem`、`0x101B57B4`=`EnableWindow`、`0x101B56BC`=`SendMessageA`）：

```asm
; —— 函数 A：0x1003B2A0（Trace 页可控态刷新；OnInitDialog 包装 0x1003BB00 及所有控件
;              处理器 0x1003BCA0/0x1003BCD0/0x1003BD60 均 tail-jmp 到此）
0x1003B39A  6A 00            push 0            ; ★★★ 硬编码；本页其它控件此处用 eax/edi
0x1003B39C  68 56 04 00 00   push 0x456        ; Trace Port 组合框
0x1003B3A1  8B CE            mov  ecx, esi
0x1003B3A3  E8 25 12 01 00   call GetDlgItem   ; (this, 0x456)
0x1003B3A8  8B C8            mov  ecx, eax
0x1003B3AA  E8 B0 11 01 00   call EnableWindow ; EnableWindow(combo, FALSE) ⇒ 灰

; —— 函数 B：0x1003B610（把设置灌进对话框；OnInitDialog 尾段调用）
0x1003BA6D  68 56 04 00 00   push 0x456
0x1003BA74  E8 54 0B 01 00   call GetDlgItem
0x1003BA7F  6A 00 6A 04 68 4E 01 00 00 FF 70 20 FF D7
                            ; SendMessageA(hwnd, CB_SETCURSEL(0x14E), 4, 0) ⇒ 强制选中 index 4
                            ;   (= “Serial Wire Output - UART/NRZ”)
0x1003BA8D  6A 00            push 0            ; ★ 再次硬禁用
0x1003BA8F  68 56 04 00 00   push 0x456
0x1003BA9D  E8 BD 0A 01 00   call EnableWindow ; EnableWindow(combo, FALSE)
```

- **穷举**：全镜像只有 4 处 `push 0x456` —— `0x1003B39C` / `0x1003BA8F`（两处 `EnableWindow(…,0)`）、
  `0x1003BA6D`（`CB_SETCURSEL`）、`0x1003BD61`（选择变更处理器里的 `CB_GETCURSEL`）。
  **没有**任何一处 `EnableWindow(0x456, TRUE)` ⇒ 该下拉**恒灰**，ETB（index 5）自然不可选。
- 选择变更处理器 `0x1003BD60`（`CB_GETCURSEL` → `cmp eax,5 / ja` → 跳表 `0x1003BE1C`）把 6 个
  index 映射到 `[0x102FA6D0]`：`0/1/2 → 0x100/0x200/0x800`（word，低字节仍为 0）、`3 → 1`、`4 → 2`、
  **`5 → 3`**，再 tail-jmp 函数 A 刷新。⇒ 选 ETB 的"下游"逻辑是完整的，只差"下拉能点开"。

**补丁（T1；两处 `6A 00` → `6A 01`）**：

| VA | 文件偏移 | 节 | 原字节 | 新字节 |
|---|---|---|---|---|
| `0x1003B39A` | `0x03A79A` | `.text` | `6A 00 68 56 04 00` | `6A 01 68 56 04 00` |
| `0x1003BA8D` | `0x03AE8D` | `.text` | `6A 00 68 56 04 00` | `6A 01 68 56 04 00` |

- 必须**两处同时**改：函数 A 与函数 B 在页面初始化 / 每次刷新都会执行（只改一处，下一次刷新会被重新灰掉）。
- 补丁后：下拉可点开、6 项齐全；选 ETB → `0x1003BD60` 写 `[0x102FA6D0]=3` 并刷新。
- ⚠️ 仍受 **(4)** 制约：**放开 UI ≠ 有数据通路** —— RDDI 层无 ETM/ETB 采集接口，选中后能否取到数据另说。
- 落地方式二选一：(a) 直接改盘上 DLL（**先备份** `CMSIS_AGDI.dll.bak`）；(b) 按**路线 1** 的进程内做法，
  由本层 `DllMain` 对 `GetModuleHandleA("CMSIS_AGDI.dll") + 0x3A79A / + 0x3AE8D` 两字节
  `VirtualProtect`→写 `01`（Keil 升级不丢失、不动官方文件，推荐作为长期解）。
- **已落地（2026-10-03，方式 (a)）**：本机 `D:\Keil_v5\ARM\BIN\CMSIS_AGDI.dll` 已打补丁，备份为
  `CMSIS_AGDI.dll.bak`；工具 `tools/patch_agdi_etb.py`（`--check` / `--apply` / `--revert`）。
  实机 UI 复验与"选 ETB 是否触发未实现调用"仍是 `Todo.md` §5 **T1** 的剩余项。

**对 §17.9(6) 的修正**：路线 1（改 UV4 类别 / 白名单）**单独做不足以**放开 `Trace Port` ——
即便 UV4 侧类别放行，AGDI 仍会把 `0x456` 灰掉。AGDI 这两行才是 T0 要抓的"UI 收紧点"。
（§17.8 关于 `DllUv3Cap` 的结论不受影响：那是 UV4 侧的驱动身份/类别校验，是**另一道**门。）

#### (8) ⭐ 实测反馈与定论（2026-10-03）：补丁生效，但 `Trace Enable` 仍灰；**ETB 是死路**

**现象**：打完 §17.9(7) 补丁并重启 µVision 后，Trace 页 `Trace Port` 下拉可点开、
`Embedded Trace Buffer` 可选；但 `Trace Enable`（控件 `0x44D`）**仍然是灰的**。

**(8.1) `Trace Enable` 为什么灰 —— 它不是硬编码，而是 AGDI 的四条件门控**

函数 A（`0x1003B2A0`）里 `0x44D` 的使能是**动态算出来的**（与 `0x456` 的硬编码 `push 0` 不同）：

```asm
; @@ 0x1003B2A0  Trace 页可控态刷新
0x1003B2A6  cmp  byte  ptr [0x1021BDA5], 0    ; 条件①
0x1003B2B2  je   0x1003B2D6                    ;   └ 不满足 → 禁
0x1003B2B4  cmp  dword ptr [0x102F8B94], 0    ; 条件②
0x1003B2BB  jne  0x1003B2D6                    ;   └ ≠0 → 禁
0x1003B2BD  cmp  dword ptr [0x102FA574], 1    ; 条件③ = SWO 槽（CMSIS_DAP_SWO_*）绑定成功
0x1003B2C4  jne  0x1003B2D6                    ;   └ 未绑定 → 禁
0x1003B2C6  test byte  ptr [0x102F8B90], 4    ; 条件④ = caps.BIT2（INFO_CAPS_SWO_UART）
0x1003B2CD  je   0x1003B2D6                    ;   └ 未置 → 禁
0x1003B2CF  mov  eax, 1
0x1003B2D6  xor  eax, eax
0x1003B2D8  push eax / push 0x44D / mov ecx,esi / call GetDlgItem / call EnableWindow
```

同页第二处（`0x1003B303`）把 `0x44D` 的**勾选态**按 `[0x102FA6C8]&1 && [0x102FA574]==1 && caps&4`
同步（`CheckDlgButton`）。

**四条逐一核对**（本层侧已查）：

| 条件 | 全局 | 本层现状 | 结论 |
|---|---|---|---|
| ① | `[0x1021BDA5]` | AGDI 内部状态（写入点在 `0x1001103A/0x10011DB6/0x10011DF4/0x10011E3D/0x10011EB8`），非本层可控 | 待运行时确认 |
| ② | `[0x102F8B94]` | caps 结构第二 dword，`CMSIS_DAP_Capabilities` 只写 4 字节，AGDI 侧清零 | 应为 0（通过） |
| ③ | `[0x102FA574]` | 本层已导出 `CMSIS_DAP_SWO_Baudrate/Control/Data/Status`（ord 36–39） | 通过 |
| ④ | `caps & 0x04` | **`CMSIS_DAP_Capabilities` 按 `probeCaps` 收口；探针自报无 0x04/0x08 → 本层不置** | **失败** |

⇒ **灰的直接原因就是条件④**：不是 AGDI 硬禁用，而是**探针自报的 `DAP_Info(0xF0)` 里没有 SWO 位**，
本层按"只宣称已验证能力"的口径如实转达（`src/ORBMDK_RDDI.cpp:2951-2973`）。

> 注意 `[0x102FA6C8]` bit0 = UI 勾选态；`0x100229FD test byte [0x102FA6C8], 1 / je → Trace=Off…`。
> 所以即便把 `0x44D` 强行点亮成可勾，**条件④不满足时勾选也不产生任何效果**（写出的仍是 `Trace=Off`）。

**(8.2) ETB 是死路 —— 编码器根本不接受 port=3**

配置串编码块（`0x100229F0` 起）只认 UART(2) / Manchester(1)：

```asm
0x100229FD  test byte [0x102FA6C8], 1
0x10022A04  je   0x10022A77                  ; Trace Enable 未勾 → "Trace=Off;TraceBaudrate=0;TraceTransport=None;"
0x10022A06  movzx eax, byte [0x102FA6D0]     ; Trace Port 取值
0x10022A0D  sub  eax, 1
0x10022A10  je   0x10022A53                  ; ==1 Manchester → test caps,0x08
0x10022A12  sub  eax, 1
0x10022A15  je   0x10022A2F                  ; ==2 UART       → test caps,0x04
0x10022A17  mov  eax, 0x2024                 ; ★ 其余一切（含 3=ETB、0x100/0x200/0x800 并口）→ 不支持的配置
```

而 AGDI 内建的配置串模板**只有 5 个**（`0x101EB3DC / 0x101EB400 / 0x101EB428 / 0x101EB454 / 0x101EB480`）：

```
Trace=SWO-UART;TraceTransport=Read;         Trace=SWO-UART;TraceTransport=Stream;
Trace=SWO-Manchester;TraceTransport=Read;   Trace=SWO-Manchester;TraceTransport=Stream;
Trace=Off;TraceBaudrate=0;TraceTransport=None;
```

**没有任何 ETB / ETM / 并口 Trace 的表达形式** ⇒ 选中 ETB 后唯一可能的结果就是 `0x2024`（不支持配置）。

⇒ **结论**：`Embedded Trace Buffer` 在 AGDI 这套实现里**不可能真正工作**。
§17.9(7) 的补丁只让"UI 能选"，功能上零收益（反而容易误判）。
§17.9(4) 的判断由"看不到数据"升级为"**根本没有通往 ETB 的代码路径**"。

> 附注：`0x1003BD60` 的跳表把 6 个下拉项映到 `[0x102FA6D0]` = `0x100/0x200/0x800/1/2/3`
> （并口三项 / Manchester / UART / **ETB=3**）；编码器取的是**低字节**，
> 所以并口三项也一并落到 `0x2024`。

**(8.3) 由此确定的可行动作**

- `Trace Enable`（=SWO）这条路本身是通的，卡点只在**条件④（caps 位）**：
  要么让探针自报 `0x04/0x08`，要么**本层不再按 `probeCaps` 收口**（= 主动宣称 SWO 能力）。
  这是一次**口径决策**，不是 bug —— 见 `Todo.md` §5 T1。
- ETB / ETM 方向建议**结案**：不是 RDDI 层能补的洞，AGDI 侧连配置串模板都不存在。

复核命令：
`python tools/pe_re.py dis "<KeilRoot>\ARM\BIN\CMSIS_AGDI.dll" 0x1003B2A0 0x100`（`0x44D` 四条件门控）
`python tools/pe_re.py dis "<KeilRoot>\ARM\BIN\CMSIS_AGDI.dll" 0x100229F0 0xB0`（编码器只认 1/2，其余 `0x2024`）
`python tools/pe_re.py hex "<KeilRoot>\ARM\BIN\CMSIS_AGDI.dll" 0x101EB3C0 0xE0`（5 个配置串模板）

复核命令：
`python tools/_rsrc_scan.py "<KeilRoot>\ARM\BIN\CMSIS_AGDI.dll" --quiet --dialog 102`
`python tools/pe_re.py off "<KeilRoot>\ARM\BIN\CMSIS_AGDI.dll" 0x1003B39A 6`（→ `file 0x03A79A`）
`python tools/pe_re.py dis "<KeilRoot>\ARM\BIN\CMSIS_AGDI.dll" 0x1003B290 0x160`
`python tools/pe_re.py dis "<KeilRoot>\ARM\BIN\CMSIS_AGDI.dll" 0x1003BD60 0x150`

> 复核命令（本节结论全部可复现）：
> `python tools/pe_re.py xref "<KeilRoot>\UV4\UV4.exe" DllUv3Cap`
> `python tools/pe_re.py dis "<KeilRoot>\UV4\UV4.exe" 0x5F3E40 0x140` / `0x7189D0 0x60` / `0x5F3F40 0x140`
> `python tools/pe_re.py dis "<KeilRoot>\ARM\BIN\CMSIS_AGDI.dll" 0x10011D70 0x220`
> `python tools/pe_re.py exports "<KeilRoot>\ARM\BIN\CMSIS_AGDI.dll"`（15 个导出，`DllUv3Cap` ord=13）

---

## 十八、已完成事项清单（2026-09-30 复核）

> 本节只保留**已关闭**的条目（编号沿用原文，故不连续）。
> 未解决项（原 §18.1 / §18.5 / §18.6 / §18.7 及 §18.2 的遗留观察）已迁出至 `Todo.md`
> （自 2026-10-03 起 `Todo.md` 只列未完成项；原 §18 体系全文留档在 `Todo.md.bak`）。
> 二轮补充迁出（2026-10-01）：正文中散落的待办 —— §五（stub 清单）、§7.3（符号路径）、
> §17.7（ETM 立项）、§4.12 两条注、§13.5 的剩余失败、§18.9 第一步 #6 —— 亦已一并迁出，
> 各处只留指针（现行落点：`Todo.md` §18.5 / §18.11 / §18.12 / §18.13 / §18.14，
> 以及 `Todo.md.bak` §18.7b / §18.10-B / §18.10-C）。

### 18.2 ✅ 块传输提速验证 —— 2026-09-30 完成

`bin\ORBMDK_BlockTransferTest.exe 0x20000000`：**12/12 通过、哨兵零损坏、加速比 7.86×**
（§9.2 有完整数据）。过程中修好了验证程序本身缺的四步（低层 `DAP_Connect` 是纯桩、
调用顺序、DP/AP 上电、目标运行时要先停核）。

> 2026-10-01 补充：本次验证只覆盖 **SWD**。曾尝试把块传输扩展到 **JTAG**，实测该命令会让
> 探针固件挂死（须重新插拔 USB），当日即**撤销并在 JTAG 下永久禁用**（JTAG 全程走本层
> 逐字传输）—— 见 **§18.9 第十步**。V1(HID) 与 V2(Bulk) 两条传输层继续支持块传输。

### 18.3 ✅ 日志模块统一（§8）—— 2026-09-30 已完成

单实现 `src/ORBMDK_Log.cpp` + 头文件宏；两套旧实现、四处"直写文件"全部并入；
`RDDI_SetLogCallback` 通道接通；HID/BULK 命令级日志补上时间戳。落地记录见 **§8.1**。

仍待办的小项：**会话分隔标记** —— ✅ 同日一并完成：`RDDI_Open` 入口打一条
INFO 级分隔行（`================ 会话 #N 开始 (pid=...) ================`），
便于在"跨进程/跨会话追加"的日志里快速定位（§4.10 的遗留项）。

### 18.4 ✅ 状态 LED 实机确认（§11.2）—— 2026-10-01 完成

`SetHostLed` / `TrackApTarWrite` 实机核对通过：接上目标后 Connect LED 常亮，
烧录/运行时 Running LED 亮、停机灭，与 §11.2 的预期一致。

### 18.9 ✅⭐ JTAG 通路（2026-09-30 定位并打通 —— 结论见**第九步**；2026-10-01 JTAG 侧块传输禁用，见**第十步**）

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
| 6 | `INFO_CAPS_JTAG` | **暂不加**——能力位只宣称已验证能力；JTAG 已于第九步实测打通。"是否对外宣称"这一决策 → 已迁至 **`Todo.md` §18.13**（Keil 的 Port 下拉框不受该位限制，照样能选） |

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

⇒ **探针/线缆/TDO/目标 JTAG 全部正常**；此前"引脚级判定"里"TDO 无驱动""connect-under-reset 无效"
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

**对前文结论的修正**：此前"SWJ-DP 只能 JTAG→SWD、必须给目标**断电**才能回到 JTAG"与本次实测
**不符** —— 本次 JTAG 成功前同一上电周期内发生过 SWD，且 `JtagInitSequence` 里的
**SWD→JTAG 切换序列**（线复位 64×1 + 16 位 `0xE73C`）实测**有效**，无需断电。结合第八步可知：
早前"扫链为空 / TDO 恒 1"的真因是**帧格式错 + 上面①的移位错位**，而非 SWJ-DP 锁死。

**改动文件**：`src/ORBMDK_RDDI.cpp`（`JtagSetIr` / `JtagDrScan` / `JtagInitSequence`）；
`test/jtagprobe.cpp`（新增"写 `DP SELECT` 后读 AP IDR"的端到端校验）。

#### 第十步（2026-10-01）：JTAG 侧放开固件块传输 —— ❌ 实测把探针挂死，已撤销

**结论（先看这个）**：`ID_DAP_TRANSFER_BLOCK` **不能在 JTAG 模式下使用**。orbtrace 固件收到
该命令后**不回应答，并停止服务 OUT 端点**，探针必须**重新插拔 USB** 才能恢复。本层因此改为
**在 JTAG 模式下永久禁用块传输**（`EnsureBlockTransferProbed()` 直接判为不可用、**连探测都
不发**），JTAG 全程走本层逐字引擎（`JtagSetIr` + `JtagDrScan`）。**SWD 侧不受影响**（§9.2 /
§18.2 的块传输结论继续有效）。

**当时的判断（错在哪）**：认为建链时已把链信息（IR 长度表）经 `DAP_JTAG_Configure` 下发给
固件，而 gateware 的 `jtagIF.v` 又实现了 `JTAG_CMD_IR` / `JTAG_CMD_TFR`，于是推断固件"自己
就能选 IR + 组 35 位 DR"，`ID_DAP_TRANSFER_BLOCK` 在 JTAG 下应当可用。
**"gateware 里有这两个底层命令" ≠ "块传输命令在 JTAG 下可用"** —— 这一步推断跳得太大。

**现场证据**（`%TEMP%\ORBMDK_RDDI.log`，V2 Bulk + JTAG，会话 #84）：

| # | 日志 | 含义 |
|---|------|------|
| 1 | `JtagInitSequence: 扫链成功 count=2 DP@0(IR=4) ... ID[0]=0x4BA00477` | JTAG 建链本身**正常**，第九步成果无回归 |
| 2 | `bulkRead IN ep=0x85 maxLen=508 TIMEOUT after 5000 ms` → `cmd=0x06 read failed (-2)` | **块传输命令（cmd=0x06）发出后设备完全无应答**（5 s 读超时）|
| 3 | 随后 `cmd=0x02`(Connect) / `cmd=0x03`(Disconnect) / `cmd=0x01` / `cmd=0x00`(Info) 全部 `bulkWrite OUT ep=0x03 TIMEOUT (pipe reset)` | **连 DAP_Info / DAP_Connect 都写不进去了** —— 设备已挂死 |
| 4 | 后续会话 #85 / #86：`calibrate: device reports packet size = 0` → `no packet size verified, falling back to 64` | 重开句柄（`rddi_Open`）也救不回来，只能重新插拔 |

**为什么"探测失败自动回退"没兜住**：回退机制假设"命令不被支持 → 返回错误或超时"。这里是
**设备本身停止工作** —— 回退发生时设备已经躺平，当前会话连同之后的所有会话（包括纯 SWD）
一起废掉。所以 JTAG 侧只能**事前禁用**，不能事后回退。

**撤销范围**（`src/ORBMDK_RDDI.cpp`）：`EnsureBlockTransferProbed()` 恢复 JTAG 早退；
删除为其准备的 `JtagForgetAfterBlockTransfer()` 及 `DAP_RegWriteRepeat` / `DAP_RegReadRepeat`
中的 JTAG 分支；`DAP_RegWriteRepeat()` 的 `JtagDapFlush()` 恢复"每个 chunk 后一次"。
**保留**：`Port=` 切换时重置 `blockTransferProbed` / `blockTransferSupported` / `jtagCurIr` /
`jtagWritePending`（换模式后能力结论本就不该复用，这一条与块传输能否用无关，仍然正确）。

**教训**：对"会改变探针固件运行状态"的命令，**不能**用"先试一下、失败再退"的策略 ——
必须先单独确认固件支持，再放开；尤其要警惕"会挂死设备、需人工插拔"这一类失败模式，
它的代价是整条调试链路而不是一次操作。

> **⚠️ 2026-10-03 复核更正（定性部分推翻；上面第 2 条表里的现场证据仍然成立）**
>
> 上面"当时的判断（错在哪）"把原因写成"固件在 JTAG 下行没有这条实现路径"，**与源码不符**：
> orbtrace-1.4.3 的门级固件**明确实现**了 JTAG 下的块传输 —— `cmsis_dap.py:766-903`
> （`RESP_TransferBlock_Setup/Process` 带 `isJTAG` 分支、`readBDelay = isJTAG & rnw`，
> 实现清单 `# DAP_TransferBlock : Done` 无 SWD-only 限定）、`dbgIF.v:480-502`（`CMD_TRANSACT`
> 的 `MODE_JTAG` 路径）、`jtagIF.v:142-273`（35 位 DPACC/APACC + 多 TAP bypass）。
> **真不支持会立刻回 `0xFF`（`RESP_Not_Implemented`）**，而不是"无声 + 停止服务 OUT 端点"。
>
> ⇒ 本步**现象**（#84 日志、须插拔）**仍然成立**，但准确定性应为：**本层主动禁用，根因未定性**
> （代码上可指认的卡死候选：`irlenx` 链信息未下发/不符、JTAG 时钟未跑、JTAG 延迟读计数与本层
> 严格校验冲突）。**完整证据、候选与判定方法见 `bug.md` B9**；复验工具
> **`test/jtagblockprobe.cpp`**（每步 0x06 之后 Ping 一次，把结果分成"可用 / 未处理但存活 / 挂死"三类）。

---

## 十九、编译器兼容性审查（仅限 Windows，2026-10-03）

> **审查方式**：**纯静态审查** —— 未改动任何源码，也未引入第二编译器做实测编译。
> **范围限定 Windows**：考察对象 = MSVC `cl.exe`（现状）、`clang-cl`、MinGW-w64 GCC、MSVC 旧版本。
> 本节只回答"代码对编译器/编译开关的依赖面"，不涉及运行时行为与逻辑缺陷。

### 19.1 结论

| 编译器 | 结论 | 主要障碍 |
|--------|------|----------|
| MSVC `cl.exe` 2017+（x86） | ✅ 唯一受支持组合（现状） | — |
| `clang-cl` | ⚠️ 理论可行（同样用 MSVC CRT / SDK / `__declspec`），**未实测** | §19.2 C3 / C4 |
| MSVC ≤ VS2013 | ❌ | §19.3 V1–V3（`snprintf` / `%zu` / `%llu` 需 UCRT） |
| MinGW-w64 GCC | ❌ | §19.2 C1 / C2 / C4 |
| 其它平台编译器 | ❌（超出本文范围） | `__declspec`、`<Windows.h>` 等 |

一句话：**本层与 MSVC / UCRT 强绑定**（Annex K 安全 CRT + `#pragma comment(lib)`）。这在"只跑 Keil、只用 `build.ps1`"的前提下不是缺陷；换编译器时的改造点见 §19.7。

### 19.2 MSVC 锁定项（换 Windows 编译器即编译失败）

| # | 级别 | 位置 | 内容 | 影响 |
|---|------|------|------|------|
| C1 | 🔴 | `include/ORBMDK.h:14-17, 75, 77, 84` | **公共 C ABI 头的 `static inline` 里直接调 `sscanf_s` / `strncpy_s` / `sprintf_s` / `_TRUNCATE`** | 把整个项目绑死在 UCRT：非 MSVC 的 Windows 编译器一包含此头就报未声明标识符 |
| C2 | 🟡 | 48 处：`ORBMDK_HID.cpp:561,568,734,735,742,766,2190,2197`；`ORBMDK_USB_Bulk.cpp:308,314,320,322,381,513,533,1816,1842`；`ORBMDK_RDDI.cpp:808,866,868,1960,2039,2050,2061,2065,2069,2079,2087,2089,3586,3588,3590,3608,3610`；`ORBMDK_Symbols.cpp:1100,1110`；`ORBMDK_Log.cpp:83`；`test/*` | `*_s` 安全 CRT + `_stricmp` / `_strnicmp` + `fopen_s` | MinGW-w64 不提供 Annex K → 编译失败 |
| C3 | 🟡 | `src/pch.h:20-22` | `INITGUID` / `COBJMACROS` / `UMDF_USING_IOCTL_DEFINE_GUIDS` 全局开启 | `INITGUID` 使每个 TU 实例化 GUID，靠 MSVC `__declspec(selectany)` 去重；GCC 侧 `selectany` 语义不一致 |
| C4 | 🟡 | `src/ORBMDK_HID.cpp:20-21`、`src/ORBMDK_USB_Bulk.cpp:24,30,36`、`src/ORBMDK_Symbols.cpp:876`、`test/{v2rawprobe,jtagrawprobe,ifacedump}.cpp` | `#pragma comment(lib, …)` ×6 | GCC 只警告"忽略"→ 需手工 `-lsetupapi -lwinusb -lhid -luuid -lshlwapi`；clang-cl 有效 |
| C5 | 🟡 | `include/ORBMDK.h:19-20`、`include/ORBMDK_RDDI.h:17-19` | `__declspec(dllexport/dllimport)` 无 `_MSC_VER` 兜底 | Windows 各家都支持 `__declspec`，风险低；属"无条件使用扩展"的写法 |
| C6 | 🟡 | `src/pch.h:17` | `_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS` | MSVC 专有宏，其它编译器忽略（无害） |

### 19.3 工具集版本门槛（同一编译器、不同版本）

| # | 位置 | 依赖 | 最低要求 |
|---|------|------|----------|
| V1 | `src/ORBMDK_Log.cpp:60, 174, 191` | `snprintf` / `vsnprintf`（C99） | VS2015+（VS2013 无） |
| V2 | `src/ORBMDK_HID.cpp:270,278,279,329,360,369,444,1025,1081,1092,1106,1121,1453`、`src/ORBMDK_USB_Bulk.cpp:1334` | `%zu` | UCRT，VS2015+ |
| V3 | `src/ORBMDK_RDDI.cpp:787,898,906,2460,2469,3464,3471,3476`、`src/ORBMDK_Coverage.cpp:648-915`、`test/` 5 个文件 | `%llu`（**均配 `(unsigned long long)` 转换，类型正确**） | VS2015+ |
| V4 | `src/pch.h`（`<shared_mutex>`/`<thread>`/`<atomic>`）、`ORBMDK_HID.cpp:122,1924`、`ORBMDK_USB_Bulk.cpp:107,1719` | `std::shared_mutex` / `std::thread` | VS2015 Update 2+ |
| V5 | `include/ORBMDK_ITM_Decoder.h:10-11`、`ORBMDK_ETM_Decoder.h:10-11`、`ORBMDK_TPIU_Decoder.h:10-11` | `<cstdbool>` | C++17 已弃用、**C++20 被移除**；现 `/std:c++17` 安全，改 `c++20` 需先换掉 |

> 两个构建脚本都声明 VS2017+，V1–V4 全部满足。真正的风险点是 **V5**：把 `/std:c++17` 改成 `c++20` 会先在这里爆。

### 19.4 构建开关依赖（不加也能编，但结果不对或刷警告）

| # | 开关 | 位置 | 不加会怎样 |
|---|------|------|-----------|
| F1 | `/utf-8` | `build.ps1:142`、`test/build_test.ps1:146` | 源码含大量中文注释/字符串；中文代码页下 MSVC 报 **C4819** 且日志串乱码 —— 最容易被"换个 IDE / 换个脚本"弄丢的开关 |
| F2 | `/D_CRT_SECURE_NO_WARNINGS` | 仅在 `build.ps1:145`；**源码内未定义** | `fopen`/`strcpy`/`strcat`/`sscanf`（`ORBMDK_Symbols.cpp:212,410,805,806,955`、`ORBMDK_Coverage.cpp:636,701,805`）报 C4996；若再加 `/WX` 直接失败 |
| F3 | `/EHsc` | 两个脚本 | 异常语义不定 |
| F4 | `/MD`（测试 exe）vs `/MT`（DLL） | `build_test.ps1:146` vs `build.ps1:142` | 当前安全（纯 C ABI、缓冲区由调用方给）；**不可跨边界传堆指针/STL 对象** |

### 19.5 低危项（当前不触发，改动后才触发）

- `test/*.cpp` 均直接 `#include <windows.h>`（`swdprobe:25`、`hidprobe:8`、`v2rawprobe:14`、`jtagprobe:19`、`jtagrawprobe:22`、`ifacedump:12`、`ORBMDK_RDDI_FullTest:6`、`ORBMDK_BlockTransferTest:50`），但 **test 目录 0 处 `NOMINMAX`**。当前未用 `std::min/max`，故无事；一旦引入 `<algorithm>` 就会撞 Windows 的 `min/max` 宏。
- `src/ORBMDK_Symbols.cpp:874` 在文件中途再 `#include <windows.h>`（pch 已引过）—— 冗余但无害。

### 19.6 已核对"无坑"项（可放心）

- **无模板定义** → MSVC 宽松模式与 `/permissive-` 的两阶段名字查找差异**无从触发**（最大的一项好消息）。
- **无 `__cplusplus` 数值判断**（只有 `#ifdef __cplusplus`，13 个头 ×2）→ 不依赖 `/Zc:__cplusplus`，MSVC 报 `199711L` 也无影响。
- 无 `__int64` / `__pragma` / `for each` / `typeof` / `__attribute__` / VLA / 匿名 struct 等 MSVC/GNU 专有语法。
- 14 个 `src/*.cpp` **全部**以 `#include "pch.h"` 开头（逐个核对）。
- `include/*.h` 中无 STL（`std::` 命中 0 处）→ 公共头保持纯 C ABI；`ORBMDK_Log.h` 零 include 亦自洽（只用 `int` / `char*` / `void*`）。
- 无非 const 字符串字面量赋值（4 处命中全是 `const char*`）→ `/Zc:strictStrings`、`-Wwrite-strings` 安全。
- 无指针截断式强转（唯一 `(int)` 命中 `ORBMDK_RDDI.cpp:2636` 是显式 `(int)(intptr_t)` 用于打印）→ 将来编 x64 也不截断。
- 无 POSIX 头（`unistd.h` / `sys/*`）与 `_open` / `_access` 系列。
- 无 `#pragma warning(disable)` → 没有靠压制警告掩盖问题。

### 19.7 若将来要支持其它 Windows 编译器

改造点集中在 **C1 / C2**（约 50 处调用，可收敛到一层 `*_s` → 标准函数 + 边界检查的适配头）与 **C4**（`#pragma comment(lib)` 换成链接器参数，两个脚本本就在命令行传 lib）。V5 在切到 C++20 时需先替换 `<cstdbool>`。以上均为**结论性定位**，未实施。

### 19.8 可选的进一步验证（本次未做）

| 手段 | 能验证什么 | 代价 |
|------|-----------|------|
| 加 `/permissive-` 试编一遍（只加开关、产物丢临时目录） | 标准符合性（预期零错误，因为无模板） | 一次编译 |
| 用 `clang-cl` 试编 | MSVC 宽松模式差异 + GNU 语法差异（Windows 下最现实的第二编译器） | 需装 LLVM |
| 在 MinGW 下试编 | 验证 §19.2 的 C1/C2 判断 | 需装 MinGW-w64 |

---

## 二十、能力探测的"深度"与一处固件副作用（2026-10-03）

> **背景**：`ProbeBlockTransfer` 原来只发 **1 个字**（读一次 DP IDCODE），`ProbeTransferMulti`
> 只发 **2 笔读**；而这两条路径实际被用到的深度远不止于此 —— 块传输 V2 下是 **125/126 字/往返**
> （见 `BlockWordsLimit()`），多笔可达上百笔**且含写**。探测太浅的后果是：固件"浅层能过、
> 深层静默出错"时**无法回退**，只能把错误数据交出去。

### 20.1 已落地：块传输探测改为"按真实工作深度深读"

`src/ORBMDK_RDDI.cpp` 的 `ProbeBlockTransfer` 改成**两段式**：

1. 单字基线（读 DP IDCODE）；
2. 按 `BlockWordsLimit()`（orbtrace 标定 508 → **125 字**；HSLinkPro 511 → 126 字；V1/HID → 14 字）
   再做一次多字块读，并**逐字校验** n 个字是否都等于该 IDCODE。

压力载荷选 **DP IDCODE**：DP 读不自增、无副作用，n 次读恒回同一值，所以"逐字相等"是硬判据。
日志变为 `Block transfer probe OK (… idcode=0x…, deep N-word read verified)`；失败则按既有约定
**永久回退逐字**（只影响性能，不影响功能）。

### 20.2 ★ 固件副作用：在 `flushBatch` 中途插"写笔"会破坏后续 AP 访存

**现象**：给 `ProbeTransferMulti` 加了一段"批量内写"校验（读 DP SELECT → **同值写回** → 读回自比）后，
orbtrace 上 `swdprobe` 从 **19/0** 变成 **14/5**，失败形态高度一致：

| 读回项 | 值 | 备注 |
|--------|----|------|
| `DP CTRL/STAT` | `0xF0000040` | 正常（调试电源已上） |
| `AP CSW` 写 → 读回 | 一致 | **AP 寄存器访问正常** |
| `SCB->CPUID` | **`0x00000000`** | AP **存储器**访问 |
| `DHCSR` | **`0x00000000`** | |
| `RAM 0x20000000` 写后读回 | **`0x00000000`** | |

⇒ 形态 = **"AP 寄存器能访问、但 AP 存储器 / 核心寄存器全部读回 0"**。

**定位（A/B 实测，同一 orbtrace、同一目标、同一测试）**：

| 被测 DLL | swdprobe 结果 |
|-----------|---------------|
| 加固前（旧的浅探测） | **19/0** |
| 只加 §20.1 块深读、**不加**批量内写校验 | **19/0** |
| 加了批量内写校验 | **14/5** |

旁证：`ORBMDK_BlockTransferTest` 的会话**未触发多笔探测**（只触发块深读），其 RAM **逐值**写读 **6/0 全过**。

**结论**：破坏出在"在 `DAP_RegAccessBlock` 的 `flushBatch` **中途**额外插入写笔"这一动作上 ——
此刻 DP/AP 的选择与 **posted-read 流水线正处于"半途"状态**，插写笔（尤其碰 **DP SELECT**）会把它带乱。
**机理未查清**（属固件侧，本层只能选择不这么做）。⚠ 本条是 **orbtrace 实测**结论，**HSLinkPro 未验**。

**处置**：该写校验**已撤回**，并在 `ProbeTransferMulti` 处留了警示注释 —— **不要**再往那一处塞写笔；
要补写语义须换载体（**不要碰 DP SELECT**）与时机，并先在 orbtrace 上做同样的实机回归。

### 20.3 本结论的复现 / 回归手段

- `bin\swdprobe.exe ORBMDK_RDDI.dll` → 期望 **19 通过 / 0 失败**（orbtrace；DPIDR `0x2BA01477`）。
- `bin\ORBMDK_BlockTransferTest.exe` → 期望 **6 通过 / 0 失败**。
- `%TEMP%\ORBMDK_RDDI.log`（`ORBMDK_LOG_LEVEL` = `1`）：两条探测行齐全，且 **0 WARN / 0 ERROR**。

---

## 二十一、RAM 读写速率实测（绕开 Keil，2026-10-03）

> **目的**：回答"**设备侧**（探针 + 目标 + 本 DLL）到底能跑多快"。Keil/AGDI 站在用户与本 DLL
> 之间，还自带每次调用的开销与交错的寄存器访问，所以**任何 Keil 会话的数字都不能代表设备极限**。
>
> **工具**：`test\ORBMDK_RAM_SpeedTest.cpp` → `bin\ORBMDK_RAM_SpeedTest.exe`
> （已挂进 `test\build_test.ps1 -All`；用法
> `ORBMDK_RAM_SpeedTest.exe [ramAddr] [bytes] [rounds] [clockHz] [dllPath]`）
>
> **方法**：直接调 `DAP_RegWriteRepeat` / `DAP_RegReadRepeat`（= 烧录热路径），链路里只有本 DLL；
> 顺序为 **停核 → 尺寸阶梯 → 稳态 N 轮 → 单笔基线**（单笔 = 1 字/次调用 = 1 次 USB 往返）。
> 逐值校验 + 首尾哨兵查越界；速率口径 `1 kB = 1024 B`，与 DLL 的 TESTSPEED 一致，可直接对比。

### 21.1 实测（orbtrace / 目标 Cortex-M4（STM32F407RET6）/ 4 KB 缓冲 / 4~8 轮）

> **笔误更正**：本节标题原写“目标 Cortex-M0+”。实测 CPUID `0x410FC241`，PartNo（bit[15:4]）`0xC24`
> = **Cortex-M4** r0p1，与 **STM32F407RET6**（192 KB SRAM）一致；DP IDCODE `0x2BA01477` 亦相符。
> 下表数据采自同一块板、同一台探针。
> **后续**：4 KB 缓冲**正好等于该链的 TAR 自增块**（见 §21.5 与 `bug.md` B7）；改用 **32 KB** 重测的
> 完整 1→100 MHz 步进曲线见 **§21.5**。

| SWD 时钟 | 写 best | 读 best | 块传输 vs 单笔 |
|---|---|---|---|
| 1 MHz | 74.3 kB/s | 78.5 kB/s | 2.6× / 2.9× |
| **10 MHz** | **582.1 kB/s** | **631.5 kB/s** | **19.5× / 25.0×** |

**尺寸阶梯**（10 MHz，写 kB/s）：16 字（64 B）46.8 → 64 字 475.6 → 256 字 429.6 → **1024 字 595.7**。
**单笔基线**：**130~155 µs/字**，即一次 USB 往返的延迟 —— 这是"逐命令一问一答"的地板。

### 21.2 结论

1. **瓶颈是 SWD 时钟，不是 USB**：1 → 10 MHz 提速 **7.9×**。
   500 B 一块在 10 MHz 上"光移位"就要 `4000 bit / 10 MHz = 400 µs`，实测每块 ≈700 µs
   ⇒ **约一半是 SWD 移位、一半是 USB 往返 + 固定开销** ⇒ **继续提高时钟仍有直接收益**
   （上限由信号完整性决定；时钟margin不足会**静默**出错，本工具会逐值抓到，Keil 不会）。
2. **与 Keil 烧录吻合**：10 MHz 下 582/631 kB/s ≈ Keil 烧录实测 565 kB/s
   ⇒ **AGDI/µVision 自身几乎没有拖累**，"Keil 慢"就是这条路径的真实极限。
3. **带宽远未成为瓶颈**：631 kB/s ≈ 5 Mbps ≈ **USB 2.0 HS 480 Mbps 的 1%**。
4. **小缓冲被固定开销吃满**：16 字只有 46.8 kB/s（每次调用的固定开销占绝大部分）。

### 21.3 顺带修掉的两个自测 bug

| # | 文件 | 现象 | 根因 | 修法 |
|---|------|------|------|------|
| 1 | `ORBMDK_RAM_SpeedTest.cpp`、`ORBMDK_BlockTransferTest.cpp` | 停核后读回 `DHCSR = 0x00000000`，误报 `[NOT halted]` | 写 DHCSR 后 CSW 的 `AddrInc=single` 已把 TAR 自增到 `DHCSR+4`，直接读 DRW 读到的是 DCRSR | 读之前**重新 arm TAR**，即得 `0x01030003 [halted]` |
| 2 | `ORBMDK_RAM_SpeedTest.cpp` | 尺寸阶梯假报"哨兵越界" | 把大缓冲"切片"当小缓冲用，尾部哨兵落在真实数据中间 | 每档**独立分配**带哨兵的缓冲 |

### 21.4 复现

```
bin\ORBMDK_RAM_SpeedTest.exe 0x20000000 4096 4 1000000      # ~74 kB/s（SWD 时钟受限）
bin\ORBMDK_RAM_SpeedTest.exe 0x20000000 4096 4 10000000     # ~582 / 631 kB/s
```

> ⚠ 会覆盖 `[ramAddr, ramAddr+bytes)`；且**跑完核处于停机状态**，须复位/上电后再跑真实程序。

### 21.5 32 KB 缓冲 · 1→100 MHz 步进扫描（2026-10-03）

> **为什么重测**：§21.1 用的 **4 KB 正好等于该链的 TAR 自增块**（`bug.md` B7），既覆盖不到块边界，
> 也无法代表大缓冲。本表用 **32 KB**（跨 8 个 TAR 块）重测。
>
> **工具**：`bin\ORBMDK_RAM_SpeedTest.exe`；按 `max_tar_block_size()` 切块、**仅在块边界重写 TAR**
> （方案 3，`bug.md` B7，落在测试路径；主代码不改）。**不这样做会被静默折返** —— 把第 6 参置 `0` 即可复现。
> **命令**：`0x20000000 32768 3 <时钟Hz>`，时钟 1→100 MHz **步进 1 MHz、首次失败即停**。

**结果：1→100 MHz 全程 0 失败**（每档 3 轮 + 尺寸阶梯 + 单笔基线，全部逐字校验 + 首尾哨兵）。

| MHz 写/读 | MHz 写/读 | MHz 写/读 | MHz 写/读 |
|---|---|---|---|
| 1 · 71.9 / 78.8 | 2 · 144.5 / 153.2 | 3 · 185.1 / 193.8 | 4 · 227.5 / 274.9 |
| 5 · 263.3 / 290.2 | 6 · 363.4 / 382.2 | 7 · 404.4 / 424.3 | 8 · 445.9 / 465.8 |
| 9 · 518.0 / 521.3 | 10 · 596.0 / 612.0 | 11 · 597.9 / 612.3 | 12 · 593.2 / 616.6 |
| **13 · 712.6 / 726.0** | 14 · 714.2 / 726.4 | 15 · 714.0 / 723.0 | 16 · 714.3 / 729.8 |
| **17 · 875.6 / 898.8** | 18 · 870.2 / 897.0 | 19 · 867.5 / 902.9 | 20 · 876.2 / 893.4 |
| 21 · 876.1 / 895.6 | 22 · 868.9 / 895.2 | 23 · 901.9 / 908.0 | 24 · 873.5 / 892.5 |
| **25 · 1144.2 / 1229.3** | 26 · 1201.9 / 1245.5 | 27 · 1194.9 / 1231.1 | 28 · 1217.2 / 1226.1 |
| 29 · 1160.2 / 1168.2 | 30 · 1207.3 / 1212.9 | 31 · 1195.0 / 1209.9 | 32 · 1130.1 / 1171.3 |
| 33 · 1211.0 / 1221.9 | 34 · 1189.7 / 1217.1 | 35 · 1203.0 / 1225.8 | 36 · 1143.9 / 1178.6 |
| 37 · 1141.0 / 1213.4 | 38 · 1144.4 / 1165.2 | 39 · 1199.8 / 1223.6 | 40 · 1182.1 / 1165.4 |
| 41 · 1150.9 / 1195.6 | 42 · 1154.3 / 1172.7 | 43 · 1144.1 / 1162.3 | 44 · 1146.6 / 1170.4 |
| 45 · 1206.5 / 1210.9 | 46 · 1200.6 / 1233.2 | 47 · 1165.3 / 1162.0 | 48 · 1159.0 / 1173.1 |
| 49 · 1178.0 / 1218.7 | 50 · 1194.2 / 1230.1 | 51 · 1204.2 / 1224.8 | 52 · 1210.7 / 1224.7 |
| 53 · 1147.0 / 1165.0 | 54 · 1213.0 / 1206.4 | 55 · 1157.4 / 1164.8 | 56 · 1201.7 / 1219.1 |
| 57 · 1200.6 / 1213.0 | 58 · 1203.9 / 1238.0 | 59 · 1287.9 / 1240.2 | 60 · 1209.0 / 1220.0 |
| 61 · 1143.3 / 1163.6 | 62 · 1214.9 / 1232.3 | 63 · 1234.4 / 1273.2 | 64 · 1259.9 / 1282.7 |
| 65 · 1222.6 / 1244.0 | 66 · 1216.0 / 1205.1 | 67 · 1229.6 / 1246.6 | 68 · 1249.9 / 1295.1 |
| 69 · 1261.5 / 1273.2 | 70 · 1191.6 / 1216.6 | 71 · 1204.3 / 1229.7 | 72 · 1251.7 / 1282.9 |
| 73 · 1206.7 / 1231.5 | 74 · 1230.2 / 1233.5 | 75 · 1219.5 / 1275.8 | 76 · 1251.0 / 1267.3 |
| 77 · 1265.4 / 1275.8 | 78 · 1205.2 / 1223.3 | 79 · 1140.4 / 1151.8 | 80 · 1138.6 / 1171.1 |
| 81 · 1145.1 / 1148.8 | 82 · 1211.2 / 1232.2 | 83 · 1197.6 / 1228.6 | 84 · 1195.0 / 1211.0 |
| 85 · 1158.8 / 1149.2 | 86 · 1200.1 / 1212.0 | 87 · 1154.0 / 1176.9 | 88 · 1202.4 / 1223.0 |
| 89 · 1206.3 / 1213.7 | 90 · 1198.7 / 1232.6 | 91 · 1153.9 / 1190.8 | 92 · 1223.4 / 1209.8 |
| 93 · 1182.1 / 1190.0 | 94 · 1232.3 / 1262.5 | 95 · 1178.5 / 1220.2 | 96 · 1253.4 / 1273.9 |
| 97 · 1172.7 / 1180.2 | 98 · 1268.3 / 1294.1 | 99 · 1246.8 / 1250.1 | 100 · 1211.6 / 1218.3 |

（单位 kB/s，`1 kB = 1024 B`。）

**读数**

| 段 | 数据 |
|---|---|
| 1→12 MHz 近似线性 | 写 71.9 → 593.2（**8.3×**），读 78.8 → 616.6（7.8×） |
| **台阶① 13 MHz** | 12 → 13：写 593 → 713（**+20%**），之后 13–16 平坦（712–714） |
| **台阶② 17 MHz** | 16 → 17：写 714 → 876（**+23%**），17–24 平坦（868–902） |
| **台阶③ 25 MHz** | 24 → 25：写 874 → 1144（**+31%**），此后进入平台 |
| **25→100 MHz 平台** | 写 1130–1288（中位 ≈**1200**）、读 1149–1295（中位 ≈**1228**）；**再提时钟无增益** |

1. **台阶 = 固件分频器的量化档**（根因见 `bug.md` **B8**，源码 `verilog/dbgIF.v`）。固件把请求值换算成
   "每个半周期几个 100 MHz tick"：`clkDiv = floor((CLK_FREQ-1)/(2·f_req))`，实际时钟 = `CLK_FREQ/(2·clkDiv)`，
   其中 `CLK_FREQ = 100 MHz`（`debug/dbgIF.py` 未传 `p_CLK_FREQ` ⇒ 取 `dbgIF.v:82` 默认；`i_clk` = `cd_debug`
   = 100 MHz，`crg_ecp5.py:93`）：

   | 请求 | clkDiv | **实际 SWD 时钟** | 实测写 kB/s | 模型（移位 + ≈350 µs/块固定开销） |
   |---|---|---|---|---|
   | 1 MHz | 50 | 1.0 MHz | 71.9 | 84 |
   | 10–12 MHz | 4 | 12.5 MHz | 593–596 | 627 |
   | 13–16 MHz | 3 | 16.67 MHz | 712–714 | 730 |
   | 17–24 MHz | 2 | 25 MHz | 868–902 | 874 |
   | **25 MHz** | **1** | **50 MHz = 最快档** | **1144** | 1088 |
   | >25 MHz | — | **请求被拒**（`perr`，分频不变） | 1130–1288 | — |

   （模型按每块 **≈5670 bit** 计 —— 504 B 只是**载荷**，SWD 线上每字 ≈45 bit：8 头 + 32 数据 + 校验 + 转向；
   §21.1 里 `500 B = 4000 bit` 那个提示是载荷位数，不是线上位数。）
   ⇒ 请求值只是"分频规格"：**25 MHz 正是最小能取到 `clkDiv=1` 的值**（2×25e6 = 50e6 ≤ 99,999,999），
   再往上加**零收益**；"平台"就是分频器最快档 **CLK_FREQ/2 = 50 MHz**。
2. **`MAX_CLOCK = CLK_FREQ/4 = 25 MHz` 是硬闸门**（`dbgIF.v:168`、`:552-558`）：`dwrite > MAX_CLOCK`
   （或 `< MIN_CLOCK = CLK_FREQ/512 ≈ 195 kHz`）时固件置 **`perr` 且不改 `clkDiv`**。
   ⚠ **陷阱**：分频寄存器**跨主机会话保持**，被拒即"沿用上一次的值"。若在**刚复位**的 FPGA 上直接请求
   >25 MHz，实际时钟停在复位默认 `DEFAULT_IF_TICKS_PER_CLK = 49` ⇒ **≈1.02 MHz（≈72 kB/s）**，仅 `perr` 提示。
   本表 26–100 MHz "看起来仍在 50 MHz"，正是因为**上一个 25 MHz 档把 clkDiv 留在了 1**（属扫描顺序的产物）。
   ⚠ **本层未上报**：`DAP_SWJ_Clock` 只回命令级状态（`ORBMDK_HID.cpp:1321-1334`），`Clock=` 到 200 MHz 才截
   （`ORBMDK_RDDI.cpp:2637-2644`）⇒ 1→100 MHz 全程 `ConfigureInterface -> 0`（列为**本层改进项**）。
3. **≥25 MHz 后瓶颈不再是 SWD 时钟**：50 MHz 下每块移位仅 **≈113 µs**，实测每块 **≈420–460 µs**
   ⇒ 其余是**与时钟无关的 ≈350 µs/块固定开销**（USB 往返 + FSM 握手）。要再快只能**减少往返次数**
   （每包 508 B → 126 字已是上限），或**提高 debug 域频率**（`MAX_CLOCK` 与最快档都随之线性放大）。
4. **100 MHz 请求值下全程无数据错误** ⇒ 信号裕量足够；但有效速率与 25 MHz **完全相同**（且这个"相同"
   是分频被拒后的**沿用值**，不是真的跑到了 100 MHz）。
5. 与 §21.1（4 KB 缓冲）同水平：1 MHz 71.9 vs 74.3；10 MHz 596.0/612.0 vs 582.1/631.5
   ⇒ **缓冲从 4 KB 加大到 32 KB 不改变结论**。

> **复现**（默认即方案 3；末位 `0` 复现 `bug.md` B7 的静默折返）：
> ```
> bin\ORBMDK_RAM_SpeedTest.exe 0x20000000 32768 3 1000000        #  71.9 /  78.8 kB/s
> bin\ORBMDK_RAM_SpeedTest.exe 0x20000000 32768 3 10000000       # 596.0 / 612.0 kB/s
> bin\ORBMDK_RAM_SpeedTest.exe 0x20000000 32768 3 25000000       # 1144.2 / 1229.3 kB/s
> bin\ORBMDK_RAM_SpeedTest.exe 0x20000000 32768 3 100000000      # 1211.6 / 1218.3 kB/s
> bin\ORBMDK_RAM_SpeedTest.exe 0x20000000 32768 1 1000000 ORBMDK_RDDI.dll 0   # 期望 FAIL（B7）
> ```
### 烧录速率统计（TESTSPEED）

把级别设为 **2**（TESTSPEED），`DAP_RegWriteRepeat` / `DAP_RegReadRepeat`
（即 AGDI 的 `SWD_WriteBlock` / `SWD_VerifyBlock` / `SWD_ReadBlock`，flash 下载与校验的真实数据通道）
会按方向统计字节数与耗时，限流输出到 `%TEMP%\ORBMDK_RDDI.log`：

```
[ORBMDK][00:12:34.101][TESTSPEED][RDDI][8112:9012] meter config: transport=V2/Bulk speed=Full(12Mbps) cmdPkt=508 B -> 125 words/round trip, blockTransfer=on | bus ceiling ~1465 kB/s
[ORBMDK][00:12:34.567][TESTSPEED][RDDI][8112:9012] WRITE speed: 4096 B in 7.1 ms -> 563.4 kB/s | 9 round trips, 788.9 us/trip | window wall 47.3 ms, dap 15.0% | total 65536 B in 115.0 ms (avg 556.5 kB/s, 147 trips)
[ORBMDK][00:12:34.589][TESTSPEED][RDDI][8112:9012] READ  speed: 388 B in 0.9 ms -> 405.2 kB/s | 1 round trips, 900.0 us/trip | window wall 2.1 ms, dap 42.9% | total 1128 B in 3.9 ms (avg 279.4 kB/s, 3 trips)
[ORBMDK][00:12:35.291][TESTSPEED][RDDI][8112:9012] meter segment: wall 1218.4 ms | dap 118.9 ms (9.8%) | other 1099.5 ms (90.2%) | write 65536 B (538.0 kB/s) + read 1128 B | 150 round trips
[ORBMDK][00:12:35.291][TESTSPEED][RDDI][8112:9012] meter usb: 150 cmds, out 96.2 ms (641.3 us/cmd), in 22.7 ms (151.3 us/cmd), round trip 792.6 us/cmd
```

| 项 | 约定 |
|----|------|
| 统计口径 | `WRITE` = 下载写入，`READ` = 回读校验；每行给出**窗口速率**与**累计平均速率**（kB/s） |
| 往返次数 | `round trips` = 窗口内实际发生的 USB 往返数（块传输 1 次/块，逐字回退 N 次），`us/trip` 由此得出 |
| 计时范围 | 只含 DAP 传输段（`DAP_TransferBlock` / `DAP_Transfer` 往返），日志格式化不计入 |
| 输出频率 | 累计 `≥ 1024 字` 或 `≥ 200 ms` 才落一行；两次调用空闲 `> 500 ms` 视为上一段烧录结束，立即结算余量 |
| 关闭开销 | 阈值高于 TESTSPEED 时连计时都不做（`ORBMDK_LogGetLevel()` 前置判断） |
| 失败不计 | 中途返回 `RDDI_DAP_ERROR` 的那次不统计（避免把失败路径算进速率） |
| 配置行 | 首次计量（传输层变化会重打）输出一条 `meter config:`，给出传输层 / **端口速度** / 出包字节数 / 每次往返字数 / 块传输是否生效，以及该速度的总线理论上限 |
| 窗口墙钟 | 速率行里的 `window wall …, dap …%`：本窗口的**真实墙钟**，以及驱动（DAP 传输）在其中的占比 |
| 段总结 | 一段连续传输结束（空闲 > 500 ms）后补打 `meter segment:`：整段的 wall / dap / other + 双向字节与往返数。**判断瓶颈在哪一层的唯一依据**。注意它是等**下一笔传输**到来时才补打的，所以日志末尾可能少最后一段 |
| USB 拆分 | 再补一条 `meter usb:`：累计命令数，以及 OUT（发命令）与 IN（收响应）各自的耗时与每命令均值 |

> **怎么读这几行**（实测样本，见 COMPAT_ANALYSIS）：
> - **先看 `meter segment` 的 dap%**：实测一次下载里驱动只占 **~10–15%**，其余（other）在 AGDI 与
>   目标端 flash 算法。把传输优化到 0，整体也只能快这么多 —— 所以"加速"的第一问是
>   **瓶颈到底在不在这一层**，而不是埋头抠 USB。
> - **再看 `meter usb` 的 out/in**：`out ↑` = 时间花在把命令包搬上线；`in ↑` = 设备应答慢
>   （SWD 时钟 / flash 算法），驱动层改什么都没用。
> - `us/trip` 必须与 `words/round trip` 一起看。两组实测摆在一起：`出包 64 B / 14 字 → ~95 µs/往返`、
>   `出包 508 B / 125 字 → ~789 µs/往返`。往返耗时几乎正比于**出包字节数**（≈1.55 µs/字节），
>   即**每个 64 字节包固定约 95 µs**，其中只有 ~43 µs 是 Full Speed 的真实线时间，
>   剩下 **~52 µs 是每包的固定开销**。
>   ⇒ 再靠"每次往返多带几个字"已经榨不出东西（总字节数没变），只能**让每个字节更便宜**：
>   把链路从 Full Speed 换成 High Speed（512 B 包，同样 508 字节只需 1 个包）。
> - `bus ceiling` 是给对照用的：实测已贴着它 = 被物理层卡死；实测只有它的 ~40%
>   （FS 下 5 字/µs 量级）时，才值得回头查驱动与端点调度。
> - **日志时间戳的间隔 ≠ 计量时间**：窗口计量 7.1 ms，两行时间戳却相差 ~47 ms —— 差值就是
>   AGDI / 目标侧的时间。`window wall` 与 `meter segment` 现在直接把它打出来了。
> - 需要传输层的详细枚举/标定过程（`[BULK]` 行）时，阈值要放到 **1(INFO) 或 0(DEBUG)** ——
>   `ORBMDK_LogTrace` 按 INFO 级过滤，阈值 2 会把 `[BULK]` 全部丢掉。
