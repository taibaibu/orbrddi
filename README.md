# ORBMDK - ORBTrace CMSIS-DAP RDDI 驱动层

> **版本 `0.5.0`**（2026-10-03）· 驱动自身发版号，定义于 `include/ORBMDK.h`（`ORBMDK_VERSION_STRING`），
> 经 `ORBMDK_GetVersionString()` / `ORBMDK_GetVersion()` 暴露。
> ⚠️ **与上报给 Keil 的协议版本串是两回事**：后者 = 设备 `DAP_Info(0x04)`，主版本经归一化**抬到 ≥ 2**
> （2026-10-01 放开 SWO 流式门控，见"USB 传输实现 → WinUSB Bulk 模式"与 `COMPAT_ANALYSIS.md` §17.5），
> **不要**与本版本号联动。发版历史见文末[版本记录](#版本记录)。

ORBMDK 提供 RDDI (Remote Debug Driver Interface) 接口，使 ORBTrace 调试器能够作为标准 CMSIS-DAP 调试器用于 Keil uVision。

## 架构

```
Keil uVision (IDE)
       │
       ▼ (通过 KEIL AGDI)
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
- **AGDI 层 = Keil 的 "CMSIS-DAP Debugger" ；ORBMDK 仅实现其下的 RDDI 驱动层。
- AGDI 层按固定文件名加载 RDDI 层 DLL：Keil 原版 "CMSIS-DAP Debugger" 加载 **`CMSIS_DAP.dll`**。ORBMDK 构建产物为 `ORBMDK_RDDI.dll`，部署时需重命名为实际生效的那个名字（见"安装"）。

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
│   ├── ORBMDK_Log.h            # 统一日志接口
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
│   ├── ORBMDK_Log.cpp          # 统一日志实现（多 Sink / 级别热更新）
│   ├── ORBMDK_ETM_Decoder.cpp  # ETM 解码器实现
│   ├── ORBMDK_TPIU_Decoder.cpp # TPIU 解码器实现
│   ├── ORBMDK_COBS.cpp         # COBS 编解码
│   ├── ORBMDK_OFLOW.cpp        # OFLOW 协议解码
│   ├── ORBMDK_Coverage.cpp     # 代码覆盖率分析
│   └── ORBMDK_Symbols.cpp      # 符号解析实现
├── obj/                         # 编译中间文件
├── test/                        # 测试 / 诊断工具（10 个 .cpp + 1 个构建脚本，清单见"验证"）
│   ├── ORBMDK_RDDI_FullTest.cpp # 全面功能测试（40 项，含完整导出扫描）
│   ├── ORBMDK_BlockTransferTest.cpp # 块传输提速 / 越界验证
│   ├── ORBMDK_RAM_SpeedTest.cpp # RAM 读写吞吐（绕开 Keil/AGDI，测设备侧极限）
│   ├── swdprobe.cpp             # SWD 通路功能回归（V2 / V1，PASS-FAIL 统计）
│   ├── hidprobe.cpp             # CMSIS-DAP v1 (HID) 极简回归
│   ├── jtagprobe.cpp            # JTAG 端到端（走本层 DLL）
│   ├── jtagrawprobe.cpp         # JTAG 裸帧 + 引脚级诊断（绕开本层）
│   ├── jtagblockprobe.cpp       # JTAG 下固件 0x06 专项复验（绕开本层，每步判活）
│   ├── v2rawprobe.cpp           # V2 裸帧 / 包长 / 分片诊断（绕开本层）
│   ├── ifacedump.cpp            # 接口清点（列全 VID/PID 下所有接口与端点，只读）
│   └── build_test.ps1           # 测试构建脚本（-All 重建全部，-Source 单个）
├── tools/                       # 一次性辅助脚本（Python，不参与编译）
│   ├── agdi_ver.py             # 从 AGDI/Keil DLL 中提取版本串
│   ├── pe_re.py                # PE 导出表 / 节表速查（逆向辅助）
│   ├── patch_agdi_etb.py       # 放开 `CMSIS_AGDI.dll` 的 Trace Port 下拉（改第三方二进制，实验用，见 §17.9(7)）
│   └── _rsrc_scan.py           # 资源表 / 对话框模板 / 导入表速查（T0/T1 侦察用，临时脚本）
├── bin/                         # 编译输出（DLL 与测试 exe 都落在这里）
│   └── ORBMDK_RDDI.dll         # RDDI 驱动 DLL
├── .vscode/
│   └── c_cpp_properties.json   # IntelliSense 配置（参数与 build.ps1 一致）
├── build.ps1                    # PowerShell 构建 / 部署脚本
├── README.md                    # 本文档
├── COMPAT_ANALYSIS.md           # AGDI 逆向分析 · ABI 兼容性结论 · 实现状态
├── bug.md                       # 实现细节 · 缺陷定性 · 排查笔记（JTAG 相关见 B9 / B11.8）
├── Todo.md                      # 未完成项（只列待办，不写实现细节）
├── PATCH_orbtrace-1.4.3_JTAG_fixes.txt  # 固件侧 JTAG 可选修复补丁（基于 orbtrace 1.4.3，含完整代码）
├── Usage.md.bak                 # 历史快照（不参与构建 / 部署）
└── Todo.md.bak                  # 历史快照（不参与构建 / 部署）
```

### 文档导航

| 文档 | 收什么 | 什么时候看 |
|------|--------|------------|
| `README.md` | 构建 / 安装 / 验证 / 不可破坏的实现约束 | 上手、改代码前 |
| `COMPAT_ANALYSIS.md` | AGDI 逆向分析、ABI 兼容性结论、历史现场记录 | 需要"为什么这么写"的依据 |
| `bug.md` | 实现细节、缺陷定性、排查笔记（JTAG 相关见 B9 / B11.8） | 复现某现象、查根因 |
| `Todo.md` | **只有未完成项** | 想知道还差什么 |
| `Usage.md.bak` | 驱动的 API 使用手册（快速开始 + 各导出函数调用示例） | 想集成、或直接调本层 DLL 时 |
| `PATCH_orbtrace-1.4.3_JTAG_fixes.txt` | 探针固件（orbtrace 门级 Verilog + Amaranth）侧的可选改动 | 想让固件侧 JTAG 通路更完备时 |

> 约定（勿混）：AGDI 逆向与 ABI 结论只进 `COMPAT_ANALYSIS.md`；实现细节与排查过程只进 `bug.md`；
> `Todo.md` 只列未完成项。固件补丁因为是**另一个仓库**的改动，单独成文件，不写进上面三份。

## 构建

### PowerShell (推荐)

```powershell
.\build.ps1
```

输出文件：`bin\ORBMDK_RDDI.dll`（MSVC **x86**，无 error / 无 warning）

构建脚本会自动探测工具链，无需手改路径：

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

## 安装

```powershell
.\build.ps1 -Deploy                                  # 编译 + 一键部署（自动定位 Keil 的 ARM\BIN）
.\build.ps1 -DeployOnly                              # 只部署，不编译
.\build.ps1 -Deploy -KeilArmBin "D:\Keil_v5\ARM\BIN" # 或显式指定 Keil 的 ARM\BIN
```

脚本会把 `bin\ORBMDK_RDDI.dll` 复制为 Keil 的 `CMSIS_DAP.dll`，并在首次覆盖前把官方原件备份为 `CMSIS_DAP.dll.bak`
（部署逻辑已并入 `build.ps1`，原 `deploy.ps1` 已删除）。部署前请关闭 µVision —— 它会把 DLL 常驻内存。

手动方式：

1. 编译得到 `bin\ORBMDK_RDDI.dll`

2. 配合 elaphureLinkAGDI（Keil 的 "CMSIS-DAP Debugger"）使用：
   - 安装 [elaphureLinkAGDI](https://github.com/fly2046/elaphureLinkAGDI)
   - **将 `ORBMDK_RDDI.dll` 重命名为 `CMSIS_DAP.dll`**，放到 Keil 的 `ARM\BIN\` 目录（如 `D:\Keil_v5\ARM\BIN\`）
   - 必须是 **32 位 (x86)** 构建（`build.ps1` 已默认 x86）；Keil µVision 是 32 位进程，无法加载 64 位 DLL
   - **覆盖前先结束整个 `UV4.exe` 进程**：µVision 会把 DLL 常驻内存，只关调试会话无效

3. 在 Keil 项目中配置调试器：
   - 打开 "Project" → "Options for Target" → "Debug"
   - 选择 "ORBMDK" 或对应的 elaphureLinkAGDI 驱动
   - 配置 SWD 接口和时钟频率

## 验证

```powershell
.\test\build_test.ps1 -All              # 一次重建全部工具（改完 DLL 后常用）
.\test\build_test.ps1 -Source ifacedump.cpp   # 只建单个工具（ifacedump 不在 -All 清单内）

.\bin\ORBMDK_RDDI_FullTest.exe          # 全面功能测试（40 项）
.\bin\swdprobe.exe                      # SWD 回归：ifNo=0 (V2 / Bulk)
.\bin\swdprobe.exe ORBMDK_RDDI.dll v1   # SWD 回归：ifNo=1 (V1 / HID)
.\bin\jtagblockprobe.exe --skip-05 --stage 2  # JTAG 下固件 0x06 通路复验（⚠️ 可能需插拔 USB）
```

测试程序默认从**自身所在目录**加载 `ORBMDK_RDDI.dll`（`build_test.ps1` 把 exe 与 DLL 都输出到 `bin\`），
也可用参数显式指定：`.\bin\ORBMDK_RDDI_FullTest.exe <dll路径>`。

### 测试工具清单（2026-10-03 更新）

| 工具 | 层次 | 用途 / 何时用 |
|------|------|----------------|
| `ORBMDK_RDDI_FullTest.exe` | 走 DLL | 全面功能测试 40 项 + 完整导出扫描；**改完 DLL 先跑它** |
| `ORBMDK_BlockTransferTest.exe <ramAddr>` | 走 DLL | 块传输正确性与哨兵越界验证（flash 下载热路径，§13.7） |
| `ORBMDK_RAM_SpeedTest.exe [ramAddr] [bytes] [rounds] [clockHz] [dll] [windowWords]` | 走 DLL | RAM 读写**吞吐**（绕开 Keil/AGDI，测设备侧极限）；`windowWords` 用来 A/B 切换"按 4 KB TAR 块切分"与"整块一次写完"（后者可复现 B7 缺陷） |
| `swdprobe.exe [dll] [v1]` | 走 DLL | SWD 通路回归：导出自检 / 适配器字段 / 版本串闸门 / 假冒指针回归 / AP 寄存器解码 / halt-PC-RAM / 日志回调，末尾给出 PASS-FAIL 统计。默认 V2，加 `v1` 走 HID |
| `hidprobe.exe` | 走 DLL | HID(V1) 极小回归（比 `swdprobe ... v1` 更快更薄） |
| `jtagprobe.exe` | 走 DLL | JTAG 端到端：`Port=JTAG` → 扫链 → IR 长度 → DP 上电（验证**本层** JTAG 建链） |
| `jtagrawprobe.exe` | 直连 WinUSB | JTAG 原始帧 + **引脚电平读取** + **手工位拷贝扫 TAP** + 时钟扫描；用于判定"固件 / 接线"卡在哪一层（§18.9） |
| `jtagblockprobe.exe [选项]` | 直连 WinUSB | **JTAG 下固件 `0x06` 专项复验**：每步之后立即 Ping，判定"可用 / 未处理但存活 / 无应答"三类。⚠️ **会真的发 `0x06`**，出现无应答时**须重新插拔 USB**；默认任一步失败即停（`--no-stop` 关闭），从 `count=1` 起（`--stage N` 可只跑到第 N 步）。目标上电后**第一次连接就要用 JTAG**（本目标 SWJ-DP 的 `JTAG←SWD` 单向） |
| `v2rawprobe.exe [pad]` | 直连 WinUSB | V2 出包长度与响应分片定标（§17.2 的"整包 = wMaxPacketSize"陷阱） |
| `ifacedump.exe [vid] [pid]` | 只读枚举 | 列出某 VID/PID 下**所有**可见接口（WinUSB / CMSIS-DAP / HID）与端点，判断复合设备到底有几个功能口、哪个才是 DAP 通道。默认 `0D28:0204`。**不发任何 USB 命令、不改设备状态**；不在 `build_test.ps1 -All` 清单里，需 `-Source ifacedump.cpp` 单独构建 |

> **直连工具会独占设备**：`jtagrawprobe` / `jtagblockprobe` / `v2rawprobe` 运行前必须关闭 Keil
> （或让它退出调试会话），否则会与 µVision 抢同一个 WinUSB 接口，两侧都报错。
> `jtagrawprobe` 会在结束前恢复 SWD 并 Disconnect；**`jtagblockprobe` 默认不切回 SWD**
> （本目标 `JTAG←SWD` 单向，切回去本上电周期内就再也进不了 JTAG），需要时用 `--restore-swd`。

**改名对照（旧 → 新，2026-09-30）**：`orbprobe` / `orbprobe2` / `orbprobe3` / `ORBMDK_RDDI_Test`
四者合并为 **`swdprobe`**；`v1probe` → **`hidprobe`**；`v2padprobe` → **`v2rawprobe`**；
`jtagprobe2` → **`jtagrawprobe`**（`jtagprobe` 名字保留给"走本层 DLL"的那个）。
`COMPAT_ANALYSIS.md` 的历史章节保留当时的旧文件名，属现场记录，不再逐一回改。

**新增（2026-10-03）**：`jtagblockprobe`（JTAG 下固件 `0x06` 专项复验）、
`ifacedump`（接口清点）、`ORBMDK_RAM_SpeedTest`（RAM 吞吐）。

在 ORBTrace + STM32F1 目标上**曾**实测 **40/40 全部通过**：

```
DAP_ReadReg(DP_IDCODE)    PASS   IDCODE: 0x2BA01477
DAP_ReadReg(AP_IDR)       PASS   AP IDR: 0x24770011
TEST SUMMARY: PASSED 40 / FAILED 0 / ALL TESTS PASSED
```

> ⚠️ **当前复现结果是 `PASSED 22 / FAILED 20`**，原因是**测试程序自身**的 `DAP_REG_*` 常量与
> `include/ORBMDK_DAP.h` 编号脱节，**不是驱动回归**（见 `COMPAT_ANALYSIS.md` §13.5）。
> 该项已列为 0.5.0 **发版阻塞项** → `Todo.md` §**18.11**。

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

> 上表是 **SWD** 通路（固件透传 `swdIF` 的 ACK）。**JTAG** 通路的 ACK 不进固件的映射，
> 由本层从 TDO 位流自行解码后走同一张表（见下节）。

### JTAG 通路实现方式

探针固件在 JTAG 上只当"通用位流发生器"用：本层只发 `0x14 DAP_JTAG_Sequence` 把 TMS/TDI 位流打到线上、回收 TDO，
选 IR / 组 35 位 DR / 解码 ACK / 处理 posted read 等 ADIv5 语义全部由本层 `src/ORBMDK_RDDI.cpp` 实现
（`DapTransferFor` 按模式分岔：SWD 走固件 `0x05`，JTAG 走本层引擎，故 `0x05`/`0x06` 与块传输在 JTAG 下一概不用）。

### 日志策略

统一实现在 **`src/ORBMDK_Log.cpp`**（接口 `include/ORBMDK_Log.h`）—— 所有模块
（RDDI / HID / BULK / …）共用一套，排障只看一个文件（设计见 `COMPAT_ANALYSIS.md` §8）。

| 项 | 约定 |
|----|------|
| 默认级别 | **ERROR** |
| 行格式 | `[ORBMDK][HH:MM:SS.mmm][级别][模块][PID:TID] 消息` |
| 过滤时机 | 在构造日志字符串**之前**判断级别，被过滤时零开销 |
| 环境变量 | `ORBMDK_LOG_LEVEL`（0–8）、`ORBMDK_LOG_FILE`（日志路径，默认 `%TEMP%\ORBMDK_RDDI.log`）；只在进程启动读一次 |
| 命令级日志 | V1/V2 每条 DAP 命令往返按 **INFO** 级参与级别过滤、只落盘并带时间戳；阈值设为 INFO/DEBUG 时才输出，默认 ERROR 下不落盘 |
| 宿主通道 | `RDDI_SetLogCallback` 已接通：日志会转发给 AGDI/Keil 的日志窗口（级别自动映射） |
| 禁止 | 逐寄存器 / 逐次传输的 INFO 日志、响应十六进制转储 |

```bat
echo 0 > %TEMP%\ORBMDK_LOG_LEVEL
```

> PowerShell 下写级别文件请用 `[IO.File]::WriteAllText("$env:TEMP\ORBMDK_LOG_LEVEL","2")`，
> 不要用 `Set-Content`（PS 5.1 默认 UTF-16，驱动按 ASCII 解析会读不出数字）。

---

## 功能

### 支持的调试协议
- **SWD** (Serial Wire Debug)
- **JTAG** - 扫链 / 多 TAP 器件枚举 / DP+AP 访问；IR/DR 时序由本层位流引擎生成（见["JTAG 通路实现方式"](#jtag-通路实现方式)）
- **USB HID V1** - 标准 CMSIS-DAP
- **USB Bulk V2** - 高速传输 (WinUSB) ✅ 已实现

### 支持的调试功能
- 内存读写（字节、半字、字、块）
- 核心寄存器访问
- 目标复位
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

**传输模式由对话框里的接口选择决定**（没有任何配置开关）：

| 适配器条目（Keil 下拉框里显示的名字） | 传输层 |
|-------------------------------------|--------|
| `CMSIS-DAP v2`（`ifNo=0`） | USB Bulk (WinUSB)，**只用 V2** |
| `CMSIS-DAP v1`（`ifNo=1`） | HID，**只用 V1** |

- 选谁就必须是谁：选中的通道打不开时**直接报错**，绝不静默换到另一条
  （静默降级会把"V2 坏了"伪装成"能用"，详见 `COMPAT_ANALYSIS.md` §17.4）。
- `RDDI_Open` 阶段（用户尚未选定接口）按"V2 优先、不可用才 V1"先打开一条。
- `Firmware Version` 一栏上报**设备自报的 `DAP_Info(0x04)` 协议版本串**，但**主版本被归一化抬到 ≥ 2**
  （设备自报 `2.1.0` → 上报 `2.1.0`；自报 `1.x` → 抬成 `2.x`；问不到设备才兜底 `1.0.0`）。原因：AGDI 对该串
  做 `cmp eax,2`，主版本 ≥ 2 才会进入 SWO 流式的 `TraceTransport=Stream`（还需 caps `0x40`）+ streaming sink
  注册分支 —— 这半门控与 caps `0x40` **必须同侧**，2026-10-01 已一并**放开**（§17.5 / `Todo.md.bak` §18.10-C）。
  设备真实版本原串仍会写进日志。

> V2 的 DAP 命令包按**端点 wMaxPacketSize** 发送（orbtrace 实测 = 64 字节）。
> 出包长度由驱动**自动标定**：先用短包(64)向设备问 `DAP_Info(0xFF)` 拿它自报的
> `packet size`（orbtrace = 508），再用可校验的只读命令验证；不合法就换候选。
> **绝不发长度为端点 `wMaxPacketSize` 的整包** —— 那在 USB 上不是短包，固件会等下一包，
> 命令永远不被派发（实测 512 时"第一条能答、之后永久死"）。详见 `COMPAT_ANALYSIS.md` §17.2。

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

## 开发

### 依赖
- Windows SDK 10.x（路径自动探测，见"构建"）
- Visual Studio 2017+（含 C++ 桌面开发工作负载）
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

## 版本记录

### 0.5.0（2026-10-03）—— 首次文档化发版

**功能**

- V1(HID) / V2(Bulk) 双传输层，Keil 适配器列表里可选可切换；**V2 已实机可用**（完整调试 + 下载）。
- SWD / JTAG 双通路建链（JTAG 只用固件通用位流命令 `0x14`，ADIv5 语义全部在本仓库实现）。
- 块传输（`DAP_TransferBlock`）能力探测 + 逐字校验，**适用 SWD**；JTAG 侧禁用（实测会挂死探针）。
- PC 采样（DWT/ITM）、Trace 解码骨架（ITM / ETM / TPIU）、符号解析（PDB / DWARF）。
- 统一日志子系统：多 Sink、`%TEMP%\ORBMDK_LOG_LEVEL` 热更新、宿主日志回调。
- **78 个** RDDI 导出函数；静态 CRT（`/MT`），产物零运行库依赖。

**已知限制 / 未完成**（完整清单见 [`Todo.md`](Todo.md)）

- `ORBMDK_RDDI_FullTest` 因**测试自身**常量陈旧，复现为 `22/20`（**发版阻塞**，§18.11）。
- `Trace Enable` 走不通：探针固件 1.4.3 **无 SWO 命令族**（定性见 `bug.md` **B12**），需刷 1.4.4。
- 多调试器并存时无法指定目标；设备拔出无探活。
- 并列 Trace 端口 / ETB 在 AGDI v1.33.24 里**无代码路径**（建议结案）。

**兼容性**

- 宿主：Keil µVision + 官方 `CMSIS_AGDI.dll` **v1.33.24**（行为逆向见 [`COMPAT_ANALYSIS.md`](COMPAT_ANALYSIS.md)）。
- 探针固件：**orbtrace 1.4.3**（1.4.4 命令层差异清单见 `bug.md` B10-D）。
- 编译器：MSVC（静态 CRT）；MinGW-w64 GCC 与 VS2013 及更早**不支持**。

> 版本号定义于 `include/ORBMDK.h`（`ORBMDK_VERSION_STRING`）；
> **与"上报给 Keil 的协议版本串"无关**（后者是 CMSIS-DAP 协议版本，来源 = 设备 `DAP_Info(0x04)`，
> 主版本归一化抬到 ≥ 2，见 `COMPAT_ANALYSIS.md` §17.5）。

## 参考资料

- [CMSIS-DAP 规范](https://arm-software.github.io/CMSIS_5/DAP/html/index.html)
- [ARM Debug Interface 规范](https://developer.arm.com/documentation/ihi0031/latest/)
- [ORBTrace 项目](https://github.com/orbcode/orbtrace) —— 探针固件；JTAG 通路只用它的通用位流命令 `0x14`，ADIv5 语义全部在本仓库实现（见["JTAG 通路实现方式"](#jtag-通路实现方式)），固件侧可选改动见 [`PATCH_orbtrace-1.4.3_JTAG_fixes.txt`](PATCH_orbtrace-1.4.3_JTAG_fixes.txt)
- [orbuculum 项目](https://github.com/orbcode/orbuculum) - Trace 解码参考

## 许可证

基于 BSD-2-Clause 许可证（参考 elaphureLink）
