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

### 续 3：V1 实机回归（hidprobe）—— 现场插的是 hslinkpro，自检如期拒绝

2026-10-02 22:20，现场设备 = hslinkpro / CherryUSB `0D28:0204`（**orbtrace 未插**，PnP 无 `VID_1209` 设备）。

```
> bin\hidprobe.exe
RDDI_Open           -> 0 (handle=0)
ConfigureInterface  -> 3  (ifNo=1 = CMSIS-DAP v1 / HID)     ← 3 = RDDI_FAILED
DetectNumberOfDAPs  -> 0 (noOfDAPs=0)
DP CTRLSTAT         -> 0x00000000  [no ack]
```

`%TEMP%\ORBMDK_RDDI.log` 关键序列：

```
22:20:13.390 Init: V2 Bulk mode OK (product='CherryUSB CMSIS-DAP', serial='A758D621...')
22:20:13.391 RDDI_Open: device DAP_Info(4)='2.1.1' (rc=6)
22:20:13.391 SelectInterface: ifNo=1 -> hid (current=2, already=0)        ← 探针要求切 V1
22:20:13.395 HID 报告布局：UsagePage=0xFF00 IN=1024 OUT=1024 负载=1023 报告ID=0x01 前缀=1 WriteFile
22:20:13.395 Init: V1 HID mode OK (vid=0x0D28 pid=0x0204 报告负载=1023)
22:20:14.395 SelectInterface: ifNo=1 (HID) 打开成功但对 DAP_Info 无有效应答 -> 该 HID 接口不是 CMSIS-DAP
22:20:14.395 CMSIS_DAP_ConfigureInterface: interface 1 (CMSIS-DAP v1/HID) could not be opened (-1) - NOT falling back
```

结论：

- **改动 #5 实机验证通过**：HID 口"能打开但 DAP_Info 无有效应答"被自检拦住，`ConfigureInterface`
  如实返回 `RDDI_FAILED(3)`，不再把 HID 能打开当成 v1 可用（旧的 `connect failed, mode=0x61` 掩盖链已断）。
- 改动 #6 本次未体现差别（`already=0`，本来就会跑自检）；orbtrace 侧不再回归，随本条一并关闭。
- **V1 通路本身仍未在真 DAPLink（orbtrace）上回归** —— 本轮设备上 ifNo=1 本来就不存在，失败是正确行为。
  用户 2026-10-02 确认 orbtrace 侧已基本完备、无需额外 V1 实机回归，**本条回归项就此关闭、不再排期**。
- 同时确认 V2 一切如常（`V2 Bulk mode OK` + `DAP_Info(4)='2.1.1'`），hslinkpro 请始终选 `ifNo=0`。

### 续 4：设备识别去白名单化 —— VID/PID 只当"优先级"，准入改走能力特征

需求：用户要"兼容很多设备"，不想每换一台 CMSIS-DAP 都改 VID/PID 表。

设计（**白名单从"准入"降级为"优先级"，识别按 CMSIS-DAP 规范的能力特征**）：

| 层 | 准入条件（不再看 VID/PID） | 首选（表内命中）的优待 |
|---|---|---|
| V2 Bulk | 绑 WinUSB + 接口类 `0xFF` + Bulk IN/OUT 对 + **DAP_Info 有应答** | 排第 1 趟扫描 |
| V1 HID | UsagePage `0xFF00` + OUT/IN 报告可用 + **DAP_Info 有应答** | 排第 1 趟扫描 |

改动点：

1. `include/ORBMDK.h`：`ORBMDK_IsSupportedDevice` -> **`ORBMDK_IsPreferredDevice`**，表头注释改写
   （明确 `0D28:0204` 下有多个 0xFF 接口，VID/PID 不能区分"是不是 DAP"，必须过能力校验）。
2. `ORBMDK_USB_Bulk.cpp` `_findAndOpenDeviceInGuid`：改成**两趟扫描**
   （`while (pass < 2 && !deviceOpened)` + index 归零，循环体缩进不变）。
   - pass 0 = 首选表命中（或调用方显式指定的 vid/pid）；
   - pass 1 = 其余接口，但先用新增的 **`_compatibleIdIsVendorClass()`** 读
     `SPDRP_COMPATIBLEIDS` 预筛 `Class_ff`，避免对系统里几十个无关 WinUSB 接口
     `CreateFile`（读不到该属性则保守放行，宁可多试不可漏）；
   - 真正的把关仍在打开之后：接口类 0xFF + Bulk 对。
3. `ORBMDK_USB_Bulk.cpp` `SelectInterface`：给 **V2 补上与 V1(#5) 对称的 `DAP_Info` 自检**。
   放开 VID/PID 后枚举可能选中别的厂商 0xFF 接口，不在这里问一句，错误会一路拖到
   第一条命令读超时才爆（现场表现为"探针不响应"而非"这不是 DAP"）。
4. `ORBMDK_HID.cpp`：`_hidDeviceAcceptable(..., bool preferredOnly)` 去掉 VID 门槛，
   改为能力准入；`CMSIS_DAP_VIDS[]` 注释降级为"首选厂商 VID"。`EnumerateDevices` /
   `ORBMDK_HID_OpenDevice` 各改两趟扫描。

实机验证（hslinkpro `0D28:0204`，`bin\swdprobe.exe`）：

```
candidate[1] '\\?\usb#vid_0d28&pid_0204&mi_00#...'     ← pass 0 首选命中，直接采用
enumeration done: 1 candidate(s) (=> 首选表优先 + 能力匹配(class 0xFF)), opened=1
Init: V2 Bulk mode OK (product='CherryUSB CMSIS-DAP', serial='A758D621...')
RDDI_Open: device DAP_Info(4)='2.1.1' (rc=6)
SelectInterface: ifNo=0 (bulk) DAP 自检通过 (fw='2.1.1')      ← 新增自检 ✅
```

即：**首选设备的行为与改动前逐字一致**（仍 1 candidate、仍打开 MI_00），
放开的分支（pass 1）在首选命中时根本不执行 —— 零回归风险。

已知限制（未处理，待观察）：若 pass 1 打开了一个"class 0xFF + 有 Bulk 对但不是 DAP"
的接口，V2 自检会如实报错，但**不会回退去试下一个候选**（枚举此时已结束）。
要消除它得把 DAP_Info 探针挪进枚举循环 —— 会拖慢枚举，暂不做。

现场暴露的点：

1. `test\hidprobe.cpp` 不检查 `ConfigureInterface` 返回值就继续发 DAP 命令：失败后通道停在 HID，
   随之 51 次读超时把通道打成**熔断**（`22:20:16.412 DAP 通道熔断`），100 多行 `-5` 淹没了关键日志。
   **已修（2026-10-02）**：`RDDI_Open` 与 `ConfigureInterface` 返回值非 0 即打印原因并退出
   （配置失败退出码 `3 = RDDI_FAILED`），不再继续发命令、不再误触发熔断；失败提示直接指向
   "该设备若无 v1/HID 通路请选 ifNo=0"。编译：`test\build_test.ps1 -Source hidprobe.cpp`（通过）。
   在 hslinkpro 上重跑：退出码 `3`，日志只余 6 行（无 `-5` 刷屏、无熔断），
   自检细节可见 `HID 响应不匹配：cmd=0x00 收到 IN[0..7]=02 'parse e'`（与上表 #5 所述同一现象）。
