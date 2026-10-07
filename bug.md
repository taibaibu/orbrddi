# ORBMDK 不可修复缺陷档

- **收录标准**：① **本层改不了**的缺陷（根因在**探针固件 RTL** 或**第三方二进制 µVision / `CMSIS_AGDI.dll`**）；
  ② 本层**已做**改动的实现细节与实机数据留档（**附录 B**，自 `Todo.md` 迁入，避免两份数据漂移）；
  ③ **口径决策的记录**（如 B12「虚构 SWO 能力位」的否证全过程）—— 凡"做过了、有结论"的内容以本档为唯一落点。
- **不进本档**：本层**能修且未修**的（→ `Todo.md`）；AGDI 逆向与 ABI 结论（→ `COMPAT_ANALYSIS.md`）。
- **固件源码**：`C:\Users\taibaibu\Desktop\orbtrace-1.4.3\`；片上是 `orbtrace/debug/cmsis_dap.py`（Amaranth RTL）+ `verilog/{dbgIF,jtagIF,swdIF}.v`。
- **最后更新**：2026-10-03（0.5.0 发版整理）

## B5 🟡 AGDI 不传设备信息（`rddi_Open(pDetails)` 恒 NULL）

- **现象**：两台同型号调试器同时插入时无法指定用哪一台，只能取枚举顺序第一台。
- **根因（不可改）**：`rddi_Open(pDetails)` 恒 NULL，RDDI 层拿不到"想用哪台"；属 AGDI 行为。
- **绕法（未实施）**：把 `(设备序号, 传输方式)` 编进 `ifNo`，配合底层按序列号打开（`ORBMDK_HID_OpenDevice(serial)` / Bulk 序列号筛选）。方案与验收见 `Todo.md.bak` §18.1；当前 `ifNo` 仍只编码传输方式（`kTransportInterfaceCount = 2`）。
- **留档理由**：以后要"选设备"别再去 AGDI 找参数 —— 它确实不传。

## B6 ⚠️ 单包容量 / 包数（协议硬限制，勿误判为 bug）

| 项 | 值 | 来源 | 后果 |
|----|----|------|------|
| V1 HID 报告负载 | 64 字节 | `DAP_V1_MAX_PACKET_SIZE = 64`（`cmsis_dap.py:28`） | 块传输 ≤ **14 字**；想快只能走 V2 |
| V2 Bulk 最大包 | 508 字节 | `DAP_V2_MAX_PACKET_SIZE = 508`（`:29`） | 块传输上限 **126 字** |
| `DAP_MAX_PACKET_COUNT` | **1**（`:27`） | 同上 | 无法靠多命令打包（pipelining）提速 |
| `DAP_CAPABILITIES` | `0x1f`（不含 SWO 流式 `0x40`，`:25`） | 同上 | 本层不得替固件宣称流式 |

## B7 ⚠️ MEM-AP TAR 自增块边界（规范/实现硬限制；本链实测 4 KB）

- **现象（实测，可复现）**：`DAP_RegWriteRepeat` / `DAP_RegReadRepeat` 在**一次调用**里连续传输超过一个
  TAR 块时，数据按块周期重复落址，**且全部返回成功** ⇒ 静默错址。

  | 缓冲 | 结果 |
  |------|------|
  | 4 KB / 1024 字 | ✅ 通过（逐字校验） |
  | 8 KB / 2048 字 | ❌ `read=0xA0000400` = `pattern[1024]` |
  | 32 KB / 8192 字 | ❌ `FAIL: data mismatch at word 0  wrote=0xA0000000 read=0xA0001C00`（= `pattern[7168]`） |

  两种尺寸都等于"**最后一个 ≡0 (mod 1024) 的下标**" ⇒ **块 = 1024 字 = 4096 B**。
  首个冲突在 **word 0** 而非 word 1024（若折返在**读**侧应出现在 1024）⇒ **写侧就已跨块**。

- **根因（不可改）**：TAR 自增只在**一个块**内有效；规范只要求**至少 10 bit（1 KB）**，块大小由实现决定。
  公开依据 = OpenOCD `src/target/arm_adi_v5.c`：
  - L85 `/* ARM ADI Specification requires at least 10 bits used for TAR autoincrement */`
  - L91-94 `max_tar_block_size(block, addr) = block - ((block - 1) & addr)`
  - L191-203 `mem_ap_update_tar_cache()`：跨块 → `tar_valid = false` → 下一笔**重写 TAR**
  - L1256 `ap->tar_autoincr_block = (1 << 10);`（默认 1 KB 块）
  - 同文件：`/* TAR is incremented after failed transfer on some devices (eg Cortex-M4) */`

  本链实测块 = **4096 B（12 bit）**，符合"至少 10 bit"。`CSW = 0x23000052`（Size=32-bit、AddrInc=single、
  DeviceEn=1）由"1024 字全部落在连续 4 字节地址且逐字通过"反证有效 ⇒ **不是 CSW 配错、不是时钟/信号问题**。

- **本层为何兜不住**：`DAP_RegWriteRepeat(handle, dapId, numRepeats, regId, dataArray)` **无地址参数**
  （`regId` 仅 AP 寄存器号）⇒ 本层无从自行重装 TAR。现实现只按 126 字切块连发
  （`ORBMDK_RDDI.cpp:2228` / `:2315`）；`lastApTar`（`:360`）只喂 DHCSR 的 LED，**不随数据前进**。
  ⇒ **Keil 路径未爆只因余量为零**：AGDI 一笔就是 4096 B（`README.md:329`：`WRITE speed: 4096 B … 9 round trips`，
  9 × 126 B ⇒ 一次 1024 字调用）。影响面 = `SWD_WriteBlock(1247)` / `SWD_VerifyBlock(1357)`。

- **应对 = 方案 3（已实施，落在测试路径）**：**按实测值处理** —— 到达 `max_tar_block_size`（本链 4 KB）边界即
  **只写 TAR**；**不等待、不回读校验、不判错（默认成功）**，**直接继续下一段 4 K**；不拆调用方缓冲、不做条件回退。
  - 落地：`test\ORBMDK_RAM_SpeedTest.cpp` —— `kTarBlockBytes = 4096` + `TarBlockRemain()`（= `max_tar_block_size`）
    + `ArmTar()`（只写 TAR，不重写 SELECT/CSW）；`Measure()` 按块剩余量切块，**仅在块边界**
    （`(addr & 0xFFF) == 0` 且非调用方已 arm 的首块）重写 TAR。
  - **主代码（`src/`）不改**：`DAP_RegWriteRepeat` / `DAP_RegReadRepeat` **拿不到地址**（只有 AP 寄存器号），
    要在本层修必须引入"下一笔 AP-DRW 地址"跟踪并让所有 AP-DRW 路径同步前进 —— 列为可选后续，不在本次范围。
  - ⚠ **只在跨块时重写**：块内仍靠自增，否则会破坏"跨调用靠自增接续"的既有用法（同文件的 serial 基线、AGDI 的续写）。

- **复现 / 回归**：
  ```
  bin\ORBMDK_RAM_SpeedTest.exe 0x20000000 32768 1 1000000 ORBMDK_RDDI.dll 0   # 第6参=0 关分块 → 期望 FAIL（= 本缺陷）
  bin\ORBMDK_RAM_SpeedTest.exe 0x20000000 32768 3 1000000                    # 默认按块边界重写 TAR → 期望 0 失败
  bin\ORBMDK_BlockTransferTest.exe                                            # 期望 6 通过 / 0 失败（n≤64，不触块边界）
  ```
- **归口**：根因不可改（⇒ 入本档）；应对落在**测试工具**（`test\`，方案 3，已实施）；主代码保持不改（原因见上）。
- **留档理由**：以后看到"**>4 KB 连续 Repeat 数据错位、命令却全 OK**"，不必再查 CSW / 时钟 / 信号完整性 —— 先看这里。


## B8 🔴 JTAG 位流批量写（`JtagApWriteBurst`）—— 门级重发 WAIT，故"一条位流连发 N 笔"语义上不可能正确（已删除）

- **现象**：用**一条** `ID_DAP_JTAG_SEQUENCE` 连发 N 笔 AP 写时，ACK 呈**严格交替** `段ACK=[2,1,2,1,2,1]`，
  日志 `第 0/N 笔 ack=2(WAIT) -> 回退逐笔`；**Keil 里读/写内存出错、下载失败**。
- **根因（不可改，在探针固件 RTL）**：`orbtrace-1.4.3/orbtrace/debug/cmsis_dap.py:712-716`
  ```
  with m.If(self.dbgif.ack==ACK_WAIT):
      m.d.sync += [ self.retries.eq(self.retries-1),
                    self.tfr_txb.eq(Mux(self.retries!=0,6,10)) ]
  ```
  收到 `ACK_WAIT`(=2) 时门级**退回状态 6，把同一笔 TRANSACT 原样重发**；`waitRetry` 默认 **4096**（`:219`）。
  ⇒ WAIT 的真语义 = "**这一笔没完成，必须重发同一笔**"。
- **`段ACK` 为何严格交替**：逐笔基线 `RegAccessBlock: numRegs=29 -> [batch=0 single=20 wait=9]`
  —— **31% 的 AP 写本来就要重试** ⇒ `2,1,2,1` 是**该 AP 的真实节律**，**不是上位机采样点错位**
  （`cap[段1]=1100000000 c=100` 按同一公式解得 `ack=2`，且全段只出现 `1`/`2`，从不出 `0`/`7`）。
  ~~方案 A「采样点错位」已作废。~~
- **本层为何兜不住**：位流**一次发完** ⇒ 返回 WAIT 的那笔**未被目标完成**，本层只拿到"**事后** ACK"；
  与调用方"失败即整体重写"叠加 = **丢字 + 写两遍 + AP TAR 多自增** ⇒ **静默错址**。
  一条 `DAP_JTAG_SEQUENCE` **无法承载"重发同一笔"**。
- **处置（已实施，2026-10-03）**：`JtagApWriteBurst` **删除**（实现体 `#if 0` 封存留证，
  `jtagApBurstEnabled` 开关与调用点一并移除）；JTAG 写全部走逐笔，恢复历史行为。
  **实机验收（用户确认）**：Keil 读写内存、下载全部恢复正常 ⇒ 根因坐实。
  （零性能代价：该 burst 此前每次都第 0 笔即回退逐笔，速度本就等同逐笔。）
  停用前为该函数加的现场 WARN（`src/ORBMDK_RDDI.cpp:1416`）随函数一并删除。
- **唯一合法批量通路（未实施）**：把 N 笔塞进**一条** `ID_DAP_TRANSFER`，让**固件门级**按 `waitRetry`
  重发 WAIT ⇒ 既正确又比逐笔快。**方案要点、延迟读规则与风险**见
  **`Todo.md` §4「JTAG 批量写通路（方案 B）」**（本档不复述）。
- **归口**：根因不可改（⇒ 入本档）；`JtagApWriteBurst` 已删；JTAG 批量通路属**本层可做但高风险**（→ `Todo.md`）。
- **留档理由**：以后看到"**JTAG 一笔连发多笔写 / ACK 交替 WAIT / 数据错址**"，不必再查采样点、时序、
  CSW —— 先看这里；**不要再做"一条 JTAG_SEQUENCE 连发多笔"**。

## B9 🟡 "JTAG 下 0x06 是禁区"—— 实测**现象**成立，但当时的**定性不可靠**（已更正）

> **最终归口（2026-10-03 第十一轮）：见 `B11.8 ACK 分叉定论`** —— 现象（JTAG 下 `0x05`/`0x06` 不可用）成立
> 且理由更硬；但"首笔被吞 / 第二笔起必挂"这条**判据作废**，改为"**这一笔的 `ack` 落在分叉哪一边**"。

- **原始记录**：`COMPAT_ANALYSIS.md` §18.9 第十步（2026-10-01，orbtrace V2+Bulk，会话 #84）：
  JTAG 下发 `ID_DAP_TRANSFER_BLOCK`(0x06) 后**5 s 读超时**，随后 `Connect/Disconnect/Info`
  全部 `bulkWrite TIMEOUT (pipe reset)`，重开句柄无效，**只能插拔 USB**。
  ⇒ 该**现象**本档认可（日志齐全），据此把 JTAG 侧块传输改成"永久禁用、连探测都不发"。
- **不可靠之处（2026-10-03 复核更正）**：当时把它解释成"固件在 JTAG 模式下**没有这条实现路径**"，
  **与源码不符**。orbtrace-1.4.3 的门级固件**明确实现**了 JTAG 下的块传输：

  | 证据 | 位置 |
  |---|---|
  | `RESP_TransferBlock_Setup/Process` 带 `isJTAG` 分支；`readBDelay = isJTAG & rnw`（**JTAG 下任何读都延迟一拍**）；实现清单 `# DAP_TransferBlock : Done`（**无 SWD-only 限定**） | `orbtrace-1.4.3/orbtrace/debug/cmsis_dap.py:766-903`、`:113-121` |
  | `CMD_TRANSACT` 的 `MODE_JTAG` 路径：先 `JTAG_CMD_IR`、置 `JTAG_trans_os`，再由 `ST_DBG_WAIT_GOCLEAR` 给 `ST_DBG_HANDLE_TRANSACT` 补发 `JTAG_CMD_TFR` | `orbtrace-1.4.3/verilog/dbgIF.v:480-502, 611-613, 654-660` |
  | 35 位 DPACC/APACC 传输、多 TAP bypass、ACK 解析（ACK≠OK 即退出，**不会**卡死） | `orbtrace-1.4.3/verilog/jtagIF.v:142-273` |
  | ⚠️ **第十一轮更正**（上一行的"**不会**卡死"只覆盖 FAULT/ERROR 一侧）：`jtag_ack=OK(1)` 会被 `dbgIF.v:281-282` 翻成 `ACK_WAIT` ⇒ 进重试环 ⇒ 该环的 case5/6 握手**无超时** ⇒ **仍会真卡死** | `verilog/dbgIF.v:281-282`、`orbtrace/debug/cmsis_dap.py:843-864`；见 **B11.8** |

  ⇒ "固件不认识这条命令"说不通：**真不认识会走 `RESP_Not_Implemented` 回 `0xFF`，属立刻应答**；
    而实测是"彻底无声 + 停止服务 OUT 端点"，更像**卡在传输执行里**（FSM 不回 IDLE ⇒ `busy` 恒 1
    ⇒ 不再取 OUT 端点）= **命令被接住了，只是没回来**。
- **代码上可指认的卡死候选**（**未实测定性**）：

  | # | 候选 | 依据 |
  |---|---|---|
  | C1 | **链信息未下发/不符**：`jtagIF.v:242-273` 的 `ST_JTAG_WRITEIR` 靠 `ndevs`/`irlenx`；`irlenx[i]` 为 0 或与真实 IR 长度不符时 `tdxcount+1 == irlenx[...]` 永不成立，状态机原地不动（**该路径无超时保护**）⇒ 永不 done ⇒ 上位机永远等不到应答 | `jtagIF.v:253,261` |
  | C2 | **JTAG 时钟未跑**：jtagIF 靠分频产生的 rising/falling 推进；`SWJ_Clock` 未设/过小可能推不动它 | `dbgIF.v:334` |
  | C3 | **延迟读 vs 本层严格校验**：JTAG 下每笔读都延迟一拍，`readBIgnore` 丢掉首笔且**不计入** `transferBCount`（`cmsis_dap.py:866-880`），响应 count 与请求数可能差 1；而本层做了硬校验（`respCount != count` ⇒ `DAP_RES_ERROR`）⇒ 即使不挂死也会被判失败并永久回退 | `src/ORBMDK_HID.cpp:1633-1638` |

- **验证工具（已交付，2026-10-03）**：`test/jtagblockprobe.cpp`（直连 WinUSB，绕开本层）。
  构建：`powershell -File test\build_test.ps1 -Source jtagblockprobe.cpp`。
  做法：建链（`Connect(2)` → TAP reset → IDCODE → `JTAG_Configure` → `SWJ_Clock`）后，
  **每跑一步 0x06 就立刻 Ping 一次（Info 0xF0）**，据此给出三类判定：

  | 判定 | 现象 | 含义 / 下一步 |
  |---|---|---|
  | **A** | 0x06 有应答（ack/count 可读） | **可用** ⇒ "禁区"定性被推翻；再核对 count 与延迟读（C3） |
  | **B** | 0x06 无应答，但 Ping 仍活 | 命令未被处理/被吞，**设备没挂** ⇒ 查 C1/C2；属"可重试"，不是永久禁用 |
  | **C** | 0x06 无应答且 Ping 也死 | **复现挂死** ⇒ 仍按禁区处理，并记录"哪一步先死 + count/TAP 数/时钟"定位 C1/C2 |

- **🔴 首轮实机取证（2026-10-03，`jtagblockprobe` 首两次运行）—— 结论出人意料：`0x05` 也挂死，比 `0x06` 更早踩中**：

  | 步骤 | 现象 | 说明 |
  |---|---|---|
  | `SWJ_Clock(1 MHz)` | `11 00` | 正常 |
  | `Connect(2)` | `02 02`（= JTAG） | JTAG 模式进得去 |
  | `JTAG_Configure(0x15, 2 TAP, [4,5])` | `15 00`（status=0） | 链信息被接受 |
  | 手动位流扫链（TMS 6×1 → RTI→SelDR→CapDR→ShiftDR，Shift-DR 64 位） | `14 00 … 77 04 A0 4B 41 30 41 06` → **IDCODE[0]=0x4BA00477、IDCODE[1]=0x06413041** | 与本层 `JtagInitSequence` 的"count=2 DP@0(IR=4)"一致 ⇒ **这条通路完全健康** |
  | 固件 `0x16 JTAG_IDCODE` | `16 00 00 00 00 00`（**count=0**） | **固件 `0x16` 在此目标上不可用**（`jtagrawprobe.cpp:3-7` 早已记为已知现象）⇒ 建链**必须**手动位流扫链，不能依赖 `0x16` |
  | `Info`(Ping) | `00 01 03` | 命令通道健康 |
  | **`0x05` 读 DP IDCODE** | 只回 **3 字节**（正常应 7 字节 `05 status count data4`） | **短响应 = 固件在半途出错** |
  | **`0x05` 读 RDBUFF** | **读超时 1515 ms** | 卡住 |
  | 随后 `SWJ_Clock` / `Info` | **全部无应答** | ⇒ **挂死**，须插拔 USB |

  ⇒ **归因更正（重要）**：`0x06` **不是唯一凶手** —— **`0x05` 单独就能让探针挂死**。两者都走
    `dbgIF.v` 的 `CMD_TRANSACT`，所以卡死点在该命令的 **JTAG 执行路径**（支持候选 C 的机制描述），
    与"某条命令是否被实现"无关。这从**反面**再次证明"固件不支持 0x06"的定性不成立：
    不支持的命令不可能连累 `0x05`。
  ⇒ **JTAG 模式下真正要划的禁区是"让固件执行 DP/AP 事务的命令（`0x05`/`0x06`）"**，不是 `0x06` 一条；
    `Info` / `Connect` / `SWJ_Clock` / `SWJ_Sequence` / `JTAG_Configure` / `JTAG_Sequence`（含长位流）**都安全**。
  ⇒ ⚠️ 操作教训：工具早期版本在"`0x05` 超时"后把收尾 `Disconnect` 一并 SKIP，**设备才停在挂死态**。
    现已改为"救援性 Ping 与收尾**无条件执行**"，并新增 `--ping-only`（最短死活判定，exit=3 即为不响应）。
  ⇒ **第二轮（干净实验，2026-10-03，插拔后）**：`--skip-05 --stage 2` ⇒ **判定 A**：

  | 步骤 | 实测 | 结论 |
  |---|---|---|
  | `0x06` read DP IDCODE（count=1 读） | `06 00 00 0C`（n=4，**0 ms** 立刻回） | **有应答** ⇒ JTAG 下 `0x06` 这条命令**被实现且会回** |
  | 同上：`respCount=0`、无数据字节；`ack=0x0C & 7 = 4` | — | 正是候选 **C3**：`readBDelay = isJTAG & rnw` ⇒ **JTAG 下首笔读被丢弃、不计入 count**（`cmsis_dap.py:866-880`） |
  | 随后 `Info` | `00 01 03` | **探针活着**（"没处理" ≠ "挂死"） |
  | 收尾 `Connect(1)`/`Disconnect` | `02 01` / `03 00` | 正常 |

  ⇒ **`0x06` 在 JTAG 下不是挂死命令**：首笔读按 adiv5 规范**延迟一拍**（count 少 1、无数据），
    设备完全存活 ⇒ 本层只要按"延迟读"语义实现即可，**不必永久禁用**。
  ⇒ **真正挂死的是 `0x05`**（首轮已证）：`0x05 read DPIDR` 回 **3 字节短响应** → 第二笔
    `0x05 read RDBUFF` 读超时 → 随后 `SWJ_Clock`/`Info` 全无应答。两条命令走**不同的固件响应
    函数**（`RESP_Transfer_*` vs `RESP_TransferBlock_*`），"一条挂、一条好"自洽。

- **🟠 第三轮（加码 `--deep --stage 5`）—— 结果被坏链路污染，但暴露两条必须记住的操作约束**：
  1. **`Connect(1 SWD)` 收尾会毁掉后续 JTAG 会话（实测）**：第二轮收尾做了 `Connect(1)`
     （当时工具的默认行为），第三轮在同一目标上立刻扫链 ⇒ **全 `0xFFFFFFFF`（TDO 悬空）**。
     这就是"本目标 SWJ-DP 的 `JTAG<-SWD` 单向"的**实测证据** —— 此后**恢复 JTAG 必须给目标板
     **断电重启**；重开句柄/重发 `Connect(2)` 都无效（`Connect(2)` 仍回 `02 02`，但 TDO 已空）。
     ⇒ 工具已改：**默认不切回 SWD**（新增 `--restore-swd` 才显式切回）。
  2. **坏链路（TDO 全 1）上跑 `0x06` 写 ⇒ 挂死**：`0x06 write DP SELECT` 读超时 1516 ms，
     之后 `Info`/收尾全无应答。而同一条坏链路上 `0x06` **读**仍立刻回 `06 00 00 0C` 且存活 ——
     因为"首笔读被忽略"时固件**不发真正的 JTAG 序列**；写则必须真发 IR/DR ⇒ 卡在**执行**里
     （与候选 **C1**：`irlenx` 与真实链不符时 `ST_JTAG_WRITEIR` 原地不动，一致）。
     ⇒ 工具已改：扫链无效即**中止**（`--force` 才继续）。

- **🟢 第四轮（2026-10-03，好链路 + 读/写分离，★ 决定性结果）**：目标板断电重启 + 探针插拔后，
  链路确认良好（手动扫链读到 `0x4BA00477` + `0x06413041`），然后：

  | 步骤 | 实测 | 结论 |
  |---|---|---|
  | `0x06` count=1 **读** DP IDCODE | `06 00 00 0C`，**0 ms** 立刻回；随后 `Info` = `00 01 03` | **读安全**（首笔读被延迟丢弃，**不发真 JTAG 序列**） |
  | `0x06` count=1 **写** DP SELECT←0 | **读超时 1500 ms（rc=-2）**；随后 `Info` **无应答** | **写挂死** —— 好链路上**确定复发**，非污染 |
  | 收尾 `Disconnect` | 无应答 | 印证已挂死 |

  ⇒ **故障判据精确到"读/写"**：`0x06` **读可用、写挂死**。读之所以安全，是因为候选 **C3** 的
    `readBDelay = isJTAG & rnw` 让**首笔读根本不进 JTAG 执行**（固件立即返回 count=0）；一旦真去跑
    `WRITEIR/WRITEDR`（写，或第二笔读）就卡住。
  ⇒ 与候选 **C1** 吻合且指向更窄：**卡点在固件的 JTAG IR/DR 执行路径**（`ST_JTAG_WRITEIR` 一族），
    而不是"JTAG 整体不可用"（读 IDCODE、`JTAG_Configure`、手动扫链都正常）。
  ⇒ **⚠️ 重要旁证纠正**：手动扫链读 IDCODE **不需要写 IR**（TAP 复位后 IDCODE 直接在 DR 路径上），
    所以它**并不能证明固件 `irlenx[]` 表正确**；`JTAG_Configure@0x15` 回 `status=0` 也只说明"命令被
    接受"。`irlenx` 与真实链是否一致，**仍未验证** —— 这正是下一步要卡的。

  ⇒ **下一步（探针需再插拔一次；目标板**不必**断电 —— 本轮默认没切回 SWD）**：
    1. `--ping-only` 确认复活；
    2. **在"写"之前先做 IR 往返自检**（**已实现**于 `jtagblockprobe`：建链后默认跑，`--no-ir-rt` 跳过）：
       写 `IR=全1`（BYPASS 指令，**与 TAP 顺序无关**）→ `Shift-DR` 读 2 位，**期望全 0**：
       · 这一步**挂** ⇒ 锁定"IR 执行路径"本身（问题比 `0x06` 更基础）；
       · 读回**非 0** ⇒ **IR 总长度与真实链不符** ⇒ 候选 **C1 直接成立**（`irlenx` 错）；
    3. 若 IR 往返正常 ⇒ 再试 `--ir 5,4` / `--cfg 0x0C` 变体，看写是否因配置而活。

- **🟣 第五轮（2026-10-03，IR 往返自检 —— ★ 结果反转了上一轮的结论）**：探针插拔后（目标板**未**断电）
  跑 `--skip-05 --stage 2`（本轮新增的 IR 往返自检**排在 S2 之前**）：

  | 步骤 | 实测 | 结论 |
  |---|---|---|
  | `0x14` Shift-IR **9 位全 1**(BYPASS) → Shift-DR 读 2 位 | `14 00 00` → **读回 0x00** | **BYPASS 生效** ⇒ **IR 总长 = 9 = cpu4 + bs5，与真实链一致** |
  | 之后再手动扫链一次 | `0x4BA00477` + `0x06413041` | **链仍活** ⇒ `0x14` 的 IR/DR 位流引擎**完全健康** |
  | **`0x06` count=1 读 DP IDCODE（在 IR 往返之后）** | **无应答（rc=-2, 1516 ms）**，随后 `Info` 也死 | **挂死！**（**同一操作**在第二/四轮是 0 ms 秒回的） |

  ⇒ ① **候选 C1 在本目标上被否定**：IR 长度表 `4,5` 与真实链**一致**（BYPASS 判据是"读回全 0"这种
    二值信号，不会误判）；`0x14` 原始位流下的 IR 写也**完全正常** ⇒ "IR 执行路径坏"不成立。
  ⇒ ② **"`0x06` 读安全"是**有条件**的**：它只在**固件自己最后一次设定过 IR 之后、外部没有改写 IR**
    时安全。本轮在固件动作（`0x16`，`jtagblockprobe.cpp:735`，**排在 IR 往返之前**）之后，用原始
    位流把 IR 打成了 **BYPASS**，`0x06` 就**连首笔读都挂死**。⇒ 敏感量是"**IR 被外部改写**"，
    而**不是** IR 长度（第五轮已证 9 位正确）、**也不是** TAP 状态（第二/四/六轮的扫链同样结束在
    `Exit1-DR`，都安全）。
  ⇒ ③ **这很可能就是 §18.9 第十步那次原始挂死的真因**：当年本层是"主机侧 `0x14` 引擎 + 固件块传输
    `0x06` **混用**"—— 引擎自己写过 IR（改成 DPACC/APACC），随后 `0x06` 撞上失配。**现在 JTAG 全程
    只走主机侧引擎**（`src/ORBMDK_RDDI.cpp:1571` `DapTransferFor`：非 SWD 一律 `JtagDapTransfer`），
    等于已绕开此坑。B9 因此从"现象定性"升级为"**混用触发器的实测复现**"。

- **🟢 第六轮（2026-10-03，干净 A/B —— ★ 钉死"唯一变量 = IR 往返"）**：探针重插后跑
  `--no-ir-rt --skip-05 --stage 2`（**唯一差别**：跳过 IR 往返，其余步骤逐字相同）：

  | 轮次 | S2 的 `0x06` count=1 读 DP IDCODE | 之后 Ping |
  |---|---|---|
  | 第四 / 第六轮（**不动 IR**） | `06 00 00 0C`，**0 ms 秒回** | 活 |
  | 第五轮（**IR 往返：IR ← BYPASS**） | **无应答，1516 ms** | **死** |

  ⇒ **A/B 成立**：同一目标、同一会话条件、同一操作，**唯一变量就是那一次 Shift-IR**。
  ⇒ **机制收敛为一条**（能同时解释全部六轮）：**固件在 JTAG 下会自行把 IR 置成 DPACC 并"记着"它，
    此后不再重写 IR；外部（本层 `0x14` 位流）一旦改写 IR，实际 IR ≠ 固件认知 ⇒ 后续 DR 扫到错误
    指令的 DR（TMS 复位后实际是 **IDCODE** 的 32 位 DR）⇒ 拿不到有效 ACK ⇒ **固件原地自旋、无超时**
    ⇒ 上位机 5 s 读超时 + 端点停摆（只能插拔 USB）。**
    · 第二/四/六轮安全的原因：`0x16` 是**固件命令**且**排在扫链之后** ⇒ 等于由固件把 IR 重新摆好；
    · 第五轮危险的原因：IR 往返**排在 `0x16` 之后** ⇒ 把固件刚刚摆好的 IR 又改掉了。
  ⇒ **直接坐实 §18.9 第十步的原始挂死真因**：当年本层是"主机侧 `0x14` 引擎 + 固件块传输 `0x06`
    **混用**" —— 引擎为读 DP/AP 而**反复改写 IR（DPACC↔APACC）**，固件那侧完全无从得知 ⇒ 必挂。
    **现在 JTAG 全程只走主机侧引擎**（`src/ORBMDK_RDDI.cpp:1571` `DapTransferFor`：非 SWD 一律
    `JtagDapTransfer`）⇒ 已彻底绕开。**本层铁律：JTAG 下"主机 `0x14` 引擎"与"固件 `0x06`/`0x05`"
    绝不可混用。**
  ⇒ **可选收尾验证（原计划；⚠️ 已被第八轮判定"无意义"，勿再执行）**：IR 往返之后
    **用 `0x14` 把 IR 写回 DPACC**（9 位；本层 IR 码见 `src/ORBMDK_RDDI.cpp:1076-1082`：
    `DPACC=0x0A`、`BYPASS=0x0F`；链序约定 `ir_length[0]` 离 TDO 最近）→ 再发 `0x06`：
    原设"**若恢复可用** ⇒ IR 失同步正反闭环"。**但第八轮已证：IR 未动照样挂** ⇒ 此验证即使做也
    得不出"可用"，**已作废**（保留记录以说明为何当时这么想）。

- **🔴 第七轮（2026-10-03，补上缺失前置 `0x04` —— 未解决；但确立"挂死 = 停顿，不是重试循环"）**：
  ★ 发现探针 `test/jtagblockprobe.cpp` **从来不发 `0x04 = ID_DAP_TRANSFER_CONFIG`**（`wait_retry`/
  `match_retry` 一处没有），而本层在 `CMSIS_DAP_Connect` / `JtagInitSequence` 里是发的
  （`DAP_ConfigureTransfer(0, 100, 10)`）—— 即"执行即挂"的全部结论都建立在一个**缺前置配置**的会话上。
  已补上（载荷 `[04][idle][wait_retry:2][match_retry:2]`），固件回 `04 00`（接受），并加 `--no-xfer-cfg`
  做 A/B。**但本轮误带默认 IR 往返**（本想测写、漏了 `--no-ir-rt`），结果 S2 的 `0x06` 读**又挂死**：
  - A/B 变成 **2 : 2 且可重复** —— 不动 IR（第四/六轮）**活**、动 IR（第五/七轮）**挂**，可复现性确立；
  - ★ **挂死不是重试循环**：若 `wait_retry=100` 管这条路径，最坏也该是"重试 100 次后回 `NO_ACK`"，
    而不是**无声死等** + 端点停摆（连 `Info` 都发不进去）⇒ 这是**停顿（stall，无超时保护）**，
    与 C1 的"`ST_JTAG_WRITEIR` 的 `tdxcount+1 == irlenx[...]` 永不成立、永不 done"完全一致。
    ⇒ **`0x04` 救不了这个挂因**（重试上限管不到"状态机不推进"）。
  - 探针已加防呆：`--stage >= 2` 时**自动跳过** IR 往返（要诊断须显式 `--ir-rt`），免得再白烧一次插拔。
  ⇒ **待验**（当轮未做，见下轮）。

- **⛔ 第八轮（2026-10-03，**终局**）：`0x04` 已补、IR 完全未动 ⇒ `0x06` 写**照样挂死**。目标"启用"判定为做不到。**
  插拔后单跑 `bin\jtagblockprobe.exe --skip-05 --stage 3`（防呆生效，IR 往返**自动跳过**，日志有明示）：
  - S0：`0x04` 回 `04 00`（接受）、`0x15` 回 status=0、手扫 IDCODE = `0x4BA00477` / `0x06413041`（链路好）、
    S0 后 Ping **活着** —— 起点干净，`0x04` 确已生效。
  - **S2 读**：`06 00 00 0C`，ack=4/FAULT、count=0，随后 Ping **活着**。★ **但这仍不算"读可用"**：
    `bug.md:200` 已记 `readBDelay = isJTAG & rnw` ⇒ **JTAG 下首笔读被丢弃、根本没进 JTAG 执行**，
    这个"秒回"只是"未执行"的产物。故 `0x06` 读在 JTAG 下**无任何一次真实成功**。
    ⚠️ **第十一轮更正**：上句"没进 JTAG 执行"**不成立**（读确实发了真事务）；"秒回"是因为这一笔的
    `ack=4` 走了"立即错误返回、不重试"分支。机理与判据见 **B11.8**。
  - **S3 写**：`0x06 write DP SELECT x1` ⇒ **1500 ms 超时挂死**，随后连 `Info` 都发不进去（端点停摆）。
  - ⇒ **决定性结论**：写挂死**与 IR 失同步无关**（本轮 IR 一步没动）、**与 `0x04`/重试上限无关**
    （已配置 `wait_retry=100`）。**第二个挂因独立存在且在固件侧**：`ST_JTAG_WRITEIR` 等固件 JTAG
    事务状态机**不推进 / 无超时保护**（C1）。**本层无法修复、也无法"启用"** —— 任何配置/顺序调整都
    救不了"状态机不推进"。
  - ⇒ 第四轮的写挂死于本轮**100% 复现**，且排除两个外因 ⇒ 归因闭环，**不再需要任何探针实验**。

- **⛔ 第九轮（2026-10-03，"解楔/启用"最后一搏）：命令级重初始化**全部无效**；现象重框架为"首笔被吞、第二笔起必挂"。**
  插拔后 `bin\jtagblockprobe.exe --skip-05 --stage 4 --deep --write-first --reconnect-between`：
  - **S3（第一笔 `0x06`，写）**：`06 00 00 0C`，ack=4、count=0、**0 ms 秒回**，S3 后 Ping **活着**。
  - **S3.6（第二笔之前）全套重初始化**：`0x14` TMS reset → `0x03` Disconnect → `0x02` Connect(JTAG) →
    `0x15` Configure，逐步均正常、Ping **全程活着**（说明 JTAG 层 + DAP 连接层都重初始化成功）。
  - **S4（第二笔 `0x06`，读 x2）**：**1500 ms 超时挂死**，随后 `Info` 也发不进去。另一路 `--reset-between`
    （仅 `0x14` reset + `0x15` Configure）同样解不开。
  - ⇒ **楔死点分层定位（本轮新增、很硬）**：首笔 `0x06` 之后 `0x14`/`0x15` **仍可用** ⇒ 楔住的**只是
    "DAP 事务"这条路**，不是整条 JTAG 路径；而 **JTAG 层（`0x15`）与 DAP 连接层（`0x03`/`0x02`）的
    重初始化都清不掉它** ⇒ 该闩锁**只有整芯片 USB 复位（插拔）能清**，命令级**不存在任何恢复入口**。
  - ⇒ **现象再用一条规则统一（比本节原"读安全 / 写挂死"更准）**：JTAG 下 **`0x06` 的第一次发送会被
    "吞掉"**（立即回 `06 00 00 0C`、count=0、**不进真实 JTAG**、**存活**），**此后本上电周期内再发必挂** ——
    判据是"**是不是本会话第一笔**"，**与读/写无关**。**第八轮的"写挂死"实为"第二笔"**（其 S2 读已先吞掉
    第一笔）；第二/四轮"读秒回"也正是"首笔被吞"。**历轮全部数据在这条规则下自洽。**

- **★★ 第十轮（2026-10-03）：根因上溯到上游固件源码 `C:\Users\taibaibu\Desktop\orbtrace-1.4.3`（探针 gateware）。**
  探针"固件" = ECP5 gateware：`verilog/dbgIF.v` + `verilog/jtagIF.v`（`orbtrace/debug/dbgIF_wrapper.py`
  以 `Instance("dbgIF", ...)` 例化，**真正跑在探针上的就是这两份 verilog**）；命令层是 Migen 的
  `orbtrace/debug/cmsis_dap.py`。
  - **根因 R1（决定性）：JTAG 的 ACK 语义接反。** `jtagIF.v:174-190` 把**第一个 TDO 位**收进 `ack[0]`
    （`0:ack[0]<=tdo; 1:ack[1]<=tdo; 2:ack[2]<=tdo`）⇒ `jtag_ack` 即 ADIv5 三位 ACK 的 LSB-first 值：
    **OK=001=1、WAIT=010=2、FAULT=100=4**。而 `dbgIF.v:281-282`
    `ack = (active_mode==MODE_SWD)?swd_ack:(jtag_ack==1)?ACK_WAIT:((jtag_ack==2)?ACK_OK:ACK_ERROR);`
    ⇒ **真 OK(=1) 被判成 WAIT、真 WAIT(=2) 被判成 OK**（源码注释 `//not the same at the JTAG versions (!!!)`
    正是把"接反"当成"编码不同"）。健康 DP 的每一笔正常访问都被判成 WAIT。
  - **根因 R2（放大器）：WAIT 重试环 + `done` 两台阶握手无超时 + `busy` 恒 1。** `cmsis_dap.py:843-859`：
    `ack==ACK_WAIT` ⇒ `tfB_txb<=Mux(Bretries!=0,4,8)` 回 case4 **重发同一笔**；case4→5→6 靠 `dbg_done`
    （`dbgIF.v:284` `done=(dbg_state==ST_DBG_IDLE)`，`cmsis_dap.py:1151-1152` CDC 同步）做**"引擎离开 IDLE
    → 回 IDLE"两台阶**握手（`dbgIF.v:662-679` / `:629-651`），**整链没有任何超时** ⇒ 一旦这笔重发没走完两台阶，
    就**永久停在 case5**、根本走不到 `case8` 的上限收尾；期间 `busy` **恒 1**（只在写路径 case0-3 的
    `:831-832` 清 0）⇒ 不取 OUT 端点 ⇒ **上位机 1500 ms 超时、连 `Info` 都发不进去** —— 第九轮看到的"挂死"。
  - ⚠️ **第十一轮更正（2026-10-03）**：上一版把 R2 写成"`Bretries` **无复位默认值(=0)** ⇒ 欠载成 `0xFFFF` ⇒
    **65535 次重发**" —— **不成立**：`cmsis_dap.py:797` 每笔 Setup 都 `Bretries.eq(waitRetry)`，`waitRetry`
    复位值 = 4096（`:219`），**不会欠载**；且该上限**只管"要不要重回 case4"**，跳不跳得出 case5/6 与它无关。
    **挂死的本质是"等待环无超时"，不是"重发次数"**（详见 B11.8）。
  - **R1+R2 完整解释了第九轮"首笔活、次笔挂"**：S3 写回来的 `ack=4` = ACK_ERROR，对应 `jtag_ack=100`
    = ADIv5 **FAULT**（DP 当时不在可访问态）⇒ 走 `:862` `Elif(ack!=OK|perr)` **立即错误返回、不重试** ⇒ 存活；
    S4 读时 DP 已正常回 **OK(=1)** ⇒ 被 R1 判成 WAIT ⇒ 进重试环 ⇒ 卡在 case5/6 的**无超时握手** ⇒ 挂死。
    **判据不是"第几笔"，而是"目标这笔的 `ack` 落在分叉哪一边"**（⚠️ 不是"65535 次重发"，见上 ⚠️ 更正）
    ⇒ 之前"第一笔被吞/第二笔起必挂"的框架是**现象假象**，本轮更正。**完整定论见 B11.8。**
  - **同源附带缺陷**：
    - `cmsis_dap.py:491` `dbgif.dev.eq(rxBlock.bit_select(8,4)-1)`：`bit_select(8,4)` 4 位而 `dev` 只 3 位，
      **Count=0 时 `-1`→`0xF`→截成 7** ⇒ `ndevs=7`；且 `:192` `jtag_ircount = Signal(5)` 每 TAP +5，
      **≥7 TAP 时回卷**(35→3) 写花 `irlenx`。
    - `jtagIF.v:253,261` `tdxcount[4:0]+1 == irlenx[5*devcount +:5]`：**`irlenx` 字段为 0（未配置/越界）时
      永不成立且无超时 ⇒ `ST_JTAG_WRITEIR` 死循环** ⇒ `idle` 永不拉高 ⇒ dbgIF 停在 `WAIT_INFERIOR_FINISH`
      ⇒ `done` 恒 0 ⇒ 真·永久挂死（即"候选 C1"；**本次链 2 TAP、`irlenx={4,5}`，不是它触发的**）。
    - `jtagIF.v:174-190` ACK 捕获**未按 `devcount==dev` 门控**：多 TAP 链里后面的 bypass 器件在 `tdxcount=0`
      仍执行 `ack[0]<=tdo`，**覆盖目标器件的 ACK[0]**（本链 target=dev0、dev1 bypass ⇒ `ack[0]` 被 dev1 的 TDO 顶掉）。
  - **⇒ 结论**：JTAG 下 `0x05`/`0x06` **在本层不可能可用**——不是"参数没配对"，而是**固件 ACK 语义接反 + 无超时重试环**。
    本层继续只走 `0x14 ID_DAP_JTAG_SEQUENCE` 通用位流（`src/ORBMDK_RDDI.cpp` 既有策略）是**正确且唯一可行**的。

- **定性（2026-10-03 第八轮后，第七次也是最后一次更正 —— ★★ 最终定论）**：
  - **`0x05`：挂死**（首轮实测）——固件 `ID_DAP_TRANSFER` 在 JTAG 下**不可用**。
  - **`0x06` 写：挂死**（第二/四/八轮；第八轮在"`0x04` 已配 + IR 未动 + 链路好 + 起点 Ping 活"下复现）
    ⇒ 根因**不在 IR、不在重试上限**，在固件 JTAG 事务状态机 ⇒ **`ID_DAP_TRANSFER_BLOCK` 在 JTAG 下不可用**。
  - **`0x06` 读：从未真实成功过**；但"秒回"的机理**不是**"首笔读被丢弃 / 未进 JTAG 执行"（⚠️ 第十一轮更正）：
    读在 `cmsis_dap.py:800` 与写一样直接进 case4 发 `go`（`Mux(rnw,4,0)`），**确实发了真事务**；`readBDelay`
    只决定"这笔数据算不算进 `count`"。秒回的真因是这次拿到的 `ack=4`（`06 00 00 0C` 第 4 字节
    = `Cat(ack,perr,0000)` = `0b0000_1100`）走了 `:862` 的"立即错误返回、不重试"分支。详见 B11.8。
  - **挂死形态：停顿（stall），不是重试循环**（第七/八轮）：`wait_retry=100` 已配仍无声死等 ⇒
    重试上限管不到它；现象 = "不吐字节 + 端点停摆（`Info` 都发不进去）"，与状态机"永不 done"一致。
  - ⇒ **两处独立挂因**（第八轮后拆分清楚）：
    ① **DAP 事务本身**：`0x06` 写，**IR 一步没动也挂**（第二/四/八轮）⇒ **主因**，在固件 JTAG 事务状态机；
    ② **IR 被外部改写**：第五/七轮，先打成 BYPASS 后连**读**都挂 ⇒ **叠加的第二因**（同样自旋）。
    ⇒ 故"只要 IR 没被改写就能用"**不成立**（第八轮即反例：IR 未动，写照样挂死）。
  - ⇒ **候选更正**：**C1（`irlenx` 与真实链不符）已被第五轮否定**（IR 总长实测 = 9，表 `4,5` 正确）；
    **C2（时钟）** 也早被"同一时钟下扫链/写 IR 都正常"排除；**C3（延迟读）** 只解释"首笔读不计入
    count"，**不能**解释"首笔读为什么也会挂"。
  - ⇒ **剩余唯一未排除的机制（假设，固件侧、本层无法验证）**：`irlenx` **数值对**，但固件
    `ST_JTAG_WRITEIR` 的**完成判据与实际移位数对不上**（如把 DP 的 `irlenx` 当旁路长度、或判据用了
    移位数而非"进入 EXIT1"），⇒ **计数永不自增、状态机永不 done** ⇒ 无超时保护下表现为**永久停顿**。
    这条同时解释①②：IR 被外部改写后，固件"写 IR"那一步的计数同样对不上 ⇒ 也自旋。
  - ⇒ **本层结论（不变，且理由更硬）**：**JTAG 下禁用一切固件 DP/AP 事务（`0x05`/`0x06`），全程走
    主机侧 `0x14` 位流引擎**（`src/ORBMDK_RDDI.cpp` `DapTransferFor`，已实现），**两者绝不可混用**；
    SWD 路径不受影响。
- ⇒ **第九轮补充（认识更正）**：本节上文"读安全 / 写挂死"的二分**不是根判据**；且该"闩锁"**命令级不可解**
  （TAP reset / Disconnect+Connect / Configure 全无效），**只有 USB 插拔（芯片复位）能清**。
  ⚠️ **第十轮再次更正**：第九轮那条"**判据 = 是不是本会话第一笔 `0x06`**（首笔被吞、第二笔起必挂）"
  **是现象假象**，已作废；真正判据是"**目标这一笔有没有回 ADIv5-OK**"（回 OK ⇒ 固件误判 WAIT ⇒ 65535 次
  重发 ⇒ 挂死）。详见上方"★★ 第十轮"。
- **归口**：固件侧行为不可改（⇒ 入本档）；验证工具落在 `test\`。**"是否放开固件 `0x05`/`0x06`"已决策
  = 不放开**（第八轮定论，执行记录见 `Todo.md.bak` §18.9 第十步；重开条件亦在该处）。
- **留档理由**：以后看到"JTAG 下 0x06 挂死"，先分清**现象（成立）**与**定性（当时写错了）** ——
  不要再用"固件没实现"解释；要区分"真挂死"还是"只是没应答"，**用 `jtagblockprobe` 的每步 Ping 分界**。

## B11 · `ST_JTAG_WRITEIR` 的 `tdxcount/irlenx` 机理复核（探针=1.4.3）（2026-10-03）

> 背景：B9 里留作"未排除"的头号候选 C1 是 `jtagIF.v` 写 IR 的完成判据 `tdxcount[4:0]+1 == irlenx[5*devcount +:5]`
> "永不自增/永不 done"。本轮把它逐位追了一遍。**结论：对已正确下发 `0x15` 的链不成立（C1 可正式关闭），
> 但它是一个"字段=0 就 100% 永久卡死"的地雷。**

### B11.1 代码与位宽（1.4.3 `verilog/jtagIF.v:242-273`，1.4.4 同段一字未改）

```verilog
ST_JTAG_WRITEIR:
  tmsbits <= 6'b000010; tmscount <= 2; next_state <= ST_JTAG_IDLE;
  if (falling)
    begin
       tdi <= (devcount==dev)?ir[tdxcount]:1'b1;
       if ((devcount==ndevs) && (tdxcount[4:0]+1 == irlenx[ 5*devcount +:5 ]))   // :253 末位拉 TMS
         tms <= 1'b1;
    end
  if (rising)
    begin
       tdxcount <= tdxcount+1;                                                   // :259
       if ( tdxcount[4:0]+1 == irlenx[ 5*devcount +:5 ] )                        // :261
         begin
            if (devcount==ndevs) jtag_state <= ST_JTAG_TMSOUT;                   // 唯一正常出口
            else begin devcount <= devcount+1; tdxcount <= 0; end
         end
    end
```

位宽：`tdxcount` = `reg [5:0]`；`irlenx` = `[29:0]`（6×5 位，`:191-192` "Length of each IR-1"）；`ndevs` = `[2:0]`；`dev` 端口 = `[2:0]`（`:102`）。

### B11.2 为什么"字段=0 ⇒ 必挂"（可证明）

- 右侧 `irlenx[5*devcount +:5]` 是 5 位字段，取值 **0..31**。
- 左侧 `tdxcount[4:0]+1`：5 位切片（0..31）与无位宽常数 `1` 相加，按 Verilog 规则在 **32 位**上下文求值 ⇒ 取值 **1..32**（只有 `tdxcount[4:0]==31` 时才是 32）。
- ⇒ **等式成立的必要条件是字段 ≠ 0**。**字段 = 0 ⇒ 永远不等 ⇒ `jtag_state` 永不离开 `ST_JTAG_WRITEIR`**（`tdxcount` 6 位自增到 63 回绕，无限继续）。
- 该状态里除 `:261` 外没有任何其它出口；1.4.3 的 `jtagIF.v` 是 `always @(posedge clk)`（`:290`，**没有复位项**）。
- ⇒ `idle`（= `jtag_state==ST_JTAG_IDLE`）永不置起 ⇒ `dbgIF.v` 的 `ST_DBG_WAIT_INFERIOR_FINISH`（`:639-643` 只等 `jtag_idle`，**无超时**）永久等待 ⇒
  调度器 `cmsis_dap.py` 的 `RESP_Wait_Done` / `tfr_txb==8` 永不满足 ⇒ **USB 无应答（真挂死）**，且**只能 FPGA 重配置（插拔/断电）才恢复** —— 与"只有插拔 USB 能清"的实测吻合。

**字段语义（易踩）**：逐位追 `field=4` ⇒ tdxcount=0,1,2,3 移出 `ir[0..3]`，第 4 位（tdxcount==3）时 `:253` 置 TMS=1，下一个上升沿 `:261` 命中并进 TMSOUT ⇒ **恰好移出 4 位、末位带 TMS** ⇒ **字段 = IR 实际位数**。
而 `dbgIF.v:192` 注释写的是 "Length of each IR-1"（长度-1），`dbgIF.v` 头注释也写 `(0..31)+1`；**注释与实现相反，实现才对**：`cmsis_dap.py:499` 打包时取的是 `IRLength` 字节的低 5 位（`bit_select(0,5)`），即 CMSIS-DAP 的"实际位数"语义。
⚠ 由此推出一个真地雷：**IR 长度 = 32 的器件（字节 32 → 低 5 位 = 0）会让该字段为 0 ⇒ 必挂**。

### B11.3 会挂的 4 个条件（任一满足）

| # | 条件 | 说明 |
|---|---|---|
| 1 | **从未下发 `0x15`** | 上电后 `irlenx` 全 0，且 `ndevs/irlenx` **不在 `dbgIF.v:391-400` 的复位表里** ⇒ 任何"需要写 IR"的 JTAG 事务必挂（`0x16` 不走 `irlenx`，`0x14` 走主机侧位流，都不受此限） |
| 2 | 链上 index ≤ `ndevs` 的任一器件 IR 长度 = 32（或 0） | 字段截断为 0 |
| 3 | `Count-1`(=ndevs) 与实际填的字段数不匹配 | 少发字节 ⇒ 落 `Error`（不挂）；多发/字段留 0 ⇒ 挂 |
| 4 | `Count ≥ 7` 且走到 `devcount ≥ 6` | `5*devcount > 29` 越界读（`irlenx` 只有 30 位），仿真为 x、综合多为 0 ⇒ 同"字段=0" |

### B11.4 对你们那条链为什么**不**成立

`0x15 Count=2 / IR={4,5}` ⇒ `ndevs<=dev` 得 `ndevs=1`，字段 0=4、字段 1=5（`:574-575`）：
`devcount=0`：`tdxcount=0..3` 移 4 位，`3+1==4` 命中，`devcount(0)!=ndevs(1)` ⇒ `devcount<=1, tdxcount<=0`；
`devcount=1`：移 5 位，`4+1==5` 命中且 `devcount==ndevs` ⇒ TMSOUT ⇒ **正常退出**。
⇒ **C1 关闭**，与第五轮实测"IR 总长 9=4+5 与真链一致、BYPASS 生效"完全一致。

**一眼判定实验（建议补做，能一次性结案）**：故意发 `0x15 Count=2` 但把第 2 个 IRLength 写 0（或 32），再做一笔 `0x06` 读 ——
**必挂（只能插拔恢复）⇒ 机理坐实**；**仍活 ⇒ C1 彻底出局**（预期是后者）。

### B11.5 复核中挖出的另一条（更贴合"`0x16` 回 0"）——1.4.3 `active_mode` 漏写

- `0x02 Connect(JTAG)` → `CMD_SET_JTAG`（`:516-525`）只设 `active_mode <= MODE_LOCAL(3)` / `commanded_mode <= MODE_JTAG(2)`；
  1.4.3 的 `ST_DBG_ESTABLISH_MODE` 收尾**不恢复 `active_mode`** ⇒ 建链后 `active_mode` **一直是 `MODE_LOCAL`**（1.4.4 才补 `active_mode <= commanded_mode`）。
- `CMD_JTAG_GET_ID`（`:527-532`）只设 `commanded_mode`，**不设 `active_mode`**（1.4.4 补）⇒ 它的 `dbg_state<=ST_DBG_WAIT_INFERIOR_START`，
  而 `ST_DBG_WAIT_INFERIOR_START` 的 `case(active_mode)` 在没有 `MODE_JTAG` 时落 **`default: perr<=1'b1; dbg_state<=ST_DBG_WAIT_GOCLEAR`** ⇒
  **命令体根本没执行、JTAG 引擎没启动**（不是"不支持 0x16"）。
- 叠加：`0x16` 的响应路径 `RESP_JTAG_IDCODE_Setup/Process`（`cmsis_dap.py:514-532`）**不把 `perr` 写进状态字节**（只有通用 `RESP_Wait_Done` 才写 `perr?0xff:0`）
  ⇒ 实机看到的是 **"成功 + IDCODE 全 0"**，即早先记的 **"固件 `0x16` 回 count=0"**。
- **可测预言**：`Connect(JTAG)` → `0x16` ⇒ 必回全 0；`Connect(JTAG)` → 一笔 `0x05/0x06` → `0x16` ⇒ 必能扫到 2 个 IDCODE。
  原因：`CMD_TRANSACT` 在 `:499` **无条件**写 `active_mode <= commanded_mode`(=MODE_JTAG)；
  而 `0x14` 走 `CMD_PINS_WRITE`（`:441-445`）把 `active_mode` 写成 **`MODE_SWJ`(1)** ⇒ `0x14` 之后再发 `0x16` **依旧失败**。
  ⇒ 这三条合起来可精确区分"引擎没起"和"引擎起了但结果错"。
- ⇒ 你 1.4.4 里补的那三处 `active_mode` + `ESTABLISH_MODE` 收尾，正是 `0x16` 能工作的**必要条件**。

### B11.6 仍未排除的候选（0x05/0x06 真挂）

- `0x05/0x06` 因 `:499` 无条件写 `active_mode`，**不受 B11.5 影响** ⇒ "读活/写死"既不能用 C1、也不能用 B11.5 解释，仍需以下之一：
  - **(a) posted-read 收集事务**：JTAG 的"任何读"要额外发一笔 `tfrReq=0x0e`（读 DP RDBUFF）去收取数据（`cmsis_dap.py:626-650`）。
    若卡在"第二笔"，表现正是**响应只回了 3 字节就被截断**（`[0x05][count][tfr-response]` 已出，4 字节数据未出）—— 与首轮"`0x05` 读只回 3 字节 → 后续全无应答"高度吻合，是当前最像的一条。
  - **(b) WAIT 重试风暴**：`waitRetry` 默认 4096（`cmsis_dap.py:219`，`0x04 DAP_TransferConfigure` 可被主机设到 65535），每次重试都是一笔完整 JTAG 事务 ⇒ 表现为"长时间无应答"，**不是真死**（用 `jtagblockprobe` 的 Ping 间隔可区分）。
    ⚠️ **第十一轮更正**：`Bretries` 上限（= `waitRetry`，每笔 Setup 重置，`cmsis_dap.py:797`）**不是挂死的原因**
    —— 它只管"要不要重回 case4"；**卡死发生在 case5/6 的无超时握手，与上限无关**，所以第九轮那种"只能插拔"
    的**真挂死**不能用 (b) 解释。Ping 间隔区分法本身仍有效。定论见 **B11.8**。
### B11.7 `tfr_txb` 6→7→8 追查（本轮，`go` 发了但引擎没起 的窗口）

- **握手判据不是脉冲而是电平**（`cmsis_dap.py:1151-1152`）：`done_cdc.eq(Cat(done_cdc[1],dbgif.done))`、**`dbg_done.eq(done_cdc==0b11)`**
  ⇒ 要求 `done` **连续 2 拍为高**才算"完成"。另 `tfrram.we.eq(done_cdc==0b10)`（`:1155`）是 `done` 的**下降沿**触发（=dbgIF 接受下一条命令的瞬间锁存 `dread`，取到的正是"上一笔的结果"）——
  与注释"at the rising edge of done"字面相反，但行为是 posted-read 想要的，**不是缺陷**。
- 状态 6（`:686-695`）发 `go=1` 并**在同周期**进 7；状态 7（`:697-702`）**只有一个 `with m.If(self.dbg_done==0)`，没有 else**：
  进 7 时 dbgIF 必然仍 IDLE（`done=1`⇒`dbg_done=1`），于是 **7 先停住并保持 `go=1`**，直到 dbgIF 真正离开 IDLE
  （`{cdc_go,go}==2'b11` 的 2 拍同步 + `CMD_TRANSACT` 还额外要求一次**目标时钟 fallingedge**，`dbgIF.v:480-502`）⇒ 这段**自洽，不是挂死点**。
  但 7/8 **没有任何超时**，`tfB_txb` 5/6（`:842-852`）同构。
- **窗口结论**：
  - 若引擎**卡在非 IDLE**（如 B11.2 的字段=0）⇒ `jtag_idle=0` ⇒ `WAIT_INFERIOR_START` 立刻放行 ⇒ 走 FINISH 回 IDLE ⇒ **不是永久挂死**（只是垃圾结果/误判 ack）。
  - 只有当**引擎一直是 IDLE、却始终不接受这一次的 `go`**（引擎接受条件是 `go & 目标时钟沿`，时钟沿永不到）⇒ `WAIT_INFERIOR_START`（`dbgIF.v:639-643`，无超时）**永久等待** ⇒ `dbg_done` 恒 0 ⇒ 状态 8 永久等待 ⇒ USB 永久无应答、只能重配置。
  - ⇒ "真挂死"的充分条件被收紧为：**dbgIF 的 `if_go` 已有效，但 jtagIF 永远等不到可用的目标时钟沿（TCK 停摆）**。待实机判别项：`0x11 SWJ_Clock` 分频被写坏 / `0x14` 把 TCK 位拉低 / 模式切换后时钟源未切回（与 B11.5 同源的一类"1.4.3 漏写"）。
- **判别实验二（待实机，2026-10-03 立项）**：目的 = 判别上式（TCK 停摆 ⇒ 永久等待）是否成立。
  - 前置：**需插拔 USB**；目标上电后**首次连接即用 JTAG**（本目标 `JTAG←SWD` 单向，切回 SWD 即污染链路）。
  - 步骤：① `jtagblockprobe --skip-05 --stage 2` 确认基线（`0x06` 读秒回 `06 00 00 0C`、Ping 活）
    → ② **单独发一笔 `0x11 SWJ_Clock`**（分频写极端值：`0` 或极大）
    → ③ 紧接着 `0x06` **读**（或直接 Ping）；用 **Ping 间隔**区分"停顿"与"重试循环"。
  - 判据：随即 1500 ms 超时 **且 Ping 无应答**（须插拔才恢复）⇒ 条件成立（时钟沿不来 ⇒ `WAIT_INFERIOR_START` 永久等待 ⇒ 状态 8 挂死）；
    若 `0x06` 仍秒回 ⇒ **排除"时钟停摆"**，回到 **B11.6 的 (a) posted-read 收集事务 / (b) WAIT 重试风暴**。
  - ⚠ 不换固件则本实验**只用于定性记录**，不改变"本层禁用固件 `0x05`/`0x06`"的现状（B9 第八轮定论仍有效）。
- **★ 判别实验二 结果（2026-10-03，已闭环；工具新增 `--clock-late HZ`）**：
  - 跑法（默认保持 JTAG，**两跑之间无需插拔**）：`--skip-05 --stage 2 --clock-late 1000000`（对照）/ `--clock-late 0`（处理）。
  - **对照组也挂死**（此即结论）：S0 全通（扫链 `0x4BA00477`/`0x06413041`、`0x04`/`0x15` 回 0、Ping 活）
    → S2 `0x06` 读**秒回** `06 00 00 0C`（0 ms、`count=0`、`ack=4/FAULT`）→ Ping 活
    → 再发一笔 `0x11`（**值 1000000，与 S0 完全相同**）→ Ping 活
    → **S2.5 同一笔 `0x06` 读 1500 ms 超时**；独立进程 `--ping-only` 亦 `exit=3`（**真挂死，须插拔**）。
  - ⇒ **`0x11` 的取值不是变量**（连"同值重发"都挂）⇒ **否定 C2（时钟停摆 / 分频被写坏）**，实验二假设**不成立**。
  - ⇒ 跨轮**现象**（仍成立）：**第 1 笔 DAP 事务 = 秒回空响应**（0 ms、`count=0`、`ack=4`、无数据、**存活**）；
    **第 2 笔起必挂死** —— 与**读/写无关**（R4 读→写、R8 读→写、本轮 读→读）、与**中间夹不夹 `0x11` 无关**
    （R8 无、本轮有）。⇒ 旧表述"读活/写死"应改判为"**第一笔活、第二笔死**"。
  - ⚠️ **但上一条"第一笔被吞 / 没进 JTAG 执行"的"机理"在第十一轮被推翻**：第一笔**真发了事务**，只是拿到
    `ack=4` 才走"立即错误返回"分支（不重试、不计数）⇒ 见 B11.8 的"ACK 分叉"。故本轮**不支持 B11.6(a)**
    （`readBDelay` 只决定"这笔数据算不算进 `count`"，不决定"发不发"）。
  - ⇒ **C2 结案**；主因按 **B11.8** 重新归口（R1 ACK 接反 = 触发，R1b ACK 捕获错位 = 变异，R2 无超时等待环 = 致命）。
  - ⚠ 工具缺陷（本轮修掉）：上一笔 `0x06` 失败后 `Cmd()` 会因 `g_failed` **早退**（打印 `SKIP`），
    使"死活判定 Ping"根本没发出去、把"跳过"误报成"挂死"。现已在 S2.5 段强制清 `g_failed/g_stopOnFail` 后再 Ping。
- 复核已确认**无**以下死锁：`go` 是"启动脉冲"（`RESP_Wait_Done` 见 `dbg_done==0` 即清 `go`），`WAIT_GOCLEAR`(`:611-613`)+`JTAG_trans_os` 的两台阶（IR→TFR）交接自洽，`ack` 的 `default:` 分支（`:645-649`/`:672-677`）是"报错返回"而非挂死。

### B11.8 ACK 分叉定论（2026-10-03 第十一轮；含对 B9 第九/十轮的更正）

> 本轮不换固件、只复读源码：把"秒回 vs 挂死"追到**一个分叉点**，并纠正 B9 两条旧记述。
> 关键杠杆：`0x06` 响应第 4 字节由固件把 `ack` **原样拼出来** ⇒ 可从响应字节**反推固件里的 `ack`**。

**① 响应第 4 字节 = 固件 `ack` 的直读**（`orbtrace/debug/cmsis_dap.py:854`）：

```python
self.txBlock.bit_select(24,8).eq(Cat(self.dbgif.ack,self.dbgif.perr,C(0,4)))
```

⇒ `06 00 00 0C` 的第 4 字节 `0x0C = 0b0000_1100` ⇒ `ack = 0b100 = 4 = ACK_ERROR`、`perr = 0`。
（`count` 只在 `:868-869` `ack==ACK_OK` 时自增 ⇒ 这就是"`count=0`"的来处。）

**② 分叉点**（`cmsis_dap.py:843-864`）：

- `ack == ACK_WAIT` ⇒ `tfB_txb <= Mux(Bretries!=0,4,8)` ⇒ **回 case4 重发同一笔**，随后进 case5→6
  的 `dbg_done` **两台阶握手**；
- `Elif (ack != ACK_OK) | perr` ⇒ `tfB_txb <= 8` ⇒ **立即错误返回、不重试**（响应即刻吐出、`busy` 立刻清）。

⇒ **"第一笔秒回、第二笔挂死"的判据不是"第几笔"，而是"这一笔的 `ack` 落在分叉的哪一边"。**

**③ 为什么一边是 `4`、一边是（被翻成）`WAIT` —— 两个独立缺陷共同造成**

- **R1（触发；B9 第十轮已定）ACK 语义接反**：`jtagIF.v:174-179` 的 `ack[2:0]` 就是 ADIv5 数值
  （OK=1 / WAIT=2 / FAULT=4），而 `dbgIF.v:281-282` 却 `jtag_ack==1 → ACK_WAIT`、`==2 → ACK_OK`
  ⇒ **健康 DP 的正常 OK 被翻成 WAIT** ⇒ **每一笔正常事务都被推进重试环**。
- **R1b（变异；B9 第十轮已记）ACK 捕获未按 `devcount==dev` 门控**：`jtagIF.v:174-179` 的
  `case(tdxcount)` 对**每个器件**都执行，而 `:193-197` 推进 `devcount` 时把 `tdxcount` 清零
  ⇒ **dev1（bypass）的 TDO 覆盖 `ack[0]`**。本链 target=dev0、dev1=bypass ⇒ 真 `OK=001` 可能被打成
  `000`/`101` ⇒ 被翻成 **ERROR(4)**。**这就解释了"第一笔为何是 `ack=4`"**，也解释了"两笔 `ack` 为何不同"。

**④ R2 的致命点（纠正 B9 第十轮的错误表述）**

- ❌ 旧表述："`Bretries` **无复位默认值(=0)** ⇒ 欠载成 `0xFFFF` ⇒ **最多 65535 次重发**"。
- ✅ 实际：`cmsis_dap.py:797` 每笔 Setup 都 `Bretries.eq(waitRetry)`，`waitRetry` 复位值 = 4096（`:219`，
  本层实测发 100）⇒ **不会欠载**；而且这个上限**只管"要不要重回 case4"**。
- ⇒ **真死因**：case5/6 依赖 `dbg_done` 的两台阶（`dbgIF.v:662-679` 等 `~jtag_idle`、
  `:629-651` 等 `jtag_idle`），**整链无超时**；一旦这笔重发没走完两台阶，就**永久停在 case5**、
  走不到 `case8` 的上限收尾；`busy` **恒 1**（`:1158` 默认置忙）⇒ OUT 端点停摆 ⇒ 连 `Info` 都发不进去。
  与第九轮实测自洽：`0x14` / `0x15` / `0x03`+`0x02` 全无效，**只有插拔能清**。

**⑤ 同步更正 B9 第九轮的两条**（上文已就地补 ⚠️ 指针）

1. "首笔读被吞、根本没进 JTAG 执行" —— **错**：读在 `cmsis_dap.py:800`
   `tfB_txb <= Mux(rxBlock.bit_select(33,1), 4, 0)`，与写一样**直接进 case4 发 `go`、真发事务**；
   `readBDelay = isJTAG & rnw` 只决定"这笔数据算不算进 `count`"（⇒ 第一笔即使成功也拿不到数据）。
2. "判据 = 是不是本会话第一笔" —— **现象假象**；真判据 = **"这一笔的 `ack` 落在分叉哪一边"**
   （`WAIT` ⇒ 进重试环 ⇒ 有挂死可能；`ERROR/FAULT` ⇒ 立即返回 ⇒ 存活）。

**⑥ 附带缺陷（同一段代码，独立于挂死）**：`jtagIF.v:182-183` 注释 `// If ack is wait then abort`
与代码 `if ({tdo,ack[1],ack[0]} != 2)` **语义相反** —— 代码是"**非 WAIT 就提前 TMSOUT**"
⇒ 对正常的 `OK` 会**提前终止 shift、丢掉随后的 32 位读数据**（注释表达的是 `== 2` 的意图）。

**⑦ 待实机验证（唯一能最终定案的一条）**：**预言 = 让目标这一笔回 `FAULT`（或任何非 OK/WAIT）应"秒回且存活"；
只要回 `OK`（被 R1 翻成 WAIT）就应挂死**。可用"读一个必然 FAULT 的地址 / 未使能的 AP"构造。
另需波形（引 `jtag_idle`）确认 case5/6 两台阶在"引擎刚跑完一笔真事务"后**具体哪一步不成立**——静态读不出。

## B10 · 固件 `orbtrace-1.4.4` 与本文档基线 1.4.3 的差异核查（2026-10-03）

- **口径**：`C:\Users\taibaibu\Desktop\orbtrace-1.4.4\`（**无 `.git`、无 `PKG-INFO`**，版本串由 `git describe` 生成 ⇒ 无法从树内自证出处）
  vs 1.4.3 逐文件比对：**仅 9 个文件不同**，其余（含 `orbtrace/debug/dbgIF.py`、`verilog/ram.v`）逐字节相同。
- **A. `verilog/dbgIF.v`（跑在探针上的状态机）**
  - `CMD_CLR_ERR`：1.4.3（`:593-594`）**只写 `dbg_state`、不清 `perr`** ⇒ 出错后 `perr` 永久粘滞；1.4.4（`:596-600`）补 `perr <= 0`。
  - `CMD_JTAG_GET_ID` / `CMD_JTAG_RESET` / `CMD_SET_SWJ`：1.4.3 **只设 `commanded_mode`、不设 `active_mode`**（`:527-547`）；1.4.4（`:530`/`:538`/`:547`）补 `active_mode <= MODE_JTAG / MODE_JTAG / MODE_SWJ`。
  - `ST_DBG_ESTABLISH_MODE` 收尾：1.4.3（`:715-719`）模式切换波形走完**不恢复 `active_mode`**（停在 `MODE_LOCAL`）；1.4.4（`:721-726`）补 `active_mode <= commanded_mode`（原注释 `// Now running in the requested mode`）。
    ⇒ **确定后果**：`active_mode` 没更新时，`ST_DBG_WAIT_INFERIOR_START` / `..._FINISH` 的 `case (active_mode)` 会落到
    **`default` → `perr <= 1'b1` + 回 `ST_DBG_WAIT_GOCLEAR`**（1.4.3 `:645-649` / `:673-677`）⇒ **这几条命令"报错返回"而不是执行**
    （**不是挂死**；唯一在 1.4.3 里能补写 `active_mode` 的普通事务路径是 `CMD_TRANSACT` `:499`）。
    ⚠ **待复核**：`0x16 DAP_JTAG_IDCODE` → `RESP_JTAG_IDCODE_Setup`（1.4.3 `cmsis_dap.py:1309`）实际发的是哪条固件命令、
      中间有没有 `CMD_SET_JTAG`，决定本档 §18.9 / B9 里 `0x16` 的行为记录要不要按此重读。
- **B. `verilog/jtagIF.v`**
  - 1.4.3：`rst` 端口在 `dbgIF.v:318-320`（`.rst(rst)`）**一直连着，但模块内完全没用**；`case (jtag_state)` 无 `default`。
  - 1.4.4：加真异步复位分支 + `default:`（未知编码回 `ST_JTAG_IDLE`、清 `tdxcount`/`devcount`）。
  - ⚠ **`ST_JTAG_WRITEIR` 的完成判据一字未改**（`tdxcount[4:0]+1 == irlenx[5*devcount +:5]`，1.4.4 `:269` / `:277`）
    ⇒ **B9 的嫌疑机制在 1.4.4 里仍未被修**，B9 的处置（JTAG 全程只走主机侧 `0x14` 位流）不变。
- **C. 其余 gateware**：`swdIF.v` 1.4.3 完全无复位 → 1.4.4 加复位分支；
  `traceIF.v` 1.4.3 是 `if (!rst)`（复位期间不初始化任何寄存器、复位形同虚设）→ 1.4.4 改 `if (rst) {初始化} else {正常}`。
- **D. `orbtrace/debug/cmsis_dap.py`（命令层，64.7 KB → 107.8 KB）**：`DAP_CAPABILITIES` `0x03` → **`0x1f`**
  （+SWO UART / SWO Manchester / Atomic Commands）；新增 SWO 全套 `0x17`~`0x1e`（1.4.3 自述
  "Not implemented (indicated in config bits)"，见 1.4.3 `:106-112`）；新增 **`0x7e` QueueCommands / `0x7f` ExecuteCommands**
  （1.4.3 这两条走 `Error` ⇒ 回 `0xFF`，见 1.4.3 `:1218`）；`CMSIS_DAP.__init__` 增 `sys_clk_freq`
  （`cmsis_dap_wrapper.py` / `soc.py` 配套透传）。`tests/test_cmsis_dap_impl.py` 是 1.4.4 新增文件。
- **对本层的影响**
  - ⚠ **B6 的 `DAP_CAPABILITIES = 0x1f` 需澄清**：它引的 `:25` 在 **1.4.3 里是 `0x03`**（`0x1f` 只对 1.4.4 成立）；
    而本工程实测期望是 `00 01 03`（`test/v2rawprobe.cpp:242`）。⇒ **探针实际报什么，取决于烧的是哪版固件，须实测确认**
    （B6 其余各行两版相同，不受影响）。
  - ⇒ 若探针升到 1.4.4：`0x7f` **原子命令可用** ⇒ "一次往返执行 N 条子命令"（`Todo.md` 的"合并批量"目标）**在固件侧具备条件**。
- **未做**：本树跑不动自带仿真（系统 `python` 无 `amaranth`/`pytest`；树内 `__pycache__` 说明你另有环境）⇒ 上述均为源码静态比对结论。

## B12 🔴 "本层虚构 SWO 能力位"路线**已实证否决** —— 探针（1.4.3 固件）没有 SWO 命令族（2026-10-03）

**结论**：`Trace Enable` 恒灰是**两道闸叠加**。本层虚构 `caps` 只推开第一道
（`COMPAT_ANALYSIS.md` §17.9(8.1) **条件④** `caps & 0x04`），AGDI 紧接着的 **SWO 时钟探测**
撞在第二道上：`ID_DAP_SWO_BAUDRATE(0x19)`，1.4.3 固件回 **`0xFF`（未实现）** ⇒ 探测全败
⇒ 界面报 `SWO CLOCK not support`。
故 `Todo.md` §5 **T1-b 路线 (b)**（"本层不再按 `probeCaps` 收口、主动宣称 SWO 能力"）
在 1.4.3 上**永远不会真的工作**：它只是把"诚实的灰按钮"换成"能勾但注入不了数据、且报时钟不支持"，
比灰掉更具误导性。**实验已撤回**（虚构代码整段删除，见文末）。定性 = 口径决策失败，非本层缺陷。

### 实机现象（实验当日：临时虚构开关 = 1；该代码现已删除）

```
[WARN][RDDI] CMSIS_DAP_Capabilities: 【临时虚构】补 SWO 位 0x0011 -> 0x001D（探针自报 0x0003；VID:PID=0000:0000；名 'CMSIS-DAP v2'；MATCH_ALL=1）
[WARN][HID]  V2 Bulk DAPCommand: cmd=0x19 未实现（设备回 0xFF）
[WARN][RDDI] CMSIS_DAP_SWO_Baudrate: 探针拒绝 10000000 Hz（第 1 次失败；…）
[WARN][HID]  V2 Bulk DAPCommand: cmd=0x19 未实现（设备回 0xFF）
[WARN][RDDI] CMSIS_DAP_SWO_Baudrate: 探针拒绝 1250000 Hz（第 2 次失败；…）
```

读法：
1. 第 1 条 = 虚构生效：`0x0011`（`0x01` SWD + `0x10` Atomic）→ `0x001D`（+`0x04` SWO_UART +`0x08` SWO_Manchester）
   ⇒ §17.9(8.1) **条件④通过**，AGDI 不再按"无可用 Trace Port"把 `Trace Enable` 灰掉。
2. AGDI 随即转入 SWO 时钟探测（按 §17.9(8) 的既定路径调 `CMSIS_DAP_SWO_Baudrate(handle, &baud)`）。
3. 本层把每个候选波特率转成 `0x19` 下发 → 固件回 `0xFF` ⇒ 逐一失败。
4. 分频全败 ⇒ `SWO CLOCK not support`，实机上 `Trace Enable` 依然用不起来。

### 根因（`bug.md` B10-D 的静态结论，本次被**实测坐实**）

| 项 | **1.4.3**（本机实烧版本） | 1.4.4 |
|---|--------------------------|-------|
| `DAP_CAPABILITIES`（`orbtrace/debug/cmsis_dap.py`） | `0x03` —— 与实测自报 `0x0003` **逐一吻合** | `0x1f`（+SWO UART `0x04` / Manchester `0x08` / Atomic `0x10`） |
| SWO 命令族 `0x17`~`0x1e` | 源码自述 **"Not implemented (indicated in config bits)"**（1.4.3 `:106-112`） | 新增整套实现 |
| `0x19` 实测响应 | **`0xFF`**（`HID_RC_NOT_IMPLEMENTED`） | 待实测 |

⇒ 本层那套 SWO 实现（`DAP_SWO_Transport/Mode/Baudrate/Control/Status/Data`）**在 1.4.3 上没有下游**：
不是本层写错，而是对端没有这条命令。**两条独立证据**（自报 `caps=0x03`、`0x19` 回 `0xFF`）同时指向 1.4.3。

### 调用链（含行号，便于复核）

| # | 位置 | 行为 |
|---|------|------|
| 1 | `src/ORBMDK_RDDI.cpp` `CMSIS_DAP_Capabilities`（虚构补位段，B12 结案时已删除） | `0x0011` → `0x001D` |
| 2 | `src/ORBMDK_RDDI.cpp:3994` `CMSIS_DAP_SWO_Baudrate` | 收 AGDI 候选波特率，转 `DAP_SWO_Baudrate` |
| 3 | `src/ORBMDK_HID.cpp:1023` `_DapCommandRaw`（**V2 Bulk 优先**分支 `:1039-1053`） | 走 Bulk 发 `0x19`；`r == HID_RC_NOT_IMPLEMENTED` → `:1048` 记 WARN 并返回 |
| 4 | `src/ORBMDK_RDDI.cpp:4017-4023` | 失败计数前 4 条记 WARN，返回 `RDDI_DAP_ERROR` |
| 5 | AGDI 侧 | 候选分频全败 → `SWO CLOCK not support` |

> ⚠ 日志里 `cmd=0x19` 那条前缀是 **`[HID]`**，但当时是 `V2 Bulk` 模式 —— **不是串了通道**：
> V2 分支的日志点就写在 `src/ORBMDK_HID.cpp:1048`（`LOG_HID_WARN`），因为 `_DapCommandRaw`
> 统一承担 V1/V2 两条通道，V2 只是它的第一个分支（判据 `ORBMDK_USB_Bulk_GetMode()`）。

### 顺带确认的实测事实（价值独立于本实验）

- 探针自报 `probeCaps = 0x0003`（`Info(0xF0)`），**不含任何 SWO 位** ⇒ 1.4.3 源码自述与实测一致。
- 日志中 `VID:PID=0000:0000`（"未取到"）：AGDI 调 `CMSIS_DAP_Capabilities` 时设备信息尚未缓存/已断开，
  这正是 `MATCH_ALL=1` 存在的理由 —— **按身份门控在此时必然落空**（返工记录见 §5 T1-b）。

### 撤回动作与后续

- 撤回：**实验代码已整段删除**（`src/ORBMDK_RDDI.cpp`）—— 门控常量块（`ORBMDK_TEMP_FAKE_*`）、
  辅助函数 `TempNameIsOrbTrace`、以及 `CMSIS_DAP_Capabilities` 里的虚构补位段，**一处不留**（非"开关置 0"）；
  `CMSIS_DAP_Capabilities` 恢复为严格按 `probeCaps` 收口。
- 若仍要 SWO：唯一干净路径是**刷 1.4.4 固件**（固件自报 `0x1f` → 本层按 `probeCaps` 收口**自然放行**，
  条件④无需虚构）。待办指针见 `Todo.md` §5 **T1-b**。

## B13 🟡 版本门口径：`Identify(idNo=4)` 主版本「钉 `1.0.0`」→「放开 ≥ 2」（口径决策记录，2026-10-03 整理）

- **背景**：AGDI 用 `cmp eax,2` 判定 `Identify(idNo=4)` 的主版本（§17.6）。早期单变量 A/B
  （`COMPAT_ANALYSIS.md` §17.5）显示上报 `2.1.0` 会撞进一条走不通的初始化分支（`RDDI-DAP Error`），
  故一度把上报串**钉在 `1.x`**（设备自报 `2.1.0` → 上报 `1.1.0`）。
- **现行口径（2026-10-01 第三轮放开）**：改为把主版本**抬到 ≥ 2**（`include/ORBMDK.h` 的
  `ORBMDK_NormalizeProtocolVersion`；`1.x` → `2.x`，`2.x` 原样），并让 `CMSIS_DAP_Capabilities` 按
  `probeCaps` 收口置 `0x40`；两道门控**必须同侧**（改一处须同时改另一处）。问不到设备时兜底串仍为
  `1.0.0`，但那是"设备没答话"的兜底，与"设备答 `1.x` 也被抬高"**不同侧**。
- **为何可放开**：早年 A 臂的失败发生在 `StreamingTrace_*` 还是骨架时；第三轮已补齐流式语义
  （细节与回退条件见 `Todo.md.bak` §18.10-C）。
- **常见误读（本次整理纠正）**：文档一度写"§18.10-C 已实机回退 / 现状钉 `1.0.0`、caps 不置 `0x40`"，
  那是 **2026-10-01 第三轮之前的旧稿残留**，与现行代码**不符**。**判口径一律以代码为准**（本层无编译开关）。
- **涉及文件**：`include/ORBMDK.h`（归一化 + 注释）、`src/ORBMDK_RDDI.cpp`（`RDDI_Open` 取值 / `Identify`
  上报 / `CMSIS_DAP_Capabilities`）、`test/swdprobe.cpp`（断言主版本 ≥ 2）。

---

## 附录 A · 硬约束（改了就复发，勿"顺手"动）

| # | 约束 | 位置 | 动了会怎样 |
|---|------|------|-----------|
| A1 | **禁止回写 `*baudrate`** | `CMSIS_DAP_SWO_Baudrate`（`src/ORBMDK_RDDI.cpp`） | `SWO CLOCK not support`（`Todo.md.bak` §18.15） |
| A2 | V2 通道**不得**在 `_bulkWrite` / `_bulkRead` 底层加锁 | `src/ORBMDK_USB_Bulk.cpp` | `_flushAndDrainPipes` 持锁段内嵌套 `_bulkRead` → **自死锁** |
| A3 | 问设备的 `DAP_Info` 必须放在 `g_hidMutex` **之外** | `ORBMDK_HID_GetDeviceInfo`（`src/ORBMDK_HID.cpp`） | V1 路径同一把**非递归** mutex → **自死锁** |
| A4 | 版本号主版本与 `caps` 的 `0x40` **必须同侧** | `include/ORBMDK.h` + `CMSIS_DAP_Capabilities` | 只开一半 → `0x2028` 中止初始化 |
| A5 | `BlockWordsLimit()` **不得超过 126** | `src/ORBMDK_RDDI.cpp`（`static_assert` 已钉死） | 固件响应 RAM 越界 → 回卷 / 通道静默变哑 |
| A6 | **AP-DRW 连续传输必须在 TAR 块边界（本链实测 4 KB / 1024 字）重写 TAR**；块内仍靠自增 | `src/ORBMDK_RDDI.cpp`（`DAP_RegWriteRepeat` / `DAP_RegReadRepeat` 及任何新增合批路径） | 超过一块即**静默错址**（命令全 OK、数据错），见 **B7** |
| A7 | **禁止用一条 `ID_DAP_JTAG_SEQUENCE` 连发多笔 AP 写**（WAIT 须由固件门级重发，本层补不了） | `src/ORBMDK_RDDI.cpp`（`JtagApWriteBurst` 已删；勿再引入同类合并） | 丢字 + 写两遍 + TAR 多自增 ⇒ **静默错址**（Keil 读写内存/下载失败），见 **B8** |

> **行号口径**：B8 的停用改动使 `src/ORBMDK_RDDI.cpp` 中 `RDDIContext` 定义处之后的行号整体偏移
> （`jtagIds` 处 **+20**、`JtagApWriteBurst` 处 **+5**、`DAP_RegWriteRepeat` 处 **+3**）。
> ⇒ 本档中早于 2026-10-03 的 `ORBMDK_RDDI.cpp` 行号引用（如 `:2228/2315`、`:2233/2259/2320/2347`，
> 现集中在 **附录 B-6**）需相应顺延约 **+25**（上表 A6 与 B7 里的 `:2228` 同此口径）。

## 附录 B · 性能工程留档（2026-10-03，自 `Todo.md` 迁入）

> 迁入理由：`Todo.md` 只列未完成项。此处留档"做了什么 + 实机数据"，便于日后回归对照。

### B-1 · 阶段1 TESTSPEED 计量 —— 已完成并实机验收（见 `COMPAT_ANALYSIS.md` §9.4）

实机步骤（保留备查）：把 `%TEMP%\ORBMDK_LOG_LEVEL` 写成 `2`（TESTSPEED）→ 在 Keil 里下一次程序 →
确认 4 类行都出现，且数字自洽：`meter config`（transport / speed / cmdPkt / words·trip / blockTransfer）、
`WRITE|READ speed`（速率 + `us/trip` + 窗口墙钟与 dap 占比 + 累计均值）、
`meter segment`（段墙钟 / dap / other / 双向字节 / 往返数）、`meter usb`（OUT、IN 各自 us/cmd）。
自洽判据：V1/HID 应 ≈14 字/往返、≈95 µs/trip；V2/508 B 包应 ≈125 字/往返、≈789 µs/trip。
若 `us/trip` 与 `cmdPkt` 明显矛盾（例如 64 B 包却报 789 µs/trip），说明记账口径有误，需先修计量再谈加速。

**实机结果**：四类行齐全，核对通过 —— `meter usb` 的 cmds 与段内往返数一致（19↔19、2↔2）、
`other` 不再为负、`window wall` 为 µs 精度。

### B-2 · 阶段2 日志热路径（E1/E2）—— 已实现并实机验证

`ORBMDK_LogIsEnabled()` 无锁读 `g_levelFast` 原子快照，所有 `ORBMDK_LOG_*` 宏先过闸门
（阈值不满足则**连调用都不发生**：参数不求值、不取锁、不建 `va_list`）；
`_resolveLevel()` 解析处 `_publishLevel()` 刷新快照；`ORBMDK_LogMeterEnabled()` 一并改无锁。
实机（HSLinkPro，INFO 阈值）：538 行日志中 **0 条 DEBUG**（闸门过滤生效）、**464 条 TESTSPEED**
（计量照常开启），0 WARN / 0 ERROR。

### B-3 · 阶段4 D1（等待值处理）—— 已实现并实机验收（见 `COMPAT_ANALYSIS.md` §9.6）

⚠ 一次失败的尝试已撤销：**"把 wait 编进批量"在 HSLinkPro 上不成立** ——
该固件的**匹配读不回数据**（`DAP/Source/DAP.c` 的 "Store data" 只在普通读分支 :832-836，
匹配读分支 :755-788 之后直接出循环）；实测吻合：`done=2` 且无失配位，读回**全 0**。
而且探测因此失败会把多笔传输整体关掉（实测 `batch=0`、`29 -> 29`）—— 已修掉这个连带影响。

**现方案 = 有界等待**：wait 仍走单笔、不编进批量；条件已满足时 1 往返（与历史一致），
未满足时用 `[掩码伪写笔]+[匹配读]`（1 往返）让固件内部等 + 补一次普通读（1 往返）取真实值
⇒ 最坏从 100 次往返压到 **3 次**；固件若忽略 match 位则本会话自动回到纯轮询。

**实机实测（HSLinkPro / CherryUSB CMSIS-DAP，会话 #25/#26）**：
`5 -> 1 [batch=1]`、`6 -> 5 [batch=1 single=2 wait=2]`、`29 -> 19 [batch=10 single=0 wait=9]`、
`60 -> 41 [batch=19 single=2 wait=20]`。
- ① 达成：**不再出现 `batch=0`**；每个 `wait` 各占 1 次往返（快路径）。
  ⚠ 总量**高于**当初预期（预期 `6 -> 3`、`60 -> 21`）：非等待寄存器呈"每 ~2 个就被一个 wait 打断"，
  故每个批量只装得下 ~2 笔 —— 属 AGDI 访问序列交错所致（正常），**不是回退**；原预期写得过于乐观。
- ② 达成：全程无 `固件 match 结果与读值不一致` → match 位有效、未回退。
- ③ `swdprobe` **19 通过 / 0 失败**（V2/Bulk，`bin\swdprobe.exe`）。

### B-4 · 阶段5 多笔传输（D2）—— 已实现并实机验收（见 `COMPAT_ANALYSIS.md` §9.5）

`DAP_RegAccessBlock` 不再逐寄存器一笔：连续的非等待寄存器合进一条 `DAP_Transfer`
（`DAP_TransferMulti` / `DAP_TransferMultiMax`，仅 SWD；能力**先探测**、失败即永久回退逐笔）。

**实机实测（HSLinkPro / CherryUSB CMSIS-DAP，会话 #25/#26）**：
- ① 达成：`TransferMulti probe OK (idcode=0x1BA01477, count>1 语义已交叉验证)`
  （实际行格式是 `idcode=…`；原先写的 `batch=… single=…` 不准确，以此为准）。
- ② 达成：无 `多笔传输失败 … 回退逐笔`，也无 `首槽即未执行`。
- ④ 达成：合并批量生效 —— `5 -> 1`、`29 -> 19`、`60 -> 41`，均带 `(multi-transfer on)`。
- ③ 数值正确性由"Keil 会话全程无异常 + 无 mismatch"间接佐证（**未逐值比对**）。
- ⑤ `swdprobe` **19 通过 / 0 失败**；JTAG 会话不受影响（该路径不走多笔）。
- 另：`meter usb` 单程往返 ≈155–300 µs/cmd；`meter segment` 的 `dap` 仅占墙钟 1%~18%
  （`other` 80–99%）⇒ 瓶颈不在 USB 传输层，再抠传输层对端到端增益有限。

### B-5 · 停用 `JtagApWriteBurst` —— 已删（详见 **B8**）

实现体 `#if 0` 封存留证，`jtagApBurstEnabled` 开关与调用点一并移除；
实机验收：Keil 读写内存、下载恢复正常。

### B-6 · 性能工程口径与实测数据（2026-10-03，自 `Todo.md` §4 迁入）

**实证口径**：一次 USB 往返 ≈ **95 µs**（FS/64 B 包）～ **789 µs**（508 B 包）＝ ≈1.55 µs/字节
+ **每包固定 ≈52 µs**（`COMPAT_ANALYSIS.md` §9 实测）。⇒ 只有"**减少往返次数**"与"让每包更大"
两条路；后者已到顶（V2 标定 508、V1 受 64 B 报告约束），所以所有优化项都在减往返。

**影响源占比（2026-10-03 实测整理）**

| 维度 | 占比 | 依据 |
|---|---|---|
| 往返构成 | **wait ≈49%** / **合并批量 ≈48%** / **孤立单笔 ≈3%** | `RegAccessBlock` 日志 `29→19`(9/19)、`60→41`(20/41)、`99→67`(33/67) 加权 |
| DAP 段内 USB 方向（**orbtrace 烧录**） | **OUT 87.3%** / IN 12.7% | `meter usb: 216 cmds, out 112.5 ms(520.7 us/cmd), in 16.4 ms(76.1 us/cmd)` |
| DAP 段内 USB 方向（**HSLinkPro**） | OUT **≈51%** / IN ≈49%（对称） | `19 cmds out 80.4 / in 75.1`；`97 cmds out 153.1 / in 147.4` |
| 墙钟分解 | dap **11.7%** / other 88.3%（orbtrace 烧录）；调试会话 dap 0.4%~18% | `meter segment` |

> ⚠ **`dap%` 只覆盖 `DAP_RegWriteRepeat` / `DAP_RegReadRepeat`** —— `ORBMDK_LogMeterAccount`
> 的 4+4 个调用点全在 `src/ORBMDK_RDDI.cpp:2233/2259/2320/2347`；`DAP_RegAccessBlock` /
> `DAP_ReadReg` / `DAP_WriteReg` / `DAP_Transfer` / `CMSIS_DAP_*` **一律未计时**，其耗时落在
> `other` 里。⇒ 调试会话的 `dap 1%~18%` **不能**解读为"传输只占这点"。

**可动空间（推算）**：
1. 往返数最多再降 **≈48%**（把非等待笔全并成 1 个批量，受 wait 挡门限制）→ 端到端约 **−5%~−6%**；
2. 消掉 OUT 不对称 → 端到端再 **−2%~−3%**。

**已锁死（0 可动）**：出包大小（V1 64 / V2 508–511）、块传输字数（≤126）、
wait 不可合并批量（占往返 49%）、`DAP_MAX_PACKET_COUNT = 1`（orbtrace）。

**已核实无需改动**：`DAP_RegWriteRepeat` / `DAP_RegReadRepeat` 已走块传输（字数按
`BlockWordsLimit()`，V2 下 125/126 字/往返；`src/ORBMDK_RDDI.cpp:2228/2315` 是两者
首处 `DAP_TransferBlock` 调用）；V2 出包长度已按设备自报值标定；`_flushAndDrainPipes()`
只在重试/换候选/开设备时调用，**不要**挪进正常路径。

> ⚠ 行号口径：上表 `src/ORBMDK_RDDI.cpp` 的 `:2228/2315`、`:2233/2259/2320/2347` 属 2026-10-03
> 早期引用；同日删除 SWO 虚构段（B12，位于文件后部）**未影响**这些行号，但如需精确定位，
> 请以符号 `DAP_TransferBlock` / `ORBMDK_LogMeterAccount` 为准。
