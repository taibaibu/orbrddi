<h1 style="color:red">🔴🔴🔴 最高警告（MAX） 🔴🔴🔴</h1>

<h1 style="color:red">任何偏离 <code>&lt;ORBAGDI 源码树&gt;\AGDI\DbgCM</code> 实现方法的实现皆为风险。</h1>

<h1 style="color:red">PDSC 是 KEIL 核心流程，严禁硬编码，软开关；功能应做就做，行到行兼容。</h1>

---
# ORBMDK - ORBTrace CMSIS-DAP RDDI 驱动层

ORBMDK 提供 RDDI (Remote Debug Driver Interface) 接口，兼容 CMSIS—DAP 调试器能够作为标准 CMSIS-DAP 调试器用于 Keil uVision。

## 架构

```
Keil uVision (IDE)
       │
       ▼ (通过 KEIL AGDI: CMSIS_AGDI.dll)
   ORBMDK_RDDI.dll (RDDI 接口)
       │
       ▼
   ORBMDK_RDDI (src/ORBMDK_RDDI.cpp)   # RDDI 层实现（唯一业务层）
       │
       ├─ ORBMDK_HID.cpp      ── USB HID  (V1) DAP 通道 ──┐
       ├─ ORBMDK_USB_Bulk.cpp ── USB Bulk (V2) DAP 通道 ──┼──→ ORBTrace 探针
       └─ ORBMDK_OrbTrace.cpp ── vendor 接口（trace 通路）┘
                                  Trace 0x54 / Power 0x50 / Version 0x56
                                          │
                                          ▼ (SWD / JTAG / SWO)
                                  目标 MCU 的调试与 trace 字节流
                                          │
                                          ▼
                       RDDI 接口 —— 本层到此为止（只搬运，不解码）
                                          │
                                          ▼
                                AGDI 层（ORBAGDI）做协议与 trace 解码
```

**注意**:
- **AGDI 层 = Keil 的 "CMSIS-DAP Debugger" ；ORBMDK 仅实现其下的 RDDI 驱动层。
- AGDI 层按固定文件名加载 RDDI 层 DLL：Keil 原版 "CMSIS-DAP Debugger" 加载 **`CMSIS_DAP.dll`**。ORBMDK 构建产物为 `ORBMDK_RDDI.dll`，部署时需重命名为实际生效的那个名字（见"安装"）。

## 项目结构

```
ORBMDK/
├── include/                     # 头文件
│   ├── ORBMDK.h                # 主头文件、版本信息、设备 VID/PID 与品牌串
│   ├── ORBMDK_DAP.h            # CMSIS-DAP 协议定义
│   ├── ORBMDK_DAPV2.h          # CMSIS-DAP V2 Bulk 协议定义
│   ├── ORBMDK_HID.h            # USB HID 层定义
│   ├── ORBMDK_Log.h            # 统一日志接口
│   ├── ORBMDK_OrbTrace.h       # orbtrace 配置接口（Trace/Power/Version 端点）
│   ├── ORBMDK_RDDI.h           # RDDI 接口定义（91 个导出）
│   └── ORBMDK_USB_Bulk.h       # USB Bulk V2 传输层定义
├── src/                         # 源代码
│   ├── pch.h / pch.cpp         # 预编译头
│   ├── ORBMDK_DLL.cpp          # DLL 入口点
│   ├── ORBMDK_HID.cpp          # 传输层 V1：USB HID + DAP 命令封装
│   ├── ORBMDK_RDDI.cpp         # RDDI 实现（核心；OrbTrace_* / TraceStream_* 在此薄包装）
│   ├── ORBMDK_Log.cpp          # 统一日志实现（多 Sink / 级别热更新）
│   ├── ORBMDK_OrbTrace.cpp     # 传输层 trace 通路：独立 WinUSB 句柄 + 后台读线程 + 环形缓冲
│   └── ORBMDK_USB_Bulk.cpp     # 传输层 V2：USB Bulk
├── obj/                         # 编译中间文件（build.ps1 运行时生成）
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
├── COMPAT_ANALYSIS.md           # ABI 兼容性结论 · 实现状态
├── bug.md                       # 实现细节 · 缺陷定性 · 排查笔记（JTAG 相关见 B9 / B11.8）
├── Todo.md                      # 未完成项（只列待办，不写实现细节）
├── PATCH_orbtrace-1.4.3_JTAG_fixes.txt  # 固件侧 JTAG 可选修复补丁（基于 orbtrace 1.4.3，含完整代码）
├── Usage.md                     # 驱动的 API 使用手册（快速开始 + 各导出函数调用示例）
├── Todo.md.bak                  # 历史快照（不参与构建 / 部署）
└── LICENSE                      # BSD 2-Clause
```

### 文档导航

| 文档 | 收什么 | 什么时候看 |
|------|--------|------------|
| `README.md` | 构建 / 安装 / 验证 / 不可破坏的实现约束 | 上手、改代码前 |
| `COMPAT_ANALYSIS.md` | ABI 兼容性结论、历史现场记录 | 需要"为什么这么写"的依据 |
| `bug.md` | 实现细节、缺陷定性、排查笔记（JTAG 相关见 B9 / B11.8） | 复现某现象、查根因 |
| `Todo.md` | **只有未完成项** | 想知道还差什么 |
| `Usage.md` | 驱动的 API 使用手册（快速开始 + 各导出函数调用示例） | 想集成、或直接调本层 DLL 时 |
| `PATCH_orbtrace-1.4.3_JTAG_fixes.txt` | 探针固件（orbtrace 门级 Verilog + Amaranth）侧的可选改动 | 想让固件侧 JTAG 通路更完备时 |

## 构建

### PowerShell (推荐)

```powershell
.\build.ps1
```

输出文件：`bin\ORBMDK_RDDI.dll`（MSVC **x86**，无 error / 无 warning）

构建脚本会自动探测工具链，无需手改路径：VS 安装位置走 `vswhere`（再回落常见安装目录），
VC 工具集与 Windows SDK 各取最高版本；链接用 `/MT` 静态 CRT，产物零运行库依赖。

## 安装

```powershell
.\build.ps1 -Deploy                                  # 编译 + 一键部署（自动定位 Keil 的 ARM\BIN）
.\build.ps1 -DeployOnly                              # 只部署，不编译
.\build.ps1 -Deploy -KeilArmBin "D:\Keil_v5\ARM\BIN" # 或显式指定 Keil 的 ARM\BIN
```

脚本会把 `bin\ORBMDK_RDDI.dll` 复制为 Keil 的 `CMSIS_DAP.dll`，并在首次覆盖前把官方原件备份为 `CMSIS_DAP.dll.bak`

手动方式：

1. 编译得到 `bin\ORBMDK_RDDI.dll`
   - **将 `ORBMDK_RDDI.dll` 重命名为 `CMSIS_DAP.dll`**，放到 Keil 的 `ARM\BIN\` 目录（如 `D:\Keil_v5\ARM\BIN\`）
   - 必须是 **32 位 (x86)** 构建（`build.ps1` 已默认 x86）；Keil µVision 是 32 位进程，无法加载 64 位 DLL

2. 在 Keil 项目中配置调试器：
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

> ✅ **2026-10-10 已修复**：此前退化为 `PASSED 22 / FAILED 20` 的根因是 `DAP_REG_*` 常量与
> `rddi_dap.h` 编号语义不符 —— `include/ORBMDK_RDDI.h` 的枚举曾按"字节偏移 + AP 恒带 RnW"写
> （`DAP_REG_DP_CTRL_STAT=0x04`、`DAP_REG_AP_CSW=0x10000`、`DAP_REG_MATCH_RETRY=0x30000` …），
> 与实现 `GetRegId()` 的低 16 位解码不符，已按 ARM `rddi_dap.h` 改回 **0..7 编号**
> （依据见 `COMPAT_ANALYSIS.md` §4.9 / §13.5；`Todo.md` §18.11 已结案）。
> 当前在 **QM-Link CMSIS-DAP + Cortex-M4** 上复现为
> `PASSED 37 / FAILED 0 / NOT IMPLEMENTED 3`（3 项 SWO 因探针自报 caps 无 SWO 位而跳过）。

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
- **trace 字节流搬运**（SWO / orbtrace trace 端点；本层只搬运，不解码）

> **断点 / 观察点 / Flash 编程**由上层 elaphureLinkAGDI 通过组合 `DAP_RegAccessBlock` /
> `DAP_RegReadRepeat` / `DAP_RegWriteRepeat` 实现，不属于 ORBMDK（RDDI 层）的职责。

### RDDI 接口

`ORBMDK_RDDI.h` 中带 `RDDI_FUNC` 的导出函数共 **91 个**，按 `GetProcAddress` 导出：

| 分类 | 数量 |
|------|------|
| RDDI 核心（Open/Close/GetLastError） | 3 |
| DAP 基础 + 寄存器 | 21 |
| CMSIS_DAP_* | 35 |
| PC_*（PC 采样） | 6 |
| StreamingTrace_* | 13 |
| **OrbTrace_* / TraceStream_*（orbtrace 配置 + raw trace 流）** | **12** |
| RDDI_SetLogCallback | 1 |

> 合计以 `tools/pe_re.py exports` 为准（当前 **91** = 上表各项之和）。

其中 elaphureLinkAGDI **实际只调用 15 个**：

| 分类 | 函数 |
|------|------|
| RDDI 核心 | `RDDI_Open`、`RDDI_Close` |
| DAP 寄存器 | `DAP_ReadReg`、`DAP_WriteReg`、`DAP_RegAccessBlock`、`DAP_RegReadRepeat`、`DAP_RegWriteRepeat` |
| CMSIS-DAP | `CMSIS_DAP_Detect`、`CMSIS_DAP_Identify`、`CMSIS_DAP_ConfigureInterface`、`CMSIS_DAP_DetectNumberOfDAPs`、`CMSIS_DAP_DetectDAPIDList`、`CMSIS_DAP_Commands`、`CMSIS_DAP_SWJ_Sequence`、`CMSIS_DAP_SWJ_Pins` |

> 完整的调用点清单（文件:行 + 所在 AGDI 函数）见 [COMPAT_ANALYSIS.md 第二节](COMPAT_ANALYSIS.md)。
> 其余导出（`StreamingTrace_*`、`CMSIS_DAP_SWO_*`、`CMSIS_DAP_JTAG_*`、`CMSIS_DAP_PC_*` 等）服务于其他宿主，不影响 Keil 调试链路。

### Trace 功能（只搬运，不解码）

本层**不内置任何 trace 解码器**（ITM / ETM / TPIU / SWO 解码均已移除，见
[`COMPAT_ANALYSIS.md` §二十二](COMPAT_ANALYSIS.md)）。本层负责把 trace 字节流**搬**到 AGDI 层，
解码由 AGDI 层完成。

| 通路 | 机制 | 状态 |
|------|------|------|
| SWO | CMSIS-DAP `DAP_SWO_*` 命令族（`CMSIS_DAP_SWO_*` 导出） | 已实现 |
| orbtrace trace 端点 | **本层统一管理 USB**：复合设备上 `bInterfaceClass=0xFF` 的三个 vendor 接口按 `bInterfaceSubClass` 分（`0x50` Power / `0x54` Trace / `0x56` Version），与 DAP 句柄并存；trace 由后台读线程 + 环形缓冲搬运，raw 交付（`OrbTrace_*` / `TraceStream_*` 导出） | 配置与读通路已实现（未实机验证），见 §二十三 / [`Usage.md` §10](Usage.md) |

### Trace 解码器（已从本层移除，2026-10-08）

本层**只做传输**（USB HID / WinUSB Bulk + RDDI 接口），不再内置任何 trace 解码器。
原八个模块已删除：

| 原模块 | 去向 |
|--------|------|
| ITM / ETM / TPIU / Trace | 解码归 **AGDI 层**（ORBAGDI 已有 ITM；ETM 已移植） |
| OFLOW / COBS | 本层不做解帧：交 raw 字节，帧同步与校验归 **AGDI 层**（§23.3 定稿） |
| Coverage / Symbols | 依赖 ETM，随解码一起移出 |

`CMSIS_DAP_PC_*` 导出**保留**（ABI 兼容），但不再在本地解码 PC 采样值 ——
原实现用内置 ITM 解码器抽 DWT PC Sample，已随解码链移除。

解码器移除**不影响**既有导出面（`CMSIS_DAP_*` / `DAP_*` / `RDDI_*` / `StreamingTrace_*` 未改名、未删除，
当时仍为 79 个）；随后新增 orbtrace 配置 / raw trace 流 **12 个**导出
（`OrbTrace_*` 7 + `TraceStream_*` 5，2026-10-08 落地；`TraceStream_DroppedBytes` 于 2026-10-13 补，
导出面 79 → **91**，见 [`COMPAT_ANALYSIS.md` §二十三](COMPAT_ANALYSIS.md)）。

### 代码覆盖率分析 / 符号解析（已从本层移除，2026-10-08）

原 `ORBMDK_Coverage.*` / `ORBMDK_Symbols.*` 依赖 ETM 解码结果，随解码一起移出本层。
覆盖率统计与符号解析属"分析"职责，应放在 AGDI 层或独立工具里，不放传输层。

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
- ⚠ 2026-10-08 起**本层不再解码**：原 `ORBMDK_ITM_Decoder` 已随解码器一起移除；
  `CMSIS_DAP_PC_*` 导出保留（ABI 兼容），但不再在本地抽出采样值。解码归 AGDI 层。
- 采样值格式（供 AGDI 层参照）：1-4 字节小端 PC 值；单字节 `0x00` = 睡眠状态

---

## 开发

### 依赖
- Windows SDK 10.x（路径自动探测，见"构建"）
- Visual Studio 2017+（含 C++ 桌面开发工作负载）
- Windows HID API
- WinUSB (USB Bulk V2 传输)
- SetupAPI

### Trace 数据流（本层只搬运，不解码）

```
目标 MCU
   │
   ├── ITM ──┐
   ├── ETM ──┼──→ TPIU ──→ SWO / 并行 Trace 口
   └── DWT ──┘
                          │
                          ▼
                  ORBTrace 探针（gateware）
                          │
   ┌──────────────────────┴─────────────────────────┐
   │ CMSIS-DAP (SWD / JTAG)                          │ trace 端点 (bInterfaceSubClass = 0x54)
   ▼                                                 ▼
ORBMDK_HID.cpp / ORBMDK_USB_Bulk.cpp        ORBMDK_OrbTrace.cpp  ← 后台读线程 + 环形缓冲
   │                                                 │
   └──────────────────────┬──────────────────────────┘
                          ▼
              ORBMDK_RDDI.dll（只搬运，不解码）
                          │
   ┌──────────────────────┴─────────────────────────┐
   │ DAP_* / CMSIS_DAP_* → 目标访问 / 断点 / 下载     │ TraceStream_Read         → raw 字节流
   │                                                │ TraceStream_DroppedBytes → 丢帧字节数
   └──────────────────────┬─────────────────────────┘
                          ▼
   AGDI 层（ORBAGDI）：COBS / OFLOW 解帧 + ITM / ETM / SWO 解码 → 应用数据输出
```

## 版本记录

> 以下各条为**发布当时**的快照（导出数、模块清单以该条为准）；跨版本的最新口径见最新条目与正文。

### 0.6.0（2026-10-14）—— 架构收敛：本层只做传输

**架构**

- 分层固定为：Keil µVision → AGDI（`CMSIS_AGDI.dll`）→ `ORBMDK_RDDI.dll` → 探针。
  本层**只做传输**（USB HID / WinUSB Bulk + RDDI 接口）；全部 trace 解码器
  （ITM / ETM / TPIU / Trace / OFLOW / COBS / Coverage / Symbols）已于 2026-10-08 移除，解码归 AGDI 层。
- 传输层三分支收敛在 `src/ORBMDK_RDDI.cpp` 之下：
  `ORBMDK_HID.cpp`（V1/HID）、`ORBMDK_USB_Bulk.cpp`（V2/Bulk）、`ORBMDK_OrbTrace.cpp`（trace 通路）。
- `ORBMDK_OrbTrace.cpp`：orbtrace 复合设备的三个 vendor 接口由本层**统一管理**
  （独立 WinUSB 句柄，与 DAP 句柄并存）；raw trace 走后台读线程 + 环形缓冲，AGDI 只取字节流。

**接口**

- 导出面 **79 → 91**：新增 `OrbTrace_*` 7 个 + `TraceStream_*` 5 个，其中 `TraceStream_DroppedBytes`
  （环溢出丢帧字节数，2026-10-13 补）是"静默丢数据"的唯一出口。
- 分类计数：RDDI 核心 3 / `DAP_*` 21 / `CMSIS_DAP_*` 35 / `CMSIS_DAP_PC_*` 6 /
  `StreamingTrace_*` 13 / `OrbTrace_*`+`TraceStream_*` 12 / `RDDI_SetLogCallback` 1。

**修复**

- 传输层块传输响应长度校验（`src/ORBMDK_HID.cpp`，2026-10-13）：响应短于 `count` 个字时**报错**，
  不再"拷多少算多少"；一并修掉 V2 侧 IN 读上限误用出包长度导致的"响应尾部混入上一条数据"。

**兼容性**

- 宿主：Keil µVision + 官方 `CMSIS_AGDI.dll` v1.33.24；探针固件：orbtrace 1.4.3。
- 编译器：MSVC（x86，静态 CRT `/MT`），产物零运行库依赖。

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
- 并列 Trace 端口 / ETB 在**原厂** `CMSIS_AGDI.dll` v1.33.24 里**无代码路径**（建议结案）；
  **自研 AGDI 层（ORBAGDI）已实现 ETB 采集**（`etb=1`，走 MEM-AP，不经本层）。
  ETM/ETB 解码不属本层，见 `COMPAT_ANALYSIS.md` §17.9(9)。
- **本层已移除全部 trace 解码器**（ITM/ETM/TPIU/Trace/OFLOW/COBS/Coverage/Symbols，
  2026-10-08），只做 HID/WinUSB 传输 + RDDI 接口；导出面 **79 个未变**。
  显示名（厂商/产品）改为**优先用设备自报串**（HID 产品串 / Bulk 接口串 / `DAP_Info(0x01/0x02)`），
  问不到才回落 `ORBMDK.h` 的 `ORBMDK_DEVICE_VENDOR` / `ORBMDK_DEVICE_PRODUCT` ——
  接其他厂商探针显示名自动跟随（识别本就按能力特征，与 VID/PID 无关）。

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

基于 BSD-2-Clause 许可证
