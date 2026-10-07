# ORBMDK 改动记录

## 2026-10-02 支持第三方 CMSIS-DAP（CherryUSB 0D28:0204）

现场：hslinkpro（CherryUSB CMSIS-DAP，VID_0D28&PID_0204）在 Keil 里连接失败，
日志 `ORBMDK_RDDI.log` 报 `HID WriteFile failed, error=87`，紧接着误触发
"DAP 通道熔断（连续 2 次超时）"。

| # | 问题（一行） | 改动 |
|---|--------------|------|
| 1 | V2 Bulk 只认 1209:3443，0D28:0204 连候选都枚举不到 | `include/ORBMDK.h` 新增 `ORBMDK_DEVICE_VIDPID_LIST` + `ORBMDK_IsSupportedDevice()`；`vid=pid=0` 表示按支持列表匹配（`ORBMDK_USB_Bulk_Init` / `ORBMDK_Bulk_Open`） |
| 2 | HID 固定发 65 字节（报告ID 0x00 + 64），报告长度不是 64 的设备被 hidclass 以 err=87 当场拒收 | 打开时用 `HidP_GetCaps`/`HidP_GetValueCaps` 解析报告布局（长度/报告ID/前缀/负载），按设备实际长度写；无 Output 报告改走 `HidD_SetFeature`；IN 报告无报告ID前缀时补占位字节 |
| 3 | HID 只校验 VID，任何 0x0D28 的 HID 设备都被当 DAP 打开（假成功） | 新增 `_hidDeviceAcceptable()`：VID+PID 精确匹配，或 VID 在册 + Usage Page 0xFF00 + 读写报告可用，否则拒绝并记日志 |
| 4 | 写立即失败也返回 -3，被 `_dapIsTimeout` 当成超时，两条命令就熔断并误报"探针不再响应" | 引入 `HID_RC_*` 返回码：-6 写立即失败 / -4 读立即失败；熔断只认 -3（写超时）/ -5（读超时） |

附带：
- V1 的 `max_packet_size` 取设备 HID 报告负载（DAP v1 上限 64）；
- V1 日志与 `deviceInfo.vid/pid` 改为如实上报实际设备（新增 `ORBMDK_HID_GetIdentity()`），
  原先恒报 1209:3443，故障时无法从日志判断接的是哪台设备。

验证：`build.ps1` 编译通过；实机回归待做 —— 插 hslinkpro 后应看到
`HID 报告布局：UsagePage=... IN=... OUT=... 负载=...` 与
`Init: V1 HID mode OK (vid=0x0D28 pid=0x0204 报告负载=...)`，且不再出现 err=87 与熔断。
若 V2 可打开（Interface 0 已绑 WinUSB），`Init: V2 Bulk mode OK` 会先出现。

### 续：现场第 2 轮日志（会话 #31）——该设备的 HID 接口不是 CMSIS-DAP

```
[WARN][HID] HID 响应不匹配：cmd=0x11 收到 IN[0..7]=02 70 61 72 73 65 20 65 (readLen=1024)
[WARN][HID] HID 响应不匹配：cmd=0x03 收到 IN[0..7]=02 70 61 72 73 65 20 65 (readLen=1024)
[ERROR][RDDI] CMSIS_DAP_Connect: connect failed, mode=97
```

- `70 61 72 73 65 20 65` = ASCII `parse e`：设备回的是**文本**，不是 DAP 响应；两条不同命令
  （0x11 / 0x03）读回字节**完全相同** → 该 HID 接口对任何输入都回同一句话，是个调试/文本通道。
  0x02 疑为响应头（或该接口的 IN 报告ID）。
- `mode=97` = 0x61 = `'a'`，正是那段文本的第 3 字节：旧代码按 `resp[2]` 取端口，取到的是文本。
- 真正可用的是 **V2 Bulk**：同一份日志里 `RDDI_Open: device DAP_Info(4)='2.1.1' (rc=6)` 是在 v2 上
  成功的。→ 这台设备上 ifNo=1 不存在，应在适配器列表里选 `CherryUSB CMSIS-DAP`（ifNo=0）。

误导来源（`src/ORBMDK_RDDI.cpp` `CMSIS_DAP_Identify`）：
- `idNo=2` 走 `ORBMDK_USB_Bulk_GetInterfaceName(ifNo)`，其中 `ifNo==1` **直接返回硬编码**
  `CMSIS-DAP v1`；`idNo=3/4` 走 `ctx->serialNumber` / `ctx->firmwareVersion`（v2 上读到的那份）。
- 即 Identify **从不按 ifNo 去查设备**，`ifNo=1` 于是报出 v2 的真实身份（序列号、`2.1.1`），
  AGDI 据此认为两条接口都是好 DAP。

| # | 问题（一行） | 改动 |
|---|--------------|------|
| 5 | 把"HID 能打开"当成"HID 是 DAP"，错误拖到 `DAP_Connect` 才爆成 `connect failed, mode=0x61`，指向"探针失效" | `ORBMDK_USB_Bulk_SelectInterface(ifNo=1)` 切换成功后做一次 `DAP_GetInfo(DAP_INFO_FIRMWARE)` 硬校验；无有效应答则返回 -1 并 `ORBMDK_DapChannelReset()`（清掉自检期间累加的超时计数），由 `CMSIS_DAP_ConfigureInterface` 如实报错 |
| 6 | `SelectInterface` 的 `already` 早退会让 HID 自检只跑一次：第一次失败后 `already=true` 直接放行 | 去掉了对 HID 的短路：只要目标是 HID 就必须过自检 |

未做（有意）：不在 `CMSIS_DAP_Detect` 里"探测不到就少报一条接口"——`Detect` 恒返回 2 是刻意的
（为避开 AGDI 的 EU02 路径），保留 HID 条目，等固件真的实现 v1 时上层不用改。

### 续 2：换上 orbtrace（真 DAPLink）后 V1 读不到 IDCODE —— 上一轮改动引入的回归

现象：CherryUSB 设备走 V2 一切正常；换成 orbtrace 走 V1(HID) 后 Keil 报"找不到 IDCODE"，
而上一次改代码之前是好的。

根因在 `_DapCommandRaw` 的 V1 响应组装：回显自校准算出的 `payloadStart` 是**负载起点**
（已经跳过命令ID），但组装写成

```cpp
memcpy(&resp[1], &reportIn[payloadStart], copyLen);   // resp[1] 拿到的是"第二个"负载字节
```

命令ID 被吃掉 ⇒ 上层按 `[报告ID][命令ID][负载...]` 解析时全体前移一位、`respLen` 少 1。
对照 V2 分支 `memcpy(&resp[1], tmp, n)`（`tmp[0]` 就是命令ID，本来正确）—— 于是表现为
"V2 没事、V1 坏"，与现场完全一致。

后果举例：
- `DAP_Connect(0x02)` 负载只有 1 字节（port）→ 组装后 `respLen=2 < 3` → `DAP_ConnectSWD`
  直接返回 -1（连接都建不起来，更别说读 IDCODE）；
- `DAP_QueueCommands` 里的 `resp[1] == ID_DAP_QUEUE_COMMANDS` 检查在 V1 下恒为假。

| # | 问题（一行） | 改动 |
|---|--------------|------|
| 7 | V1 响应组装丢掉命令ID（`payloadStart` 之后直接拷进 `resp[1]`），全体前移一位 + `respLen` 少 1；V2 分支本来对的，故表现为"V2 好、V1 坏" | `src/ORBMDK_HID.cpp` `_DapCommandRaw` 尾部改为 `resp[0]=报告ID占位; resp[1]=cmd[0]; memcpy(&resp[2], &reportIn[payloadStart], ...)`，与 V2 分支逐字节对齐 |

验证：`build.ps1` 通过；实机回归待做 —— orbtrace 走 V1 应能读到 IDCODE，
且不再出现 `HID 响应不匹配`（合规设备的响应都回显命令ID）。
