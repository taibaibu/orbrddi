# ORBMDK 未解决问题清单

- **来源**：① 首轮由 `COMPAT_ANALYSIS.md` §18 迁出（2026-10-01）；该节现为「已完成事项清单」，只留 ✅ 条目。② 二轮迁出（2026-10-01）：§5（stub 清单）、§7.3（符号路径）、§17.7（ETM 立项）整体并入，另将散落在 §4.12 / §13.5 / §18.9 的 4 个待办收进 18.11~18.14 —— 原文各处只留指针
- **优先级**：🔴 待做（影响可用性 / 有价值）· 🟡 可选（不影响当前 Keil 链路）· ⛔ 不做（已决）
- **状态标记**：`[ ]` 未完成 · `[x]` 已完成 · `[-]` 已决定不做（不视为待办）
- **编号**：沿用原文 §18 编号（故不连续）；原文中两处同名 18.7 拆为 `18.7a` / `18.7b`；`18.11`~`18.14` 为二轮迁出时新增（原文无对应编号，就近排）

## 概览

| 编号 | 优先级 | 主题 | 位置 | 状态 |
|------|--------|------|------|------|
| 18.1 | 🔴 | 多台调试器同时插入时无法指定用哪一台 | `CMSIS_DAP_Detect` / `Identify` / `ConfigureInterface`（`src/ORBMDK_RDDI.cpp`）+ 设备枚举 | `[ ]` |
| 18.8 | 🔴 | 界面无法开启 `Trace Enable`（AGDI 门控 / caps 上报） | AGDI trace 配置块 `0x10022839`起 + `CMSIS_DAP_Capabilities` / `Identify(idNo=4)` / `CMSIS_DAP_SWO_*`（`src/ORBMDK_RDDI.cpp`） | `[ ]` |
| 18.10 | 🔴 | 无法开启 ETM 指令跟踪（**结论：Keil + CMSIS-DAP 链路不支持 ETM** —— Trace 页选「并行 Trace 端口」或「ETB」即被 AGDI 判为"不支持的配置"，配置串无对应编码） | `CMSIS_AGDI.dll`：Trace Port 下拉框 / 配置串构建 / streaming sink 注册 / DLL 加载；本层 `src/ORBMDK_RDDI.cpp` / `ORBMDK_HID.cpp` / `ORBMDK_Trace.cpp` / `ORBMDK_ETM_Decoder.*` | `[ ]` |
| 18.10-B | 🔴 | SWO 流式传输立项（原「ETM 前置」**已降级**）：把 SWO 从「轮询 Read」升级为「流式 Stream」并完成 streaming sink 注册，**与 ETM 无关**；P0/P2 已答，P1/P3 待做 | `CMSIS_AGDI.dll` 两处固件版本号门控（`cmp eax,2`）+ sink 描述记录区；`tools/pe_re.py`、ProcMon | `[ ]` |
| 18.10-C | 🔴 | 路线 1 完整方案：放开版本号到 2.x + 补齐流式语义（开关化 / 先探测后实现 / 一键回退；6 阶段 + 回归矩阵） | `src/ORBMDK_RDDI.cpp` / `ORBMDK_HID.cpp` / `ORBMDK_USB_Bulk.cpp` / `ORBMDK_Trace.cpp` + `include/ORBMDK.h` 开关 | `[ ]` |
| 18.2-遗留 | 🟡 | 块传输测试 `DHCSR` 回读为 0（非阻塞遗留观察） | `test/ORBMDK_BlockTransferTest.cpp` | `[ ]` |
| 18.5 | 🟡 | 简化实现（只在跑 trace / sequencer 时才需补） | `src/ORBMDK_RDDI.cpp`、`src/ORBMDK_HID.cpp`、`src/ORBMDK_Trace.cpp`、ETM v4 适配层 | `[ ]` |
| 18.6 | 🟡 | `Firmware Version` 一栏的显示 | `CMSIS_DAP_Identify(idNo=4)` 上报串 | `[ ]` |
| 18.11 | 🟡 | 测试程序 `DAP_REG_*` 常量陈旧（`PASSED 22 / FAILED 20`） | `test/ORBMDK_RDDI_FullTest.cpp` | `[ ]` |
| 18.12 | 🟡 | `CMSIS_DAP_GetNumberOfDevices` 在 JTAG 路径的语义待复核 | `src/ORBMDK_RDDI.cpp` | `[ ]` |
| 18.13 | 🟡 | 是否对外宣称 `INFO_CAPS_JTAG`（待评估） | `CMSIS_DAP_Capabilities` 上报位 | `[ ]` |
| 18.14 | 🟡 | 设备拔出后适配器下拉框仍显示（`IsConnected` 无探活） | `ORBMDK_HID_IsConnected()`（`src/ORBMDK_HID.cpp`） | `[ ]` |
| 18.7a | ⛔ | ULINK+ 专有接口（`ULINKPLUS_*`） | —（不实现） | `[-]` |
| 18.7b | ⛔ | §7.3 符号解析候选搜索路径 | `src/ORBMDK_Symbols.cpp` | `[-]` |

---

## 🔴 待做

### [ ] 18.1 多台调试器同时插入时无法指定用哪一台

- **位置**：`CMSIS_DAP_Detect` / `CMSIS_DAP_Identify` / `CMSIS_DAP_ConfigureInterface` / `RDDI_Open`（`src/ORBMDK_RDDI.cpp`）+ 设备枚举（`src/ORBMDK_HID.cpp`、`src/ORBMDK_USB_Bulk.cpp`）
- **现状**：AGDI 的 `rddi_Open(pDetails)` 恒为 NULL，本层没有任何"用哪台设备"的信息可传；两台同型号（orbtrace）同时插入时只能取枚举顺序第一台，界面上两条目完全相同，用户无法选择（原 `ORBMDK_SERIAL` 开关已按"不留隐藏开关"原则删除，§17.4）。
- **根因**（三层各缺一块）：
  1. **AGDI 不传设备信息**：`rddi_Open(pDetails)` 恒为 NULL（§2.1 实测），RDDI 层拿不到"用户想用哪台"。
  2. **`RDDI_Open` 只能"哪个能用用哪个"**：按 V2(Bulk) 优先、失败退 V1(HID) 打开，且**不带序列号**，故恒落在枚举顺序第一台。
  3. **接口编号只编码"传输方式"**：`kTransportInterfaceCount = 2`（`ifNo 0 = v2/Bulk`、`1 = v1/HID`）；`Identify` 仅按 `ifNo` 换名字，`ConfigureInterface` 仅切传输层 —— 同一台的两个接口可区分，**两台设备之间没有任何区分位**。
- **可复用能力**（底层已支持"按序列号选设备"，只差上层没用）：
  - `src/ORBMDK_HID.cpp`：`EnumerateDevices(serials)` 已能列出 CMSIS-DAP VID 下各设备的序列号；`ORBMDK_HID_OpenDevice(const char* serial)` 已按序列号筛选打开。
  - `src/ORBMDK_USB_Bulk.cpp`：`_findAndOpenDeviceInGuid(..., const char* serial, ...)` 已实现序列号筛选（注释所述"多调试器场景"即指此），仅在未指定序列号时取第一台；序列号由 `_readDeviceSerial` 读 `iSerialNumber` 得到。
  - 附带一处**过期注释**：`_findAndOpenDevice` 的注释仍写"序列号筛选尚未实现"，与代码不符（纯注释问题，非缺陷）。
- **方案**：
  1. 枚举所有同型号设备的序列号（HID + V2 两路），**按序列号字典序排序**得到稳定索引；
  2. `CMSIS_DAP_Detect` 返回 `N × 2`（`n=0` 时仍报 1），并按 AGDI 上限 16 项封顶；
  3. `Identify(idNo=2/3)`：`dev = ifNo / 2`、`transport = ifNo % 2`，`idNo=2` 返回 `CMSIS-DAP v2 (SN)` / `v1 (SN)`，`idNo=3` 返回序列号；
  4. `ConfigureInterface(ifNo)`：把 `(序列号, 传输)` 记入 `ctx`，按序列号去开设备（HID 走 `ORBMDK_HID_OpenDevice(SN)`，V2 走带 SN 的 Bulk 打开）；
  5. `RDDI_Open`：若已记录目标序列号（如工程里保存过的选择），按它打开而非第一台；已连同一台时复用，避免反复 Open/Close。
- **风险点**：
  - AGDI 遍历适配器条目上限 16 项（`0x10022084 cmp esi,0x10`）→ 超额需截断策略（如只列前 8 台的 v2 条目）；
  - 条目顺序必须稳定，否则工程里保存的选择会串位 → 必须用序列号排序，**不能依赖 SetupDi 枚举顺序**；
  - 按序列号打开会多一次设备查询/打开，需确认 V2 下不与其他进程占用冲突。
- **验收**：两台同型号设备同时插入时，对话框出现可区分的（带序列号）条目，选中其一后只与该台通信；拔插后条目顺序保持不变。

### [ ] 18.8 界面无法开启 `Trace Enable`（Trace 页勾选无效）

- **位置**：Keil 端 `CMSIS_AGDI.dll` 的 trace 配置块（`0x10022839` 起）；RDDI 侧 `CMSIS_DAP_Capabilities`（caps 上报）、`CMSIS_DAP_Identify(idNo=4)`（固件版本）、`CMSIS_DAP_SWO_*`（`src/ORBMDK_RDDI.cpp`）。
- **现状**：Debug → Trace 页勾选 `Trace Enable`、选择 SWO 模式后，调试会话内 trace 不工作（无数据 / 无窗口）。注意：同一份 AGDI 二进制里 trace 接口**已全部动态绑定**（第二轮 `GetProcAddress`，见下"绑定事实"），故开关不在"接口缺失"，而在**运行时门控**。
- **绑定事实**（静态实测，`CMSIS_AGDI.dll v1.33.24.0`）：
  - 第一轮加载 `0x1002C200`–`0x1002C660`，落 4 个 SWO 槽：`CMSIS_DAP_SWO_Data`=`0x10362FC8`、`SWO_Mode`=`0x10362FBC` 等（`0x10362FBC`–`0x10362FC8`）。
  - 第二轮加载 `0x1003CC80`–`0x1003CEF1`，把 13 个 `StreamingTrace_*` 全部绑到槽 `0x10362FDC`–`0x1036300C`（落表点 `0x1003CD28`–`0x1003CDFB`）。
  - 两组都有真实 `call [slot]` 调用点，例：`0x1003D094 call dword ptr [0x10362FBC]`。
  - trace sink 名恒为 `"cmsis_dap_swo_trace"`（串 `0x101EFA30`，匹配处 `0x1003D3B0`）。
- **根因**（AGDI trace 配置块反汇编，逐条门控）：
  1. `0x10022858`：`CMSIS_DAP_Capabilities(handle, ifNo, &caps)`，caps 落在 `0x102F8B90`（BSS 零填充区，故 `hex`/`refs` 扫不到属正常）。
  2. `0x10022870`：取 `Identify(idNo=4)` 的 **major**（固件版本主版本），`cmp eax,2 / jb 0x10022897` —— **`major < 2` 只是跳过"caps → transport（Read/Stream）"的判定**（caps 本身在 `0x10022858` 已取到并落 `0x102F8B90`）；`jb` 落点紧接 `0x1002289C` 的表 1/表 2 安装，**该安装不受 major 影响**（详见 §18.10 结论 4）。
  3. `0x1002287A`：`test byte ptr [0x102F8B90], 0x40`（`INFO_CAPS_SWO_STREAMING_TRACE`）→ 决定传输方式写入 `[0x10304B79]`（`0`=Read、`1`=Stream）。
  4. `0x100229FD`：`test byte ptr [0x102FA6C8], 1` —— **★ Trace Enable（UI 勾选状态）**；未勾 → 直接写 `Trace=Off;...`。
  5. `0x10022A06`：`movzx` `[0x102FA6D0]` = **Trace Port**（`1`=Manchester、`2`=UART）。**注意 UI 上该下拉框有 6 项**（索引 `0/1/2`=并行 1/2/4-bit、`3`=Manchester、`4`=UART/NRZ、`5`=ETB，写入值 `0x0100`/`0x0200`/`0x0800`/`1`/`2`/`3`），**只有 `1`/`2` 能被编码**，其余取值直接 `0x2024` 返回（详见 §18.10 结论 1）。
  6. `0x10022A2F`：`test caps, 0x04`（`SWO_UART`）；`0x10022A53`：`test caps, 0x08`（`SWO_MANCHESTER`）。
- **关键结论**：**`Read` 传输只需 `caps & 0x04`（SWO_UART）或 `caps & 0x08`（Manchester）**，**不要求 `major ≥ 2`、也不要求 `caps & 0x40`**。配置串模板：
  - `0x101EB3DC` `Trace=SWO-UART;TraceTransport=Read;`
  - `0x101EB400` `Trace=SWO-UART;...Stream;`
  - `0x101EB428` `Trace=SWO-Manchester;...Read;`
  - `0x101EB454` `Trace=SWO-Manchester;...Stream;`
  - `0x101EB480` `Trace=Off;TraceBaudrate=0;TraceTransport=None;`
  → 故"开不了 trace"最可能落在 **RDDI 侧 caps 位没按预期对上**（再叠加 UI 勾选位 `0x102FA6C8` / 端口位 `0x102FA6D0`）。
- **方案**（RDDI 侧，`src/ORBMDK_RDDI.cpp`）：
  1. 让 `CMSIS_DAP_Capabilities` 正确上报：至少置 `bit2=SWO_UART`(`0x04`)；Manchester 设备再置 `bit3`(`0x08`)；确需 Streaming 才置 `bit6`(`0x40`)。
  2. `Identify(idNo=4)` 固件版本主版本保持 = 1（当前上报 `1.0.0`，钉住即可；一旦放开到 `2.x` 会切走 AGDI 另一分支，须 A/B 复验）。
  3. 复核 `CMSIS_DAP_SWO_Data` / `SWO_Mode` / `SWO_Baudrate` / `SWO_Control` 四槽（`0x10362FBC`–`0x10362FC8`）在 `Read` 传输下的实际落地路径（`src/ORBMDK_Trace.cpp`）。
- **验收**：Keil Trace 页勾选 `Trace Enable` 后能选到 SWO-UART / Manchester，进入调试后 sink `cmsis_dap_swo_trace` 有数据、`Trace=...;TraceTransport=Read;` 生效。
- **附**：本轮 `tools/pe_re.py` 已升级出 `hex` / `refs` / 多段 `slots` / `scan` 子命令（`scan` 刻意走字节模式、不用线性反汇编）；本次全部槽表（103 个）与两轮绑定表由 `scan` 自动兜出，可复现。

### [ ] 18.10 无法开启 ETM 指令跟踪（结论：Keil + CMSIS-DAP 链路不支持 ETM）

- **现象**：µVision 的 Trace 配置页里 SWO 能正常打开，但选择「并行 Trace 端口（Sync Trace Port，1 / 2 / 4-bit）」或「ETB（片上 Trace 缓冲）」后无法调试 / Trace 窗口始终无数据。
- **结论（2026-10-01 静态分析 `CMSIS_AGDI.dll v1.33.24.0`）**：**ETM 在这条链路上没有通路**。卡点既不在本层（ORBMDK / CMSIS-DAP 适配层），也不在固件版本号 `major`，而是 **AGDI 自身不认这个选项** —— 属架构限制。下列三条证据相互独立：

**证据 1：µVision 的 Trace Port 选项里，只有 SWO 能落地成配置串**
- AGDI 把 Trace 页的选择编码成一行 RDDI 配置参数串（`Trace=…;TraceTransport=…`）。Trace Port 下拉框共 6 项，写入的选择值依次为：并行 1-bit / 2-bit / 4-bit（`0x0100` / `0x0200` / `0x0800`）、SWO-Manchester（`1`）、SWO-UART/NRZ（`2`）、ETB（`3`）。
- 编码函数**只识别 SWO 两项的低字节值 1 / 2**（分别对应 `Trace=SWO-Manchester`、`Trace=SWO-UART`，且各需相应能力位 `0x08` / `0x04`）；**其余取值一律直接返回错误码 `0x2024`（不支持的配置）**，`ConfigureInterface` 都不会被调用 → 三项并行 Trace 与 ETB **必然走这条拒绝分支**。
- AGDI 内建的配置串模板只有 5 个：SWO-UART × {轮询 Read，流式 Stream}、SWO-Manchester × {Read, Stream}、`Trace=Off`。**没有任何并行 Trace / ETM / ETB 的表达形式** → 协议层就无法描述 ETM，这是最硬的一层门。

**证据 2：`StreamingTrace` 这套接口是「SWO 流式传输」，不是 ETM 通路**
- 注册过程即：取固件主版本 → 版本 `< 2` 直接返回成功但什么都不做 → 已注册过则退出 → 填 8 条 sink 描述记录（每条内嵌 2 块 128 字节缓冲区）→ 把 13 个 `StreamingTrace_*` 函数指针绑定到固定槽位 → 校验 6 个必填槽 → `StreamingTrace_Connect` → `GetSinkCount` → 逐条 `GetSinkDetails`；任一失败返回 `0x2028`。
- 旧文档把「`StreamingTrace` 函数指针表」当成 ETM 通路（"ETM 走 StreamingTrace 表"）是核心误判，已作废（详见 18.10-B 风险点）。

**证据 3：ETM / TPIU 字符串只是 CoreSight 组件识别用的名字表**

- AGDI 里确实存在 "ETM (Cortex-M3)"、"TPIU" 等字符串，但唯一引用点是**读取目标 ROM 表、自动识别 CoreSight 组件**（供 Trace 配置页显示），不参与 trace 数据采集。

**关于固件版本号 `major ≥ 2`（澄清：它不是 ETM 的前置）**

- `Identify(idNo=4)` 上报的主版本号 ≥ 2 时，AGDI 才允许两件事：① 把 SWO 传输方式从轮询升级为流式（`Read` → `Stream`）；② 注册上面那套 streaming sink。
- 那两个函数指针表（含 `StreamingTrace` 表）是**无条件安装**的，与 `major` 无关 → 旧文档「`major < 2` 会跳过换表、所以 ETM 进不去」的说法作废。
- 也就是说：放开 `major` 只能换来 SWO Stream（吞吐提升），**换不到 ETM**；且会与 §18.6「版本号钉在 1.0.0」的决策冲突。

**附带澄清：错误码 `0x2029` 的来源**

- `0x2029` = AGDI 加载同目录 `CMSIS_DAP.dll` 失败（`LoadLibraryA` 返回 NULL）。该加载函数**同时负责绑定全部 RDDI / DAP 函数指针**，单 DAP 路径（`major=1`，现网可用）也必须走它 → 若真失败现网调试也会挂，故 18.10-B 的「假说 1」基本可排除。
- 本层 `StreamingTrace_*` 的 SWO 流式路径确实尚未实现（`Connect` 只记模式、`Start(2)` 直接返回失败、`GetSinkDetails` 返回"未实现"），但这属于「SWO Stream 性能项」，**不是 ETM 的前置** → 归入 18.10-B。
- **本层待办**：无 —— Keil 内 ETM 不可达，不属本层（适配层）可补范围。
- **替代方案**：SWO + ITM(printf) + DWT PC 采样（§18.8 已可开）—— 能得到 printf 输出与热点近似分析，**得不到**逐条指令流与代码覆盖率。
- **ETM 的可行路线（退路）**：脱离 Keil，用独立上位机 + 探针硬件并行 Trace 采集（配合 `ORBMDK_ETM_Decoder` / `ORBMDK_Coverage`）。前提：① 目标 MCU 自带 ETM（Cortex-M3 / M4 / M7 等，M0 / M0+ 没有）；② 目标的多根 Trace 引脚已物理接到探针 Trace 口；③ 探针硬件具备并行 Trace 捕获能力。ETM 属**并行 Trace**（走 TPIU 的 1/2/4-bit 数据口），**不是 SWO 单线** —— SWO 只承载 ITM/printf 与 PC 采样，单线里没有 ETM 指令流。
- **验收**：本条按「**Keil 内不可达**（AGDI / 协议层不支持，选中并行或 ETB 项即返回 `0x2024`）」关闭；ETM 需求另立项（立项前先确认上述硬件前提）。
- **证据地址（供复核）**：Trace Port 下拉框 `0x1003BD60`（跳表 `0x1003BE1C`，显示文字 `0x10367730`）；配置串构建 `0x10022A06`–`0x10022A77`（错误码 `0x2024`）；配置串模板 `0x101EB3DC` / `0x101EB400` / `0x101EB428` / `0x101EB454` / `0x101EB480`；streaming sink 注册 `0x1003CB90`–`0x1003CEFD`（sink 名 `0x101EFA30`，失败码 `0x2028`）；RDDI 模块加载 `0x1002C1B0`（`0x2029` @`0x1002C235`）；ETM / TPIU 名表 `0x101E937C` / `0x101E93F0`（唯一引用 `0x1003ED03`）。
- **支撑材料**：`COMPAT_ANALYSIS.md` **§17.7**（原「多 DAP 分支」专章）已整体并入 18.10-B；「为何现在必须把版本号钉在 1.0.0」见 **§17.5 / §17.6**。

#### [ ] 18.10-B 立项：启用 SWO 流式传输（Stream 传输 + 流式 sink 注册）—— 由「ETM 前置」降级

- **背景与目标**：18.10 已确认 Keil 内 ETM 不可达，故本项**不再是 ETM 的前置**，而是独立的 SWO 性能项 —— 把 SWO 采集从「轮询读取（Read）」升级为「流式传输（Stream）」，并让 µVision 完成 streaming sink 注册。
- **成功标志**：
  1. `Identify(idNo=4)` 上报 `2.x` 时，Keil 能完整走完 `Open → Detect → Identify → ConfigureInterface → Connect → GetNumberOfDevices → GetDeviceIDList → ConfigureDebugger → 进入调试`（即 §17.5 的 B 臂序列），不再出现 `RDDI-DAP Error`；
  2. `StreamingTrace_Connect` / `GetSinkCount` / `GetSinkDetails` 全部返回 0（本层现为骨架，会返回 `0x2028`）。
- **为什么门槛是固件主版本 `major ≥ 2`**：该门槛控制两件事 —— ① 把能力位 `INFO_CAPS_SWO_STREAMING_TRACE(0x40)` 翻译成"传输方式 = Stream"；② 允许注册 streaming sink。门槛判断排在能力位判断**之前**，所以「`major` 保持 1、只加能力位 `0x40`」取不到 Stream。两个函数指针表与 `major` 无关（本来就安装，见 18.10 澄清）。
- **已排除的缺口**：§16.2（#48~#51）已把该路径会用到的接口按 ABI 补齐（`DAP_GetDAPIDList` 双解释、`GetIDCODEs` 改走 `dapIdList`、`DetectNumberOfDevices` 真实计数、`ConfigureDebugger` / `ResetDAP` 均已导出，详见下方接口账）。§17.5 的 A 臂是在那一轮**之后**跑的、**仍然失败** → **缺口不在这批接口的 ABI**。
- **未定论的关键问题（先探测，别改代码）**：

| 步骤 | 动作 | 工具 | 判据 / 产出 |
|------|------|------|-------------|
| **P0** ✅**本轮已答** | ~~反汇编那张表的落点~~ → 该落点**不是函数指针表**，而是 **8 条 sink 描述记录**（每条内嵌 2 块 128 字节缓冲区），由 `StreamingTrace_GetSinkDetails` 填充；13 个 `StreamingTrace_*` 函数指针绑定到固定槽位 | `tools/pe_re.py` | 注册链路真正依赖的导出 = **6 个必填槽**，本层**均已导出** → 失败不是"缺函数" |
| **P1** | **验证假说 1（已大幅降级，仍实测一次）**：用 ProcMon 观察 AGDI 是否成功加载同目录 `CMSIS_DAP.dll`、µVision 是否报 `0x2029` | ProcMon + µVision 日志 | 假说 1 为真 → 本项判死（转退路）；为假 → 本项可做，重点转 P3 |
| **P2** ✅**本轮已读** | ~~反汇编另一个分支的代码~~ → 那是「Read / Stream 选择」的汇合点，紧接着**无条件**安装两个函数表：单 DAP 与多 DAP 在此处只差"传输方式"，与 `major` 无关 | `tools/pe_re.py dis` | 旧假设「A 臂因跳过换表而失败」作废 |
| **P3** | A 臂下把本层日志开到 `VERBOSE`，与 B 臂（§17.5）做**接口调用差集** | `ORBMDK_LOG_LEVEL` | 定位第一个"没被调用 / 被调但失败"的函数 |

- **两个待验证的假说**：
  - **假说 1（基本可排除）**：AGDI 加载同目录 `CMSIS_DAP.dll` 失败 → `0x2029`。该加载函数同时绑定全部 RDDI / DAP 函数指针，单 DAP 路径（`major=1`，现网可用）也依赖它 → 若真失败，现网调试也会挂。**故 A 臂失败更可能是会话建立失败**（会话句柄为空 → `0x200C`），与"本层把自己部署成 `CMSIS_DAP.dll`"无关。
  - **假说 2（保留）**：放开 `major` 后 AGDI 走的初始化路径更长（两个函数表 + streaming sink），其前置条件与单 DAP 路径不同。A 臂"连设备枚举都没做"= 门槛未过 → 属本层可补范围。
- **风险点**：
  - 与 **18.6** 冲突：18.6 要求把版本号钉在 1.0.0，本项要求放开到 2.x → 必须**同批**决策，不能只改一处；
  - 放开后实际新增的行为：① 传输方式切换为 Stream（换 4 个 SWO 数据槽）；② streaming sink 注册往返（本层为骨架 → 会直接返回 `0x2028`）。**回归面大**（设备列表、Flash 下载、JTAG 链路都要重测）；
  - 旧说法「放开 `major` 会解锁另一套驱动表」**作废** —— 那两张表本来就安装。
- **门槛（固件主版本 ≥ 2）的两处判定**：

| 判定点 | 位置 | 条件 | 通过后做什么 |
|--------|------|------|--------------|
| 第一处 | 会话建立 / 配置路径 | 固件主版本 ≥ 2 | 安装两个函数表（与 `major` 无关，本就执行）；若能力位含 `0x40`，则把传输方式置为 Stream |
| 第二处 | 第二轮 `GetProcAddress` 之前 | 固件主版本 ≥ 2 **且** sink 尚未注册 | 填 8 条 sink 描述记录 → 绑定 13 个 `StreamingTrace_*` → `Connect` / `GetSinkCount` / `GetSinkDetails`（失败 `0x2028`） |

  版本号来源：`Identify(idNo=4)` 上报的 `x.y.z` 字符串经 `sscanf("%lu.%lu.%lu")` 解析，**只取主版本号**。

- **A 臂（`major=2`）的中断现象**：`Open → Detect(2) → Identify → ConfigureInterface → Connect`（DPIDR 读到 `0x2BA01477`）→ **再读 3 条寄存器** → `Disconnect` / `Close`；**没有** `GetNumberOfDevices` / `GetDeviceIDList` / `ConfigureDebugger`。两个假说并不互斥，但 `0x2029` 只可能来自那一次 DLL 加载（两臂共用），故优先按假说 2 排查：A 臂应在 `Connect` 之后被判失败（会话错误一类），而非加载失败。
- **验收**：P1 定论并落档；若本项可行 → 复刻 §17.5 B 臂序列，且能力位 `0x40` 时传输方式为 Stream、sink 注册链路全 0 返回；若判死 → 本项标 `[-]`，18.10 收敛到「替代方案（SWO Read）」。

- **接口账**（长时间初始化路径会用到的函数，用于 P0 对照）：

| 函数 | 功能 | 现状 | 是否够用 |
|------|------|------|----------|
| `DAP_GetNumberOfDAPs` | 返回 DAP（调试端口）数量 | 恒返回 1，不访问目标 | ⚠️ 单目标应为 1，未必是失败点，但语义上不查询真实值 |
| `DAP_GetDAPIDList` | 返回 DAP ID 列表 | 兼容两种 `sizeOfArray` 解释（§16.2 #48） | ✅ 已修 |
| `CMSIS_DAP_DetectNumberOfDevices` | 探测目标数量 | 空表时主动探测，返回真实数量 | ✅ |
| `CMSIS_DAP_GetDeviceIDList` / `DetectDAPIDList` | 返回设备 IDCODE 列表 | 回写**真实 IDCODE**（不再走仅 JTAG 可用的老路径，§16.2 #49） | ✅ |
| `CMSIS_DAP_JTAG_GetIDCODEs` | JTAG 扫描 IDCODE | 优先用已探测列表填充；"扫不到"仍视为成功且数量为 0 | ✅ |
| `CMSIS_DAP_ConfigureDebugger` / `ResetDAP` / `ConfigureDAP` | 调试器 / DAP 配置与复位 | 均已导出（§10.5(4) 的四个空指针检查都通过） | ✅ |

- **三条解法路线**（互斥）：

| 路线 | 做法 | 结论 |
|------|------|------|
| **1（顺水推舟）** | 放开版本号到 2.x，并补齐接口账里尚缺的语义，使 AGDI 的长时间初始化路径（两个函数表 + streaming sink）能完整跑完 | 若 P1 证明假说 1 为假，走这条 —— **完整方案见 18.10-C**。**代价**：与 18.6「钉 1.0.0」直接冲突，且须整体回归（设备列表、Flash、JTAG） |
| **2（绕开门槛）** | 不放版本号，只在 `major=1` 下置能力位 `0x40` 取得 Stream | ❌ **已证死**：门槛判断排在能力位判断之前 |
| **3（不碰 AGDI）** | 放弃 Keil 内 ETM，改用独立上位机 + 硬件并行 Trace（SWO 仍留在 Keil） | ETM 已按此路线收敛（18.10 结论）；本项若判死也走这条 |

- **证据地址（供复核）**：两处版本号判定 `0x10022870` / `0x1003CB92`；两个函数表 `0x1003D030` / `0x1003D0F0`；sink 描述记录区 `0x10363020`+`i*0x20`（内嵌缓冲 `0x10363120` / `0x10363520`…）；`StreamingTrace_*` 槽位 `0x10362FDC`–`0x1036300C`；版本号解析 `0x10021FB0`；DLL 加载 `0x1002C1B0`。

#### [ ] 18.10-C 完整方案：放开版本号到 2.x + 补齐流式语义（= 18.10-B 路线 1 落地）

**目标**：让 `Identify(idNo=4)` 上报 `2.x` 后，AGDI 的长时间初始化路径（两个函数表 + streaming sink 注册）能完整跑完，并真正用上 `TraceTransport=Stream`。

**成功判据（三条同时成立）**：
1. §17.5 的 B 臂序列能完整走完（Open → … → ConfigureDebugger → 进入调试），且 AGDI 下发的配置串里出现 `TraceTransport=Stream;`；
2. sink 注册链路全部返回成功：`StreamingTrace_Attach` → `Connect` → `GetSinkCount` → `GetSinkDetails` → `Start`（现在 `GetSinkDetails` 返回"未实现" → `0x2028` 中止初始化）；
3. Trace 窗口有真实数据（ITM / printf 经 `cmsis_dap_swo_trace` sink 流出）。

**三条总原则（先立规矩再动手）**：
- **一个开关管两处**：版本号与能力位 `0x40` 必须由同一个编译期开关（`ORBMDK_SWO_STREAM`）控制，不允许出现"上报 2.x 但 caps 里没有 Stream"或反之；
- **先探测后实现**：AGDI 侧的参数语义（`Connect` 的 mode 值、`GetSinkDetails` 的参数含义、`WaitForEvent` 的 eventType 约定、`SubmitEventBuffer` 的缓冲归属、`Get/SetConfigItem` 的 item 编号）**必须先反汇编确认再写代码** —— 现有代码里那套 0 / 1 / 2 三档是猜的；
- **可一键回退**：回退 = 改回 `1.0.0` + 不置 `0x40`；RDDI 侧不留状态残留。

**阶段 0 · 探测（先定论，不改行为）**

| 编号 | 动作 | 工具 | 判据 |
|------|------|------|------|
| P1 | 沿用 18.10-B：观察 AGDI 是否成功加载同目录 `CMSIS_DAP.dll` | ProcMon | 假说 1 为真 → 本方案判死 |
| P4 | 反汇编 sink 注册块与各调用点，确认 4 个参数语义（见下表） | `tools/pe_re.py slots/dis` | 得到参数语义表，作为阶段 2 的输入 |
| P5 | A 臂 + 本层 `VERBOSE` 日志，与 B 臂做调用差集 | `ORBMDK_LOG_LEVEL` | 定位第一个"没被调用 / 被调但失败"的函数 |
| P6 | 抓 AGDI 下发的配置串 | 本层日志 | 确认放开后是 `TraceTransport=Stream;` 而非 `Read;` |
| P7 | 探针固件是否支持 V2 流式 SWO 命令 `0xB0`–`0xB3` | 发一条 `0xB0` 看应答 | 决定阶段 3 走 A 还是 B |

P4 必须确认的四件事：

| # | 待确认 | 为什么关键 |
|---|--------|------------|
| 1 | `StreamingTrace_Connect(handle, sinkName, mode)` 的 `mode` 实际取值；`sinkName` 是否恒为 `"cmsis_dap_swo_trace"` | 决定 mode 语义表、以及要不要校验 sink 名 |
| 2 | `StreamingTrace_GetSinkDetails(handle, r1, r2)` 两参是"索引 + 调用方记录指针"还是两个标量；官方实现不写调用方缓冲，那 8 条记录究竟由谁填 | **注册能否通过的胜负手**；猜错会崩溃（历史上 3 参 / 6 参就崩过一次，见 `include/ORBMDK_RDDI.h` 注释） |
| 3 | `StreamingTrace_WaitForEvent` 的 `eventType` 期望值（现实现只会给 0 / 1）与超时语义 | 决定流式数据循环怎么写 |
| 4 | `StreamingTrace_SubmitEventBuffer(handle, buffer, size)` 的 buffer 是"AGDI 提供的落点"还是"事件数据" | 决定要不要真接收 |

**阶段 1 · 版本号与能力位（开关化）**
- 位置：`src/ORBMDK_RDDI.cpp`（`kDriverFirmwareVersion`、`CMSIS_DAP_Capabilities`）、`include/ORBMDK.h`（新增开关宏）。
- 改法：
  1. 新增编译期开关 `ORBMDK_SWO_STREAM`（默认关）：关 → 版本 `1.0.0`、caps 不置 `0x40`（即现状）；开 → 版本 `2.0.0`、caps 追加 `INFO_CAPS_SWO_STREAMING_TRACE(0x40)`。
  2. 版本串**统一成一个来源**，消灭三处硬编码（`src/ORBMDK_RDDI.cpp` 的常量、`src/ORBMDK_HID.cpp` 的 `g_firmwareVersion`、`src/ORBMDK_USB_Bulk.cpp` 的 `CMSIS_DAP_V2_GetInfo`）；设备自报串仍只写日志、不向宿主暴露。
  3. 验证期 A/B 用环境变量 `ORBMDK_FWVER`（与 `ORBMDK_LOG_LEVEL` 同一机制），**只在验证期存在**，定论后按 §17.5 原则删除。
- ⚠️ **只放开版本号一定失败**：第二处门控只看版本号，AGDI 必然去注册 sink，而 `GetSinkDetails` 未实现 → `0x2028` 中止。故阶段 1 不可单独发布。

**阶段 2 · streaming sink 补齐（RDDI 侧语义，`src/ORBMDK_RDDI.cpp`）**

| 函数 | 现状 | 要改成 |
|------|------|--------|
| `Attach` | 只 Init + 置标志 | 校验 sink 名（恒为 `cmsis_dap_swo_trace`，容忍 NULL 但记警告）；建会话、引用计数 +1 |
| `Detach` | 直接关子系统 | 引用计数 -1，归零才真正关闭；清空环形缓冲 |
| `Connect` | 忽略 sinkName，只认 mode 1 / 2 | 按 P4 解析 mode：SWO 接受；ETM 一律**明确拒绝**，不假装成功 |
| `GetSinkCount` | 恒返回 2（SWO + ETM） | 改为**只报 SWO = 1**（报 2 会让 AGDI 去取一个不存在的 sink） |
| `GetSinkDetails` | 恒返回"未实现" | 按 P4 实现：先"只填必需字段 + 返回 0"的最小版，再补字段 |
| `Start` | 只发 Transport + Control | 补全序列：`SWO_Transport` → `SWO_Mode`（**当前漏了**）→ `SWO_Baudrate` → `SWO_Control(Start)` → 起读取线程 |
| `Stop` / `Disconnect` | 只置标志 | 停线程 → `SWO_Control(Stop)` → 清状态 |
| `Flush` | 清 `ctx` 里一个空 vector（无效） | 清空**真实**环形缓冲 |
| `WaitForEvent` | 1 ms 轮询 `GetStatus`，每次轮询都发一条 HID 命令 | 改为条件变量阻塞等待：数据到达 / 超时 / 已停止三种事件，`eventType` 按 P4 约定 |
| `SubmitEventBuffer` | 空转返回成功 | 按 P4 实现，或明确返回"不支持" |
| `GetConfigItem` / `SetConfigItem` | item 0 / 1 / 2 的语义是**猜的** | 按 P4 修正；未知 item 返回 `RDDI_BADARG` |

**阶段 3 · SWO 数据通路（按 P7 结论二选一）**
- **A（首选，需固件支持）**：V2 走 `ID_DAP_SWO_DATA_START / STATUS / READ / STOP`（`0xB0`–`0xB3`；常量已在 `include/ORBMDK_DAPV2.h`，**实现为空**）→ 真正的流式。工作：在 `src/ORBMDK_USB_Bulk.cpp` 实现这 4 条命令，并在 HID 层加统一入口。
- **B（兜底，不需改固件）**：底层仍用 `ID_DAP_SWO_DATA` 轮询，由本层**后台线程**持续读入环形缓冲，对 AGDI 呈现"流式"。代价：CPU / 总线占用高、延迟大；优点是能先把 AGDI 的注册与生命周期跑通，也可作为 A 的对照。
- 共同的 HID 层改动（`src/ORBMDK_HID.cpp`）：
  1. `StreamingTrace_GetData` 现在自拼 `ID_DAP_SWO_DATA` 并解析 `resp[5]`，与 `DAP_SWO_Data` 重复且绕过状态解析 → 统一走 `DAP_SWO_Data`；
  2. 64 KB 静态 `vector` → **环形缓冲 + 条件变量**，支持生产者 / 消费者两个线程；
  3. `StreamingTrace_Start(mode != 1)` 直接失败的行为**保留**（未知模式明确失败，不假成功）；
  4. `DAP_SWO_Control(1, NULL)` 的 `status` 允许为 NULL —— 在注释里写明"这是允许的"，避免以后被当 bug 改掉。
- `src/ORBMDK_Trace.cpp` 的三个空函数：`SWO_SetBaud` 必须**真配置**（否则流式波特率不生效）；`GetCPUState` / `SetAltAddrEncode` 先看日志里 AGDI 是否调用，再决定。

**阶段 4 · 接口账语义补齐**

| 接口 | 现状 | 补齐 |
|------|------|------|
| `DAP_GetNumberOfDAPs` | 恒 1，不访问目标 | 与 `CMSIS_DAP_DetectNumberOfDAPs` / `DetectNumberOfDevices` **共用同一数据源**（`ctx->dapIdList`），三处口径一致；仍遵守"`DAP_Connect` 之前不得与目标通信"（用缓存值） |
| `DAP_GetDAPIDList` | 已兼容两种 `sizeOfArray` 解释 | 确认返回值语义（元素数 / 字节数），并写回真实 IDCODE |
| `CMSIS_DAP_DetectNumberOfDevices` / `GetDeviceIDList` | 已真实探测 | 补"探测失败"语义：明确返回 0（AGDI 用 0 表示"无设备"）还是错误码 |
| `CMSIS_DAP_ConfigureDebugger` | 只把 cfg 存进 `lastErrorStr` | 明确为 no-op + 成功并补日志；若 P5 显示 A 臂中断在其之后，再评估是否需要真实动作 |

**阶段 5 · 翻开关 + 整体回归**
- 回归矩阵：{V2 Bulk, V1 HID} × {SWD, JTAG} × {Trace 关, SWO Read, SWO Stream}，另加设备列表行数、Flash 下载、断点 / 单步、`Trace Enable` 勾选前后对比。
- 必须同时满足：§17.5 的 A / B（`2.x` 能进 Debug）、§18.8 的 `TraceTransport=Read` 不被破坏、§18.12 / §18.13 不受影响。
- 验收 = 成功判据三条 + 回归矩阵全绿。

**阶段 6 · 落档与清理**
- 删除验证期的 `ORBMDK_FWVER` 环境变量（§17.5 原则：不留能掩盖问题的旁路）。
- 同步更新：§18.6（正式取值）、§18.8（caps 位条件）、§18.5（勾掉已补的 stub）、§18.10-B（P1 / P3 / P4 结论与结项）、`COMPAT_ANALYSIS.md` §17.5–§17.7 的"钉值"叙述。

**执行顺序（重要）**：阶段 0 → 在**版本号仍为 `1.0.0`** 时把阶段 2 / 3 / 4 全部做完（对现网零影响）→ 最后阶段 1 与阶段 5 同批翻开关。原因：一旦上报 `2.x`，AGDI 必然走 sink 注册，"未实现"立刻 `0x2028`，拆开发布不存在可用的中间态。

**判死条件与回退**：P1 证明假说 1 为真，或翻开关后 A 臂仍与 §17.5 完全一致地失败且 P5 找不到可补缺口 → 回退 `1.0.0`，18.10 收敛到「替代方案（SWO Read）」，ETM 走独立上位机路线。

**改动面与风险**：4 个源文件（`ORBMDK_RDDI.cpp` / `ORBMDK_HID.cpp` / `ORBMDK_USB_Bulk.cpp` / `ORBMDK_Trace.cpp`）+ 1 个头文件开关，无新增依赖。**最大风险**：`GetSinkDetails` 的真实 ABI（P4 #2，猜错会崩溃）；其次是 V2 流式命令的固件支持（P7）。

---

## 🟡 可选

### [ ] 18.2-遗留 块传输测试 `DHCSR` 回读为 0（非阻塞）

- **位置**：`test/ORBMDK_BlockTransferTest.cpp`（`RunRound` 里的 DHCSR 检查段）。驱动侧 `src/ORBMDK_RDDI.cpp` 的 `DAP_ReadReg` / `DAP_WriteReg` 只转发单次 AP 访问、**不改写 TAR**，符合 ARM"AP bank/TAR 由调用方管理"的约定 —— **不需要改驱动**。
- **现状**：12/12 通过、哨兵零损坏，但打印 `DHCSR -> 0x00000000 [NOT halted]`；原记录据此推断"那次停核没有真正生效"。
- **根因**（两处，都在测试侧；任何一处都能单独造成回读 0）：
  1. **TAR 漂移（主因）**：`PrepareAP` 设的 `CSW = 0x23000052` 含 `AddrInc=0b01`（单次自增），而 MEM-AP 会在**每次 DRW 访问后**按访问宽度自增 TAR。测试顺序是"写 `TAR=0xE000EDF0` → 写 `DRW=0xA05F0003`（停核）→ 直接读 `DRW`"：第一次 DRW 写后 TAR 已被硬件加到 `0xE000EDF4`，第二次读 DRW 实际读到的是 **DCRSR**（读为 0）—— 与实测 `0x00000000` 完全吻合。
  2. **读失败被吞**：`DAP_ReadReg` 在**所有**失败路径下也会执行 `*value = 0`，而测试没有检查返回码 —— 现场无法区分"读失败"与"真读到 0"。
- **结论修正**：停核那次**写**落在正确地址（写不参与漂移），所以**停核大概率是生效的**，这也解释了随后 12/12 通过、哨兵零损坏。原"停核没生效"的判断缺乏依据，应改记为"回读地址错"。
- **方案**（只改测试程序）：
  1. 读 DHCSR 前**重新写一次** `TAR = 0xE000EDF0`（最小改动）；或该检查段单独用 `AddrInc=off` 的 CSW（`0x23000050`）；
  2. 检查 `ReadReg` 的返回码并打印失败状态，避免与"真读到 0"混淆；
  3. 判定条件用 `bit0 (C_DEBUGEN) | bit1 (C_HALT) | bit17 (S_HALT)` —— 正常停核回读应为 `0x00030003`。
- **验收**：回读出现 bit0/bit1 置位（或 bit17 `S_HALT`），且 `ReadReg` 返回 `RDDI_SUCCESS`。
- **影响面**：仅测试程序；Keil 链路不受影响（AGDI 的 `SWD_WriteData` / `SWD_ReadData` 每次访问前都会重写 TAR，天然不漂移）。

### [ ] 18.5 简化实现（stub 清单；均不影响 Keil 调试链路）

（二轮迁出自 `COMPAT_ANALYSIS.md` §5.1~§5.4，该节现为指针）

- **位置**：`src/ORBMDK_RDDI.cpp`、`src/ORBMDK_HID.cpp`、`src/ORBMDK_Trace.cpp`、ETM v4 适配层
- **待补清单**（只有真要跑 trace / sequencer 时才需要）：
  - `DAP_DefineSequence` / `DAP_RunSequence`（忽略全部参数，返回成功）
  - `StreamingTrace_SubmitEventBuffer`（忽略 buffer 返回成功）
  - `CMSIS_DAP_Commands`（仅处理 `ID_DAP_RESET_TARGET`，其余返回 `RDDI_BADARG`）
  - `CMSIS_DAP_ConfigureDAP`（仅识别 `SWJSwitch=` 前缀，无实质动作）
  - `CMSIS_DAP_JTAG_GetIRLengths`（硬编码 `*count=0`）
  - `CMSIS_DAP_Atomic_Result`（硬编码 `*result=0`）
  - `CMSIS_DAP_GetGUID`（序列号拼串，非真 GUID）
  - `DAP_Target` 的 Halt / Resume / Step（仅置 0，只有 Reset 有真实调用）
  - `ORBMDK_Trace.cpp` 的 3 个空函数：`GetCPUState` / `SetAltAddrEncode` / `SWO_SetBaud`
  - ETM v4 的 `FuncReturn` / `ExceptionReturn` / `Prefix`（`return 0` 空操作）
- **解码器实现深度**（原 §5.3）：ITM ✅ 状态机完整（同步/SW/HW/TS/GTS/XTN/NISYNC/RSVD）；TPIU ✅ 状态机自洽（显式 `packetReady`）；ETM v3.5 处理 A-SYNC/ISYNC/CYCCNT/CONTEXTID/分支（ISYNC 地址宽度可配 `addrBytes`，默认 4）；**ETM v4 仅短/长地址 + 原子 + 少量异常**（深度见 18.10）。
- **验收**：当前状态即"不影响 Keil 链路"；仅在启用 trace / sequencer 时要求补齐后对应功能可用。

### [ ] 18.6 `Firmware Version` 一栏的显示

- **位置**：`CMSIS_DAP_Identify(idNo=4)` 的上报串
- **现状**：固定上报 `1.0.0` —— 设备真实值 `2.1.0`（主版本 2）会触发 AGDI 的另一套逻辑（§17.5 单变量 A/B、§17.6 反汇编 `cmp eax,2`），故必须钉住。
- **方案**：只把主版本归一化为 1（`2.1.0` → `1.1.0`），语义上仍能过闸；纯显示偏好。
- **验收**：改完需一次 A/B 复验，Keil 仍能进入 Debug。
- **⚠️ 依赖变更**：本项「钉 1.0.0」的结论与 **18.10-C（路线 1：放开到 2.x）**直接冲突 —— 两者必须**同批**决策，不能只改一处。若 18.10-C 落地，本节应改写为"正式取值 2.0.0 + 说明为什么不再归一化"；若 18.10-C 判死，本节按原方案钉 1。

### [ ] 18.11 测试程序 `DAP_REG_*` 常量与 `rddi_dap.h` 编号语义不符

（迁出自 `COMPAT_ANALYSIS.md` §13.5）

- **位置**：`test/ORBMDK_RDDI_FullTest.cpp`
- **现状**：该文件的 `DAP_REG_*` 常量仍是"字节偏移"风格（例如 `DAP_REG_AP_DRW` 传成 `0x0001000C`），与 `rddi_dap.h` 的**编号**语义（DP 0–3 / AP 4–7）不符 → Test 9/10 报 `RDDI_DAP_BAD_REGISTER_ID`；当前 `PASSED 22 / FAILED 20`，**不再是 40/40**。
- **剩余失败分两类**：① 测试自身的常量 / 预期陈旧（即本项）；② 目标侧状态 —— `DP CTRL/STAT = 0x00000000 [NOT powered]`（目标复位后调试电源未上，AP 访问必然 FAULT；三种传输模式表现**完全一致**，可排除传输层）。
- **方案**：① 把 `DAP_REG_*` 改为编号语义；② 对 ② 类用例在运行前补 DP/AP 上电（参考 §9.2 的四步）。
- **验收**：恢复 `PASSED 40 / FAILED 0`；或在"目标未上电"时明确 skip 而非 FAIL。
- **注**：与 Keil 链路无关（AGDI 不走该测试的调用路径）。

### [ ] 18.12 `CMSIS_DAP_GetNumberOfDevices` 在 JTAG 路径上的语义待复核

（迁出自 `COMPAT_ANALYSIS.md` §4.12 注）

- **位置**：`src/ORBMDK_RDDI.cpp`
- **现状**：该接口（RDDI 层另一个计数接口，AGDI **仅在 JTAG 分支**调用，见 `0x1002C9BC`）仍返回 `kSingleDapCount`；它是 §4.6 #16 为修复 "No Debug Unit Found" 而定，且不在 SWD 路径上。
- **待复核**：JTAG 已于 §18.9 第九步打通 → 该返回值在多 TAP / 多目标场景下是否仍适用（当前 SWD 路径不受影响）。
- **验收**：JTAG 模式下设备数语义与 AGDI 预期一致（或明确记录"只需 1"并结案）。

### [ ] 18.13 是否对外宣称 `INFO_CAPS_JTAG`

（迁出自 `COMPAT_ANALYSIS.md` §18.9 第一步 #6）

- **位置**：`CMSIS_DAP_Capabilities` 的上报位（`src/ORBMDK_RDDI.cpp`）
- **现状**：JTAG 已于第九步实测打通，但该能力位**暂不加** —— 原则是"能力位只宣称已验证能力"。
- **待评估**：是否对外宣称 `INFO_CAPS_JTAG`。注：Keil 的 Port 下拉框**不受该位限制**，不宣称也能选 JTAG。
- **验收**：给出结论（加 / 不加）并写入本档；若加，需一次 SWD / JTAG 双路径回归。

### [ ] 18.14 设备拔出后适配器下拉框仍显示（`IsConnected` 无拔出探活）

（迁出自 `COMPAT_ANALYSIS.md` §4.12 另注）

- **位置**：`ORBMDK_HID_IsConnected()`（`src/ORBMDK_HID.cpp`）
- **现状**：设备拔出后适配器下拉框仍显示 `CMSIS-DAP v1` / 序列号 —— 属 AGDI 缓存的 `MonConf` 与 HID 层连接状态未失效；**不影响** SW Device 列表行为（§4.12 已修）。
- **方案**：让 `ORBMDK_HID_IsConnected()` 具备拔出探活能力（轻量 I/O 探测或句柄失效检测）。
- **验收**：拔出设备后下拉框条目随之失效 / 消失。

---

## ⛔ 不做（已决）

### [-] 18.7a ULINK+ 专有接口（`ULINKPLUS_*`）

Keil 独有，ORBTrace 用不到 —— 不实现。

### [-] 18.7b 符号解析的候选搜索路径（`src/ORBMDK_Symbols.cpp`）

（迁出自 `COMPAT_ANALYSIS.md` §7.3，🟡 低）

- **硬编码候选**：`src/ORBMDK_Symbols.cpp:927-933` 的 Keil `TOOLS.INI` 五个路径（含 `D:\Keil_v5\`、`D:\MDK5\`）；`973-979` 的 GNU Arm Toolchain 7 条候选（含 `12.3.rel1` 等版本号）；`include/ORBMDK_Symbols.h:243` 注释中的 `C:\Keil_v5\ARM\ARMCC\Bin`（仅文档）。
- **结论**：`ORBMDK_FindObjdumpPath` 已按「环境变量 → Keil `TOOLS.INI` → 常见安装目录 → `PATH`」顺序探测，前序步骤（`OBJDUMP` / `ARM_TOOLCHAIN_PATH` / `ARMGCC_DIR`）与 `PATH` 兜底可覆盖多数场景 → **保持现状**（设计内 fallback，`D:\Keil_v5\` 已于 2026-09-30 补上）。
- **若将来要精简**：`D:\MDK5\TOOLS.INI` 与带具体版本号的 GNU 候选较脆，可收敛为「系统盘默认项 + 环境变量」。

---

## ✅ 已完成（留档）

- [x] 18.2 块传输提速验证 —— 2026-09-30 完成（12/12 通过、哨兵零损坏、加速比 7.86×）
- [x] 18.3 日志模块统一（§8）—— 2026-09-30 完成（单实现 + 会话分隔标记）
- [x] 18.4 状态 LED 实机确认（§11.2）—— 2026-10-01 完成
- [x] 18.9 JTAG 通路打通（§18.9 第九步）—— 2026-09-30 完成（扫链 + DP/AP 访问均通）
