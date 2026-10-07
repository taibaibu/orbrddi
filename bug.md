# ORBMDK 不可修复缺陷档

- **收录标准**：只收**本层改不了**的缺陷 —— 根因在**探针固件 RTL** 或**第三方二进制（µVision / `CMSIS_AGDI.dll`）**。
- **不进本档**：本层能修的（→ `Todo.md`）；AGDI 逆向与 ABI 结论（→ `COMPAT_ANALYSIS.md`）。
- **固件源码**：`C:\Users\taibaibu\Desktop\orbtrace-1.4.3\`；片上是 `orbtrace/debug/cmsis_dap.py`（Amaranth RTL）+ `verilog/{dbgIF,jtagIF,swdIF}.v`。
- **最后更新**：2026-10-03

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

## 附录 A · 硬约束（改了就复发，勿"顺手"动）

| # | 约束 | 位置 | 动了会怎样 |
|---|------|------|-----------|
| A1 | **禁止回写 `*baudrate`** | `CMSIS_DAP_SWO_Baudrate`（`src/ORBMDK_RDDI.cpp`） | `SWO CLOCK not support`（`Todo.md.bak` §18.15） |
| A2 | V2 通道**不得**在 `_bulkWrite` / `_bulkRead` 底层加锁 | `src/ORBMDK_USB_Bulk.cpp` | `_flushAndDrainPipes` 持锁段内嵌套 `_bulkRead` → **自死锁** |
| A3 | 问设备的 `DAP_Info` 必须放在 `g_hidMutex` **之外** | `ORBMDK_HID_GetDeviceInfo`（`src/ORBMDK_HID.cpp`） | V1 路径同一把**非递归** mutex → **自死锁** |
| A4 | 版本号主版本与 `caps` 的 `0x40` **必须同侧** | `include/ORBMDK.h` + `CMSIS_DAP_Capabilities` | 只开一半 → `0x2028` 中止初始化 |
| A5 | `BlockWordsLimit()` **不得超过 126** | `src/ORBMDK_RDDI.cpp`（`static_assert` 已钉死） | 固件响应 RAM 越界 → 回卷 / 通道静默变哑 |
