# ORBMDK 不可修复缺陷档

- **收录标准**：只收**确认在本层（ORBMDK 适配层）无法修复**的缺陷 —— 根因在**探针固件 RTL** 或**第三方二进制（µVision / `CMSIS_AGDI.dll`）**，本层既改不了源、又无法用代码绕过。
- **不进本档**：本层能修的（→ `Todo.md` 待办 / 已完成）、AGDI 逆向与 ABI 结论（→ `COMPAT_ANALYSIS.md`）、有确定绕法且已落地的（→ 只在此处留一行指针）。
- **记录要求**：每条必须给出**机制**（不是"偶发"）、**为什么改不了**、**本层残余动作**（哪怕只是"熔断 + 明确报错"）。
- **固件源码位置**：`C:\Users\taibaibu\Desktop\orbtrace-1.4.3\`；下文路径相对该根目录。真正跑在片上的 DAP 是 `orbtrace/debug/cmsis_dap.py`（Amaranth RTL）+ `verilog/{dbgIF,jtagIF,swdIF}.v`（底层 JTAG/SWD 位移引擎）；`daplink/**` 只是随包附带的 DAPLink C 参考实现，**不是片上的东西**。
- **最后更新**：2026-10-01

## 概览

| 编号 | 类别 | 主题 | 根因位置 | 可否缓解 |
|------|------|------|----------|----------|
| **B1** | 固件 RTL | DAP 命令通道无超时兜底 → 一次握手失败即**永久变哑**，只能断电重插 | `cmsis_dap.py`（`busy` / 状态机） | 本层**已做熔断**：立即失败 + 明确报错 + 可自愈；缺陷本身修不了 |
| **B2** | 固件 RTL | 块传输请求字数在固件侧**无夹取**（≥127 字越界） | `cmsis_dap.py:128-132 / 1169` | 本层自限 125 字（已规避） |
| **B3** | 固件 RTL | JTAG 链容量硬上限：链 ≤ 6 器件、单器件 IR ≤ 31 bit（5 bit 截断） | `verilog/jtagIF.v:28-30`、`cmsis_dap.py:709-718` | 无（硬件约束） |
| **B4** | 第三方二进制 | Keil 内 ETM 不可达（µVision 按驱动类别筛 Trace 页） | `UV4.exe` + `CMSIS_AGDI.dll` | 无（改 DLL 不可维护） |
| **B5** | 第三方二进制 | AGDI 不传设备信息（`rddi_Open(pDetails)` 恒 NULL） | `CMSIS_AGDI.dll` | 有绕法，见 `Todo.md` §18.1 |
| **B6** | 协议 / 物理 | V1 HID 单包 64 字节 → 块传输 ≤ 14 字；固件 `DAP_MAX_PACKET_COUNT = 1` | USB HID 报告描述符 + `cmsis_dap.py:27-28` | 换 V2 Bulk（唯一出路） |

---

## 甲、固件侧（改不了，除非改 RTL 重出 bitstream 或换固件）

### B1 ⛔ DAP 命令通道无超时兜底 —— 一次握手失败即永久变哑

**现象（现场判据，三件同时成立）**

1. USB 层**完全正常**：设备照常枚举、能打开、能读描述符 / 序列号；
2. **所有** DAP 命令超时（1 s 级），包括最简单的一条 `DAP_Info`：V2 下 `ORBMDK_HID_DAPCommand` 返回 `-2`，V1 下返回 `-5`（读超时）/ `-3`（写超时）；
3. 重开 Keil 会话、换 V1 / V2、换 SWD / JTAG **全部无效**；**只有给探针重新上电（拔插）才好**。

**机制（三步，全部有码可查）**

1. **命令入口由 `busy` 直接闸死**：

   ```1712:1712:C:\Users\taibaibu\Desktop\orbtrace-1.4.3\orbtrace\debug\cmsis_dap.py
           m.d.comb += self.streamOut.ready.eq(~self.busy)
   ```

   `busy = 1` ⇒ `streamOut.ready = 0` ⇒ **设备从此一个命令字节都不再接收**。USB 收发是独立层次，所以枚举照旧 —— 这正是"设备在、命令全废"的来源。

2. **块传输状态机在等底层 `done` 脉冲，且没有任何超时**：

   ```1103:1148:C:\Users\taibaibu\Desktop\orbtrace-1.4.3\orbtrace\debug\cmsis_dap.py
               with m.Case(4):
                   m.d.sync += [
                       self.dbgif.go.eq(1),
                       self.Bretries.eq(self.Bretries-1),
                       self.tfB_txb.eq(5)
                   ]
               with m.Case(5):
                   with m.If(~self.dbg_done):
                       m.d.sync += [
                           self.dbgif.go.eq(0),
                           self.tfB_txb.eq(6)
                           ]
               with m.Case(6):
                   with m.If(self.dbg_done==1):
                       ...
   ```

   状态 5 只等 `go` 被下层接收，状态 6 **只等 `dbg_done==1`**。`done` 由底层引擎产生（`dbgIF.py:122-127` 把 `i_command/i_go/i_dev` 交给 `verilog/jtagIF.v`），只有把整条链的 DR / IR 移完才置位。**若下层永远不出 `done`，状态机就永久停在 6**。

3. **`busy` 只在这些状态机的"正常路径"里被清**（`cmsis_dap.py:653 / 728 / 837 / 909 / 1100 / 1307 / 1322 / 1486 …`），没有任何一条"异常 / 超时"分支会清它。RTL 里**没有看门狗**。

**触发条件**（任一即可，具体哪一次触发**未定论** —— 现场日志首行已是超时，触发点在前一次会话）：目标的 TCK 被拉住 / SRST 被外部拉低 / JTAG 链与 `ndevs`、`dev` 不符导致走链异常 / 目标掉电 / 调试口被目标固件复用作普通 IO。

**为什么本层改不了**

- `busy` 与状态机固化在 bitstream 里，本层只有 USB 两条通道可用；
- CMSIS-DAP 命令集里**没有任何"复位调试器自身"的命令**（`DAP_ResetTarget = 0x0a` 复位的是**目标**，不是探针），本层没有别的通道能把它救回来；
- 实测唯一恢复手段就是给探针重新上电。

**本层已做的缓解：DAP 命令通道熔断**（2026-10-01 实现，`src/ORBMDK_HID.cpp`）

- **判据**：连续 `kDapTripAfter = 2` 次**超时**即熔断；任何一次成功即清零，协议层失败既不计数也不清零 —— 否则会在"块传输不支持 → 回退逐字传输"这类正常回退路径上误熔断。哪次算超时随传输层而定：V2 是 `-2`，V1 是 `-3`（写）/ `-5`（读）；V1 里 `-2` 是 cmdLen 越界（编程错误），**不**作为熔断依据。
- **熔断后**：`ORBMDK_HID_DAPCommand` 在**碰 USB 之前**就返回超时码（`ORBMDK_DapFastFailCode()`，与真超时同值，上层无需新增分支），并打一条**默认级别可见**的 `ERROR`，直接写明"唯一有效恢复：给探针断电重插"。被拦下的命令限流 10 s 报一条，避免日志被冲垮。
- **自愈**（三条路，保证"重插"永远不会被熔断挡住）：① 任意一条命令成功；② 设备被重新打开（HID `ORBMDK_HID_OpenDevice` / WinUSB `_initWinUSB` 的成功点）；③ **半开窗口**到期放行一条探测命令，成功即解除 —— 窗口自 3 s 起、探测继续失败就翻倍、封顶 30 s。所以探针重新上电后最迟一个窗口内自动恢复，**不必重开 Keil**。
- **状态放命名共享内存** `Local\ORBMDK_DapHealth`（与 `_nextOpenSeq` 同法）：AGDI 一次会话内会反复卸载 / 重载本 DLL，用普通 `static` 会被清零 → 熔断刚建立就失效。
- **闸门挂两层**：统一分发点 `ORBMDK_HID_DAPCommand`（负责记账）与 V2 直连入口 `ORBMDK_USB_Bulk_DAPCommand`（只拦不记账 —— 覆盖 `CMSIS_DAP_PC_Capture` 这类绕过分发点的调用，同时避免同一次失败被记两次）。
- **熔断 ≠ 修复**：探针该断电重插还是要断电重插；熔断只是把"每条命令各等 1 s"换成"立即失败 + 一句话说清要做什么"。
- **现场验证**：`%TEMP%\ORBMDK_RDDI.log` 里出现 `================ DAP 通道熔断 ================` 即已生效（ERROR 级，默认阈值就能看到，不必调级别）。
- ⚠ 熔断**不得**用于放宽 B2 的字数上限来"补偿"。

**对比**：官方 DAPLink（C 版）有 `retry_count` + `DAP_TransferAbort` 兜底；orbtrace 的 RTL 没有对应机制。

---

### B2 ⛔ 块传输请求字数在固件侧无夹取（≥127 字越界）

**机制**：响应缓冲 `tfrram` 是 127 字的 RAM，地址 7 bit：

```126:132:C:\Users\taibaibu\Desktop\orbtrace-1.4.3\orbtrace\debug\cmsis_dap.py
class WideRam(Elaboratable):
    def __init__(self):
        self.adr   = Signal(range((MAX_MSG_LEN//4)))
```

块传输读方向每读一个字 `tfrram.adr` 自增（`cmsis_dap.py:1141`），结束时响应长度按地址算：

```1169:1169:C:\Users\taibaibu\Desktop\orbtrace-1.4.3\orbtrace\debug\cmsis_dap.py
                    self.txedLen.eq((self.tfrram.adr*4)+4)  # Record length of data that will be returned
```

`RESP_TransferBlock_Setup`（`cmsis_dap.py:1034-1081`）里**只** `transferBCount = Count-1`，**没有任何夹取**。

| 请求字数 | `tfrram.adr` 终值 | 响应字节 `adr*4+4` | 与 `MAX_MSG_LEN = 508` 比 |
|----------|-------------------|--------------------|---------------------------|
| 125 | 125 | 504 | 安全（**本层 V2 上限**） |
| 126 | 126 | 508 | 顶格 |
| **127** | 127 | **512** | **越界 4 字节** → IN 流按超缓冲长度发送 |
| ≥128 | 7 bit 回卷 | 错乱 | 已覆盖 RAM + 长度错乱 |

**为什么改不了**：同 B1，在固件 RTL 里。

**本层已做（规避，不是修复）**：`src/ORBMDK_RDDI.cpp:155-169` 的 `BlockWordsLimit() = (pkt-5)/4`，`pkt` 取自开机的出包自标定（V2 下读 `DAP_Info(0xFF) = 508`）；V2 得 **125 字**，V1 HID 走 `kMaxBlockWords = 14`。

**必须守住的线**：`BlockWordsLimit()` **永远不得超过 126**。一旦哪轮标定 / 改写把 `pkt` 抬到 512 以上、(pkt-5)/4 ≥ 127，就会踩进固件越界分支 —— 而 B1 会让这一踩变成"永久变哑"。

---

### B3 ⛔ JTAG 链容量硬上限（≤ 6 器件 / IR ≤ 31 bit）

**机制**：底层引擎的链参数宽度是硬编码的 5 bit / 3 bit：

```27:31:C:\Users\taibaibu\Desktop\orbtrace-1.4.3\verilog\jtagIF.v
	// Upwards interface to command controller ========================================================
        input [2:0] 	          dev,                                  // Device in chain we're talking to
        input [2:0] 	          ndevs,                               // Number of devices in JTAG chain-1
        input [29:0] 	          irlenx,                                  // Length of each IR-1, 6x5 bits
        input [3:0]               ir,                                                  // ARM JTAG IR value
```

上层同样是 5 bit 一格地攒 IR 长度：

```713:719:C:\Users\taibaibu\Desktop\orbtrace-1.4.3\orbtrace\debug\cmsis_dap.py
    def RESP_JTAG_Configure_Process(self, m):
        # Collect octets representing the irlength for each member of the chain
        with m.If(self.streamOut.valid & self.streamOut.ready):
            m.d.sync += [
                self.dbgif.dwrite.bit_select( self.jtag_ircount,5 ).eq(self.streamOut.payload.bit_select(0,5)),
                self.jtag_ircount.eq(self.jtag_ircount+5)
            ]
```

**后果**：链上**超过 6 个器件**装不下；单个器件的 **IR 长度 > 31 bit 会被静默截断**（不报错，只是走链错 → 又回到 B1 的触发条件）。`dbgif.dev` 本身也只有 3 bit（`dbgIF.py:19`，`cmsis_dap.py:709` 把那 4 bit 的 Count 塞进去）。

**为什么改不了**：位宽是 RTL 里的常量，等于硬件接口契约；改它要重出 bitstream。

**本层残余动作**：无法补救 —— 只能保证"链超过 6 器件 / IR 超 31 bit"时**不硬发**（目前没有这条检查；属可选加固，非必须）。

---

## 乙、第三方二进制侧（不可改）

### B4 ⛔ Keil 内 ETM 不可达

- **结论**：卡点不在本层、不在固件版本号、也不在本层上报的 `caps` —— **µVision 按"调试器驱动类别"限定了 Trace 页可用项**。判据取自 `CMSIS_AGDI.dll` 导出 `DllUv3Cap(2)` 的**常量返回值 `7`**，全程不经过 RDDI 层。
- **"并行 Trace 端口（1/2/4-bit）"与"ETB"在 UI 层就直接置灰 / 不列出**（2026-10-01 实机复核确认），用户根本走不到"选了之后被拒"这一步 → 改 `caps` 任何位都不可能点亮（2026-10-01 已证伪）。
- **为什么改不了**：唯一的硬办法是改 `CMSIS_AGDI.dll` 里 `DllUv3Cap(2)` 的常量，让 µVision 误认成另一类调试器 —— 属修改第三方二进制，**不可维护、随驱动升级即失效**；而且**即便点亮也仍然用不了**：AGDI 内建的配置串模板只有 5 个（`SWO-UART / SWO-Manchester × {Read, Stream}` + `Trace=Off`），里面**没有并行 Trace / ETB / ETM 的表达形式**，点选之后无配置可下发。故这条"绕法"不成立，不必尝试。
- **本层待办**：无。**出路**：脱离 Keil，用独立上位机 + 探针硬件并行 Trace（需目标自带 ETM + Trace 引脚已物理接到探针）。
- **完整证据**：`Todo.md` §18.10（证据 1/2/3 + 结论 5 + 证据地址）。

### B5 🟡 AGDI 不传设备信息（`rddi_Open(pDetails)` 恒 NULL）

- **现象**：两台同型号调试器同时插入时无法指定用哪一台，只能取枚举顺序第一台。
- **上游根因（不可改）**：AGDI 的 `rddi_Open(pDetails)` **恒为 NULL**，RDDI 层没有任何"用户想用哪台"的信息可传；这条属 AGDI 行为，本层无法要求它多传一个参数。
- **本层有绕法**（故非死结）：靠"接口编号只编码传输方式"的既有约定，把 `(设备序号, 传输方式)` 编进 `ifNo`，配合底层已具备的按序列号打开能力（`ORBMDK_HID_OpenDevice(serial)` / Bulk 的序列号筛选）实现区分。方案与验收见 `Todo.md` §18.1。
- **记录理由**：根因在第三方二进制，故在此留档 —— 若哪天要重新评估"能不能选设备"，别再去 AGDI 里找参数，它确实不传。

---

## 丙、协议 / 物理硬限制（不是缺陷，但会被反复误判为 bug）

### B6 ⚠️ 单包容量

| 项 | 值 | 来源 | 后果 |
|----|----|------|------|
| V1 HID 报告负载 | 64 字节 | USB HID 报告描述符 + 固件 `DAP_V1_MAX_PACKET_SIZE = 64`（`cmsis_dap.py:28`） | `DAP_TransferBlock` 头 5 字节 → **(64-5)/4 = 14 字** 上限；想再快**只能走 V2** |
| V2 Bulk 最大包 | 508 字节 | 固件 `DAP_V2_MAX_PACKET_SIZE = 508`（`:29`）+ `DAP_Info(0xFF)` 实回 508 | 上限 125 字（见 B2） |
| 固件 `DAP_MAX_PACKET_COUNT` | **1**（`cmsis_dap.py:27`） | 同上 | 一次只能派发一条命令，**无法靠多命令打包（pipelining）提升吞吐** |
| 固件 `DAP_CAPABILITIES` | `0x1f`（SWD｜JTAG｜SWO UART｜SWO Manchester｜Atomic Commands，`cmsis_dap.py:25`） | 同上 | 固件**不宣称** SWO 流式（`0x40`），本层若置该位属"替固件宣称"（见 `Todo.md` §18.10-C 的两道门控约定） |

---

## 附录 A · 硬约束（改了就复发，勿"顺手"动）

| # | 约束 | 位置 | 动了会怎样 |
|---|------|------|-----------|
| A1 | **禁止回写 `*baudrate`** | `CMSIS_DAP_SWO_Baudrate`（`src/ORBMDK_RDDI.cpp`） | AGDI 包装 `0x1003D030` 会 `cmp [edi], ebx`，值被改写即返回 `0x2022` → SWO 时钟整轮探测失败（`SWO CLOCK not support`）。详见 `Todo.md` §18.15 |
| A2 | V2 通道**不得**在 `_bulkWrite` / `_bulkRead` 底层加锁 | `src/ORBMDK_USB_Bulk.cpp` | `_flushAndDrainPipes` 在持锁段内嵌套调用 `_bulkRead` → **自死锁** |
| A3 | 问设备的 `DAP_Info` 必须放在 `g_hidMutex` **之外** | `ORBMDK_HID_GetDeviceInfo`（`src/ORBMDK_HID.cpp`） | V1 路径的 `DAP_Info` 内部自己加同一把**非递归** mutex → **自死锁** |
| A4 | 版本号主版本与 `caps` 的 `0x40` **必须同侧** | `include/ORBMDK.h` + `CMSIS_DAP_Capabilities`（`src/ORBMDK_RDDI.cpp`） | 只开一半 = 向 AGDI 宣称流式能力却永远等不到 sink 注册 → `0x2028` 中止初始化。回退步骤写在 `include/ORBMDK.h` |
| A5 | `BlockWordsLimit()` **不得超过 126** | `src/ORBMDK_RDDI.cpp:155-169` | 落进 B2 的固件越界分支，而 B1 会把这一踩升级成"永久变哑" |

---

## 附录 B · 现场速查：这是不是 B1？

- 设备在设备管理器里**在**、能打开、序列号能读 → 传输层没问题；
- 打一条最简 `DAP_Info(0x00)` 就**超时**：V2 返回 `-2`，V1 返回 `-5`（写超时是 `-3`）；
- 换 V1 / V2、换 SWD / JTAG、重开 Keil **都没用**；
- **拔插探针即恢复**。

四条全中 = B1，**不要再去查本层代码**（本层改不了），记录触发场景后给探针断电重插即可。
