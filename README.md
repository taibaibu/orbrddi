# ORBMDK - ORBTrace CMSIS-DAP RDDI 驱动层

ORBMDK 提供 RDDI (Remote Debug Driver Interface) 接口，兼容 [elaphureLinkAGDI](https://github.com/fly2046/elaphureLinkAGDI)，使 ORBTrace 调试器能够作为标准 CMSIS-DAP 调试器用于 Keil uVision。

## 架构

```
Keil uVision (IDE)
       │
       ▼ (通过 elaphureLinkAGDI，加载 elaphureRddi.dll,elaphureLinkAGDI就是KEIL AGDI)
   ORBMDK_RDDI.dll (RDDI 接口)
       │
       ▼
   ORBMDK_RDDI       # RDDI 层实现
       │
       ├─ USB HID (V1) ──────→ ORBTrace 调试器
       └─ USB Bulk (V2) ──────→ ORBTrace 调试器
              │
              ▼ (SWD/SWO)
         ITM/ETM/SWO/TPIU
              │
              ▼ (Trace 解码)
         独立解码器实现
              │
              ▼
         Trace 数据输出
```

**注意**:
- **AGDI 层 = Keil 的 "CMSIS-DAP Debugger" = elaphureLinkAGDI**，三者是同一个东西；ORBMDK 仅实现其下的 RDDI 驱动层。
- AGDI 层按固定文件名加载 RDDI 层 DLL：Keil 原版 "CMSIS-DAP Debugger" 加载 **`CMSIS_DAP.dll`**（elaphureLink 定制版为 `elaphureRddi.dll`）。ORBMDK 构建产物为 `ORBMDK_RDDI.dll`，部署时需重命名为实际生效的那个名字（见"安装"）。

## 项目结构

```
ORBMDK/
├── include/                     # 头文件
│   ├── ORBMDK.h                # 主头文件、版本信息、ORBTrace VID/PID
│   ├── ORBMDK_COBS.h           # COBS 帧同步解码
│   ├── ORBMDK_Coverage.h       # 代码覆盖率分析接口
│   ├── ORBMDK_DAP.h            # CMSIS-DAP 协议定义
│   ├── ORBMDK_DAPV2.h          # CMSIS-DAP V2 Bulk 协议定义
│   ├── ORBMDK_ETM_Decoder.h    # ETM 解码器
│   ├── ORBMDK_HID.h            # USB HID 层定义
│   ├── ORBMDK_ITM_Decoder.h    # ITM 解码器
│   ├── ORBMDK_OFLOW.h          # OFLOW 时间戳协议解码
│   ├── ORBMDK_RDDI.h           # RDDI 接口定义（78 个导出）
│   ├── ORBMDK_Symbols.h        # 符号解析接口 (Objdump/DWARF)
│   ├── ORBMDK_TPIU_Decoder.h   # TPIU 解码器
│   ├── ORBMDK_Trace.h          # Trace 解码器定义
│   └── ORBMDK_USB_Bulk.h       # USB Bulk V2 传输层定义
├── src/                         # 源代码
│   ├── pch.h / pch.cpp         # 预编译头
│   ├── ORBMDK_DLL.cpp          # DLL 入口点
│   ├── ORBMDK_HID.cpp          # USB HID 通信层 + DAP 命令封装
│   ├── ORBMDK_RDDI.cpp         # RDDI 实现（核心）
│   ├── ORBMDK_USB_Bulk.cpp     # USB Bulk V2 传输层
│   ├── ORBMDK_Trace.cpp        # Trace 解码器适配层
│   ├── ORBMDK_ITM_Decoder.cpp  # ITM 解码器实现
│   ├── ORBMDK_ETM_Decoder.cpp  # ETM 解码器实现
│   ├── ORBMDK_TPIU_Decoder.cpp # TPIU 解码器实现
│   ├── ORBMDK_COBS.cpp         # COBS 编解码
│   ├── ORBMDK_OFLOW.cpp        # OFLOW 协议解码
│   ├── ORBMDK_Coverage.cpp     # 代码覆盖率分析
│   └── ORBMDK_Symbols.cpp      # 符号解析实现
├── deprecated/                  # 废弃文件（不参与编译）
├── obj/                         # 编译中间文件
├── test/                        # 测试文件
│   ├── ORBMDK_RDDI_FullTest.cpp # 实机功能测试（40 项）
│   ├── ORBMDK_RDDI_Test.cpp     # 基础接口测试
│   └── build_test.ps1           # 测试构建脚本
├── bin/                         # 编译输出
│   └── ORBMDK_RDDI.dll         # RDDI 驱动 DLL
├── build.ps1                    # PowerShell 构建脚本
├── README.md                    # 本文档
└── COMPAT_ANALYSIS.md           # 兼容性分析与实现状态
```

## 构建

### PowerShell (推荐)

```powershell
.\build.ps1
```

输出文件：`bin\ORBMDK_RDDI.dll`（MSVC 2017 x64，无 error / 无 warning）

## 安装

1. 编译得到 `bin\ORBMDK_RDDI.dll`

2. 配合 elaphureLinkAGDI（Keil 的 "CMSIS-DAP Debugger"）使用：
   - 安装 [elaphureLinkAGDI](https://github.com/fly2046/elaphureLinkAGDI)
   - **将 `ORBMDK_RDDI.dll` 重命名为 `CMSIS_DAP.dll`**，放到 Keil 的 `ARM\BIN\` 目录
   - 必须是 **32 位 (x86)** 构建（`build.ps1` 已默认 x86）；Keil µVision 是 32 位进程，无法加载 64 位 DLL

3. 在 Keil 项目中配置调试器：
   - 打开 "Project" → "Options for Target" → "Debug"
   - 选择 "ORBMDK" 或对应的 elaphureLinkAGDI 驱动
   - 配置 SWD 接口和时钟频率

## 验证

```powershell
.\test\build_test.ps1 -Source ORBMDK_RDDI_FullTest.cpp
.\bin\ORBMDK_RDDI_FullTest.exe
```

在 ORBTrace + STM32F1 目标上实测 **40/40 全部通过**：

```
DAP_ReadReg(DP_IDCODE)    PASS   IDCODE: 0x2BA01477
DAP_ReadReg(AP_IDR)       PASS   AP IDR: 0x24770011
TEST SUMMARY: PASSED 40 / FAILED 0 / ALL TESTS PASSED
```

---

## 兼容性与实现要点

> 以下为经实机验证后**固化**的实现约束，修改相关代码时不可破坏。
> 详细分析、根因定位与验证数据见 [COMPAT_ANALYSIS.md](COMPAT_ANALYSIS.md)。

### 设备识别

| 项 | 值 | 说明 |
|----|----|------|
| ORBTrace VID | `0x1209` | 定义于 `include/ORBMDK.h` 的 `ORBMDK_ORBTRACE_VID` |
| ORBTrace PID | `0x3443` | 定义于 `include/ORBMDK.h` 的 `ORBMDK_ORBTRACE_PID` |

> ⚠️ HID 层与 Bulk 层**必须共用同一份常量**。历史上两层各自定义（`0x3456` / `0x6A02`）导致匹配到不同设备。
> 取值依据：orbtrace 固件 `orbtrace/debug/cmsis_test.py`、orbuculum `Src/orbtraceIf.c`。

### DAP 枚举契约（关键，勿破坏）

ARM `rddi_dap.h` 规定的调用顺序：

```
RDDI_Open → DAP_Configure → DAP_GetNumberOfDAPs → DAP_GetDAPIDList
          → DAP_Connect → ...
```

即 **`DAP_GetNumberOfDAPs` / `DAP_GetDAPIDList` 会在连接目标之前被调用**，且规范明确要求这两个函数
`does not communicate with the target`。因此它们**不得**依赖需要连接目标才能得到的 IDCODE 列表：

| 函数 | 必须返回 |
|------|----------|
| `DAP_GetNumberOfDAPs` | `1`（ORBTrace 单 DAP） |
| `DAP_GetDAPIDList` | `[0]`（DAP **索引**，会被当作 CMSIS-DAP 的 DAP Index 字节使用，**不是 IDCODE**） |
| `CMSIS_DAP_GetNumberOfDevices` | `1` |

> ⚠️ 历史问题：`DAP_GetNumberOfDAPs` 曾返回 `dapIdList.size()`，而该列表只有
> `CMSIS_DAP_DetectNumberOfDAPs`（需连接目标）才会填充 → Keil 在 Connect 之前拿到 0 个 DAP，
> 报 **"No Debug Unit Found"**。`CMSIS_DAP_DetectNumberOfDAPs` / `CMSIS_DAP_DetectDAPIDList`
> 返回的是目标 **IDCODE**，供 AGDI 的 `SWD_ReadID` / `JTAG_DetectDevices` 使用，两者语义不同。

### 输出参数必须写入

所有带标量输出参数的 RDDI 接口，**即使句柄无效也要先写输出参数**（写 0 / 空串）再返回错误：

```cpp
if (noOfIFs) { *noOfIFs = 0; }        // 先写
RDDIContext* ctx = GetContext(handle);
if (!ctx) { return RDDI_INVHANDLE; }  // 再判错
```

> 原因：调用方（AGDI 的 `PDSCDebug_InitDebugger`）**只看输出值、不看返回值** ——
> `CMSIS_DAP_Detect(...); if (numOfIFs == 0) return EU02;`。若不写，调用方读到自己的初始值 0，
> 会把"句柄无效"误报成 "No Debug Unit Found"，且真实错误码被吞掉。
> `CMSIS_DAP_Identify` 另有风险：不写 `str` 时 AGDI 会对**未初始化栈内存**做 `strcmp`。

### HID 响应偏移（全层统一约定）

CMSIS-DAP V1 的 IN 报告含 1 字节**报告ID**，所有 `DAP_*` / `CMSIS_DAP_*` 解析必须遵守：

```
resp[0] = 报告ID（通常 0x00）
resp[1] = 命令ID
resp[2..] = 响应负载
```

- OUT 报告：`reportOut[0] = 0x00`（报告ID），命令自 `reportOut[1]` 起，总长 65 字节。
- `DAP_Transfer` 响应为 `[cmd][Transfer Count][Transfer Response][Data]`，**计数在状态之前**；`DAP_TransferBlock` 状态在 `resp[4]`。

### JTAG-to-SWD 切换序列（必须）

orbtrace 固件 `DAP_Connect(port=SWD)` 的内部切换序列 line reset 仅 50 个周期（卡在 ADIv5 "至少 50" 的临界值），
**不足以让目标可靠进入 SWD 模式**。主机侧必须在 `DAP_Connect` **之后**补发标准切换序列：

```
DAP_SWJ_Sequence(56, {0xFF×7}) → (16, {0x9E,0xE7}) → (56, {0xFF×7}) → (8, {0x00})
```

> ⚠️ 顺序不可颠倒：放在 Connect **之前**会被固件内部切换覆盖。
> `CMSIS_DAP_Connect` 与 `CMSIS_DAP_ResetDAP` 均需执行。缺失该序列的典型症状是
> `DAP_Transfer` 返回 `Transfer Response = 0x07`（ACK=7=NO_ACK）。

### ACK 编码映射

固件透传 SWD 的 3 位 ACK，需映射为内部 `DAP_RES_*`：

| 标准 ACK | 含义 | 内部 |
|----------|------|------|
| 1 | OK | `DAP_RES_OK`(0) |
| 2 | WAIT | `DAP_RES_WAIT`(1) |
| 4 | FAULT | `DAP_RES_FAULT`(2) |
| 7 | NO_ACK（SWDIO 未被驱动） | `DAP_RES_NO_ACK`(3) |

### 日志策略

| 项 | 约定 |
|----|------|
| 默认级别 | **ERROR**（`ORBMDK_RDDI.cpp` / `ORBMDK_HID.cpp`） |
| 过滤时机 | 在构造日志字符串**之前**判断级别，被过滤时零开销 |
| 临时恢复 | 环境变量 `ORBMDK_LOG_LEVEL`：`0=DEBUG 1=INFO 2=WARN 3=ERROR`，无需重新编译 |
| 禁止 | 逐寄存器 / 逐次传输的 INFO 日志、响应十六进制转储 |

```bat
set ORBMDK_LOG_LEVEL=0
```

---

## 功能

### 支持的调试协议
- **SWD** (Serial Wire Debug)
- **JTAG**
- **USB HID V1** - 标准 CMSIS-DAP
- **USB Bulk V2** - 高速传输 (WinUSB) ✅ 已实现

### 支持的调试功能
- 内存读写（字节、半字、字、块）
- 核心寄存器访问
- 目标复位
- 电源控制
- **Trace 数据采集** (ITM/ETM/SWO/TPIU)
- **PC 采样** (ETM/ITM PC Sampling) ✅ 已实现

> **断点 / 观察点 / Flash 编程**由上层 elaphureLinkAGDI 通过组合 `DAP_RegAccessBlock` /
> `DAP_RegReadRepeat` / `DAP_RegWriteRepeat` 实现，不属于 ORBMDK（RDDI 层）的职责。

### RDDI 接口

`ORBMDK_RDDI.h` 中带 `RDDI_FUNC` 的导出函数共 **78 个**，按 `GetProcAddress` 导出：

| 分类 | 数量 |
|------|------|
| RDDI 核心（Open/Close/GetLastError） | 3 |
| DAP 基础 + 寄存器 | 20 |
| CMSIS_DAP_* | 30 |
| PC_*（PC 采样） | 6 |
| StreamingTrace_* | 13 |
| RDDI_SetLogCallback | 1 |

其中 elaphureLinkAGDI **实际只调用 15 个**：

| 分类 | 函数 |
|------|------|
| RDDI 核心 | `RDDI_Open`、`RDDI_Close` |
| DAP 寄存器 | `DAP_ReadReg`、`DAP_WriteReg`、`DAP_RegAccessBlock`、`DAP_RegReadRepeat`、`DAP_RegWriteRepeat` |
| CMSIS-DAP | `CMSIS_DAP_Detect`、`CMSIS_DAP_Identify`、`CMSIS_DAP_ConfigureInterface`、`CMSIS_DAP_DetectNumberOfDAPs`、`CMSIS_DAP_DetectDAPIDList`、`CMSIS_DAP_Commands`、`CMSIS_DAP_SWJ_Sequence`、`CMSIS_DAP_SWJ_Pins` |

> 完整的调用点清单（文件:行 + 所在 AGDI 函数）见 [COMPAT_ANALYSIS.md 第二节](COMPAT_ANALYSIS.md)。
> 其余导出（`StreamingTrace_*`、`CMSIS_DAP_SWO_*`、`CMSIS_DAP_JTAG_*`、`CMSIS_DAP_PC_*` 等）服务于其他宿主，不影响 Keil 调试链路。

### Trace 功能

#### SWO (Serial Wire Output)
| 模式 | 说明 |
|------|------|
| UART | 异步串行输出 (115200-921600 baud) |
| Manchester | Manchester 编码同步输出 |

#### ITM (Instrumentation Trace Macrocell)
- 32 个激励端口 (Port 0-31)
- 时间戳支持
- 支持 printf 调试

#### ETM (Embedded Trace Macrocell)
- 指令流跟踪（v3.5 基本可用；v4 部分 opcode 未实现）
- 分支跟踪
- 完整程序执行记录
- **PC Sampling** - 程序计数器采样（硬件断点辅助）

> ⚠️ ETM 解码器为骨架实现，`FuncReturn` / `ExceptionReturn` / `Prefix` 等分支尚未处理，需结合具体 ETM 配置与实机 trace 数据完善。

**ETM v3.5 支持：**
- A-SYNC 同步检测
- ISYNC 指令同步
- 分支地址编码 (8/11/19 位偏移)
- 周期计数 (CYCCNT)
- 上下文 ID 追踪
- 异常入口/出口追踪

**ETM v4.x 支持：**
- 短地址编码 (IS0/IS1)
- 长地址编码 (32/64-bit)
- 原子指令序列
- 条件分支
- 函数返回
- 异常追踪

#### TPIU (Trace Port Interface Unit)
- 并行 trace 端口 (1/2/4 bit)
- SWO 模式输出
- 可配置时钟分频

### Trace 解码器 ✅

独立实现的 trace 解码器，无需外部依赖：

| 解码器 | 说明 | 状态 |
|--------|------|------|
| COBS | Consistent Overhead Byte Stuffing - 帧同步 | ✅ 完整（按 orbuculum 对齐） |
| OFLOW | 带时间戳的 trace 数据流协议 | ✅ |
| ITM | ITM 激励数据解码 (SW/HW/TS/GTS/XTN/NISYNC) | ✅ 状态机完整 |
| ETM | ETM 指令流解码 (v3.5/v4.x) | ⚠️ v3.5 基本可用；v4 部分 opcode 未实现 |
| SWO | SWO UART/Manchester 数据解码 | ✅ |
| TPIU | 并行 trace 端口数据解析 | ✅ 状态机自洽 |

**解码器特性：**

- ITM: 支持 PC Sample、DWT 事件、软件 printf
- ETM: 支持分支跟踪、周期计数、上下文切换
- TPIU: 支持 Manchester/NRZ 异步解码
- 所有解码器都提供统计信息

### 代码覆盖率分析 ✅

基于 ETM trace 数据实现完整的代码覆盖率统计：

**覆盖数据类型：**

| 类型 | 说明 |
|------|------|
| 指令执行记录 | 记录每个地址的执行计数 |
| 分支覆盖分析 | 统计条件分支的执行情况 |
| 函数调用追踪 | 记录函数调用关系和成本 |
| 基本块覆盖 | 追踪代码块执行情况 |
| 周期计数 | 记录指令执行周期数 |

**报告格式：**

| 格式 | 说明 |
|------|------|
| KCacheGrind | Callgrind 格式，用于可视化分析 |
| 文本 | 简化文本报告 |
| JSON | 结构化 JSON 输出 |
| GCOV | GCC 覆盖率格式 |

**分析维度：**

- 指令覆盖率 (Instruction Coverage)
- 分支覆盖率 (Branch Coverage)
- 函数覆盖率 (Function Coverage)
- 行覆盖率 (Line Coverage)

### 符号解析 ✅

支持两种模式解析 ELF/DWARF 符号信息：

| 模式 | 依赖 | 特点 |
|------|------|------|
| Objdump 模式 | arm-none-eabi-objdump | 简单可靠 |
| DWARF 模式 | libdwarf/libelf | 高性能，直接解析 |

**功能：**

- 地址 → 函数名/文件名/行号 查找
- 汇编指令文本生成
- 函数边界识别
- 源码行映射
- C++ 名称 demangling
- 工具路径自动检测 (Keil/GCC)

## USB 传输实现

### HID 模式 (V1)
- 使用 Windows HID API
- 报告负载 64 字节，**含 1 字节报告ID 前缀**（OUT 共 65 字节）
- 通用兼容模式（ORBTrace 固件仅支持 V1）

### WinUSB Bulk 模式 (V2)
- 使用 SetupAPI 枚举设备
- WinUSB API 实现高速传输
- 支持端点：
  - Bulk OUT (EP 0x01)
  - Bulk IN (EP 0x81)
- 支持超时控制（重叠 I/O）
- 自动 fallback 到 HID 模式

### PC 采样实现 ✅
- **CMSIS_DAP_PC_Capture** - 启动/停止 PC 采样
- **CMSIS_DAP_PC_GetNumberOfChannels** - 获取通道数
- **CMSIS_DAP_PC_GetChannelInfos** - 获取通道信息
- **CMSIS_DAP_PC_GetCommonFrequency** - 获取采样频率
- **CMSIS_DAP_PC_GetData** - 获取 PC 数据
- **CMSIS_DAP_PC_GetValues** - 获取 PC 值

**实现细节**：
- 通过 ITM HW 包 (srcAddr=2) 接收 DWT PC Sample
- ITM 解码器 (`ORBMDK_ITM_Decoder`) 解析 trace 数据
- 支持睡眠状态检测 (0x00 = 睡眠)
- 支持 1-4 字节 PC 值 (小端序)

---

## 与 elaphureLink 的关系

本项目基于 elaphureLink 的架构设计，提供独立的 RDDI 驱动层：

| 组件 | 来源 |
|------|------|
| AGDI 层 | [elaphureLinkAGDI](https://github.com/fly2046/elaphureLinkAGDI) |
| RDDI 驱动层 | ORBMDK (本项目) |

ORBMDK 专注于底层通信和协议实现，与 AGDI 层解耦。

## 开发

### 依赖
- Windows SDK (10.0.17763.0+)
- Visual Studio 2017+ 或 MSVC 工具链
- Windows HID API
- WinUSB (USB Bulk V2 传输)
- SetupAPI

### Trace 数据流

```
目标 MCU
   │
   ├── ITM ──────────┐
   ├── ETM ──────────┼──→ TPIU ──→ SWO ──→ 主机
   └── DWT ─────────┘
                        │
                        ▼
                  USB HID/Bulk
                        │
                        ▼
                  COBS 解码
                        │
                        ▼
                  OFLOW 解码 (带时间戳)
                        │
                        ▼
                  ITM/ETM/SWO 解码
                        │
                        ▼
                  应用数据输出
```

## 参考资料

- [CMSIS-DAP 规范](https://arm-software.github.io/CMSIS_5/DAP/html/index.html)
- [ARM Debug Interface 规范](https://developer.arm.com/documentation/ihi0031/latest/)
- [elaphureLink 项目](https://github.com/elaphureLink/elaphureLink)
- [elaphureLinkAGDI 项目](https://github.com/fly2046/elaphureLinkAGDI)
- [ORBTrace 项目](https://github.com/orbcode/orbtrace)
- [orbuculum 项目](https://github.com/orbcode/orbuculum) - Trace 解码参考

## 许可证

基于 BSD-2-Clause 许可证（参考 elaphureLink）
