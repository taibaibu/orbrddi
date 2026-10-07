# ORBMDK 待办

**本文件只列未完成项。** 已完成的改动、核查结论、实现细节一律不写入 ——
实现细节/排查笔记 → **`bug.md`**；AGDI 逆向与 ABI 结论 → **`COMPAT_ANALYSIS.md`**；代码内注释就地说明。

> 当前版本 **0.5.0**（2026-10-03）。
> 条目标签 `18.x` 沿用历史编号，与 `Todo.md.bak` 及 `COMPAT_ANALYSIS.md` 的旧引用一一对应，便于交叉检索；
> 已决"**不做**"（18.7a / 18.7b）与**已完成**（18.2 / 18.3 / 18.4 / 18.6 / 18.9 / 18.15）条目仍留在 `Todo.md.bak`。

## 发版阻塞（0.5.0）

- [ ] **18.11 测试程序 `DAP_REG_*` 常量与 `rddi_dap.h` 编号语义不符**：`test/ORBMDK_RDDI_FullTest.cpp`
      仍按旧编号声明，导致 `ORBMDK_RDDI_FullTest` 由 **40/40 退化为 `PASSED 22 / FAILED 20`**
      （`COMPAT_ANALYSIS.md` §一 / §13.5）。**是测试自身的问题，不是驱动回归** —— 但发版前必须修好，
      否则 README"验证"一节的 40/40 无法复现。

## 3. 其它待办

- [ ] **18.1 多台调试器同时插入时无法指定用哪一台**：枚举到多个候选时按固定顺序取第一个，
      无 UI / 参数可选（详见 `Todo.md.bak` §18.1）。
- [ ] **18.14 设备拔出后适配器下拉框仍显示**：`IsConnected` 无拔出探活（无热插拔通知）。
- [ ] **18.12 `CMSIS_DAP_GetNumberOfDevices` 在 JTAG 路径上的语义待复核**（`Todo.md.bak` §18.12）。
- [ ] **18.13 是否对外宣称 `INFO_CAPS_JTAG`**：现未宣称（`Todo.md.bak` §18.13）。
- [ ] **18.2-遗留 块传输测试 `DHCSR` 回读为 0**（非阻塞；`Todo.md.bak` §18.2-遗留）。
- [ ] **18.5 简化实现（stub 清单）**：`StreamingTrace_*` 等仍为骨架，均**不影响** Keil 调试链路
      （清单见 `Todo.md.bak` §18.5）。
- 已知限制（暂不处理）：V2 枚举 pass 1 若打开"class `0xFF` + 有 Bulk 对但不是 DAP"的接口，
      自检会如实报错，但**不会回退去试下一个候选**；要消除得把 `DAP_Info` 探针挪进枚举循环，会拖慢枚举。

## 4. 性能加速（2026-10-03 立项；**JTAG 相关本轮不做，另行再议**）

> 实证口径、影响源占比表、可动空间与"已锁死"清单已迁 **`bug.md` 附录 B-6**（自本节迁出，避免两份数据漂移）。
> 一句话结论：只有"**减少 USB 往返次数**"一条路（出包大小与块字数均已到顶），推算端到端最多 **−5%~−6%**。

- [ ] **阶段3 · 自适应超时（D4）**：`ORBMDK_TIMEOUT_CMD_MS = 1000`（`include/ORBMDK_HID.h:29`）；
      连续成功 N 次后降到 200~300 ms、失败回 1 s（与熔断 `kDapTripAfter = 2` 配合）。
      目标场景：目标复位/掉电瞬间从"数秒卡"降到百毫秒级。

- [ ] **阶段6 · E3 / D5**：日志写盘批量化（去掉逐行 `fflush` 与每行 `std::string`，仅影响日志开启时）；
      SWO 后台轮询节奏自适应（`TraceReadThreadFunc`：现固定 `wait_for(2 ms)` + 每轮一次 `DAP_SWO_Data`；
      `src/ORBMDK_HID.cpp:2100` 线程入口 / `:2113` 轮询 / `:2123` 睡眠 —— 与 AGDI 命令抢同一条总线）。

- [ ] **JTAG 批量写通路（方案 B）—— ⚠️ 前置已被阻断，暂缓**：完整依据/风险见 `bug.md` **B8**。
      要点：把 N 笔塞进**一条** `ID_DAP_TRANSFER`，让**固件门级**按 `waitRetry` 重发 WAIT（`cmsis_dap.py:697-716`）；
      当前 `RegAccessBlock` 对 JTAG 把批量上限写死为 `0`（`ctx->isSWD ? … : 0`），故日志恒 `batch=0`。
      动了须一并处理 JTAG **延迟读**（`cmsis_dap.py:626-637`、`:778-786`）。
      ⚠ **阻断原因**：本方案依赖固件 `0x06`，而"JTAG 下启用固件 `0x05`/`0x06`"已结案为
      **本层做不到、已放弃**（八轮复验终局 + 判别实验二，见 `bug.md` **B9 / B11.7**）
      ⇒ **除非先拿到修过的探针固件，本项不开工**。
      **不要**再尝试"一条 `DAP_JTAG_SEQUENCE` 连发多笔"（已证伪，见 B8/A7）。

- 明确不做：块传输字数 > 126（固件 RAM 127 字 / `txedLen` 9 bit）、Queue 多包流水线
      （`DAP_MAX_PACKET_COUNT = 1`）、`_bulkWrite/_bulkRead` 内加锁（`_flushAndDrainPipes` 嵌套 → 自死锁）、
      把 `DAP_TransferAbort` 当"卡死恢复"（`busy` 闸死，命令进不去）。
- 验收基线（改动前后必须一致）：`swdprobe` **19 通过 / 0 失败**；
      `ORBMDK_BlockTransferTest` 的 `speedup 7.86x`（V1/HID 基线）。

## 5. Trace 页「并行 Trace 端口 / ETB」可放开性（2026-10-03 立项）

> 逆向结论与三路线解法在 **`COMPAT_ANALYSIS.md` §17.9**（§17.8 的表述已在那里精确化）。
> **T0 已关闭（2026-10-03）**：`Trace Port` 下拉被 **AGDI 自己**在 `0x1003B39A` / `0x1003BA8D`
> 两处硬编 `EnableWindow(0x456, FALSE)` 灰掉（全镜像**无任何** enable 点）—— 依据与控件映射见 §17.9 **(7)**。
> **T1 实机复验已完成（2026-10-03）**：补丁生效 —— `Trace Port` 下拉可点开、`Embedded Trace Buffer` 可选；
> 但 `Trace Enable` 仍然灰，且 **ETB 是死路**（编码器拒收 port=3、AGDI 无 ETB 配置串模板）—— 见 §17.9 **(8)**。
> 已定论、勿重复踩：改 magic / 改 caps **放不开 `Trace Port`**（那是硬编码 `push 0`）；改 UV4 类别（路线 1）也**不够**；
> 放开 UI ≠ 有数据通路；**ETB / ETM / 并口在本版 AGDI 里没有任何代码路径**。

- [ ] **T1-a（决策）**：盘上 AGDI 补丁**留还是撤**？功能上零收益（ETB 走不通，只让 UI 可选、易误判）；
      留着仅当诊断用。要撤：`python tools/patch_agdi_etb.py --revert`。
- [ ] **T1-b（决策 + 实现）· 路线 (b) 已实证否决（2026-10-03）**：点亮 `Trace Enable`（=SWO）的卡点是
      §17.9(8.1) **条件④**（`caps` 无 `0x04/0x08`）。两条路里 **(b)「本层主动宣称 SWO 能力」实测走不通**
      —— 探针固件 1.4.3 **没有 SWO 命令族**（`0x19` 回 `0xFF`），虚构 caps 只把"灰按钮"换成
      "能勾但注入不了数据 + 报 SWO CLOCK not support"。**定性、日志与调用链见 `bug.md` B12。**
      虚构实验代码（门控常量 + `TempNameIsOrbTrace` + 补位段）已从 `src/ORBMDK_RDDI.cpp` **整段删除**（不留宏开关）。
      **剩余未完成项 = 路线 (a)**：刷 **1.4.4 固件** —— 固件自报 `caps=0x1f`（含 `0x04/0x08`），
      本层按 `probeCaps` 收口**自然放行**，条件④无需任何虚构；届时顺带复验 `0x19` 是否真的可用
      （1.4.4 的 SWO 实装质量未经验证）与 `0x7f` 原子命令。
      参考：1.4.3↔1.4.4 命令层差异清单见 `bug.md` B10-D；门控返工记录（V2 下 `productName` 取到的是
      Bulk 接口名 `CMSIS-DAP v2`，身份判据只能靠 VID/PID）见 B12 末节。
- [ ] **T2（建议结案）**：ETM / 并口 / ETB —— §17.9(8.2) 已证明 AGDI 侧连配置串模板都不存在，
      RDDI 层再补也无处挂载。除非换 AGDI 版本或自研上游，不建议立项。
- **已放开（2026-10-01）**：**18.10-B / 18.10-C**「SWO 流式（上报版本主版本抬到 ≥ 2 + caps `0x40`）」
  —— **代码已放开两道门控**（`include/ORBMDK.h` 的归一化 + `CMSIS_DAP_Capabilities` 按 `probeCaps` 收口）。
  完整方案与一键回退步骤见 `Todo.md.bak` §18.10-B / §18.10-C。
