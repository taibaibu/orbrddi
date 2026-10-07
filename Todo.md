# ORBMDK 待办

**本文件只列未完成项。** 已完成的改动、核查结论、实现细节一律不写入（见 git 提交记录、代码内注释与 `COMPAT_ANALYSIS.md`）。

## 1. SWO —— 本探针固件没有 SWO

探针自报 `caps` 的 SWO 三位全 0；固件侧 `SWO_UART=0 / SWO_MANCHESTER=0 / SWO_STREAM=0`
（`DAP.c` 的 `0x17~0x1E` handler 未参与编译，`SWO.c` 未编译）⇒ 除非改固件（超出本 DLL 范围），SWO 不可能出数据。

- [ ] 实机：部署 DLL → µVision Trace 页确认 SWO 控件变灰 / 提示不支持，不再出现"勾了 Trace 却无数据"的假启用。
      （`swdprobe` / 测试工具里 SWO 一组 FAIL 属预期行为，不必修。）

## 2. V1(HID) 准入收紧后的回归（等真 v1 设备，如 orbtrace）

- [ ] 负载恰为 64 的 v1 接口仍被收下；`Identify(ifNo=1, idNo=2)` 仍为 `CMSIS-DAP v1`；
      `ConfigureInterface(ifNo=1)` 自检通过；Keil 里 v1 适配器可用。
- 残余风险（待观察）：**非首选** VID 且负载 != 64 的真 v1 会被拒。按规范 v1 负载恒为 64，暂无此类实例。

## 3. 其它待办

- [ ] 既有基线（**勿误判为回归**）：`ORBMDK_RDDI_FullTest.exe` 实机为 `PASSED 20~22 / FAILED 20~22`
      —— 失败仅两类：① 测试自身 `DAP_REG_*` 常量陈旧；② `DP CTRL/STAT = 0 [NOT powered]`
      （目标复位后调试电源未上，AP 访问必 FAULT；三种传输模式表现一致，可排除传输层）。
      **SWD 通路是否正常的判据看 `swdprobe`**（V2 与 V1 均 19 通过 / 0 失败）。
- 已知限制（暂不处理）：V2 枚举 pass 1 若打开"class `0xFF` + 有 Bulk 对但不是 DAP"的接口，
      自检会如实报错，但**不会回退去试下一个候选**；要消除得把 `DAP_Info` 探针挪进枚举循环，会拖慢枚举。
